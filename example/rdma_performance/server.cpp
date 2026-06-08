// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.


#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <atomic>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <vector>
#include <gflags/gflags.h>
#include "bthread/unstable.h"
#include "butil/atomicops.h"
#include "butil/logging.h"
#include "butil/time.h"
#include "brpc/iouring/iouring_helper.h"
#include "brpc/rdma/rdma_helper.h"
#include "brpc/server.h"
#include "bvar/variable.h"
#include "test.pb.h"

namespace bthread {
DECLARE_int32(bthread_concurrency);
}

DEFINE_int32(port, 8002, "TCP Port of this server");
DEFINE_string(transport, "rdma",
              "Transport mode: tcp, rdma, or iouring. When explicitly set, "
              "this overrides --use_rdma.");
DEFINE_bool(use_rdma, true,
            "Compatibility flag. Used only when --transport is not explicitly "
            "set: true means rdma, false means tcp.");
DEFINE_int32(server_num_threads, -1,
             "Number of brpc server worker threads. -1 keeps brpc default, "
             "0 lets bthread_concurrency control the worker count, >0 sets "
             "ServerOptions.num_threads explicitly");
DEFINE_bool(server_bind_bthread_workers, false,
            "Bind each brpc bthread worker pthread to one CPU");
DEFINE_string(server_worker_affinity_cpus, "",
              "CPU list for binding brpc bthread worker pthreads, for example "
              "`112-127` or `112-119,124,126`. Empty means using the current "
              "process affinity mask, such as the mask inherited from taskset");

butil::atomic<uint64_t> g_last_time(0);
std::vector<int> g_worker_affinity_cpus;
std::atomic<int> g_next_worker_affinity_index(0);
bool g_transport_explicit = false;

bool IsFlagExplicitlySet(const char* flag_name) {
    GFLAGS_NAMESPACE::CommandLineFlagInfo info;
    return GFLAGS_NAMESPACE::GetCommandLineFlagInfo(flag_name, &info) &&
           !info.is_default;
}

struct TransportChoice {
    brpc::SocketMode socket_mode;
    const char* name;
    bool initialize_iouring;
};

bool ResolveTransportMode(TransportChoice* choice) {
    std::string transport = FLAGS_transport;
    for (size_t i = 0; i < transport.size(); ++i) {
        transport[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(transport[i])));
    }
    if (!g_transport_explicit) {
        transport = FLAGS_use_rdma ? "rdma" : "tcp";
    }

    if (transport == "tcp") {
        choice->socket_mode = brpc::SOCKET_MODE_TCP;
        choice->name = "tcp";
        choice->initialize_iouring = false;
        return true;
    }
    if (transport == "rdma") {
#if BRPC_WITH_RDMA
        choice->socket_mode = brpc::SOCKET_MODE_RDMA;
        choice->name = "rdma";
        choice->initialize_iouring = false;
        return true;
#else
        LOG(ERROR) << "transport=rdma requires BRPC_WITH_RDMA=1. "
                   << "Rebuild brpc/example with RDMA support or use "
                   << "--transport=tcp/--transport=iouring.";
        return false;
#endif
    }
    if (transport == "iouring") {
#if BRPC_WITH_IOURING
        choice->socket_mode = brpc::SOCKET_MODE_IOURING;
        choice->name = "iouring";
        choice->initialize_iouring = true;
        return true;
#else
        LOG(ERROR) << "transport=iouring requires BRPC_WITH_IOURING=1. "
                   << "Rebuild brpc/example with io_uring support or use "
                   << "--transport=tcp/--transport=rdma.";
        return false;
#endif
    }
    LOG(ERROR) << "Invalid transport=" << FLAGS_transport
               << ", valid values are tcp, rdma, and iouring";
    return false;
}

bool InitializeTransportRuntime(const TransportChoice& choice) {
    if (choice.socket_mode == brpc::SOCKET_MODE_RDMA) {
#if BRPC_WITH_RDMA
        brpc::rdma::GlobalRdmaInitializeOrDie();
        return true;
#else
        return false;
#endif
    }
    if (choice.initialize_iouring) {
#if BRPC_WITH_IOURING
        brpc::iouring::GlobalIouringInitializeOrDie();
        if (!brpc::iouring::InitPollingModeWithTag(/*tag=*/0)) {
            LOG(ERROR) << "Fail to init io_uring polling mode";
            return false;
        }
        return true;
#else
        return false;
#endif
    }
    return true;
}

bool ParseNonNegativeInt(const std::string& token, int* value) {
    if (token.empty()) {
        return false;
    }
    char* end = NULL;
    errno = 0;
    const long parsed = strtol(token.c_str(), &end, 10);
    if (errno != 0 || end == token.c_str() || *end != '\0' ||
        parsed < 0 || parsed >= CPU_SETSIZE || parsed > INT_MAX) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

bool HasDuplicateCpu(const std::vector<int>& cpus, int* duplicated_cpu) {
    for (size_t i = 0; i < cpus.size(); ++i) {
        for (size_t j = i + 1; j < cpus.size(); ++j) {
            if (cpus[i] == cpus[j]) {
                *duplicated_cpu = cpus[i];
                return true;
            }
        }
    }
    return false;
}

bool ParseCpuList(const std::string& text,
                  std::vector<int>* cpus,
                  std::string* error) {
    cpus->clear();
    if (text.empty()) {
        *error = "server_worker_affinity_cpus is empty";
        return false;
    }

    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t comma = text.find(',', begin);
        const size_t end = (comma == std::string::npos ? text.size() : comma);
        const std::string item = text.substr(begin, end - begin);
        if (item.empty()) {
            *error = "Empty CPU list item in `" + text + "`";
            return false;
        }

        const size_t dash = item.find('-');
        if (dash == std::string::npos) {
            int cpu = -1;
            if (!ParseNonNegativeInt(item, &cpu)) {
                *error = "Invalid CPU id `" + item + "`";
                return false;
            }
            cpus->push_back(cpu);
        } else {
            if (item.find('-', dash + 1) != std::string::npos) {
                *error = "Invalid CPU range `" + item + "`";
                return false;
            }
            int first = -1;
            int last = -1;
            if (!ParseNonNegativeInt(item.substr(0, dash), &first) ||
                !ParseNonNegativeInt(item.substr(dash + 1), &last) ||
                first > last) {
                *error = "Invalid CPU range `" + item + "`";
                return false;
            }
            for (int cpu = first; cpu <= last; ++cpu) {
                cpus->push_back(cpu);
            }
        }

        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }

    if (cpus->empty()) {
        *error = "server_worker_affinity_cpus does not contain any CPU";
        return false;
    }

    int duplicated_cpu = -1;
    if (HasDuplicateCpu(*cpus, &duplicated_cpu)) {
        *error = "Duplicated CPU id `" + std::to_string(duplicated_cpu) +
                 "` in server_worker_affinity_cpus";
        return false;
    }
    return true;
}

bool GetCurrentProcessAffinityCpus(std::vector<int>* cpus,
                                   std::string* error) {
    cpus->clear();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        *error = "Fail to get current process CPU affinity, errno=" +
                 std::to_string(errno);
        return false;
    }

    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &cpuset)) {
            cpus->push_back(cpu);
        }
    }
    if (cpus->empty()) {
        *error = "Current process CPU affinity mask does not contain any CPU";
        return false;
    }
    return true;
}

std::string FormatCpuList(const std::vector<int>& cpus) {
    std::string result;
    for (size_t i = 0; i < cpus.size(); ++i) {
        if (i != 0) {
            result.append(",");
        }
        result.append(std::to_string(cpus[i]));
    }
    return result;
}

void BindBthreadWorkerToCpu(bthread_tag_t tag) {
    if (g_worker_affinity_cpus.empty()) {
        return;
    }

    const int index =
        g_next_worker_affinity_index.fetch_add(1, std::memory_order_relaxed);
    const int cpu =
        g_worker_affinity_cpus[index % g_worker_affinity_cpus.size()];
    if (index >= static_cast<int>(g_worker_affinity_cpus.size())) {
        LOG(ERROR) << "More brpc worker pthreads than affinity CPUs, worker_index="
                   << index << " cpu_count=" << g_worker_affinity_cpus.size()
                   << ". Reusing cpu=" << cpu << " by round-robin";
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    const int rc =
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    if (rc != 0) {
        LOG(ERROR) << "Fail to bind brpc worker pthread=" << pthread_self()
                   << " tag=" << tag << " to cpu=" << cpu
                   << ", errno=" << rc;
    } else {
        LOG(INFO) << "Bind brpc worker pthread=" << pthread_self()
                  << " tag=" << tag << " to cpu=" << cpu;
    }
}

bool InitBthreadWorkerAffinity() {
    if (!FLAGS_server_bind_bthread_workers) {
        return true;
    }

    std::string error_text;
    if (!FLAGS_server_worker_affinity_cpus.empty()) {
        if (!ParseCpuList(FLAGS_server_worker_affinity_cpus,
                          &g_worker_affinity_cpus, &error_text)) {
            LOG(ERROR) << error_text;
            return false;
        }
    } else {
        if (!GetCurrentProcessAffinityCpus(&g_worker_affinity_cpus,
                                           &error_text)) {
            LOG(ERROR) << error_text;
            return false;
        }
    }

    if (FLAGS_server_num_threads > 0 &&
        static_cast<int>(g_worker_affinity_cpus.size()) <
            FLAGS_server_num_threads) {
        LOG(ERROR) << "Not enough CPUs for one-worker-one-core binding, "
                   << "server_num_threads=" << FLAGS_server_num_threads
                   << " cpu_count=" << g_worker_affinity_cpus.size()
                   << " cpus=" << FormatCpuList(g_worker_affinity_cpus);
        return false;
    }

    if (bthread_set_tagged_worker_startfn(BindBthreadWorkerToCpu) != 0) {
        LOG(ERROR) << "Fail to set bthread tagged worker start function";
        return false;
    }
    return true;
}

namespace test {
class PerfTestServiceImpl : public PerfTestService {
public:
    PerfTestServiceImpl() {}
    ~PerfTestServiceImpl() {}

    void Test(google::protobuf::RpcController* cntl_base,
              const PerfTestRequest* request,
              PerfTestResponse* response,
              google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        brpc::Controller* cntl = static_cast<brpc::Controller*>(cntl_base);
        if (cntl->request_protocol() != brpc::PROTOCOL_HTTP &&
            cntl->request_protocol() != brpc::PROTOCOL_H2 &&
            request->minimal_response()) {
            response->set_cpu_usage("");
            if (request->echo_attachment()) {
                cntl->response_attachment().append(cntl->request_attachment());
            }
            return;
        }

        uint64_t last = g_last_time.load(butil::memory_order_relaxed);
        uint64_t now = butil::monotonic_time_us();
        std::string cpu_usage;
        if (now > last && now - last > 100000) {
            if (g_last_time.exchange(now, butil::memory_order_relaxed) == last) {
                cpu_usage = bvar::Variable::describe_exposed("process_cpu_usage");
            } else {
                cpu_usage.clear();
            }
        } else {
            cpu_usage.clear();
        }
        if (cntl->request_protocol() == brpc::PROTOCOL_HTTP ||
            cntl->request_protocol() == brpc::PROTOCOL_H2) {
            cntl->http_response().set_content_type("application/json");
            cntl->http_response().SetHeader("X-Server-Cpu-Usage", cpu_usage);
            const std::string* echo_param =
                    cntl->http_request().uri().GetQuery("echo_attachment");
            const bool echo = echo_param != NULL &&
                    (*echo_param == "true" || *echo_param == "1");
            if (echo) {
                cntl->response_attachment().append(cntl->request_attachment());
            } else {
                cntl->response_attachment().append("{\"cpu_usage\":\"");
                cntl->response_attachment().append(cpu_usage);
                cntl->response_attachment().append("\"}");
            }
            return;
        }
        response->set_cpu_usage(cpu_usage);
        if (request->echo_attachment()) {
            cntl->response_attachment().append(cntl->request_attachment());
        }
    }
};
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
    g_transport_explicit = IsFlagExplicitlySet("transport");
    TransportChoice transport;
    if (!ResolveTransportMode(&transport)) {
        return -1;
    }
    if (!InitializeTransportRuntime(transport)) {
        return -1;
    }
    if (!InitBthreadWorkerAffinity()) {
        return -1;
    }

    brpc::Server server;
    test::PerfTestServiceImpl perf_test_service_impl;

    brpc::ServiceOptions service_options;
    service_options.ownership = brpc::SERVER_DOESNT_OWN_SERVICE;
    service_options.allow_http_body_to_pb = false;
    if (server.AddService(&perf_test_service_impl, service_options) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }
    g_last_time.store(0, butil::memory_order_relaxed);

    brpc::ServerOptions options;
    options.socket_mode = transport.socket_mode;
    if (FLAGS_server_num_threads >= 0) {
        options.num_threads = FLAGS_server_num_threads;
    }
    LOG(INFO) << "Starting rdma_performance_server"
              << " port=" << FLAGS_port
              << " transport=" << transport.name
              << " server_num_threads=" << FLAGS_server_num_threads
              << " bthread_concurrency=" << bthread::FLAGS_bthread_concurrency
              << " server_bind_bthread_workers="
              << (FLAGS_server_bind_bthread_workers ? "true" : "false")
              << " worker_affinity_cpus="
              << (g_worker_affinity_cpus.empty()
                      ? std::string("none")
                      : FormatCpuList(g_worker_affinity_cpus));
    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Fail to start EchoServer";
        return -1;
    }

    server.RunUntilAskedToQuit();
    return 0;
}

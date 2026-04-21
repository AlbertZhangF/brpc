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

#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/fast_rand.h"
#include "butil/logging.h"
#include "brpc/rdma/rdma_helper.h"
#include "brpc/server.h"
#include "brpc/channel.h"
#include "bthread/bthread.h"
#include "bvar/latency_recorder.h"
#include "bvar/variable.h"
#include "test.pb.h"

#ifdef BRPC_WITH_RDMA

DEFINE_int32(thread_num, 16, "Number of sender threads");
DEFINE_int32(attachment_size, 1024, "Attachment size in bytes");
DEFINE_bool(echo_attachment, false, "Whether server should echo attachment");
DEFINE_string(connection_type, "single",
              "Connection type: single, pooled, short");
DEFINE_string(protocol, "baidu_std", "Protocol type");
DEFINE_string(servers, "127.0.0.1:8002", "Server addresses separated by +");
DEFINE_bool(use_rdma, false, "Use RDMA or not");
DEFINE_int32(rpc_timeout_ms, 5000, "RPC call timeout");
DEFINE_int32(test_seconds, 10, "Test duration in seconds");
DEFINE_int32(dummy_port, 8001, "Dummy server port");
DEFINE_bool(ignore_eovercrowded, false, "Ignore EOVERCROWDED errors");
DEFINE_int32(max_inflight, 256,
             "Max inflight requests per sender thread");

bvar::LatencyRecorder g_latency_recorder("extreme_client");
bvar::LatencyRecorder g_server_cpu_recorder("extreme_server_cpu");
bvar::LatencyRecorder g_client_cpu_recorder("extreme_client_cpu");
bvar::Adder<uint64_t> g_total_bytes("extreme_total_bytes");
bvar::Adder<uint64_t> g_total_cnt("extreme_total_cnt");
bvar::Adder<uint64_t> g_error_cnt("extreme_error_cnt");
bvar::Adder<uint64_t> g_overcrowded_cnt("extreme_overcrowded_cnt");

butil::atomic<uint64_t> g_last_cpu_time(0);
std::vector<std::string> g_servers;
volatile bool g_stop = false;
brpc::Channel* g_shared_channel = nullptr;

struct RespClosure {
    brpc::Controller* cntl;
    test::PerfTestResponse* resp;
};

void HandleResponse(RespClosure* closure) {
    std::unique_ptr<brpc::Controller> cntl_guard(closure->cntl);
    std::unique_ptr<test::PerfTestResponse> resp_guard(closure->resp);

    if (closure->cntl->Failed()) {
        g_error_cnt << 1;
        if (closure->cntl->ErrorCode() == brpc::EOVERCROWDED) {
            g_overcrowded_cnt << 1;
        }
        delete closure;
        return;
    }

    g_latency_recorder << closure->cntl->latency_us();
    if (closure->resp->cpu_usage().size() > 0) {
        g_server_cpu_recorder << atof(closure->resp->cpu_usage().c_str()) * 100;
    }
    g_total_bytes << closure->cntl->request_attachment().size();
    g_total_cnt << 1;

    uint64_t now = butil::gettimeofday_us();
    uint64_t last = g_last_cpu_time.load(butil::memory_order_relaxed);
    if (now > last && now - last > 100000) {
        if (g_last_cpu_time.exchange(now, butil::memory_order_relaxed) == last) {
            g_client_cpu_recorder <<
                atof(bvar::Variable::describe_exposed("process_cpu_usage").c_str()) * 100;
        }
    }

    delete closure;
}

butil::atomic<int> g_inflight(0);

bool SendRequest(butil::IOBuf& attachment, bool echo_attachment) {
    if (g_stop || !g_shared_channel) return false;

    RespClosure* closure = new RespClosure;
    test::PerfTestRequest request;
    closure->resp = new test::PerfTestResponse();
    closure->cntl = new brpc::Controller();
    if (FLAGS_ignore_eovercrowded) {
        closure->cntl->ignore_eovercrowded();
    }
    request.set_echo_attachment(echo_attachment);
    closure->cntl->request_attachment().append(attachment);
    g_inflight.fetch_add(1, butil::memory_order_relaxed);

    google::protobuf::Closure* done = brpc::NewCallback(&HandleResponse, closure);
    test::PerfTestService_Stub stub(g_shared_channel);
    stub.Test(closure->cntl, &request, closure->resp, done);
    return true;
}

void HandleResponseForInflight(RespClosure* closure) {
    std::unique_ptr<brpc::Controller> cntl_guard(closure->cntl);
    std::unique_ptr<test::PerfTestResponse> resp_guard(closure->resp);
    g_inflight.fetch_sub(1, butil::memory_order_relaxed);

    if (closure->cntl->Failed()) {
        g_error_cnt << 1;
        if (closure->cntl->ErrorCode() == brpc::EOVERCROWDED) {
            g_overcrowded_cnt << 1;
        }
        delete closure;
        return;
    }

    g_latency_recorder << closure->cntl->latency_us();
    if (closure->resp->cpu_usage().size() > 0) {
        g_server_cpu_recorder << atof(closure->resp->cpu_usage().c_str()) * 100;
    }
    g_total_bytes << closure->cntl->request_attachment().size();
    g_total_cnt << 1;

    uint64_t now = butil::gettimeofday_us();
    uint64_t last = g_last_cpu_time.load(butil::memory_order_relaxed);
    if (now > last && now - last > 100000) {
        if (g_last_cpu_time.exchange(now, butil::memory_order_relaxed) == last) {
            g_client_cpu_recorder <<
                atof(bvar::Variable::describe_exposed("process_cpu_usage").c_str()) * 100;
        }
    }

    delete closure;
}

struct SenderArg {
    butil::IOBuf attachment;
    bool echo_attachment;
};

static void* RunSender(void* arg) {
    SenderArg* sa = (SenderArg*)arg;
    uint64_t start_time = butil::gettimeofday_us();

    while (!g_stop) {
        uint64_t now = butil::gettimeofday_us();
        if (now - start_time > (uint64_t)FLAGS_test_seconds * 1000000u) {
            break;
        }

        int current = g_inflight.load(butil::memory_order_relaxed);
        if (current < FLAGS_max_inflight) {
            if (g_stop || !g_shared_channel) break;

            RespClosure* closure = new RespClosure;
            test::PerfTestRequest request;
            closure->resp = new test::PerfTestResponse();
            closure->cntl = new brpc::Controller();
            if (FLAGS_ignore_eovercrowded) {
                closure->cntl->ignore_eovercrowded();
            }
            request.set_echo_attachment(sa->echo_attachment);
            closure->cntl->request_attachment().append(sa->attachment);
            g_inflight.fetch_add(1, butil::memory_order_relaxed);

            google::protobuf::Closure* done = brpc::NewCallback(&HandleResponseForInflight, closure);
            test::PerfTestService_Stub stub(g_shared_channel);
            stub.Test(closure->cntl, &request, closure->resp, done);
        } else {
            bthread_usleep(100);
        }
    }

    return NULL;
}

void RunTest() {
    std::cout << "=== Extreme Concurrency Benchmark ===" << std::endl;
    std::cout << "[Threads: " << FLAGS_thread_num
        << ", MaxInflight: " << FLAGS_max_inflight
        << ", Attachment: " << FLAGS_attachment_size << "B"
        << ", Connection: " << FLAGS_connection_type
        << ", RDMA: " << (FLAGS_use_rdma ? "yes" : "no")
        << ", Echo: " << (FLAGS_echo_attachment ? "yes" : "no")
        << ", IgnoreOvercrowded: " << (FLAGS_ignore_eovercrowded ? "yes" : "no")
        << "]" << std::endl;

    brpc::ChannelOptions options;
    options.use_rdma = FLAGS_use_rdma;
    options.protocol = FLAGS_protocol;
    options.connection_type = FLAGS_connection_type;
    options.timeout_ms = FLAGS_rpc_timeout_ms;
    options.max_retry = 0;

    g_shared_channel = new brpc::Channel();
    std::string server = g_servers[0];
    if (g_shared_channel->Init(server.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to init channel to " << server;
        _exit(1);
    }

    {
        brpc::Controller cntl;
        test::PerfTestResponse resp;
        test::PerfTestRequest req;
        test::PerfTestService_Stub stub(g_shared_channel);
        stub.Test(&cntl, &req, &resp, NULL);
        if (cntl.Failed()) {
            LOG(ERROR) << "Warmup failed: " << cntl.ErrorText();
            _exit(1);
        }
    }
    std::cout << "Warmup done." << std::endl;

    SenderArg sa;
    if (FLAGS_attachment_size > 0) {
        void* addr = malloc(FLAGS_attachment_size);
        butil::fast_rand_bytes(addr, FLAGS_attachment_size);
        sa.attachment.append(addr, FLAGS_attachment_size);
        free(addr);
    }
    sa.echo_attachment = FLAGS_echo_attachment;

    uint64_t start_time = butil::gettimeofday_us();

    std::vector<bthread_t> tids(FLAGS_thread_num);
    for (int i = 0; i < FLAGS_thread_num; ++i) {
        bthread_start_background(&tids[i], &BTHREAD_ATTR_NORMAL,
                RunSender, &sa);
    }

    for (int sec = 0; sec < FLAGS_test_seconds; ++sec) {
        bthread_usleep(1000000);
        uint64_t now = butil::gettimeofday_us();
        uint64_t elapsed_us = now - start_time;
        uint64_t cnt = g_total_cnt.get_value();
        uint64_t bytes = g_total_bytes.get_value();
        double qps = (elapsed_us > 0) ? (double)cnt * 1000000 / elapsed_us : 0;
        double throughput_mb = (elapsed_us > 0) ? (double)bytes / 1.048576 / elapsed_us : 0;
        int total_inflight = g_inflight.load(butil::memory_order_relaxed);

        std::cout << "[" << (sec + 1) << "s] "
            << "QPS: " << (uint64_t)qps
            << ", Throughput: " << throughput_mb << "MB/s"
            << ", Inflight: " << total_inflight
            << ", Errors: " << g_error_cnt.get_value()
            << ", Overcrowded: " << g_overcrowded_cnt.get_value()
            << std::endl;
    }

    g_stop = true;

    for (int i = 0; i < 200; ++i) {
        int total_inflight = g_inflight.load(butil::memory_order_relaxed);
        if (total_inflight == 0) break;
        bthread_usleep(10000);
    }

    uint64_t end_time = butil::gettimeofday_us();
    double elapsed_s = (end_time - start_time) / 1000000.0;

    uint64_t total_cnt = g_total_cnt.get_value();
    uint64_t total_bytes = g_total_bytes.get_value();
    uint64_t total_errors = g_error_cnt.get_value();
    uint64_t total_overcrowded = g_overcrowded_cnt.get_value();

    std::cout << "\n=== Final Results ===" << std::endl;
    std::cout << "Duration: " << elapsed_s << "s" << std::endl;
    std::cout << "Total requests: " << total_cnt << std::endl;
    std::cout << "Total errors: " << total_errors << std::endl;
    std::cout << "Overcrowded rejections: " << total_overcrowded << std::endl;
    std::cout << "QPS: " << (uint64_t)(total_cnt / elapsed_s) << std::endl;
    std::cout << "Throughput: " << (total_bytes / 1.048576 / (end_time - start_time)) << "MB/s" << std::endl;
    std::cout << "Avg-Latency: " << g_latency_recorder.latency(10)
        << ", 90th: " << g_latency_recorder.latency_percentile(0.9)
        << ", 99th: " << g_latency_recorder.latency_percentile(0.99)
        << ", 99.9th: " << g_latency_recorder.latency_percentile(0.999) << std::endl;
    std::cout << "Server CPU: " << g_server_cpu_recorder.latency(10) << "%" << std::endl;
    std::cout << "Client CPU: " << g_client_cpu_recorder.latency(10) << "%" << std::endl;

    if (total_overcrowded > 0) {
        std::cout << "\n*** EOVERCROWDED detected! Socket write buffer overflow. ***" << std::endl;
        std::cout << "Try: --ignore_eovercrowded=true --socket_max_unwritten_bytes=268435456" << std::endl;
    }

    delete g_shared_channel;
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_use_rdma) {
        brpc::rdma::GlobalRdmaInitializeOrDie();
    }

    brpc::StartDummyServerAt(FLAGS_dummy_port);

    std::string::size_type pos1 = 0;
    std::string::size_type pos2 = FLAGS_servers.find('+');
    while (pos2 != std::string::npos) {
        g_servers.push_back(FLAGS_servers.substr(pos1, pos2 - pos1));
        pos1 = pos2 + 1;
        pos2 = FLAGS_servers.find('+', pos1);
    }
    g_servers.push_back(FLAGS_servers.substr(pos1));

    RunTest();

    _exit(0);
}

#else

int main(int argc, char* argv[]) {
    LOG(ERROR) << "brpc is not compiled with rdma";
    return 0;
}

#endif

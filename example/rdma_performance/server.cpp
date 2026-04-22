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


#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/logging.h"
#include "butil/time.h"
#include "brpc/rdma/rdma_helper.h"
#include "brpc/server.h"
#include "bvar/variable.h"
#include "test.pb.h"

#ifdef BRPC_WITH_RDMA

DEFINE_int32(port, 8002, "TCP Port of this server");
DEFINE_bool(use_rdma, false, "Use RDMA or not");
DEFINE_int32(num_threads, 0, "Number of worker threads (0=#cpu-cores)");
DEFINE_int32(perf_rdma_sq_size, 1024, "RDMA SQ size for this test");
DEFINE_int32(perf_rdma_rq_size, 1024, "RDMA RQ size for this test");
DEFINE_bool(perf_rdma_use_polling, true, "Use RDMA polling mode for this test");
DEFINE_int32(perf_rdma_poller_num, 4, "Number of RDMA polling threads for this test");
DEFINE_int32(perf_rdma_cqe_poll_once, 64, "Max CQEs polled per CQ poll for this test");
DEFINE_int32(perf_socket_recv_buf, -1, "Socket recv buffer size (-1=system default)");
DEFINE_int32(perf_socket_send_buf, -1, "Socket send buffer size (-1=system default)");

butil::atomic<uint64_t> g_last_time(0);

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
        uint64_t last = g_last_time.load(butil::memory_order_relaxed);
        uint64_t now = butil::monotonic_time_us();
        if (now > last && now - last > 100000) {
            if (g_last_time.exchange(now, butil::memory_order_relaxed) == last) {
                response->set_cpu_usage(bvar::Variable::describe_exposed("process_cpu_usage"));
            } else {
                response->set_cpu_usage("");
            }
        } else {
            response->set_cpu_usage("");
        }
        if (request->echo_attachment()) {
            brpc::Controller* cntl =
                static_cast<brpc::Controller*>(cntl_base);
            cntl->response_attachment().append(cntl->request_attachment());
        }
    }
};
}

void ApplyCommonFlags() {
    if (FLAGS_perf_socket_recv_buf > 0) {
        GFLAGS_NAMESPACE::SetCommandLineOption("socket_recv_buffer_size",
            std::to_string(FLAGS_perf_socket_recv_buf).c_str());
    }
    if (FLAGS_perf_socket_send_buf > 0) {
        GFLAGS_NAMESPACE::SetCommandLineOption("socket_send_buffer_size",
            std::to_string(FLAGS_perf_socket_send_buf).c_str());
    }

    LOG(INFO) << "Common configuration: socket_recv_buf=" << FLAGS_perf_socket_recv_buf
              << ", socket_send_buf=" << FLAGS_perf_socket_send_buf;
}

void ApplyRdmaFlags() {
    if (!FLAGS_use_rdma) return;

    GFLAGS_NAMESPACE::SetCommandLineOption("rdma_sq_size", std::to_string(FLAGS_perf_rdma_sq_size).c_str());
    GFLAGS_NAMESPACE::SetCommandLineOption("rdma_rq_size", std::to_string(FLAGS_perf_rdma_rq_size).c_str());
    GFLAGS_NAMESPACE::SetCommandLineOption("rdma_use_polling", FLAGS_perf_rdma_use_polling ? "true" : "false");
    GFLAGS_NAMESPACE::SetCommandLineOption("rdma_poller_num", std::to_string(FLAGS_perf_rdma_poller_num).c_str());
    GFLAGS_NAMESPACE::SetCommandLineOption("rdma_cqe_poll_once", std::to_string(FLAGS_perf_rdma_cqe_poll_once).c_str());
    GFLAGS_NAMESPACE::SetCommandLineOption("rdma_prepared_qp_size", std::to_string(FLAGS_perf_rdma_sq_size).c_str());

    LOG(INFO) << "RDMA configuration: sq_size=" << FLAGS_perf_rdma_sq_size
              << ", rq_size=" << FLAGS_perf_rdma_rq_size
              << ", polling=" << FLAGS_perf_rdma_use_polling
              << ", poller_num=" << FLAGS_perf_rdma_poller_num
              << ", cqe_poll_once=" << FLAGS_perf_rdma_cqe_poll_once;
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    ApplyCommonFlags();
    ApplyRdmaFlags();

    if (FLAGS_use_rdma) {
        brpc::rdma::GlobalRdmaInitializeOrDie();
    }

    brpc::Server server;
    test::PerfTestServiceImpl perf_test_service_impl;

    if (server.AddService(&perf_test_service_impl, 
                          brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }
    g_last_time.store(0, butil::memory_order_relaxed);

    brpc::ServerOptions options;
    options.use_rdma = FLAGS_use_rdma;
    if (FLAGS_num_threads > 0) {
        options.num_threads = FLAGS_num_threads;
    }
    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Fail to start EchoServer";
        return -1;
    }

    LOG(INFO) << "Server started on port " << FLAGS_port
              << " with RDMA=" << (FLAGS_use_rdma ? "yes" : "no")
              << " num_threads=" << (FLAGS_num_threads > 0 ? FLAGS_num_threads : 0);

    server.RunUntilAskedToQuit();
    return 0;
}

#else


int main(int argc, char* argv[]) {
    LOG(ERROR) << " brpc is not compiled with rdma. To enable it, please refer to https://github.com/apache/brpc/blob/master/docs/en/rdma.md";
    return 0;
}

#endif

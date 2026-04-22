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

DEFINE_int32(thread_num, 0, "How many threads are used");
DEFINE_int32(queue_depth, 64, "How many requests can be pending per thread");
DEFINE_int32(expected_qps, 0, "The expected QPS (0 means unlimited)");
DEFINE_int32(max_thread_num, 16, "The max number of threads are used");
DEFINE_int32(attachment_size, -1, "Attachment size is used (in Bytes)");
DEFINE_bool(echo_attachment, false, "Select whether attachment should be echo");
DEFINE_string(connection_type, "pooled", "Connection type of the channel");
DEFINE_string(protocol, "baidu_std", "Protocol type.");
DEFINE_string(servers, "0.0.0.0:8002", "IP Address of servers");
DEFINE_bool(use_rdma, false, "Use RDMA or not");
DEFINE_int32(rpc_timeout_ms, 5000, "RPC call timeout");
DEFINE_int32(test_seconds, 20, "Test running time");
DEFINE_int32(test_iterations, 0, "Test iterations");
DEFINE_int32(dummy_port, 8001, "Dummy server port number");
DEFINE_int32(perf_rdma_sq_size, 1024, "RDMA SQ size for this test");
DEFINE_int32(perf_rdma_rq_size, 1024, "RDMA RQ size for this test");
DEFINE_bool(perf_rdma_use_polling, true, "Use RDMA polling mode for this test");
DEFINE_int32(perf_rdma_poller_num, 4, "Number of RDMA polling threads for this test");
DEFINE_int32(perf_rdma_cqe_poll_once, 64, "Max CQEs polled per CQ poll for this test");
DEFINE_int32(channel_num, 0, "Number of channels per thread (0=auto)");
DEFINE_int32(perf_max_conn_pool_size, 1000, "Max pooled connections per endpoint");
DEFINE_int32(perf_socket_recv_buf, -1, "Socket recv buffer size (-1=system default)");
DEFINE_int32(perf_socket_send_buf, -1, "Socket send buffer size (-1=system default)");
DEFINE_int32(perf_bthread_concurrency, 0, "bthread worker concurrency (0=auto)");

bvar::LatencyRecorder g_latency_recorder("client");
bvar::LatencyRecorder g_server_cpu_recorder("server_cpu");
bvar::LatencyRecorder g_client_cpu_recorder("client_cpu");
butil::atomic<uint64_t> g_last_time(0);
butil::atomic<uint64_t> g_total_bytes;
butil::atomic<uint64_t> g_total_cnt;
butil::atomic<uint64_t> g_error_cnt;
std::vector<std::string> g_servers;
butil::atomic<int> g_rr_index(0);
volatile bool g_stop = false;

butil::atomic<int64_t> g_token(10000);

static void* GenerateToken(void* arg) {
    int64_t start_time = butil::monotonic_time_ns();
    int64_t accumulative_token = g_token.load(butil::memory_order_relaxed);
    while (!g_stop) {
        bthread_usleep(100000);
        int64_t now = butil::monotonic_time_ns();
        if (accumulative_token * 1000000000 / (now - start_time) < FLAGS_expected_qps) {
            int64_t delta = FLAGS_expected_qps * (now - start_time) / 1000000000 - accumulative_token;
            g_token.fetch_add(delta, butil::memory_order_relaxed);
            accumulative_token += delta;
        }
    }
    return NULL;
}

class PerformanceTest;

struct RequestContext {
    brpc::Controller cntl;
    test::PerfTestResponse resp;
    PerformanceTest* test;
    int channel_index;
};

class PerformanceTest {
public:
    PerformanceTest(int attachment_size, bool echo_attachment, int num_channels)
        : _addr(NULL)
        , _start_time(0)
        , _iterations(0)
        , _stop(false)
        , _inflight(0)
        , _echo_attachment(echo_attachment)
        , _num_channels(num_channels)
        , _channel_counter(0)
    {
        if (attachment_size > 0) {
            _addr = malloc(attachment_size);
            butil::fast_rand_bytes(_addr, attachment_size);
            _attachment.append(_addr, attachment_size);
        }
        _channels.resize(_num_channels, NULL);
    }

    ~PerformanceTest() {
        for (int i = 0; i < _num_channels; ++i) {
            delete _channels[i];
        }
        if (_addr) {
            free(_addr);
        }
    }

    inline bool IsStop() { return _stop; }

    int Init() {
        for (int i = 0; i < _num_channels; ++i) {
            brpc::ChannelOptions options;
            options.use_rdma = FLAGS_use_rdma;
            options.protocol = FLAGS_protocol;
            options.connection_type = FLAGS_connection_type;
            options.timeout_ms = FLAGS_rpc_timeout_ms;
            options.max_retry = 0;
            int server_idx = g_rr_index.fetch_add(1, butil::memory_order_relaxed) % g_servers.size();
            std::string server = g_servers[server_idx];
            _channels[i] = new brpc::Channel();
            if (_channels[i]->Init(server.c_str(), &options) != 0) {
                LOG(ERROR) << "Fail to initialize channel " << i;
                return -1;
            }
        }
        {
            brpc::Controller cntl;
            test::PerfTestResponse response;
            test::PerfTestRequest request;
            request.set_echo_attachment(_echo_attachment);
            test::PerfTestService_Stub stub(_channels[0]);
            stub.Test(&cntl, &request, &response, NULL);
            if (cntl.Failed()) {
                LOG(ERROR) << "RPC call failed: " << cntl.ErrorText();
                return -1;
            }
        }
        return 0;
    }

    void SendRequest() {
        if (_stop) return;
        if (FLAGS_expected_qps > 0) {
            while (g_token.load(butil::memory_order_relaxed) <= 0 && !g_stop) {
                bthread_usleep(1);
            }
            if (g_stop) return;
            g_token.fetch_sub(1, butil::memory_order_relaxed);
        }

        RequestContext* ctx = new RequestContext();
        ctx->test = this;
        ctx->channel_index = _channel_counter.fetch_add(1, butil::memory_order_relaxed) % _num_channels;
        ctx->cntl.request_attachment().append(_attachment);

        test::PerfTestRequest request;
        request.set_echo_attachment(_echo_attachment);

        _inflight.fetch_add(1, butil::memory_order_relaxed);

        test::PerfTestService_Stub stub(_channels[ctx->channel_index]);
        google::protobuf::Closure* done = brpc::NewCallback(&HandleResponse, ctx);
        stub.Test(&ctx->cntl, &request, &ctx->resp, done);
    }

    static void HandleResponse(RequestContext* ctx) {
        PerformanceTest* test = ctx->test;

        if (ctx->cntl.Failed()) {
            g_error_cnt.fetch_add(1, butil::memory_order_relaxed);
            if (ctx->cntl.ErrorCode() != brpc::ELOGOFF &&
                ctx->cntl.ErrorCode() != brpc::ERPCTIMEDOUT) {
                LOG(ERROR) << "RPC call failed: " << ctx->cntl.ErrorText();
                test->_stop = true;
            }
            delete ctx;
            test->_inflight.fetch_sub(1, butil::memory_order_relaxed);
            return;
        }

        g_latency_recorder << ctx->cntl.latency_us();
        if (ctx->resp.cpu_usage().size() > 0) {
            g_server_cpu_recorder << atof(ctx->resp.cpu_usage().c_str()) * 100;
        }
        g_total_bytes.fetch_add(ctx->cntl.request_attachment().size(), butil::memory_order_relaxed);
        g_total_cnt.fetch_add(1, butil::memory_order_relaxed);

        if (test->_iterations == 0 && FLAGS_test_iterations > 0) {
            test->_stop = true;
            delete ctx;
            test->_inflight.fetch_sub(1, butil::memory_order_relaxed);
            return;
        }
        --test->_iterations;

        uint64_t last = g_last_time.load(butil::memory_order_relaxed);
        uint64_t now = butil::gettimeofday_us();
        if (now > last && now - last > 100000) {
            if (g_last_time.exchange(now, butil::memory_order_relaxed) == last) {
                g_client_cpu_recorder <<
                    atof(bvar::Variable::describe_exposed("process_cpu_usage").c_str()) * 100;
            }
        }
        if (now - test->_start_time > (uint64_t)FLAGS_test_seconds * 1000000u) {
            test->_stop = true;
            delete ctx;
            test->_inflight.fetch_sub(1, butil::memory_order_relaxed);
            return;
        }

        delete ctx;
        test->_inflight.fetch_sub(1, butil::memory_order_relaxed);
        test->SendRequest();
    }

    static void* RunTest(void* arg) {
        PerformanceTest* test = (PerformanceTest*)arg;
        test->_start_time = butil::gettimeofday_us();
        test->_iterations = FLAGS_test_iterations;

        for (int i = 0; i < FLAGS_queue_depth; ++i) {
            test->SendRequest();
        }

        while (!test->_stop) {
            bthread_usleep(1000);
        }

        while (test->_inflight.load(butil::memory_order_relaxed) > 0) {
            bthread_usleep(1000);
        }

        return NULL;
    }

private:
    void* _addr;
    std::vector<brpc::Channel*> _channels;
    uint64_t _start_time;
    uint32_t _iterations;
    volatile bool _stop;
    butil::IOBuf _attachment;
    bool _echo_attachment;
    int _num_channels;
    butil::atomic<int> _channel_counter;
    butil::atomic<int> _inflight;
};

static void* DeleteTest(void* arg) {
    PerformanceTest* test = (PerformanceTest*)arg;
    delete test;
    return NULL;
}

void ApplyCommonFlags() {
    if (FLAGS_perf_max_conn_pool_size > 0) {
        GFLAGS_NAMESPACE::SetCommandLineOption("max_connection_pool_size",
            std::to_string(FLAGS_perf_max_conn_pool_size).c_str());
    }
    if (FLAGS_perf_socket_recv_buf > 0) {
        GFLAGS_NAMESPACE::SetCommandLineOption("socket_recv_buffer_size",
            std::to_string(FLAGS_perf_socket_recv_buf).c_str());
    }
    if (FLAGS_perf_socket_send_buf > 0) {
        GFLAGS_NAMESPACE::SetCommandLineOption("socket_send_buffer_size",
            std::to_string(FLAGS_perf_socket_send_buf).c_str());
    }
    if (FLAGS_perf_bthread_concurrency > 0) {
        GFLAGS_NAMESPACE::SetCommandLineOption("bthread_concurrency",
            std::to_string(FLAGS_perf_bthread_concurrency).c_str());
    }

    LOG(INFO) << "Common configuration: max_conn_pool=" << FLAGS_perf_max_conn_pool_size
              << ", socket_recv_buf=" << FLAGS_perf_socket_recv_buf
              << ", socket_send_buf=" << FLAGS_perf_socket_send_buf
              << ", bthread_concurrency=" << FLAGS_perf_bthread_concurrency;
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

void Test(int thread_num, int attachment_size) {
    int num_channels = FLAGS_channel_num > 0 ? FLAGS_channel_num : std::max(1, thread_num);
    std::cout << "[Threads: " << thread_num
        << ", Depth: " << FLAGS_queue_depth
        << ", Attachment: " << attachment_size << "B"
        << ", RDMA: " << (FLAGS_use_rdma ? "yes" : "no")
        << ", Echo: " << (FLAGS_echo_attachment ? "yes" : "no")
        << ", ConnType: " << FLAGS_connection_type
        << ", Channels: " << num_channels
        << ", ConnPool: " << FLAGS_perf_max_conn_pool_size
        << "]" << std::endl;
    g_total_bytes.store(0, butil::memory_order_relaxed);
    g_total_cnt.store(0, butil::memory_order_relaxed);
    g_error_cnt.store(0, butil::memory_order_relaxed);
    std::vector<PerformanceTest*> tests;
    for (int k = 0; k < thread_num; ++k) {
        PerformanceTest* t = new PerformanceTest(attachment_size, FLAGS_echo_attachment, num_channels);
        if (t->Init() < 0) {
            exit(1);
        }
        tests.push_back(t);
    }
    uint64_t start_time = butil::gettimeofday_us();
    bthread_t* tid = new bthread_t[thread_num];
    if (FLAGS_expected_qps > 0) {
        bthread_t token_tid;
        bthread_start_background(&token_tid, &BTHREAD_ATTR_NORMAL, GenerateToken, NULL);
    }
    for (int k = 0; k < thread_num; ++k) {
        bthread_start_background(&tid[k], &BTHREAD_ATTR_NORMAL,
                PerformanceTest::RunTest, tests[k]);
    }
    for (int k = 0; k < thread_num; ++k) {
        while (!tests[k]->IsStop()) {
            bthread_usleep(10000);
        }
    }
    uint64_t end_time = butil::gettimeofday_us();
    double elapsed_s = (end_time - start_time) / 1000000.0;
    uint64_t total_cnt = g_total_cnt.load(butil::memory_order_relaxed);
    uint64_t error_cnt = g_error_cnt.load(butil::memory_order_relaxed);
    double throughput = g_total_bytes.load(butil::memory_order_relaxed) / 1.048576 / (end_time - start_time);
    double qps = total_cnt / elapsed_s;
    if (FLAGS_test_iterations == 0) {
        std::cout << "QPS: " << (uint64_t)qps
            << ", Avg-Latency: " << g_latency_recorder.latency(10)
            << ", 90th: " << g_latency_recorder.latency_percentile(0.9)
            << ", 99th: " << g_latency_recorder.latency_percentile(0.99)
            << ", 99.9th: " << g_latency_recorder.latency_percentile(0.999)
            << ", Throughput: " << throughput << "MB/s"
            << ", Errors: " << error_cnt
            << ", Server-CPU: " << g_server_cpu_recorder.latency(10) << "%"
            << ", Client-CPU: " << g_client_cpu_recorder.latency(10) << "%"
            << std::endl;
    } else {
        std::cout << " Throughput: " << throughput << "MB/s" << std::endl;
    }
    g_stop = true;
    for (int k = 0; k < thread_num; ++k) {
        bthread_start_background(&tid[k], &BTHREAD_ATTR_NORMAL, DeleteTest, tests[k]);
    }
    delete[] tid;
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    ApplyCommonFlags();
    ApplyRdmaFlags();

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

    if (FLAGS_thread_num > 0 && FLAGS_attachment_size >= 0) {
        Test(FLAGS_thread_num, FLAGS_attachment_size);
    } else if (FLAGS_thread_num <= 0 && FLAGS_attachment_size >= 0) {
        for (int i = 1; i <= FLAGS_max_thread_num; i *= 2) {
            Test(i, FLAGS_attachment_size);
        }
    } else if (FLAGS_thread_num > 0 && FLAGS_attachment_size < 0) {
        for (int i = 1; i <= 1024; i *= 4) {
            Test(FLAGS_thread_num, i);
        }
    } else {
        for (int j = 1; j <= 1024; j *= 4) {
            for (int i = 1; i <= FLAGS_max_thread_num; i *= 2) {
                Test(i, j);
            }
        }
    }

    return 0;
}

#else

int main(int argc, char* argv[]) {
    LOG(ERROR) << " brpc is not compiled with rdma. To enable it, please refer to https://github.com/apache/brpc/blob/master/docs/en/rdma.md";
    return 0;
}

#endif

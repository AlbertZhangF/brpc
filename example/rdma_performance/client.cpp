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
DEFINE_int32(max_inflight, 10000, "Max inflight requests per thread (0=unlimited)");
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
DEFINE_int32(channel_num, 0, "Number of shared channels (0=auto, =min(thread_num, 16))");
DEFINE_int32(perf_max_conn_pool_size, 1000, "Max pooled connections per endpoint");
DEFINE_int32(perf_socket_recv_buf, -1, "Socket recv buffer size (-1=system default)");
DEFINE_int32(perf_socket_send_buf, -1, "Socket send buffer size (-1=system default)");
DEFINE_int32(perf_bthread_concurrency, 0, "bthread worker concurrency (0=CPU cores)");
DEFINE_int32(warmup_timeout_ms, 30000, "Timeout for connection warmup in ms");
DEFINE_int32(warmup_batch_size, 10, "Number of channels to warmup in parallel");

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
std::vector<brpc::Channel*> g_channels;
butil::atomic<int> g_channel_counter(0);

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
};

class PerformanceTest {
public:
    PerformanceTest(int attachment_size, bool echo_attachment)
        : _addr(NULL)
        , _start_time(0)
        , _remaining_iterations(0)
        , _stop(false)
        , _inflight(0)
        , _echo_attachment(echo_attachment)
        , _consecutive_errors(0)
    {
        if (attachment_size > 0) {
            _addr = malloc(attachment_size);
            butil::fast_rand_bytes(_addr, attachment_size);
            _attachment.append(_addr, attachment_size);
        }
    }

    ~PerformanceTest() {
        if (_addr) {
            free(_addr);
        }
    }

    inline bool IsStop() { return _stop; }

    bool SendRequest() {
        if (_stop) return false;

        if (FLAGS_test_iterations > 0) {
            if (_remaining_iterations.fetch_sub(1, butil::memory_order_relaxed) <= 0) {
                _remaining_iterations.fetch_add(1, butil::memory_order_relaxed);
                if (_inflight.load(butil::memory_order_relaxed) == 0) {
                    _stop = true;
                }
                return false;
            }
        }

        if (FLAGS_max_inflight > 0) {
            int old_val = _inflight.load(butil::memory_order_relaxed);
            while (old_val < FLAGS_max_inflight) {
                if (_inflight.compare_exchange_strong(old_val, old_val + 1,
                        butil::memory_order_relaxed)) {
                    break;
                }
            }
            if (old_val >= FLAGS_max_inflight) {
                return false;
            }
        } else {
            _inflight.fetch_add(1, butil::memory_order_relaxed);
        }

        if (FLAGS_expected_qps > 0) {
            if (g_token.load(butil::memory_order_relaxed) <= 0) {
                _inflight.fetch_sub(1, butil::memory_order_relaxed);
                return false;
            }
            g_token.fetch_sub(1, butil::memory_order_relaxed);
        }

        RequestContext* ctx = new RequestContext();
        ctx->test = this;
        ctx->cntl.request_attachment().append(_attachment);

        test::PerfTestRequest request;
        request.set_echo_attachment(_echo_attachment);

        int channel_idx = g_channel_counter.fetch_add(1, butil::memory_order_relaxed) % g_channels.size();
        test::PerfTestService_Stub stub(g_channels[channel_idx]);
        google::protobuf::Closure* done = brpc::NewCallback(&HandleResponse, ctx);
        stub.Test(&ctx->cntl, &request, &ctx->resp, done);
        return true;
    }

    static void HandleResponse(RequestContext* ctx) {
        PerformanceTest* test = ctx->test;

        if (ctx->cntl.Failed()) {
            g_error_cnt.fetch_add(1, butil::memory_order_relaxed);
            int errors = test->_consecutive_errors.fetch_add(1, butil::memory_order_relaxed) + 1;
            if (errors >= MAX_CONSECUTIVE_ERRORS) {
                LOG(ERROR) << "Too many consecutive errors (" << errors << "), stopping. Last error: "
                           << ctx->cntl.ErrorText();
                test->_stop = true;
            } else if (ctx->cntl.ErrorCode() != brpc::ELOGOFF &&
                       ctx->cntl.ErrorCode() != brpc::ERPCTIMEDOUT &&
                       ctx->cntl.ErrorCode() != brpc::EFAILEDSOCKET) {
                LOG(WARNING) << "RPC call failed: " << ctx->cntl.ErrorText()
                             << " (consecutive_errors=" << errors << ")";
            }
            delete ctx;
            test->_inflight.fetch_sub(1, butil::memory_order_relaxed);
            return;
        }

        test->_consecutive_errors.store(0, butil::memory_order_relaxed);

        g_latency_recorder << ctx->cntl.latency_us();
        if (ctx->resp.cpu_usage().size() > 0) {
            g_server_cpu_recorder << atof(ctx->resp.cpu_usage().c_str()) * 100;
        }
        g_total_bytes.fetch_add(ctx->cntl.request_attachment().size(), butil::memory_order_relaxed);
        g_total_cnt.fetch_add(1, butil::memory_order_relaxed);

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

        if (FLAGS_test_iterations > 0 &&
            test->_remaining_iterations.load(butil::memory_order_relaxed) == 0 &&
            test->_inflight.load(butil::memory_order_relaxed) == 0) {
            test->_stop = true;
        }
    }

    static void* RunTest(void* arg) {
        PerformanceTest* test = (PerformanceTest*)arg;
        test->_start_time = butil::gettimeofday_us();
        test->_remaining_iterations.store(FLAGS_test_iterations, butil::memory_order_relaxed);

        while (!test->_stop) {
            if (!test->SendRequest()) {
                bthread_usleep(1);
            }
        }

        uint64_t drain_start = butil::gettimeofday_us();
        while (test->_inflight.load(butil::memory_order_relaxed) > 0) {
            bthread_usleep(100);
            if (butil::gettimeofday_us() - drain_start > 5000000) {
                LOG(WARNING) << "Drain timeout, " << test->_inflight.load() << " requests still in flight";
                break;
            }
        }

        return NULL;
    }

private:
    void* _addr;
    uint64_t _start_time;
    butil::atomic<uint32_t> _remaining_iterations;
    volatile bool _stop;
    butil::IOBuf _attachment;
    bool _echo_attachment;
    butil::atomic<int> _inflight;
    butil::atomic<int> _consecutive_errors;
    static const int MAX_CONSECUTIVE_ERRORS = 100;
};

static void* DeleteTest(void* arg) {
    PerformanceTest* test = (PerformanceTest*)arg;
    delete test;
    return NULL;
}

int InitGlobalChannels(int num_channels) {
    for (int i = 0; i < num_channels; ++i) {
        brpc::ChannelOptions options;
        options.use_rdma = FLAGS_use_rdma;
        options.protocol = FLAGS_protocol;
        options.connection_type = FLAGS_connection_type;
        options.timeout_ms = FLAGS_rpc_timeout_ms;
        options.max_retry = 0;
        int server_idx = g_rr_index.fetch_add(1, butil::memory_order_relaxed) % g_servers.size();
        std::string server = g_servers[server_idx];
        brpc::Channel* channel = new brpc::Channel();
        if (channel->Init(server.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to initialize channel " << i << " to " << server;
            delete channel;
            return -1;
        }
        g_channels.push_back(channel);
    }
    return 0;
}

struct WarmupContext {
    brpc::Controller cntl;
    test::PerfTestResponse resp;
    butil::atomic<bool> done;
    bool success;
    WarmupContext() : done(false), success(false) {}
};

static void WarmupDone(WarmupContext* ctx) {
    ctx->success = !ctx->cntl.Failed();
    if (!ctx->success) {
        LOG(ERROR) << "Warmup RPC failed: " << ctx->cntl.ErrorText();
    }
    ctx->done.store(true, butil::memory_order_release);
}

int WarmupChannels() {
    LOG(INFO) << "Warming up " << g_channels.size() << " channels...";
    int batch_size = FLAGS_warmup_batch_size;
    if (batch_size <= 0) batch_size = 10;
    
    int warmed = 0;
    int failed = 0;
    
    for (size_t i = 0; i < g_channels.size(); i += batch_size) {
        size_t end = std::min(i + batch_size, g_channels.size());
        std::vector<WarmupContext*> ctxs(end - i);
        
        for (size_t j = i; j < end; ++j) {
            ctxs[j - i] = new WarmupContext();
            test::PerfTestRequest request;
            request.set_echo_attachment(FLAGS_echo_attachment);
            test::PerfTestService_Stub stub(g_channels[j]);
            ctxs[j - i]->cntl.set_timeout_ms(FLAGS_warmup_timeout_ms);
            google::protobuf::Closure* done = brpc::NewCallback(&WarmupDone, ctxs[j - i]);
            stub.Test(&ctxs[j - i]->cntl, &request, &ctxs[j - i]->resp, done);
        }
        
        int64_t wait_start = butil::gettimeofday_ms();
        for (size_t j = i; j < end; ++j) {
            while (!ctxs[j - i]->done.load(butil::memory_order_acquire)) {
                bthread_usleep(1000);
                if (butil::gettimeofday_ms() - wait_start > FLAGS_warmup_timeout_ms) {
                    LOG(ERROR) << "Warmup timeout for channel " << j;
                    break;
                }
            }
            if (ctxs[j - i]->success) {
                warmed++;
            } else {
                failed++;
            }
            delete ctxs[j - i];
        }
        
        if (failed > 0 && failed > static_cast<int>(g_channels.size()) / 2) {
            LOG(ERROR) << "Too many warmup failures (" << failed << "/" << g_channels.size() << ")";
            return -1;
        }
    }
    
    LOG(INFO) << "Warmup completed: " << warmed << "/" << g_channels.size() << " channels ready";
    return (warmed > 0) ? 0 : -1;
}

void CleanupGlobalChannels() {
    for (size_t i = 0; i < g_channels.size(); ++i) {
        delete g_channels[i];
    }
    g_channels.clear();
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
    
    int bthread_conc = FLAGS_perf_bthread_concurrency;
    if (bthread_conc <= 0) {
        bthread_conc = sysconf(_SC_NPROCESSORS_ONLN);
        if (bthread_conc <= 0) bthread_conc = 8;
    }
    GFLAGS_NAMESPACE::SetCommandLineOption("bthread_concurrency",
        std::to_string(bthread_conc).c_str());

    LOG(INFO) << "Common configuration: max_conn_pool=" << FLAGS_perf_max_conn_pool_size
              << ", socket_recv_buf=" << FLAGS_perf_socket_recv_buf
              << ", socket_send_buf=" << FLAGS_perf_socket_send_buf
              << ", bthread_concurrency=" << bthread_conc;
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
    int num_channels = FLAGS_channel_num > 0 ? FLAGS_channel_num : std::min(thread_num, 16);
    
    std::cout << "[Threads: " << thread_num
        << ", MaxInflight: " << (FLAGS_max_inflight > 0 ? std::to_string(FLAGS_max_inflight) : "unlimited")
        << ", Attachment: " << attachment_size << "B"
        << ", RDMA: " << (FLAGS_use_rdma ? "yes" : "no")
        << ", Echo: " << (FLAGS_echo_attachment ? "yes" : "no")
        << ", ConnType: " << FLAGS_connection_type
        << ", SharedChannels: " << num_channels
        << ", ConnPool: " << FLAGS_perf_max_conn_pool_size
        << "]" << std::endl;
    
    g_total_bytes.store(0, butil::memory_order_relaxed);
    g_total_cnt.store(0, butil::memory_order_relaxed);
    g_error_cnt.store(0, butil::memory_order_relaxed);
    g_channel_counter.store(0, butil::memory_order_relaxed);
    
    if (InitGlobalChannels(num_channels) != 0) {
        LOG(ERROR) << "Failed to initialize global channels";
        return;
    }
    
    if (WarmupChannels() != 0) {
        LOG(ERROR) << "Failed to warmup channels";
        CleanupGlobalChannels();
        return;
    }
    
    std::vector<PerformanceTest*> tests;
    for (int k = 0; k < thread_num; ++k) {
        PerformanceTest* t = new PerformanceTest(attachment_size, FLAGS_echo_attachment);
        tests.push_back(t);
    }
    
    uint64_t start_time = butil::gettimeofday_us();
    bthread_t* tid = new bthread_t[thread_num];
    
    if (FLAGS_expected_qps > 0) {
        bthread_t token_tid;
        bthread_start_background(&token_tid, &BTHREAD_ATTR_NORMAL, GenerateToken, NULL);
    }
    
    int batch_launch = 8;
    for (int k = 0; k < thread_num; k += batch_launch) {
        int end = std::min(k + batch_launch, thread_num);
        for (int i = k; i < end; ++i) {
            bthread_start_background(&tid[i], &BTHREAD_ATTR_NORMAL,
                    PerformanceTest::RunTest, tests[i]);
        }
        if (end < thread_num) {
            bthread_usleep(10000);
        }
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
    
    CleanupGlobalChannels();
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

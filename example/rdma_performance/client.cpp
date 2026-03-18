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
#include <inttypes.h>
#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/fast_rand.h"
#include "butil/logging.h"
#include "butil/string_splitter.h"
#include "brpc/rdma/rdma_helper.h"
#include "brpc/server.h"
#include "brpc/channel.h"
#include "bthread/bthread.h"
#include "bvar/latency_recorder.h"
#include "bvar/variable.h"
#include "test.pb.h"

DEFINE_bool(enable_schedule_tracing, false, "Enable collection of schedule latency traces from server");
DEFINE_int32(max_trace_count, 1000, "Maximum number of traces to collect");

#ifdef BRPC_WITH_RDMA

DEFINE_int32(thread_num, 0, "How many threads are used");
DEFINE_int32(queue_depth, 1, "How many requests can be pending in the queue");
DEFINE_int32(expected_qps, 0, "The expected QPS");
DEFINE_int32(max_thread_num, 16, "The max number of threads are used");
DEFINE_int32(attachment_size, -1, "Attachment size is used (in Bytes)");
DEFINE_bool(echo_attachment, false, "Select whether attachment should be echo");
DEFINE_string(connection_type, "single", "Connection type of the channel");
DEFINE_string(protocol, "baidu_std", "Protocol type.");
DEFINE_string(servers, "0.0.0.0:8002+0.0.0.0:8002", "IP Address of servers");
DEFINE_bool(use_rdma, true, "Use RDMA or not");
DEFINE_int32(rpc_timeout_ms, 2000, "RPC call timeout");
DEFINE_int32(test_seconds, 20, "Test running time");
DEFINE_int32(test_iterations, 0, "Test iterations");
DEFINE_int32(dummy_port, 8001, "Dummy server port number");
DEFINE_int32(complexity, 0, "Complexity of the request");
DEFINE_int32(matrix_size, 0, "Size of the matrix");

bvar::LatencyRecorder g_latency_recorder("client");
bvar::LatencyRecorder g_server_cpu_recorder("server_cpu");
bvar::LatencyRecorder g_client_cpu_recorder("client_cpu");
butil::atomic<uint64_t> g_last_time(0);
butil::atomic<uint64_t> g_total_bytes;
butil::atomic<uint64_t> g_total_cnt;
std::vector<std::string> g_servers;
int rr_index = 0;
volatile bool g_stop = false;

// For collecting schedule latency trace info from server
struct ScheduleTraceInfo {
    uint64_t msg_received_ns{0};
    uint64_t queue_msg_start_ns{0};
    uint64_t queue_msg_end_ns{0};
    uint64_t bthread_queued_ns{0};
    uint64_t bthread_signaled_ns{0};
    uint64_t bthread_stolen_ns{0};
    uint64_t bthread_scheduled_ns{0};
    uint64_t bthread_running_ns{0};
    uint64_t process_input_ns{0};
    uint64_t process_rpc_ns{0};
};
butil::Mutex g_trace_mutex;
std::vector<ScheduleTraceInfo> g_schedule_traces;
butil::atomic<bool> g_tracing_full{false};

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

class PerformanceTest {
public:
    PerformanceTest(int attachment_size, bool echo_attachment)
        : _addr(NULL)
        , _channel(NULL)
        , _start_time(0)
        , _iterations(0)
        , _stop(false)
    {
        if (attachment_size > 0) {
            _addr = malloc(attachment_size);
            butil::fast_rand_bytes(_addr, attachment_size);
            _attachment.append(_addr, attachment_size);
        }
        _echo_attachment = echo_attachment;
    }

    ~PerformanceTest() {
        if (_addr) {
            free(_addr);
        }
        delete _channel;
    }

    inline bool IsStop() { return _stop; }

    int Init() {
        brpc::ChannelOptions options;
        options.use_rdma = FLAGS_use_rdma;
        options.protocol = FLAGS_protocol;
        options.connection_type = FLAGS_connection_type;
        options.timeout_ms = FLAGS_rpc_timeout_ms;
        options.max_retry = 0;
        std::string server = g_servers[(rr_index++) % g_servers.size()];
        _channel = new brpc::Channel();
        if (_channel->Init(server.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to initialize channel";
            return -1;
        }
        brpc::Controller cntl;
        test::PerfTestResponse response;
        test::PerfTestRequest request;
        request.set_echo_attachment(_echo_attachment);
        test::PerfTestService_Stub stub(_channel);
        stub.Test(&cntl, &request, &response, NULL);
        if (cntl.Failed()) {
            LOG(ERROR) << "RPC call failed: " << cntl.ErrorText();
            return -1;
        }
        return 0;
    }

    struct RespClosure {
        brpc::Controller* cntl;
        test::PerfTestResponse* resp;
        PerformanceTest* test;
    };

    void SendRequest() {
        if (FLAGS_expected_qps > 0) {
            while (g_token.load(butil::memory_order_relaxed) <= 0) {
                bthread_usleep(10);
            }
            g_token.fetch_sub(1, butil::memory_order_relaxed);
        }
        RespClosure* closure = new RespClosure;
        test::PerfTestRequest request;
        closure->resp = new test::PerfTestResponse();
        closure->cntl = new brpc::Controller();
        request.set_echo_attachment(_echo_attachment);
        if (FLAGS_matrix_size > 0) {
            for (int i = 0; i < FLAGS_matrix_size; ++i) {
                request.add_matrix(i);
            }
            request.set_matrix_dimension(FLAGS_matrix_size);
        }
        if (FLAGS_complexity > 0) {
            request.set_processing_complexity(FLAGS_complexity);
        }
        closure->cntl->request_attachment().append(_attachment);
        closure->test = this;
        google::protobuf::Closure* done = brpc::NewCallback(&HandleResponse, closure);
        test::PerfTestService_Stub stub(_channel);
        stub.Test(closure->cntl, &request, closure->resp, done);
    }

    static void HandleResponse(RespClosure* closure) {
        std::unique_ptr<brpc::Controller> cntl_guard(closure->cntl);
        std::unique_ptr<test::PerfTestResponse> response_guard(closure->resp);
        if (closure->cntl->Failed()) {
            LOG(ERROR) << "RPC call failed: " << closure->cntl->ErrorText();
            closure->test->_stop = true;
            return;
        }

        g_latency_recorder << closure->cntl->latency_us();
        if (closure->resp->cpu_usage().size() > 0) {
            g_server_cpu_recorder << atof(closure->resp->cpu_usage().c_str()) * 100;
        }
        g_total_bytes.fetch_add(closure->cntl->request_attachment().size(), butil::memory_order_relaxed);
        g_total_cnt.fetch_add(1, butil::memory_order_relaxed);

        // Collect schedule latency trace from response attachment
        if (FLAGS_enable_schedule_tracing && !g_tracing_full.load(butil::memory_order_relaxed) &&
            !cntl_guard->response_attachment().empty()) {

            butil::IOBufAsZeroCopyInputStream wrapper(cntl_guard->response_attachment());
            const void* data = NULL;
            int size = 0;

            if (wrapper.Next(&data, &size) && size >= 6) { // At least "TRACE:" prefix
                const char* buf = static_cast<const char*>(data);
                if (memcmp(buf, "TRACE:", 6) == 0) { // Verify trace magic
                    const char* trace_buf = buf + 6;
                    int trace_len = size - 6;

                    // Check if we have space before doing any parsing
                    BAIDU_SCOPED_LOCK(g_trace_mutex);
                    if (g_schedule_traces.size() >= (size_t)FLAGS_max_trace_count) {
                        g_tracing_full.store(true, butil::memory_order_relaxed);
                        return;
                    }

                    ScheduleTraceInfo trace;
                    // Use KeyValuePairsSplitter to parse key=value|key=value format
                    butil::KeyValuePairsSplitter splitter(trace_buf, trace_len, '|', '=');
                    while (splitter.next()) {
                        butil::StringPiece key = splitter.key();
                        butil::StringPiece value = splitter.value();
                        uint64_t val = 0;
                        if (butil::StringToUInt64(value, &val)) {
                            if (key == "msg_received_ns") trace.msg_received_ns = val;
                            else if (key == "queue_msg_start_ns") trace.queue_msg_start_ns = val;
                            else if (key == "queue_msg_end_ns") trace.queue_msg_end_ns = val;
                            else if (key == "bthread_queued_ns") trace.bthread_queued_ns = val;
                            else if (key == "bthread_signaled_ns") trace.bthread_signaled_ns = val;
                            else if (key == "bthread_stolen_ns") trace.bthread_stolen_ns = val;
                            else if (key == "bthread_scheduled_ns") trace.bthread_scheduled_ns = val;
                            else if (key == "bthread_running_ns") trace.bthread_running_ns = val;
                            else if (key == "process_input_ns") trace.process_input_ns = val;
                            else if (key == "process_rpc_ns") trace.process_rpc_ns = val;
                        }
                    }

                    g_schedule_traces.push_back(trace);
                }
            }
        }

        cntl_guard.reset(NULL);
        response_guard.reset(NULL);

        if (closure->test->_iterations == 0 && FLAGS_test_iterations > 0) {
            closure->test->_stop = true;
            return;
        }
        --closure->test->_iterations;
        uint64_t last = g_last_time.load(butil::memory_order_relaxed);
        uint64_t now = butil::gettimeofday_us();
        if (now > last && now - last > 100000) {
            if (g_last_time.exchange(now, butil::memory_order_relaxed) == last) {
                g_client_cpu_recorder <<
                    atof(bvar::Variable::describe_exposed("process_cpu_usage").c_str()) * 100;
            }
        }
        if (now - closure->test->_start_time > FLAGS_test_seconds * 1000000u) {
            closure->test->_stop = true;
            return;
        }
        closure->test->SendRequest();
    }

    static void* RunTest(void* arg) {
        PerformanceTest* test = (PerformanceTest*)arg;
        test->_start_time = butil::gettimeofday_us();
        test->_iterations = FLAGS_test_iterations;
        
        for (int i = 0; i < FLAGS_queue_depth; ++i) {
            test->SendRequest();
        }

        return NULL;
    }

private:
    void* _addr;
    brpc::Channel* _channel;
    uint64_t _start_time;
    uint32_t _iterations;
    volatile bool _stop;
    butil::IOBuf _attachment;
    bool _echo_attachment;
};

static void* DeleteTest(void* arg) {
    PerformanceTest* test = (PerformanceTest*)arg;
    delete test;
    return NULL;
}

void Test(int thread_num, int attachment_size) {
    std::cout << "[Threads: " << thread_num
        << ", Depth: " << FLAGS_queue_depth
        << ", Attachment: " << attachment_size << "B"
        << ", RDMA: " << (FLAGS_use_rdma ? "yes" : "no")
        << ", Echo: " << (FLAGS_echo_attachment ? "yes]" : "no]")
        << std::endl;
    g_total_bytes.store(0, butil::memory_order_relaxed);
    g_total_cnt.store(0, butil::memory_order_relaxed);
    BAIDU_SCOPED_LOCK(g_trace_mutex);
    g_schedule_traces.clear();
    g_schedule_traces.reserve(FLAGS_max_trace_count); // Pre-allocate space
    g_tracing_full.store(false, butil::memory_order_relaxed);
    std::vector<PerformanceTest*> tests;
    for (int k = 0; k < thread_num; ++k) {
        PerformanceTest* t = new PerformanceTest(attachment_size, FLAGS_echo_attachment);
        if (t->Init() < 0) {
            exit(1);
        }
        tests.push_back(t);
    }
    uint64_t start_time = butil::gettimeofday_us();
    bthread_t tid[thread_num];
    if (FLAGS_expected_qps > 0) {
        bthread_t tid;
        bthread_start_background(&tid, &BTHREAD_ATTR_NORMAL, GenerateToken, NULL);
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
    double throughput = g_total_bytes / 1.048576 / (end_time - start_time);
    if (FLAGS_test_iterations == 0) {
        std::cout << "Avg-Latency: " << g_latency_recorder.latency(10)
            << ", 90th-Latency: " << g_latency_recorder.latency_percentile(0.9)
            << ", 99th-Latency: " << g_latency_recorder.latency_percentile(0.99)
            << ", 99.9th-Latency: " << g_latency_recorder.latency_percentile(0.999)
            << ", Throughput: " << throughput << "MB/s"
            << ", QPS: " << (g_total_cnt.load(butil::memory_order_relaxed) * 1000 / (end_time - start_time)) << "k"
            << ", Server CPU-utilization: " << g_server_cpu_recorder.latency(10) << "\%"
            << ", Client CPU-utilization: " << g_client_cpu_recorder.latency(10) << "\%"
            << std::endl;
    } else {
        std::cout << " Throughput: " << throughput << "MB/s" << std::endl;
    }
    g_stop = true;
    for (int k = 0; k < thread_num; ++k) {
        bthread_start_background(&tid[k], &BTHREAD_ATTR_NORMAL, DeleteTest, tests[k]);
    }

    // Print all collected schedule latency traces
    BAIDU_SCOPED_LOCK(g_trace_mutex);
    if (!g_schedule_traces.empty()) {
        std::cout << "\n=== Collected Schedule Latency Traces ("
                  << g_schedule_traces.size() << " entries) ===" << std::endl;
        std::cout << "# | msg_received → queue_msg_start → queue_msg_end → bthread_queued "
                  << "→ bthread_signaled → bthread_stolen → bthread_scheduled "
                  << "→ bthread_running → process_input → process_rpc | total (us)" << std::endl;
        std::cout << "---" << std::endl;

        for (size_t i = 0; i < g_schedule_traces.size(); ++i) {
            const auto& trace = g_schedule_traces[i];

            // Validate timestamps to avoid underflow
            auto safe_diff = [](uint64_t a, uint64_t b) -> uint64_t {
                return (a > b) ? (a - b) : 0;
            };

            uint64_t t1 = safe_diff(trace.queue_msg_start_ns, trace.msg_received_ns) / 1000;
            uint64_t t2 = safe_diff(trace.queue_msg_end_ns, trace.queue_msg_start_ns) / 1000;
            uint64_t t3 = safe_diff(trace.bthread_queued_ns, trace.queue_msg_end_ns) / 1000;
            uint64_t t4 = safe_diff(trace.bthread_signaled_ns, trace.bthread_queued_ns) / 1000;
            uint64_t t5 = trace.bthread_stolen_ns ? safe_diff(trace.bthread_stolen_ns, trace.bthread_signaled_ns) / 1000 : 0;
            uint64_t t6 = safe_diff(trace.bthread_scheduled_ns, trace.bthread_stolen_ns ? trace.bthread_stolen_ns : trace.bthread_signaled_ns) / 1000;
            uint64_t t7 = safe_diff(trace.bthread_running_ns, trace.bthread_scheduled_ns) / 1000;
            uint64_t t8 = safe_diff(trace.process_input_ns, trace.bthread_running_ns) / 1000;
            uint64_t t9 = safe_diff(trace.process_rpc_ns, trace.process_input_ns) / 1000;
            uint64_t total_ns = safe_diff(trace.process_rpc_ns, trace.msg_received_ns);

            printf("%zu | %6" PRIu64 " → %6" PRIu64 " → %6" PRIu64 " → %6" PRIu64 " → %6" PRIu64 " → %6" PRIu64 " → %6" PRIu64 " → %6" PRIu64 " → %6" PRIu64 " → %6" PRIu64 " | %6.2f\n",
                   i + 1, t1, t2, t3, t4, t5, t6, t7, t8, t9, total_ns / 1000.0);
        }

        // Print summary statistics
        if (g_schedule_traces.size() > 1) {
            std::cout << "\n=== Trace Summary Statistics ===" << std::endl;
            uint64_t min_total = ~0ULL, max_total = 0, sum_total = 0;
            for (const auto& trace : g_schedule_traces) {
                uint64_t total = trace.process_rpc_ns - trace.msg_received_ns;
                min_total = std::min(min_total, total);
                max_total = std::max(max_total, total);
                sum_total += total;
            }
            double avg_total = sum_total * 1.0 / g_schedule_traces.size() / 1000;
            printf("Min: %.2fus | Max: %.2fus | Avg: %.2fus | Total Traces: %zu\n",
                   (double)min_total / 1000.0, (double)max_total / 1000.0, avg_total, g_schedule_traces.size());
        }
    }
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    // Initialize RDMA environment in advance.
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

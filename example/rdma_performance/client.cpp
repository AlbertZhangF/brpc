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
#include <csignal>
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

// Use DECLARE to reference flag defined in brpc library
DECLARE_bool(enable_schedule_tracing);
// Define client-specific flags
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
    uint64_t bthread_scheduled_ns{0};
    uint64_t bthread_running_ns{0};
    uint64_t process_input_ns{0};
    uint64_t process_rpc_ns{0};
};
butil::Mutex g_trace_mutex;
std::vector<ScheduleTraceInfo> g_schedule_traces;
butil::atomic<bool> g_tracing_full{false};

butil::atomic<int64_t> g_token(10000);

// Signal handler for printing traces on interrupt
#include <signal.h>
static volatile sig_atomic_t g_interrupted = 0;
static void PrintTracesOnInterrupt(int sig) {
    // Signal handler only performs async-safe operations, sets stop flag
    // Printing logic will be executed automatically in main flow
    g_interrupted = 1;
    g_stop = true;
}

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
        : _attachment_size(attachment_size)
        , _echo_attachment(echo_attachment) {
        if (_attachment_size > 0) {
            char* buf = new char[_attachment_size];
            memset(buf, 'x', _attachment_size);
            _attachment.append(buf, _attachment_size);
            delete[] buf;
        }
    }

    ~PerformanceTest() {
        if (_channel) {
            delete _channel;
            _channel = NULL;
        }
    }

    int Init() {
        _channel = new brpc::Channel;
        brpc::ChannelOptions options;
        options.protocol = FLAGS_protocol;
        options.connection_type = FLAGS_connection_type;
        options.timeout_ms = FLAGS_rpc_timeout_ms;
        options.max_retry = 3;
        if (FLAGS_use_rdma) {
            options.use_rdma = true;
        }
        std::string server = g_servers[(rr_index++) % g_servers.size()];
        if (_channel->Init(server.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to initialize channel";
            return -1;
        }
        return 0;
    }

    void Start() {
        _start_time = butil::monotonic_time_ns();
        _iterations = 0;
        _stop = false;
    }

    void Stop() {
        _stop = true;
    }

    bool IsStopped() const {
        return _stop;
    }

    static void* RunTest(void* arg) {
        PerformanceTest* test = static_cast<PerformanceTest*>(arg);
        test->Run();
        return NULL;
    }

    void Run() {
        test::PerfTestService_Stub stub(_channel);
        uint64_t local_bytes = 0;
        uint64_t local_cnt = 0;

        while (!g_stop && !_stop) {
            if (FLAGS_expected_qps > 0) {
                while (g_token.load(butil::memory_order_relaxed) <= 0 && !g_stop) {
                    bthread_usleep(1000);
                }
                g_token.fetch_sub(1, butil::memory_order_relaxed);
            }

            test::PerfTestRequest req;
            test::PerfTestResponse res;
            brpc::Controller cntl;
            req.set_complexity(FLAGS_complexity);
            req.set_matrix_size(FLAGS_matrix_size);
            if (_attachment_size > 0) {
                cntl.request_attachment().append(_attachment);
            }

            stub.Test(&cntl, &req, &res, NULL);
            if (!cntl.Failed()) {
                local_cnt++;
                local_bytes += _attachment_size;
                if (_echo_attachment && cntl.response_attachment().length() != (size_t)_attachment_size) {
                    LOG(ERROR) << "Echo attachment size mismatch";
                }
                if (FLAGS_enable_schedule_tracing) {
                    // Parse schedule latency trace info from response attachment
                    const butil::IOBuf& resp_attachment = cntl.response_attachment();
                    if (!resp_attachment.empty()) {
                        // Convert entire IOBuf to string to handle multi-segment buffer
                        std::string attachment_str;
                        resp_attachment.copy_to(&attachment_str);

                        // Look for trace magic prefix
                        const char TRACE_MAGIC[] = "TRACE:";
                        const size_t TRACE_MAGIC_LEN = sizeof(TRACE_MAGIC) - 1;
                        if (attachment_str.size() >= TRACE_MAGIC_LEN &&
                            memcmp(attachment_str.data(), TRACE_MAGIC, TRACE_MAGIC_LEN) == 0) {
                            // Parse trace info
                            ScheduleTraceInfo info;
                            const char* trace_data = attachment_str.data() + TRACE_MAGIC_LEN;
                            size_t trace_size = attachment_str.size() - TRACE_MAGIC_LEN;
                            std::string trace_str(trace_data, trace_size);
                            // Parse key=value pairs separated by '|'
                            size_t pos = 0;
                            while (pos < trace_str.size()) {
                                size_t eq_pos = trace_str.find('=', pos);
                                if (eq_pos == std::string::npos) break;
                                size_t pipe_pos = trace_str.find('|', eq_pos);
                                if (pipe_pos == std::string::npos) pipe_pos = trace_str.size();
                                std::string key = trace_str.substr(pos, eq_pos - pos);
                                std::string value_str = trace_str.substr(eq_pos + 1, pipe_pos - eq_pos - 1);
                                uint64_t value = strtoull(value_str.c_str(), NULL, 10);
                                if (key == "msg_received_ns") info.msg_received_ns = value;
                                else if (key == "queue_msg_start_ns") info.queue_msg_start_ns = value;
                                else if (key == "queue_msg_end_ns") info.queue_msg_end_ns = value;
                                else if (key == "bthread_queued_ns") info.bthread_queued_ns = value;
                                else if (key == "bthread_signaled_ns") info.bthread_signaled_ns = value;
                                else if (key == "bthread_scheduled_ns") info.bthread_scheduled_ns = value;
                                else if (key == "bthread_running_ns") info.bthread_running_ns = value;
                                else if (key == "process_input_ns") info.process_input_ns = value;
                                else if (key == "process_rpc_ns") info.process_rpc_ns = value;
                                pos = pipe_pos + 1;
                            }
                            // Store trace info
                            if (!g_tracing_full.load(butil::memory_order_relaxed)) {
                                BAIDU_SCOPED_LOCK(g_trace_mutex);
                                if (g_schedule_traces.size() < (size_t)FLAGS_max_trace_count) {
                                    g_schedule_traces.push_back(info);
                                } else {
                                    g_tracing_full.store(true, butil::memory_order_relaxed);
                                }
                            }
                        }
                    }
                }
            } else {
                if (cntl.ErrorCode() != brpc::EREQUEST && cntl.ErrorCode() != brpc::ERESPONSE) {
                    LOG(WARNING) << "PerformanceTest failed: " << cntl.ErrorText();
                }
            }
        }
        g_total_bytes.fetch_add(local_bytes, butil::memory_order_relaxed);
        g_total_cnt.fetch_add(local_cnt, butil::memory_order_relaxed);
    }

private:
    void* _addr;
    brpc::Channel* _channel;
    uint64_t _start_time;
    uint32_t _iterations;
    volatile bool _stop;
    butil::IOBuf _attachment;
    bool _echo_attachment;
    int _attachment_size;
};


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
    // Global timeout check, ensure stop even if no response
    uint64_t test_end_time = start_time + FLAGS_test_seconds * 1000000u;
    bool all_stopped = false;
    while (!g_interrupted && !all_stopped && butil::gettimeofday_us() < test_end_time) {
        all_stopped = true;
        for (int k = 0; k < thread_num; ++k) {
            if (!tests[k]->IsStopped()) {
                all_stopped = false;
                break;
            }
        }
        if (!all_stopped) {
            sleep(1);
        }
    }
    for (int k = 0; k < thread_num; ++k) {
        tests[k]->Stop();
    }
    uint64_t end_time = butil::gettimeofday_us();
    uint64_t total_bytes = g_total_bytes.load(butil::memory_order_relaxed);
    uint64_t total_cnt = g_total_cnt.load(butil::memory_order_relaxed);
    double qps = (double)total_cnt / ((end_time - start_time) / 1000000.0);
    double mbps = (double)total_bytes / 1024 / 1024 / ((end_time - start_time) / 1000000.0);
    std::cout << "QPS: " << qps
        << ", MB/s: " << mbps
        << ", Latency: " << g_latency_recorder.latency(10) << "us"
        << ", Client CPU-utilization: " << g_client_cpu_recorder.latency(10) << "%"
        << ", Server CPU-utilization: " << g_server_cpu_recorder.latency(10) << "%"
        << std::endl;
    for (int k = 0; k < thread_num; ++k) {
        delete tests[k];
    }
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_use_rdma) {
        brpc::rdma::GlobalRdmaInitializeOrDie();
    }

    // Start dummy server for exposing built-in services
    brpc::StartDummyServerAt(FLAGS_dummy_port);

    // Parse server list
    std::string::size_type pos1 = 0;
    std::string::size_type pos2 = FLAGS_servers.find('+');
    while (pos2 != std::string::npos) {
        g_servers.push_back(FLAGS_servers.substr(pos1, pos2 - pos1));
        pos1 = pos2 + 1;
        pos2 = FLAGS_servers.find('+', pos1);
    }
    g_servers.push_back(FLAGS_servers.substr(pos1));

    // Install signal handler
    signal(SIGINT, PrintTracesOnInterrupt);
    signal(SIGTERM, PrintTracesOnInterrupt);

    if (FLAGS_thread_num > 0 && FLAGS_attachment_size >= 0) {
        Test(FLAGS_thread_num, FLAGS_attachment_size);
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

    // Wait for all background threads to exit
    bthread_usleep(500000);

    return 0;
}

#else

int main(int argc, char* argv[]) {
    LOG(ERROR) << " brpc is not compiled with rdma. To enable it, please refer to https://github.com/apache/brpc/blob/master/docs/en/rdma.md";
    return 0;
}

#endif
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

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gflags/gflags.h>

#include "butil/atomicops.h"
#include "butil/fast_rand.h"
#include "butil/logging.h"
#include "brpc/channel.h"
#include "brpc/rdma/rdma_helper.h"
#include "brpc/server.h"
#include "bthread/bthread.h"
#include "bvar/latency_recorder.h"
#include "bvar/variable.h"
#include "test.pb.h"

#ifdef BRPC_WITH_RDMA

DEFINE_int32(thread_num, 0, "How many worker threads are used");
DEFINE_int32(queue_depth, 1, "How many requests are pending per worker in closed_loop mode");
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
DEFINE_int32(test_iterations, 0, "Total request budget, 0 means time-based run");
DEFINE_int32(dummy_port, 8001, "Dummy server port number");
DEFINE_string(load_mode, "closed_loop", "Load mode of the client: closed_loop or open_loop");
DEFINE_int32(connection_num, 0, "How many Channel instances should be created");
DEFINE_int32(max_inflight, 0, "Global inflight limit in open_loop mode");
DEFINE_bool(unique_connection_group, false,
            "Create a unique connection_group per Channel to prevent SocketMap reuse");
DEFINE_bool(report_connection_stats, true, "Print connection-level counters");
DEFINE_string(payload_format, "attachment",
              "Payload format: attachment or raw_json");
DEFINE_string(json_file, "", "Read raw JSON request body from this file");
DEFINE_bool(json_echo_check, false,
            "Check raw JSON response body equals request body when echo_attachment is true");

bvar::LatencyRecorder g_latency_recorder("client");
bvar::LatencyRecorder g_server_cpu_recorder("server_cpu");
bvar::LatencyRecorder g_client_cpu_recorder("client_cpu");
butil::atomic<uint64_t> g_last_time(0);
butil::atomic<uint64_t> g_total_bytes;
butil::atomic<uint64_t> g_total_cnt;
butil::atomic<uint64_t> g_failed_cnt;
butil::atomic<uint64_t> g_timeout_cnt;
butil::atomic<int64_t> g_inflight(0);
butil::atomic<int64_t> g_peak_inflight(0);
butil::atomic<int64_t> g_request_budget(0);
butil::atomic<uint32_t> g_rr_index(0);
butil::atomic<int64_t> g_token(10000);
butil::atomic<int64_t> g_open_loop_inflight_limit(0);
std::vector<std::string> g_servers;
std::string g_json_payload;
volatile bool g_stop = false;

namespace {

const char* kClosedLoop = "closed_loop";
const char* kOpenLoop = "open_loop";
const char* kPayloadAttachment = "attachment";
const char* kPayloadRawJson = "raw_json";
const char* kRawJsonPath = "/test.PerfTestService/Test";

struct ConnectionSlot {
    explicit ConnectionSlot(int connection_index_in)
        : connection_index(connection_index_in)
        , channel(NULL)
        , sent(0)
        , completed(0)
        , failed(0)
        , timeouts(0)
    {}

    ~ConnectionSlot() {
        delete channel;
        channel = NULL;
    }

    int connection_index;
    std::string server;
    std::string connection_group;
    brpc::Channel* channel;
    butil::atomic<uint64_t> sent;
    butil::atomic<uint64_t> completed;
    butil::atomic<uint64_t> failed;
    butil::atomic<uint64_t> timeouts;
};

struct Worker;

struct RespClosure {
    brpc::Controller* cntl;
    test::PerfTestResponse* resp;
    Worker* worker;
    ConnectionSlot* slot;
};

struct Worker {
    Worker(int worker_index_in, int attachment_size, bool echo_attachment)
        : worker_index(worker_index_in)
        , addr(NULL)
        , start_time_us(0)
        , stop(false)
        , echo_attachment_flag(echo_attachment)
    {
        if (attachment_size > 0) {
            addr = malloc(attachment_size);
            butil::fast_rand_bytes(addr, attachment_size);
            attachment.append(addr, attachment_size);
        }
    }

    ~Worker() {
        if (addr) {
            free(addr);
            addr = NULL;
        }
    }

    void* addr;
    int worker_index;
    uint64_t start_time_us;
    volatile bool stop;
    butil::IOBuf attachment;
    bool echo_attachment_flag;
};

std::vector<ConnectionSlot*> g_connection_slots;

static bool IsOpenLoop() {
    return FLAGS_load_mode == kOpenLoop;
}

static bool IsClosedLoop() {
    return FLAGS_load_mode == kClosedLoop;
}

static bool IsRawJsonPayload() {
    return FLAGS_payload_format == kPayloadRawJson;
}

static bool IsAttachmentPayload() {
    return FLAGS_payload_format == kPayloadAttachment;
}

static std::string LowerString(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

static bool IsRawJsonProtocolAllowed() {
    const std::string protocol = LowerString(FLAGS_protocol);
    return protocol == "http" || protocol == "h2";
}

static bool ReadFileToString(const std::string& path, std::string* output) {
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        LOG(ERROR) << "Fail to open json_file=" << path;
        return false;
    }
    std::ostringstream oss;
    oss << input.rdbuf();
    *output = oss.str();
    return true;
}

static std::string GenerateJsonPayload(int target_size) {
    const std::string prefix = "{\"payload\":\"";
    const std::string suffix = "\"}";
    if (target_size <= (int)(prefix.size() + suffix.size())) {
        return prefix + suffix;
    }
    return prefix + std::string(target_size - prefix.size() - suffix.size(), 'x') + suffix;
}

static bool InitRawJsonPayload(int attachment_size) {
    if (!FLAGS_json_file.empty()) {
        return ReadFileToString(FLAGS_json_file, &g_json_payload);
    }
    g_json_payload = GenerateJsonPayload(std::max(attachment_size, 0));
    return true;
}

static std::string ExtractCpuUsageFromJson(const butil::IOBuf& body) {
    const std::string text = body.to_string();
    const std::string key = "\"cpu_usage\":\"";
    size_t pos = text.find(key);
    if (pos == std::string::npos) {
        return "";
    }
    pos += key.size();
    const size_t end = text.find('"', pos);
    if (end == std::string::npos) {
        return "";
    }
    return text.substr(pos, end - pos);
}

static uint64_t NowUs() {
    return butil::gettimeofday_us();
}

static bool ShouldStopByTime(uint64_t start_time_us) {
    return FLAGS_test_seconds > 0 &&
           NowUs() - start_time_us >= (uint64_t)FLAGS_test_seconds * 1000000UL;
}

static void UpdatePeakInflight(int64_t inflight) {
    int64_t peak = g_peak_inflight.load(butil::memory_order_relaxed);
    while (inflight > peak &&
           !g_peak_inflight.compare_exchange_weak(
                   peak, inflight, butil::memory_order_relaxed)) {
    }
}

static ConnectionSlot* PickConnectionSlot() {
    uint32_t index = g_rr_index.fetch_add(1, butil::memory_order_relaxed);
    return g_connection_slots[index % g_connection_slots.size()];
}

static bool ConsumeRequestBudget() {
    if (FLAGS_test_iterations <= 0) {
        return true;
    }
    int64_t old_value = g_request_budget.load(butil::memory_order_relaxed);
    while (old_value > 0) {
        if (g_request_budget.compare_exchange_weak(
                    old_value, old_value - 1, butil::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

static bool AcquireQpsToken() {
    if (FLAGS_expected_qps <= 0) {
        return true;
    }
    while (!g_stop) {
        int64_t token = g_token.load(butil::memory_order_relaxed);
        if (token > 0 &&
            g_token.compare_exchange_weak(
                    token, token - 1, butil::memory_order_relaxed)) {
            return true;
        }
        bthread_usleep(10);
    }
    return false;
}

static bool TryAcquireRequestPermit(uint64_t start_time_us) {
    if (g_stop || ShouldStopByTime(start_time_us)) {
        return false;
    }
    if (!ConsumeRequestBudget()) {
        return false;
    }
    if (!AcquireQpsToken()) {
        return false;
    }
    return true;
}

static void UpdateClientCpuSample() {
    uint64_t last = g_last_time.load(butil::memory_order_relaxed);
    uint64_t now = NowUs();
    if (now > last && now - last > 100000) {
        if (g_last_time.exchange(now, butil::memory_order_relaxed) == last) {
            g_client_cpu_recorder <<
                    atof(bvar::Variable::describe_exposed("process_cpu_usage").c_str()) * 100;
        }
    }
}

static void* GenerateToken(void* arg) {
    int64_t start_time = butil::monotonic_time_ns();
    int64_t accumulative_token = g_token.load(butil::memory_order_relaxed);
    while (!g_stop) {
        bthread_usleep(100000);
        int64_t now = butil::monotonic_time_ns();
        if (accumulative_token * 1000000000 / (now - start_time) < FLAGS_expected_qps) {
            int64_t delta =
                    FLAGS_expected_qps * (now - start_time) / 1000000000 - accumulative_token;
            g_token.fetch_add(delta, butil::memory_order_relaxed);
            accumulative_token += delta;
        }
    }
    return NULL;
}

static void SendRequest(Worker* worker);

static void HandleResponse(RespClosure* closure) {
    std::unique_ptr<brpc::Controller> cntl_guard(closure->cntl);
    std::unique_ptr<test::PerfTestResponse> response_guard(closure->resp);
    std::unique_ptr<RespClosure> closure_guard(closure);
    ConnectionSlot* slot = closure->slot;
    Worker* worker = closure->worker;

    g_inflight.fetch_sub(1, butil::memory_order_relaxed);

    if (closure->cntl->Failed()) {
        slot->failed.fetch_add(1, butil::memory_order_relaxed);
        g_failed_cnt.fetch_add(1, butil::memory_order_relaxed);
        if (closure->cntl->ErrorCode() == brpc::ERPCTIMEDOUT) {
            slot->timeouts.fetch_add(1, butil::memory_order_relaxed);
            g_timeout_cnt.fetch_add(1, butil::memory_order_relaxed);
        }
        LOG(ERROR) << "RPC call failed: " << closure->cntl->ErrorText();
        worker->stop = true;
        g_stop = true;
        return;
    }

    slot->completed.fetch_add(1, butil::memory_order_relaxed);
    g_latency_recorder << closure->cntl->latency_us();
    if (!IsRawJsonPayload() && !closure->resp->cpu_usage().empty()) {
        g_server_cpu_recorder << atof(closure->resp->cpu_usage().c_str()) * 100;
    } else if (IsRawJsonPayload()) {
        const std::string* cpu_header =
                closure->cntl->http_response().GetHeader("X-Server-Cpu-Usage");
        if (cpu_header != NULL && !cpu_header->empty()) {
            g_server_cpu_recorder << atof(cpu_header->c_str()) * 100;
        } else if (!worker->echo_attachment_flag) {
            const std::string cpu_usage = ExtractCpuUsageFromJson(
                    closure->cntl->response_attachment());
            if (!cpu_usage.empty()) {
                g_server_cpu_recorder << atof(cpu_usage.c_str()) * 100;
            }
        }
        if (FLAGS_json_echo_check && worker->echo_attachment_flag) {
            const std::string response_body =
                    closure->cntl->response_attachment().to_string();
            if (response_body != g_json_payload) {
                slot->failed.fetch_add(1, butil::memory_order_relaxed);
                g_failed_cnt.fetch_add(1, butil::memory_order_relaxed);
                LOG(ERROR) << "Raw JSON echo check failed";
                worker->stop = true;
                g_stop = true;
                return;
            }
        }
    }
    const size_t payload_bytes = IsRawJsonPayload() ?
            g_json_payload.size() : closure->cntl->request_attachment().size();
    g_total_bytes.fetch_add(payload_bytes, butil::memory_order_relaxed);
    g_total_cnt.fetch_add(1, butil::memory_order_relaxed);
    UpdateClientCpuSample();

    if (ShouldStopByTime(worker->start_time_us) ||
        (FLAGS_test_iterations > 0 &&
         g_request_budget.load(butil::memory_order_relaxed) <= 0 &&
         g_inflight.load(butil::memory_order_relaxed) <= 0)) {
        worker->stop = true;
        return;
    }

    if (IsClosedLoop() && !g_stop && !worker->stop) {
        SendRequest(worker);
    }
}

static void SendRequest(Worker* worker) {
    if (!TryAcquireRequestPermit(worker->start_time_us)) {
        worker->stop = true;
        return;
    }

    ConnectionSlot* slot = PickConnectionSlot();
    RespClosure* closure = new RespClosure;
    closure->worker = worker;
    closure->slot = slot;
    closure->cntl = new brpc::Controller();
    closure->resp = IsRawJsonPayload() ? NULL : new test::PerfTestResponse();
    if (IsRawJsonPayload()) {
        closure->cntl->http_request().uri() = std::string(kRawJsonPath) +
                "?echo_attachment=" + (worker->echo_attachment_flag ? "true" : "false");
        closure->cntl->http_request().set_method(brpc::HTTP_METHOD_POST);
        closure->cntl->http_request().set_content_type("application/json");
        closure->cntl->request_attachment().append(g_json_payload);
    }
    google::protobuf::Closure* done = brpc::NewCallback(&HandleResponse, closure);
    slot->sent.fetch_add(1, butil::memory_order_relaxed);
    int64_t inflight = g_inflight.fetch_add(1, butil::memory_order_relaxed) + 1;
    UpdatePeakInflight(inflight);
    if (IsRawJsonPayload()) {
        slot->channel->CallMethod(NULL, closure->cntl, NULL, NULL, done);
    } else {
        test::PerfTestRequest request;
        request.set_echo_attachment(worker->echo_attachment_flag);
        closure->cntl->request_attachment().append(worker->attachment);
        test::PerfTestService_Stub stub(slot->channel);
        stub.Test(closure->cntl, &request, closure->resp, done);
    }
}

static void* RunClosedLoopWorker(void* arg) {
    Worker* worker = static_cast<Worker*>(arg);
    worker->start_time_us = NowUs();
    for (int i = 0; i < FLAGS_queue_depth; ++i) {
        if (g_stop || worker->stop) {
            break;
        }
        SendRequest(worker);
    }
    return NULL;
}

static void* RunOpenLoopWorker(void* arg) {
    Worker* worker = static_cast<Worker*>(arg);
    worker->start_time_us = NowUs();
    const int64_t inflight_limit =
            g_open_loop_inflight_limit.load(butil::memory_order_relaxed);
    while (!g_stop && !worker->stop) {
        if (ShouldStopByTime(worker->start_time_us)) {
            worker->stop = true;
            break;
        }
        if (FLAGS_test_iterations > 0 &&
            g_request_budget.load(butil::memory_order_relaxed) <= 0) {
            worker->stop = true;
            break;
        }
        if (g_inflight.load(butil::memory_order_relaxed) >= inflight_limit) {
            bthread_usleep(50);
            continue;
        }
        SendRequest(worker);
    }
    return NULL;
}

static int InitConnectionSlots(int connection_num, bool echo_attachment) {
    for (size_t i = 0; i < g_connection_slots.size(); ++i) {
        delete g_connection_slots[i];
    }
    g_connection_slots.clear();
    g_connection_slots.reserve(connection_num);

    for (int i = 0; i < connection_num; ++i) {
        ConnectionSlot* slot = new ConnectionSlot(i);
        slot->server = g_servers[i % g_servers.size()];
        if (FLAGS_unique_connection_group) {
            slot->connection_group = "rdma_perf_tcp_conn_" + std::to_string(i);
        }
        brpc::ChannelOptions options;
        options.use_rdma = FLAGS_use_rdma;
        options.protocol = FLAGS_protocol;
        options.connection_type = FLAGS_connection_type;
        options.timeout_ms = FLAGS_rpc_timeout_ms;
        options.max_retry = 0;
        options.connection_group = slot->connection_group;
        slot->channel = new brpc::Channel();
        if (slot->channel->Init(slot->server.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to initialize channel index=" << i;
            delete slot;
            return -1;
        }

        brpc::Controller cntl;
        if (IsRawJsonPayload()) {
            cntl.http_request().uri() = std::string(kRawJsonPath) +
                    "?echo_attachment=" + (echo_attachment ? "true" : "false");
            cntl.http_request().set_method(brpc::HTTP_METHOD_POST);
            cntl.http_request().set_content_type("application/json");
            cntl.request_attachment().append(g_json_payload);
            slot->channel->CallMethod(NULL, &cntl, NULL, NULL, NULL);
        } else {
            test::PerfTestResponse response;
            test::PerfTestRequest request;
            request.set_echo_attachment(echo_attachment);
            test::PerfTestService_Stub stub(slot->channel);
            stub.Test(&cntl, &request, &response, NULL);
        }
        if (cntl.Failed()) {
            LOG(ERROR) << "Warmup RPC failed on connection " << i << ": "
                       << cntl.ErrorText();
            delete slot;
            return -1;
        }
        g_connection_slots.push_back(slot);
    }
    return 0;
}

static void DestroyConnectionSlots() {
    for (size_t i = 0; i < g_connection_slots.size(); ++i) {
        delete g_connection_slots[i];
    }
    g_connection_slots.clear();
}

static void PrintConnectionStats() {
    if (!FLAGS_report_connection_stats) {
        return;
    }
    std::cout << "Connection stats:" << std::endl;
    for (size_t i = 0; i < g_connection_slots.size(); ++i) {
        ConnectionSlot* slot = g_connection_slots[i];
        std::cout << "  [conn=" << slot->connection_index
                  << ", server=" << slot->server
                  << ", group=" << (slot->connection_group.empty() ? "<shared>" :
                                    slot->connection_group)
                  << "] sent=" << slot->sent.load(butil::memory_order_relaxed)
                  << ", completed=" << slot->completed.load(butil::memory_order_relaxed)
                  << ", failed=" << slot->failed.load(butil::memory_order_relaxed)
                  << ", timeouts=" << slot->timeouts.load(butil::memory_order_relaxed)
                  << std::endl;
    }
}

static void Test(int thread_num, int attachment_size) {
    const int actual_connection_num =
            FLAGS_connection_num > 0 ? FLAGS_connection_num : thread_num;
    const int64_t open_loop_inflight_limit =
            FLAGS_max_inflight > 0 ? FLAGS_max_inflight :
            (int64_t)thread_num * FLAGS_queue_depth;
    std::cout << "[Threads: " << thread_num
              << ", Depth: " << FLAGS_queue_depth
              << ", Attachment: " << attachment_size << "B"
              << ", RDMA: " << (FLAGS_use_rdma ? "yes" : "no")
              << ", Echo: " << (FLAGS_echo_attachment ? "yes" : "no")
              << ", PayloadFormat: " << FLAGS_payload_format
              << ", LoadMode: " << FLAGS_load_mode
              << ", ConnectionType: " << FLAGS_connection_type
              << ", ConnectionNum: " << actual_connection_num
              << ", UniqueConnectionGroup: "
              << (FLAGS_unique_connection_group ? "true" : "false")
              << ", ModelConcurrency: "
              << (IsClosedLoop() ? (int64_t)thread_num * FLAGS_queue_depth :
                                   open_loop_inflight_limit)
              << "]" << std::endl;
    if (IsClosedLoop()) {
        std::cout << "Closed-loop keeps roughly thread_num * queue_depth requests in flight."
                  << std::endl;
    } else {
        std::cout << "Open-loop keeps sending until max_inflight is hit, decoupled from worker count."
                  << std::endl;
    }

    if (IsRawJsonPayload() && !InitRawJsonPayload(attachment_size)) {
        exit(1);
    }

    g_stop = false;
    g_last_time.store(0, butil::memory_order_relaxed);
    g_total_bytes.store(0, butil::memory_order_relaxed);
    g_total_cnt.store(0, butil::memory_order_relaxed);
    g_failed_cnt.store(0, butil::memory_order_relaxed);
    g_timeout_cnt.store(0, butil::memory_order_relaxed);
    g_inflight.store(0, butil::memory_order_relaxed);
    g_peak_inflight.store(0, butil::memory_order_relaxed);
    g_request_budget.store(FLAGS_test_iterations, butil::memory_order_relaxed);
    g_rr_index.store(0, butil::memory_order_relaxed);
    g_open_loop_inflight_limit.store(
            open_loop_inflight_limit, butil::memory_order_relaxed);

    if (InitConnectionSlots(actual_connection_num, FLAGS_echo_attachment) < 0) {
        DestroyConnectionSlots();
        exit(1);
    }

    std::vector<Worker*> workers;
    workers.reserve(thread_num);
    const int worker_attachment_size = IsRawJsonPayload() ? 0 : attachment_size;
    for (int k = 0; k < thread_num; ++k) {
        workers.push_back(new Worker(k, worker_attachment_size, FLAGS_echo_attachment));
    }

    uint64_t start_time = NowUs();
    bthread_t tids[thread_num];
    if (FLAGS_expected_qps > 0) {
        bthread_t tid;
        bthread_start_background(&tid, &BTHREAD_ATTR_NORMAL, GenerateToken, NULL);
    }
    for (int k = 0; k < thread_num; ++k) {
        void* (*fn)(void*) = IsClosedLoop() ? RunClosedLoopWorker : RunOpenLoopWorker;
        bthread_start_background(&tids[k], &BTHREAD_ATTR_NORMAL, fn, workers[k]);
    }

    while (true) {
        bool all_workers_stopped = true;
        for (int k = 0; k < thread_num; ++k) {
            if (!workers[k]->stop) {
                all_workers_stopped = false;
                break;
            }
        }
        bool no_more_inflight = g_inflight.load(butil::memory_order_relaxed) == 0;
        if ((all_workers_stopped && no_more_inflight) ||
            (FLAGS_test_seconds > 0 && NowUs() - start_time >=
                    (uint64_t)(FLAGS_test_seconds + 1) * 1000000UL &&
             no_more_inflight)) {
            break;
        }
        if (FLAGS_test_seconds > 0 && ShouldStopByTime(start_time)) {
            g_stop = true;
        }
        bthread_usleep(10000);
    }

    uint64_t end_time = NowUs();
    double throughput = 0;
    if (end_time > start_time) {
        throughput = g_total_bytes.load(butil::memory_order_relaxed) /
                     1.048576 / (end_time - start_time);
    }
    if (FLAGS_test_iterations == 0) {
        std::cout << "Avg-Latency: " << g_latency_recorder.latency(10)
                  << ", 90th-Latency: " << g_latency_recorder.latency_percentile(0.9)
                  << ", 99th-Latency: " << g_latency_recorder.latency_percentile(0.99)
                  << ", 99.9th-Latency: " << g_latency_recorder.latency_percentile(0.999)
                  << ", Throughput: " << throughput << "MB/s"
                  << ", QPS: "
                  << (g_total_cnt.load(butil::memory_order_relaxed) * 1000 /
                      (end_time - start_time))
                  << "k"
                  << ", Failed: " << g_failed_cnt.load(butil::memory_order_relaxed)
                  << ", Timeout: " << g_timeout_cnt.load(butil::memory_order_relaxed)
                  << ", PeakInflight: " << g_peak_inflight.load(butil::memory_order_relaxed)
                  << ", Server CPU-utilization: " << g_server_cpu_recorder.latency(10) << "%"
                  << ", Client CPU-utilization: " << g_client_cpu_recorder.latency(10) << "%"
                  << std::endl;
    } else {
        std::cout << "Throughput: " << throughput << "MB/s"
                  << ", Completed: " << g_total_cnt.load(butil::memory_order_relaxed)
                  << ", Failed: " << g_failed_cnt.load(butil::memory_order_relaxed)
                  << ", PeakInflight: " << g_peak_inflight.load(butil::memory_order_relaxed)
                  << std::endl;
    }

    PrintConnectionStats();

    g_stop = true;
    for (int k = 0; k < thread_num; ++k) {
        delete workers[k];
    }
    DestroyConnectionSlots();
}

}  // namespace

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);

    if (!IsClosedLoop() && !IsOpenLoop()) {
        LOG(ERROR) << "Invalid load_mode=" << FLAGS_load_mode
                   << ", valid values are closed_loop and open_loop";
        return -1;
    }
    if (!IsAttachmentPayload() && !IsRawJsonPayload()) {
        LOG(ERROR) << "Invalid payload_format=" << FLAGS_payload_format
                   << ", valid values are attachment and raw_json";
        return -1;
    }
    if (IsRawJsonPayload() && !IsRawJsonProtocolAllowed()) {
        LOG(ERROR) << "payload_format=raw_json requires --protocol=http or --protocol=h2, "
                   << "but protocol=" << FLAGS_protocol;
        return -1;
    }

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

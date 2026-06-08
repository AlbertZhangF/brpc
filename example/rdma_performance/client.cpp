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

#include <errno.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_set>
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

DEFINE_int32(thread_num, 0, "How many worker threads are used");
DEFINE_int32(queue_depth, 1, "How many requests are pending per worker in closed_loop mode");
DEFINE_int32(expected_qps, 0, "The expected QPS");
DEFINE_int32(max_thread_num, 16, "The max number of threads are used");
DEFINE_int32(attachment_size, -1, "Attachment size is used (in Bytes)");
DEFINE_bool(echo_attachment, false, "Select whether attachment should be echo");
DEFINE_string(connection_type, "single", "Connection type of the channel");
DEFINE_string(protocol, "baidu_std", "Protocol type.");
DEFINE_string(servers, "0.0.0.0:8002+0.0.0.0:8002", "IP Address of servers");
DEFINE_string(transport, "rdma",
              "Transport mode: tcp, rdma, or iouring. When explicitly set, "
              "this overrides --use_rdma.");
DEFINE_bool(use_rdma, true,
            "Compatibility flag. Used only when --transport is not explicitly "
            "set: true means rdma, false means tcp.");
DEFINE_int32(rpc_timeout_ms, 2000, "RPC call timeout");
DEFINE_int32(connect_timeout_ms, -1,
             "Connection establishment timeout. -1 means use rpc_timeout_ms");
DEFINE_int32(test_seconds, 20, "Test running time");
DEFINE_int32(stop_grace_ms, 5000,
             "Max time to wait for in-flight RPCs after test_seconds is reached");
DEFINE_bool(cancel_inflight_on_stop, false,
            "Cancel outstanding asynchronous RPCs when the timed test stops. "
            "Disabled by default to avoid per-RPC tracking overhead");
DEFINE_bool(log_rpc_error, false,
            "Print each failed RPC. Disabled by default to avoid log flooding under overload");
DEFINE_int32(test_iterations, 0, "Total request budget, 0 means time-based run");
DEFINE_int32(dummy_port, 8001, "Dummy server port number");
DEFINE_string(load_mode, "closed_loop", "Load mode of the client: closed_loop or open_loop");
DEFINE_int32(connection_num, 0, "How many Channel instances should be created");
DEFINE_int32(max_inflight, 0, "Global inflight limit in open_loop mode");
DEFINE_bool(unique_connection_group, false,
            "Create a unique connection_group per Channel to prevent SocketMap reuse");
DEFINE_bool(report_connection_stats, false, "Print connection-level counters");
DEFINE_string(payload_format, "attachment",
              "Payload format: attachment or raw_json");
DEFINE_string(json_file, "", "Read raw JSON request body from this file");
DEFINE_bool(json_echo_check, false,
            "Check raw JSON response body equals request body when echo_attachment is true");
DEFINE_string(response_mode, "normal",
              "Response handling mode for protobuf payload: normal or minimal");
DEFINE_bool(record_latency, true,
            "Record latency percentiles. Disable for max-throughput tests to reduce client-side stats overhead");

std::unique_ptr<bvar::LatencyRecorder> g_latency_recorder;
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
std::mutex g_call_ids_mutex;
std::unordered_set<uint64_t> g_call_ids;
bool g_transport_explicit = false;

namespace {

const char* kClosedLoop = "closed_loop";
const char* kOpenLoop = "open_loop";
const char* kPayloadAttachment = "attachment";
const char* kPayloadRawJson = "raw_json";
const char* kResponseModeNormal = "normal";
const char* kResponseModeMinimal = "minimal";
const char* kRawJsonPath = "/test.PerfTestService/Test";
brpc::SocketMode g_socket_mode = brpc::SOCKET_MODE_TCP;
std::string g_transport_name = "tcp";

bool IsFlagExplicitlySet(const char* flag_name) {
    GFLAGS_NAMESPACE::CommandLineFlagInfo info;
    return GFLAGS_NAMESPACE::GetCommandLineFlagInfo(flag_name, &info) &&
           !info.is_default;
}

struct TransportChoice {
    brpc::SocketMode socket_mode;
    const char* name;
};

bool ResolveTransportMode(TransportChoice* choice) {
    std::string transport = FLAGS_transport;
    std::transform(transport.begin(), transport.end(), transport.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (!g_transport_explicit) {
        transport = FLAGS_use_rdma ? "rdma" : "tcp";
    }

    if (transport == "tcp") {
        choice->socket_mode = brpc::SOCKET_MODE_TCP;
        choice->name = "tcp";
        return true;
    }
    if (transport == "rdma") {
#if BRPC_WITH_RDMA
        choice->socket_mode = brpc::SOCKET_MODE_RDMA;
        choice->name = "rdma";
        return true;
#else
        LOG(ERROR) << "transport=rdma requires BRPC_WITH_RDMA=1. "
                   << "Rebuild brpc/example with RDMA support or use "
                   << "--transport=tcp/--transport=iouring.";
        return false;
#endif
    }
    if (transport == "iouring") {
        // Match example/iouring_echo_c++: io_uring is enabled on the server
        // side, while the benchmark client keeps using a normal TCP Channel.
        // This avoids exercising the experimental client-side io_uring write
        // path from the benchmark.
        choice->socket_mode = brpc::SOCKET_MODE_TCP;
        choice->name = "iouring";
        return true;
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
    return true;
}

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
    brpc::CallId call_id;
    bool track_call_id;
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

static bool IsMinimalResponseMode() {
    return FLAGS_response_mode == kResponseModeMinimal;
}

static bool IsNormalResponseMode() {
    return FLAGS_response_mode == kResponseModeNormal;
}

static int LatencyWindowSeconds() {
    return FLAGS_test_seconds > 0 ? FLAGS_test_seconds : 10;
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

static void ReleaseInflightPermit() {
    g_inflight.fetch_sub(1, butil::memory_order_relaxed);
}

static void RegisterCallId(brpc::CallId id) {
    std::lock_guard<std::mutex> lock(g_call_ids_mutex);
    g_call_ids.insert(id.value);
}

static void UnregisterCallId(brpc::CallId id) {
    std::lock_guard<std::mutex> lock(g_call_ids_mutex);
    g_call_ids.erase(id.value);
}

static void CancelOutstandingRpc() {
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lock(g_call_ids_mutex);
        ids.assign(g_call_ids.begin(), g_call_ids.end());
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        brpc::CallId id = { ids[i] };
        brpc::StartCancel(id);
    }
    if (!ids.empty()) {
        LOG(WARNING) << "Canceled " << ids.size() << " outstanding RPCs";
    }
}

static std::string FormatQps(uint64_t completed_count, uint64_t elapsed_us) {
    if (elapsed_us == 0) {
        return "0.000";
    }
    const double qps = completed_count * 1000000.0 / elapsed_us;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    if (qps >= 1000.0) {
        oss << qps / 1000.0 << "k";
    } else {
        oss << qps;
    }
    return oss.str();
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

static bool SendRequest(Worker* worker);

static bool ShouldStopWorkerAfterResponse(Worker* worker) {
    return ShouldStopByTime(worker->start_time_us) ||
           (FLAGS_test_iterations > 0 &&
            g_request_budget.load(butil::memory_order_relaxed) <= 0 &&
            g_inflight.load(butil::memory_order_relaxed) <= 0);
}

static void ContinueClosedLoopIfNeeded(Worker* worker) {
    if (ShouldStopWorkerAfterResponse(worker)) {
        worker->stop = true;
        return;
    }
    if (IsClosedLoop() && !g_stop && !worker->stop) {
        SendRequest(worker);
    }
}

static void HandleResponse(RespClosure* closure) {
    std::unique_ptr<brpc::Controller> cntl_guard(closure->cntl);
    std::unique_ptr<test::PerfTestResponse> response_guard(closure->resp);
    std::unique_ptr<RespClosure> closure_guard(closure);
    ConnectionSlot* slot = closure->slot;
    Worker* worker = closure->worker;

    if (closure->track_call_id) {
        UnregisterCallId(closure->call_id);
    }
    ReleaseInflightPermit();

    if (closure->cntl->Failed()) {
        slot->failed.fetch_add(1, butil::memory_order_relaxed);
        g_failed_cnt.fetch_add(1, butil::memory_order_relaxed);
        if (closure->cntl->ErrorCode() == brpc::ERPCTIMEDOUT ||
            closure->cntl->ErrorCode() == ETIMEDOUT) {
            slot->timeouts.fetch_add(1, butil::memory_order_relaxed);
            g_timeout_cnt.fetch_add(1, butil::memory_order_relaxed);
        }
        if (FLAGS_log_rpc_error && !g_stop) {
            LOG(ERROR) << "RPC call failed: " << closure->cntl->ErrorText();
        }
        ContinueClosedLoopIfNeeded(worker);
        return;
    }

    slot->completed.fetch_add(1, butil::memory_order_relaxed);
    if (FLAGS_record_latency) {
        if (g_latency_recorder) {
            *g_latency_recorder << closure->cntl->latency_us();
        }
    }
    if (!IsRawJsonPayload() && closure->resp != NULL &&
        !closure->resp->cpu_usage().empty()) {
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
                if (!g_stop) {
                    LOG(ERROR) << "Raw JSON echo check failed";
                }
                ContinueClosedLoopIfNeeded(worker);
                return;
            }
        }
    }
    const size_t payload_bytes = IsRawJsonPayload() ?
            g_json_payload.size() : closure->cntl->request_attachment().size();
    g_total_bytes.fetch_add(payload_bytes, butil::memory_order_relaxed);
    g_total_cnt.fetch_add(1, butil::memory_order_relaxed);
    UpdateClientCpuSample();

    ContinueClosedLoopIfNeeded(worker);
}

static bool SendRequest(Worker* worker) {
    if (!TryAcquireRequestPermit(worker->start_time_us)) {
        worker->stop = true;
        return false;
    }

    ConnectionSlot* slot = PickConnectionSlot();
    RespClosure* closure = new RespClosure;
    closure->worker = worker;
    closure->slot = slot;
    closure->cntl = new brpc::Controller();
    closure->resp =
            (IsRawJsonPayload() || IsMinimalResponseMode()) ?
            NULL : new test::PerfTestResponse();
    closure->track_call_id = FLAGS_cancel_inflight_on_stop;
    if (closure->track_call_id) {
        closure->call_id = closure->cntl->call_id();
        RegisterCallId(closure->call_id);
    }
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
        request.set_minimal_response(IsMinimalResponseMode());
        closure->cntl->request_attachment().append(worker->attachment);
        test::PerfTestService_Stub stub(slot->channel);
        stub.Test(closure->cntl, &request, closure->resp, done);
    }
    return true;
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
        if (!SendRequest(worker) && !worker->stop && !g_stop) {
            bthread_usleep(50);
        }
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
            slot->connection_group = "rdma_perf_" + g_transport_name +
                                     "_conn_" + std::to_string(i);
        }
        brpc::ChannelOptions options;
        options.socket_mode = g_socket_mode;
        options.protocol = FLAGS_protocol;
        options.connection_type = FLAGS_connection_type;
        options.connect_timeout_ms =
                FLAGS_connect_timeout_ms >= 0 ? FLAGS_connect_timeout_ms : FLAGS_rpc_timeout_ms;
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
            request.set_minimal_response(IsMinimalResponseMode());
            test::PerfTestService_Stub stub(slot->channel);
            stub.Test(&cntl, &request,
                      IsMinimalResponseMode() ? NULL : &response, NULL);
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
    const int connect_timeout_ms =
            FLAGS_connect_timeout_ms >= 0 ? FLAGS_connect_timeout_ms : FLAGS_rpc_timeout_ms;
    std::cout << "[Threads: " << thread_num
              << ", Depth: " << FLAGS_queue_depth
              << ", Attachment: " << attachment_size << "B"
              << ", Transport: " << g_transport_name
              << ", Echo: " << (FLAGS_echo_attachment ? "yes" : "no")
              << ", PayloadFormat: " << FLAGS_payload_format
              << ", ResponseMode: " << FLAGS_response_mode
              << ", RecordLatency: " << (FLAGS_record_latency ? "true" : "false")
              << ", LoadMode: " << FLAGS_load_mode
              << ", ConnectionType: " << FLAGS_connection_type
              << ", ConnectionNum: " << actual_connection_num
              << ", RpcTimeoutMs: " << FLAGS_rpc_timeout_ms
              << ", ConnectTimeoutMs: " << connect_timeout_ms
              << ", StopGraceMs: " << FLAGS_stop_grace_ms
              << ", CancelInflightOnStop: "
              << (FLAGS_cancel_inflight_on_stop ? "true" : "false")
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
    const size_t request_bytes = IsRawJsonPayload() ?
            g_json_payload.size() : std::max(attachment_size, 0);
    if (request_bytes >= 1024000 && FLAGS_echo_attachment && FLAGS_rpc_timeout_ms <= 2000) {
        LOG(WARNING) << "Large echo payload with rpc_timeout_ms=" << FLAGS_rpc_timeout_ms
                     << " may hit normal RPC timeout under pooled/open_loop pressure. "
                     << "Consider --rpc_timeout_ms=10000 or higher for large-payload tests.";
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
    if (FLAGS_record_latency) {
        g_latency_recorder.reset(
                new bvar::LatencyRecorder("client", LatencyWindowSeconds()));
    } else {
        g_latency_recorder.reset();
    }

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

    bool canceled_inflight = false;
    bool stop_grace_expired = false;
    uint64_t stop_time_us = 0;
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
            if (stop_time_us == 0) {
                stop_time_us = NowUs();
            }
            if (FLAGS_cancel_inflight_on_stop && !canceled_inflight) {
                canceled_inflight = true;
                CancelOutstandingRpc();
            }
            if (!no_more_inflight && FLAGS_stop_grace_ms >= 0 &&
                NowUs() - stop_time_us >= (uint64_t)FLAGS_stop_grace_ms * 1000UL) {
                stop_grace_expired = true;
                LOG(ERROR) << "Stop grace expired with inflight="
                           << g_inflight.load(butil::memory_order_relaxed)
                           << ". Printing summary and exiting.";
                break;
            }
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
        if (FLAGS_record_latency && g_latency_recorder) {
            std::cout << "Avg-Latency: " << g_latency_recorder->latency()
                      << ", 90th-Latency: " << g_latency_recorder->latency_percentile(0.9)
                      << ", 99th-Latency: " << g_latency_recorder->latency_percentile(0.99)
                      << ", 99.9th-Latency: " << g_latency_recorder->latency_percentile(0.999);
        } else {
            std::cout << "Avg-Latency: N/A"
                      << ", 90th-Latency: N/A"
                      << ", 99th-Latency: N/A"
                      << ", 99.9th-Latency: N/A";
        }
        std::cout << ", Throughput: " << throughput << "MB/s"
                  << ", QPS: " << FormatQps(
                          g_total_cnt.load(butil::memory_order_relaxed),
                          end_time - start_time)
                  << ", Failed: " << g_failed_cnt.load(butil::memory_order_relaxed)
                  << ", Timeout: " << g_timeout_cnt.load(butil::memory_order_relaxed)
                  << ", PeakInflight: " << g_peak_inflight.load(butil::memory_order_relaxed)
                  << ", Server CPU-utilization: ";
        if (!IsRawJsonPayload() && IsMinimalResponseMode()) {
            std::cout << "N/A";
        } else {
            std::cout << g_server_cpu_recorder.latency(10) << "%";
        }
        std::cout << ", Client CPU-utilization: "
                  << g_client_cpu_recorder.latency(10) << "%"
                  << std::endl;
    } else {
        std::cout << "Throughput: " << throughput << "MB/s"
                  << ", Completed: " << g_total_cnt.load(butil::memory_order_relaxed)
                  << ", Failed: " << g_failed_cnt.load(butil::memory_order_relaxed)
                  << ", PeakInflight: " << g_peak_inflight.load(butil::memory_order_relaxed)
                  << std::endl;
    }

    PrintConnectionStats();

    if (stop_grace_expired) {
        fflush(NULL);
        _exit(0);
    }

    g_stop = true;
    for (int k = 0; k < thread_num; ++k) {
        delete workers[k];
    }
    DestroyConnectionSlots();
}

}  // namespace

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
    g_socket_mode = transport.socket_mode;
    g_transport_name = transport.name;
    if (g_transport_name == "iouring") {
        LOG(INFO) << "transport=iouring uses a TCP client Channel. "
                  << "io_uring is enabled on the server side, following "
                  << "example/iouring_echo_c++.";
    }

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
    if (!IsNormalResponseMode() && !IsMinimalResponseMode()) {
        LOG(ERROR) << "Invalid response_mode=" << FLAGS_response_mode
                   << ", valid values are normal and minimal";
        return -1;
    }
    if (IsRawJsonPayload() && IsMinimalResponseMode()) {
        LOG(ERROR) << "response_mode=minimal is only supported for protobuf attachment payloads";
        return -1;
    }
    if (IsRawJsonPayload() && !IsRawJsonProtocolAllowed()) {
        LOG(ERROR) << "payload_format=raw_json requires --protocol=http or --protocol=h2, "
                   << "but protocol=" << FLAGS_protocol;
        return -1;
    }
    if (FLAGS_connect_timeout_ms < -1) {
        LOG(ERROR) << "connect_timeout_ms must be >= -1";
        return -1;
    }
    if (FLAGS_stop_grace_ms < -1) {
        LOG(ERROR) << "stop_grace_ms must be >= -1";
        return -1;
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

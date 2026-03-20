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

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>
#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/logging.h"
#include "butil/time.h"
#include "brpc/compress.h"
#include "brpc/global.h"
#include "brpc/server.h"
#include "bvar/variable.h"
#include "test.pb.h"

#ifdef BRPC_WITH_RDMA

DEFINE_int32(port, 8002, "TCP Port of this server");
DEFINE_bool(use_rdma, true, "Use RDMA or not");
DEFINE_string(response_compress_type, "none",
              "Compression algorithm for response protobuf body: none/snappy/gzip/zlib");

butil::atomic<uint64_t> g_last_time(0);
brpc::CompressType g_response_compress_type = brpc::COMPRESS_TYPE_NONE;

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string JoinSupportedCompressionNames() {
    std::vector<brpc::CompressHandler> handlers;
    brpc::ListCompressHandler(&handlers);
    std::string names = "none";
    for (size_t i = 0; i < handlers.size(); ++i) {
        names.append("/");
        names.append(handlers[i].name);
    }
    return names;
}

bool ParseCompressionType(const std::string& name,
                          brpc::CompressType* type,
                          std::string* error) {
    const std::string normalized = ToLower(name);
    if (normalized == "none") {
        *type = brpc::COMPRESS_TYPE_NONE;
        return true;
    }
    if (normalized == "snappy") {
        *type = brpc::COMPRESS_TYPE_SNAPPY;
    } else if (normalized == "gzip") {
        *type = brpc::COMPRESS_TYPE_GZIP;
    } else if (normalized == "zlib") {
        *type = brpc::COMPRESS_TYPE_ZLIB;
    } else if (normalized == "lz4") {
        *type = brpc::COMPRESS_TYPE_LZ4;
    } else {
        *error = "Unknown compression type `" + name + "`, supported values: "
               + JoinSupportedCompressionNames();
        return false;
    }
    if (brpc::FindCompressHandler(*type) == NULL) {
        *error = "Compression type `" + normalized + "` is defined but not registered "
               "in this brpc build, supported values: " + JoinSupportedCompressionNames();
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
        brpc::Controller* cntl =
            static_cast<brpc::Controller*>(cntl_base);
        cntl->set_response_compress_type(g_response_compress_type);
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
        response->set_server_request_decompress_avg_ns(
            brpc::GetRpcCompressStageLatency(brpc::RPC_COMPRESS_STAGE_SERVER_REQUEST));
        response->set_server_request_decompress_p99_ns(
            brpc::GetRpcCompressStageLatencyPercentile(
                brpc::RPC_COMPRESS_STAGE_SERVER_REQUEST, 0.99));
        response->set_server_response_compress_avg_ns(
            brpc::GetRpcCompressStageLatency(brpc::RPC_COMPRESS_STAGE_SERVER_RESPONSE));
        response->set_server_response_compress_p99_ns(
            brpc::GetRpcCompressStageLatencyPercentile(
                brpc::RPC_COMPRESS_STAGE_SERVER_RESPONSE, 0.99));
        if (request->echo_attachment() && !request->payload().empty()) {
            response->set_payload(request->payload());
        }
        if (request->echo_attachment()) {
            cntl->response_attachment().append(cntl->request_attachment());
        }
    }
};
}

int main(int argc, char* argv[]) {
    GFLAGS_NAMESPACE::ParseCommandLineFlags(&argc, &argv, true);
    brpc::GlobalInitializeOrDie();

    std::string error_text;
    if (!ParseCompressionType(FLAGS_response_compress_type,
                              &g_response_compress_type, &error_text)) {
        LOG(ERROR) << error_text;
        return -1;
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
    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Fail to start EchoServer";
        return -1;
    }

    server.RunUntilAskedToQuit();
    return 0;
}

#else


int main(int argc, char* argv[]) {
    LOG(ERROR) << " brpc is not compiled with rdma. To enable it, please refer to https://github.com/apache/brpc/blob/master/docs/en/rdma.md";
    return 0;
}

#endif

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


#include <atomic>
#include "butil/logging.h"
#include "bvar/latency_recorder.h"
#include "json2pb/json_to_pb.h"
#include "brpc/compress.h"
#include "brpc/protocol.h"
#include "brpc/proto_base.pb.h"

namespace brpc {

DEFINE_bool(log_rpc_compress_details, false,
            "Print INFO logs when RPC compression/decompression stages are exercised."
            " Logs the first few requests and then every 1000th request per stage/type.");

namespace {

bvar::LatencyRecorder g_rpc_compress_latency[RPC_COMPRESS_STAGE_COUNT] = {
    bvar::LatencyRecorder("rpc_client_request_compress"),
    bvar::LatencyRecorder("rpc_server_request_decompress"),
    bvar::LatencyRecorder("rpc_server_response_compress"),
    bvar::LatencyRecorder("rpc_client_response_decompress"),
};

std::atomic<uint64_t> g_rpc_compress_log_count[RPC_COMPRESS_STAGE_COUNT]
                                             [CompressType_MAX + 1];

}  // namespace

static const int MAX_HANDLER_SIZE = 1024;
static CompressHandler s_handler_map[MAX_HANDLER_SIZE] = { { NULL, NULL, NULL } };

int RegisterCompressHandler(CompressType type, 
                            CompressHandler handler) {
    if (NULL == handler.Compress || NULL == handler.Decompress) {
        LOG(FATAL) << "Invalid parameter: handler function is NULL";
        return -1;
    }
    int index = type;
    if (index < 0 || index >= MAX_HANDLER_SIZE) {
        LOG(FATAL) << "CompressType=" << type << " is out of range";
        return -1;
    }
    if (s_handler_map[index].Compress != NULL) {
        LOG(FATAL) << "CompressType=" << type << " was registered";
        return -1;
    }
    s_handler_map[index] = handler;
    return 0;
}

// Find CompressHandler by type.
// Returns NULL if not found
const CompressHandler* FindCompressHandler(CompressType type) {
    int index = type;
    if (index < 0 || index >= MAX_HANDLER_SIZE) {
        LOG(ERROR) << "CompressType=" << type << " is out of range";
        return NULL;
    }
    if (NULL == s_handler_map[index].Compress) {
        return NULL;
    }
    return &s_handler_map[index];
}

const char* CompressTypeToCStr(CompressType type) {
    if (type == COMPRESS_TYPE_NONE) {
        return "none";
    }
    const CompressHandler* handler = FindCompressHandler(type);
    return (handler != NULL ? handler->name : "unknown");
}

void ListCompressHandler(std::vector<CompressHandler>* vec) {
    vec->clear();
    for (int i = 0; i < MAX_HANDLER_SIZE; ++i) {
        if (s_handler_map[i].Compress != NULL) {
            vec->push_back(s_handler_map[i]);
        }
    }
}

bool ParseFromCompressedData(const butil::IOBuf& data, 
                             google::protobuf::Message* msg,
                             CompressType compress_type) {
    if (compress_type == COMPRESS_TYPE_NONE) {
        return ParsePbFromIOBuf(msg, data);
    }
    const CompressHandler* handler = FindCompressHandler(compress_type);
    if (NULL == handler) {
        return false;
    }

    Deserializer deserializer([msg](google::protobuf::io::ZeroCopyInputStream* input) {
        return msg->ParseFromZeroCopyStream(input);
    });
    return handler->Decompress(data, &deserializer);
}

bool SerializeAsCompressedData(const google::protobuf::Message& msg,
                               butil::IOBuf* buf, CompressType compress_type) {
    if (compress_type == COMPRESS_TYPE_NONE) {
        butil::IOBufAsZeroCopyOutputStream wrapper(buf);
        return msg.SerializeToZeroCopyStream(&wrapper);
    }
    const CompressHandler* handler = FindCompressHandler(compress_type);
    if (NULL == handler) {
        return false;
    }

    Serializer serializer([&msg](google::protobuf::io::ZeroCopyOutputStream* output) {
        return msg.SerializeToZeroCopyStream(output);
    });
    return handler->Compress(serializer, buf);
}

const char* RpcCompressStageToCStr(RpcCompressStage stage) {
    switch (stage) {
    case RPC_COMPRESS_STAGE_CLIENT_REQUEST:
        return "client_request_compress";
    case RPC_COMPRESS_STAGE_SERVER_REQUEST:
        return "server_request_decompress";
    case RPC_COMPRESS_STAGE_SERVER_RESPONSE:
        return "server_response_compress";
    case RPC_COMPRESS_STAGE_CLIENT_RESPONSE:
        return "client_response_decompress";
    case RPC_COMPRESS_STAGE_COUNT:
        break;
    }
    return "unknown";
}

void RecordRpcCompressStage(RpcCompressStage stage,
                            CompressType type,
                            int64_t latency_us,
                            size_t input_size,
                            size_t output_size) {
    if (type == COMPRESS_TYPE_NONE ||
        stage < RPC_COMPRESS_STAGE_CLIENT_REQUEST ||
        stage >= RPC_COMPRESS_STAGE_COUNT) {
        return;
    }
    g_rpc_compress_latency[stage] << latency_us;
    if (!FLAGS_log_rpc_compress_details) {
        return;
    }
    const int type_index = static_cast<int>(type);
    if (type_index < 0 || type_index > CompressType_MAX) {
        return;
    }
    const uint64_t seq =
        g_rpc_compress_log_count[stage][type_index].fetch_add(1) + 1;
    if (seq <= 5 || seq % 1000 == 0) {
        LOG(INFO) << "RPC " << RpcCompressStageToCStr(stage)
                  << " took " << latency_us << "us"
                  << ", type=" << CompressTypeToCStr(type)
                  << ", input_size=" << input_size
                  << ", output_size=" << output_size
                  << ", seq=" << seq;
    }
}

int64_t GetRpcCompressStageLatency(RpcCompressStage stage) {
    if (stage < RPC_COMPRESS_STAGE_CLIENT_REQUEST ||
        stage >= RPC_COMPRESS_STAGE_COUNT) {
        return 0;
    }
    return g_rpc_compress_latency[stage].latency(10);
}

int64_t GetRpcCompressStageLatencyPercentile(RpcCompressStage stage,
                                             double ratio) {
    if (stage < RPC_COMPRESS_STAGE_CLIENT_REQUEST ||
        stage >= RPC_COMPRESS_STAGE_COUNT) {
        return 0;
    }
    return g_rpc_compress_latency[stage].latency_percentile(ratio);
}

::google::protobuf::Metadata Serializer::GetMetadata() const {
    ::google::protobuf::Metadata metadata{};
    metadata.descriptor = SerializerBase::descriptor();
    metadata.reflection = nullptr;
    return metadata;
}

::google::protobuf::Metadata Deserializer::GetMetadata() const {
    ::google::protobuf::Metadata metadata{};
    metadata.descriptor = DeserializerBase::descriptor();
    metadata.reflection = nullptr;
    return metadata;
}

} // namespace brpc

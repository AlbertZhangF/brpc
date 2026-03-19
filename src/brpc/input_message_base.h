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


#ifndef BRPC_INPUT_MESSAGE_BASE_H
#define BRPC_INPUT_MESSAGE_BASE_H

#include "brpc/socket_id.h"           // SocketId
#include "brpc/destroyable.h"         // DestroyingPtr


namespace brpc {

// Messages returned by Parse handlers must extend this class
class InputMessageBase : public Destroyable {
protected:
    // Implement this method to customize deletion of this message.
    virtual void DestroyImpl() = 0;
    
public:
    // Called to release the memory of this message instead of "delete"
    void Destroy();
    
    // Own the socket where this message is from.
    Socket* ReleaseSocket();

    // Get the socket where this message is from.
    Socket* socket() const { return _socket.get(); }

    // Arg of the InputMessageHandler which parses this message successfully.
    const void* arg() const { return _arg; }

    // [Internal]
    int64_t received_us() const { return _received_us; }
    int64_t base_real_us() const { return _base_real_us; }

public:
    // For schedule latency analysis (unit: ns)
    // These timestamps track the scheduling path from message received to processing
    // All timestamps are set to 0 by default and only populated when tracking is enabled
    uint64_t msg_received_ns{0};      // Message received (cut complete)
    uint64_t queue_msg_start_ns{0};   // QueueMessage start
    uint64_t queue_msg_end_ns{0};     // QueueMessage end (bthread_start_background returned)
    uint64_t bthread_queued_ns{0};    // bthread queued (ready_to_run done)
    uint64_t bthread_signaled_ns{0};  // signal_task called
    uint64_t bthread_scheduled_ns{0}; // sched_to completed
    uint64_t bthread_running_ns{0};   // task_runner started
    uint64_t process_input_ns{0};     // ProcessInputMessage started
    uint64_t process_rpc_ns{0};       // ProcessRpcRequest started

protected:
    virtual ~InputMessageBase();

private:
friend class InputMessenger;
friend void* ProcessInputMessage(void*);
friend class Stream;
    int64_t _received_us;
    int64_t _base_real_us;
    SocketUniquePtr _socket;
    void (*_process)(InputMessageBase* msg);
    const void* _arg;
};

} // namespace brpc


#endif  // BRPC_INPUT_MESSAGE_BASE_H
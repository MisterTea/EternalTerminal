// Copyright 2025 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "util/mach/exception_handler_protocol.h"

#include <mach/mig_errors.h>

#include "base/apple/mach_logging.h"
#include "base/logging.h"
#include "util/mach/mach_message.h"

namespace crashpad {

std::string ClientToServerMessage::Payload() const {
  const char* payload_data = static_cast<const char*>(payload.address);
  if (!payload_data || payload.size == 0) {
    return std::string();
  }
  return std::string(payload_data, static_cast<size_t>(payload.size - 1));
}

bool SendClientToServerMessage(mach_port_t port,
                               ClientToServerMessage::Type type,
                               const std::string& payload) {
  if (port == MACH_PORT_NULL) {
    DLOG(ERROR) << "cannot send client-to-server message: invalid port";
    return false;
  }

  ClientToServerMessage message = {};
  message.header.msgh_bits =
      MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
  message.header.msgh_size = sizeof(message);
  message.header.msgh_remote_port = port;
  message.header.msgh_local_port = MACH_PORT_NULL;
  message.header.msgh_id = kClientToServerMessageID;

  message.body.msgh_descriptor_count = 1;

  message.payload.address = const_cast<char*>(payload.c_str());
  message.payload.size = static_cast<mach_msg_size_t>(payload.size() + 1);
  message.payload.deallocate = FALSE;
  message.payload.copy = MACH_MSG_VIRTUAL_COPY;
  message.payload.type = MACH_MSG_OOL_DESCRIPTOR;

  message.ndr = NDR_record;
  message.type = type;

  kern_return_t kr = mach_msg(&message.header,
                              MACH_SEND_MSG,
                              sizeof(message),
                              0,
                              MACH_PORT_NULL,
                              MACH_MSG_TIMEOUT_NONE,
                              MACH_PORT_NULL);
  if (kr != KERN_SUCCESS) {
    MACH_LOG(ERROR, kr) << "mach_msg";
    return false;
  }

  return true;
}

const ClientToServerMessage* ReceiveClientToServerMessage(
    const mach_msg_header_t* in_header,
    mach_msg_header_t* out_header) {
  if (in_header->msgh_id != kClientToServerMessageID) {
    return nullptr;
  }

  PrepareMIGReplyFromRequest(in_header, out_header);
  SetMIGReplyError(out_header, MIG_NO_REPLY);
  return reinterpret_cast<const ClientToServerMessage*>(in_header);
}

}  // namespace crashpad

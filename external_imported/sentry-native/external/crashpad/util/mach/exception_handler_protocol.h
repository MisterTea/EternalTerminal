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

#ifndef CRASHPAD_UTIL_MACH_EXCEPTION_HANDLER_PROTOCOL_H_
#define CRASHPAD_UTIL_MACH_EXCEPTION_HANDLER_PROTOCOL_H_

#include <mach/mach.h>
#include <stdint.h>

#include <string>

namespace crashpad {

//! \brief Mach message ID used by client-to-server control messages sent to
//!     the crashpad handler's exception port.
//!
//! FourCC `'CPad'`, chosen far from Apple's Mach MIG subsystem bases (which
//! cluster between 64 and ~6000) so it cannot collide with a kernel-originated
//! message delivered to the handler's exception port.
constexpr mach_msg_id_t kClientToServerMessageID = 0x43506164;  // 'CPad'

//! \brief A control message sent by a Crashpad client to the handler.
//!
//! The message carries a typed command and an optional out-of-line payload.
//! The payload is always transferred as an OOL descriptor even when empty,
//! so the struct layout is uniform for all message types.
struct ClientToServerMessage {
  //! \brief Command types understood by the handler.
  enum Type : uint32_t {
    //! \brief Ask the handler to retry pending report uploads. Carries no
    //!     meaningful payload.
    kRequestRetry = 1,

    //! \brief Add a file to the list of files attached to crash reports. The
    //!     payload contains the attachment path.
    kAddAttachment = 2,

    //! \brief Remove a file from the list of files attached to crash reports.
    //!     The payload contains the attachment path.
    kRemoveAttachment = 3,
  };

  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_ool_descriptor_t payload;
  NDR_record_t ndr;
  Type type;

  //! \brief Returns the optional string payload.
  std::string Payload() const;
};

//! \brief Sends a ClientToServerMessage to the handler.
//!
//! \param[in] port The handler's exception port.
//! \param[in] type The command type.
//! \param[in] payload An optional string payload. The null terminator is
//!     included in the transferred OOL region, so `payload.size` is always
//!     at least 1.
//!
//! \return `true` on success, `false` otherwise (logs the error).
bool SendClientToServerMessage(mach_port_t port,
                               ClientToServerMessage::Type type,
                               const std::string& payload = "");

//! \brief Validates and unwraps a received ClientToServerMessage.
//!
//! \param[in] in_header The incoming Mach message header.
//! \param[out] out_header The reply header to populate.
//!
//! \return A pointer to the received ClientToServerMessage on success (aliases
//!     \a in_header). `nullptr` if the message is not a ClientToServerMessage
//!     or is malformed.
const ClientToServerMessage* ReceiveClientToServerMessage(
    const mach_msg_header_t* in_header,
    mach_msg_header_t* out_header);

}  // namespace crashpad

#endif  // CRASHPAD_UTIL_MACH_EXCEPTION_HANDLER_PROTOCOL_H_

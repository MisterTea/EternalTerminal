#ifndef __ET_TERMINAL_INTERRUPT_DRAIN__
#define __ET_TERMINAL_INTERRUPT_DRAIN__

#include <deque>

#include "ETerminal.pb.h"
#include "Headers.hpp"
#include "Packet.hpp"
#include "SocketHandler.hpp"
#include "WriteBuffer.hpp"

namespace et {

/**
 * @brief On client interrupt, drain ready packets from the terminal fd.
 *
 * TERMINAL_BUFFER flood is enqueued into @p buf (then filterDroppable).
 * Non-buffer packets (notably TERMINAL_EXIT_STATUS) must be preserved for
 * the normal dispatch path — same idea as jumphost pending.
 */
inline void drainDiscardReadableBytes(shared_ptr<SocketHandler> handler, int fd,
                                      WriteBuffer* buf,
                                      std::deque<Packet>* preserved) {
  bool got = false;
  while (true) {
    if (!waitOnSocketData(fd, 0, 0)) {
      break;
    }
    Packet packet;
    try {
      if (!handler->readPacket(fd, &packet)) {
        continue;
      }
    } catch (const std::runtime_error&) {
      break;
    }
    // Only TERMINAL_BUFFER flood is filtered on interrupt. Non-buffer packets
    // (notably TERMINAL_EXIT_STATUS) are preserved for the normal dispatch
    // path — same idea as jumphost pending.
    if (packet.getHeader() == TerminalPacketType::TERMINAL_BUFFER) {
      et::TerminalBuffer tb =
          stringToProto<et::TerminalBuffer>(packet.getPayload());
      buf->enqueue(tb.buffer());
      got = true;
    } else if (preserved != nullptr) {
      preserved->push_back(packet);
    }
  }
  if (got) {
    buf->filterDroppable();
  }
}

}  // namespace et

#endif  // __ET_TERMINAL_INTERRUPT_DRAIN__

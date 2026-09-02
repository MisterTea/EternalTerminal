#ifndef __HTM_CONTROL_COMMANDS_H__
#define __HTM_CONTROL_COMMANDS_H__

#include "ControlMode.hpp"
#include "Headers.hpp"
#include "MultiplexerState.hpp"

namespace et {

enum class ControlAction {
  None,
  Detach,
  KillServer,
  Error,
};

/** @brief Dispatch one tmux control-mode command against the multiplexer. */
ControlAction executeControlCommand(MultiplexerState* mux,
                                    ControlWriter* writer, const string& line);

string encodeSendKeys(const vector<string>& tokens, bool hex, bool literal);

}  // namespace et

#endif

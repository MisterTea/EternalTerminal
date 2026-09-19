#pragma once
namespace et { namespace htm {
/// Separate controller scope for persistent named sessions (Issue #779)
struct ControllerScope {
    bool isolated = false;
    int sessionId = 0;
};
}} // namespace

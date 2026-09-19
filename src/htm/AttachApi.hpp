#pragma once
namespace et { namespace htm {
/// Named session attach/detach API (Issue #779)
void attachNamedSession(const char* name);
void detachNamedSession(const char* name);
}} // namespace

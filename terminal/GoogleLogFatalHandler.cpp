#include "GoogleLogFatalHandler.hpp"

#include "Headers.hpp"

namespace google {
namespace glog_internal_namespace_ {
void DumpStackTraceToString(string* stacktrace);
}
}  // namespace google

static void DumpStackTraceToFileAndExit() {
  string s;
  google::glog_internal_namespace_::DumpStackTraceToString(&s);
  LOG(ERROR) << "STACK TRACE:\n" << s;
  abort();
}

namespace et {
void GoogleLogFatalHandler::handle() {
  google::InstallFailureFunction(&DumpStackTraceToFileAndExit);
}

}  // namespace et

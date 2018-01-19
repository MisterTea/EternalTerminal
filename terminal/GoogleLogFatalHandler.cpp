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

  // TOOD(hamaji): Use signal instead of sigaction?
  if (google::glog_internal_namespace_::IsFailureSignalHandlerInstalled()) {
// Set the default signal handler for SIGABRT, to avoid invoking our
// own signal handler installed by InstallFailureSignalHandler().
#ifdef HAVE_SIGACTION
    struct sigaction sig_action;
    memset(&sig_action, 0, sizeof(sig_action));
    sigemptyset(&sig_action.sa_mask);
    sig_action.sa_handler = SIG_DFL;
    sigaction(SIGABRT, &sig_action, NULL);
#elif defined(OS_WINDOWS)
    signal(SIGABRT, SIG_DFL);
#endif  // HAVE_SIGACTION
  }

  abort();
}

namespace et {
void GoogleLogFatalHandler::handle() {
  google::InstallFailureFunction(&DumpStackTraceToFileAndExit);
}

}  // namespace et

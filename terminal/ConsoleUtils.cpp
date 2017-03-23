#include <sys/types.h>
#include <paths.h>
#include <pwd.h>
#include "ConsoleUtils.hpp"

namespace et {
std::string getTerminal() {
  char *env_shell = ::getenv("SHELL");
  if (env_shell) {
    return string(env_shell);
  }
  struct passwd *pwent = getpwuid(geteuid());
  if (!pwent) {
    return string(_PATH_BSHELL);
  }
  return string(pwent->pw_shell);
}
}

#include "SystemUtils.hpp"
#include <paths.h>
#include <pwd.h>
#include <sys/types.h>

#include <grp.h>

#ifdef WITH_SELINUX
#include <selinux/get_context_list.h>
#include <selinux/selinux.h>
#endif

namespace et {
void rootToUser(passwd* pwd) {
  string terminal;
  gid_t groups[65536];
  int ngroups = 65536;
#ifdef WITH_SELINUX
  char* sename = NULL;
  char* level = NULL;
  FATAL_FAIL(getseuserbyname(pwd->pw_name, &sename, &level));
  security_context_t user_ctx = NULL;
  FATAL_FAIL(
      get_default_context_with_level(sename, level, NULL, &user_ctx));
  setexeccon(user_ctx);
  free(sename);
  free(level);
#endif

#ifdef __APPLE__
  if (getgrouplist(pwd->pw_name, pwd->pw_gid, (int*)groups, &ngroups) ==
      -1) {
    LOG(FATAL) << "User is part of more than 65536 groups!";
  }
#else
  if (getgrouplist(pwd->pw_name, pwd->pw_gid, groups, &ngroups) == -1) {
    LOG(FATAL) << "User is part of more than 65536 groups!";
  }
#endif

#ifdef setresgid
  FATAL_FAIL(setresgid(pwd->pw_gid, pwd->pw_gid, pwd->pw_gid));
#else  // OS/X
  FATAL_FAIL(setregid(pwd->pw_gid, pwd->pw_gid));
#endif

#ifdef __APPLE__
  FATAL_FAIL(initgroups(pwd->pw_name, pwd->pw_gid));
#else
  FATAL_FAIL(::setgroups(ngroups, groups));
#endif

#ifdef setresuid
  FATAL_FAIL(setresuid(pwd->pw_uid, pwd->pw_uid, pwd->pw_uid));
#else  // OS/X
  FATAL_FAIL(setreuid(pwd->pw_uid, pwd->pw_uid));
#endif
  if (pwd->pw_shell) {
    terminal = pwd->pw_shell;
  } else {
    char *env_shell = ::getenv("SHELL");
    if (env_shell) {
      terminal = string(env_shell);
    } else {
      terminal = string(_PATH_BSHELL);
    }
  }
  setenv("SHELL", terminal.c_str(), 1);

  const char* homedir = pwd->pw_dir;
  setenv("HOME", homedir, 1);
  setenv("USER", pwd->pw_name, 1);
  setenv("LOGNAME", pwd->pw_name, 1);
  setenv("PATH", "/usr/local/bin:/bin:/usr/bin", 1);
  chdir(pwd->pw_dir);
}
}  // namespace et

#include "DaemonCreator.hpp"

namespace et {
int DaemonCreator::create() {
  pid_t pid;

  /* Fork off the parent process */
  pid = fork();

  /* An error occurred */
  if (pid < 0) exit(EXIT_FAILURE);

  /* Success: Return so the parent can continue */
  if (pid > 0) return PARENT;

  /* On success: The child process becomes session leader */
  if (setsid() < 0) exit(EXIT_FAILURE);

  /* Catch, ignore and handle signals */
  // TODO: Implement a working signal handler */
  signal(SIGCHLD, SIG_IGN);
  signal(SIGHUP, SIG_IGN);

  /* Fork off for the second time*/
  pid = fork();

  /* An error occurred */
  if (pid < 0) exit(EXIT_FAILURE);

  /* Success: Let the parent terminate */
  if (pid > 0) exit(EXIT_SUCCESS);

  /* Set new file permissions */
  umask(0);

  /* Change the working directory to the root directory */
  /* or another appropriated directory */
  chdir("/");

  /* Close all open file descriptors */
  int x;
  for (x = sysconf(_SC_OPEN_MAX); x >= 0; x--) {
    close(x);
  }
  return CHILD;
}
}  // namespace et
#include "ProcessHelper.hpp"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syslog.h>
#include <signal.h>

using namespace et;

void ProcessHelper::daemonize() {
  pid_t pid;

  /* Fork off the parent process */
  pid = fork();

  /* An error occurred */
  if (pid < 0)
    exit(EXIT_FAILURE);

  /* Success: Let the parent terminate */
  if (pid > 0)
    exit(EXIT_SUCCESS);

  /* On success: The child process becomes session leader */
  if (setsid() < 0)
    exit(EXIT_FAILURE);

  /* Catch, ignore and handle signals */
  //TODO: Implement a working signal handler */
  signal(SIGCHLD, SIG_IGN);
  signal(SIGHUP, SIG_IGN);

  /* Fork off for the second time*/
  pid = fork();

  /* An error occurred */
  if (pid < 0)
    exit(EXIT_FAILURE);

  /* Success: Let the parent terminate */
  if (pid > 0)
    exit(EXIT_SUCCESS);

  /* Set new file permissions */
  //umask(0);

  /* Close all open file descriptors */
  int x;
  for (x = sysconf(_SC_OPEN_MAX); x>0; x--)
  {
    close (x);
  }

  /* Reopen stdin (fd = 0), stdout (fd = 1), stderr (fd = 2) */
  stdin = fopen("/dev/null", "r");
  stdout = fopen("/tmp/et_err", "w+");
  setvbuf(stdout, NULL, _IOLBF, BUFSIZ);  // set to line buffering
  stderr = fopen("/tmp/et_err", "w+");
  setvbuf(stderr, NULL, _IOLBF, BUFSIZ);  // set to line buffering

  /* Open the log file */
  //openlog ("ETServer", LOG_PID, LOG_DAEMON);
}

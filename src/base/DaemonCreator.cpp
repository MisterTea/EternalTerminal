#include "DaemonCreator.hpp"

namespace et {
int DaemonCreator::createSessionLeader() { return ::daemon(0, 0); }

int DaemonCreator::create(bool parentExit, string childPidFile) {
  pid_t pid;

  /* Fork off the parent process */
  pid = fork();

  /* An error occurred */
  if (pid < 0) exit(EXIT_FAILURE);

  /* Success: Return so the parent can continue */
  if (pid > 0) {
    if (parentExit) {
      exit(EXIT_SUCCESS);
    }
    return PARENT;
  }

  /* On success: The child process becomes session leader */
  if (setsid() < 0) exit(EXIT_FAILURE);

  /* Catch, ignore and handle signals */
  signal(SIGHUP, SIG_IGN);

  /* Fork off for the second time*/
  pid = fork();

  /* An error occurred */
  if (pid < 0) exit(EXIT_FAILURE);

  /* Success: Let the parent terminate */
  if (pid > 0) exit(EXIT_SUCCESS);

  /* Child process, write pid file */
  if (childPidFile != "") {
    int pidFilehandle = open(childPidFile.c_str(), O_RDWR | O_CREAT, 0600);
    if (pidFilehandle == -1) {
      STFATAL << "Error opening pidfile for writing: " << childPidFile;
    }

    // Max pid length for x86_64 is 2^22 ~ 4000000
    std::stringstream pid_ss;
    pid_ss << getpid() << "\n";
    std::string pid_str = pid_ss.str();
    write(pidFilehandle, pid_str.c_str(), pid_str.length());
    close(pidFilehandle);
  }

  /* Change the working directory to the root directory */
  /* or another appropriated directory */
  chdir("/");

  auto fd = open("/dev/null", O_WRONLY | O_CREAT, 0666);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);

  auto fd2 = open("/dev/null", O_RDONLY);
  dup2(fd2, STDIN_FILENO);

  return CHILD;
}
}  // namespace et

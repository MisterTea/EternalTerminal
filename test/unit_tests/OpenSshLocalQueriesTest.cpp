#include <cstdio>
#include <fstream>

#include "SubprocessUtils.hpp"
#include "TestHeaders.hpp"
#ifndef WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace et;

namespace {

string findEtBinary() {
#ifndef WIN32
  if (access("./et", X_OK) == 0) {
    return "./et";
  }
  if (access("../build/et", X_OK) == 0) {
    return "../build/et";
  }
#endif
  return "";
}

}  // namespace

#ifndef WIN32
TEST_CASE("et -V prints OpenSSH_ and exits 0 without connecting",
          "[OpenSshLocalQueries][cli]") {
  const string et = findEtBinary();
  if (et.empty()) {
    SKIP("et binary is not built");
  }

  string cmd = et + " -V";
  FILE* pipe = popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[256];
  string output;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  int status = pclose(pipe);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  REQUIRE(output.find("OpenSSH_") != string::npos);
  REQUIRE(output.find("et version") == string::npos);
}

TEST_CASE("et --version still prints et version",
          "[OpenSshLocalQueries][cli]") {
  const string et = findEtBinary();
  if (et.empty()) {
    SKIP("et binary is not built");
  }

  string cmd = et + " --version";
  FILE* pipe = popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[256];
  string output;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  int status = pclose(pipe);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  REQUIRE(output.find("et version") != string::npos);
}

TEST_CASE("et -G -F -o prints resolved keywords without connecting",
          "[OpenSshLocalQueries][cli]") {
  const string et = findEtBinary();
  if (et.empty()) {
    SKIP("et binary is not built");
  }

  const fs::path tempDir = fs::temp_directory_path() /
                           ("et_test_openssh_cli_" + sole::uuid4().str());
  fs::remove_all(tempDir);
  fs::create_directories(tempDir);
  const fs::path configPath = tempDir / "config";
  std::ofstream(configPath) << "Host nowhere.invalid\n"
                               "  HostName 203.0.113.9\n"
                               "  User cursor\n"
                               "  Port 22\n";

  // Destination is non-routable documentation space; -G must not dial it.
  string cmd = et + " -G -F " + configPath.string() +
               " -o ConnectTimeout=9 -o RemoteCommand=true"
               " -o ControlPath=/tmp/et-cm -o ControlMaster=auto"
               " -o ServerAliveInterval=4 -o BatchMode=yes"
               " -o ClearAllForwardings=yes -o ExitOnForwardFailure=yes"
               " -o ControlPersist=yes nowhere.invalid";
  FILE* pipe = popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[512];
  string output;
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output += buffer;
  }
  int status = pclose(pipe);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);
  REQUIRE(output.find("host nowhere.invalid\n") != string::npos);
  REQUIRE(output.find("user cursor\n") != string::npos);
  REQUIRE(output.find("hostname 203.0.113.9\n") != string::npos);
  REQUIRE(output.find("connecttimeout 9\n") != string::npos);
  REQUIRE(output.find("remotecommand true\n") != string::npos);
  REQUIRE(output.find("controlpath /tmp/et-cm\n") != string::npos);
  REQUIRE(output.find("controlmaster auto\n") != string::npos);
  REQUIRE(output.find("serveraliveinterval 4\n") != string::npos);
  REQUIRE(output.find("Could not reach") == string::npos);

  fs::remove_all(tempDir);
}
#endif

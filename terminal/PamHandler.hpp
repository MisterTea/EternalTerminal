#include "Headers.hpp"

#include <pwd.h>
#include <util.h>
#include <security/pam_appl.h>

namespace et {
class PamHandler {
 public:
  PamHandler(passwd* _pwd);

  void initialize();

  void beginSession();

  void setupEnvironment();

  void cleanup();
 protected:
  passwd* pwd;
  pam_conv conv;
  pam_handle_t* pam_h;
  int authenticated;
  int setcred;
  int opened_session;
  int last_status;
  int conv_reject_prompts = 0;
};
}

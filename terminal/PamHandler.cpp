#include "PamHandler.hpp"

#define PAM_SERVICE_NAME "ET"

namespace et {
PamHandler::PamHandler(passwd* _pwd) :
    pwd(_pwd) {
  pam_h = 0;
  authenticated = 0, setcred = 0, opened_session = 0,
      last_status = PAM_SUCCESS;
  conv_reject_prompts = 0;
  //conv = { &conv_fn, this };
}

void PamHandler::initialize() {
  return;
  LOG(INFO) << "CALLING PAM INITIALIZE" << endl;
  int rv;
  if ((rv = pam_start(PAM_SERVICE_NAME, pwd->pw_name, &conv, &pam_h)) != PAM_SUCCESS) {
    LOG(FATAL) << "pam_start() failure: " << rv << endl;
  }

  if ((rv = pam_authenticate(pam_h, 0)) != PAM_SUCCESS) {
    LOG(FATAL) << "pam_authenticate(): " << pam_strerror(pam_h, rv);
  }

  rv = pam_acct_mgmt(pam_h, 0);

  if (rv == PAM_NEW_AUTHTOK_REQD) {
    LOG(INFO) << "pam_acct_mgmt(): PAM_NEW_AUTHTOK_REQD for " << pwd->pw_name << endl;;
    rv = pam_chauthtok(pam_h, PAM_CHANGE_EXPIRED_AUTHTOK);
    if (rv != PAM_SUCCESS) {
      LOG(FATAL) << "pam_chauthtok(PAM_CHANGE_EXPIRED_AUTHTOK): "
                 << pam_strerror(pam_h, rv) << endl;
    }
  } else if (rv != PAM_SUCCESS) {
    LOG(FATAL) << "pam_acct_mgmt(): " << pam_strerror(pam_h, rv) << endl;
  }

  authenticated = 1;
  LOG(FATAL) << "PAM INIT FINISHED";
}

void PamHandler::beginSession() {
  return;
  LOG(INFO) << "Beginning pam session";
  const char* username = pwd->pw_name;
  int rv, i;
  if ((rv = pam_start(PAM_SERVICE_NAME, username, &conv, &pam_h)) != PAM_SUCCESS)
    LOG(FATAL) << "pam_start() failure: " << rv;

  conv_reject_prompts = 1;

  int setcred_first = 1;

  for (i = 0; i < 2; ++i) {
    if (i != setcred_first) {
      if ((rv = pam_setcred(pam_h, PAM_ESTABLISH_CRED)) != PAM_SUCCESS) {
        LOG(INFO) << "pam_setcred(PAM_ESTABLISH_CRED): " << pam_strerror(pam_h, rv);
      } else {
        setcred = 1;
      }
    } else {
      if ((rv = pam_open_session(pam_h, 0)) != PAM_SUCCESS) {
        LOG(INFO) << "pam_open_session(): " << pam_strerror(pam_h, rv);
      } else {
        opened_session = 1;
      }
    }
  }
}

void PamHandler::setupEnvironment() {
  return;
  /* XXX should we really prevent MAIL, PATH set through PAM? */
  static const char* banned_env[] = {"SHELL", "HOME", "LOGNAME", "MAIL", "CDPATH",
                               "IFS", "PATH", "LD_", 0 };

  char **pam_env = pam_getenvlist(pam_h), **env;
  if (!pam_env) {
    LOG(INFO) << "pam_getenvlist() failed";
    return;
  }
  for (env = pam_env; *env; ++env) {
    bool banned=false;
    const char** banp;
    for (banp = banned_env; *banp; ++banp) {
      if (strncmp(*env, *banp, strlen(*banp)) == 0) {
        banned=true;
        break;
      }
    }
    if (!banned) {
      putenv(*env);
    }
  }
  free(pam_env);
}

void PamHandler::cleanup() {
  return;
  int rv;
  if (!pam_h) return;
  if (setcred) {
    if ((rv = pam_setcred(pam_h, PAM_DELETE_CRED)) != PAM_SUCCESS)
      VLOG(1) << "pam_setcred(PAM_DELETE_CRED): " << pam_strerror(pam_h, rv);
  }
  if (opened_session) {
    if ((rv = pam_close_session(pam_h, 0)) != PAM_SUCCESS)
      VLOG(1) << "pam_close_session(): " << pam_strerror(pam_h, rv);
  }
  if ((rv = pam_end(pam_h, last_status)) != PAM_SUCCESS)
    LOG(FATAL) << "pam_end(" << last_status << "): " << rv;
  pam_h = 0;
}
}

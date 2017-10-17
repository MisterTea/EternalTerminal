/* Acknowledgement: this file gathers config file parsing related functions in
 * libssh */
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <string>

/* This is needed for a standard getpwuid_r on opensolaris */
#define _POSIX_PTHREAD_SEMANTICS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif /* HAVE_SYS_TIME_H */

using namespace std;

#define MAX_LINE_SIZE 1024

#define SAFE_FREE(x)   \
  do {                 \
    if ((x) != NULL) { \
      free(x);         \
      x = NULL;        \
    }                  \
  } while (0)

#ifndef NSS_BUFLEN_PASSWD
#define NSS_BUFLEN_PASSWD 4096
#endif /* NSS_BUFLEN_PASSWD */

#ifndef MAX_BUF_SIZE
#define MAX_BUF_SIZE 4096
#endif

/* Socket type */
#ifndef socket_t
typedef int socket_t;
#endif

enum ssh_config_opcode_e {
  SOC_UNSUPPORTED = -1,
  SOC_HOST,
  SOC_HOSTNAME,
  SOC_PORT,
  SOC_USERNAME,
  SOC_TIMEOUT,
  SOC_PROTOCOL,
  SOC_STRICTHOSTKEYCHECK,
  SOC_KNOWNHOSTS,
  SOC_PROXYCOMMAND,
  SOC_GSSAPISERVERIDENTITY,
  SOC_GSSAPICLIENTIDENTITY,
  SOC_GSSAPIDELEGATECREDENTIALS,
  SOC_INCLUDE,
  SOC_PROXYJUMP,
  SOC_END /* Keep this one last in the list */
};

enum ssh_options_e {
  SSH_OPTIONS_HOST,
  SSH_OPTIONS_PORT,
  SSH_OPTIONS_PORT_STR,
  SSH_OPTIONS_FD,
  SSH_OPTIONS_USER,
  SSH_OPTIONS_SSH_DIR,
  SSH_OPTIONS_IDENTITY,
  SSH_OPTIONS_ADD_IDENTITY,
  SSH_OPTIONS_KNOWNHOSTS,
  SSH_OPTIONS_TIMEOUT,
  SSH_OPTIONS_TIMEOUT_USEC,
  SSH_OPTIONS_SSH1,
  SSH_OPTIONS_SSH2,
  SSH_OPTIONS_LOG_VERBOSITY,
  SSH_OPTIONS_LOG_VERBOSITY_STR,
  SSH_OPTIONS_CIPHERS_C_S,
  SSH_OPTIONS_CIPHERS_S_C,
  SSH_OPTIONS_COMPRESSION_C_S,
  SSH_OPTIONS_COMPRESSION_S_C,
  SSH_OPTIONS_PROXYCOMMAND,
  SSH_OPTIONS_BINDADDR,
  SSH_OPTIONS_STRICTHOSTKEYCHECK,
  SSH_OPTIONS_COMPRESSION,
  SSH_OPTIONS_COMPRESSION_LEVEL,
  SSH_OPTIONS_KEY_EXCHANGE,
  SSH_OPTIONS_HOSTKEYS,
  SSH_OPTIONS_GSSAPI_SERVER_IDENTITY,
  SSH_OPTIONS_GSSAPI_CLIENT_IDENTITY,
  SSH_OPTIONS_GSSAPI_DELEGATE_CREDENTIALS,
  SSH_OPTIONS_HMAC_C_S,
  SSH_OPTIONS_HMAC_S_C,
  SSH_OPTIONS_PROXYJUMP,
};

struct Options {
  char *username;
  char *host;
  char *sshdir;
  char *knownhosts;
  char *ProxyCommand;
  char *ProxyJump;
  unsigned long timeout; /* seconds */
  unsigned int port;
  int StrictHostKeyChecking;
  int ssh2;
  int ssh1;
  char *gss_server_identity;
  char *gss_client_identity;
  int gss_delegate_creds;
};

struct ssh_config_keyword_table_s {
  const char *name;
  enum ssh_config_opcode_e opcode;
};

static struct ssh_config_keyword_table_s ssh_config_keyword_table[] = {
    {"host", SOC_HOST},
    {"hostname", SOC_HOSTNAME},
    {"port", SOC_PORT},
    {"user", SOC_USERNAME},
    {"connecttimeout", SOC_TIMEOUT},
    {"protocol", SOC_PROTOCOL},
    {"stricthostkeychecking", SOC_STRICTHOSTKEYCHECK},
    {"userknownhostsfile", SOC_KNOWNHOSTS},
    {"proxycommand", SOC_PROXYCOMMAND},
    {"gssapiserveridentity", SOC_GSSAPISERVERIDENTITY},
    {"gssapiclientidentity", SOC_GSSAPICLIENTIDENTITY},
    {"gssapidelegatecredentials", SOC_GSSAPIDELEGATECREDENTIALS},
    {"include", SOC_INCLUDE},
    {"proxyjump", SOC_PROXYJUMP},
    {NULL, SOC_UNSUPPORTED}};

static enum ssh_config_opcode_e ssh_config_get_opcode(char *keyword) {
  int i;

  for (i = 0; ssh_config_keyword_table[i].name != NULL; i++) {
    if (strcasecmp(keyword, ssh_config_keyword_table[i].name) == 0) {
      return ssh_config_keyword_table[i].opcode;
    }
  }

  return SOC_UNSUPPORTED;
}

static int ssh_config_parse_line(struct Options *options, const char *line,
                                 unsigned int count, int *parsing, int seen[]);

char *ssh_get_user_home_dir(void) {
  char *szPath = NULL;
  struct passwd pwd;
  struct passwd *pwdbuf;
  char buf[NSS_BUFLEN_PASSWD];
  int rc;

  rc = getpwuid_r(getuid(), &pwd, buf, NSS_BUFLEN_PASSWD, &pwdbuf);
  if (rc != 0) {
    szPath = getenv("HOME");
    if (szPath == NULL) {
      return NULL;
    }
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "%s", szPath);

    return strdup(buf);
  }

  szPath = strdup(pwd.pw_dir);

  return szPath;
}

char *ssh_get_local_username(void) {
  struct passwd pwd;
  struct passwd *pwdbuf;
  char buf[NSS_BUFLEN_PASSWD];
  char *name;
  int rc;

  rc = getpwuid_r(getuid(), &pwd, buf, NSS_BUFLEN_PASSWD, &pwdbuf);
  if (rc != 0) {
    return NULL;
  }

  name = strdup(pwd.pw_name);

  if (name == NULL) {
    return NULL;
  }

  return name;
}

char *ssh_lowercase(const char *str) {
  char *n, *p;

  if (str == NULL) {
    return NULL;
  }

  n = strdup(str);
  if (n == NULL) {
    return NULL;
  }

  for (p = n; *p; p++) {
    *p = tolower(*p);
  }

  return n;
}

/*
 * Returns true if the given string matches the pattern (which may contain ?
 * and * as wildcards), and zero if it does not match.
 */
static int match_pattern(const char *s, const char *pattern) {
  if (s == NULL || pattern == NULL) {
    return 0;
  }

  for (;;) {
    /* If at end of pattern, accept if also at end of string. */
    if (*pattern == '\0') {
      return (*s == '\0');
    }

    if (*pattern == '*') {
      /* Skip the asterisk. */
      pattern++;

      /* If at end of pattern, accept immediately. */
      if (!*pattern) return 1;

      /* If next character in pattern is known, optimize. */
      if (*pattern != '?' && *pattern != '*') {
        /*
         * Look instances of the next character in
         * pattern, and try to match starting from
         * those.
         */
        for (; *s; s++)
          if (*s == *pattern && match_pattern(s + 1, pattern + 1)) {
            return 1;
          }
        /* Failed. */
        return 0;
      }
      /*
       * Move ahead one character at a time and try to
       * match at each position.
       */
      for (; *s; s++) {
        if (match_pattern(s, pattern)) {
          return 1;
        }
      }
      /* Failed. */
      return 0;
    }
    /*
     * There must be at least one more character in the string.
     * If we are at the end, fail.
     */
    if (!*s) {
      return 0;
    }

    /* Check if the next character of the string is acceptable. */
    if (*pattern != '?' && *pattern != *s) {
      return 0;
    }

    /* Move to the next character, both in string and in pattern. */
    s++;
    pattern++;
  }

  /* NOTREACHED */
  return 0;
}

/*
 * Tries to match the string against the comma-separated sequence of subpatterns
 * (each possibly preceded by ! to indicate negation).
 * Returns -1 if negation matches, 1 if there is a positive match, 0 if there is
 * no match at all.
 */
static int match_pattern_list(const char *string, const char *pattern,
                              unsigned int len, int dolower) {
  char sub[1024];
  int negated;
  int got_positive;
  unsigned int i, subi;

  got_positive = 0;
  for (i = 0; i < len;) {
    /* Check if the subpattern is negated. */
    if (pattern[i] == '!') {
      negated = 1;
      i++;
    } else {
      negated = 0;
    }

    /*
     * Extract the subpattern up to a comma or end.  Convert the
     * subpattern to lowercase.
     */
    for (subi = 0; i < len && subi < sizeof(sub) - 1 && pattern[i] != ',';
         subi++, i++) {
      sub[subi] = dolower && isupper(pattern[i]) ? (char)tolower(pattern[i])
                                                 : pattern[i];
    }

    /* If subpattern too long, return failure (no match). */
    if (subi >= sizeof(sub) - 1) {
      return 0;
    }

    /* If the subpattern was terminated by a comma, skip the comma. */
    if (i < len && pattern[i] == ',') {
      i++;
    }

    /* Null-terminate the subpattern. */
    sub[subi] = '\0';

    /* Try to match the subpattern against the string. */
    if (match_pattern(string, sub)) {
      if (negated) {
        return -1; /* Negative */
      } else {
        got_positive = 1; /* Positive */
      }
    }
  }

  /*
   * Return success if got a positive match.  If there was a negative
   * match, we have already returned -1 and never get here.
   */
  return got_positive;
}

/*
 * Tries to match the host name (which must be in all lowercase) against the
 * comma-separated sequence of subpatterns (each possibly preceded by ! to
 * indicate negation).
 * Returns -1 if negation matches, 1 if there is a positive match, 0 if there
 * is no match at all.
 */
int match_hostname(const char *host, const char *pattern, unsigned int len) {
  return match_pattern_list(host, pattern, len, 1);
}

/**
 * @brief Expand a directory starting with a tilde '~'
 *
 * @param[in]  d        The directory to expand.
 *
 * @return              The expanded directory, NULL on error.
 */
char *ssh_path_expand_tilde(const char *d) {
  char *h = NULL, *r;
  const char *p;
  size_t ld;
  size_t lh = 0;

  if (d[0] != '~') {
    return strdup(d);
  }
  d++;

  /* handle ~user/path */
  p = strchr(d, '/');
  if (p != NULL && p > d) {
    struct passwd *pw;
    size_t s = p - d;
    char u[128];

    if (s >= sizeof(u)) {
      return NULL;
    }
    memcpy(u, d, s);
    u[s] = '\0';
    pw = getpwnam(u);
    if (pw == NULL) {
      return NULL;
    }
    ld = strlen(p);
    h = strdup(pw->pw_dir);
  } else {
    ld = strlen(d);
    p = (char *)d;
    h = ssh_get_user_home_dir();
  }
  if (h == NULL) {
    return NULL;
  }
  lh = strlen(h);

  r = static_cast<char *>(malloc(ld + lh + 1));
  if (r == NULL) {
    SAFE_FREE(h);
    return NULL;
  }

  if (lh > 0) {
    memcpy(r, h, lh);
  }
  SAFE_FREE(h);
  memcpy(r + lh, p, ld + 1);

  return r;
}

char *ssh_path_expand_escape(struct Options *options, const char *s) {
  char host[NI_MAXHOST];
  char buf[MAX_BUF_SIZE];
  char *r, *x = NULL;
  const char *p;
  size_t i, l;

  r = ssh_path_expand_tilde(s);
  if (r == NULL) {
    cout << "error" << endl;
    return NULL;
  }

  if (strlen(r) > MAX_BUF_SIZE) {
    cout << "string to expand too long" << endl;
    free(r);
    return NULL;
  }

  p = r;
  buf[0] = '\0';

  for (i = 0; *p != '\0'; p++) {
    if (*p != '%') {
      buf[i] = *p;
      i++;
      if (i >= MAX_BUF_SIZE) {
        free(r);
        return NULL;
      }
      buf[i] = '\0';
      continue;
    }

    p++;
    if (*p == '\0') {
      break;
    }

    switch (*p) {
      case 'd':
        x = strdup(options->sshdir);
        break;
      case 'u':
        x = ssh_get_local_username();
        break;
      case 'l':
        if (gethostname(host, sizeof(host) == 0)) {
          x = strdup(host);
        }
        break;
      case 'h':
        x = strdup(options->host);
        break;
      case 'r':
        x = strdup(options->username);
        break;
      case 'p':
        if (options->port < 65536) {
          char tmp[6];

          snprintf(tmp, sizeof(tmp), "%u", options->port);
          x = strdup(tmp);
        }
        break;
      default:
        cout << "Wrong escape sequence detected" << endl;
        free(r);
        return NULL;
    }

    if (x == NULL) {
      cout << "error" << endl;
      free(r);
      return NULL;
    }

    i += strlen(x);
    if (i >= MAX_BUF_SIZE) {
      cout << "String too long" << endl;
      free(x);
      free(r);
      return NULL;
    }
    l = strlen(buf);
    strncpy(buf + l, x, sizeof(buf) - l - 1);
    buf[i] = '\0';
    SAFE_FREE(x);
  }

  free(r);
  return strdup(buf);
#undef MAX_BUF_SIZE
}

// ssh_options_set
/**
 * @brief This function can set all possible ssh options.
 *
 * @param  session An allocated SSH session structure.
 *
 * @param  type The option type to set. This could be one of the
 *              following:
 *
 *              - SSH_OPTIONS_HOST:
 *                The hostname or ip address to connect to (const char *).
 *
 *              - SSH_OPTIONS_PORT:
 *                The port to connect to (unsigned int).
 *
 *              - SSH_OPTIONS_PORT_STR:
 *                The port to connect to (const char *).
 *
 *              - SSH_OPTIONS_FD:
 *                The file descriptor to use (socket_t).\n
 *                \n
 *                If you wish to open the socket yourself for a reason
 *                or another, set the file descriptor. Don't forget to
 *                set the hostname as the hostname is used as a key in
 *                the known_host mechanism.
 *
 *              - SSH_OPTIONS_BINDADDR:
 *                The address to bind the client to (const char *).
 *
 *              - SSH_OPTIONS_USER:
 *                The username for authentication (const char *).\n
 *                \n
 *                If the value is NULL, the username is set to the
 *                default username.
 *
 *              - SSH_OPTIONS_SSH_DIR:
 *                Set the ssh directory (const char *,format string).\n
 *                \n
 *                If the value is NULL, the directory is set to the
 *                default ssh directory.\n
 *                \n
 *                The ssh directory is used for files like known_hosts
 *                and identity (private and public key). It may include
 *                "%s" which will be replaced by the user home
 *                directory.
 *
 *              - SSH_OPTIONS_KNOWNHOSTS:
 *                Set the known hosts file name (const char *,format string).\n
 *                \n
 *                If the value is NULL, the directory is set to the
 *                default known hosts file, normally
 *                ~/.ssh/known_hosts.\n
 *                \n
 *                The known hosts file is used to certify remote hosts
 *                are genuine. It may include "%s" which will be
 *                replaced by the user home directory.
 *
 *              - SSH_OPTIONS_IDENTITY:
 *                Set the identity file name (const char *,format string).\n
 *                \n
 *                By default identity, id_dsa and id_rsa are checked.\n
 *                \n
 *                The identity file used authenticate with public key.
 *                It may include "%s" which will be replaced by the
 *                user home directory.
 *
 *              - SSH_OPTIONS_TIMEOUT:
 *                Set a timeout for the connection in seconds (long).
 *
 *              - SSH_OPTIONS_TIMEOUT_USEC:
 *                Set a timeout for the connection in micro seconds
 *                        (long).
 *
 *              - SSH_OPTIONS_SSH1:
 *                Allow or deny the connection to SSH1 servers
 *                (int, 0 is false).
 *
 *              - SSH_OPTIONS_SSH2:
 *                Allow or deny the connection to SSH2 servers
 *                (int, 0 is false).
 *
 *              - SSH_OPTIONS_LOG_VERBOSITY:
 *                Set the session logging verbosity (int).\n
 *                \n
 *                The verbosity of the messages. Every log smaller or
 *                equal to verbosity will be shown.
 *                - SSH_LOG_NOLOG: No logging
 *                - SSH_LOG_RARE: Rare conditions or warnings
 *                - SSH_LOG_ENTRY: API-accessible entrypoints
 *                - SSH_LOG_PACKET: Packet id and size
 *                - SSH_LOG_FUNCTIONS: Function entering and leaving
 *
 *              - SSH_OPTIONS_LOG_VERBOSITY_STR:
 *                Set the session logging verbosity (const char *).\n
 *                \n
 *                The verbosity of the messages. Every log smaller or
 *                equal to verbosity will be shown.
 *                - SSH_LOG_NOLOG: No logging
 *                - SSH_LOG_RARE: Rare conditions or warnings
 *                - SSH_LOG_ENTRY: API-accessible entrypoints
 *                - SSH_LOG_PACKET: Packet id and size
 *                - SSH_LOG_FUNCTIONS: Function entering and leaving
 *                \n
 *                See the corresponding numbers in libssh.h.
 *
 *              - SSH_OPTIONS_AUTH_CALLBACK:
 *                Set a callback to use your own authentication function
 *                (function pointer).
 *
 *              - SSH_OPTIONS_AUTH_USERDATA:
 *                Set the user data passed to the authentication
 *                function (generic pointer).
 *
 *              - SSH_OPTIONS_LOG_CALLBACK:
 *                Set a callback to use your own logging function
 *                (function pointer).
 *
 *              - SSH_OPTIONS_LOG_USERDATA:
 *                Set the user data passed to the logging function
 *                (generic pointer).
 *
 *              - SSH_OPTIONS_STATUS_CALLBACK:
 *                Set a callback to show connection status in realtime
 *                (function pointer).\n
 *                \n
 *                @code
 *                fn(void *arg, float status)
 *                @endcode
 *                \n
 *                During ssh_connect(), libssh will call the callback
 *                with status from 0.0 to 1.0.
 *
 *              - SSH_OPTIONS_STATUS_ARG:
 *                Set the status argument which should be passed to the
 *                status callback (generic pointer).
 *
 *              - SSH_OPTIONS_CIPHERS_C_S:
 *                Set the symmetric cipher client to server (const char *,
 *                comma-separated list).
 *
 *              - SSH_OPTIONS_CIPHERS_S_C:
 *                Set the symmetric cipher server to client (const char *,
 *                comma-separated list).
 *
 *              - SSH_OPTIONS_KEY_EXCHANGE:
 *                Set the key exchange method to be used (const char *,
 *                comma-separated list). ex:
 *                "ecdh-sha2-nistp256,diffie-hellman-group14-sha1,diffie-hellman-group1-sha1"
 *
 *              - SSH_OPTIONS_HOSTKEYS:
 *                Set the preferred server host key types (const char *,
 *                comma-separated list). ex:
 *                "ssh-rsa,ssh-dss,ecdh-sha2-nistp256"
 *
 *              - SSH_OPTIONS_COMPRESSION_C_S:
 *                Set the compression to use for client to server
 *                communication (const char *, "yes", "no" or a specific
 *                algorithm name if needed ("zlib","zlib@openssh.com","none").
 *
 *              - SSH_OPTIONS_COMPRESSION_S_C:
 *                Set the compression to use for server to client
 *                communication (const char *, "yes", "no" or a specific
 *                algorithm name if needed ("zlib","zlib@openssh.com","none").
 *
 *              - SSH_OPTIONS_COMPRESSION:
 *                Set the compression to use for both directions
 *                communication (const char *, "yes", "no" or a specific
 *                algorithm name if needed ("zlib","zlib@openssh.com","none").
 *
 *              - SSH_OPTIONS_COMPRESSION_LEVEL:
 *                Set the compression level to use for zlib functions. (int,
 *                value from 1 to 9, 9 being the most efficient but slower).
 *
 *              - SSH_OPTIONS_STRICTHOSTKEYCHECK:
 *                Set the parameter StrictHostKeyChecking to avoid
 *                asking about a fingerprint (int, 0 = false).
 *
 *              - SSH_OPTIONS_PROXYCOMMAND:
 *                Set the command to be executed in order to connect to
 *                server (const char *).
 *
 *              - SSH_OPTIONS_GSSAPI_SERVER_IDENTITY
 *                Set it to specify the GSSAPI server identity that libssh
 *                should expect when connecting to the server (const char *).
 *
 *              - SSH_OPTIONS_GSSAPI_CLIENT_IDENTITY
 *                Set it to specify the GSSAPI client identity that libssh
 *                should expect when connecting to the server (const char *).
 *
 *              - SSH_OPTIONS_GSSAPI_DELEGATE_CREDENTIALS
 *                Set it to specify that GSSAPI should delegate credentials
 *                to the server (int, 0 = false).
 *
 * @param  value The value to set. This is a generic pointer and the
 *               datatype which is used should be set according to the
 *               type set.
 *
 * @return       0 on success, < 0 on error.
 */
int ssh_options_set(struct Options *options, enum ssh_options_e type,
                    const void *value) {
  const char *v;
  char *p, *q;
  long int i;
  int rc;

  if (options == NULL) {
    return -1;
  }

  switch (type) {
    case SSH_OPTIONS_HOST:
      v = static_cast<const char *>(value);
      if (v == NULL || v[0] == '\0') {
        cout << "invalid error" << endl;
        return -1;
      } else {
        q = strdup(static_cast<const char *>(value));
        if (q == NULL) {
          cout << "error" << endl;
          return -1;
        }
        p = strchr(q, '@');

        if (options->host) SAFE_FREE(options->host);

        if (p) {
          *p = '\0';
          options->host = strdup(p + 1);
          if (options->host == NULL) {
            SAFE_FREE(q);
            cout << "error" << endl;
            return -1;
          }

          SAFE_FREE(options->username);
          options->username = strdup(q);
          SAFE_FREE(q);
          if (options->username == NULL) {
            cout << "error" << endl;
            return -1;
          }
        } else {
          options->host = q;
        }
      }
      break;
    case SSH_OPTIONS_PORT:
      if (value == NULL) {
        cout << "invalid error" << endl;
        return -1;
      } else {
        int *x = (int *)value;
        if (*x <= 0) {
          cout << "invalid error" << endl;
          return -1;
        }

        options->port = *x & 0xffff;
      }
      break;
    case SSH_OPTIONS_PORT_STR:
      v = static_cast<const char *>(value);
      if (v == NULL || v[0] == '\0') {
        cout << "invalid error" << endl;
        return -1;
      } else {
        q = strdup(v);
        if (q == NULL) {
          cout << "error" << endl;
          return -1;
        }
        i = strtol(q, &p, 10);
        if (q == p) {
          SAFE_FREE(q);
        }
        SAFE_FREE(q);
        if (i <= 0) {
          cout << "invalid error" << endl;
          return -1;
        }

        options->port = i & 0xffff;
      }
      break;
    case SSH_OPTIONS_USER:
      v = static_cast<const char *>(value);
      SAFE_FREE(options->username);
      if (v == NULL) {
        q = ssh_get_local_username();
        if (q == NULL) {
          cout << "error" << endl;
          return -1;
        }
        options->username = q;
      } else if (v[0] == '\0') {
        cout << "invalid error" << endl;
        return -1;
      } else { /* username provided */
        options->username = strdup(static_cast<const char *>(value));
        if (options->username == NULL) {
          cout << "error" << endl;
          return -1;
        }
      }
      break;
    case SSH_OPTIONS_PROXYJUMP:
      v = static_cast<const char *>(value);
      SAFE_FREE(options->ProxyJump);
      if (v == NULL || v[0] == '\0') {
        cout << "invalid error" << endl;
        return -1;
      } else { /* ProxyJump provided */
        options->ProxyJump = strdup(static_cast<const char *>(value));
        if (options->ProxyJump == NULL) {
          cout << "error" << endl;
          return -1;
        }
      }
      break;
    case SSH_OPTIONS_KNOWNHOSTS:
      v = static_cast<const char *>(value);
      SAFE_FREE(options->knownhosts);
      if (v == NULL) {
        options->knownhosts = ssh_path_expand_escape(options, "%d/known_hosts");
        if (options->knownhosts == NULL) {
          cout << "error" << endl;
          return -1;
        }
      } else if (v[0] == '\0') {
        cout << "invalid error" << endl;
        return -1;
      } else {
        options->knownhosts = strdup(v);
        if (options->knownhosts == NULL) {
          cout << "error" << endl;
          return -1;
        }
      }
      break;
    case SSH_OPTIONS_TIMEOUT:
      if (value == NULL) {
        cout << "invalid error" << endl;
        return -1;
      } else {
        long *x = (long *)value;
        if (*x < 0) {
          cout << "invalid error" << endl;
          return -1;
        }

        options->timeout = *x & 0xffffffff;
      }
      break;
    case SSH_OPTIONS_SSH1:
      if (value == NULL) {
        cout << "invalid error" << endl;
        return -1;
      } else {
        int *x = (int *)value;
        if (*x < 0) {
          cout << "invalid error" << endl;
          return -1;
        }

        options->ssh1 = *x;
      }
      break;
    case SSH_OPTIONS_SSH2:
      if (value == NULL) {
        cout << "invalid error" << endl;
        return -1;
      } else {
        int *x = (int *)value;
        if (*x < 0) {
          cout << "invalid error" << endl;
          return -1;
        }

        options->ssh2 = *x & 0xffff;
      }
      break;
    case SSH_OPTIONS_STRICTHOSTKEYCHECK:
      if (value == NULL) {
        cout << "invalid error" << endl;
        return -1;
      } else {
        int *x = (int *)value;

        options->StrictHostKeyChecking = (*x & 0xff) > 0 ? 1 : 0;
      }
      options->StrictHostKeyChecking = *(int *)value;
      break;
    case SSH_OPTIONS_PROXYCOMMAND:
      v = static_cast<const char *>(value);
      if (v == NULL || v[0] == '\0') {
        cout << "invalid error" << endl;
        return -1;
      } else {
        SAFE_FREE(options->ProxyCommand);
        /* Setting the command to 'none' disables this option. */
        rc = strcasecmp(v, "none");
        if (rc != 0) {
          q = strdup(v);
          if (q == NULL) {
            return -1;
          }
          options->ProxyCommand = q;
        }
      }
      break;
    case SSH_OPTIONS_GSSAPI_SERVER_IDENTITY:
      v = static_cast<const char *>(value);
      if (v == NULL || v[0] == '\0') {
        cout << "invalid error" << endl;
        return -1;
      } else {
        SAFE_FREE(options->gss_server_identity);
        options->gss_server_identity = strdup(v);
        if (options->gss_server_identity == NULL) {
          cout << "error" << endl;
          return -1;
        }
      }
      break;
    case SSH_OPTIONS_GSSAPI_CLIENT_IDENTITY:
      v = static_cast<const char *>(value);
      if (v == NULL || v[0] == '\0') {
        cout << "invalid error" << endl;
        return -1;
      } else {
        SAFE_FREE(options->gss_client_identity);
        options->gss_client_identity = strdup(v);
        if (options->gss_client_identity == NULL) {
          cout << "error" << endl;
          return -1;
        }
      }
      break;
    case SSH_OPTIONS_GSSAPI_DELEGATE_CREDENTIALS:
      if (value == NULL) {
        cout << "invalid error" << endl;
        return -1;
      } else {
        int x = *(int *)value;

        options->gss_delegate_creds = (x & 0xff);
      }
      break;

    default:
      cout << "Unknown ssh option" << endl;
      return -1;
      break;
  }

  return 0;
}

static char *ssh_config_get_cmd(char **str) {
  register char *c;
  char *r;

  /* Ignore leading spaces */
  for (c = *str; *c; c++) {
    if (!isblank(*c)) {
      break;
    }
  }

  if (*c == '\"') {
    for (r = ++c; *c; c++) {
      if (*c == '\"') {
        *c = '\0';
        goto out;
      }
    }
  }

  for (r = c; *c; c++) {
    if (*c == '\n') {
      *c = '\0';
      goto out;
    }
  }

out:
  *str = c + 1;

  return r;
}

static char *ssh_config_get_token(char **str) {
  register char *c;
  char *r;

  c = ssh_config_get_cmd(str);

  for (r = c; *c; c++) {
    if (isblank(*c) || *c == '=') {
      *c = '\0';
      goto out;
    }
  }

out:
  *str = c + 1;

  return r;
}

static int ssh_config_get_int(char **str, int notfound) {
  char *p, *endp;
  int i;

  p = ssh_config_get_token(str);
  if (p && *p) {
    i = strtol(p, &endp, 10);
    if (p == endp) {
      return notfound;
    }
    return i;
  }

  return notfound;
}

static const char *ssh_config_get_str_tok(char **str, const char *def) {
  char *p;

  p = ssh_config_get_token(str);
  if (p && *p) {
    return p;
  }

  return def;
}

static int ssh_config_get_yesno(char **str, int notfound) {
  const char *p;

  p = ssh_config_get_str_tok(str, NULL);
  if (p == NULL) {
    return notfound;
  }

  if (strncasecmp(p, "yes", 3) == 0) {
    return 1;
  } else if (strncasecmp(p, "no", 2) == 0) {
    return 0;
  }

  return notfound;
}

static void local_parse_file(struct Options *options, const char *filename,
                             int *parsing, int seen[]) {
  ifstream local_config_file(filename);
  string line;
  unsigned int count = 0;

  if (!local_config_file) {
    cout << "Can't open file" << endl;
    return;
  }

  while (getline(local_config_file, line)) {
    count++;
    if (ssh_config_parse_line(options, line.c_str(), count, parsing, seen) <
        0) {
      local_config_file.close();
      return;
    }
  }
  local_config_file.close();
  return;
}

static int ssh_config_parse_line(struct Options *options, const char *line,
                                 unsigned int count, int *parsing, int seen[]) {
  enum ssh_config_opcode_e opcode;
  const char *p;
  char *s, *x;
  char *keyword;
  char *lowerhost;
  size_t len;
  int i;

  x = s = strdup(line);
  if (s == NULL) {
    cout << "error reading file" << endl;
    return -1;
  }

  /* Remove trailing spaces */
  for (len = strlen(s) - 1; len > 0; len--) {
    if (!isspace(s[len])) {
      break;
    }
    s[len] = '\0';
  }

  keyword = ssh_config_get_token(&s);
  if (keyword == NULL || *keyword == '#' || *keyword == '\0' ||
      *keyword == '\n') {
    SAFE_FREE(x);
    return 0;
  }

  opcode = ssh_config_get_opcode(keyword);
  if (*parsing == 1 && opcode != SOC_HOST && opcode != SOC_UNSUPPORTED &&
      opcode != SOC_INCLUDE) {
    if (seen[opcode] != 0) {
      SAFE_FREE(x);
      return 0;
    }
    seen[opcode] = 1;
  }

  switch (opcode) {
    case SOC_INCLUDE: /* recursive include of other files */

      p = ssh_config_get_str_tok(&s, NULL);
      if (p && *parsing) {
        local_parse_file(options, p, parsing, seen);
      }
      break;
    case SOC_HOST: {
      int ok = 0;

      *parsing = 0;
      lowerhost = (options->host) ? ssh_lowercase(options->host) : NULL;
      for (p = ssh_config_get_str_tok(&s, NULL); p != NULL && p[0] != '\0';
           p = ssh_config_get_str_tok(&s, NULL)) {
        if (ok >= 0) {
          ok = match_hostname(lowerhost, p, strlen(p));
          if (ok < 0) {
            *parsing = 0;
          } else if (ok > 0) {
            *parsing = 1;
          }
        }
      }
      SAFE_FREE(lowerhost);
      break;
    }
    case SOC_HOSTNAME:
      p = ssh_config_get_str_tok(&s, NULL);
      if (p && *parsing) {
        char *z = ssh_path_expand_escape(options, p);
        if (z == NULL) {
          z = strdup(p);
        }
        ssh_options_set(options, SSH_OPTIONS_HOST, z);
        free(z);
      }
      break;
    case SOC_PORT:
      if (options->port == 0) {
        p = ssh_config_get_str_tok(&s, NULL);
        if (p && *parsing) {
          ssh_options_set(options, SSH_OPTIONS_PORT_STR, p);
        }
      }
      break;
    case SOC_USERNAME:
      if (options->username == NULL) {
        p = ssh_config_get_str_tok(&s, NULL);
        if (p && *parsing) {
          ssh_options_set(options, SSH_OPTIONS_USER, p);
        }
      }
      break;
    case SOC_PROXYJUMP:
      if (options->ProxyJump == NULL) {
        p = ssh_config_get_str_tok(&s, NULL);
        if (p && *parsing) {
          ssh_options_set(options, SSH_OPTIONS_PROXYJUMP, p);
        }
      }
      break;
    case SOC_PROTOCOL:
      p = ssh_config_get_str_tok(&s, NULL);
      if (p && *parsing) {
        char *a, *b;
        b = strdup(p);
        if (b == NULL) {
          SAFE_FREE(x);
          cout << "error" << endl;
          return -1;
        }
        i = 0;
        ssh_options_set(options, SSH_OPTIONS_SSH1, &i);
        ssh_options_set(options, SSH_OPTIONS_SSH2, &i);

        for (a = strtok(b, ","); a; a = strtok(NULL, ",")) {
          switch (atoi(a)) {
            case 1:
              i = 1;
              ssh_options_set(options, SSH_OPTIONS_SSH1, &i);
              break;
            case 2:
              i = 1;
              ssh_options_set(options, SSH_OPTIONS_SSH2, &i);
              break;
            default:
              break;
          }
        }
        SAFE_FREE(b);
      }
      break;
    case SOC_TIMEOUT:
      i = ssh_config_get_int(&s, -1);
      if (i >= 0 && *parsing) {
        ssh_options_set(options, SSH_OPTIONS_TIMEOUT, &i);
      }
      break;
    case SOC_STRICTHOSTKEYCHECK:
      i = ssh_config_get_yesno(&s, -1);
      if (i >= 0 && *parsing) {
        ssh_options_set(options, SSH_OPTIONS_STRICTHOSTKEYCHECK, &i);
      }
      break;
    case SOC_KNOWNHOSTS:
      p = ssh_config_get_str_tok(&s, NULL);
      if (p && *parsing) {
        ssh_options_set(options, SSH_OPTIONS_KNOWNHOSTS, p);
      }
      break;
    case SOC_PROXYCOMMAND:
      p = ssh_config_get_cmd(&s);
      if (p && *parsing) {
        ssh_options_set(options, SSH_OPTIONS_PROXYCOMMAND, p);
      }
      break;
    case SOC_GSSAPISERVERIDENTITY:
      p = ssh_config_get_str_tok(&s, NULL);
      if (p && *parsing) {
        ssh_options_set(options, SSH_OPTIONS_GSSAPI_SERVER_IDENTITY, p);
      }
      break;
    case SOC_GSSAPICLIENTIDENTITY:
      p = ssh_config_get_str_tok(&s, NULL);
      if (p && *parsing) {
        ssh_options_set(options, SSH_OPTIONS_GSSAPI_CLIENT_IDENTITY, p);
      }
      break;
    case SOC_GSSAPIDELEGATECREDENTIALS:
      i = ssh_config_get_yesno(&s, -1);
      if (i >= 0 && *parsing) {
        ssh_options_set(options, SSH_OPTIONS_GSSAPI_DELEGATE_CREDENTIALS, &i);
      }
      break;
    case SOC_UNSUPPORTED:
      LOG(INFO) << "unsupported config line: " << string(line) << ", ignored"
                << endl;
      break;
    default:
      cout << "parse error" << endl;
      SAFE_FREE(x);
      return -1;
      break;
  }

  SAFE_FREE(x);
  return 0;
}

int parse_ssh_config_file(struct Options *options, string filename) {
  string line;
  unsigned int count = 0;
  int parsing;
  int seen[SOC_END - SOC_UNSUPPORTED] = {0};

  ifstream config_file(filename.c_str());
  parsing = 1;
  while (getline(config_file, line)) {
    count++;
    if (ssh_config_parse_line(options, line.c_str(), count, &parsing, seen) <
        0) {
      config_file.close();
      return -1;
    }
  }
  config_file.close();
  return 0;
}

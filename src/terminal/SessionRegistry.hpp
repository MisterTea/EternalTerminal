#ifndef __ET_SESSION_REGISTRY_HPP__
#define __ET_SESSION_REGISTRY_HPP__

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Headers.hpp"

namespace et {

/**
 * @brief Unified model for session identity, attachment time, and expiry.
 *
 * Coordinates session identity (id/name), last-attachment timestamp, and
 * expiry lifetime in one place so that listing, introspection, and cleanup
 * do not require separate ad-hoc mechanisms. Addresses issue #428.
 */
struct SessionEntry {
  std::string id;
  std::string name;
  std::chrono::system_clock::time_point lastAttachTime;
  bool attached = false;
  std::chrono::seconds expiry;

  bool isExpired() const {
    if (expiry.count() <= 0) return false;  // no expiry
    auto now = std::chrono::system_clock::now();
    return (now - lastAttachTime) > expiry;
  }
};

class SessionRegistry {
 public:
  explicit SessionRegistry(std::chrono::seconds defaultExpiry = std::chrono::seconds(0));

  /// Register or refresh a session. Updates attachment time if re-attached.
  void attach(const std::string& id, const std::string& name,
              std::chrono::seconds expiry = std::chrono::seconds(0));

  /// Detach: mark session as unattached but still tracked for listing.
  void detach(const std::string& id);

  /// Remove a session entirely.
  bool remove(const std::string& id);

  /// Get a session entry by ID (nullptr if not found).
  std::shared_ptr<SessionEntry> get(const std::string& id) const;

  /// List all sessions, optionally filtered by attached/unattached state.
  std::vector<std::shared_ptr<SessionEntry>>
  list(bool onlyUnattached = false) const;

  /// List only sessions whose expiry has elapsed.
  std::vector<std::shared_ptr<SessionEntry>> listExpired() const;

  /// Purge all expired sessions. Returns number purged.
  size_t purgeExpired();

  /// Set default expiry for future sessions.
  void setDefaultExpiry(std::chrono::seconds expiry);

  /// Get the default expiry.
  std::chrono::seconds getDefaultExpiry() const;

  /// Number of tracked sessions.
  size_t size() const;

  /// Check if a session exists.
  bool exists(const std::string& id) const;

 private:
  std::unordered_map<std::string, std::shared_ptr<SessionEntry>> sessions_;
  mutable std::recursive_mutex mutex_;
  std::chrono::seconds defaultExpiry_;
};

}  // namespace et

#endif  // __ET_SESSION_REGISTRY_HPP__

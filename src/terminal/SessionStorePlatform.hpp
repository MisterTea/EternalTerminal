#ifndef __ET_SESSION_STORE_PLATFORM__
#define __ET_SESSION_STORE_PLATFORM__

#include <filesystem>

#include "Headers.hpp"

namespace et {
/**
 * @brief File primitives behind SessionStore.
 *
 * The Unix backend (SessionStoreUnix.cpp) refuses files and directories with
 * unsafe owners, modes or links because they hold passkeys. The Windows
 * backend (SessionStoreWindows.cpp) uses plain std::filesystem.
 */
namespace session_storage {
namespace fs = std::filesystem;

/** @brief Creates @p path if needed and makes it owner-only. */
void ensureDir(const fs::path& path);

/**
 * @brief Throws unless @p sessionsPath and its parent are safe to trust.
 *
 * @param allowMissing Accept directories that do not exist yet.
 */
void verifySessionDirectories(const fs::path& sessionsPath, bool allowMissing);

/**
 * @brief Reads a session file.
 *
 * @param name Session name, for log messages.
 * @param lastSeenAt Receives the file's mtime in seconds since the epoch.
 * @return The file contents, or nullopt if it is missing or unsafe.
 */
optional<string> readSessionFile(const fs::path& path, const string& name,
                                 int64_t* lastSeenAt);

/**
 * @brief Writes @p contents to @p tmpPath and moves it to @p finalPath.
 *
 * @param replaceExisting When false, fail instead of replacing an existing
 *   @p finalPath.
 */
void writeSessionFile(const fs::path& tmpPath, const fs::path& finalPath,
                      const string& contents, bool replaceExisting);

/** @brief Updates a session file's mtime. @return false on failure. */
bool touchSessionFile(const fs::path& path);

/** @brief Deletes a session file; a missing file is not an error. */
void deleteSessionFile(const fs::path& path);
}  // namespace session_storage
}  // namespace et

#endif  // __ET_SESSION_STORE_PLATFORM__

#ifndef __ET_SECURE_FILE_UNIX__
#define __ET_SECURE_FILE_UNIX__

#ifndef WIN32
#include "Headers.hpp"

namespace et::private_storage {
// Walk directory components through descriptors. Reject other users' writable
// ancestors, planted links, and unsafe owners. Group write is allowed only
// through the owner's user private group. Existing private directories
// may lose read/execute permissions for other users, but writable ones fail.
void ensureDirectory(const fs::path& path);

// Files must be regular, owned by the effective user, singly linked, and not
// writable by group/other. Reads and writes use the verified descriptor;
// permissions become 0600. Missing reads return nullopt.
optional<string> readFile(const fs::path& path, size_t maxBytes);
bool createFile(const fs::path& path, const string& contents);
void replaceFile(const fs::path& path, const string& contents);
}  // namespace et::private_storage
#endif

#endif  // __ET_SECURE_FILE_UNIX__

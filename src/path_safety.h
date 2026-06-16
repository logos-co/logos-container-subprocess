#ifndef LOGOS_PATH_SAFETY_H
#define LOGOS_PATH_SAFETY_H

// Qt-free helper for validating that an untrusted string is a single,
// safe filesystem path segment before it is used to derive a path.
//
// Module names originate from untrusted plugin JSON metadata
// (logos-module module_metadata.cpp sets name straight from the "name"
// field; isValid() only checks non-empty). That name is propagated
// verbatim into filesystem/socket path sinks:
//   - the subprocess token socket: "logos_token_" + name, resolved by
//     ::logos::unixSocketPath() (tempdir + "/" + name, no validation)
//     and fed to QLocalServer::removeServer()/listen() — removeServer()
//     UNLINKS the file at the resolved path.
//   - instance persistence: basePath + "/" + name + "/" + instanceId.
//
// Without a guard, a crafted name like "x/../../victim" escapes the
// intended directory and lets removeServer() delete (or listen() clobber)
// an attacker-chosen file with the host process's privileges. Rejecting
// any name that is not a single safe path segment closes every such sink.
//
// This mirrors the policy already enforced for the persistence sink
// (logos-module InstancePersistence::isValidPathSegment), kept here as a
// single shared definition so both sinks agree.

#include <cstddef>
#include <string>

namespace logos {

// Returns true iff `name` is a safe single path segment: non-empty, not
// over 255 bytes, not "." or "..", and free of path separators ('/', '\\')
// and embedded NUL bytes. Anything else can escape the directory the
// segment is meant to live in and must be rejected at the trust boundary.
inline bool isSafePathSegment(const std::string& name) {
    if (name.empty() || name.size() > 255)
        return false;
    if (name == "." || name == "..")
        return false;
    if (name.find_first_of("/\\") != std::string::npos)
        return false;
    if (name.find('\0') != std::string::npos)
        return false;
    return true;
}

}  // namespace logos

#endif  // LOGOS_PATH_SAFETY_H

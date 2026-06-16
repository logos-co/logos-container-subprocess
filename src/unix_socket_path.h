#ifndef LOGOS_UNIX_SOCKET_PATH_H
#define LOGOS_UNIX_SOCKET_PATH_H

// Qt-free helpers for resolving the temp directory and constructing a
// Unix-domain socket path. Used on both sides of the parent/child
// token handoff: SubprocessManager (parent, no Qt) computes the path
// to connect() to, and QtTokenReceiver (child, Qt) computes the same
// path and hands it to QLocalServer::listen() as an absolute path —
// bypassing Qt's QDir::tempPath() resolution so the two sides cannot
// disagree.
//
// The mismatch we are guarding against is acutely visible under
// `nix run`, which spawns the parent with $TMPDIR unset. A naive
// getenv("TMPDIR") on macOS falls back to /tmp/<name> while Qt
// resolves /var/folders/.../T/<name> via
// confstr(_CS_DARWIN_USER_TEMP_DIR), causing every module's
// subprocess token-handoff to time out.
//
// We mirror Qt's resolution order so child and parent agree:
//   1. $TMPDIR if set (Linux + macOS)
//   2. macOS: confstr(_CS_DARWIN_USER_TEMP_DIR) — per-user dir,
//      always set on Apple platforms even when env is empty
//   3. /tmp fallback

#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace logos {

inline std::string qtCompatibleTempDir() {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir;
    if (tmp && tmp[0]) {
        dir = tmp;
    } else {
#ifdef __APPLE__
        // confstr returns the buffer length INCLUDING the null
        // terminator on success; 0 on error. Probe with a zero-sized
        // buffer first to learn the exact size — Apple's per-user
        // temp dir lives under /var/folders/<2>/<28>/T/ and can
        // exceed PATH_MAX on some configurations, so a fixed-size
        // stack buffer is unsafe (would silently fall back to /tmp
        // and reintroduce the parent/child path mismatch this
        // helper is meant to prevent).
        size_t n = ::confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
        if (n > 0) {
            std::vector<char> buf(n);
            size_t copied = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buf.data(), buf.size());
            if (copied > 0 && copied <= buf.size()) {
                dir.assign(buf.data(), copied - 1);  // drop trailing NUL
            } else {
                dir = "/tmp";
            }
        } else {
            dir = "/tmp";
        }
#else
        dir = "/tmp";
#endif
    }
    while (!dir.empty() && dir.back() == '/') dir.pop_back();
    return dir;
}

inline std::string unixSocketPath(const std::string& name) {
    return qtCompatibleTempDir() + "/" + name;
}

}  // namespace logos

#endif  // LOGOS_UNIX_SOCKET_PATH_H

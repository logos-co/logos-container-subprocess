#ifndef LOGOS_PEER_CREDENTIALS_H
#define LOGOS_PEER_CREDENTIALS_H

// Qt-free helper to authenticate the peer of a connected AF_UNIX socket by
// its (effective) uid. Used on BOTH sides of the parent/child token handoff
// (SubprocessContainer sender + SubprocessTokenReceiver child) so that a
// same-host attacker running under a *different* uid cannot inject or
// intercept the auth token over the predictable socket path under $TMPDIR.
//
// Background (F-012): the token socket lives at a public, predictable path in
// the world-writable /tmp. Restricting the socket node to its owner stops a
// different-uid process from connect()ing at the filesystem layer; this peer
// check is the defence-in-depth counterpart — it rejects a different-uid peer
// even if it somehow obtains a connected fd (e.g. by winning a race to create
// the listener the parent then connects to). Pair both: node permissions guard
// the receiver, the peer check guards the sender against a hostile listener.
//
// Residual risk: a uid check authenticates the connecting *uid*, not the
// specific parent/child process. A same-uid sibling can still race the socket;
// closing that gap requires a per-user 0700 socket directory and/or a pid
// check, which is left as future hardening.

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace logos {

// Returns true iff the connected peer on `fd` has the same (effective) uid as
// this process. `fd` must be a connected AF_UNIX socket. Fails closed: returns
// false on a bad fd, a failed syscall, or an unsupported platform — callers
// must treat false as "do not trust this peer".
inline bool socketPeerIsSameUid(int fd) {
    if (fd < 0) return false;

#if defined(__linux__) && defined(SO_PEERCRED)
    // ucred.uid is the peer's *effective* uid captured at connect() time, so
    // compare it against our own effective uid — matching getpeereuid() below.
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return false;
    return cred.uid == ::geteuid();
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
    // getpeereid() (declared in <unistd.h>) returns the peer's effective uid
    // and gid; on these platforms it is the BSD-portable analogue of
    // SO_PEERCRED. We only need the uid, but the gid out-param is mandatory.
    uid_t peerEuid = 0;
    gid_t peerEgid = 0;
    if (::getpeereid(fd, &peerEuid, &peerEgid) != 0)
        return false;
    return peerEuid == ::geteuid();
#else
    (void)fd;
    return false;  // Unknown platform: fail closed.
#endif
}

}  // namespace logos

#endif  // LOGOS_PEER_CREDENTIALS_H

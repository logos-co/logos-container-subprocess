#ifndef SUBPROCESS_TOKEN_RECEIVER_H
#define SUBPROCESS_TOKEN_RECEIVER_H

#include <string>

// Child-side counterpart of SubprocessContainer::sendTokenToProcess().
// Receives the auth token over a QLocalServer Unix socket.
// This is a container concern: the subprocess container delivers credentials
// this way; a different container (Docker, in-process) would use a different
// mechanism.
namespace SubprocessTokenReceiver {
    std::string receive(const std::string& moduleName);
}

// Backward-compat alias for existing code that uses the old name.
namespace QtTokenReceiver {
    inline std::string receiveAuthToken(const std::string& moduleName) {
        return SubprocessTokenReceiver::receive(moduleName);
    }
}

#endif // SUBPROCESS_TOKEN_RECEIVER_H

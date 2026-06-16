#include "token_receiver.h"
#include "peer_credentials.h"
#include "unix_socket_path.h"
#include "path_safety.h"
#include <logos_container/module_name_validation.h>
#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QString>
#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstdlib>
#include <string>

namespace SubprocessTokenReceiver {

std::string receive(const std::string& moduleName)
{
    // Defense in depth at the sink. The registry already rejects invalid
    // names when loading in-process (ModuleRegistry::processModuleInternal),
    // but the host child re-derives the name from an unvalidated --name CLI
    // arg (command_line_parser.cpp), so a directly-invoked host would still
    // reach this point with an attacker-controlled name. Validate before the
    // name becomes a path segment: socketName flows into unixSocketPath() and
    // then QLocalServer::removeServer(), which UNLINKS the resolved file. A
    // crafted name like "x/../../victim" would otherwise delete an
    // attacker-chosen file with this process's privileges. Uses the shared
    // module-name allowlist (module_name_validation.h) so both handoff sides
    // agree on which names are legal.
    if (!::logos::isValidModuleName(moduleName)) {
        spdlog::critical("Refusing invalid module name for token socket: {}", moduleName);
        return {};
    }

    // Scope the socket name by LOGOS_INSTANCE_ID so parallel Logos instances
    // (multiple daemons or Basecamp profiles) loading the same module don't
    // race on a shared socket. SubprocessContainer (parent) uses the same
    // scheme; both read the instance ID from the inherited env var.
    const char* instanceIdEnv = std::getenv("LOGOS_INSTANCE_ID");
    std::string socketName = "logos_token_" + moduleName;
    if (instanceIdEnv && instanceIdEnv[0]) {
        // LOGOS_INSTANCE_ID is also appended verbatim and so becomes part of
        // the socket path — apply the same single-segment guard to it.
        if (!::logos::isSafePathSegment(instanceIdEnv)) {
            spdlog::critical("Refusing unsafe LOGOS_INSTANCE_ID for token socket: {}",
                             instanceIdEnv);
            return {};
        }
        socketName += '_';
        socketName += instanceIdEnv;
    }

    // Resolve the full absolute path using the shared Qt-free helper
    // (see unix_socket_path.h). The parent in SubprocessContainer uses
    // the same helper for connect(); passing the absolute path to
    // QLocalServer::listen() bypasses Qt's QDir::tempPath() resolution
    // so the two sides cannot disagree under environments like
    // `nix run`, where $TMPDIR is unset on macOS and naive
    // getenv("TMPDIR") falls back to /tmp/<name> while Qt resolves
    // /var/folders/.../T/<name> via confstr(_CS_DARWIN_USER_TEMP_DIR).
    const std::string socketPath = ::logos::unixSocketPath(socketName);
    const QString socketPathQ = QString::fromStdString(socketPath);

    // Stack-allocated: the Qt event loop is not guaranteed to be
    // running when this function exits, so deleteLater() is unsafe.
    // RAII makes every exit path (incl. listen() failure) clean up.
    QLocalServer tokenServer;

    // Harden the socket node to owner-only (mode 0600) so a co-tenant
    // process running under a different uid cannot connect() at all. The
    // socket lives at a public, predictable path under the world-writable
    // $TMPDIR (default /tmp); without this, the default NoOptions leaves
    // it group/other-accessible and a local attacker could race the
    // parent to inject an attacker-chosen auth token (F-012, CWE-862).
    // This must be set before listen() creates the node.
    tokenServer.setSocketOptions(QLocalServer::UserAccessOption);

    QLocalServer::removeServer(socketPathQ);

    if (!tokenServer.listen(socketPathQ)) {
        spdlog::critical("Failed to start token server at {}: {}", socketPath,
                         tokenServer.errorString().toStdString());
        return {};
    }

    std::string authToken;
    if (tokenServer.waitForNewConnection(10000)) {
        QLocalSocket* clientSocket = tokenServer.nextPendingConnection();

        // Authenticate the peer by uid: only a process running as our own
        // uid (the legitimate parent host) may hand us a token. The
        // owner-only socket node above already blocks a different-uid
        // attacker; this is the defence-in-depth check that rejects any
        // peer that slips through, rather than trusting whatever bytes the
        // first connector sends (F-012, CWE-862).
        const int peerFd = static_cast<int>(clientSocket->socketDescriptor());
        if (!::logos::socketPeerIsSameUid(peerFd)) {
            spdlog::critical(
                "Rejecting token connection from untrusted peer (expected uid {})",
                ::getuid());
            clientSocket->abort();
            return {};
        }

        if (clientSocket->waitForReadyRead(5000)) {
            QByteArray tokenData = clientSocket->readAll();
            authToken = QString::fromUtf8(tokenData).toStdString();
        }
        // The socket is parented to tokenServer and dies with it.
        clientSocket->disconnectFromServer();
    } else {
        spdlog::critical("Timeout waiting for auth token");
        return {};
    }

    if (authToken.empty()) {
        spdlog::critical("No auth token received");
        return {};
    }

    return authToken;
}

} // namespace SubprocessTokenReceiver

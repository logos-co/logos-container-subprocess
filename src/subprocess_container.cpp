// _GNU_SOURCE exposes struct ucred / SO_PEERCRED from <sys/socket.h> on glibc.
// Must precede any system header. CMake's default -std=gnu++17 predefines it,
// but pin it here so the peer-credential check below compiles regardless of
// the C++ dialect a downstream consumer builds us with.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "subprocess_container.h"

#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>

#include "peer_credentials.h"
#include "unix_socket_path.h"
#include <logos_container/module_name_validation.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace bp2  = boost::process::v2;
namespace asio = boost::asio;

namespace {

std::shared_ptr<spdlog::logger>& moduleStdoutLogger() {
    static std::shared_ptr<spdlog::logger> logger = []() {
        auto l = spdlog::stdout_color_mt("logos_module_stdout");
        l->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [out] %v");
        return l;
    }();
    return logger;
}

// ---------------------------------------------------------------------------
// Background io_context: one thread, kept alive by a work guard.
// ---------------------------------------------------------------------------

struct IoRuntime {
    asio::io_context ctx;
    asio::executor_work_guard<asio::io_context::executor_type> guard;
    std::thread thread;

    IoRuntime()
        : guard(asio::make_work_guard(ctx))
        , thread([this]() { ctx.run(); })
    {}

    ~IoRuntime();
};

IoRuntime& ioRuntime() {
    static IoRuntime s_runtime;
    return s_runtime;
}

// ---------------------------------------------------------------------------
// ProcessEntry: owns one live child process and its read pipes.
// ---------------------------------------------------------------------------

struct ProcessEntry {
    bp2::process                              process;
    asio::readable_pipe                       out_pipe;
    asio::readable_pipe                       err_pipe;
    SubprocessContainer::ProcessCallbacks     callbacks;
    std::string                               name;
    std::array<char, 4096>                    out_read_buf{};
    std::array<char, 4096>                    err_read_buf{};
    std::string                               out_line_buf;
    std::string                               err_line_buf;
    std::atomic<bool>                         exited{false};
    std::atomic<bool>                         cancelled{false};

    ProcessEntry(bp2::process proc,
                 asio::readable_pipe out_rp, asio::readable_pipe err_rp,
                 const std::string& n, const SubprocessContainer::ProcessCallbacks& cb)
        : process(std::move(proc))
        , out_pipe(std::move(out_rp))
        , err_pipe(std::move(err_rp))
        , name(n)
        , callbacks(cb)
    {}
};

// ---------------------------------------------------------------------------
// Global process registry
//
// Declared at namespace scope (constructed before main), while the
// IoRuntime singleton above is a function-local static (constructed
// lazily on first use, from main). C++ destroys statics in reverse
// order of construction, so at exit ~IoRuntime fires first — tearing
// down the asio::io_context (and its epoll_reactor) — and *then*
// s_processes is destroyed, dropping its shared_ptr<ProcessEntry>s,
// each of which closes asio handles (process / pipes) tied to the
// already-freed reactor. Use-after-free → heap corruption → SIGABRT.
// ~IoRuntime (defined below, out-of-line) handles this by clearing
// s_processes itself while ctx is still alive.
// ---------------------------------------------------------------------------

std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> s_processes;
std::mutex s_processesMutex;

// ---------------------------------------------------------------------------

IoRuntime::~IoRuntime() {
    guard.reset();
    ctx.stop();
    if (thread.joinable()) {
        // Common case: destructor fires from the main thread at process
        // exit, ctx.run() returned cleanly, just join.
        //
        // Pathological case: destructor fires from *this very thread*.
        // Happens when an asio handler running on `thread` calls
        // exit() (e.g. the onFinished callback below crash-aborts the
        // process). exit() triggers static destruction in the calling
        // thread; that's us. join() on yourself is EDEADLK and would
        // throw a std::system_error → uncaught → terminate() → SIGABRT,
        // masking the real crash that triggered the exit() in the first
        // place. Detach instead: the OS reaps the thread on process
        // exit, no observable difference vs join in this single-process
        // scenario.
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
        } else {
            thread.join();
        }
    }

    // Tear down ProcessEntries while ctx (and its epoll_reactor) is
    // still alive. See the static-destruction-order note on s_processes
    // above. Doing this from ~IoRuntime instead of relying on the
    // implicit reverse order of static destruction guarantees that
    // every io_object_impl::~io_object_impl() (which calls
    // reactor.deregister_descriptor) runs against a live reactor.
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        s_processes.clear();
    }
}

// ---------------------------------------------------------------------------
// Unix domain socket path
// ---------------------------------------------------------------------------

// Resolve the socket path via the shared Qt-free helper. The child
// (qt_token_receiver) computes the same path with the same helper and
// passes it to QLocalServer::listen() as an absolute path, so Qt's
// QDir::tempPath() resolution is bypassed on both sides.
using ::logos::unixSocketPath;

// ---------------------------------------------------------------------------
// Listener authentication (CWE-940)
// ---------------------------------------------------------------------------
//
// The token socket lives at a predictable path under a world-writable temp
// dir (see unix_socket_path.h) and we connect() to it in a retry loop. A
// successful connect() proves only that *something* is listening there — not
// that it is the child module we spawned. A local co-tenant can pre-bind that
// path before the real child calls listen(); our connect() then lands on the
// attacker's socket and, without this check, write()s the genuine auth token
// straight to them. They replay it as `authToken` to make authorized
// cross-module calls.
//
// Before writing the secret we verify the peer's kernel-reported credentials:
//   - the peer must run as our own euid (no other user may receive the token);
//   - when we know the child's pid (the common case — launch() records it
//     before sendToken() runs), the peer must be exactly that process.
//
// Any failure is fatal: we refuse to write the token rather than risk leaking
// it. This is the sender/parent half of the handoff (CWE-940 / finding F-010);
// the child/receiver side (token_receiver.cpp) has a symmetric exposure that
// still needs the mirror-image peer check.
//
// The peer pid comes from SO_PEERCRED on Linux and getsockopt(SOL_LOCAL,
// LOCAL_PEERPID) on macOS, so both platforms enforce the uid+pid gate.
//
// Residual risk: when no child pid is recorded (e.g. the placeholder path used
// by some callers) only the uid gate applies, so a same-uid co-tenant could
// still receive the token. Even with the pid gate, a same-uid attacker can in
// principle race for the path between the child's listen() and our connect().
// The race window is fully closed only by handing the child a pre-connected
// socketpair() fd instead of a named path — see the note in docs/spec.md.
// The peer-credential check is the defense-in-depth that closes the cross-uid
// theft this finding describes and, when the child pid is known, the same-uid
// squat as well.
//
// Returns true if the connected peer is acceptable, false if the token must
// not be written. `expectedPid <= 0` means "child pid unknown, skip the pid
// gate" (the uid gate still applies).
bool peerIsTrusted(int sock, int64_t expectedPid, const std::string& name)
{
#if defined(__linux__)
    struct ucred cred{};
    socklen_t len = sizeof(cred);
    if (::getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        spdlog::warn("SO_PEERCRED failed for token peer of {}: {}",
                                    name, strerror(errno));
        return false;
    }
    if (cred.uid != ::geteuid()) {
        spdlog::warn(
            "token peer uid mismatch for {} (peer uid {} != {}); refusing to send token",
            name, static_cast<unsigned>(cred.uid), static_cast<unsigned>(::geteuid()));
        return false;
    }
    if (expectedPid > 0 && static_cast<int64_t>(cred.pid) != expectedPid) {
        spdlog::warn(
            "token peer pid mismatch for {} (peer pid {} != child {}); refusing to send token",
            name, static_cast<int>(cred.pid), static_cast<long long>(expectedPid));
        return false;
    }
    return true;
#elif defined(__APPLE__)
    // macOS has no SO_PEERCRED/ucred, but it exposes the two facts we need
    // through separate APIs: getpeereid() for the peer's effective uid, and
    // getsockopt(SOL_LOCAL, LOCAL_PEERPID) for the peer's pid (since macOS
    // 10.8). Checking both gives parity with the Linux uid+pid gate, so a
    // same-uid co-tenant squatting the path is rejected on the pid mismatch
    // just as it is on Linux — not only the cross-uid theft.
    uid_t peerUid = 0;
    gid_t peerGid = 0;
    if (::getpeereid(sock, &peerUid, &peerGid) != 0) {
        spdlog::warn("getpeereid failed for token peer of {}: {}",
                                    name, strerror(errno));
        return false;
    }
    if (peerUid != ::geteuid()) {
        spdlog::warn(
            "token peer uid mismatch for {} (peer uid {} != {}); refusing to send token",
            name, static_cast<unsigned>(peerUid), static_cast<unsigned>(::geteuid()));
        return false;
    }
    // When the child pid is known, enforce it too. LOCAL_PEERPID reports the
    // pid of the process that connect()ed the socket — exactly the peer we are
    // about to hand the token to. A getsockopt failure (e.g. EOPNOTSUPP on a
    // pre-10.8 kernel) is fatal: we fail closed rather than silently downgrade
    // to the uid-only gate and leak the token to a same-uid squatter.
    if (expectedPid > 0) {
        pid_t peerPid = 0;
        socklen_t pidLen = sizeof(peerPid);
        if (::getsockopt(sock, SOL_LOCAL, LOCAL_PEERPID, &peerPid, &pidLen) != 0) {
            spdlog::warn(
                "LOCAL_PEERPID failed for token peer of {}: {}; refusing to send token",
                name, strerror(errno));
            return false;
        }
        if (static_cast<int64_t>(peerPid) != expectedPid) {
            spdlog::warn(
                "token peer pid mismatch for {} (peer pid {} != child {}); refusing to send token",
                name, static_cast<int>(peerPid), static_cast<long long>(expectedPid));
            return false;
        }
    }
    return true;
#else
    // Unknown platform: we cannot authenticate the peer, so we cannot make
    // the security guarantee. Fail closed rather than leak the token.
    (void)sock; (void)expectedPid;
    spdlog::error(
        "no peer-credential API on this platform; refusing to send token for {}", name);
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Async read loop
// ---------------------------------------------------------------------------

void scheduleRead(std::shared_ptr<ProcessEntry> entry, bool isStderr);

void handleRead(std::shared_ptr<ProcessEntry> entry, bool isStderr,
                const boost::system::error_code& ec, std::size_t n)
{
    auto& buf      = isStderr ? entry->err_read_buf : entry->out_read_buf;
    auto& line_buf = isStderr ? entry->err_line_buf : entry->out_line_buf;

    if (n > 0) {
        // Everything already in line_buf was scanned for '\n' on previous
        // reads and contained none (the loop below consumes through every
        // newline and erase() drops the consumed prefix), so resume the
        // search at the old end instead of rescanning from offset 0. Without
        // this, a newline-free stream from a child re-scans the whole growing
        // buffer on every 4 KB read — O(N^2) CPU that pins the shared io
        // thread supervising all modules (F-014).
        const std::size_t search_start = line_buf.size();
        line_buf.append(buf.data(), n);

        std::size_t pos = 0, nl, search = search_start;
        while ((nl = line_buf.find('\n', search)) != std::string::npos) {
            std::string line = line_buf.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && entry->callbacks.onOutput)
                entry->callbacks.onOutput(entry->name, line, isStderr);
            pos = nl + 1;
            search = pos;
        }
        line_buf.erase(0, pos);

        // Bound the unterminated remainder. The child runs partially-trusted
        // module code; one that emits a long newline-free stream (or a single
        // multi-GB write) would otherwise grow line_buf without limit, pinning
        // host memory until the OS OOM-kills the trusted parent and every
        // module it supervises. Once the buffered prefix reaches the cap,
        // force-flush it as a line and reset so memory stays bounded — a
        // module must not be able to take the host down this way (F-014).
        if (line_buf.size() >= SubprocessContainer::kMaxOutputLineBytes) {
            if (entry->callbacks.onOutput)
                entry->callbacks.onOutput(entry->name, line_buf, isStderr);
            line_buf.clear();
        }
    }

    if (!ec) {
        scheduleRead(std::move(entry), isStderr);
    } else {
        if (!line_buf.empty() && entry->callbacks.onOutput) {
            if (line_buf.back() == '\r') line_buf.pop_back();
            if (!line_buf.empty())
                entry->callbacks.onOutput(entry->name, line_buf, isStderr);
        }
        line_buf.clear();
    }
}

void scheduleRead(std::shared_ptr<ProcessEntry> entry, bool isStderr) {
    auto* e = entry.get();
    auto& pipe = isStderr ? e->err_pipe : e->out_pipe;
    auto& buf  = isStderr ? e->err_read_buf : e->out_read_buf;
    pipe.async_read_some(
        asio::buffer(buf),
        [entry = std::move(entry), isStderr](const boost::system::error_code& ec, std::size_t n) mutable {
            handleRead(std::move(entry), isStderr, ec, n);
        });
}

// ---------------------------------------------------------------------------
// Async wait
// ---------------------------------------------------------------------------

void scheduleWait(std::shared_ptr<ProcessEntry> entry) {
    auto* e = entry.get();
    e->process.async_wait(
        [entry = std::move(entry)](const boost::system::error_code& /*ec*/, int raw_status) mutable {
            entry->exited.store(true);

            std::string name = entry->name;
            bool was_cancelled = entry->cancelled.load();

            {
                std::lock_guard<std::mutex> lock(s_processesMutex);
                s_processes.erase(name);
            }

            if (!was_cancelled && entry->callbacks.onFinished) {
                bool crashed = false;
                int exit_code = raw_status;
#if defined(WIFEXITED)
                if (WIFSIGNALED(raw_status)) {
                    crashed    = true;
                    exit_code  = WTERMSIG(raw_status);
                } else if (WIFEXITED(raw_status)) {
                    exit_code = WEXITSTATUS(raw_status);
                }
#endif
                entry->callbacks.onFinished(name, exit_code, crashed);
            }
        });
}

// ---------------------------------------------------------------------------
// Synchronous kill
// ---------------------------------------------------------------------------

void syncKill(std::shared_ptr<ProcessEntry> entry) {
    if (!entry) return;

    entry->cancelled.store(true);

    boost::system::error_code ec;
    entry->out_pipe.close(ec);
    entry->err_pipe.close(ec);

    entry->process.request_exit(ec);

    auto wait = [&](std::chrono::milliseconds budget) -> bool {
        auto deadline = std::chrono::steady_clock::now() + budget;
        while (!entry->exited.load()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return true;
    };

    if (!wait(std::chrono::seconds(5))) {
        spdlog::warn("Process did not terminate gracefully, killing: {}",
                                    entry->name);
        entry->process.terminate(ec);
        if (!wait(std::chrono::seconds(2))) {
            spdlog::error("Process did not respond to SIGKILL: {}",
                                         entry->name);
        }
    }
}

} // anonymous namespace

// ===========================================================================
// ModuleContainer interface
// ===========================================================================

bool SubprocessContainer::canHandle(const LogosCore::ModuleDescriptor& /*desc*/) const
{
    return true;
}

bool SubprocessContainer::launch(const LogosCore::ModuleDescriptor& desc,
                                  const std::string& hostBinary,
                                  const std::vector<std::string>& args,
                                  std::function<void(const std::string&)> onTerminated,
                                  LogosCore::LoadedModuleHandle& out)
{
    ProcessCallbacks callbacks;

    // A crashing module must NOT take down the host: process isolation exists
    // precisely so a module fault is contained. Treat a crash the same as a
    // normal exit — log it and notify so the registry marks the module
    // unloaded and the UI can react. (Calling exit() here also ran C++ static
    // destructors / Qt plugin unload / GUI teardown from this boost::asio
    // worker thread, racing the main thread and segfaulting.)
    // `onTerminated` is documented as safe to invoke from a background thread
    // (see ModuleLoader::load); ModuleRegistry::markUnloaded is mutex-guarded.
    callbacks.onFinished = [onTerminated](const std::string& pName, int exitCode, bool crashed) {
        (void)exitCode;
        if (crashed)
            spdlog::critical("Module process crashed: {}", pName);
        if (onTerminated)
            onTerminated(pName);
    };

    callbacks.onError = [onTerminated](const std::string& pName, bool crashed) {
        if (crashed)
            spdlog::critical("Module process crashed: {}", pName);
        if (onTerminated)
            onTerminated(pName);
    };

    callbacks.onOutput = [](const std::string& pName, const std::string& line, bool isStderr) {
        if (!isStderr) {
            moduleStdoutLogger()->info("[{}] {}", pName, line);
            return;
        }
        auto contains = [&](std::initializer_list<const char*> keywords) {
            for (const char* k : keywords)
                if (line.find(k) != std::string::npos) return true;
            return false;
        };
        if (contains({"Critical:", "CRITICAL:", "Fatal:", "FATAL:"}))
            spdlog::critical("[{}] {}", pName, line);
        else if (contains({"Error:", "ERROR:", "FAILED:"}))
            spdlog::error("[{}] {}", pName, line);
        else if (contains({"Warning:", "WARNING:"}))
            spdlog::warn("[{}] {}", pName, line);
        else if (contains({"Debug:", "DEBUG:"}))
            spdlog::debug("[{}] {}", pName, line);
        else if (contains({"Trace:", "TRACE:"}))
            spdlog::trace("[{}] {}", pName, line);
        else
            spdlog::info("[{}] {}", pName, line);
    };

    if (!startProcess(desc.name, hostBinary, args, callbacks))
        return false;

    out.name = desc.name;
    out.pid  = getProcessId(desc.name);
    return true;
}

bool SubprocessContainer::sendToken(const std::string& name, const std::string& token)
{
    return sendTokenToProcess(name, token);
}

void SubprocessContainer::terminate(const std::string& name)
{
    terminateProcess(name);
}

void SubprocessContainer::terminateAll()
{
    terminateAllProcesses();
}

bool SubprocessContainer::hasModule(const std::string& name) const
{
    return hasProcess(name);
}

std::optional<int64_t> SubprocessContainer::pid(const std::string& name) const
{
    int64_t p = getProcessId(name);
    if (p < 0) return std::nullopt;
    return p;
}

std::unordered_map<std::string, int64_t> SubprocessContainer::getAllPids() const
{
    return getAllProcessIds();
}

// ===========================================================================
// Static process management API
// ===========================================================================

bool SubprocessContainer::startProcess(const std::string& name, const std::string& executable,
                                        const std::vector<std::string>& arguments,
                                        const ProcessCallbacks& callbacks)
{
    IoRuntime& rt = ioRuntime();

    boost::system::error_code ec;

    asio::readable_pipe out_rpipe(rt.ctx), err_rpipe(rt.ctx);
    asio::writable_pipe out_wpipe(rt.ctx), err_wpipe(rt.ctx);

    asio::connect_pipe(out_rpipe, out_wpipe, ec);
    if (ec) {
        spdlog::error("Failed to create stdout pipe for {}: {}",
                                     name, ec.message());
        return false;
    }
    asio::connect_pipe(err_rpipe, err_wpipe, ec);
    if (ec) {
        spdlog::error("Failed to create stderr pipe for {}: {}",
                                     name, ec.message());
        return false;
    }

    bp2::process_stdio pstdio;
    pstdio.out = out_wpipe;
    pstdio.err = err_wpipe;

    bp2::process proc = bp2::default_process_launcher()(rt.ctx, ec, executable, arguments, pstdio);

    out_wpipe.close();
    err_wpipe.close();

    if (ec) {
        spdlog::error("Failed to start process for {}: {}",
                                     name, ec.message());
        return false;
    }

    auto entry = std::make_shared<ProcessEntry>(
        std::move(proc), std::move(out_rpipe), std::move(err_rpipe), name, callbacks);

    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        s_processes[name] = entry;
    }

    asio::post(rt.ctx, [entry]() {
        scheduleRead(entry, /*isStderr=*/false);
        scheduleRead(entry, /*isStderr=*/true);
        scheduleWait(entry);
    });

    return true;
}

bool SubprocessContainer::sendTokenToProcess(const std::string& name,
                                              const std::string& token,
                                              int max_wait_ms)
{
    // Defense-in-depth: `name` is validated at the registry trust boundary
    // (ModuleRegistry::processModuleInternal), but the token-handoff socket
    // filename must never depend on raw caller input even if some future path
    // reaches here without going through registration. A name with '/' or
    // ".." would otherwise escape qtCompatibleTempDir() and let the parent
    // connect() to an attacker-controlled path, leaking the auth token
    // (CWE-22). See module_name_validation.h.
    if (!logos::isValidModuleName(name)) {
        spdlog::error(
            "Refusing token handoff for invalid module name: {}", name);
        std::shared_ptr<ProcessEntry> entry;
        {
            std::lock_guard<std::mutex> lock(s_processesMutex);
            auto it = s_processes.find(name);
            if (it != s_processes.end()) {
                entry = it->second;
                s_processes.erase(it);
            }
        }
        syncKill(entry);
        return false;
    }

    const char* instanceId = std::getenv("LOGOS_INSTANCE_ID");
    std::string socketName = "logos_token_" + name;
    if (instanceId && *instanceId) {
        socketName += "_";
        socketName += instanceId;
    }
    std::string path = unixSocketPath(socketName);

    {
        struct sockaddr_un sample{};
        if (path.size() >= sizeof(sample.sun_path)) {
            spdlog::error("Unix socket path too long ({} >= {}): {}",
                                         path.size(), sizeof(sample.sun_path), path);
            std::shared_ptr<ProcessEntry> entry;
            {
                std::lock_guard<std::mutex> lock(s_processesMutex);
                auto it = s_processes.find(name);
                if (it != s_processes.end()) {
                    entry = it->second;
                    s_processes.erase(it);
                }
            }
            syncKill(entry);
            return false;
        }
    }

    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(max_wait_ms);

    // The child we spawned, if known. launch() -> startProcess() records the
    // child's pid before sendToken() runs, so in the normal load path this is
    // the genuine child's pid and lets us reject any other listener squatting
    // the predictable socket path. -1 (placeholder / unknown) skips the pid
    // gate but still enforces the uid gate in peerIsTrusted().
    const int64_t expectedPid = getProcessId(name);

    int sock = -1;
    for (;;) {
        sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            spdlog::error("socket() failed: {}", strerror(errno));
            break;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            // connect() only proves something is listening at this predictable,
            // world-writable path — not that it is our child. Authenticate the
            // peer before writing the secret (CWE-940). A trusted peer ends the
            // loop; an untrusted one is treated like a failed connect, so a
            // co-tenant squatting the path cannot steal the token and the real
            // child can still claim the path before the deadline.
            if (peerIsTrusted(sock, expectedPid, name))
                break;
            ::close(sock);
            sock = -1;
            if (clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        ::close(sock);
        sock = -1;
        if (clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (sock < 0) {
        spdlog::error("Failed to connect to token socket for: {}", name);

        std::shared_ptr<ProcessEntry> entry;
        {
            std::lock_guard<std::mutex> lock(s_processesMutex);
            auto it = s_processes.find(name);
            if (it != s_processes.end()) {
                entry = it->second;
                s_processes.erase(it);
            }
        }
        syncKill(entry);
        return false;
    }

    // Authenticate the listener before handing it the token. The socket path
    // is public and predictable under the world-writable $TMPDIR, so a
    // co-tenant attacker could win the race to create the listener we just
    // connect()ed to. Only deliver the credential if the peer runs as our own
    // uid (the legitimate child host); otherwise we'd be leaking the token to
    // a hostile process (F-012, CWE-862). Symmetric with the receiver's check.
    if (!::logos::socketPeerIsSameUid(sock)) {
        spdlog::warn(
            "Refusing to send token to untrusted listener (uid mismatch) for: {}", name);
        ::close(sock);

        std::shared_ptr<ProcessEntry> entry;
        {
            std::lock_guard<std::mutex> lock(s_processesMutex);
            auto it = s_processes.find(name);
            if (it != s_processes.end()) {
                entry = it->second;
                s_processes.erase(it);
            }
        }
        syncKill(entry);
        return false;
    }

    const char* data = token.c_str();
    std::size_t total = token.size();
    std::size_t written = 0;
    while (written < total) {
        ssize_t n = ::write(sock, data + written, total - written);
        if (n <= 0) break;
        written += static_cast<std::size_t>(n);
    }

    ::close(sock);
    return written == total;
}

void SubprocessContainer::terminateProcess(const std::string& name)
{
    std::shared_ptr<ProcessEntry> entry;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        auto it = s_processes.find(name);
        if (it == s_processes.end()) return;
        entry = it->second;
        s_processes.erase(it);
    }
    syncKill(entry);
}

void SubprocessContainer::terminateAllProcesses()
{
    std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        if (s_processes.empty()) return;
        snapshot.swap(s_processes);
    }
    for (auto& [n, entry] : snapshot)
        syncKill(entry);
}

bool SubprocessContainer::hasProcess(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    return s_processes.count(name) > 0;
}

int64_t SubprocessContainer::getProcessId(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    auto it = s_processes.find(name);
    if (it == s_processes.end()) return -1;
    if (!it->second)              return -1;
    return static_cast<int64_t>(it->second->process.id());
}

std::unordered_map<std::string, int64_t> SubprocessContainer::getAllProcessIds()
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    std::unordered_map<std::string, int64_t> result;
    for (auto& [n, entry] : s_processes)
        if (entry)
            result[n] = static_cast<int64_t>(entry->process.id());
    return result;
}

void SubprocessContainer::clearAll()
{
    std::unordered_map<std::string, std::shared_ptr<ProcessEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        snapshot.swap(s_processes);
    }
    for (auto& [n, entry] : snapshot)
        syncKill(entry);
}

void SubprocessContainer::registerProcess(const std::string& name)
{
    std::lock_guard<std::mutex> lock(s_processesMutex);
    if (!s_processes.count(name))
        s_processes[name] = nullptr;
}

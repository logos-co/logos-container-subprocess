#include "subprocess_container.h"

#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/stdio.hpp>

#include <spdlog/spdlog.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#include <cerrno>
#include <cstdlib>
// environ is declared by <unistd.h> on glibc and musl but NOT on Apple, so it
// is declared here -- at file scope, where there can only be one of it.
extern "C" char** environ;
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 34)
#define LOGOS_HAVE_CLOSEFROM_NP 1
#endif
#endif
#endif
#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
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
    // Parent write-end of the child's stdin. The auth token is delivered by
    // writing it here (see sendTokenToProcess): the child inherited the read
    // end as fd 0, so this pipe is private to the parent/child pair —
    // unforgeable, with no predictable filesystem path to squat. Held open
    // from launch until sendToken writes the token and closes it.
    asio::writable_pipe                       in_pipe;
    SubprocessContainer::ProcessCallbacks     callbacks;
    std::string                               name;
    std::array<char, 4096>                    out_read_buf{};
    std::array<char, 4096>                    err_read_buf{};
    std::string                               out_line_buf;
    std::string                               err_line_buf;
    std::atomic<bool>                         exited{false};
    std::atomic<bool>                         cancelled{false};
    // Set when stdout reaches EOF, which for a dead child is the point after
    // which no further status line can arrive. The exit verdict waits for it so
    // a child that reports a reason and then exits is described by the reason.
    std::atomic<bool>                         out_eof{false};

    // The child's own verdict on its startup, and how a waiter in awaitLoad()
    // hears about it. Both the status line (io thread, handleRead) and the exit
    // (io thread, scheduleWait) settle this, so awaitLoad returns as soon as
    // either happens rather than waiting out its deadline.
    std::mutex                                status_mutex;
    std::condition_variable                   status_cv;
    std::optional<LogosCore::LoadOutcome>     status;
    int                                       exit_code{0};
    bool                                      exit_crashed{false};
#ifdef _WIN32
    // Needed to ask the child to quit: see requestGracefulExit(). Zero if the
    // launcher never reported one, in which case we fall back to terminate().
    DWORD                                     main_thread_id{0};
#endif

    ProcessEntry(bp2::process proc,
                 asio::readable_pipe out_rp, asio::readable_pipe err_rp,
                 asio::writable_pipe in_wp,
                 const std::string& n, const SubprocessContainer::ProcessCallbacks& cb)
        : process(std::move(proc))
        , out_pipe(std::move(out_rp))
        , err_pipe(std::move(err_rp))
        , in_pipe(std::move(in_wp))
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
// Async read loop
// ---------------------------------------------------------------------------

// Settle the child's load verdict, once. First writer wins: a host that reports
// a failure and then exits should be reported by what it SAID, not by the exit
// code that followed from it.
void settleLoadStatus(ProcessEntry& entry, LogosCore::LoadOutcome outcome) {
    {
        std::lock_guard<std::mutex> lock(entry.status_mutex);
        if (entry.status.has_value()) return;
        entry.status = std::move(outcome);
    }
    entry.status_cv.notify_all();
}

// Recognise the child's status line and consume it. Returns true when the line
// was protocol and should not be relayed as module output.
bool consumeLoadStatusLine(ProcessEntry& entry, const std::string& line) {
    const std::string prefix(LogosCore::kLoadStatusPrefix);
    if (line.rfind(prefix, 0) != 0) return false;

    std::string rest = line.substr(prefix.size());
    const std::size_t start = rest.find_first_not_of(" \t");
    rest = (start == std::string::npos) ? std::string() : rest.substr(start);

    if (rest == LogosCore::kLoadStatusOk) {
        settleLoadStatus(entry, {LogosCore::LoadVerdict::Loaded, {}});
        return true;
    }

    const std::string failed(LogosCore::kLoadStatusFailed);
    if (rest.rfind(failed, 0) == 0) {
        std::string reason = rest.substr(failed.size());
        const std::size_t r = reason.find_first_not_of(" \t");
        reason = (r == std::string::npos) ? std::string() : reason.substr(r);
        if (reason.empty()) reason = "the module host reported a load failure";
        settleLoadStatus(entry, {LogosCore::LoadVerdict::Failed, std::move(reason)});
        return true;
    }

    // Our prefix, a word we do not know: a newer host talking to an older
    // container. Say so once rather than relaying it as module output.
    spdlog::debug("Unrecognised load-status line from {}: {}", entry.name, line);
    return true;
}

// Settle the exit as a failed load, once both facts are in: the child is gone
// and its stdout is drained.
void maybeSettleExitAsFailure(ProcessEntry& entry) {
    if (!entry.exited.load() || !entry.out_eof.load()) return;

    std::string reason;
    if (entry.cancelled.load())
        reason = "the module process was terminated before it reported that it had loaded";
    else if (entry.exit_crashed)
        reason = "the module process died on signal " + std::to_string(entry.exit_code) +
                 " before it reported that it had loaded";
    else
        reason = "the module process exited with code " + std::to_string(entry.exit_code) +
                 " before it reported that it had loaded";

    settleLoadStatus(entry, {LogosCore::LoadVerdict::Failed, std::move(reason)});
}

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
            if (!line.empty() && !consumeLoadStatusLine(*entry, line) &&
                entry->callbacks.onOutput)
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
        if (!line_buf.empty()) {
            if (line_buf.back() == '\r') line_buf.pop_back();
            if (!line_buf.empty() && !consumeLoadStatusLine(*entry, line_buf) &&
                entry->callbacks.onOutput)
                entry->callbacks.onOutput(entry->name, line_buf, isStderr);
        }
        line_buf.clear();
        if (!isStderr) {
            entry->out_eof.store(true);
            maybeSettleExitAsFailure(*entry);
        }
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
            std::string name = entry->name;
            bool was_cancelled = entry->cancelled.load();

            {
                bool crashed = false;
                // async_wait's int is ALREADY evaluated (boost/process/v2/
                // exit_code.hpp): WEXITSTATUS for an exit, WTERMSIG for a
                // signal, with nothing left to say which it was. Re-decoding it
                // as a wait status made every exit code in 1..126 look like a
                // fatal signal -- so a module host exiting 1 because its plugin
                // would not load was logged as a crash. The raw status is still
                // on the handle, and it is what distinguishes the two.
                int exit_code = raw_status;
#if defined(WIFEXITED)
                const auto native = entry->process.native_exit_code();
                if (WIFSIGNALED(native)) {
                    crashed    = true;
                    exit_code  = WTERMSIG(native);
                } else if (WIFEXITED(native)) {
                    exit_code = WEXITSTATUS(native);
                }
#elif defined(_WIN32)
                // Windows has no wait-status encoding: the raw value IS the
                // exit code. Without this branch `crashed` stayed false for
                // every child, so `logosctl status` reported an empty
                // crash_signal even for a module that died on an access
                // violation.
                //
                // There is no signal to report, so treat the standard
                // fatal-exception status codes as a crash. These are the
                // NTSTATUS values the OS uses when it kills a process, all of
                // which have the severity bits set (0xC0000000).
                switch (static_cast<unsigned long>(raw_status)) {
                    case 0xC0000005ul:  // ACCESS_VIOLATION
                    case 0xC000001Dul:  // ILLEGAL_INSTRUCTION
                    case 0xC0000025ul:  // NONCONTINUABLE_EXCEPTION
                    case 0xC0000026ul:  // INVALID_DISPOSITION
                    case 0xC000008Cul:  // ARRAY_BOUNDS_EXCEEDED
                    case 0xC0000094ul:  // INTEGER_DIVIDE_BY_ZERO
                    case 0xC0000096ul:  // PRIVILEGED_INSTRUCTION
                    case 0xC00000FDul:  // STACK_OVERFLOW
                    case 0xC0000409ul:  // STACK_BUFFER_OVERRUN / __fastfail
                    case 0xC0000374ul:  // HEAP_CORRUPTION
                        crashed = true;
                        break;
                    default:
                        break;
                }
#endif
                // Recorded before `exited`, so a waiter that wakes on it reads a
                // settled exit code rather than a zero-initialised one.
                entry->exit_code    = exit_code;
                entry->exit_crashed = crashed;
                entry->exited.store(true);
                maybeSettleExitAsFailure(*entry);
            }

            {
                std::lock_guard<std::mutex> lock(s_processesMutex);
                s_processes.erase(name);
            }

            if (!was_cancelled && entry->callbacks.onFinished)
                entry->callbacks.onFinished(name, entry->exit_code, entry->exit_crashed);
        });
}

#ifndef _WIN32
// ---------------------------------------------------------------------------
// POSIX child-process plumbing
//
// WHY NOT bp2's LAUNCHER. Its POSIX default forks, and the FORKED CHILD calls
// ctx.notify_fork(fork_child) before execve. That walks every asio service
// taking their locks -- service_registry::mutex_, which every pipe
// construction on this context also takes, and the reactor's descriptor mutex,
// which pipe teardown takes on the io thread. A lock held by any other thread
// at fork time is inherited locked with no owner, so the child blocks forever
// before exec while the parent blocks forever on an untimed exec-status pipe
// read. Measured: a 6 h CI hang, leaving a live single-threaded never-exec'd
// child. Serializing spawns does not fix it -- the io thread is a second
// thread and takes the reactor mutex on every module exit.
//
// posix_spawn runs NO user code between fork and exec: the file actions are
// declarative and libc performs them with async-signal-safe calls only.
// Not vfork -- on macOS that has been plain fork since 12.0, so the child's
// error write is invisible to the parent and a failed exec reports SUCCESS.
// ---------------------------------------------------------------------------

// Everything above stderr that is not this child's own stdio. bp2's launcher
// did this unconditionally in the forked child; posix_spawn does not, so it is
// spelled out here -- best mechanism first, because glibc only grew
// addclosefrom_np in 2.34 and Ubuntu 20.04 and RHEL 8 are older than that.
int addCloseForeignFds(posix_spawn_file_actions_t& fa, int a, int b, int c)
{
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    (void)fa; (void)a; (void)b; (void)c;
    return 0;                                   // the attr flag covers it
#elif defined(LOGOS_HAVE_CLOSEFROM_NP)
    (void)a; (void)b; (void)c;
    return ::posix_spawn_file_actions_addclosefrom_np(&fa, STDERR_FILENO + 1);
#else
    // Enumerate. A descriptor opened after this races and is missed -- the same
    // race bp2's own /proc walk had.
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr) d = ::opendir("/dev/fd");
    if (d != nullptr) {
        const int dfd = ::dirfd(d);
        int e = 0;
        while (const dirent* ent = ::readdir(d)) {
            const int fd = ::atoi(ent->d_name);
            if (fd <= STDERR_FILENO || fd == dfd || fd == a || fd == b || fd == c)
                continue;
            e = ::posix_spawn_file_actions_addclose(&fa, fd);
            if (e) break;
        }
        ::closedir(d);
        return e;
    }
    const long lim = ::sysconf(_SC_OPEN_MAX);
    const int  top = lim > 0 ? static_cast<int>(lim) : 1024;
    for (int fd = STDERR_FILENO + 1; fd < top; ++fd) {
        if (fd == a || fd == b || fd == c || ::fcntl(fd, F_GETFD) == -1) continue;
        if (const int e = ::posix_spawn_file_actions_addclose(&fa, fd)) return e;
    }
    return 0;
#endif
}

// Returns 0, or an errno.
int spawnChild(const std::string& executable,
               const std::vector<std::string>& arguments,
               int child_in, int child_out, int child_err,
               pid_t& out_pid)
{
    // File actions run in order, so a source that is already 0/1/2 would be
    // clobbered by an earlier dup2. Our pipes are always above stderr; refuse
    // loudly rather than silently crossing a module's streams.
    if (child_in <= STDERR_FILENO || child_out <= STDERR_FILENO ||
        child_err <= STDERR_FILENO) {
        spdlog::error("Refusing to spawn {}: a pipe landed on fd {}/{}/{}, which "
                      "means this process was started with stdio closed",
                      executable, child_in, child_out, child_err);
        return EBADF;
    }

    posix_spawn_file_actions_t fa;
    if (int e = ::posix_spawn_file_actions_init(&fa)) return e;
    posix_spawnattr_t attr;
    if (int e = ::posix_spawnattr_init(&attr)) {
        ::posix_spawn_file_actions_destroy(&fa);
        return e;
    }

    int e = 0;
    // dup2 clears FD_CLOEXEC on the TARGET, which is what lets these three
    // survive exec while their close-on-exec originals do not.
    if (!e) e = ::posix_spawn_file_actions_adddup2(&fa, child_in,  STDIN_FILENO);
    if (!e) e = ::posix_spawn_file_actions_adddup2(&fa, child_out, STDOUT_FILENO);
    if (!e) e = ::posix_spawn_file_actions_adddup2(&fa, child_err, STDERR_FILENO);
    // Foreign descriptors -- Qt's, the embedding app's -- which we did not open
    // and cannot mark close-on-exec ourselves.
    if (!e) e = addCloseForeignFds(fa, child_in, child_out, child_err);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    if (!e) e = ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif

    if (!e) {
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        // argv[0] is the executable path, as bp2's launcher sets it.
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& a : arguments) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        // posix_spawn, not posix_spawnp: bp2 used execve, so no PATH search.
        pid_t pid = -1;
        e = ::posix_spawn(&pid, executable.c_str(), &fa, &attr, argv.data(), environ);
        if (!e) out_pid = pid;
    }

    ::posix_spawnattr_destroy(&attr);
    ::posix_spawn_file_actions_destroy(&fa);
    return e;
}

// Our own pipe ends must not reach a child. asio::connect_pipe does not mark
// them (boost/asio/impl/connect_pipe.ipp is a bare ::pipe), and a child that
// inherits a SIBLING's ends never lets that module's stdout reach EOF, so its
// exit is never settled and awaitLoad returns a vacuously wrong verdict, while
// its stdin read end stays open and it waits for a token that cannot arrive.
//
// Belt to addCloseForeignFds' braces: that enumerates at spawn time and so
// cannot see a pipe created moments later by another thread; this marks ours at
// birth. Neither alone covers both directions.
bool markCloexec(std::initializer_list<int> fds)
{
    for (int fd : fds) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags == -1 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
            return false;
    }
    return true;
}
#endif  // !_WIN32

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Windows child-process plumbing
//
// Two POSIX mechanisms this container relies on have no direct Win32
// equivalent, and both are handled here.
//
// 1. GRACEFUL EXIT. bp2's process::request_exit() is, on Windows,
//    EnumWindows + SendMessageW(WM_CLOSE) (boost/process src/detail/
//    process_handle_windows.cpp). EnumWindows only visits TOP-LEVEL WINDOWS,
//    and a module host is a windowless QCoreApplication, so no HWND ever
//    matches its pid: the callback never fires, EnumWindows returns success,
//    and request_exit reports NO ERROR while doing nothing at all. Every
//    module would then burn the full 5s grace period and be TerminateProcess'd,
//    skipping the destructor chain that unlinks its QtRO endpoint.
//
//    The working equivalent is PostThreadMessage(WM_QUIT) to the child's MAIN
//    thread, because Qt's own Win32 dispatcher turns that into
//    QCoreApplication::quit() (qeventdispatcher_win.cpp: "else if
//    (msg.message == WM_QUIT) ... instance()->quit()"), which is precisely
//    what the POSIX build's SIGTERM self-pipe achieves. That needs the child's
//    thread id, which bp2 discards -- hence the launcher hook below.
//
// 2. ORPHAN REAPING. There is no prctl(PR_SET_PDEATHSIG). The Win32 answer is
//    a Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: children assigned
//    to it die when the last handle to the job closes, which happens
//    automatically when this process exits -- crash included. One job for the
//    whole container, created on first use.
// ---------------------------------------------------------------------------

HANDLE containerJob() {
    static HANDLE job = [] () -> HANDLE {
        HANDLE h = ::CreateJobObjectW(nullptr, nullptr);
        if (h == nullptr) {
            spdlog::warn("CreateJobObject failed ({}); orphaned module processes "
                         "will not be reaped if this process dies abruptly",
                         ::GetLastError());
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(h, JobObjectExtendedLimitInformation,
                                       &li, sizeof(li))) {
            spdlog::warn("SetInformationJobObject failed ({}); continuing without "
                         "kill-on-close", ::GetLastError());
        }
        return h;
    }();
    return job;
}

// bp2 initializer. on_setup runs before CreateProcessW, on_success after it
// and -- importantly -- BEFORE the launcher closes hThread, which is the only
// window in which the thread id and thread handle are still available.
struct WindowsChildSetup {
    DWORD* out_thread_id;

    template <typename Launcher>
    boost::system::error_code on_setup(Launcher& l, const bp2::filesystem::path&,
                                       std::wstring&) {
        // Start suspended so the child is assigned to the job BEFORE it can
        // run and spawn any grandchildren of its own; otherwise those escape.
        l.creation_flags |= CREATE_SUSPENDED;
        return {};
    }

    template <typename Launcher>
    void on_success(Launcher& l, const bp2::filesystem::path&, std::wstring&) {
        const PROCESS_INFORMATION& pi = l.process_information;
        if (HANDLE job = containerJob())
            if (!::AssignProcessToJobObject(job, pi.hProcess))
                spdlog::warn("AssignProcessToJobObject failed ({})", ::GetLastError());
        if (out_thread_id) *out_thread_id = pi.dwThreadId;
        // Undo CREATE_SUSPENDED. If this fails the child never runs, so log
        // loudly rather than leaving a mystery hang.
        if (::ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
            spdlog::error("ResumeThread failed ({}); child will not start",
                          ::GetLastError());
    }
};
#endif  // _WIN32

// ---------------------------------------------------------------------------
// Synchronous kill
// ---------------------------------------------------------------------------

void syncKill(std::shared_ptr<ProcessEntry> entry) {
    if (!entry) return;

    entry->cancelled.store(true);

    boost::system::error_code ec;
    entry->out_pipe.close(ec);
    entry->err_pipe.close(ec);
    // Close the stdin write end too: if we kill the child before a token was
    // delivered, this gives it EOF on fd 0 so a blocking token read returns
    // instead of hanging until the wait deadline.
    entry->in_pipe.close(ec);

#ifdef _WIN32
    // NOT request_exit(): on Windows that is EnumWindows + WM_CLOSE, which a
    // windowless QCoreApplication never receives -- it would silently succeed
    // and do nothing, costing the full grace period per module. WM_QUIT to the
    // main thread is what Qt actually turns into QCoreApplication::quit().
    if (entry->main_thread_id != 0) {
        if (!::PostThreadMessageW(entry->main_thread_id, WM_QUIT, 0, 0))
            spdlog::warn("PostThreadMessage(WM_QUIT) failed for {} ({}); "
                         "falling back to terminate", entry->name, ::GetLastError());
    } else {
        spdlog::warn("No main thread id recorded for {}; cannot request a "
                     "graceful exit, will terminate", entry->name);
    }
#else
    entry->process.request_exit(ec);
#endif

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

    // Tell the host where to read its auth token: this container delivers it
    // over the child's stdin (see sendTokenToProcess). Token delivery is the
    // container's responsibility — the host stays agnostic and just reads the
    // channel we name here. A different container would name a different one.
    std::vector<std::string> launchArgs = args;
    launchArgs.push_back("--token-source");
    launchArgs.push_back("stdin");

    if (!startProcess(desc.name, hostBinary, launchArgs, callbacks))
        return false;

    out.name = desc.name;
    out.pid  = getProcessId(desc.name);
    return true;
}

bool SubprocessContainer::sendToken(const std::string& name, const std::string& token)
{
    return sendTokenToProcess(name, token);
}

LogosCore::LoadOutcome SubprocessContainer::awaitLoad(const std::string& name,
                                                      std::chrono::milliseconds timeout)
{
    std::shared_ptr<ProcessEntry> entry;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        auto it = s_processes.find(name);
        if (it != s_processes.end())
            entry = it->second;
    }

    // The entry is erased when the child dies, so its absence here means the
    // process is already gone -- whatever else is true, the module is not there.
    if (!entry)
        return {LogosCore::LoadVerdict::Failed,
                "the module process exited before it reported that it had loaded"};

    // The entry outlives its removal from s_processes through this shared_ptr,
    // so a verdict that settles while we wait still reaches us.
    std::unique_lock<std::mutex> lock(entry->status_mutex);
    entry->status_cv.wait_for(lock, timeout,
                              [&entry] { return entry->status.has_value(); });
    if (entry->status)
        return *entry->status;

    // Alive and silent: a host too old to report, or one still starting. Not
    // evidence of failure.
    return {};
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

    // Child stdin: the parent keeps in_wpipe and writes the auth token to it in
    // sendTokenToProcess; the child inherits in_rpipe as fd 0 and reads its
    // token from stdin (see --token-source). A private inherited pipe with no
    // filesystem name, so there is nothing for a co-tenant to squat or
    // authenticate against — this replaces the old predictable-socket handoff.
    asio::readable_pipe in_rpipe(rt.ctx);
    asio::writable_pipe in_wpipe(rt.ctx);

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
    asio::connect_pipe(in_rpipe, in_wpipe, ec);
    if (ec) {
        spdlog::error("Failed to create stdin pipe for {}: {}",
                                     name, ec.message());
        return false;
    }

#ifdef _WIN32
    bp2::process_stdio pstdio;
    pstdio.in  = in_rpipe;
    pstdio.out = out_wpipe;
    pstdio.err = err_wpipe;

    // The extra initializer assigns the child to the container job and hands
    // back its main thread id, which is what makes a graceful stop possible
    // (see the WindowsChildSetup comment).
    DWORD childMainThread = 0;
    bp2::process proc = bp2::default_process_launcher()(
        rt.ctx, ec, executable, arguments, pstdio,
        WindowsChildSetup{&childMainThread});
#else
    // Only the three the child inherits as its stdio survive exec, and only
    // because dup2 clears the flag on the target.
    if (!markCloexec({out_rpipe.native_handle(), out_wpipe.native_handle(),
                      err_rpipe.native_handle(), err_wpipe.native_handle(),
                      in_rpipe.native_handle(),  in_wpipe.native_handle()})) {
        spdlog::error("Failed to mark pipes close-on-exec for {}: {}", name,
                      boost::system::error_code(errno, boost::system::system_category()).message());
        return false;
    }

    pid_t child = -1;
    if (const int e = spawnChild(executable, arguments,
                                 in_rpipe.native_handle(),
                                 out_wpipe.native_handle(),
                                 err_wpipe.native_handle(), child)) {
        spdlog::error("Failed to start process for {}: {}", name,
                      boost::system::error_code(e, boost::system::system_category()).message());
        return false;
    }
    // The attach bp2's own launcher ends with, so nothing downstream differs.
    bp2::process proc(rt.ctx.get_executor(), child);
#endif

    out_wpipe.close();
    err_wpipe.close();
    in_rpipe.close();   // child holds its own copy; parent only needs in_wpipe

    if (ec) {
        spdlog::error("Failed to start process for {}: {}",
                                     name, ec.message());
        return false;
    }

    auto entry = std::make_shared<ProcessEntry>(
        std::move(proc), std::move(out_rpipe), std::move(err_rpipe),
        std::move(in_wpipe), name, callbacks);
#ifdef _WIN32
    entry->main_thread_id = childMainThread;
#endif

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
                                              int /*max_wait_ms*/)
{
    // Deliver the token over the child's stdin pipe, set up in startProcess().
    // The child inherited the read end as fd 0 and blocks reading its token
    // there (see --token-source stdin in logos_host). This pipe is private to
    // the parent/child pair and has no filesystem name, so there is no
    // predictable path to squat and no peer to authenticate — the whole
    // CWE-940 / F-012 socket-handoff hardening is unnecessary by construction.
    //
    // A trailing newline frames the token so the child can read exactly one
    // line; we then close our write end (EOF) to release the child even if it
    // reads to end-of-stream.
    std::shared_ptr<ProcessEntry> entry;
    {
        std::lock_guard<std::mutex> lock(s_processesMutex);
        auto it = s_processes.find(name);
        if (it != s_processes.end())
            entry = it->second;
    }

    if (!entry) {
        spdlog::error("No process entry to deliver token to for: {}", name);
        return false;
    }

    std::string payload = token;
    payload.push_back('\n');

    boost::system::error_code ec;
    boost::asio::write(entry->in_pipe, boost::asio::buffer(payload), ec);

    // Close the write end so the child sees EOF after the token. Best-effort:
    // even if the close reports an error the token bytes were already written.
    boost::system::error_code close_ec;
    entry->in_pipe.close(close_ec);

    if (ec) {
        spdlog::error("Failed to write token to stdin pipe for {}: {}",
                      name, ec.message());
        std::shared_ptr<ProcessEntry> dead;
        {
            std::lock_guard<std::mutex> lock(s_processesMutex);
            auto it = s_processes.find(name);
            if (it != s_processes.end()) {
                dead = it->second;
                s_processes.erase(it);
            }
        }
        syncKill(dead);
        return false;
    }

    return true;
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

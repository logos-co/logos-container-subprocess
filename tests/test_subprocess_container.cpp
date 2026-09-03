// =============================================================================
// Tests for SubprocessContainer — the ModuleContainer implementation that
// manages child processes via Boost.Process v2.
//
// Verifies the container interface methods (launch, sendToken, terminate,
// hasModule, pid, getAllPids) independently from any ModuleFormatLoader.
// =============================================================================
#include <gtest/gtest.h>
#include "subprocess_container.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <cerrno>
#include <future>
#include <pthread.h>
#include <unistd.h>
#endif

#ifndef _WIN32
// Defined with the fork tests at the end of this file. Declared here because
// the fixture disarms the trap unconditionally: a test that fails between
// arming and releasing must not leave it armed for the next one.
void disarmAtforkTrap();
#endif

class SubprocessContainerTest : public ::testing::Test {
protected:
    SubprocessContainer container;

    void SetUp() override { SubprocessContainer::clearAll(); }
    void TearDown() override {
        SubprocessContainer::clearAll();
#ifndef _WIN32
        disarmAtforkTrap();   // never leave it armed for the next test
#endif
    }
};

// Launch a fake module host under `name` that stays alive ~5s. It must tolerate
// the extra CLI args the container appends in launch() (e.g. --token-source
// stdin): /bin/sleep can't be used directly because it rejects unknown options,
// so we run a shell that execs sleep and ignores the trailing args. exec keeps
// the pid stable (the shell becomes sleep), matching what the pid assertions
// expect.
static bool launchFakeModule(SubprocessContainer& c, const char* name,
                             LogosCore::LoadedModuleHandle& handle) {
    LogosCore::ModuleDescriptor desc;
    desc.name = name;
    return c.launch(desc, "/bin/sh", {"-c", "exec sleep 5"}, nullptr, handle);
}

// ---------------------------------------------------------------------------
// canHandle: subprocess container accepts any module descriptor
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, CanHandle_AcceptsAnyDescriptor) {
    LogosCore::ModuleDescriptor desc;
    desc.format = "qt-plugin";
    EXPECT_TRUE(container.canHandle(desc));

    desc.format = "wasm";
    EXPECT_TRUE(container.canHandle(desc));

    desc.format = "";
    EXPECT_TRUE(container.canHandle(desc));
}

TEST_F(SubprocessContainerTest, Id_ReturnsSubprocess) {
    EXPECT_EQ(container.id(), "subprocess");
}

// ---------------------------------------------------------------------------
// launch: spawns a real child and populates the handle
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, Launch_StartsProcessAndPopulatesHandle) {
    LogosCore::LoadedModuleHandle handle;
    bool ok = launchFakeModule(container, "launch_test", handle);
    EXPECT_TRUE(ok);
    EXPECT_EQ(handle.name, "launch_test");
    EXPECT_GT(handle.pid, 0);
}

TEST_F(SubprocessContainerTest, Launch_HasModuleReturnsTrueAfterLaunch) {
    LogosCore::LoadedModuleHandle handle;
    launchFakeModule(container, "has_mod", handle);
    EXPECT_TRUE(container.hasModule("has_mod"));
}

TEST_F(SubprocessContainerTest, Launch_PidReturnsPidAfterLaunch) {
    LogosCore::LoadedModuleHandle handle;
    launchFakeModule(container, "pid_mod", handle);
    auto pid = container.pid("pid_mod");
    ASSERT_TRUE(pid.has_value());
    EXPECT_GT(*pid, 0);
    EXPECT_EQ(*pid, handle.pid);
}

TEST_F(SubprocessContainerTest, Launch_FailsForNonexistentBinary) {
    LogosCore::ModuleDescriptor desc;
    desc.name = "bad_launch";
    LogosCore::LoadedModuleHandle handle;

    bool ok = container.launch(desc, "/nonexistent/binary", {}, nullptr, handle);
    EXPECT_FALSE(ok);
}

// ---------------------------------------------------------------------------
// terminate
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, Terminate_RemovesModule) {
    LogosCore::LoadedModuleHandle handle;
    launchFakeModule(container, "term_mod", handle);
    ASSERT_TRUE(container.hasModule("term_mod"));

    container.terminate("term_mod");
    EXPECT_FALSE(container.hasModule("term_mod"));
}

TEST_F(SubprocessContainerTest, TerminateAll_RemovesAllModules) {
    LogosCore::LoadedModuleHandle h1;
    launchFakeModule(container, "ta_1", h1);
    LogosCore::LoadedModuleHandle h2;
    launchFakeModule(container, "ta_2", h2);

    container.terminateAll();

    EXPECT_FALSE(container.hasModule("ta_1"));
    EXPECT_FALSE(container.hasModule("ta_2"));
}

// ---------------------------------------------------------------------------
// getAllPids
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, GetAllPids_ReturnsAllRunningPids) {
    LogosCore::LoadedModuleHandle h1;
    launchFakeModule(container, "gp_a", h1);
    LogosCore::LoadedModuleHandle h2;
    launchFakeModule(container, "gp_b", h2);

    auto pids = container.getAllPids();
    EXPECT_EQ(pids.size(), 2u);
    EXPECT_GT(pids.at("gp_a"), 0);
    EXPECT_GT(pids.at("gp_b"), 0);
    EXPECT_NE(pids.at("gp_a"), pids.at("gp_b"));
}

// ---------------------------------------------------------------------------
// onOutput callback via launch
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, Launch_OutputCallbackReceivesStdoutAndStderr) {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<std::pair<std::string, bool>> lines;
    std::atomic<bool> terminated{false};

    LogosCore::ModuleDescriptor desc;
    desc.name = "output_test";
    LogosCore::LoadedModuleHandle handle;

    // We can't easily capture onOutput through the container interface alone
    // since launch() sets its own callbacks. Instead, use the low-level API.
    SubprocessContainer::ProcessCallbacks cb;
    cb.onOutput = [&](const std::string&, const std::string& line, bool isStderr) {
        std::lock_guard<std::mutex> lock(mtx);
        lines.emplace_back(line, isStderr);
        cv.notify_all();
    };
    cb.onFinished = [&](const std::string&, int, bool) {
        terminated.store(true);
        cv.notify_all();
    };

    ASSERT_TRUE(SubprocessContainer::startProcess(
        "output_test", "/bin/sh", {"-c", "echo out-line; echo err-line >&2"}, cb));

    std::unique_lock<std::mutex> lock(mtx);
    cv.wait_for(lock, std::chrono::seconds(5),
                [&]() { return terminated.load() && lines.size() >= 2; });

    ASSERT_GE(lines.size(), 2u);
    bool saw_stdout = false, saw_stderr = false;
    for (auto& [line, isStderr] : lines) {
        if (!isStderr && line == "out-line") saw_stdout = true;
        if ( isStderr && line == "err-line") saw_stderr = true;
    }
    EXPECT_TRUE(saw_stdout);
    EXPECT_TRUE(saw_stderr);
}

// ---------------------------------------------------------------------------
// Output relay is bounded — a module cannot OOM/stall the host via stdout
// ---------------------------------------------------------------------------
//
// F-014: the parent (trusted host) relays each child's stdout/stderr to
// onOutput line-by-line, buffering bytes until a '\n' arrives. A
// partially-trusted module that emits a long *newline-free* stream (or one
// giant write) would, without a cap, grow the per-stream line buffer without
// bound — pinning host memory until the OS OOM-kills basecamp/logoscore and
// every module it supervises — while each 4 KB read re-scanned the whole
// accumulated buffer for a newline (O(N^2) CPU on the single shared io
// thread). Process isolation exists precisely so a module fault cannot take
// the host down; an unbounded relay buffer breaks that guarantee.
//
// This test drives a child that writes ~5 MB to stdout with NO newline, then
// closes it. The contract: the host must surface that output in bounded
// pieces (each <= kMaxOutputLineBytes plus at most one read chunk), never as a
// single unbounded line. Against the pre-fix code the entire payload is
// accumulated and emitted as one ~5 MB line, which fails the per-piece bound.

TEST_F(SubprocessContainerTest, Launch_BoundsUnterminatedOutputLine) {
    // The child writes a fixed number of newline-free bytes via dd. Skip
    // cleanly if /dev/zero isn't available (some constrained sandboxes).
    if (access("/dev/zero", R_OK) != 0)
        GTEST_SKIP() << "/dev/zero not available";

    // ~5 MB of newline-free output: several times the 1 MiB per-line cap, and
    // deliberately not an exact multiple of it so the final EOF flush carries
    // a non-trivial remainder too.
    constexpr std::size_t kTotalBytes = 5'000'000;

    std::mutex mtx;
    std::condition_variable cv;
    std::size_t received_total = 0;
    std::size_t max_piece = 0;
    std::size_t piece_count = 0;
    std::atomic<bool> terminated{false};

    SubprocessContainer::ProcessCallbacks cb;
    cb.onOutput = [&](const std::string&, const std::string& line, bool isStderr) {
        if (isStderr) return; // dd's own summary is silenced, but be defensive
        std::lock_guard<std::mutex> lock(mtx);
        received_total += line.size();
        max_piece = std::max(max_piece, line.size());
        ++piece_count;
        cv.notify_all();
    };
    cb.onFinished = [&](const std::string&, int, bool) {
        terminated.store(true);
        cv.notify_all();
    };

    // bs=1000000 count=5 => exactly 5,000,000 NUL bytes on stdout, no newline.
    // dd's transfer summary goes to stderr, which we discard.
    ASSERT_TRUE(SubprocessContainer::startProcess(
        "oom_relay_test", "/bin/sh",
        {"-c", "dd if=/dev/zero bs=1000000 count=5 2>/dev/null"}, cb));

    {
        std::unique_lock<std::mutex> lock(mtx);
        // Wait until every byte has been relayed (the meaningful condition);
        // onFinished may fire slightly before the final pipe read drains.
        bool ok = cv.wait_for(lock, std::chrono::seconds(15),
                              [&]() { return received_total >= kTotalBytes; });
        ASSERT_TRUE(ok) << "host relayed only " << received_total << " of "
                        << kTotalBytes << " bytes before timing out";
    }

    std::lock_guard<std::mutex> lock(mtx);

    // No data lost or duplicated: every byte the child wrote is surfaced.
    EXPECT_EQ(received_total, kTotalBytes);

    // The core invariant: a single newline-free stream is delivered in bounded
    // pieces, not one unbounded line. A flush is triggered once the buffer
    // reaches the cap, after appending at most one read chunk (4096 bytes), so
    // no emitted piece may exceed cap + one chunk.
    EXPECT_LE(max_piece, SubprocessContainer::kMaxOutputLineBytes + 4096u)
        << "host buffered a " << max_piece << "-byte line without a newline; "
           "the per-stream relay buffer is unbounded (F-014) — a module can "
           "OOM/stall the trusted host through stdout.";

    // ~5 MB at a 1 MiB cap must split into several pieces. Pre-fix this is
    // exactly one (everything accumulated, emitted once at EOF).
    EXPECT_GT(piece_count, 1u)
        << "expected the capped relay to split a 5 MB newline-free stream into "
           "multiple bounded pieces, got " << piece_count;
}

// ---------------------------------------------------------------------------
// Crash detection — the kernel of crash isolation
// ---------------------------------------------------------------------------
//
// When a child dies on a signal, the container must surface it via
// onFinished(name, exit_code, crashed=true) and tear down its own
// bookkeeping. Every observer above (composite_module_loader → module_manager's
// onTerminated → registry.markUnloaded → daemon → CLI) hangs off this
// callback — if it stops firing with crashed=true, isolation breaks
// silently. The end-to-end test in logos-logoscore-cli covers the full
// stack at ~13s; this one pins the mechanism at its source in ~50ms.

TEST_F(SubprocessContainerTest, Launch_OnFinishedReportsCrashedOnSignal) {
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> fired{false};
    std::string gotName;
    int gotExitCode = -1;
    bool gotCrashed = false;

    SubprocessContainer::ProcessCallbacks cb;
    cb.onFinished = [&](const std::string& n, int code, bool crashed) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            gotName = n;
            gotExitCode = code;
            gotCrashed = crashed;
        }
        fired.store(true);
        cv.notify_all();
    };

    // `kill -SEGV $$` makes the shell signal itself — a portable way to
    // exercise WIFSIGNALED without depending on /bin/kill -s SEGV syntax
    // or shipping a custom helper binary.
    ASSERT_TRUE(SubprocessContainer::startProcess(
        "crash_test", "/bin/sh", {"-c", "kill -SEGV $$"}, cb));

    std::unique_lock<std::mutex> lock(mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&]() { return fired.load(); }))
        << "onFinished never fired for a signaled child";

    EXPECT_EQ(gotName, "crash_test");
    EXPECT_TRUE(gotCrashed)
        << "container must report crashed=true on signal-exit (WIFSIGNALED). "
           "Without this, the markUnloaded propagation up the stack stalls "
           "and a crashed module looks 'loaded' forever.";
    EXPECT_EQ(gotExitCode, SIGSEGV)
        << "exit_code must carry the signal number (WTERMSIG), not the "
           "raw waitpid status. Got " << gotExitCode << ".";

    // Bookkeeping side of the contract: once onFinished has fired the
    // container must no longer claim the module exists. This is what
    // feeds upstream observers via their own ProcessEntry cleanup.
    EXPECT_FALSE(container.hasModule("crash_test"));
    EXPECT_FALSE(container.pid("crash_test").has_value());
}

// The other half of the same contract, and the one nothing covered: a child
// that EXITS must not be described as one that was signalled. Boost.Process v2
// hands async_wait an exit code that is already evaluated -- WEXITSTATUS for an
// exit, WTERMSIG for a signal -- so re-decoding it as a wait status makes every
// exit code from 1..126 look like a fatal signal. The signal test above passes
// either way (WTERMSIG(11) == 11), which is why this went unseen; the daemon
// meanwhile logged "Module process crashed" for a module host that exited 1
// because its plugin would not load.
TEST_F(SubprocessContainerTest, Launch_OnFinishedReportsPlainExitAsNotCrashed) {
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> fired{false};
    int gotExitCode = -1;
    bool gotCrashed = true;

    SubprocessContainer::ProcessCallbacks cb;
    cb.onFinished = [&](const std::string&, int code, bool crashed) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            gotExitCode = code;
            gotCrashed = crashed;
        }
        fired.store(true);
        cv.notify_all();
    };

    ASSERT_TRUE(SubprocessContainer::startProcess(
        "exit_test", "/bin/sh", {"-c", "exit 3"}, cb));

    std::unique_lock<std::mutex> lock(mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5),
                            [&]() { return fired.load(); }));

    EXPECT_FALSE(gotCrashed);
    EXPECT_EQ(gotExitCode, 3);
}

// ---------------------------------------------------------------------------
// Token delivery over the child's stdin pipe
//
// sendToken/sendTokenToProcess writes the auth token to the child's stdin (a
// private inherited pipe, set up in startProcess) followed by a newline, then
// closes the write end. The child reads its token from fd 0. This replaced the
// old predictable-socket handoff; here we prove the token actually arrives by
// spawning a shell that reads one line from stdin and echoes it back on
// stdout, which the container relays to onOutput.
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, SendToken_DeliversTokenOverStdin) {
    std::mutex mtx;
    std::condition_variable cv;
    std::string received;
    std::atomic<bool> got{false};

    SubprocessContainer::ProcessCallbacks cb;
    cb.onOutput = [&](const std::string&, const std::string& line, bool isStderr) {
        if (isStderr) return;
        std::lock_guard<std::mutex> lock(mtx);
        if (line.rfind("GOT:", 0) == 0) {       // line starts with "GOT:"
            received = line.substr(4);
            got.store(true);
            cv.notify_all();
        }
    };

    // `read tok` consumes exactly the one newline-terminated line the parent
    // writes; the child then echoes it so the test can observe what arrived.
    ASSERT_TRUE(SubprocessContainer::startProcess(
        "tok_stdin", "/bin/sh", {"-c", "read tok; echo \"GOT:$tok\""}, cb));

    const std::string secret = "f47ac10b-58cc-4372-a567-0e02b2c3d479";
    EXPECT_TRUE(SubprocessContainer::sendTokenToProcess("tok_stdin", secret));

    std::unique_lock<std::mutex> lock(mtx);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return got.load(); }))
        << "child never echoed a token read from stdin";
    EXPECT_EQ(received, secret)
        << "token delivered over stdin did not match what the parent sent";
}

TEST_F(SubprocessContainerTest, SendToken_FailsForUnknownProcess) {
    // No entry registered for this name → nothing to write the token to.
    EXPECT_FALSE(SubprocessContainer::sendTokenToProcess("no_such_module", "tok"));
}

// ---------------------------------------------------------------------------
// pid/hasModule for unknown names
// ---------------------------------------------------------------------------

TEST_F(SubprocessContainerTest, Pid_ReturnsNulloptForUnknown) {
    EXPECT_FALSE(container.pid("nonexistent").has_value());
}

TEST_F(SubprocessContainerTest, HasModule_ReturnsFalseForUnknown) {
    EXPECT_FALSE(container.hasModule("nonexistent"));
}

TEST_F(SubprocessContainerTest, Terminate_NoopForUnknown) {
    container.terminate("nonexistent");
    SUCCEED();
}

// ---------------------------------------------------------------------------
// awaitLoad: the child's own verdict on whether its plugin loaded
//
// launch() answers "spawned". These cover the fact it cannot reach: what the
// child then did. A verdict must arrive as soon as the child settles it, so
// each of these asserts on elapsed time too -- a deadline reached is the one
// answer that means "nothing was learned", and it must not be reachable by a
// child that already spoke or already died.
// ---------------------------------------------------------------------------

namespace {

LogosCore::LoadOutcome awaitAfterLaunch(SubprocessContainer& c, const char* name,
                                        const std::string& script,
                                        std::chrono::milliseconds timeout,
                                        std::chrono::milliseconds& elapsed) {
    LogosCore::ModuleDescriptor desc;
    desc.name = name;
    LogosCore::LoadedModuleHandle handle;
    if (!c.launch(desc, "/bin/sh", {"-c", script}, nullptr, handle))
        return {LogosCore::LoadVerdict::Failed, "launch failed"};

    const auto start = std::chrono::steady_clock::now();
    LogosCore::LoadOutcome out = c.awaitLoad(name, timeout);
    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    return out;
}

} // namespace

TEST_F(SubprocessContainerTest, AwaitLoad_ReportsLoadedWhenTheChildSaysSo) {
    std::chrono::milliseconds elapsed{};
    const auto out = awaitAfterLaunch(
        container, "ok_mod",
        "printf '%s\\n' '@logos-load-status ok'; exec sleep 5",
        std::chrono::seconds(5), elapsed);

    EXPECT_EQ(out.verdict, LogosCore::LoadVerdict::Loaded);
    EXPECT_LT(elapsed, std::chrono::seconds(4));
}

TEST_F(SubprocessContainerTest, AwaitLoad_CarriesTheReasonTheChildReported) {
    std::chrono::milliseconds elapsed{};
    const auto out = awaitAfterLaunch(
        container, "failed_mod",
        "printf '%s\\n' '@logos-load-status failed undefined symbol: logos_module_install'; exit 1",
        std::chrono::seconds(5), elapsed);

    EXPECT_EQ(out.verdict, LogosCore::LoadVerdict::Failed);
    EXPECT_NE(out.reason.find("undefined symbol: logos_module_install"),
              std::string::npos);
    EXPECT_LT(elapsed, std::chrono::seconds(4));
}

// A child that dies without a word is still a failed load, and the exit code is
// the only reason available to describe it.
TEST_F(SubprocessContainerTest, AwaitLoad_ReportsFailureWhenTheChildJustDies) {
    std::chrono::milliseconds elapsed{};
    const auto out = awaitAfterLaunch(container, "dead_mod", "exit 3",
                                      std::chrono::seconds(5), elapsed);

    EXPECT_EQ(out.verdict, LogosCore::LoadVerdict::Failed);
    EXPECT_NE(out.reason.find("3"), std::string::npos);
    EXPECT_LT(elapsed, std::chrono::seconds(4));
}

// Alive and silent at the deadline is not evidence of failure: it is what a
// module host too old to report the line looks like.
TEST_F(SubprocessContainerTest, AwaitLoad_ReportsUnknownForASilentLiveChild) {
    std::chrono::milliseconds elapsed{};
    const auto out = awaitAfterLaunch(container, "silent_mod", "exec sleep 5",
                                      std::chrono::milliseconds(300), elapsed);

    EXPECT_EQ(out.verdict, LogosCore::LoadVerdict::Unknown);
    EXPECT_TRUE(out.reason.empty());
    EXPECT_GE(elapsed, std::chrono::milliseconds(250));
}

TEST_F(SubprocessContainerTest, AwaitLoad_ReportsFailureForAModuleThatWasNeverLaunched) {
    const auto out = container.awaitLoad("never_launched", std::chrono::milliseconds(10));
    EXPECT_EQ(out.verdict, LogosCore::LoadVerdict::Failed);
    EXPECT_FALSE(out.reason.empty());
}

// The status line is protocol, not module output: relaying it would put it in
// the module's log stream on every single load.
TEST_F(SubprocessContainerTest, AwaitLoad_StatusLineIsNotRelayedAsModuleOutput) {
    std::mutex m;
    std::vector<std::string> lines;
    SubprocessContainer::ProcessCallbacks cbs;
    cbs.onOutput = [&](const std::string&, const std::string& line, bool) {
        std::lock_guard<std::mutex> g(m);
        lines.push_back(line);
    };

    ASSERT_TRUE(SubprocessContainer::startProcess(
        "quiet_mod", "/bin/sh",
        {"-c", "printf '%s\\n' '@logos-load-status ok' 'hello'; exec sleep 5"}, cbs));

    const auto out = container.awaitLoad("quiet_mod", std::chrono::seconds(5));
    ASSERT_EQ(out.verdict, LogosCore::LoadVerdict::Loaded);

    // The plain line still has to get through; only the protocol one is eaten.
    for (int i = 0; i < 100; ++i) {
        {
            std::lock_guard<std::mutex> g(m);
            if (!lines.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::lock_guard<std::mutex> g(m);
    EXPECT_EQ(lines, std::vector<std::string>{"hello"});
}

#ifndef _WIN32
// ---------------------------------------------------------------------------
// The launcher must not fork()
//
// A forked child inherits asio's locks with no owner and wedges before execve,
// while the parent blocks on an untimed exec-status read -- see spawnChild.
// These two assert that, the first on the mechanism and the second on the
// symptom, both deterministically: pthread_atfork handlers run for fork() and
// are NOT run for posix_spawn.
// ---------------------------------------------------------------------------

namespace {

std::atomic<int>  g_atfork_prepare{0};
std::atomic<int>  g_atfork_parent{0};
std::atomic<bool> g_trap_armed{false};
std::atomic<int>  g_trap_fd{-1};

void onAtforkPrepare() { g_atfork_prepare.fetch_add(1); }
void onAtforkParent()  { g_atfork_parent.fetch_add(1); }

// Stands in for any lock inherited in the locked state: same window, timing
// made certain. read() is async-signal-safe, so this is legal in a child
// handler in a way that taking a mutex would not be.
void onAtforkChild()
{
    if (!g_trap_armed.load()) return;
    char c = 0;
    while (::read(g_trap_fd.load(), &c, 1) == -1 && errno == EINTR) {}
}

}  // namespace

void disarmAtforkTrap() { g_trap_armed.store(false); }

namespace {

// pthread_atfork handlers cannot be unregistered, hence the armed flag.
int ensureAtforkHandlers()
{
    static const int rc =
        ::pthread_atfork(&onAtforkPrepare, &onAtforkParent, &onAtforkChild);
    return rc;
}

}  // namespace

// This encodes an opinion worth stating out loud: a fix that keeps fork() and
// merely puts a deadline on the exec-status read would fail here. That is
// intended -- a deadline turns a hang into a spurious launch failure plus a
// wedged child that nobody reaps.
TEST_F(SubprocessContainerTest, Launch_DoesNotFork) {
    ASSERT_EQ(ensureAtforkHandlers(), 0)
        << "pthread_atfork registration failed; this test would pass vacuously";

    // Positive control: without it, a handler that never runs is
    // indistinguishable from a launcher that never forks.
    g_atfork_prepare.store(0);
    const pid_t control = ::fork();
    if (control == 0) ::_exit(0);
    ASSERT_GT(control, 0);
    ::waitpid(control, nullptr, 0);
    ASSERT_EQ(g_atfork_prepare.load(), 1) << "the handlers are not live";

    g_atfork_prepare.store(0);
    g_atfork_parent.store(0);

    LogosCore::LoadedModuleHandle handle;
    ASSERT_TRUE(launchFakeModule(container, "no_fork", handle));

    EXPECT_EQ(g_atfork_prepare.load(), 0)
        << "the launcher forked; a forked child inherits asio's service and "
           "reactor mutexes locked with no owner and can never reach execve";
    EXPECT_EQ(g_atfork_parent.load(), 0);
}

// The symptom: a child wedged before exec must not hang the parent. Under a
// forking launcher this never returns; the 5 s bound is ~1000x a healthy
// launch, so it cannot flake.
TEST_F(SubprocessContainerTest, Launch_DoesNotBlockOnAWedgedChild) {
    ensureAtforkHandlers();

    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);
    g_trap_fd.store(fds[0]);
    g_trap_armed.store(true);

    // shared_ptr so a detached worker on the failure path cannot dangle.
    auto done = std::make_shared<std::promise<bool>>();
    auto fut = done->get_future();
    std::thread worker([this, done] {
        LogosCore::LoadedModuleHandle handle;
        try {
            done->set_value(launchFakeModule(container, "wedged", handle));
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });

    const bool returned = fut.wait_for(std::chrono::seconds(5)) ==
                          std::future_status::ready;

    // Release BEFORE asserting, so a red run frees its child rather than
    // leaving one wedged for the rest of the suite.
    g_trap_armed.store(false);
    const ssize_t released = ::write(fds[1], "x", 1);
    EXPECT_EQ(released, 1);

    // Bounded: the byte above only frees a child blocked in onAtforkChild, and
    // an unbounded join on any other wedge would hang the whole run with no
    // output.
    if (returned || fut.wait_for(std::chrono::seconds(5)) ==
                        std::future_status::ready) {
        worker.join();
    } else {
        worker.detach();
    }
    ::close(fds[0]);
    ::close(fds[1]);
    g_trap_fd.store(-1);

    ASSERT_TRUE(returned)
        << "launch() did not return within 5s: the launcher forked, the child "
           "wedged before execve, and the parent is on an untimed read of the "
           "exec-status pipe";
    // Otherwise a launch that failed outright would satisfy the timing bound.
    EXPECT_TRUE(fut.get()) << "launch returned quickly because it FAILED, so "
                              "the timing assertion above proved nothing";
}
#endif  // !_WIN32

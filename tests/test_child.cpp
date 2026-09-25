// The child process the container tests launch, the same executable on every
// platform. It runs its arguments as a script, in order, and ignores what it
// does not know, such as the options the container appends to a module launch:
//
//   print TEXT    TEXT and a newline on stdout
//   streams       "out-line" on stdout and "err-line" on stderr
//   flood N       N NUL bytes on stdout, no newline
//   token         read a line from stdin, print "GOT:" and it
//   sleep S       stay up S seconds, or until asked to stop
//   exit N        exit with code N
//   crash         die on an access violation
//   late-queue MS (Windows, first only) take a message queue MS late
//   pdeathsig     (Linux) die with the thread that started this process

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace {

// Until killed or `seconds` pass. The container asks a child to stop with
// SIGTERM on Unix, which ends this process, and with WM_QUIT to its main thread
// on Windows, which a module host's event loop ends on; so does this one.
void stayUp(int seconds)
{
#ifdef _WIN32
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    for (auto now = std::chrono::steady_clock::now(); now < deadline;
         now = std::chrono::steady_clock::now()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left.count()),
                                    QS_ALLPOSTMESSAGE);
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            if (msg.message == WM_QUIT) std::exit(0);
    }
#else
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
#endif
}

// As a module host dies: SIGSEGV on Unix, an access violation on Windows.
[[noreturn]] void crash()
{
#ifdef _WIN32
    // Through a volatile, so the compiler cannot see the null and trap instead.
    volatile std::uintptr_t address = 0;
    *reinterpret_cast<volatile int*>(address) = 1;
#else
    std::raise(SIGSEGV);
#endif
    std::abort();
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> script(argv + 1, argv + argc);
    size_t i = 0;
#ifdef _WIN32
    // A thread has a message queue only once it asks for one, and the
    // container's WM_QUIT cannot be posted before then.
    if (script.size() >= 2 && script[0] == "late-queue") {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(script[1].c_str())));
        i = 2;
    }
    MSG queue;
    ::PeekMessageW(&queue, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
#endif
    for (; i < script.size(); ++i) {
        const std::string& step = script[i];
        const bool hasArg = i + 1 < script.size();
        if (step == "print" && hasArg) {
            std::cout << script[++i] << std::endl;
        } else if (step == "streams") {
            std::cout << "out-line" << std::endl;
            std::cerr << "err-line" << std::endl;
        } else if (step == "flood" && hasArg) {
            const std::string chunk(1 << 16, '\0');
            for (long long left = std::atoll(script[++i].c_str()); left > 0;) {
                const auto n = static_cast<size_t>(std::min<long long>(left, chunk.size()));
                std::cout.write(chunk.data(), static_cast<std::streamsize>(n));
                left -= static_cast<long long>(n);
            }
            std::cout.flush();
        } else if (step == "token") {
            std::string token;
            std::getline(std::cin, token);
            if (!token.empty() && token.back() == '\r') token.pop_back();
            std::cout << "GOT:" << token << std::endl;
        } else if (step == "sleep" && hasArg) {
            stayUp(std::atoi(script[++i].c_str()));
        } else if (step == "exit" && hasArg) {
            return std::atoi(script[++i].c_str());
        } else if (step == "crash") {
            crash();
        } else if (step == "pdeathsig") {
#ifdef __linux__
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        }
    }
    return 0;
}

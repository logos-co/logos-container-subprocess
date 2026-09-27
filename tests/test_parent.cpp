// A parent for the exit test: runs its arguments as a channel process, prints
// that child's pid, and calls exit(0) with the child still running.
#include <logos_container/channel_process.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    auto child = LogosCore::startChannelProcess(argv[1], {argv + 2, argv + argc}, {});
    if (!child) return 1;
    std::printf("%lld\n", static_cast<long long>(child->pid()));
    std::fflush(stdout);
    std::exit(0);
}

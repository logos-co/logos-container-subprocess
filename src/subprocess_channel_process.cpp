// Link-time provider for logos-container's channel-process seam: a child that
// is not a module, run by this container's process management, with its stdin
// and stdout as a private line channel.
#include <logos_container/channel_process.h>
#include "subprocess_container.h"

#include <atomic>
#include <utility>

namespace LogosCore {
namespace {

class SubprocessChannelProcess : public ChannelProcess {
public:
    SubprocessChannelProcess(std::string name, int64_t pid) : m_name(std::move(name)), m_pid(pid) {}
    ~SubprocessChannelProcess() override { terminate(); }

    bool writeLine(const std::string& line) override
    {
        return SubprocessContainer::writeLineToProcess(m_name, line);
    }
    void closeInput() override { SubprocessContainer::closeProcessStdin(m_name); }
    void terminate() override { SubprocessContainer::terminateProcess(m_name); }
    int64_t pid() const override { return m_pid; }

private:
    std::string m_name;
    int64_t m_pid;
};

} // namespace

std::unique_ptr<ChannelProcess> startChannelProcess(const std::string& executable,
                                                    const std::vector<std::string>& args,
                                                    ChannelCallbacks callbacks)
{
    // Not a module name, so it never meets one in this container's table.
    static std::atomic<unsigned> next{0};
    const std::string name = SubprocessContainer::kChannelProcessPrefix + std::to_string(++next);

    SubprocessContainer::ProcessCallbacks process;
    process.onOutput = [onLine = callbacks.onLine, onLog = callbacks.onLog](
                           const std::string&, const std::string& line, bool isStderr) {
        if (isStderr) {
            if (onLog) onLog(line);
        } else if (onLine) {
            onLine(line);
        }
    };
    process.onFinished = [onExit = callbacks.onExit](const std::string&, int code, bool crashed) {
        if (onExit) onExit(code, crashed);
    };
    if (!SubprocessContainer::startProcess(name, executable, args, process)) return nullptr;
    return std::make_unique<SubprocessChannelProcess>(name, SubprocessContainer::getProcessId(name));
}

} // namespace LogosCore

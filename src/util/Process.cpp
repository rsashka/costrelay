#include "util/Process.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace proxy {
namespace {

// Код выхода = 128 + номер сигнала (правило shell).
constexpr int kSignalExitBase = 128;

// Владелец файлового дескриптора: закрывает его на любом пути выхода.
class FdGuard {
  public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() { reset(); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;

    int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

  private:
    int fd_ = -1;
};

} // namespace

ProcessResult runProcess(const std::vector<std::string>& args) {
    ProcessResult result;
    if (args.empty() || args.front().empty()) {
        result.output = "empty command";
        return result;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.output = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }
    // Читающий конец живёт до конца чтения, пишущий закрывается сразу после spawn:
    // иначе дочерний процесс не получит EOF и waitpid зависнет.
    FdGuard read_fd(pipefd[0]);
    FdGuard write_fd(pipefd[1]);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    const bool actions_ok =
        posix_spawn_file_actions_init(&actions) == 0 &&
        posix_spawn_file_actions_adddup2(&actions, write_fd.get(), STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_adddup2(&actions, write_fd.get(), STDERR_FILENO) == 0 &&
        posix_spawn_file_actions_addclose(&actions, read_fd.get()) == 0 &&
        posix_spawn_file_actions_addclose(&actions, write_fd.get()) == 0;

    pid_t pid = -1;
    const int spawn_error = actions_ok
                                ? posix_spawn(&pid, args.front().c_str(), &actions, nullptr,
                                              argv.data(), environ)
                                : errno;
    if (actions_ok) posix_spawn_file_actions_destroy(&actions);
    write_fd.reset();
    if (!actions_ok) {
        result.output = std::string("spawn actions failed: ") + std::strerror(spawn_error);
        return result;
    }
    if (spawn_error != 0) {
        result.output = std::string("spawn failed: ") + std::strerror(spawn_error);
        return result;
    }

    char buffer[4096];
    bool output_truncated = false;
    int read_error = 0;
    for (;;) {
        const ssize_t count = read(read_fd.get(), buffer, sizeof(buffer));
        if (count > 0) {
            constexpr std::size_t kMaxOutput = 64 * 1024;
            if (result.output.size() < kMaxOutput) {
                const auto available = kMaxOutput - result.output.size();
                const auto take = std::min<std::size_t>(static_cast<std::size_t>(count), available);
                result.output.append(buffer, take);
                if (take < static_cast<std::size_t>(count)) output_truncated = true;
            } else {
                output_truncated = true;
            }
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        // count == 0 — конец потока; любая другая ошибка чтения не должна
        // выглядеть как успешно прочитанный вывод.
        if (count < 0) read_error = errno;
        break;
    }

    result.truncated = output_truncated;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        result.exit_code = -1;
        result.output += "waitpid failed: ";
        result.output += std::strerror(errno);
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = kSignalExitBase + WTERMSIG(status);
    }
    if (read_error != 0) {
        result.exit_code = -1;
        result.output += "read failed: ";
        result.output += std::strerror(read_error);
    }
    return result;
}

} // namespace proxy
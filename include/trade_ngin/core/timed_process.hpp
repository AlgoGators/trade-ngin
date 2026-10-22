// include/trade_ngin/core/timed_process.hpp
#pragma once

#include <chrono>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace trade_ngin {
namespace core {

/**
 * A shell command run with a time limit (HD 2026-10-10, T-DATED R4).
 *
 * The report's charts are drawn by a child process whose output the run used to read with no
 * limit: a child that never finished, and never closed its output, stopped the run for ever
 * after the day's rows were stored and before the report was sent. Here the child is given
 * `timeout`; when it has not finished by then its whole process group is killed, the caller is
 * told (`timed_out`), and the caller goes on. A child that cannot be reaped within a short grace
 * after the kill is left behind: the run never waits on it again.
 */
struct TimedProcessResult {
    bool started{false};
    bool timed_out{false};
    int status{-1};       ///< the wait status of a child that finished (0: clean exit)
    std::string output;   ///< what the child wrote to stdout (and stderr when the command joins them)
};

inline TimedProcessResult run_with_timeout(const std::string& command,
                                           std::chrono::milliseconds timeout) {
    TimedProcessResult result;
    int fds[2];
    if (pipe(fds) != 0) return result;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    result.started = true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[256];
    bool output_closed = false;
    while (!output_closed) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        struct pollfd pfd = {fds[0], POLLIN, 0};
        const int ready = poll(&pfd, 1, static_cast<int>(left.count()));
        if (ready < 0) break;
        if (ready == 0) continue;
        const ssize_t n = read(fds[0], buffer, sizeof(buffer));
        if (n > 0) {
            result.output.append(buffer, static_cast<std::size_t>(n));
        } else {
            output_closed = true;
        }
    }

    // The output is closed, or the time is up. A finished child is reaped; one still running at
    // the deadline is killed with everything it started.
    auto reap_until = [&](std::chrono::steady_clock::time_point until) {
        while (true) {
            int status = 0;
            const pid_t done = waitpid(pid, &status, WNOHANG);
            if (done == pid) {
                result.status = status;
                return true;
            }
            if (done < 0) return true;
            if (std::chrono::steady_clock::now() >= until) return false;
            usleep(10 * 1000);
        }
    };
    if (!output_closed || !reap_until(deadline)) {
        // Something the command started is still running, or still holds the output open.
        result.timed_out = true;
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        reap_until(std::chrono::steady_clock::now() + std::chrono::seconds(2));
    }
    close(fds[0]);
    return result;
}

}  // namespace core
}  // namespace trade_ngin

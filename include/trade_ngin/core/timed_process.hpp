// include/trade_ngin/core/timed_process.hpp
#pragma once

#include <chrono>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace trade_ngin {
namespace core {

/**
 * A shell command run with a time limit.
 *
 * The report's charts are drawn by a child process. Read with no limit, a child that never
 * finished, and never closed its output, would stop the run for ever after the day's rows were
 * stored and before the report was sent. Here the child is given
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

#ifdef _WIN32
// Windows: the command runs under cmd.exe in a job object, so a command that outlives the limit is
// killed with everything it started. `status` is the exit code of a child that finished.
inline TimedProcessResult run_with_timeout(const std::string& command,
                                           std::chrono::milliseconds timeout) {
    TimedProcessResult result;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return result;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string line = "cmd.exe /c " + command;
    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr,
                        nullptr, &si, &pi)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return result;
    }
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job != nullptr) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(write_end);
    result.started = true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[256];
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            if (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
                result.output.append(buffer, static_cast<std::size_t>(got));
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) {
            finished = true;
            break;
        }
    }
    if (finished) {
        DWORD available = 0;
        while (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            if (!ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) || got == 0) break;
            result.output.append(buffer, static_cast<std::size_t>(got));
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        result.status = static_cast<int>(code);
    } else {
        result.timed_out = true;
        if (job != nullptr) {
            TerminateJobObject(job, 1);
        } else {
            TerminateProcess(pi.hProcess, 1);
        }
    }
    if (job != nullptr) CloseHandle(job);
    CloseHandle(pi.hProcess);
    CloseHandle(read_end);
    return result;
}
#else
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
#endif  // _WIN32

}  // namespace core
}  // namespace trade_ngin

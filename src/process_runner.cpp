#include "ve/process_runner.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ve {
namespace {

void appendBounded(std::string& destination, const char* data, std::size_t count,
                   std::size_t maximum, bool& truncated) {
    const auto available = destination.size() < maximum ? maximum - destination.size() : 0U;
    const auto retained = std::min(available, count);
    destination.append(data, retained);
    if (retained != count) truncated = true;
}

void validateInvocation(const std::filesystem::path& executable,
                        const std::vector<std::string>& arguments,
                        std::chrono::milliseconds timeout, std::size_t maximum) {
    if (executable.empty()) throw std::invalid_argument("process executable is empty");
    if (timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("process timeout must be positive");
    }
    if (maximum == 0U) throw std::invalid_argument("process output bound must be positive");
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos) {
            throw std::invalid_argument("process argument contains a NUL byte");
        }
    }
}

#ifdef _WIN32

std::wstring utf16(std::string_view input) {
    if (input.empty()) return {};
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                            static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("process argument is not valid UTF-8");
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), count) != count) {
        throw std::runtime_error("could not convert process argument to UTF-16");
    }
    return output;
}

std::wstring quoteWindowsArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring output(1, L'"');
    std::size_t slashes = 0;
    for (const auto value : argument) {
        if (value == L'\\') {
            ++slashes;
        } else if (value == L'"') {
            output.append(slashes * 2U + 1U, L'\\');
            output.push_back(L'"');
            slashes = 0;
        } else {
            output.append(slashes, L'\\');
            slashes = 0;
            output.push_back(value);
        }
    }
    output.append(slashes * 2U, L'\\');
    output.push_back(L'"');
    return output;
}

std::wstring commandLine(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments) {
    std::wstring output = quoteWindowsArgument(executable.wstring());
    for (const auto& argument : arguments) {
        output.push_back(L' ');
        output += quoteWindowsArgument(utf16(argument));
    }
    return output;
}

struct WindowsHandle {
    HANDLE value{nullptr};
    WindowsHandle() = default;
    explicit WindowsHandle(HANDLE handle) : value(handle) {}
    WindowsHandle(const WindowsHandle&) = delete;
    WindowsHandle& operator=(const WindowsHandle&) = delete;
    WindowsHandle(WindowsHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    WindowsHandle& operator=(WindowsHandle&& other) noexcept {
        if (this != &other) {
            if (value) CloseHandle(value);
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }
    ~WindowsHandle() { if (value) CloseHandle(value); }
};

bool drainWindowsPipe(HANDLE pipe, std::string& output, std::size_t maximum, bool& truncated) {
    bool readAnything = false;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
        std::array<char, 16U * 1024U> buffer{};
        DWORD read = 0;
        const auto requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(pipe, buffer.data(), requested, &read, nullptr) || read == 0) break;
        appendBounded(output, buffer.data(), static_cast<std::size_t>(read), maximum, truncated);
        readAnything = true;
    }
    return readAnything;
}

#else

void setNonBlocking(int descriptor) {
    const auto flags = fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
}

bool drainPosixPipe(int descriptor, std::string& output, std::size_t maximum,
                    bool& truncated, bool& open) {
    bool readAnything = false;
    std::array<char, 16U * 1024U> buffer{};
    for (;;) {
        const auto count = read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            appendBounded(output, buffer.data(), static_cast<std::size_t>(count), maximum, truncated);
            readAnything = true;
            continue;
        }
        if (count == 0) {
            close(descriptor);
            open = false;
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(descriptor);
            open = false;
        }
        break;
    }
    return readAnything;
}

#endif

} // namespace

ProcessResult runProcess(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments,
                         std::chrono::milliseconds timeout,
                         std::size_t maxOutputBytes) {
    validateInvocation(executable, arguments, timeout, maxOutputBytes);
    ProcessResult result;

#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdoutReadRaw = nullptr;
    HANDLE stdoutWriteRaw = nullptr;
    HANDLE stderrReadRaw = nullptr;
    HANDLE stderrWriteRaw = nullptr;
    if (!CreatePipe(&stdoutReadRaw, &stdoutWriteRaw, &security, 0) ||
        !CreatePipe(&stderrReadRaw, &stderrWriteRaw, &security, 0)) {
        if (stdoutReadRaw) CloseHandle(stdoutReadRaw);
        if (stdoutWriteRaw) CloseHandle(stdoutWriteRaw);
        if (stderrReadRaw) CloseHandle(stderrReadRaw);
        if (stderrWriteRaw) CloseHandle(stderrWriteRaw);
        throw std::runtime_error("could not create process output pipes");
    }
    WindowsHandle stdoutRead(stdoutReadRaw);
    WindowsHandle stdoutWrite(stdoutWriteRaw);
    WindowsHandle stderrRead(stderrReadRaw);
    WindowsHandle stderrWrite(stderrWriteRaw);
    if (!SetHandleInformation(stdoutRead.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderrRead.value, HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("could not protect process pipe handles");
    }

    WindowsHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.value) throw std::runtime_error("could not create process job object");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        throw std::runtime_error("could not configure process job object");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdoutWrite.value;
    startup.hStdError = stderrWrite.value;
    PROCESS_INFORMATION process{};
    auto command = commandLine(executable, arguments);
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("could not start process (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    }
    WindowsHandle processHandle(process.hProcess);
    WindowsHandle threadHandle(process.hThread);
    if (!AssignProcessToJobObject(job.value, processHandle.value)) {
        TerminateProcess(processHandle.value, 1);
        throw std::runtime_error("could not contain child process in a job object");
    }
    stdoutWrite = WindowsHandle{};
    stderrWrite = WindowsHandle{};
    if (ResumeThread(threadHandle.value) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job.value, 1);
        throw std::runtime_error("could not resume child process");
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool finished = false;
    while (!finished) {
        (void)drainWindowsPipe(stdoutRead.value, result.stdoutText, maxOutputBytes,
                              result.stdoutTruncated);
        (void)drainWindowsPipe(stderrRead.value, result.stderrText, maxOutputBytes,
                              result.stderrTruncated);
        finished = WaitForSingleObject(processHandle.value, 20) == WAIT_OBJECT_0;
        if (!finished && std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            TerminateJobObject(job.value, 124);
            (void)WaitForSingleObject(processHandle.value, 5000);
            finished = true;
        }
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
        const bool readOutput = drainWindowsPipe(stdoutRead.value, result.stdoutText, maxOutputBytes,
                                                 result.stdoutTruncated);
        const bool readError = drainWindowsPipe(stderrRead.value, result.stderrText, maxOutputBytes,
                                                result.stderrTruncated);
        if (!readOutput && !readError) break;
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(processHandle.value, &exitCode)) {
        result.exitCode = static_cast<int>(exitCode);
    }
#else
    int stdoutPipe[2]{};
    int stderrPipe[2]{};
    if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        if (stdoutPipe[0] != 0 || stdoutPipe[1] != 0) {
            close(stdoutPipe[0]); close(stdoutPipe[1]);
        }
        throw std::runtime_error("could not create process output pipes");
    }
    const auto pid = fork();
    if (pid < 0) {
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        close(stderrPipe[0]); close(stderrPipe[1]);
        throw std::runtime_error("could not fork child process");
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        (void)dup2(stdoutPipe[1], STDOUT_FILENO);
        (void)dup2(stderrPipe[1], STDERR_FILENO);
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        close(stderrPipe[0]); close(stderrPipe[1]);
        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1U);
        storage.push_back(executable.string());
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1U);
        for (auto& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        if (executable.has_parent_path()) execv(executable.c_str(), argv.data());
        else execvp(executable.c_str(), argv.data());
        const auto message = std::string("could not execute process: ") + std::strerror(errno) + "\n";
        const auto ignored = write(STDERR_FILENO, message.data(), message.size());
        (void)ignored;
        _exit(127);
    }

    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    setNonBlocking(stdoutPipe[0]);
    setNonBlocking(stderrPipe[0]);
    bool stdoutOpen = true;
    bool stderrOpen = true;
    bool processFinished = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!processFinished || stdoutOpen || stderrOpen) {
        std::array<pollfd, 2> descriptors{{
            {stdoutOpen ? stdoutPipe[0] : -1, POLLIN | POLLHUP, 0},
            {stderrOpen ? stderrPipe[0] : -1, POLLIN | POLLHUP, 0},
        }};
        (void)poll(descriptors.data(), descriptors.size(), 20);
        if (stdoutOpen) (void)drainPosixPipe(stdoutPipe[0], result.stdoutText,
                                             maxOutputBytes, result.stdoutTruncated, stdoutOpen);
        if (stderrOpen) (void)drainPosixPipe(stderrPipe[0], result.stderrText,
                                             maxOutputBytes, result.stderrTruncated, stderrOpen);
        if (!processFinished) {
            const auto waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) processFinished = true;
            else if (waited < 0 && errno != EINTR) processFinished = true;
        }
        if (!processFinished && std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            (void)kill(-pid, SIGKILL);
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            processFinished = true;
        }
    }
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
#endif
    return result;
}

} // namespace ve

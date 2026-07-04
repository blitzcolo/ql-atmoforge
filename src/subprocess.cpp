#include "subprocess.h"

#include <chrono>
#include <thread>

#include "util.h"

#ifdef _WIN32
// ------------------------------------------------------------- Windows ----
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ql_atmoforge {

RunResult run_process(const fs::path& exe, const fs::path& cwd,
                      const fs::path& log_file, int timeout_s) {
    RunResult r;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(log_file.wstring().c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        r.status = RunResult::StartFailed;
        r.message = "cannot open log file: " + log_file.string();
        return r;
    }
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, 0, &sa, OPEN_EXISTING, 0, nullptr);

    // Job object: TerminateJobObject on timeout kills the whole tree, and
    // KILL_ON_JOB_CLOSE reaps it even if we crash.
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl{};
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jl, sizeof(jl));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hNul;
    si.hStdOutput = hLog;
    si.hStdError = hLog;

    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr,
                             TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                             cwd.wstring().c_str(), &si, &pi);
    if (!ok) {
        r.status = RunResult::StartFailed;
        r.message = strf("CreateProcess failed: %lu", GetLastError());
        CloseHandle(hLog); CloseHandle(hNul); CloseHandle(hJob);
        return r;
    }
    AssignProcessToJobObject(hJob, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    DWORD wait = WaitForSingleObject(pi.hProcess, (DWORD)timeout_s * 1000);
    if (wait == WAIT_TIMEOUT) {
        TerminateJobObject(hJob, 1);
        WaitForSingleObject(pi.hProcess, 10000);
        r.status = RunResult::Timeout;
        r.message = strf("timeout after %d s, process tree killed", timeout_s);
    } else {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        r.exit_code = (int)code;
        if (code != 0) {
            r.status = RunResult::NonZeroExit;
            r.message = strf("exit code %lu", code);
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(hJob);
    CloseHandle(hLog);
    CloseHandle(hNul);
    return r;
}

}  // namespace ql_atmoforge

#else
// --------------------------------------------------------------- POSIX ----
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace ql_atmoforge {

RunResult run_process(const fs::path& exe, const fs::path& cwd,
                      const fs::path& log_file, int timeout_s) {
    RunResult r;

    pid_t pid = fork();
    if (pid < 0) {
        r.status = RunResult::StartFailed;
        r.message = strf("fork failed: %s", strerror(errno));
        return r;
    }
    if (pid == 0) {
        // child: own process group so a timeout kill takes any grandchildren
        setpgid(0, 0);
        if (chdir(cwd.c_str()) != 0) _exit(126);
        int logfd = open(log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int nulfd = open("/dev/null", O_RDONLY);
        if (logfd >= 0) { dup2(logfd, 1); dup2(logfd, 2); }
        if (nulfd >= 0) dup2(nulfd, 0);
        execl(exe.c_str(), exe.c_str(), (char*)nullptr);
        _exit(127);  // exec failed
    }
    setpgid(pid, pid);  // also from the parent side (race-free either way)

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout_s);
    int wstatus = 0;
    for (;;) {
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == pid) break;
        if (w < 0) {
            r.status = RunResult::StartFailed;
            r.message = strf("waitpid failed: %s", strerror(errno));
            return r;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            killpg(pid, SIGKILL);
            waitpid(pid, &wstatus, 0);
            r.status = RunResult::Timeout;
            r.message = strf("timeout after %d s, process group killed", timeout_s);
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (WIFEXITED(wstatus)) {
        r.exit_code = WEXITSTATUS(wstatus);
        if (r.exit_code == 126) {
            r.status = RunResult::StartFailed;
            r.message = "chdir to work dir failed: " + cwd.string();
        } else if (r.exit_code == 127) {
            r.status = RunResult::StartFailed;
            r.message = "exec failed: " + exe.string();
        } else if (r.exit_code != 0) {
            r.status = RunResult::NonZeroExit;
            r.message = strf("exit code %d", r.exit_code);
        }
    } else if (WIFSIGNALED(wstatus)) {
        r.status = RunResult::NonZeroExit;
        r.exit_code = -WTERMSIG(wstatus);
        r.message = strf("killed by signal %d", WTERMSIG(wstatus));
    }
    return r;
}

}  // namespace ql_atmoforge

#endif

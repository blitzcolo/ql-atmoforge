#pragma once
#include <filesystem>
#include <string>

namespace ql_atmoforge {
namespace fs = std::filesystem;

struct RunResult {
    enum Status { Ok, StartFailed, Timeout, NonZeroExit } status = Ok;
    int exit_code = 0;
    std::string message;
};

// Run `exe` with cwd = `cwd`, stdout+stderr redirected to `log_file`,
// stdin from the null device. Kills the whole process tree on timeout
// (POSIX process group / Windows Job Object).
//
// NonZeroExit is informational only: MODTRAN returns 0 even on fatal
// errors, so the caller decides success from tape6, never from here.
RunResult run_process(const fs::path& exe, const fs::path& cwd,
                      const fs::path& log_file, int timeout_s);

}  // namespace ql_atmoforge

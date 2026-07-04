#pragma once
#include <filesystem>
#include <string>

namespace ql_atmoforge {
namespace fs = std::filesystem;

// True when running inside WSL (Windows exe reachable via binfmt interop).
bool is_wsl();

// True when `exe` is a Windows binary invoked through WSL interop --
// the case where POSIX symlinks are invisible to the child process.
bool needs_windows_link(const fs::path& exe);

// Make `data_dir` visible inside `worker_dir` under data_dir's own basename
// (plus an extra "DATA" symlink on pure Linux if the basename differs --
// native MODTRAN builds usually open "DATA/").
//
// mode: auto | junction | symlink | copy. auto resolves to:
//   Windows build            -> junction (mklink /J, no admin needed)
//   WSL + Windows exe        -> junction via cmd.exe (ln -s is an LX
//                               reparse point Windows processes can't follow)
//   pure Linux               -> directory symlink
//
// Verifies the link by opening a file through it. Returns a human-readable
// description of what was done; throws on failure.
std::string ensure_data_link(const fs::path& worker_dir, const fs::path& data_dir,
                             const std::string& mode, const fs::path& exe);

}  // namespace ql_atmoforge

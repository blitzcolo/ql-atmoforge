#include "datalink.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "util.h"

namespace ql_atmoforge {

bool is_wsl() {
#ifdef _WIN32
    return false;
#else
    return fs::exists("/proc/sys/fs/binfmt_misc/WSLInterop") ||
           std::getenv("WSL_DISTRO_NAME") != nullptr;
#endif
}

bool needs_windows_link(const fs::path& exe) {
#ifdef _WIN32
    (void)exe;
    return true;
#else
    std::string ext = exe.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return is_wsl() && ext == ".exe";
#endif
}

// /mnt/d/foo/bar -> D:\foo\bar (what cmd.exe understands)
static std::string wsl_to_windows_path(const fs::path& p) {
    std::string s = fs::absolute(p).lexically_normal().string();
    if (s.rfind("/mnt/", 0) != 0 || s.size() < 6 || !std::isalpha((unsigned char)s[5]))
        throw std::runtime_error(
            "path is not under /mnt/<drive> -- a Windows exe cannot reach WSL-native "
            "filesystem paths. Put out_dir on a Windows drive: " + s);
    std::string out;
    out += (char)std::toupper((unsigned char)s[5]);
    out += ":";
    std::string rest = s.substr(6);  // "" or "/foo/bar"
    if (rest.empty()) rest = "/";
    for (char c : rest) out += (c == '/') ? '\\' : c;
    return out;
}

// The link is valid only if a real file is reachable through it (fs::exists
// on the link dir is true even for a broken LX symlink target).
static bool link_works(const fs::path& link, const fs::path& data_dir) {
    fs::path probe;
    for (const auto& e : fs::directory_iterator(data_dir))
        if (e.is_regular_file()) { probe = e.path().filename(); break; }
    if (probe.empty()) return fs::exists(link);
    std::ifstream f(link / probe, std::ios::binary);
    return f.good();
}

static void make_junction(const fs::path& link, const fs::path& target) {
#ifdef _WIN32
    std::string cmd = "cmd /c mklink /J \"" + link.string() + "\" \""
                    + target.string() + "\" >NUL 2>&1";
    if (std::system(cmd.c_str()) != 0)
        throw std::runtime_error("mklink /J failed: " + link.string());
#else
    // WSL: drive cmd.exe through interop; paths must be Windows-style.
    // cwd must itself be Windows-reachable, so run from the link's parent.
    std::string wlink = link.filename().string();
    std::string wtarget = wsl_to_windows_path(target);
    std::string cmd = "cd '" + link.parent_path().string() + "' && "
                      "cmd.exe /c mklink /J '" + wlink + "' '" + wtarget +
                      "' >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0)
        throw std::runtime_error("cmd.exe mklink /J failed for " + link.string() +
                                 " (is cmd.exe interop enabled?)");
#endif
}

static void copy_data(const fs::path& link, const fs::path& target) {
    fs::create_directories(link);
    fs::copy(target, link,
             fs::copy_options::recursive | fs::copy_options::skip_existing);
}

std::string ensure_data_link(const fs::path& worker_dir, const fs::path& data_dir,
                             const std::string& mode, const fs::path& exe) {
    if (!fs::exists(data_dir))
        throw std::runtime_error("MODTRAN data dir not found: " + data_dir.string());

    std::string resolved = mode;
    if (mode == "auto")
        resolved = needs_windows_link(exe) ? "junction" : "symlink";
#ifdef _WIN32
    if (resolved == "symlink") resolved = "junction";  // no admin rights needed
#endif

    fs::path link = worker_dir / data_dir.filename();
    if (link_works(link, data_dir))
        return "existing " + link.filename().string();

    // stale/broken leftover from a previous attempt
    std::error_code ec;
    fs::remove(link, ec);

    if (resolved == "junction") {
        if (!needs_windows_link(exe) && !is_wsl()) {
#ifndef _WIN32
            throw std::runtime_error("link_mode=junction requires Windows or WSL");
#endif
        }
        make_junction(link, data_dir);
    } else if (resolved == "symlink") {
        fs::create_directory_symlink(fs::absolute(data_dir), link);
#ifndef _WIN32
        // native Linux MODTRAN builds typically open "DATA/"
        if (link.filename() != "DATA") {
            fs::path upper = worker_dir / "DATA";
            if (!fs::exists(upper))
                fs::create_directory_symlink(fs::absolute(data_dir), upper);
        }
#endif
    } else if (resolved == "copy") {
        copy_data(link, data_dir);
    } else {
        throw std::runtime_error("unknown link_mode: " + resolved);
    }

    if (!link_works(link, data_dir))
        throw std::runtime_error(
            resolved + " link created but files are not reachable through it: " +
            link.string() + " -> " + data_dir.string() +
            (resolved == "symlink" && needs_windows_link(exe)
                 ? " (Windows exe cannot follow WSL symlinks; use link_mode=junction)"
                 : ""));
    return resolved + " " + link.filename().string() + " -> " + data_dir.string();
}

}  // namespace ql_atmoforge

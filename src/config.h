#pragma once
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ql_atmoforge {
namespace fs = std::filesystem;

// Horizontal: h1 -> h1, near-level view path (89.5 deg tilt, see tape5.cpp)
// SlantToGround: h1 -> ground, downward view path, zenith in (90, 180]
// Sky: h1 -> TOA (100 km), upward view path, zenith in [0, 90) -- the sky-dome
//      radiance dataset for miss rays; lpath of this path IS the sky radiance,
//      so the ldown run is never produced even for thermal bands
enum class PathType { Horizontal, SlantToGround, Sky };
const char* path_type_name(PathType pt);

struct BandSpec {
    std::string name = "custom";
    double v1_cm = 0.0;    // low wavenumber bound  [cm-1]
    double v2_cm = 0.0;    // high wavenumber bound [cm-1]
    double dv_cm = 1.0;    // output grid step      [cm-1]
    double fwhm_cm = 2.0;  // triangular slit FWHM  [cm-1]
    bool thermal = false;  // true -> run 3 (ldown) is produced
    int K() const { return (int)std::llround((v2_cm - v1_cm) / dv_cm) + 1; }
};

struct Dist {
    enum Kind { Values, Uniform, LogUniform } kind = Uniform;
    std::vector<double> values;  // Kind::Values
    double lo = 0.0, hi = 1.0;   // Uniform / LogUniform
    int grid_n = 0;              // grid-sampler resolution for continuous dims (0 = unset)
};

struct Config {
    fs::path config_path;

    // [modtran]
    fs::path exe;
    fs::path data_dir;
    int timeout_s = 300;
    std::string link_mode = "auto";  // auto | junction | symlink | copy

    // [run]
    fs::path out_dir;
    int workers = 0;  // 0 = hardware_concurrency
    uint64_t seed = 0;
    uint64_t n_samples = 0;
    std::string sampler = "sobol";  // sobol | random | grid
    int csv_preview = 0;

    // dataset identity: one dataset = one band + one path type
    BandSpec band;
    PathType path_type = PathType::Horizontal;

    // parameter assignment (declared order preserved for `sampled`)
    std::vector<std::pair<std::string, double>> fixed;
    std::vector<std::pair<std::string, Dist>> sampled;

    // spectral column whitelist applied at merge; empty = keep all
    std::vector<std::string> columns;
};

// Parse + expand ${VAR:-default} + structural validation.
// Parameter-level validation (names, domains) happens in ParamSpace.
Config load_config(const fs::path& path);

}  // namespace ql_atmoforge

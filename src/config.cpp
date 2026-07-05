#include "config.h"

#include <cstdlib>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "util.h"

// ordered_json keeps the declaration order of "sampled" -- the feature-vector
// layout is defined as "sampled dims in declared order", so this matters.
using json = nlohmann::ordered_json;

namespace ql_atmoforge {

const char* path_type_name(PathType pt) {
    return pt == PathType::Horizontal ? "horizontal" : "slant_to_ground";
}

// ${NAME} / ${NAME:-default} expansion over the raw config text.
static std::string expand_env(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '$' && i + 1 < text.size() && text[i+1] == '{') {
            size_t end = text.find('}', i + 2);
            if (end == std::string::npos)
                throw std::runtime_error("config: unterminated ${...} expansion");
            std::string body = text.substr(i + 2, end - i - 2);
            std::string name = body, def;
            bool has_def = false;
            size_t sep = body.find(":-");
            if (sep != std::string::npos) {
                name = body.substr(0, sep);
                def = body.substr(sep + 2);
                has_def = true;
            }
            const char* v = std::getenv(name.c_str());
            if (v && *v) out += v;
            else if (has_def) out += def;
            else throw std::runtime_error("config: environment variable not set and no default: " + name);
            i = end + 1;
        } else {
            out += text[i++];
        }
    }
    return out;
}

static BandSpec band_preset(const std::string& name) {
    // Wavenumber bounds follow the existing project's band table.
    BandSpec b;
    b.name = name;
    if      (name == "vis")      { b.v1_cm = 12500; b.v2_cm = 25000; b.thermal = false; }
    else if (name == "nir")  { b.v1_cm =  8333; b.v2_cm = 10753; b.thermal = false; }
    else if (name == "swir") { b.v1_cm =  4167; b.v2_cm =  7143; b.thermal = false; }
    else if (name == "mwir")     { b.v1_cm =  2000; b.v2_cm =  3333; b.thermal = true;  }
    else if (name == "lwir")     { b.v1_cm =   833; b.v2_cm =  1250; b.thermal = true;  }
    else throw std::runtime_error("config: unknown band preset: " + name);
    return b;
}

static Dist parse_dist(const std::string& pname, const json& j) {
    if (!j.is_object())
        throw std::runtime_error("config: sampled." + pname + " must be an object");
    Dist d;
    int kinds = 0;
    if (j.contains("values")) {
        d.kind = Dist::Values;
        d.values = j.at("values").get<std::vector<double>>();
        if (d.values.empty())
            throw std::runtime_error("config: sampled." + pname + ".values is empty");
        ++kinds;
    }
    if (j.contains("uniform")) {
        d.kind = Dist::Uniform;
        auto v = j.at("uniform").get<std::vector<double>>();
        if (v.size() != 2) throw std::runtime_error("config: sampled." + pname + ".uniform needs [lo,hi]");
        d.lo = v[0]; d.hi = v[1];
        ++kinds;
    }
    if (j.contains("log_uniform")) {
        d.kind = Dist::LogUniform;
        auto v = j.at("log_uniform").get<std::vector<double>>();
        if (v.size() != 2) throw std::runtime_error("config: sampled." + pname + ".log_uniform needs [lo,hi]");
        d.lo = v[0]; d.hi = v[1];
        if (d.lo <= 0.0) throw std::runtime_error("config: sampled." + pname + ".log_uniform lower bound must be > 0");
        ++kinds;
    }
    if (kinds != 1)
        throw std::runtime_error("config: sampled." + pname
                                 + " needs exactly one of values/uniform/log_uniform");
    if (d.kind != Dist::Values && d.lo >= d.hi)
        throw std::runtime_error("config: sampled." + pname + " requires lo < hi");
    d.grid_n = j.value("grid_n", 0);
    return d;
}

Config load_config(const fs::path& path) {
    Config c;
    c.config_path = path;
    json root = json::parse(expand_env(read_text_file(path.string())));

    const json& m = root.at("modtran");
    c.exe = fs::path(m.at("exe").get<std::string>());
    c.data_dir = fs::path(m.at("data_dir").get<std::string>());
    c.timeout_s = m.value("timeout_s", 300);
    c.link_mode = m.value("link_mode", std::string("auto"));
    if (c.link_mode != "auto" && c.link_mode != "junction" &&
        c.link_mode != "symlink" && c.link_mode != "copy")
        throw std::runtime_error("config: modtran.link_mode must be auto|junction|symlink|copy");

    const json& r = root.at("run");
    c.out_dir = fs::path(r.at("out_dir").get<std::string>());
    c.workers = r.value("workers", 0);
    c.seed = r.value("seed", (uint64_t)0);
    c.n_samples = r.at("n_samples").get<uint64_t>();
    c.sampler = r.value("sampler", std::string("sobol"));
    c.csv_preview = r.value("csv_preview", 0);
    if (c.sampler != "sobol" && c.sampler != "random" && c.sampler != "grid")
        throw std::runtime_error("config: run.sampler must be sobol|random|grid");
    if (c.n_samples == 0)
        throw std::runtime_error("config: run.n_samples must be > 0");

    const json& b = root.at("band");
    if (b.contains("preset")) {
        c.band = band_preset(b.at("preset").get<std::string>());
    } else {
        c.band.name = b.value("name", std::string("custom"));
        c.band.v1_cm = b.at("v1_cm").get<double>();
        c.band.v2_cm = b.at("v2_cm").get<double>();
        c.band.thermal = b.at("thermal").get<bool>();
    }
    c.band.dv_cm = b.value("dv_cm", 1.0);
    c.band.fwhm_cm = b.value("fwhm_cm", 2.0);
    if (c.band.v1_cm <= 0 || c.band.v2_cm <= c.band.v1_cm || c.band.dv_cm <= 0)
        throw std::runtime_error("config: band requires 0 < v1_cm < v2_cm and dv_cm > 0");

    std::string pt = root.at("path_type").get<std::string>();
    if (pt == "horizontal") c.path_type = PathType::Horizontal;
    else if (pt == "slant_to_ground") c.path_type = PathType::SlantToGround;
    else throw std::runtime_error("config: path_type must be horizontal|slant_to_ground");

    if (root.contains("fixed"))
        for (auto it = root["fixed"].begin(); it != root["fixed"].end(); ++it)
            c.fixed.emplace_back(it.key(), it.value().get<double>());

    if (root.contains("sampled"))
        for (auto it = root["sampled"].begin(); it != root["sampled"].end(); ++it)
            c.sampled.emplace_back(it.key(), parse_dist(it.key(), it.value()));

    if (root.contains("columns") && root["columns"].is_array())
        c.columns = root["columns"].get<std::vector<std::string>>();
    // "columns": "all" (or absent) -> empty vector -> keep everything

    return c;
}

}  // namespace ql_atmoforge

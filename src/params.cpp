#include "params.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util.h"

namespace ql_atmoforge {

static constexpr double kEarthRadiusKm = 6371.0;
static constexpr double kPi = 3.14159265358979323846;

#define SET(field) [](SampleParams& p, double v) { p.field = v; }
#define SETI(field) [](SampleParams& p, double v) { p.field = (int)std::llround(v); }
#define GET(field) [](const SampleParams& p) { return (double)p.field; }

const std::vector<ParamDesc>& param_table() {
    static const std::vector<ParamDesc> t = {
        // name                 int    def      lo      hi      allowed
        {"atmos_model",         true,  2,       1,      6,      {1,2,3,4,5,6},            SETI(atmos_model), GET(atmos_model)},
        {"ihaze",               true,  1,       0,      10,     {0,1,2,3,4,5,6,8,9,10},   SETI(ihaze),       GET(ihaze)},
        {"icld",                true,  0,       0,      19,     {0,1,2,3,4,5,6,7,8,9,10,18,19}, SETI(icld), GET(icld)},
        {"vis_km",              false, 0.0,     0.0,    300.0,  {},                       SET(vis_km),       GET(vis_km)},
        {"rainrt_mm_h",         false, 0.0,     0.0,    100.0,  {},                       SET(rainrt_mm_h),  GET(rainrt_mm_h)},
        {"t_ground_K",          false, 288.15,  200.0,  340.0,  {},                       SET(t_ground_K),   GET(t_ground_K)},
        {"rh",                  false, 0.5,     0.0,    1.0,    {},                       SET(rh),           GET(rh)},
        {"p_hPa",               false, 1013.25, 500.0,  1100.0, {},                       SET(p_hPa),        GET(p_hPa)},
        {"h2o_scale",           false, 1.0,     0.01,   10.0,   {},                       SET(h2o_scale),    GET(h2o_scale)},
        {"o3_scale",            false, 1.0,     0.01,   10.0,   {},                       SET(o3_scale),     GET(o3_scale)},
        {"co2_ppmv",            false, 365.0,   100.0,  1000.0, {},                       SET(co2_ppmv),     GET(co2_ppmv)},
        {"h1_km",               false, 0.0,     0.0,    99.0,   {},                       SET(h1_km),        GET(h1_km)},
        {"range_km",            false, 1.0,     0.001,  1000.0, {},                       SET(range_km),     GET(range_km)},
        {"view_zenith_deg",     false, 180.0,   90.0,   180.0,  {},                       SET(view_zenith_deg), GET(view_zenith_deg)},
        {"sun_zenith_deg",      false, 45.0,    0.0,    89.9,   {},                       SET(sun_zenith_deg), GET(sun_zenith_deg)},
        // default 90, not 0: with the default sun_zenith=45 and
        // ldown_zenith=45, rel_az=0 puts the sun exactly on the ldown LOS --
        // scattering angle 0 and a guaranteed PHASEF hard stop in MODTRAN
        {"sun_rel_azimuth_deg", false, 90.0,   -180.0,  180.0,  {},                       SET(sun_rel_azimuth_deg), GET(sun_rel_azimuth_deg)},
        {"iday",                true,  93,      1,      365,    {},                       SETI(iday),        GET(iday)},
        {"ldown_zenith_deg",    false, 45.0,    0.0,    89.9,   {},                       SET(ldown_zenith_deg), GET(ldown_zenith_deg)},
    };
    return t;
}

#undef SET
#undef SETI
#undef GET

const ParamDesc* find_param(const std::string& name) {
    for (const auto& d : param_table())
        if (name == d.name) return &d;
    return nullptr;
}

static void check_value(const ParamDesc& d, double v) {
    if (v < d.lo || v > d.hi)
        throw std::runtime_error(strf("param %s: value %g outside domain [%g, %g]",
                                      d.name, v, d.lo, d.hi));
    if (!d.allowed.empty() &&
        std::find(d.allowed.begin(), d.allowed.end(), v) == d.allowed.end())
        throw std::runtime_error(strf("param %s: value %g not in the legal set", d.name, v));
}

ParamSpace::ParamSpace(const Config& cfg)
    : sampled_(cfg.sampled), path_type_(cfg.path_type), n_samples_(cfg.n_samples) {
    // fixed set
    for (const auto& [name, value] : cfg.fixed) {
        const ParamDesc* d = find_param(name);
        if (!d) throw std::runtime_error("config: unknown parameter in fixed: " + name);
        check_value(*d, value);
        for (const auto& [sn, sd] : cfg.sampled)
            if (sn == name)
                throw std::runtime_error("config: parameter both fixed and sampled: " + name);
        fixed_.emplace_back(d, value);
    }
    // sampled set (declared order = feature layout)
    std::vector<uint64_t> grid_shape;
    for (const auto& [name, dist] : cfg.sampled) {
        const ParamDesc* d = find_param(name);
        if (!d) throw std::runtime_error("config: unknown parameter in sampled: " + name);
        if (dist.kind == Dist::Values) {
            for (double v : dist.values) check_value(*d, v);
            grid_shape.push_back(dist.values.size());
        } else {
            check_value(*d, dist.lo);
            check_value(*d, dist.hi);
            if (d->is_int && d->allowed.empty() == false)
                throw std::runtime_error("config: discrete parameter " + name
                                         + " must use a values list");
            if (cfg.sampler == "grid") {
                if (dist.grid_n < 2)
                    throw std::runtime_error("config: grid sampler requires grid_n >= 2 on "
                                             "continuous dim " + name);
                grid_shape.push_back((uint64_t)dist.grid_n);
            } else {
                grid_shape.push_back(0);  // unused
            }
        }
        dims_.push_back({d, dist});
        feat_names_.push_back(name);
    }
    // geometry / path-type consistency
    auto is_sampled = [&](const char* n) {
        for (const auto& dim : dims_) if (std::string(n) == dim.d->name) return true;
        return false;
    };
    if (path_type_ == PathType::Horizontal && is_sampled("view_zenith_deg"))
        throw std::runtime_error("config: horizontal path fixes view_zenith_deg = 90; "
                                 "do not sample it");
    if (path_type_ == PathType::SlantToGround && is_sampled("range_km"))
        throw std::runtime_error("config: slant_to_ground derives range_km from (h1, zenith); "
                                 "do not sample it");
    if (path_type_ == PathType::SlantToGround) {
        // h1 = 0 makes the slant path degenerate (zero length)
        for (const auto& dim : dims_)
            if (std::string("h1_km") == dim.d->name) {
                double lo = dim.dist.kind == Dist::Values
                    ? *std::min_element(dim.dist.values.begin(), dim.dist.values.end())
                    : dim.dist.lo;
                if (lo <= 0.0)
                    throw std::runtime_error("config: slant_to_ground requires h1_km > 0");
            }
        for (const auto& [d, v] : fixed_)
            if (std::string("h1_km") == d->name && v <= 0.0)
                throw std::runtime_error("config: slant_to_ground requires h1_km > 0");
    }

    sampler_ = make_sampler(cfg.sampler, cfg.seed, (int)dims_.size(), grid_shape);
    if (cfg.sampler == "grid") {
        uint64_t total = 1;
        for (uint64_t s : grid_shape) total *= s;
        if (n_samples_ > total)
            throw std::runtime_error(strf("config: grid has %llu points but n_samples=%llu",
                                          (unsigned long long)total,
                                          (unsigned long long)n_samples_));
    }

    // geometry block appended to the feature vector
    feat_names_.push_back("h1_km");
    feat_names_.push_back("h2_km");
    feat_names_.push_back("cos_view_zenith");
    feat_names_.push_back("range_km");
}

SampleParams ParamSpace::at(uint64_t index) const {
    SampleParams p;  // table defaults are the member initializers
    for (const auto& [d, v] : fixed_) d->set(p, v);
    for (size_t i = 0; i < dims_.size(); ++i) {
        const auto& [d, dist] = dims_[i];
        double u = sampler_->value(index, (int)i);
        double v = 0.0;
        switch (dist.kind) {
            case Dist::Values: {
                size_t k = (size_t)(u * (double)dist.values.size());
                if (k >= dist.values.size()) k = dist.values.size() - 1;
                v = dist.values[k];
                break;
            }
            case Dist::Uniform:
                v = dist.lo + u * (dist.hi - dist.lo);
                break;
            case Dist::LogUniform:
                v = std::exp(std::log(dist.lo) + u * (std::log(dist.hi) - std::log(dist.lo)));
                break;
        }
        if (d->is_int) v = std::round(v);
        d->set(p, v);
    }
    resolve(p);
    return p;
}

void ParamSpace::resolve(SampleParams& p) const {
    // conditional dim: rain rate exists only inside the rain cloud model
    if (p.icld != 6) p.rainrt_mm_h = 0.0;

    if (path_type_ == PathType::Horizontal) {
        p.view_zenith_deg = 90.0;
        p.h2_km = p.h1_km;
        // range_km stays as sampled/fixed
    } else {
        p.h2_km = 0.0;
        // A ray from altitude h1 at zenith angle theta only hits the ground if
        // sin(theta) <= Re/(Re+h1). Clamp to the grazing limit plus a margin;
        // the clamped value is what goes into tape5 AND the feature vector.
        double r1 = kEarthRadiusKm + p.h1_km;
        double theta_min = 180.0 - std::asin(kEarthRadiusKm / r1) * 180.0 / kPi;
        if (p.view_zenith_deg < theta_min + 0.05)
            p.view_zenith_deg = theta_min + 0.05;
        // spherical, no-refraction slant range down to the ground
        double mu = std::cos(p.view_zenith_deg * kPi / 180.0);
        double disc = r1 * r1 * mu * mu - (r1 * r1 - kEarthRadiusKm * kEarthRadiusKm);
        if (disc < 0.0) disc = 0.0;  // numerically grazing
        p.range_km = -r1 * mu - std::sqrt(disc);
    }
    p.cos_view_zenith = std::cos(p.view_zenith_deg * kPi / 180.0);

    if (p.iday < 1) p.iday = 1;
    if (p.iday > 365) p.iday = 365;
}

std::vector<double> ParamSpace::features(const SampleParams& p) const {
    std::vector<double> f;
    f.reserve(feat_names_.size());
    for (const auto& dim : dims_) f.push_back(dim.d->get(p));
    f.push_back(p.h1_km);
    f.push_back(p.h2_km);
    f.push_back(p.cos_view_zenith);
    f.push_back(p.range_km);
    return f;
}

}  // namespace ql_atmoforge

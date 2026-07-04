#pragma once
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "sampler.h"

namespace ql_atmoforge {

// Fully resolved parameters for one sample. Flat struct: tape5 assembly
// reads it directly, no indirection.
struct SampleParams {
    int    atmos_model = 2;          // Card 1 M1-M6 (MODEL=7)
    int    ihaze = 1;                // Card 2
    int    icld = 0;                 // Card 2 (+2A)
    double vis_km = 0.0;             // Card 2 VIS; 0 = IHAZE default
    double rainrt_mm_h = 0.0;        // Card 2 RAINRT; forced 0 unless icld==6
    double t_ground_K = 288.15;      // Card 2C1 ground level, JCHAR='AAH'
    double rh = 0.5;                 // relative humidity [0,1] -> WMOL(1) in %
    double p_hPa = 1013.25;          // ground pressure
    double h2o_scale = 1.0;          // Card 1A H2OSTR (column scale factor)
    double o3_scale = 1.0;           // Card 1A O3STR
    double co2_ppmv = 365.0;         // Card 1A CO2MX
    double h1_km = 0.0;              // sensor altitude
    double range_km = 1.0;           // horizontal: sampled; slant: derived
    double view_zenith_deg = 90.0;   // slant: sampled (90,180]; horizontal: 90
    double sun_zenith_deg = 45.0;    // Card 3A2 PARM2 (IPARM=2)
    double sun_rel_azimuth_deg = 0.0;// Card 3A2 PARM1, LOS->sun, [-180,180]
    int    iday = 93;                // Card 3A1 IDAY (Earth-sun distance)
    double ldown_zenith_deg = 45.0;  // run 3 view zenith (looking up)
    // derived by ParamSpace::resolve()
    double h2_km = 0.0;
    double cos_view_zenith = 0.0;
};

// Static descriptor: one row per configurable parameter. The config file
// assigns each name to `fixed` or `sampled`; everything else is data-driven.
struct ParamDesc {
    const char* name;
    bool is_int;
    double def;
    double lo, hi;                 // validation domain (inclusive)
    std::vector<double> allowed;   // non-empty -> discrete legal set
    void (*set)(SampleParams&, double);
    double (*get)(const SampleParams&);
};

const std::vector<ParamDesc>& param_table();
const ParamDesc* find_param(const std::string& name);  // nullptr if unknown

// index -> SampleParams, a pure function of (config, seed, index).
class ParamSpace {
public:
    explicit ParamSpace(const Config& cfg);  // validates names/domains, throws

    uint64_t n_samples() const { return n_samples_; }
    SampleParams at(uint64_t index) const;

    // Feature vector layout: [sampled dims, declared order] +
    // [h1_km, h2_km, cos_view_zenith, range_km]. Values are post-resolve
    // (what MODTRAN actually saw), so clamped/constrained dims stay honest.
    std::vector<double> features(const SampleParams& p) const;
    const std::vector<std::string>& feature_names() const { return feat_names_; }
    size_t n_features() const { return feat_names_.size(); }

    const std::vector<std::pair<std::string, Dist>>& sampled() const { return sampled_; }
    std::string sampler_name() const { return sampler_->name(); }

private:
    void resolve(SampleParams& p) const;

    struct Dim { const ParamDesc* d; Dist dist; };
    std::vector<Dim> dims_;
    std::vector<std::pair<const ParamDesc*, double>> fixed_;
    std::vector<std::pair<std::string, Dist>> sampled_;  // for manifest
    std::vector<std::string> feat_names_;
    std::unique_ptr<Sampler> sampler_;
    PathType path_type_;
    uint64_t n_samples_;
};

}  // namespace ql_atmoforge

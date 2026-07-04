#pragma once
#include <string>

#include "config.h"
#include "params.h"

namespace ql_atmoforge {

// The 2-3 MODTRAN runs that make up one sample.
enum class RunKind {
    Tau,    // IEMSCT=0, line-of-sight path, transmittance components
    Lpath,  // IEMSCT=2, line-of-sight path, radiance components
    Ldown,  // IEMSCT=2, from target altitude looking up (thermal bands only)
};
const char* run_kind_name(RunKind rk);

// Full tape5 text (CARD 1 .. CARD 5) for one run.
std::string tape5_text(const SampleParams& p, const BandSpec& band,
                       PathType pt, RunKind rk);

}  // namespace ql_atmoforge

#pragma once
#include <cstdint>

#include "config.h"
#include "params.h"

namespace ql_atmoforge {

struct GenSummary {
    uint64_t total = 0;
    uint64_t skipped = 0;   // already present in shards (resume)
    uint64_t ok = 0;
    uint64_t partial = 0;
    uint64_t failed = 0;
};

// The whole generation pass: persistent worker dirs + Data links, resume
// scan, atomic-counter dispatch, per-sample 2-3 MODTRAN runs, shard append.
GenSummary run_generation(const Config& cfg, const ParamSpace& space);

}  // namespace ql_atmoforge

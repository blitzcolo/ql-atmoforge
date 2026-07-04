#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ql_atmoforge {

// Counter-based sampler: value(index, dim) is a pure function of
// (seed, index, dim). Scheduling order, worker count and resume never
// change what a given sample index contains.
class Sampler {
public:
    virtual ~Sampler() = default;
    // u in [0,1) for sample `index`, dimension `dim`
    virtual double value(uint64_t index, int dim) const = 0;
    virtual std::string name() const = 0;
};

// kind: "sobol" | "random" | "grid".
// grid_shape: points per dimension, required (all > 0) for "grid" only.
// Sobol supports up to 40 dimensions (embedded Joe-Kuo direction numbers).
std::unique_ptr<Sampler> make_sampler(const std::string& kind, uint64_t seed,
                                      int ndims, const std::vector<uint64_t>& grid_shape);

}  // namespace ql_atmoforge

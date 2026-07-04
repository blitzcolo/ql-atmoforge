#include "sampler.h"

#include <stdexcept>

namespace ql_atmoforge {

// ---------------------------------------------------------------- random --

static uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

class RandomSampler final : public Sampler {
public:
    explicit RandomSampler(uint64_t seed) : seed_(splitmix64(seed)) {}
    double value(uint64_t index, int dim) const override {
        uint64_t h = splitmix64(seed_ ^ splitmix64(index + 1));
        h = splitmix64(h ^ splitmix64((uint64_t)dim + 0x9E3779B97F4A7C15ULL));
        return (double)(h >> 11) * 0x1.0p-53;  // 53-bit mantissa, [0,1)
    }
    std::string name() const override { return "random"; }
private:
    uint64_t seed_;
};

// ----------------------------------------------------------------- sobol --

// Joe & Kuo direction numbers (new-joe-kuo-6.21201), dimensions 2..40.
// Dimension 1 is the van der Corput sequence. Source copy kept at
// third_party/joe-kuo-6.40.txt.
struct JoeKuoRow { int s; int a; uint32_t m[8]; };
static const JoeKuoRow kJoeKuo[] = {
    {1,  0, {1}},
    {2,  1, {1,3}},
    {3,  1, {1,3,1}},
    {3,  2, {1,1,1}},
    {4,  1, {1,1,3,3}},
    {4,  4, {1,3,5,13}},
    {5,  2, {1,1,5,5,17}},
    {5,  4, {1,1,5,5,5}},
    {5,  7, {1,1,7,11,19}},
    {5, 11, {1,1,5,1,1}},
    {5, 13, {1,1,1,3,11}},
    {5, 14, {1,3,5,5,31}},
    {6,  1, {1,3,3,9,7,49}},
    {6, 13, {1,1,1,15,21,21}},
    {6, 16, {1,3,1,13,27,49}},
    {6, 19, {1,1,1,15,7,5}},
    {6, 22, {1,3,1,15,13,25}},
    {6, 25, {1,1,5,5,19,61}},
    {7,  1, {1,3,7,11,23,15,103}},
    {7,  4, {1,3,7,13,13,15,69}},
    {7,  7, {1,1,3,13,7,35,63}},
    {7,  8, {1,3,5,9,1,25,53}},
    {7, 14, {1,3,1,13,9,35,107}},
    {7, 19, {1,3,1,5,27,61,31}},
    {7, 21, {1,1,5,11,19,41,61}},
    {7, 28, {1,3,5,3,3,13,69}},
    {7, 31, {1,1,7,13,1,19,1}},
    {7, 32, {1,3,7,5,13,19,59}},
    {7, 37, {1,1,3,9,25,29,41}},
    {7, 41, {1,3,5,13,23,1,55}},
    {7, 42, {1,3,7,3,13,59,17}},
    {7, 50, {1,3,1,3,5,53,69}},
    {7, 55, {1,1,5,5,23,33,13}},
    {7, 56, {1,1,7,7,1,61,123}},
    {7, 59, {1,1,7,9,13,61,49}},
    {7, 62, {1,3,3,5,3,55,33}},
    {8, 14, {1,3,1,15,31,13,49,245}},
    {8, 21, {1,3,5,15,31,59,63,97}},
    {8, 22, {1,3,1,11,11,11,77,249}},
};
static constexpr int kSobolMaxDims = 1 + (int)(sizeof(kJoeKuo) / sizeof(kJoeKuo[0]));
static constexpr int kSobolBits = 32;

class SobolSampler final : public Sampler {
public:
    SobolSampler(uint64_t seed, int ndims) : seed_(seed), ndims_(ndims) {
        if (ndims > kSobolMaxDims)
            throw std::runtime_error("sobol: at most " + std::to_string(kSobolMaxDims)
                                     + " sampled dimensions supported; use sampler=random");
        V_.resize((size_t)ndims);
        for (int d = 0; d < ndims; ++d) {
            auto& V = V_[(size_t)d];
            V.assign(kSobolBits, 0);
            if (d == 0) {  // van der Corput
                for (int j = 0; j < kSobolBits; ++j) V[(size_t)j] = 1u << (31 - j);
                continue;
            }
            const JoeKuoRow& r = kJoeKuo[d - 1];
            for (int j = 0; j < r.s && j < kSobolBits; ++j)
                V[(size_t)j] = r.m[j] << (31 - j);
            for (int j = r.s; j < kSobolBits; ++j) {
                uint32_t v = V[(size_t)(j - r.s)] ^ (V[(size_t)(j - r.s)] >> r.s);
                for (int k = 1; k < r.s; ++k)
                    if ((r.a >> (r.s - 1 - k)) & 1) v ^= V[(size_t)(j - k)];
                V[(size_t)j] = v;
            }
        }
    }

    double value(uint64_t index, int dim) const override {
        // index+1 skips the all-zero point 0; seed acts as a sequence offset
        // (digital shift by position), keeping determinism per (seed,index).
        uint64_t n = index + 1 + seed_;
        uint64_t g = n ^ (n >> 1);  // Gray code -> standard Sobol ordering
        uint32_t x = 0;
        const auto& V = V_[(size_t)dim];
        for (int j = 0; j < kSobolBits && g; ++j, g >>= 1)
            if (g & 1) x ^= V[(size_t)j];
        return (double)x * 0x1.0p-32;
    }
    std::string name() const override { return "sobol"; }

private:
    uint64_t seed_;
    int ndims_;
    std::vector<std::vector<uint32_t>> V_;
};

// ------------------------------------------------------------------ grid --

class GridSampler final : public Sampler {
public:
    explicit GridSampler(std::vector<uint64_t> shape) : shape_(std::move(shape)) {
        total_ = 1;
        for (uint64_t s : shape_) {
            if (s == 0) throw std::runtime_error("grid: zero-sized dimension");
            total_ *= s;
        }
    }
    double value(uint64_t index, int dim) const override {
        // mixed-radix decode: dim 0 varies fastest
        uint64_t rest = index % total_;
        for (int d = 0; d < dim; ++d) rest /= shape_[(size_t)d];
        uint64_t k = rest % shape_[(size_t)dim];
        return ((double)k + 0.5) / (double)shape_[(size_t)dim];
    }
    std::string name() const override { return "grid"; }
    uint64_t total() const { return total_; }

private:
    std::vector<uint64_t> shape_;
    uint64_t total_;
};

std::unique_ptr<Sampler> make_sampler(const std::string& kind, uint64_t seed,
                                      int ndims, const std::vector<uint64_t>& grid_shape) {
    if (kind == "random") return std::make_unique<RandomSampler>(seed);
    if (kind == "sobol")  return std::make_unique<SobolSampler>(seed, ndims);
    if (kind == "grid") {
        if ((int)grid_shape.size() != ndims)
            throw std::runtime_error("grid: internal shape/ndims mismatch");
        return std::make_unique<GridSampler>(grid_shape);
    }
    throw std::runtime_error("unknown sampler: " + kind);
}

}  // namespace ql_atmoforge

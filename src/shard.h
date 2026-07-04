#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ql_atmoforge {
namespace fs = std::filesystem;

// Per-worker append-only shard of fixed-length records:
//   header (32 B): magic "TRSHRD1\0", record_size u64, n_params u32,
//                  n_spec u32, reserved u64
//   record: [u64 index][u8 status][u8 pad x7][f64 x P][f32 x S]
// Fixed length => resume is a linear scan of (index, status) and a torn
// tail write is detected by length and truncated away. Little-endian only.
enum class SampleStatus : uint8_t { Ok = 0, Partial = 1, Failed = 2 };
const char* sample_status_name(SampleStatus s);

struct ShardLayout {
    uint32_t n_params = 0;  // P: feature-vector doubles
    uint32_t n_spec = 0;    // S: total spectral floats (C_total * K)
    uint64_t record_size() const {
        return 8 + 8 + 8ull * n_params + 4ull * n_spec;
    }
};

class ShardWriter {
public:
    // Opens (or creates) the shard, validates the header against `layout`,
    // truncates a torn trailing record if present. Throws on mismatch.
    ShardWriter(const fs::path& path, const ShardLayout& layout);
    ~ShardWriter();
    ShardWriter(const ShardWriter&) = delete;
    ShardWriter& operator=(const ShardWriter&) = delete;

    // params: P doubles; spec: S floats (zeros for failed samples)
    void append(uint64_t index, SampleStatus status,
                const double* params, const float* spec);

private:
    FILE* f_ = nullptr;
    ShardLayout layout_;
    std::vector<char> buf_;
};

struct ShardRecordRef {
    uint64_t index;
    SampleStatus status;
    fs::path file;
    uint64_t offset;  // byte offset of the record
};

// Scan every shards/shard_*.bin under `shard_dir`. Later files win on
// duplicate indices (only possible across interrupted runs). Layout
// mismatches throw -- a config change invalidates the shard set.
std::map<uint64_t, ShardRecordRef> scan_shards(const fs::path& shard_dir,
                                               const ShardLayout& layout);

// Read one record's params/spec back (for merge).
void read_shard_record(const ShardRecordRef& ref, const ShardLayout& layout,
                       std::vector<double>& params, std::vector<float>& spec);

}  // namespace ql_atmoforge

#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "config.h"

namespace ql_atmoforge {
namespace fs = std::filesystem;

// Streaming .npy v1.0 writer: shape is fixed up-front, data is appended in
// C order. Streaming matters -- a merged vis-band dataset does not fit in
// memory. descr examples: "<f4" "<f8" "<u8" "|u1" (little-endian only).
class NpyWriter {
public:
    NpyWriter(const fs::path& path, const std::string& descr,
              const std::vector<uint64_t>& shape);
    ~NpyWriter();
    NpyWriter(const NpyWriter&) = delete;
    NpyWriter& operator=(const NpyWriter&) = delete;

    void write(const void* data, size_t bytes);
    void close();  // validates total byte count; throws on mismatch

private:
    FILE* f_ = nullptr;
    fs::path path_;
    uint64_t expected_ = 0, written_ = 0;
};

// convenience for small arrays
void write_npy(const fs::path& path, const std::string& descr,
               const std::vector<uint64_t>& shape, const void* data, size_t bytes);

// shards/ -> out_dir root: params/tau/lpath[/ldown]/wavenumber/index/status
// .npy + manifest.json + failures.jsonl + optional preview CSVs.
void merge_dataset(const Config& cfg);

}  // namespace ql_atmoforge

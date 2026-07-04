#include "shard.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "util.h"

namespace ql_atmoforge {

// plain fseek takes a 32-bit long on Windows; shards can exceed 2 GB
static int fseek64(FILE* f, uint64_t ofs) {
#ifdef _WIN32
    return _fseeki64(f, (long long)ofs, SEEK_SET);
#else
    return fseeko(f, (off_t)ofs, SEEK_SET);
#endif
}

static const char kMagic[8] = {'T', 'R', 'S', 'H', 'R', 'D', '1', '\0'};
static constexpr uint64_t kHeaderSize = 32;

const char* sample_status_name(SampleStatus s) {
    switch (s) {
        case SampleStatus::Ok:      return "ok";
        case SampleStatus::Partial: return "partial";
        case SampleStatus::Failed:  return "failed";
    }
    return "?";
}

static void write_header(FILE* f, const ShardLayout& layout) {
    char h[kHeaderSize] = {};
    memcpy(h, kMagic, 8);
    uint64_t rs = layout.record_size();
    memcpy(h + 8, &rs, 8);
    memcpy(h + 16, &layout.n_params, 4);
    memcpy(h + 20, &layout.n_spec, 4);
    if (fwrite(h, 1, kHeaderSize, f) != kHeaderSize)
        throw std::runtime_error("shard: header write failed");
}

static void check_header(FILE* f, const fs::path& path, const ShardLayout& layout) {
    char h[kHeaderSize];
    if (fread(h, 1, kHeaderSize, f) != kHeaderSize)
        throw std::runtime_error("shard: short header in " + path.string());
    if (memcmp(h, kMagic, 8) != 0)
        throw std::runtime_error("shard: bad magic in " + path.string());
    uint64_t rs; uint32_t np, ns;
    memcpy(&rs, h + 8, 8);
    memcpy(&np, h + 16, 4);
    memcpy(&ns, h + 20, 4);
    if (rs != layout.record_size() || np != layout.n_params || ns != layout.n_spec)
        throw std::runtime_error(
            "shard: layout mismatch in " + path.string() +
            " (config changed since these shards were written? move or delete "
            "the old shards/ directory)");
}

ShardWriter::ShardWriter(const fs::path& path, const ShardLayout& layout)
    : layout_(layout) {
    buf_.resize(layout.record_size());
    bool existed = fs::exists(path) && fs::file_size(path) > 0;
    f_ = fopen(path.string().c_str(), existed ? "r+b" : "w+b");
    if (!f_) throw std::runtime_error("shard: cannot open " + path.string());
    if (existed) {
        check_header(f_, path, layout);
        // drop a torn trailing record, position at the true end
        uint64_t size = fs::file_size(path);
        uint64_t body = size - kHeaderSize;
        uint64_t whole = body - body % layout.record_size();
        fseek64(f_, kHeaderSize + whole);
    } else {
        write_header(f_, layout);
    }
}

ShardWriter::~ShardWriter() {
    if (f_) fclose(f_);
}

void ShardWriter::append(uint64_t index, SampleStatus status,
                         const double* params, const float* spec) {
    char* p = buf_.data();
    memcpy(p, &index, 8);
    p[8] = (char)status;
    memset(p + 9, 0, 7);
    memcpy(p + 16, params, 8ull * layout_.n_params);
    memcpy(p + 16 + 8ull * layout_.n_params, spec, 4ull * layout_.n_spec);
    if (fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size())
        throw std::runtime_error("shard: record write failed (disk full?)");
    fflush(f_);
}

std::map<uint64_t, ShardRecordRef> scan_shards(const fs::path& shard_dir,
                                               const ShardLayout& layout) {
    std::map<uint64_t, ShardRecordRef> out;
    if (!fs::exists(shard_dir)) return out;

    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(shard_dir)) {
        std::string n = e.path().filename().string();
        if (n.rfind("shard_", 0) == 0 && e.path().extension() == ".bin")
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());

    const uint64_t rs = layout.record_size();
    for (const auto& path : files) {
        FILE* f = fopen(path.string().c_str(), "rb");
        if (!f) continue;
        try {
            check_header(f, path, layout);
        } catch (...) {
            fclose(f);
            throw;
        }
        uint64_t size = fs::file_size(path);
        uint64_t n = (size - kHeaderSize) / rs;
        char head[16];
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t ofs = kHeaderSize + i * rs;
            fseek64(f, ofs);
            if (fread(head, 1, 16, f) != 16) break;
            ShardRecordRef ref;
            memcpy(&ref.index, head, 8);
            ref.status = (SampleStatus)(uint8_t)head[8];
            ref.file = path;
            ref.offset = ofs;
            out[ref.index] = ref;  // later files win
        }
        fclose(f);
    }
    return out;
}

void read_shard_record(const ShardRecordRef& ref, const ShardLayout& layout,
                       std::vector<double>& params, std::vector<float>& spec) {
    FILE* f = fopen(ref.file.string().c_str(), "rb");
    if (!f) throw std::runtime_error("shard: cannot reopen " + ref.file.string());
    params.resize(layout.n_params);
    spec.resize(layout.n_spec);
    fseek64(f, ref.offset + 16);
    bool ok = fread(params.data(), 8, layout.n_params, f) == layout.n_params &&
              fread(spec.data(), 4, layout.n_spec, f) == layout.n_spec;
    fclose(f);
    if (!ok) throw std::runtime_error("shard: short record read in " + ref.file.string());
}

}  // namespace ql_atmoforge

#include "npy.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "params.h"
#include "parse.h"
#include "shard.h"
#include "util.h"

using json = nlohmann::ordered_json;

namespace ql_atmoforge {

// ------------------------------------------------------------ NpyWriter --

static size_t descr_itemsize(const std::string& descr) {
    if (descr == "<f4") return 4;
    if (descr == "<f8") return 8;
    if (descr == "<u8") return 8;
    if (descr == "|u1") return 1;
    throw std::runtime_error("npy: unsupported descr " + descr);
}

NpyWriter::NpyWriter(const fs::path& path, const std::string& descr,
                     const std::vector<uint64_t>& shape)
    : path_(path) {
    expected_ = descr_itemsize(descr);
    std::string sh = "(";
    for (size_t i = 0; i < shape.size(); ++i) {
        expected_ *= shape[i];
        sh += std::to_string(shape[i]);
        if (i + 1 < shape.size() || shape.size() == 1) sh += ",";
        if (i + 1 < shape.size()) sh += " ";
    }
    sh += ")";
    std::string dict = "{'descr': '" + descr +
                       "', 'fortran_order': False, 'shape': " + sh + ", }";
    // total header (magic 6 + version 2 + len 2 + dict) padded to 64 bytes
    size_t base = 10;
    size_t total = ((base + dict.size() + 1 + 63) / 64) * 64;
    dict.resize(total - base - 1, ' ');
    dict += '\n';

    f_ = fopen(path.string().c_str(), "wb");
    if (!f_) throw std::runtime_error("npy: cannot create " + path.string());
    unsigned char magic[8] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
    uint16_t hlen = (uint16_t)dict.size();
    fwrite(magic, 1, 8, f_);
    fwrite(&hlen, 2, 1, f_);
    fwrite(dict.data(), 1, dict.size(), f_);
}

NpyWriter::~NpyWriter() {
    if (f_) fclose(f_);
}

void NpyWriter::write(const void* data, size_t bytes) {
    if (fwrite(data, 1, bytes, f_) != bytes)
        throw std::runtime_error("npy: write failed for " + path_.string());
    written_ += bytes;
}

void NpyWriter::close() {
    if (!f_) return;
    fclose(f_);
    f_ = nullptr;
    if (written_ != expected_)
        throw std::runtime_error(strf("npy: %s got %llu bytes, expected %llu",
                                      path_.string().c_str(),
                                      (unsigned long long)written_,
                                      (unsigned long long)expected_));
}

void write_npy(const fs::path& path, const std::string& descr,
               const std::vector<uint64_t>& shape, const void* data, size_t bytes) {
    NpyWriter w(path, descr, shape);
    w.write(data, bytes);
    w.close();
}

// ---------------------------------------------------------------- merge --

// A spectral block of the shard record: tau, lpath, or ldown.
struct Block {
    std::string name;
    std::vector<std::string> all_cols;  // canonical order inside the shard
    std::vector<size_t> keep;           // indices retained by the whitelist
    size_t offset = 0;                  // float offset inside the spec area
};

void merge_dataset(const Config& cfg) {
    ParamSpace space(cfg);
    const uint64_t K = (uint64_t)cfg.band.K();
    const uint32_t P = (uint32_t)space.n_features();

    std::vector<Block> blocks;
    blocks.push_back({"tau", tau_column_names(), {}, 0});
    blocks.push_back({"lpath", radiance_column_names(), {}, 0});
    if (cfg.band.thermal)
        blocks.push_back({"ldown", radiance_column_names(), {}, 0});
    size_t spec_floats = 0;
    for (auto& b : blocks) {
        b.offset = spec_floats;
        spec_floats += b.all_cols.size() * K;
        for (size_t c = 0; c < b.all_cols.size(); ++c)
            if (cfg.columns.empty() ||
                std::find(cfg.columns.begin(), cfg.columns.end(), b.all_cols[c])
                    != cfg.columns.end())
                b.keep.push_back(c);
    }

    ShardLayout layout{P, (uint32_t)spec_floats};
    auto records = scan_shards(cfg.out_dir / "shards", layout);
    // canonical dataset = indices below the configured n_samples
    std::vector<ShardRecordRef> ordered;
    for (const auto& [idx, ref] : records)
        if (idx < cfg.n_samples) ordered.push_back(ref);
    if (ordered.empty())
        throw std::runtime_error("merge: no shard records found under " +
                                 (cfg.out_dir / "shards").string());
    const uint64_t N = ordered.size();
    if (N < cfg.n_samples)
        fprintf(stderr, "merge: warning: %llu of %llu samples present "
                        "(run `ql-atmoforge gen` to completion first?)\n",
                (unsigned long long)N, (unsigned long long)cfg.n_samples);

    fs::create_directories(cfg.out_dir);

    // stream every array; a full vis dataset does not fit in memory
    NpyWriter w_params(cfg.out_dir / "params.npy", "<f8", {N, P});
    NpyWriter w_index(cfg.out_dir / "index.npy", "<u8", {N});
    NpyWriter w_status(cfg.out_dir / "status.npy", "|u1", {N});
    std::vector<std::unique_ptr<NpyWriter>> w_blocks;
    for (const auto& b : blocks)
        w_blocks.push_back(std::make_unique<NpyWriter>(
            cfg.out_dir / (b.name + ".npy"), "<f4",
            std::vector<uint64_t>{N, (uint64_t)b.keep.size(), K}));

    uint64_t n_ok = 0, n_partial = 0, n_failed = 0;
    std::vector<double> params;
    std::vector<float> spec;
    for (const auto& ref : ordered) {
        read_shard_record(ref, layout, params, spec);
        w_params.write(params.data(), 8ull * P);
        w_index.write(&ref.index, 8);
        uint8_t st = (uint8_t)ref.status;
        w_status.write(&st, 1);
        switch (ref.status) {
            case SampleStatus::Ok:      ++n_ok; break;
            case SampleStatus::Partial: ++n_partial; break;
            case SampleStatus::Failed:  ++n_failed; break;
        }
        for (size_t bi = 0; bi < blocks.size(); ++bi)
            for (size_t c : blocks[bi].keep)
                w_blocks[bi]->write(spec.data() + blocks[bi].offset + c * K, 4ull * K);
    }
    w_params.close();
    w_index.close();
    w_status.close();
    for (auto& w : w_blocks) w->close();

    // wavenumber grid (identical for every sample by construction)
    {
        std::vector<float> wn((size_t)K);
        for (uint64_t i = 0; i < K; ++i)
            wn[(size_t)i] = (float)(cfg.band.v1_cm + (double)i * cfg.band.dv_cm);
        write_npy(cfg.out_dir / "wavenumber.npy", "<f4", {K},
                  wn.data(), wn.size() * 4);
    }

    // failures.jsonl: one line per failed sample, collected from the archive
    {
        std::ofstream out(cfg.out_dir / "failures.jsonl", std::ios::trunc);
        fs::path fdir = cfg.out_dir / "failures";
        if (fs::exists(fdir)) {
            std::vector<fs::path> entries;
            for (const auto& e : fs::directory_iterator(fdir))
                if (fs::exists(e.path() / "error.json"))
                    entries.push_back(e.path() / "error.json");
            std::sort(entries.begin(), entries.end());
            for (const auto& p : entries) {
                std::string text = read_text_file(p.string());
                out << rstrip(text) << "\n";
            }
        }
    }

    // manifest
    {
        json man;
        man["format"] = "ql-atmoforge-dataset-v1";
        std::time_t now = std::time(nullptr);
        char ts[64];
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
        man["created"] = ts;
        man["seed"] = cfg.seed;
        man["sampler"] = space.sampler_name();
        man["n_samples"] = cfg.n_samples;
        man["n_present"] = N;
        man["counts"] = {{"ok", n_ok}, {"partial", n_partial}, {"failed", n_failed}};
        man["path_type"] = path_type_name(cfg.path_type);
        man["band"] = {{"name", cfg.band.name}, {"v1_cm", cfg.band.v1_cm},
                       {"v2_cm", cfg.band.v2_cm}, {"dv_cm", cfg.band.dv_cm},
                       {"fwhm_cm", cfg.band.fwhm_cm}, {"K", K},
                       {"thermal", cfg.band.thermal}};
        man["feature_names"] = space.feature_names();
        json sampled = json::object();
        for (const auto& [name, dist] : space.sampled()) {
            json d;
            switch (dist.kind) {
                case Dist::Values:     d["values"] = dist.values; break;
                case Dist::Uniform:    d["uniform"] = {dist.lo, dist.hi}; break;
                case Dist::LogUniform: d["log_uniform"] = {dist.lo, dist.hi}; break;
            }
            if (dist.grid_n) d["grid_n"] = dist.grid_n;
            sampled[name] = d;
        }
        man["sampled"] = sampled;
        // effective value of every non-sampled parameter (config or default)
        json fixed_eff = json::object();
        {
            SampleParams defaults;
            for (const auto& d : param_table()) {
                bool is_sampled = false;
                for (const auto& [sn, sd] : space.sampled())
                    if (sn == d.name) { is_sampled = true; break; }
                if (is_sampled) continue;
                double v = d.get(defaults);
                for (const auto& [fn, fv] : cfg.fixed)
                    if (fn == d.name) v = fv;
                fixed_eff[d.name] = v;
            }
        }
        man["fixed_effective"] = fixed_eff;
        json arrays = json::object();
        arrays["params"] = {{"file", "params.npy"}, {"dtype", "float64"},
                            {"shape", {N, P}}};
        for (const auto& b : blocks) {
            std::vector<std::string> kept;
            for (size_t c : b.keep) kept.push_back(b.all_cols[c]);
            arrays[b.name] = {{"file", b.name + ".npy"}, {"dtype", "float32"},
                              {"shape", {N, kept.size(), K}}, {"columns", kept}};
        }
        arrays["wavenumber"] = {{"file", "wavenumber.npy"}, {"dtype", "float32"},
                                {"shape", {K}}, {"units", "cm-1"}};
        arrays["index"] = {{"file", "index.npy"}, {"dtype", "uint64"}, {"shape", {N}}};
        arrays["status"] = {{"file", "status.npy"}, {"dtype", "uint8"}, {"shape", {N}},
                            {"legend", {{"0", "ok"}, {"1", "partial"}, {"2", "failed"}}}};
        man["arrays"] = arrays;
        man["units"] = {
            {"tau", "dimensionless transmittance (LOG_TOTAL is -ln)"},
            {"lpath", "W cm-2 sr-1 / cm-1 (spectral radiance per wavenumber; "
                      "TOT_TRANS and DEPTH dimensionless)"},
            {"ldown", "same as lpath, viewed from target altitude looking up"},
        };
        json mod;
        mod["exe"] = cfg.exe.string();
        std::error_code ec;
        uint64_t sz = fs::exists(cfg.exe, ec) ? (uint64_t)fs::file_size(cfg.exe, ec) : 0;
        mod["exe_size_bytes"] = sz;
        mod["data_dir"] = cfg.data_dir.string();
        man["modtran"] = mod;

        std::ofstream mf(cfg.out_dir / "manifest.json", std::ios::trunc);
        mf << man.dump(2) << "\n";
    }

    // preview CSVs: eyeball K rows x all kept columns per sample
    if (cfg.csv_preview > 0) {
        fs::path pdir = cfg.out_dir / "preview";
        fs::create_directories(pdir);
        int emitted = 0;
        for (const auto& ref : ordered) {
            if (emitted >= cfg.csv_preview) break;
            if (ref.status == SampleStatus::Failed) continue;
            read_shard_record(ref, layout, params, spec);
            std::ofstream csv(pdir / strf("sample_%06llu.csv",
                                          (unsigned long long)ref.index));
            csv << "wavenumber_cm";
            for (const auto& b : blocks)
                for (size_t c : b.keep)
                    csv << "," << b.name << "." << b.all_cols[c];
            csv << "\n";
            for (uint64_t k = 0; k < K; ++k) {
                csv << strf("%.1f", cfg.band.v1_cm + (double)k * cfg.band.dv_cm);
                for (const auto& b : blocks)
                    for (size_t c : b.keep)
                        csv << strf(",%.6g", spec[b.offset + c * K + k]);
                csv << "\n";
            }
            ++emitted;
        }
    }

    printf("merge: %llu samples (%llu ok, %llu partial, %llu failed) -> %s\n",
           (unsigned long long)N, (unsigned long long)n_ok,
           (unsigned long long)n_partial, (unsigned long long)n_failed,
           cfg.out_dir.string().c_str());
}

}  // namespace ql_atmoforge

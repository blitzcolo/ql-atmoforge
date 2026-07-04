#include "worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "datalink.h"
#include "parse.h"
#include "subprocess.h"
#include "shard.h"
#include "tape5.h"
#include "util.h"

using json = nlohmann::ordered_json;

namespace ql_atmoforge {

namespace {

struct BlockDef {
    RunKind kind;
    const std::vector<std::string>* cols;
    size_t offset;  // float offset in the spec area
};

struct SampleOutcome {
    SampleStatus status = SampleStatus::Ok;
    std::string error;       // failure reason (Failed only)
    std::string fail_run;    // run kind that failed
    std::vector<std::string> warnings;
};

// Every MODTRAN invocation gets a unique file prefix (via modroot.in), so a
// run NEVER recreates a file name that was just deleted. Windows keeps a
// deleted file in delete-pending state while a scanner (Defender, indexer)
// still holds a handle; recreating that name fails and MODTRAN dies at
// startup with *ERR* IO-09. Unique names sidestep the race entirely and
// make stale-output cleanup safe to do lazily and best-effort.
std::string run_prefix(uint64_t counter) {
    // modulo keeps the prefix at exactly 8 chars forever; by the time a
    // counter value repeats, the file from its previous use is long gone
    return strf("r%06llu_", (unsigned long long)(counter % 1000000));
}

// best-effort removal of outputs from earlier runs (keep the current prefix)
void remove_stale_outputs(const fs::path& wdir, const std::string& keep_prefix) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(wdir, ec)) {
        std::string n = e.path().filename().string();
        bool ours = (n.rfind("TRANS.", 0) == 0) ||
                    (n.size() > 8 && n[0] == 'r' && n[7] == '_' &&
                     n.compare(0, keep_prefix.size(), keep_prefix) != 0);
        if (ours) fs::remove(e.path(), ec);  // delete-pending errors are fine
    }
}

void archive_failure(const fs::path& out_dir, const fs::path& wdir,
                     const std::string& prefix, uint64_t index, RunKind rk,
                     const SampleOutcome& oc, const SampleParams& p) {
    std::error_code ec;
    fs::path dst = out_dir / "failures" / strf("%06llu", (unsigned long long)index);
    fs::create_directories(dst, ec);
    const char* kind = run_kind_name(rk);
    for (const char* suf : {".tp5", ".tp6", ".tp7"})
        if (fs::exists(wdir / (prefix + suf), ec))
            fs::copy_file(wdir / (prefix + suf), dst / (kind + std::string(suf)),
                          fs::copy_options::overwrite_existing, ec);
    fs::path log = wdir / (std::string("run_") + kind + ".log");
    if (fs::exists(log, ec))
        fs::copy_file(log, dst / log.filename(), fs::copy_options::overwrite_existing, ec);

    json e;
    e["index"] = index;
    e["run"] = run_kind_name(rk);
    e["error"] = oc.error;
    e["warnings"] = oc.warnings;
    json pj = json::object();
    for (const auto& d : param_table()) pj[d.name] = d.get(p);
    pj["h2_km"] = p.h2_km;
    pj["range_km_resolved"] = p.range_km;
    e["params"] = pj;
    std::ofstream f(dst / "error.json", std::ios::trunc);
    f << e.dump() << "\n";  // single line: merge concatenates into .jsonl
}

void write_warnings_sidecar(const fs::path& out_dir, uint64_t index,
                            const SampleOutcome& oc) {
    std::error_code ec;
    fs::path dst = out_dir / "failures" / strf("%06llu", (unsigned long long)index);
    fs::create_directories(dst, ec);
    std::ofstream f(dst / "warnings.txt", std::ios::trunc);
    for (const auto& w : oc.warnings) f << w << "\n";
}

class Generator {
public:
    Generator(const Config& cfg, const ParamSpace& space)
        : cfg_(cfg), space_(space), K_((uint64_t)cfg.band.K()) {
        blocks_.push_back({RunKind::Tau, &tau_column_names(), 0});
        size_t off = tau_column_names().size() * K_;
        blocks_.push_back({RunKind::Lpath, &radiance_column_names(), off});
        off += radiance_column_names().size() * K_;
        if (cfg.band.thermal) {
            blocks_.push_back({RunKind::Ldown, &radiance_column_names(), off});
            off += radiance_column_names().size() * K_;
        }
        layout_.n_params = (uint32_t)space.n_features();
        layout_.n_spec = (uint32_t)off;
    }

    const ShardLayout& layout() const { return layout_; }

    GenSummary run() {
        GenSummary sum;
        sum.total = cfg_.n_samples;

        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 4;
        unsigned W = cfg_.workers > 0 ? (unsigned)cfg_.workers : hw;
        if ((uint64_t)W > cfg_.n_samples) W = (unsigned)cfg_.n_samples;

        fs::create_directories(cfg_.out_dir / "shards");
        fs::create_directories(cfg_.out_dir / "failures");

        // resume: whatever is already in the shards is done (ok AND failed)
        for (const auto& [idx, ref] : scan_shards(cfg_.out_dir / "shards", layout_))
            if (idx < cfg_.n_samples) done_.insert(idx);
        sum.skipped = done_.size();
        if (sum.skipped)
            printf("resume: %llu samples already in shards, skipping\n",
                   (unsigned long long)sum.skipped);

        // worker dirs + Data links, serial, before any thread starts
        std::vector<fs::path> wdirs;
        for (unsigned w = 0; w < W; ++w) {
            fs::path wd = cfg_.out_dir / "workers" / strf("worker_%u", w);
            fs::create_directories(wd);
            std::string how = ensure_data_link(wd, cfg_.data_dir,
                                               cfg_.link_mode, cfg_.exe);
            if (w == 0) printf("data link: %s\n", how.c_str());
            wdirs.push_back(wd);
        }
        printf("workers: %u, samples: %llu, runs per sample: %zu\n",
               W, (unsigned long long)cfg_.n_samples, blocks_.size());

        std::vector<std::thread> threads;
        for (unsigned w = 0; w < W; ++w)
            threads.emplace_back([this, w, &wdirs, &sum] {
                worker_main(w, wdirs[w], sum);
            });
        for (auto& t : threads) t.join();
        return sum;
    }

private:
    void worker_main(unsigned w, const fs::path& wdir, GenSummary& sum) {
        ShardWriter shard(cfg_.out_dir / "shards" / strf("shard_%u.bin", w), layout_);
        std::vector<float> spec((size_t)layout_.n_spec);
        uint64_t run_counter = 0;

        for (;;) {
            uint64_t i = next_.fetch_add(1);
            if (i >= cfg_.n_samples) break;
            if (done_.count(i)) continue;

            auto t0 = std::chrono::steady_clock::now();
            SampleParams p = space_.at(i);
            std::vector<double> feats = space_.features(p);
            std::fill(spec.begin(), spec.end(), 0.0f);

            SampleOutcome oc = run_sample(wdir, i, p, spec, run_counter);
            shard.append(i, oc.status, feats.data(), spec.data());

            double dt = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - t0).count();
            {
                std::lock_guard<std::mutex> lk(print_mtx_);
                switch (oc.status) {
                    case SampleStatus::Ok:
                        ++sum.ok;
                        printf("[w%u] #%llu ok (%.1fs)\n", w,
                               (unsigned long long)i, dt);
                        break;
                    case SampleStatus::Partial:
                        ++sum.partial;
                        printf("[w%u] #%llu PARTIAL (%.1fs): %s\n", w,
                               (unsigned long long)i, dt,
                               oc.warnings.empty() ? "" : oc.warnings[0].c_str());
                        break;
                    case SampleStatus::Failed:
                        ++sum.failed;
                        printf("[w%u] #%llu FAILED in %s (%.1fs): %s\n", w,
                               (unsigned long long)i, oc.fail_run.c_str(), dt,
                               oc.error.c_str());
                        break;
                }
                fflush(stdout);
            }
        }
    }

    SampleOutcome run_sample(const fs::path& wdir, uint64_t index,
                             const SampleParams& p, std::vector<float>& spec,
                             uint64_t& run_counter) {
        SampleOutcome oc;
        for (const auto& blk : blocks_) {
            std::string prefix = run_prefix(run_counter++);
            std::string err = run_one(wdir, prefix, p, blk, spec, oc.warnings);
            if (!err.empty()) {
                // one retry: transient environment failures (scanner holding
                // handles, exhausted resources) look identical to real ones
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                oc.warnings.push_back(std::string(run_kind_name(blk.kind)) +
                                      ": retried after: " + err);
                prefix = run_prefix(run_counter++);
                err = run_one(wdir, prefix, p, blk, spec, oc.warnings);
            }
            if (!err.empty()) {
                oc.status = SampleStatus::Failed;
                oc.error = err;
                oc.fail_run = run_kind_name(blk.kind);
                std::fill(spec.begin(), spec.end(), 0.0f);  // no half-samples
                archive_failure(cfg_.out_dir, wdir, prefix, index, blk.kind, oc, p);
                return oc;
            }
        }
        if (!oc.warnings.empty()) {
            oc.status = SampleStatus::Partial;
            write_warnings_sidecar(cfg_.out_dir, index, oc);
        }
        // keep the worker dir lean on success
        remove_stale_outputs(wdir, "");
        return oc;
    }

    // one MODTRAN invocation; returns "" on success, error text otherwise
    std::string run_one(const fs::path& wdir, const std::string& prefix,
                        const SampleParams& p, const BlockDef& blk,
                        std::vector<float>& spec,
                        std::vector<std::string>& warnings) {
        remove_stale_outputs(wdir, prefix);
        {
            std::ofstream mr(wdir / "modroot.in", std::ios::trunc);
            mr << prefix;
        }
        {
            std::ofstream tp5(wdir / (prefix + ".tp5"), std::ios::trunc);
            tp5 << tape5_text(p, cfg_.band, cfg_.path_type, blk.kind);
            if (!tp5) return "cannot write " + prefix + ".tp5 in " + wdir.string();
        }
        fs::path log = wdir / (std::string("run_") + run_kind_name(blk.kind) + ".log");
        RunResult rr = run_process(cfg_.exe, wdir, log, cfg_.timeout_s);
        if (rr.status == RunResult::StartFailed || rr.status == RunResult::Timeout)
            return rr.message;
        // non-zero exit is recorded but tape6 decides -- MODTRAN returns 0
        // even on fatal errors, and some wrappers return junk on success
        Tape6Status t6 = check_tape6(wdir / (prefix + ".tp6"));
        if (!t6.success)
            return t6.first_error + (rr.exit_code ? strf(" [exit %d]", rr.exit_code) : "");
        for (const auto& wmsg : t6.warnings)
            warnings.push_back(std::string(run_kind_name(blk.kind)) + ": " + wmsg);

        std::ifstream tp7(wdir / (prefix + ".tp7"));
        if (!tp7) return prefix + ".tp7 missing after successful run";
        Spectrum sp = blk.kind == RunKind::Tau
                          ? parse_tape7_transmittance(tp7)
                          : parse_tape7_radiance(tp7);
        if (!sp.ok) return sp.error;
        for (const auto& wmsg : sp.warnings)
            warnings.push_back(std::string(run_kind_name(blk.kind)) + ": " + wmsg);

        // grid validation: exactly the configured K on the configured bounds
        if ((uint64_t)sp.wavenumber_cm.size() != K_)
            return strf("%s: parsed %zu points, expected %llu", run_kind_name(blk.kind),
                        sp.wavenumber_cm.size(), (unsigned long long)K_);
        if (std::fabs(sp.wavenumber_cm.front() - cfg_.band.v1_cm) > 0.5 * cfg_.band.dv_cm ||
            std::fabs(sp.wavenumber_cm.back() - cfg_.band.v2_cm) > 0.5 * cfg_.band.dv_cm)
            return strf("%s: grid [%g, %g] does not match band [%g, %g]",
                        run_kind_name(blk.kind), sp.wavenumber_cm.front(),
                        sp.wavenumber_cm.back(), cfg_.band.v1_cm, cfg_.band.v2_cm);

        // pack into the canonical column order; a missing column is NaN + warning
        const auto& want = *blk.cols;
        for (size_t c = 0; c < want.size(); ++c) {
            float* dst = spec.data() + blk.offset + c * K_;
            auto it = std::find(sp.names.begin(), sp.names.end(), want[c]);
            if (it == sp.names.end()) {
                for (uint64_t k = 0; k < K_; ++k)
                    dst[k] = std::numeric_limits<float>::quiet_NaN();
                warnings.push_back(std::string(run_kind_name(blk.kind)) +
                                   ": column missing in tape7: " + want[c]);
                continue;
            }
            const auto& src = sp.cols[(size_t)(it - sp.names.begin())];
            memcpy(dst, src.data(), 4ull * K_);
        }
        return "";
    }

    const Config& cfg_;
    const ParamSpace& space_;
    uint64_t K_;
    std::vector<BlockDef> blocks_;
    ShardLayout layout_;
    std::set<uint64_t> done_;
    std::atomic<uint64_t> next_{0};
    std::mutex print_mtx_;
};

}  // namespace

GenSummary run_generation(const Config& cfg, const ParamSpace& space) {
    Generator gen(cfg, space);
    return gen.run();
}

}  // namespace ql_atmoforge

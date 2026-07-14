#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include "config.h"
#include "datalink.h"
#include "npy.h"
#include "params.h"
#include "parse.h"
#include "subprocess.h"
#include "tape5.h"
#include "util.h"
#include "worker.h"

using namespace ql_atmoforge;

static void usage() {
    fprintf(stderr,
        "ql-atmoforge - MODTRAN atmospheric dataset generator\n"
        "\n"
        "usage:\n"
        "  ql-atmoforge gen    <config.json>          generate samples (resumable)\n"
        "  ql-atmoforge merge  <config.json>          shards -> npy dataset\n"
        "  ql-atmoforge print  <config.json> -i <N>   show sample N: params + tape5 cards\n"
        "  ql-atmoforge doctor <config.json>          environment self-check + trial run\n");
}

static void cmd_print(const Config& cfg, uint64_t index) {
    ParamSpace space(cfg);
    SampleParams p = space.at(index);

    printf("sample %llu  (seed=%llu sampler=%s path=%s band=%s K=%d)\n\n",
           (unsigned long long)index, (unsigned long long)cfg.seed,
           space.sampler_name().c_str(), path_type_name(cfg.path_type),
           cfg.band.name.c_str(), cfg.band.K());
    printf("resolved parameters:\n");
    for (const auto& d : param_table())
        printf("  %-22s %g\n", d.name, d.get(p));
    printf("  %-22s %g\n", "h2_km (derived)", p.h2_km);
    printf("  %-22s %g\n", "cos_view (derived)", p.cos_view_zenith);

    printf("\nfeature vector [%zu]:\n", space.n_features());
    auto f = space.features(p);
    for (size_t i = 0; i < f.size(); ++i)
        printf("  [%2zu] %-22s %g\n", i, space.feature_names()[i].c_str(), f[i]);

    std::vector<RunKind> runs = {RunKind::Tau, RunKind::Lpath};
    if (cfg.band.thermal && cfg.path_type != PathType::Sky)
        runs.push_back(RunKind::Ldown);
    for (RunKind rk : runs) {
        printf("\n----- tape5 (%s) -----\n%s", run_kind_name(rk),
               tape5_text(p, cfg.band, cfg.path_type, rk).c_str());
    }
}

static int cmd_doctor(const Config& cfg) {
    int fails = 0;
    auto check = [&](bool ok, const std::string& what, const std::string& detail) {
        printf("  [%s] %s%s%s\n", ok ? "ok" : "FAIL", what.c_str(),
               detail.empty() ? "" : ": ", detail.c_str());
        if (!ok) ++fails;
    };

    printf("doctor:\n");
    check(fs::exists(cfg.exe), "exe exists", cfg.exe.string());
    if (fs::exists(cfg.exe))
        check(true, "exe size", strf("%llu bytes",
              (unsigned long long)fs::file_size(cfg.exe)));
    size_t nfiles = 0;
    if (fs::exists(cfg.data_dir))
        for (const auto& e : fs::directory_iterator(cfg.data_dir))
            if (e.is_regular_file()) ++nfiles;
    check(fs::exists(cfg.data_dir), "data_dir exists",
          cfg.data_dir.string() + strf(" (%zu files)", nfiles));

    bool winlink = needs_windows_link(cfg.exe);
    printf("  [info] platform: %s%s\n",
#ifdef _WIN32
           "windows",
#else
           is_wsl() ? "linux (WSL)" : "linux",
#endif
           winlink ? ", Windows exe -> junction link required" : "");
    if (winlink) {
        std::string abs = fs::absolute(cfg.out_dir).string();
        check(abs.rfind("/mnt/", 0) == 0 || abs.size() < 2 || abs[1] == ':',
              "out_dir reachable by Windows exe", abs);
    }

    try {
        fs::path wd = cfg.out_dir / "workers" / "worker_0";
        fs::create_directories(wd);
        std::string how = ensure_data_link(wd, cfg.data_dir, cfg.link_mode, cfg.exe);
        check(true, "data link", how);

        // trial run: sample 0, tau
        ParamSpace space(cfg);
        SampleParams p = space.at(0);
        std::ofstream mr(wd / "modroot.in", std::ios::trunc);
        mr << "TRANS";
        mr.close();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(wd))
            if (e.path().filename().string().rfind("TRANS.", 0) == 0)
                fs::remove(e.path(), ec);
        std::ofstream tp5(wd / "TRANS.tp5", std::ios::trunc);
        tp5 << tape5_text(p, cfg.band, cfg.path_type, RunKind::Tau);
        tp5.close();

        printf("  [..] trial MODTRAN run (sample 0, tau) ...\n");
        RunResult rr = run_process(cfg.exe, wd, wd / "run_doctor.log", cfg.timeout_s);
        check(rr.status == RunResult::Ok || rr.status == RunResult::NonZeroExit,
              "process ran", rr.message);
        Tape6Status t6 = check_tape6(wd / "TRANS.tp6");
        check(t6.success, "tape6 CARD 5 marker", t6.first_error);
        std::ifstream tp7(wd / "TRANS.tp7");
        Spectrum sp = parse_tape7_transmittance(tp7);
        check(sp.ok && (int)sp.wavenumber_cm.size() == cfg.band.K(),
              "tape7 parsed",
              sp.ok ? strf("%zu points, %zu columns, TOTAL[0]=%g",
                           sp.wavenumber_cm.size(), sp.names.size(),
                           sp.cols.empty() ? 0.0 : (double)sp.cols[0][0])
                    : sp.error);
    } catch (const std::exception& e) {
        check(false, "worker setup / trial run", e.what());
    }

    printf(fails ? "doctor: %d problem(s) found\n" : "doctor: all good\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    fs::path cfg_path = argv[2];

    try {
        Config cfg = load_config(cfg_path);

        if (cmd == "gen") {
            ParamSpace space(cfg);
            printf("gen: %s, band=%s (K=%d), path=%s, sampler=%s, seed=%llu\n",
                   cfg.out_dir.string().c_str(), cfg.band.name.c_str(), cfg.band.K(),
                   path_type_name(cfg.path_type), cfg.sampler.c_str(),
                   (unsigned long long)cfg.seed);
            GenSummary s = run_generation(cfg, space);
            printf("done: %llu ok, %llu partial, %llu failed, %llu skipped (resume)\n",
                   (unsigned long long)s.ok, (unsigned long long)s.partial,
                   (unsigned long long)s.failed, (unsigned long long)s.skipped);
            printf("next: ql-atmoforge merge %s\n", cfg_path.string().c_str());
            return s.ok + s.partial + s.skipped > 0 ? 0 : 1;
        }
        if (cmd == "merge") {
            merge_dataset(cfg);
            return 0;
        }
        if (cmd == "print") {
            uint64_t index = 0;
            for (int i = 3; i + 1 < argc; ++i)
                if (!strcmp(argv[i], "-i")) index = strtoull(argv[i + 1], nullptr, 10);
            if (index >= cfg.n_samples)
                fprintf(stderr, "note: index %llu >= n_samples %llu (printing anyway)\n",
                        (unsigned long long)index, (unsigned long long)cfg.n_samples);
            cmd_print(cfg, index);
            return 0;
        }
        if (cmd == "doctor") {
            return cmd_doctor(cfg);
        }
        usage();
        return 2;
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

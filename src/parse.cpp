#include "parse.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>

#include "util.h"

namespace ql_atmoforge {

const std::vector<std::string>& tau_column_names() {
    // tape7 IEMSCT=0 header order: FREQ TOTAL H2O CO2+ O3 TRACE N2 H2O MOLEC
    // AER HNO3 AERab -LOG CO2 CO CH4 N2O O2 NH3 NO NO2 SO2, with the units
    // row disambiguating the duplicates (N2 CONT, H2O CONT, MOLEC SCAT).
    static const std::vector<std::string> n = {
        "TOTAL", "H2O", "CO2P", "O3", "TRACE", "N2_CONT", "H2O_CONT",
        "MOLEC_SCAT", "AER", "HNO3", "AER_AB", "LOG_TOTAL",
        "CO2", "CO", "CH4", "N2O", "O2", "NH3", "NO", "NO2", "SO2",
    };
    return n;
}

const std::vector<std::string>& radiance_column_names() {
    // tape7 IEMSCT=2 header: FREQ TOT TRANS PTH THRML THRML SCT SURF EMIS
    // SOL SCAT SING SCAT GRND RFLT DRCT RFLT TOTAL RAD REF SOL SOL@OBS DEPTH
    static const std::vector<std::string> n = {
        "TOT_TRANS", "PTH_THRML", "THRML_SCT", "SURF_EMIS", "SOL_SCAT",
        "SING_SCAT", "GRND_RFLT", "DRCT_RFLT", "TOTAL_RAD",
        "REF_SOL", "SOL_OBS", "DEPTH",
    };
    return n;
}

static bool parse_num(const std::string& s, double& out) {
    const char* c = s.c_str();
    char* end = nullptr;
    out = std::strtod(c, &end);
    if (end == c) return false;
    while (*end == ' ') ++end;
    return *end == '\0';
}

// ---------------------------------------------------- transmittance table --

// Map a (name, unit) header pair to the canonical column name.
static std::string canon_tau_name(const std::string& name, const std::string& unit) {
    if (name == "-LOG") return "LOG_TOTAL";
    if (name == "AERab") return "AER_AB";  // aerosol absorption
    std::string out;
    for (char c : name) out += (c == '+') ? 'P' : c;  // CO2+ -> CO2P
    if (unit == "CONT") out += "_CONT";
    if (unit == "SCAT") out += "_SCAT";
    return out;
}

Spectrum parse_tape7_transmittance(std::istream& in) {
    Spectrum sp;
    std::string line;

    // locate the header pair: "FREQ TOTAL ..." then "CM-1 TRANS ..."
    std::vector<std::string> names_row, units_row;
    while (std::getline(in, line)) {
        auto tok = tokenize(rstrip(line));
        if (tok.size() >= 3 && tok[0] == "FREQ" && tok[1] == "TOTAL") {
            names_row = tok;
            if (std::getline(in, line)) units_row = tokenize(rstrip(line));
            break;
        }
    }
    if (names_row.empty()) {
        sp.error = "transmittance header not found in tape7";
        return sp;
    }
    if (units_row.size() != names_row.size()) {
        sp.error = strf("transmittance units row mismatch (%zu vs %zu tokens)",
                        units_row.size(), names_row.size());
        return sp;
    }
    for (size_t i = 1; i < names_row.size(); ++i)
        sp.names.push_back(canon_tau_name(names_row[i], units_row[i]));
    sp.cols.assign(sp.names.size(), {});

    const size_t ntok = names_row.size();
    while (std::getline(in, line)) {
        auto tok = tokenize(rstrip(line));
        if (tok.empty()) continue;
        double freq;
        if (!parse_num(tok[0], freq)) break;      // some trailing text: done
        if (freq <= -9999.0 + 0.5) break;         // "-9999." terminator
        if (tok.size() != ntok) {
            sp.warnings.push_back(strf("row with %zu tokens (expected %zu) at freq %.1f",
                                       tok.size(), ntok, freq));
            continue;
        }
        sp.wavenumber_cm.push_back(freq);
        for (size_t i = 1; i < ntok; ++i) {
            double v;
            if (!parse_num(tok[i], v)) v = std::numeric_limits<double>::quiet_NaN();
            sp.cols[i - 1].push_back((float)v);
        }
    }
    if (sp.wavenumber_cm.empty()) {
        sp.error = "transmittance table has no data rows";
        return sp;
    }
    sp.ok = true;
    return sp;
}

// -------------------------------------------------------- radiance table --

Spectrum parse_tape7_radiance(std::istream& in) {
    Spectrum sp;
    std::string line;

    // header: "  FREQ   TOT TRANS  PTH THRML ..."
    bool found = false;
    while (std::getline(in, line)) {
        std::string l = rstrip(line);
        if (l.find("FREQ") != std::string::npos &&
            l.find("TOT TRANS") != std::string::npos) {
            found = true;
            break;
        }
    }
    if (!found) {
        sp.error = "radiance header not found in tape7";
        return sp;
    }
    sp.names = radiance_column_names();
    const size_t C = sp.names.size();
    sp.cols.assign(C, {});

    // fixed-width row: FREQ(7) + 9 fields x 11 + REF SOL(9) + SOL@OBS(9)
    // + DEPTH(8) = 132 chars. Whitespace tokenizing is unsafe here because
    // THRML SCT prints fully blank; slice by position instead.
    struct Slice { size_t pos, len; };
    static const Slice kSlices[12] = {
        {7, 11}, {18, 11}, {29, 11}, {40, 11}, {51, 11}, {62, 11},
        {73, 11}, {84, 11}, {95, 11}, {106, 9}, {115, 9}, {124, 8},
    };
    while (std::getline(in, line)) {
        std::string l = rstrip(line);
        if (l.size() < 7) { if (l.empty()) continue; break; }
        double freq;
        std::string ftok = l.substr(0, 7);
        if (!parse_num(ftok, freq)) break;
        if (freq <= -9999.0 + 0.5) break;
        sp.wavenumber_cm.push_back(freq);
        for (size_t c = 0; c < C; ++c) {
            // Blank field (or a line ending early) is how MODTRAN prints an
            // exact zero -- e.g. THRML SCT is fully blank without multiple
            // scattering. Zero, not missing data; no warning.
            float v = 0.0f;
            if (kSlices[c].pos < l.size()) {
                std::string field = l.substr(kSlices[c].pos,
                                             std::min(kSlices[c].len,
                                                      l.size() - kSlices[c].pos));
                if (field.find_first_not_of(' ') != std::string::npos) {
                    double d;
                    if (parse_num(field, d)) v = (float)d;
                    else {
                        v = std::numeric_limits<float>::quiet_NaN();
                        sp.warnings.push_back(strf("unparseable %s field at freq %.1f: '%s'",
                                                   sp.names[c].c_str(), freq, field.c_str()));
                    }
                }
            }
            sp.cols[c].push_back(v);
        }
    }
    if (sp.wavenumber_cm.empty()) {
        sp.error = "radiance table has no data rows";
        return sp;
    }
    sp.ok = true;
    return sp;
}

// ---------------------------------------------------------------- tape6 --

// MODTRAN wraps diagnostics across lines ("...THE LIQUID WATER DROPLET
// DENSITY" / "IS POSITIVE [0.15 GM/M3] EVEN THOUGH..."); capturing only the
// matched line drops the half of the message that says what happened. Pull
// in indented continuation lines until a blank or a new left-margin block.
static std::string with_continuations(const std::vector<std::string>& lines,
                                      size_t i) {
    std::string msg = lines[i];
    for (size_t j = i + 1; j < lines.size() && j < i + 4; ++j) {
        const std::string& n = lines[j];
        if (n.empty() || n.find_first_not_of(' ') == std::string::npos) break;
        if (n.find_first_not_of(' ') < 6) break;  // new left-margin block
        msg += " " + n.substr(n.find_first_not_of(' '));
    }
    return msg;
}

Tape6Status check_tape6(const fs::path& tp6) {
    Tape6Status st;
    std::ifstream f(tp6);
    if (!f) {
        st.first_error = "tape6 missing: " + tp6.string();
        return st;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(rstrip(line));
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& l = lines[i];
        if (l.find("CARD 5") != std::string::npos) st.success = true;
        if (l.find("FATAL") != std::string::npos ||
            l.find("Error") != std::string::npos ||
            l.find("ERROR") != std::string::npos) {
            if (st.first_error.empty())
                st.first_error = with_continuations(lines, i);
        }
        if ((l.find("WAS RESET") != std::string::npos ||
             l.find("WARNING") != std::string::npos) &&
            st.warnings.size() < 10)
            st.warnings.push_back(with_continuations(lines, i));
    }
    if (!st.success && st.first_error.empty())
        st.first_error = "tape6 ended without CARD 5 marker";
    return st;
}

}  // namespace ql_atmoforge

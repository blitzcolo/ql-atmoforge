#pragma once
#include <filesystem>
#include <istream>
#include <string>
#include <vector>

namespace ql_atmoforge {
namespace fs = std::filesystem;

// Canonical column orders. Worker packs shard records in exactly this order
// and merge names the npy axes with it -- keep them in one place.
const std::vector<std::string>& tau_column_names();       // 21 components
const std::vector<std::string>& radiance_column_names();  // 12 components

struct Spectrum {
    std::vector<double> wavenumber_cm;        // ascending, as printed in tape7
    std::vector<std::string> names;           // component columns (FREQ excluded)
    std::vector<std::vector<float>> cols;     // [C][K]
    std::vector<std::string> warnings;        // blank columns, odd rows, ...
    bool ok = false;                          // table found and non-empty
    std::string error;                        // set when !ok
};

// tape7 IEMSCT=0 table: two header lines (names + units), whitespace tokens.
Spectrum parse_tape7_transmittance(std::istream& in);
// tape7 IEMSCT=2 table: fixed-width columns; THRML SCT is often fully blank.
Spectrum parse_tape7_radiance(std::istream& in);

struct Tape6Status {
    bool success = false;                 // "CARD 5" end marker found
    std::string first_error;              // first FATAL/Error line, if any
    std::vector<std::string> warnings;    // e.g. "H1 WAS RESET TO GNDALT"
};
// MODTRAN's exit code is a lie (0 on fatal errors) -- this is the real
// success criterion.
Tape6Status check_tape6(const fs::path& tp6);

}  // namespace ql_atmoforge

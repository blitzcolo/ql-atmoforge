#include "tape5.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "util.h"

namespace ql_atmoforge {

const char* run_kind_name(RunKind rk) {
    switch (rk) {
        case RunKind::Tau:   return "tau";
        case RunKind::Lpath: return "lpath";
        case RunKind::Ldown: return "ldown";
    }
    return "?";
}

std::string tape5_text(const SampleParams& p, const BandSpec& band,
                       PathType pt, RunKind rk) {
    const bool rad = (rk != RunKind::Tau);
    std::string t;
    t.reserve(4096);

    // CARD 1: MODTRN SPEED MODEL(I3) ITYPE IEMSCT IMULT M1..M6 MDEF IM NOPRNT
    //         TPTEMP(F8.3) SURREF(F7.2)
    // MODEL=7 + IM=1: user profile from CARD 2C/2C1 replaces the atmosphere;
    // M1..M6 point back to the model atmosphere so every level we leave blank
    // inherits it. TPTEMP=0 / SURREF=0 are deliberate: the dataset holds pure
    // atmospheric quantities; surface emission/reflection is composed
    // downstream from tau and ldown.
    t += strf("%c%c%3d%5d%5d%5d%5d%5d%5d%5d%5d%5d%5d%5d%5d%8.3f%7.2f\n",
              'T', ' ', 7, 2, rad ? 2 : 0, 0,
              p.atmos_model, p.atmos_model, p.atmos_model,
              p.atmos_model, p.atmos_model, p.atmos_model,
              1, 1, 0, 0.0, 0.0);

    // CARD 1A: (2L1, I3, L1, I4, F10.5, 2A10, ...) =
    //          DIS DISAZM NSTR LSUN ISUN CO2MX H2OSTR O3STR
    // No DISORT (DIS=F, Isaacs two-stream when scattering is needed).
    // LSUN=T + ISUN=5 in radiance mode: 5 cm-1 smoothed TOA solar irradiance.
    // H2OSTR/O3STR are A10 fields; a bare number means "column scale factor".
    t += strf("%c%c%3d%c%4d%10.5f%10.5f%10.5f\n",
              'F', 'F', 2, rad ? 'T' : 'F', 5,
              p.co2_ppmv, p.h2o_scale, p.o3_scale);

    // CARD 2: APLUS IHAZE CNOVAM ISEASN ARUSS IVULCN ICSTL ICLD IVSA
    //         VIS WSS WHH RAINRT GNDALT
    // VIS=0 keeps the IHAZE default visibility. GNDALT=0: ground at sea level.
    t += strf("%s%3d%c%4d%s%2d%5d%5d%5d%10.3f%10.3f%10.3f%10.3f%10.3f\n",
              "  ", p.ihaze, ' ', 0, "   ", 0, 0, p.icld, 0,
              p.vis_km, 0.0, 0.0, p.rainrt_mm_h, 0.0);

    // CARD 2A: cloud/rain defaults (-9 = model default) for water clouds,
    // cirrus form for ICLD 18/19.
    if (p.icld >= 1 && p.icld <= 10)
        t += strf("%8.3f%8.3f%8.3f%4d%4d%8.3f%8.3f%8.3f%8.3f%8.3f%8.3f\n",
                  -9.0, -9.0, -9.0, -9, -9, -9.0, -9.0, -9.0, -9.0, -9.0, -9.0);
    if (p.icld == 18 || p.icld == 19)
        t += strf("%8.3f%8.3f%8.3f%s\n", -9.0, -9.0, -9.0, "         0");

    // CARD 2C/2C1: full 0-100 km altitude grid so MODEL=7 has a complete
    // atmosphere (a truncated profile makes MODTRAN treat the top level as
    // TOA -> FATAL on upward paths). Only the GROUND level carries the
    // T/RH/P override (JCHAR='AAH'); every other level has blank JCHAR and
    // inherits the model atmosphere. h1 is inserted so path endpoints sit on
    // a level. NOTE: this is a deliberate fix over the legacy code, which put
    // the override at h1 -- wrong semantics for slant paths, where the
    // weather knobs describe conditions at the ground, not at the sensor.
    static const double kGridKm[] = {0, 1, 2, 3, 5, 8, 10, 15, 20, 25, 30, 40, 50, 70, 100};
    constexpr double EPS = 1e-6;
    std::vector<double> levels(std::begin(kGridKm), std::end(kGridKm));
    bool have_h1 = false;
    for (double z : levels)
        if (std::fabs(z - p.h1_km) <= EPS) have_h1 = true;
    if (!have_h1) levels.push_back(p.h1_km);
    std::sort(levels.begin(), levels.end());

    t += strf("%5d%5d%5d%s\n", (int)levels.size(), 0, 0, "GROUND T/P/RH OVERRIDE");
    for (size_t i = 0; i < levels.size(); ++i) {
        if (i == 0) {
            // ZM P T WMOL(1..3) JCHAR: 'A'=mb, 'A'=K, 'H'=WMOL(1) is RH%
            t += strf("%10.3f%10.3f%10.3f%10.3f%10.3f%10.3f%-14s %c\n",
                      levels[i], p.p_hPa, p.t_ground_K, p.rh * 100.0,
                      0.0, 0.0, "AAH", ' ');
        } else {
            t += strf("%10.3f%10.3f%10.3f%10.3f%10.3f%10.3f%-14s %c\n",
                      levels[i], 0.0, 0.0, 0.0, 0.0, 0.0, "", ' ');
        }
    }

    // CARD 3: H1 H2 ANGLE RANGE BETA RO (6F10.3) LENN(I5) 5X PHI(F10.3)
    double h1, h2, angle, range;
    if (rk == RunKind::Ldown) {
        // downwelling sky radiance at the TARGET position, looking up
        h1 = (pt == PathType::SlantToGround) ? 0.0 : p.h1_km;
        h2 = 100.0;
        angle = p.ldown_zenith_deg;
        range = 0.0;
    } else if (pt == PathType::Horizontal) {
        h1 = p.h1_km; h2 = p.h1_km; angle = 90.0; range = p.range_km;
    } else {
        h1 = p.h1_km; h2 = 0.0; angle = p.view_zenith_deg; range = 0.0;
    }
    t += strf("%10.3f%10.3f%10.3f%10.3f%10.3f%10.3f%5d%s%10.3f\n",
              h1, h2, angle, range, 0.0, 0.0, 0, "     ", 0.0);

    // CARD 3A1/3A2: solar geometry, radiance mode only. IPARM=2 takes the
    // sun directly as (relative azimuth LOS->sun, solar zenith at H1) --
    // no lat/lon/date/time ephemeris detour. IPH=2: internal Mie database.
    if (rad) {
        t += strf("%5d%5d%5d%5d\n", 2, 2, p.iday, 0);
        t += strf("%10.3f%10.3f%10.3f%10.3f%10.3f%10.3f%10.3f%10.3f\n",
                  p.sun_rel_azimuth_deg, p.sun_zenith_deg,
                  0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
    }

    // CARD 4: V1 V2 DV FWHM + FLAGS(7)='WTA    ' (W=cm-1, Triangular slit,
    // Absolute FWHM), YFLAG/XFLAG only steer .plt which we do not read.
    t += strf("%10.3f%10.3f%10.3f%10.3f%s%c%c\n",
              band.v1_cm, band.v2_cm, band.dv_cm, band.fwhm_cm,
              "WTA    ", rad ? 'R' : 'T', 'N');

    // CARD 5: IRPT=0, end of run
    t += strf("%5d\n", 0);
    return t;
}

}  // namespace ql_atmoforge

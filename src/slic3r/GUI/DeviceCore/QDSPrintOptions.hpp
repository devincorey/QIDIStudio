#ifndef slic3r_GUI_DeviceCore_QDSPrintOptions_hpp_
#define slic3r_GUI_DeviceCore_QDSPrintOptions_hpp_

#include <string>
#include <string_view>

namespace Slic3r::GUI::QDSPrintOptions {

inline constexpr int full_bed_leveling_estimate_seconds = 10 * 60;

struct BedLevelingPreparation
{
    std::string gcode_preamble;
    int         estimated_overhead_seconds{0};
};

// QIDI Plus 4 firmware evaluates the G29 state from PRINT_START.  Place the
// selected state in the uploaded job as well as sending it over the control
// channel so an asynchronous upload/start cannot overtake the control command.
inline BedLevelingPreparation prepare_bed_leveling(std::string_view selection)
{
    if (selection == "on")
        return {"G31\n", full_bed_leveling_estimate_seconds};
    if (selection == "off")
        return {"G32\n", 0};
    return {};
}

} // namespace Slic3r::GUI::QDSPrintOptions

#endif

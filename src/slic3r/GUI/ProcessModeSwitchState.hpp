#ifndef slic3r_GUI_ProcessModeSwitchState_hpp_
#define slic3r_GUI_ProcessModeSwitchState_hpp_

namespace Slic3r::GUI {

struct ProcessModeSwitchState
{
    bool value;
    bool enabled;
};

constexpr ProcessModeSwitchState process_mode_switch_state(bool advanced_settings_visible,
                                                           bool develop_mode) noexcept
{
    // Developer mode is a superset of Advanced. Keep the redundant switch
    // disabled, but render it as active instead of leaving its default value
    // dependent on the order in which the modes were selected.
    return {advanced_settings_visible, !develop_mode};
}

} // namespace Slic3r::GUI

#endif // slic3r_GUI_ProcessModeSwitchState_hpp_

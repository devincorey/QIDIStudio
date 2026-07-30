#include <iostream>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/popupwin.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <wx/weakref.h>

#include "slic3r/GUI/DeviceCore/QDSModalClose.hpp"

namespace {

class LifecycleTestApp : public wxApp
{
public:
    bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(LifecycleTestApp);

bool run_cycles(bool use_window_close, int requested_result = wxID_CANCEL)
{
    auto *dialog = new wxDialog(nullptr, wxID_ANY, "QDS modal lifecycle");
    auto *popup  = new wxPopupTransientWindow(dialog, wxBORDER_NONE);
    auto *timer  = new wxTimer(dialog);
    auto *primary_action = new wxButton(dialog, wxID_OK, "OK");
    auto *cancel_action  = new wxButton(dialog, wxID_CANCEL, "Cancel");
    popup->SetSize(wxSize(20, 20));

    int timer_events = 0;
    dialog->Bind(wxEVT_TIMER, [&timer_events](wxTimerEvent &) { ++timer_events; }, timer->GetId());
    timer->Start(5);

    Slic3r::GUI::QDSModalCloseCoordinator close;
    int accepted_requests = 0;
    dialog->Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent &event) {
        if (event.CanVeto())
            event.Veto();
        accepted_requests += close.request(*dialog, timer, popup, primary_action, cancel_action, wxID_CANCEL) ? 1 : 0;
        accepted_requests += close.request(*dialog, timer, popup, primary_action, cancel_action, wxID_OK) ? 1 : 0;
    });

    bool passed = true;
    for (int cycle = 0; cycle < 2; ++cycle) {
        close.reset(primary_action, cancel_action);
        timer->Start(5);
        passed = passed && primary_action->IsEnabled() && cancel_action->IsEnabled();
        wxTheApp->CallAfter([&]() {
            popup->Popup();
            if (use_window_close) {
                dialog->Close();
            } else {
                accepted_requests += close.request(*dialog, timer, popup, primary_action, cancel_action, requested_result) ? 1 : 0;
                accepted_requests += close.request(*dialog, timer, popup, primary_action, cancel_action, wxID_CANCEL) ? 1 : 0;
            }
        });

        const int modal_result = dialog->ShowModal();
        const int expected_result = use_window_close ? wxID_CANCEL : requested_result;
        passed = passed && modal_result == expected_result && accepted_requests == cycle + 1 && close.closing() &&
                 !primary_action->IsEnabled() && !cancel_action->IsEnabled() &&
                 !timer->IsRunning() && !popup->IsShown();
    }
    const int events_after_close = timer_events;

    popup->Destroy();
    delete timer;
    dialog->Destroy();
    wxTheApp->ProcessPendingEvents();

    return passed && timer_events == events_after_close;
}

bool run_parent_hide_cycle()
{
    auto *dialog = new wxDialog(nullptr, wxID_ANY, "QDS transient cleanup");
    auto *popup  = new wxPopupTransientWindow(dialog, wxBORDER_NONE);
    popup->SetSize(wxSize(480, 165));

    bool passed = true;
    for (int cycle = 0; cycle < 2; ++cycle) {
        const bool dialog_shown = dialog->Show();
        popup->Popup();
        passed = passed && dialog_shown && popup->IsShown();
        const bool parent_hidden = Slic3r::GUI::hide_window_after_dismissing_transients(
            {popup}, [dialog]() { return dialog->Show(false); });
        wxTheApp->ProcessPendingEvents();
        passed = passed && parent_hidden && !dialog->IsShown() && !popup->IsShown();
        passed = passed && !Slic3r::GUI::dismiss_transient_popup(popup);
    }

    popup->Destroy();
    dialog->Destroy();
    wxTheApp->ProcessPendingEvents();
    return passed;
}

bool run_heap_popup_destroy_cycles()
{
    auto *dialog = new wxDialog(nullptr, wxID_ANY, "QDS heap popup cleanup");
    bool passed = true;

    for (int cycle = 0; cycle < 2; ++cycle) {
        auto *popup = new wxPopupTransientWindow(dialog, wxBORDER_NONE);
        wxWeakRef<wxWindow> weak_popup(popup);
        bool destroyed = false;
        popup->Bind(wxEVT_DESTROY, [&destroyed](wxWindowDestroyEvent &event) {
            destroyed = true;
            event.Skip();
        });
        popup->SetSize(wxSize(480, 165));
        dialog->Show();
        popup->Popup();

        passed = passed && popup->IsShown() &&
                 Slic3r::GUI::destroy_transient_popup(popup) && popup == nullptr;
        wxTheApp->ProcessPendingEvents();
        wxTheApp->ProcessIdle();
        wxTheApp->ProcessPendingEvents();
        passed = passed && destroyed && !weak_popup;
    }

    dialog->Destroy();
    wxTheApp->ProcessPendingEvents();
    return passed;
}

bool run_nested_tooltip_destroy_cycle()
{
    auto *dialog = new wxDialog(nullptr, wxID_ANY, "QDS nested tooltip cleanup");
    auto *owner = new wxPopupTransientWindow(dialog, wxBORDER_NONE);
    // AmsMapingPopup has regular controls in addition to its native tooltip,
    // so wxPopupTransientWindow captures itself rather than the tooltip.
    auto *owner_content = new wxButton(owner, wxID_ANY, "Mapping");
    auto *tooltip = new wxPopupWindow(owner, wxBORDER_NONE);
    wxWeakRef<wxWindow> weak_owner(owner);
    wxWeakRef<wxWindow> weak_tooltip(tooltip);
    (void) owner_content;

    dialog->Show();
    owner->Popup();
    tooltip->Show();
    const bool shown = owner->IsShown() && tooltip->IsShown();

    // Match AmsMapingPopup::Dismiss(): release transient capture before
    // destroying and detaching the nested native top-level tooltip.
    owner->Dismiss();
    const bool cleanup_requested = Slic3r::GUI::destroy_transient_popup(tooltip);
    bool late_event_ran = false;
    bool tooltip_recreated = false;
    wxTheApp->CallAfter([&]() {
        late_event_ran = true;
        if (Slic3r::GUI::can_show_transient_child(owner)) {
            tooltip = new wxPopupWindow(owner, wxBORDER_NONE);
            tooltip->Show();
            tooltip_recreated = true;
        }
    });
    wxTheApp->ProcessPendingEvents();
    delete owner;
    owner = nullptr;
    dialog->Destroy();
    wxTheApp->ProcessPendingEvents();
    wxTheApp->ProcessIdle();
    wxTheApp->ProcessPendingEvents();

    return shown && cleanup_requested && late_event_ran && !tooltip_recreated &&
           tooltip == nullptr && !weak_tooltip && !weak_owner;
}

} // namespace

int main(int argc, char **argv)
{
    if (!wxEntryStart(argc, argv)) {
        std::cerr << "wxEntryStart failed\n";
        return 1;
    }
    (void) wxGetApp();
    if (!wxTheApp || !wxTheApp->CallOnInit()) {
        std::cerr << "wxApp initialization failed\n";
        wxEntryCleanup();
        return 1;
    }

    const bool cancel_passed = run_cycles(false);
    const bool close_passed  = run_cycles(true);
    const bool success_passed = run_cycles(false, wxID_YES);
    const bool parent_hide_passed = run_parent_hide_cycle();
    const bool heap_popup_destroy_passed = run_heap_popup_destroy_cycles();
    const bool nested_tooltip_destroy_passed = run_nested_tooltip_destroy_cycle();

    wxTheApp->OnExit();
    wxEntryCleanup();

    if (!cancel_passed || !close_passed || !success_passed || !parent_hide_passed ||
        !heap_popup_destroy_passed || !nested_tooltip_destroy_passed) {
        std::cerr << "QDS dialog lifecycle cleanup failed\n";
        return 1;
    }
    std::cout << "QDS dialog lifecycle cleanup passed for cancel, window-close, success, parent-hide, and nested-tooltip paths\n";
    return 0;
}

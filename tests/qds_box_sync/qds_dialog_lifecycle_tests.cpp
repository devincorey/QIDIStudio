#include <iostream>

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/popupwin.h>
#include <wx/timer.h>

#include "slic3r/GUI/DeviceCore/QDSModalClose.hpp"

namespace {

class LifecycleTestApp : public wxApp
{
public:
    bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(LifecycleTestApp);

bool run_cycle(bool use_window_close)
{
    auto *dialog = new wxDialog(nullptr, wxID_ANY, "QDS modal lifecycle");
    auto *popup  = new wxPopupTransientWindow(dialog, wxBORDER_NONE);
    auto *timer  = new wxTimer(dialog);
    popup->SetSize(wxSize(20, 20));

    int timer_events = 0;
    dialog->Bind(wxEVT_TIMER, [&timer_events](wxTimerEvent &) { ++timer_events; }, timer->GetId());
    timer->Start(5);

    Slic3r::GUI::QDSModalCloseCoordinator close;
    int accepted_requests = 0;
    dialog->Bind(wxEVT_CLOSE_WINDOW, [&](wxCloseEvent &event) {
        if (event.CanVeto())
            event.Veto();
        accepted_requests += close.request(*dialog, timer, popup, nullptr, nullptr, wxID_CANCEL) ? 1 : 0;
        accepted_requests += close.request(*dialog, timer, popup, nullptr, nullptr, wxID_OK) ? 1 : 0;
    });

    wxTheApp->CallAfter([&]() {
        popup->Popup();
        if (use_window_close) {
            dialog->Close();
        } else {
            accepted_requests += close.request(*dialog, timer, popup, nullptr, nullptr, wxID_CANCEL) ? 1 : 0;
            accepted_requests += close.request(*dialog, timer, popup, nullptr, nullptr, wxID_OK) ? 1 : 0;
        }
    });

    const int modal_result = dialog->ShowModal();
    const int events_after_close = timer_events;
    const bool passed = modal_result == wxID_CANCEL && accepted_requests == 1 && close.closing() &&
                        !timer->IsRunning() && !popup->IsShown();

    popup->Destroy();
    delete timer;
    dialog->Destroy();
    wxTheApp->ProcessPendingEvents();

    return passed && timer_events == events_after_close;
}

} // namespace

int main(int argc, char **argv)
{
    if (!wxEntryStart(argc, argv)) {
        std::cerr << "wxEntryStart failed\n";
        return 1;
    }
    if (!wxTheApp || !wxTheApp->CallOnInit()) {
        std::cerr << "wxApp initialization failed\n";
        wxEntryCleanup();
        return 1;
    }

    const bool cancel_passed = run_cycle(false);
    const bool close_passed  = run_cycle(true);

    wxTheApp->OnExit();
    wxEntryCleanup();

    if (!cancel_passed || !close_passed) {
        std::cerr << "QDS modal lifecycle cleanup failed\n";
        return 1;
    }
    std::cout << "QDS modal lifecycle cleanup passed twice\n";
    return 0;
}

#ifndef slic3r_GUI_DeviceCore_QDSModalClose_hpp_
#define slic3r_GUI_DeviceCore_QDSModalClose_hpp_

#include <wx/dialog.h>
#include <wx/popupwin.h>
#include <wx/timer.h>
#include <wx/weakref.h>

namespace Slic3r { namespace GUI {

// Coordinates every modal-exit path so a queued close cannot race another
// button/window event or a late timer callback.
class QDSModalCloseCoordinator
{
public:
    void reset(wxWindow *primary_action = nullptr, wxWindow *cancel_action = nullptr)
    {
        m_closing = false;
        if (primary_action)
            primary_action->Enable();
        if (cancel_action)
            cancel_action->Enable();
    }
    bool closing() const { return m_closing; }

    bool request(wxDialog &dialog,
                 wxTimer *timer,
                 wxPopupTransientWindow *popup,
                 wxWindow *primary_action,
                 wxWindow *cancel_action,
                 int result)
    {
        if (m_closing)
            return false;

        m_closing = true;
        if (popup && popup->IsShown())
            popup->Dismiss();
        if (timer)
            timer->Stop();
        if (primary_action)
            primary_action->Disable();
        if (cancel_action)
            cancel_action->Disable();

        wxWeakRef<wxDialog> weak_dialog(&dialog);
        dialog.CallAfter([weak_dialog, result]() mutable {
            if (!weak_dialog)
                return;
            if (weak_dialog->IsModal())
                weak_dialog->EndModal(result);
            else
                weak_dialog->Show(false);
        });
        return true;
    }

private:
    bool m_closing{false};
};

}} // namespace Slic3r::GUI

#endif

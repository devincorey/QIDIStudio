#ifndef slic3r_GUI_TransientWindowCleanup_hpp_
#define slic3r_GUI_TransientWindowCleanup_hpp_

#include <initializer_list>
#include <type_traits>
#include <utility>

#include <wx/popupwin.h>

namespace Slic3r { namespace GUI {

inline bool can_show_transient_child(const wxWindow *owner)
{
    return owner && owner->IsShown() && !owner->IsBeingDeleted();
}

inline bool dismiss_transient_popup(wxPopupTransientWindow *popup)
{
    if (!popup || !popup->IsShown())
        return false;

    popup->Dismiss();
    return true;
}

inline void dismiss_transient_windows(std::initializer_list<wxPopupTransientWindow *> popups)
{
    for (wxPopupTransientWindow *popup : popups)
        dismiss_transient_popup(popup);
}

// Heap-allocated popup windows are top-level windows on macOS and are not
// destroyed with their nominal parent. Clear the owner's pointer first so a
// hide/destroy callback cannot schedule a second deletion.
template <typename Popup>
bool destroy_transient_popup(Popup *&popup)
{
    if (!popup)
        return false;
    Popup *doomed = popup;
    popup = nullptr;
    if (doomed->IsShown()) {
        if constexpr (std::is_base_of_v<wxPopupTransientWindow, Popup>)
            doomed->Dismiss();
        else
            doomed->Hide();
    }
#ifdef __APPLE__
    // Cocoa popup windows participate in their nominal parent's wx child
    // list even though they are native top-level windows. A transient popup's
    // Destroy() is deferred, so leaving it attached lets an immediately
    // destroyed parent delete it first and leaves a stale wxPendingDelete
    // entry behind.
    if (wxWindow *parent = doomed->GetParent())
        parent->RemoveChild(doomed);
#endif
    doomed->Destroy();
    return true;
}

template <typename HideWindow>
bool hide_window_after_dismissing_transients(
    std::initializer_list<wxPopupTransientWindow *> popups,
    HideWindow &&hide_window)
{
    dismiss_transient_windows(popups);
    return std::forward<HideWindow>(hide_window)();
}

}} // namespace Slic3r::GUI

#endif

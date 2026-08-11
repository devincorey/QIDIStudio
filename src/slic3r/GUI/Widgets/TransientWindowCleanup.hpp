#ifndef slic3r_GUI_TransientWindowCleanup_hpp_
#define slic3r_GUI_TransientWindowCleanup_hpp_

#include <initializer_list>
#include <type_traits>
#include <utility>

#include <wx/popupwin.h>
#include <wx/weakref.h>

namespace Slic3r { namespace GUI {

inline bool can_show_transient_child(const wxWindow *owner)
{
    return owner && owner->IsShown() && !owner->IsBeingDeleted();
}

inline bool ensure_transient_popup_hidden(wxPopupTransientWindow *popup)
{
    if (!popup)
        return false;

    if (popup->IsShown())
        popup->Show(false);
    return !popup->IsShown();
}

inline bool dismiss_transient_popup(wxPopupTransientWindow *popup)
{
    if (!popup || !popup->IsShown())
        return false;

    popup->Dismiss();
    ensure_transient_popup_hidden(popup);
    return true;
}

// On macOS, dismissing a native transient window from the mouse-down callback
// that selected an item can leave its Cocoa surface composited after wxWidgets
// considers the popup hidden. When the selection was posted to the owner, let
// that queued handler dismiss the popup on the next event-loop turn. Other
// platforms retain their existing immediate-dismiss behaviour, and a failed
// post still closes the popup everywhere.
inline bool dismiss_transient_after_selection(wxPopupTransientWindow *popup,
                                              bool selection_event_posted)
{
#ifdef __APPLE__
    if (selection_event_posted)
        return false;
#else
    (void) selection_event_posted;
#endif
    return dismiss_transient_popup(popup);
}

inline void dismiss_transient_windows(std::initializer_list<wxPopupTransientWindow *> popups)
{
    for (wxPopupTransientWindow *popup : popups)
        dismiss_transient_popup(popup);
}

inline bool refresh_transient_owner(wxWeakRef<wxWindow> owner)
{
    wxWindow *window = owner.get();
    if (!window || window->IsBeingDeleted())
        return false;

    window->Refresh(true);
    window->Update();
    return true;
}

// Transient popups often highlight the control that opened them. Store that
// control weakly and clear it before the popup disappears so a destroyed owner
// cannot be dereferenced and a stale selection outline cannot survive dismissal.
template <typename SelectionWindow, typename ClearSelection>
bool clear_transient_selection(wxWeakRef<SelectionWindow> &selection, ClearSelection &&clear_selection)
{
    SelectionWindow *selected = selection.get();
    selection = static_cast<SelectionWindow *>(nullptr);
    if (!selected || selected->IsBeingDeleted())
        return false;

    std::forward<ClearSelection>(clear_selection)(*selected);
    selected->Refresh(false);
    selected->Update();
    return true;
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

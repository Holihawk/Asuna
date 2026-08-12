#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "pet/outfits.hpp"

namespace asuna {

// The right-click menu.
//
// A GMenu model driving a GtkPopoverMenu, rather than hand-built widgets: the
// model gives submenus, radio state and keyboard navigation for free, and it is
// the same shape the Phase 5 CLI will drive, so every entry here already has a
// name that can be sent over the socket.
//
// The menu is rebuilt on an outfit change, because the expression and motion
// lists are the model's own inventory, not a fixed list.
class Menu {
public:
    struct Actions {
        std::function<void(const std::string& id)> outfit;
        std::function<void(const std::string& name)> expression;
        std::function<void(const std::string& name)> motion;
        std::function<void()> resetPosition;
        std::function<void()> resetSize;
        // Only ever offered while an extension helper is subscribed - there is
        // nothing to talk to otherwise. See Menu::rebuild's `withChat`.
        std::function<void()> chat;
        // Deferred out of Phase 4 until `asuna show` existed to undo it: a Hide
        // in a menu that disappears with her is a one-way door.
        std::function<void()> hide;
        std::function<void()> quit;
        // Called with true when the menu opens and false when it closes, so the
        // shell can widen the input region for as long as it is up.
        std::function<void(bool open)> visibility;
    };

    ~Menu();

    // `parent` is the widget the popover hangs off - the GL area. Call once.
    void attach(GtkWidget* parent, Actions actions);

    // Fills in the outfit list and this model's expressions and motions. Safe to
    // call again after a costume change.
    //
    // `withChat` puts Chat at the top. It is a property of the moment rather
    // than of the model - whether a helper is listening right now - so the shell
    // marks the menu stale when that changes, the same way an outfit switch
    // does.
    void rebuild(const std::vector<Outfit>& outfits, const std::string& currentId,
                 const std::vector<std::string>& expressions,
                 const std::vector<std::string>& motions, bool withChat);

    // Opens beside `body` - the rectangle she occupies, in the parent's
    // coordinates (logical px). The menu goes to her right, vertically centred
    // on her, and flips to her left when there is no room; the compositor makes
    // that decision, because only it knows where the screen ends.
    //
    // Not at the pointer: a menu under the cursor covers exactly the part of her
    // that was just clicked.
    void popupBeside(const GdkRectangle& body);

    void close();
    bool isOpen() const;

    // Activates an entry by name, exactly as clicking it does - the action
    // group is the same one the popover drives. For ASUNA_DEBUG_ACTION, which
    // exists because a menu item is the one part of the UI that cannot be
    // reached without a pointer this machine has no way to synthesise. False if
    // there is no such action.
    bool activate(const std::string& name, const std::string& target = "");

private:
    static void onSimple(GSimpleAction* action, GVariant* param, gpointer self);
    static void onClosed(GtkPopover* popover, gpointer self);
    static void onMenuMotion(GtkEventControllerMotion* c, double x, double y,
                             gpointer self);
    static gboolean onIdleTimeout(gpointer self);
    static gboolean onDeferredPopup(gpointer self);
    // ASUNA_DEBUG_MENU only: where the popup surface actually ended up, which is
    // a different question from where we asked it to go.
    static gboolean onReportPlacement(gpointer self);
    // Restarts the inactivity countdown. Called on every pointer motion over
    // the menu, so the timeout only ever fires on a menu nobody is using.
    void touchIdleTimer();
    void cancelIdleTimer();

    GtkWidget* mParent = nullptr;
    GtkWidget* mPopover = nullptr;
    GSimpleActionGroup* mActions = nullptr;
    Actions mHandlers;
    guint mIdleTimer = 0;
    guint mPopupIdle = 0;
    bool mFresh = false;   // model just replaced; let GTK measure it first
};

}  // namespace asuna

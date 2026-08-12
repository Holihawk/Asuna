#include "ui/menu.hpp"

#include <cstdio>
#include <cstdlib>

namespace asuna {
namespace {

// Outfits per nested submenu. 42 entries in one list is taller than the screen
// and GTK would have to scroll it; a handful at a time stays a glance rather
// than a scroll.
//
// Seven rather than a rounder ten because 42 divides by it. Ten left a final
// submenu holding two, which reads as a leftover rather than a group - and
// seven falls out of the collection twice over: the ids sort with all 28 bust
// outfits ahead of all 14 full-body ones, so four full submenus land exactly on
// the boundary and the last two are exactly the full-body set.
//
// That second alignment is a property of these 42, not something enforced here.
// A collection that does not divide evenly simply gets a short last submenu
// again, which is the behaviour this replaced and is fine.
constexpr size_t kOutfitChunk = 7;

// Gap between her outline and the menu, logical px. Small on purpose: the
// rectangle this is measured from is her silhouette, so the gap is the whole
// distance rather than being added to a bleed margin.
constexpr int kGap = 4;

// How long an untouched menu stays up.
//
// It closes on a click elsewhere anyway, and any pointer motion over it restarts
// the clock, so this only ever fires on a menu that was opened and abandoned -
// which matters because an open menu widens the input region to the whole strip
// (see Shell's visibility handler), and a forgotten one would leave a band of
// the desktop swallowing clicks. Long enough to read 42 outfit ids without the
// menu vanishing mid-thought; short enough that walking away tidies up.
constexpr guint kIdleCloseMs = 15000;

void appendAction(GMenu* menu, const std::string& label, const std::string& action,
                  const std::string& target) {
    const std::string detailed = target.empty() ? action : action + "::" + target;
    g_menu_append(menu, label.c_str(), detailed.c_str());
}

}  // namespace

Menu::~Menu() {
    cancelIdleTimer();
    if (mPopupIdle) g_source_remove(mPopupIdle);
    // mPopover is nulled by the weak pointer if GTK tore it down with its parent
    // first - which is exactly what happens when the compositor closes the
    // window rather than the user choosing Quit.
    if (mPopover) gtk_widget_unparent(mPopover);
    if (mActions) g_object_unref(mActions);
}

void Menu::attach(GtkWidget* parent, Actions actions) {
    mParent = parent;
    mHandlers = std::move(actions);

    static const GActionEntry entries[] = {
        {"outfit", onSimple, "s", nullptr, nullptr, {0, 0, 0}},
        {"expression", onSimple, "s", nullptr, nullptr, {0, 0, 0}},
        {"motion", onSimple, "s", nullptr, nullptr, {0, 0, 0}},
        {"chat", onSimple, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"reset-position", onSimple, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"reset-size", onSimple, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"hide", onSimple, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"quit", onSimple, nullptr, nullptr, nullptr, {0, 0, 0}},
    };
    mActions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(mActions), entries,
                                    G_N_ELEMENTS(entries), this);
    gtk_widget_insert_action_group(parent, "asuna", G_ACTION_GROUP(mActions));

    mPopover = gtk_popover_menu_new_from_model(nullptr);
    gtk_widget_set_parent(mPopover, parent);
    g_object_add_weak_pointer(G_OBJECT(mPopover), reinterpret_cast<gpointer*>(&mPopover));
    gtk_widget_add_css_class(mPopover, "asuna-menu");
    gtk_popover_set_has_arrow(GTK_POPOVER(mPopover), FALSE);
    // Beside her rather than over her. GTK turns this into a GdkPopupLayout
    // with FLIP and SLIDE hints, so the compositor moves it to her other side
    // when she is too close to the screen edge for it to fit - which is exactly
    // the behaviour wanted, and it cannot be done correctly on this end: a
    // layer-shell client is not told where its own surface ended up.
    gtk_popover_set_position(GTK_POPOVER(mPopover), GTK_POS_RIGHT);
    gtk_popover_set_offset(GTK_POPOVER(mPopover), kGap, 0);
    // A layer surface with keyboard mode NONE never gets keyboard focus, so the
    // menu is pointer-driven. Letting it try to take a keyboard grab it cannot
    // have would leave it unable to close itself.
    gtk_popover_set_autohide(GTK_POPOVER(mPopover), TRUE);
    g_signal_connect(mPopover, "closed", G_CALLBACK(onClosed), this);

    // Anything the pointer does over the menu counts as still using it.
    GtkEventController* motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(onMenuMotion), this);
    gtk_widget_add_controller(mPopover, motion);
}

void Menu::rebuild(const std::vector<Outfit>& outfits, const std::string& currentId,
                   const std::vector<std::string>& expressions,
                   const std::vector<std::string>& motions, bool withChat) {
    if (!mPopover) return;
    GMenu* root = g_menu_new();

    if (withChat) {
        // Its own section at the top, above the outfit list: it is the only
        // entry here that starts a conversation rather than changing how she
        // looks, and it is the one being reached for when it is there at all.
        GMenu* head = g_menu_new();
        appendAction(head, "Chat\xe2\x80\xa6", "asuna.chat", "");
        g_menu_append_section(root, nullptr, G_MENU_MODEL(head));
        g_object_unref(head);
    }

    if (!outfits.empty()) {
        GMenu* outfitMenu = g_menu_new();
        const bool chunked = outfits.size() > kOutfitChunk + 2;
        GMenu* chunk = chunked ? g_menu_new() : outfitMenu;
        size_t chunkStart = 0;

        for (size_t i = 0; i < outfits.size(); ++i) {
            // A tick on the one she is wearing: with 42 near-identical ids it is
            // otherwise impossible to tell where you are.
            const std::string label =
                (outfits[i].id == currentId ? "\xe2\x97\x8f  " : "\xe2\x80\x83  ") + outfits[i].id;
            appendAction(chunk, label, "asuna.outfit", outfits[i].id);

            const bool last = i + 1 == outfits.size();
            if (chunked && ((i + 1) % kOutfitChunk == 0 || last)) {
                const std::string range =
                    outfits[chunkStart].id + " \xe2\x80\x93 " + outfits[i].id;
                g_menu_append_submenu(outfitMenu, range.c_str(), G_MENU_MODEL(chunk));
                g_object_unref(chunk);
                chunkStart = i + 1;
                if (!last) chunk = g_menu_new();
            }
        }
        g_menu_append_submenu(root, "Outfit", G_MENU_MODEL(outfitMenu));
        g_object_unref(outfitMenu);
    }

    if (!expressions.empty()) {
        GMenu* menu = g_menu_new();
        for (const auto& name : expressions) appendAction(menu, name, "asuna.expression", name);
        g_menu_append_submenu(root, "Expression", G_MENU_MODEL(menu));
        g_object_unref(menu);
    }

    if (!motions.empty()) {
        GMenu* menu = g_menu_new();
        for (const auto& name : motions) appendAction(menu, name, "asuna.motion", name);
        g_menu_append_submenu(root, "Motion", G_MENU_MODEL(menu));
        g_object_unref(menu);
    }

    GMenu* section = g_menu_new();
    appendAction(section, "Reset position", "asuna.reset-position", "");
    appendAction(section, "Reset size", "asuna.reset-size", "");
    g_menu_append_section(root, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);

    GMenu* tail = g_menu_new();
    // Its own section, next to Quit rather than next to the reset items: these
    // two are the ways she goes away, and the difference between them is worth
    // a separator. The label carries the way back because the menu is the only
    // place it can be said - once she is gone there is nothing left to read.
    appendAction(tail, "Hide  (asuna show)", "asuna.hide", "");
    appendAction(tail, "Quit", "asuna.quit", "");
    g_menu_append_section(root, nullptr, G_MENU_MODEL(tail));
    g_object_unref(tail);

    gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(mPopover), G_MENU_MODEL(root));
    mFresh = true;
    g_object_unref(root);
}

void Menu::popupBeside(const GdkRectangle& body) {
    if (!mPopover) return;
    static const bool debug = getenv("ASUNA_DEBUG_MENU") != nullptr;
    if (debug)
        printf("asuna: menu pointing to (%d,%d) %dx%d\n", body.x, body.y, body.width,
               body.height);
    gtk_popover_set_pointing_to(GTK_POPOVER(mPopover), &body);
    if (mHandlers.visibility) mHandlers.visibility(true);
    // Popping up in the same main-loop turn as set_menu_model() sizes the popup
    // from a menu GTK has not finished laying out, and it comes up exactly one
    // item short - the last entry is clipped off the bottom, which is why Quit
    // was never visible. One idle turn is enough for the measurement to settle,
    // and it is imperceptible next to a right-click.
    if (mFresh) {
        mFresh = false;
        mPopupIdle = g_idle_add(onDeferredPopup, this);
    } else {
        gtk_popover_popup(GTK_POPOVER(mPopover));
    }
    if (debug) g_timeout_add(120, onReportPlacement, this);
    touchIdleTimer();
}

// Where the popup surface actually is, relative to ours. The rectangle we hand
// to set_pointing_to is a request; this is the answer, and the two being
// different is the whole difference between "we anchored to the wrong place"
// and "we anchored to the right place and GTK did not move the popup".
gboolean Menu::onReportPlacement(gpointer data) {
    auto* self = static_cast<Menu*>(data);
    if (!self->mPopover) return G_SOURCE_REMOVE;
    GtkNative* native = gtk_widget_get_native(self->mPopover);
    GdkSurface* surface = native ? gtk_native_get_surface(native) : nullptr;
    if (!surface || !GDK_IS_POPUP(surface)) {
        printf("asuna: menu placement - no popup surface\n");
        return G_SOURCE_REMOVE;
    }
    printf("asuna: menu placed at (%d,%d) %dx%d visible=%d\n",
           gdk_popup_get_position_x(GDK_POPUP(surface)),
           gdk_popup_get_position_y(GDK_POPUP(surface)),
           gdk_surface_get_width(surface), gdk_surface_get_height(surface),
           gtk_widget_get_visible(self->mPopover));
    return G_SOURCE_REMOVE;
}

void Menu::close() {
    // A popup still waiting on its idle turn has to be called off, or it opens
    // a menu that was already dismissed.
    if (mPopupIdle) g_source_remove(mPopupIdle);
    mPopupIdle = 0;
    if (mPopover) gtk_popover_popdown(GTK_POPOVER(mPopover));
}

bool Menu::isOpen() const {
    return mPopover && gtk_widget_get_visible(mPopover);
}

bool Menu::activate(const std::string& name, const std::string& target) {
    if (!mActions) return false;
    GActionGroup* group = G_ACTION_GROUP(mActions);
    if (!g_action_group_has_action(group, name.c_str())) return false;
    // The entries that take a target ("outfit", "expression", "motion") and the
    // ones that do not are activated differently, and the action itself is the
    // authority on which it is - asking it keeps this from having a list of
    // names to be kept in step with the one in attach().
    const GVariantType* type = g_action_group_get_action_parameter_type(group, name.c_str());
    if (type && target.empty()) return false;
    g_action_group_activate_action(group, name.c_str(),
                                   type ? g_variant_new_string(target.c_str()) : nullptr);
    return true;
}

void Menu::touchIdleTimer() {
    cancelIdleTimer();
    mIdleTimer = g_timeout_add(kIdleCloseMs, onIdleTimeout, this);
}

void Menu::cancelIdleTimer() {
    if (mIdleTimer) g_source_remove(mIdleTimer);
    mIdleTimer = 0;
}

gboolean Menu::onDeferredPopup(gpointer data) {
    auto* self = static_cast<Menu*>(data);
    self->mPopupIdle = 0;
    if (self->mPopover) gtk_popover_popup(GTK_POPOVER(self->mPopover));
    return G_SOURCE_REMOVE;
}

gboolean Menu::onIdleTimeout(gpointer data) {
    auto* self = static_cast<Menu*>(data);
    self->mIdleTimer = 0;
    self->close();
    return G_SOURCE_REMOVE;
}

void Menu::onMenuMotion(GtkEventControllerMotion*, double, double, gpointer data) {
    static_cast<Menu*>(data)->touchIdleTimer();
}

void Menu::onClosed(GtkPopover*, gpointer data) {
    auto* self = static_cast<Menu*>(data);
    if (getenv("ASUNA_DEBUG_MENU")) printf("asuna: menu closed\n");
    self->cancelIdleTimer();
    if (self->mHandlers.visibility) self->mHandlers.visibility(false);
}

void Menu::onSimple(GSimpleAction* action, GVariant* param, gpointer data) {
    auto* self = static_cast<Menu*>(data);
    const std::string name = g_action_get_name(G_ACTION(action));
    const char* target = param ? g_variant_get_string(param, nullptr) : nullptr;

    if (name == "outfit" && self->mHandlers.outfit && target) self->mHandlers.outfit(target);
    else if (name == "expression" && self->mHandlers.expression && target) self->mHandlers.expression(target);
    else if (name == "motion" && self->mHandlers.motion && target) self->mHandlers.motion(target);
    else if (name == "chat" && self->mHandlers.chat) self->mHandlers.chat();
    else if (name == "reset-position" && self->mHandlers.resetPosition) self->mHandlers.resetPosition();
    else if (name == "reset-size" && self->mHandlers.resetSize) self->mHandlers.resetSize();
    else if (name == "hide" && self->mHandlers.hide) self->mHandlers.hide();
    else if (name == "quit" && self->mHandlers.quit) self->mHandlers.quit();
}

}  // namespace asuna

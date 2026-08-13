"""asuna-prompt - the one line you type at her.

Prints what was typed and exits 0; exits 1 if it was dismissed with Escape or
timed out. That is the whole contract, so `ext.prompt_command` can be anything
else that behaves the same way.

Why this exists rather than `fuzzel --dmenu`, which is what it replaces: fuzzel
has no input-method support at all - it never binds `zwp_text_input_v3`, so
Ctrl+Space cannot reach fcitx5 and there is no way to type Chinese into it. A
GTK window has IM support through the toolkit, which is the whole difference.

It is a layer-shell surface rather than an ordinary window for two reasons: a
tiling compositor would otherwise fit a one-line dialog into the layout and push
everything else around, and a layer surface can ask for keyboard focus on
demand, appear over the top of whatever is there, and be placed exactly above
her head. It looks like her speech bubble, on purpose - it is her side of the
conversation, not a launcher that happens to be open.

Being a layer surface is also why it can be picked up and moved: the label on
the left is a handle, and dragging it carries the prompt. There is no title bar
to grab and no compositor-side move to ask for.

Which is why the surface is the whole screen and the prompt is a child inside
it, moved by `Gtk.Fixed` rather than by the margins the surface is anchored
with. Wayland reports the pointer relative to the surface it is over, so a
surface that moves under the pointer moves the pointer's coordinates with it,
and a drag that reads those coordinates to decide where to move the surface is
a feedback loop around the compositor's own latency - it oscillates, and what
that looks like is a window that will not sit still. A surface that never moves
has no such loop, and placing the prompt inside it is a layout change with no
round trip in it at all. The cost is that a full-screen surface would swallow
every click on the desktop, so the input region is cut down to the prompt's own
rectangle - the same trick the pet uses to be a strip you can click through (see
`Shell::applyRegion`), and the same reason she, too, moves inside something that
does not.

That is only half of it, though, and the half that is easy to see. The other
half is that a GtkGesture reports its points in the coordinates of the widget it
is attached to, and the handle rides along with the prompt - so the offsets it
hands to `drag-update` have every move already taken out of them, and the loop
is back inside GTK with the layout pass for its latency. `on_screen` below is
what closes it off for good.
"""

# PyGObject's gi.repository importer creates these symbols dynamically after
# require_version(); Pyright cannot inspect that runtime namespace.
# pyright: reportAttributeAccessIssue=false

import argparse
import ctypes.util
import json
import os
import sys

# gtk4-layer-shell works by interposing on libwayland-client's registry, so it
# has to be loaded *before* libwayland-client is - which is the same trap the
# C++ side documents as "link order is load-bearing", wearing different clothes.
# A Python process cannot control its link order, because importing gi loads GTK
# and libwayland with it, so the only lever left is LD_PRELOAD; upstream says so
# too. Re-exec once, before anything imports gi, and mark it done so the second
# run does not do it again.
if not os.environ.get("ASUNA_PROMPT_PRELOADED"):
    lib = ctypes.util.find_library("gtk4-layer-shell")
    if lib:
        preload = os.environ.get("LD_PRELOAD", "")
        os.environ["LD_PRELOAD"] = lib + (":" + preload if preload else "")
    os.environ["ASUNA_PROMPT_PRELOADED"] = "1"
    # The GL renderer for the same reason the daemon picks it: this is a small
    # translucent surface over the desktop, and spinning up Vulkan for one line
    # of text costs more than it draws. Not overwritten, so the environment wins.
    os.environ.setdefault("GSK_RENDERER", "gl")
    os.execv(sys.executable, [sys.executable] + sys.argv)

import cairo
import gi

gi.require_version("Gdk", "4.0")
gi.require_version("Graphene", "1.0")
gi.require_version("Gtk", "4.0")
gi.require_version("Gtk4LayerShell", "1.0")
from gi.repository import (Gdk, GLib, Graphene, Gtk,  # noqa: E402
                           Gtk4LayerShell as LayerShell)

from .control import Control, socket_path  # noqa: E402

CSS = b"""
window { background: none; }
#asuna-prompt {
  background-color: rgba(26, 24, 36, 0.82);
  border: 1px solid rgba(255, 255, 255, 0.16);
  border-radius: 16px;
  padding: 6px 10px;
  box-shadow: 0 8px 28px rgba(0, 0, 0, 0.45);
}
/* The handle. It is the one part of the prompt no text ever reaches, which is
   why the drag lives on it and not on the window: the entry beside it can then
   keep the left button entirely for putting the caret somewhere and selecting.
   The rule spells that division out, and the padding makes it a block worth
   aiming at rather than five small letters. */
#asuna-prompt label {
  color: rgba(241, 237, 248, 0.65);
  font-size: 13px;
  padding: 0 9px 0 3px;
  border-right: 1px solid rgba(255, 255, 255, 0.13);
}
#asuna-prompt entry {
  background: none;
  border: none;
  box-shadow: none;
  outline: none;
  color: #f4f1fa;
  font-size: 14px;
  min-width: 320px;
  caret-color: #d9c7ff;
}
#asuna-prompt entry selection { background-color: rgba(190, 160, 255, 0.35); }
"""

# How close to a screen edge it may be put, px. Applies to where it opens and to
# where it can be dragged, so it can always be got hold of again.
EDGE = 8

# Pointer coordinates can move slightly across an otherwise stationary click.
# Only a deliberate move pins the prompt.
DRAG_THRESHOLD = 2


class Prompt:
    def __init__(self, args):
        self.args = args
        self.text = None
        # Where the prompt sits on the screen: px off the left edge and px off
        # the bottom one. The surface covers the whole screen, so these are also
        # where it sits inside the surface, give or take the flip in move_to.
        self.left = 0
        self.bottom = 0
        self.size = (0, 0)      # the prompt's own, measured in place()
        self.screen = (0, 0)
        self.grab = None        # where it and the hand were, when it was grabbed
        self.window = None
        # build() must run before place(); annotations record that invariant for
        # type checkers without weakening these widget types to Optional.
        self.fixed: Gtk.Fixed
        self.frame: Gtk.Box
        self.surface = None
        self.drag_moved = False

    def build(self, app):
        window = Gtk.ApplicationWindow(application=app)
        window.set_decorated(False)
        self.window = window

        css = Gtk.CssProvider()
        css.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )

        LayerShell.init_for_window(window)
        LayerShell.set_namespace(window, "asuna-prompt")
        # Overlay, so it is above her rather than behind her - she lives on the
        # top layer and the prompt sits on her head.
        LayerShell.set_layer(window, LayerShell.Layer.OVERLAY)
        # ON_DEMAND rather than EXCLUSIVE: it takes the keyboard while it is
        # focused and gives it back when it is not, so a prompt left open cannot
        # hold the keyboard hostage the way an exclusive grab would.
        LayerShell.set_keyboard_mode(window, LayerShell.KeyboardMode.ON_DEMAND)
        # Anchored to all four edges, which is how a layer surface says "the
        # whole output" - and -1 exclusive zone so a panel's reserved strip does
        # not shrink it, because the daemon's idea of where she is is in the
        # output's coordinates and the two have to be the same picture.
        #
        # The prompt is then placed *within* it, by move_to(). That is the point
        # of the arrangement, and the reason there are no margins here: see the
        # module docstring.
        for edge in (LayerShell.Edge.TOP, LayerShell.Edge.BOTTOM,
                     LayerShell.Edge.LEFT, LayerShell.Edge.RIGHT):
            LayerShell.set_anchor(window, edge, True)
        LayerShell.set_exclusive_zone(window, -1)
        # A screen-sized surface would take every click on the desktop with it,
        # so the compositor is told which part of it is really there. Needs a
        # GdkSurface, which does not exist until the window is realized.
        window.connect("realize", self.on_realize)

        frame = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        frame.set_name("asuna-prompt")
        label = Gtk.Label(label=self.args.prompt)
        entry = Gtk.Entry()
        entry.set_has_frame(False)
        entry.set_placeholder_text(self.args.placeholder)
        # No spell-check hint: it is one line of Chinese typed at a cartoon, and
        # the hint is a request for a red underline under all of it.
        entry.set_input_hints(Gtk.InputHints.EMOJI)
        entry.connect("activate", self.on_activate, window)
        # GtkEntry's own cut/copy/paste menu, turned off. It is a themed popover
        # arriving inside a surface with no theme of its own, which is why it
        # came up as a bright white slab over her; and a one-line prompt has
        # nothing in it worth a menu. Both routes to it have to go: the action
        # is what the Menu key and Shift+F10 use, the gesture below is what the
        # right button uses.
        text = entry.get_first_child()
        if text is not None:
            text.action_set_enabled("menu.popup", False)

        frame.append(label)
        frame.append(entry)
        # Gtk.Fixed because the prompt's position on the screen is the thing
        # being set, and Fixed is the one layout that takes a position.
        fixed = Gtk.Fixed()
        fixed.put(frame, 0, 0)
        window.set_child(fixed)
        self.fixed, self.frame = fixed, frame

        keys = Gtk.EventControllerKey()
        keys.connect("key-pressed", self.on_key, window)
        window.add_controller(keys)

        # Right-click anywhere on the prompt puts it away, which is the same
        # thing Escape does and the same thing right-clicking her does to her
        # menu. In the capture phase so it is claimed before the entry sees it.
        click = Gtk.GestureClick(button=Gdk.BUTTON_SECONDARY)
        click.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        click.connect("pressed", self.on_secondary, window)
        window.add_controller(click)

        # Drag the label to move the window. On the label, and only the label,
        # because that is the one part of the prompt text never reaches: put
        # anywhere that overlaps the entry and it has to be told apart from
        # putting the caret somewhere and from sweeping a selection, and the
        # only ways to do that are a hold before it starts (a wait, on every
        # move) or a race with GtkText over which gesture claims the press
        # (whose winner is GtkText's business, not ours). A separate widget has
        # neither problem, so the drag can begin on the press.
        drag = Gtk.GestureDrag(button=Gdk.BUTTON_PRIMARY)
        drag.connect("drag-begin", self.on_drag_begin)
        drag.connect("drag-update", self.on_drag_update)
        drag.connect("drag-end", self.on_drag_end)
        label.add_controller(drag)
        reset = Gtk.GestureClick(button=Gdk.BUTTON_PRIMARY)
        # Observe clicks before the target-phase drag gesture arbitrates the
        # same sequence. Motion still cancels GestureClick in the usual way.
        reset.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
        reset.connect("released", self.on_handle_released)
        label.add_controller(reset)
        # So the handle looks like one. GTK4 CSS has no cursor property, so this
        # is the only place it can be said.
        label.set_cursor(Gdk.Cursor.new_from_name("grab", None))
        self.handle = label

        # Measured from the content, not read off the window: the window is the
        # screen now, and even before that it was still 0 wide at present()
        # time, so centring on it put the prompt in the bottom-left corner -
        # which is exactly what it did.
        self.place()
        window.present()
        entry.grab_focus()

        if self.args.timeout > 0:
            GLib.timeout_add(int(self.args.timeout * 1000), self.on_timeout, window)

    def place(self):
        self.screen = self.screen_size()
        _minimum, wide, _mb, _nb = self.frame.measure(Gtk.Orientation.HORIZONTAL, -1)
        _minimum, tall, _mb, _nb = self.frame.measure(Gtk.Orientation.VERTICAL, wide)
        self.size = (wide, tall)
        if self.args.position_left is not None and self.args.position_bottom is not None:
            self.move_to(self.args.position_left, self.args.position_bottom)
            return
        self.place_at({"anchor_x": self.args.x,
                       "anchor_bottom": self.args.bottom,
                       "anchor_place": self.args.place})

    def place_at(self, where):
        """Place against an anchor supplied by the daemon."""
        x = int(where.get("anchor_x", 0))
        bottom = int(where.get("anchor_bottom", 0))
        place = where.get("anchor_place", "above")
        wide, tall = self.size
        # `--x` and `--bottom` are one point on her; `--place` says which part of
        # the prompt to hang on it. The daemon picks between them because it is
        # the one that knows how big she is and how much screen is left; the
        # arithmetic is here because it is the one that knows how big the prompt
        # is, which it only finds out by measuring, above.
        if place == "left":                   # beside her, on her left
            self.move_to(x - wide, bottom - tall // 2)
        elif place == "right":                # ...or on her right
            self.move_to(x, bottom - tall // 2)
        else:                                 # over her head, centred on her
            self.move_to(x - wide // 2, bottom)

    def screen_size(self):
        """How big the surface is about to be, which is the whole output."""
        if self.args.screen_width > 0 and self.args.screen_height > 0:
            return self.args.screen_width, self.args.screen_height
        # Run by hand, or by a daemon too old to say. Ask GDK instead: the
        # compositor puts an unassigned layer surface on the active output, and
        # for one monitor that is the only answer there is.
        monitors = Gdk.Display.get_default().get_monitors()
        if monitors.get_n_items():
            geometry = monitors.get_item(0).get_geometry()
            return geometry.width, geometry.height
        return 0, 0

    def move_to(self, left, bottom):
        """Put it there, or as near as the screen allows.

        Reached both by place() and by every step of a drag, and it is the same
        arithmetic either way, because there is nothing asynchronous in it: the
        surface is already the size of the screen and stays where it is. All
        that happens is that a child moves inside it, which GTK does within the
        frame it is asked in.
        """
        screen_w, screen_h = self.screen
        self.left = self.clamp(left, self.size[0], screen_w)
        self.bottom = self.clamp(bottom, self.size[1], screen_h)
        self.fixed.move(self.frame, self.left, self.top())
        # Not while dragging: the button is held, so the pointer is ours by
        # implicit grab wherever it goes, and the region only has to be true
        # again by the time it is let go of.
        if self.grab is None:
            self.sync_region()

    @staticmethod
    def clamp(value, size, screen):
        # No screen given, so only the near edge can be enforced.
        if screen <= 0:
            return max(EDGE, value)
        return min(max(EDGE, value), max(EDGE, screen - size - EDGE))

    def top(self):
        """The same position, the other way up.

        Gtk.Fixed measures from the top left; everything else here measures off
        the bottom, because that is the edge she stands on and the edge the
        daemon reports her head above.
        """
        screen_h = self.screen[1]
        return screen_h - self.bottom - self.size[1] if screen_h > 0 else EDGE

    def on_realize(self, window):
        self.surface = window.get_surface()
        self.sync_region()

    def sync_region(self):
        """Tell the compositor which part of the screen this surface really is.

        Without this the prompt is a sheet of glass over the desktop: the
        surface is the whole output, so the whole output stops taking clicks -
        including her, on the layer below. With it, everything outside the
        prompt's own rectangle is not there at all.
        """
        if self.surface is None:
            return
        wide, tall = self.size
        self.surface.set_input_region(
            cairo.Region(cairo.RectangleInt(self.left, self.top(), wide, tall)))

    # --- carrying it around --------------------------------------------------

    def on_drag_begin(self, _gesture, x, y):
        # Where it was when it was picked up, and where the hand was. Every step
        # of the drag is measured from here rather than from the last step, so a
        # drag that runs into a screen edge and comes back arrives where the
        # hand is instead of where the clamp left it.
        self.grab = (self.left, self.bottom, self.on_screen(x, y))
        self.drag_moved = False
        self.handle.set_cursor(Gdk.Cursor.new_from_name("grabbing", None))

    def on_screen(self, x, y):
        """A point on the handle, put back where it is on the screen.

        The reason the offsets GtkGestureDrag hands to drag-update cannot be
        used, and this has to be done the long way round. A gesture reports its
        points in the coordinates of the widget the controller is on - and that
        widget is *inside* the thing being dragged, so it moves whenever the
        drag moves it. Every move is therefore already subtracted out of the
        next offset, which is the same feedback loop the surface used to be,
        wearing GTK's clothes instead of the compositor's: acting on it directly
        settles at half the hand's speed and alternates about that from frame to
        frame, so the prompt both trails the pointer and appears twice.

        Reading the raw point and putting it back into the window's coordinates
        undoes the translation the event arrived through - the same allocation
        goes in both directions, so it cancels whether or not the last move has
        been laid out yet. The window is the whole screen and never moves, so
        what comes out is the hand, on the screen, full stop.
        """
        ok, point = self.handle.compute_point(
            self.window, Graphene.Point().init(x, y))
        return (point.x, point.y) if ok else None

    def on_drag_update(self, gesture, _dx, _dy):
        if self.grab is None:
            return
        left, bottom, was = self.grab
        ok, x, y = gesture.get_point()
        now = self.on_screen(x, y) if ok else None
        if was is None or now is None:
            return
        # Window coordinates run down the screen and `bottom` runs up it.
        self.move_to(left + round(now[0] - was[0]), bottom - round(now[1] - was[1]))

    def on_drag_end(self, gesture, dx, dy):
        start = self.grab[:2] if self.grab is not None else (self.left, self.bottom)
        self.on_drag_update(gesture, dx, dy)
        self.drag_moved = max(abs(self.left - start[0]),
                              abs(self.bottom - start[1])) > DRAG_THRESHOLD
        self.grab = None
        self.sync_region()          # ...and it is somewhere else now
        self.handle.set_cursor(Gdk.Cursor.new_from_name("grab", None))
        if self.drag_moved:
            self.report_position()

    def on_handle_released(self, _gesture, n_press, _x, _y):
        if n_press == 2 and not self.drag_moved:
            self.reset_position()

    def reset_position(self):
        """Return to the pet's current anchor and release the process-local pin."""
        try:
            where = Control(socket_path()).call("ext", status=True)
        except (IOError, OSError):
            # The opening anchor is preferable to a reset that appears to do
            # nothing when the daemon disappears during this prompt.
            where = {"anchor_x": self.args.x,
                     "anchor_bottom": self.args.bottom,
                     "anchor_place": self.args.place}
        if "anchor_x" not in where:
            where = {"anchor_x": self.args.x,
                     "anchor_bottom": self.args.bottom,
                     "anchor_place": self.args.place,
                     "screen_width": where.get("screen_width", 0),
                     "screen_height": where.get("screen_height", 0)}
        if where.get("screen_width") and where.get("screen_height"):
            self.screen = (int(where["screen_width"]), int(where["screen_height"]))
        self.place_at(where)
        self.report_reset()

    def report_position(self):
        """Send coordinates to the owning helper without mixing them into stdout."""
        if self.args.position_fd < 0:
            return
        try:
            report = json.dumps(
                {"event": "position", "left": self.left, "bottom": self.bottom}) + "\n"
            os.write(self.args.position_fd, report.encode())
        except OSError:
            # The helper may have been stopped while this window was closing.
            pass

    def report_reset(self):
        if self.args.position_fd < 0:
            return
        try:
            os.write(self.args.position_fd, b'{"event":"reset"}\n')
        except OSError:
            pass

    def on_activate(self, entry, window):
        self.text = entry.get_text().strip()
        window.close()

    def on_key(self, _controller, keyval, _code, _state, window):
        if keyval == Gdk.KEY_Escape:
            self.text = None
            window.close()
            return True
        return False

    def on_secondary(self, gesture, _n, _x, _y, window):
        # Claimed, so the press stops here rather than reaching the entry, which
        # would answer it with the menu this exists to prevent.
        gesture.set_state(Gtk.EventSequenceState.CLAIMED)
        self.text = None
        window.close()

    def on_timeout(self, window):
        window.close()
        return GLib.SOURCE_REMOVE


def main():
    parser = argparse.ArgumentParser(description="Ask for one line, beside her.")
    parser.add_argument("--prompt", default="Asuna")
    parser.add_argument("--placeholder", default="")
    parser.add_argument("--x", type=int, default=0, help="the point on her, logical px")
    parser.add_argument("--bottom", type=int, default=0, help="...and how far up it is")
    parser.add_argument("--place", default="above", choices=("above", "left", "right"),
                        help="which part of the prompt to hang on that point: over"
                             " her head, or beside her on that side of her")
    # The screen it has to stay on, and now also the screen it is drawn inside:
    # everything else here measures off the bottom edge and Gtk.Fixed measures
    # off the top, so the height is what turns one into the other. Both default
    # to 0, meaning "not told", and GDK is asked for the monitor instead.
    parser.add_argument("--screen-width", type=int, default=0)
    parser.add_argument("--screen-height", type=int, default=0)
    parser.add_argument("--position-left", type=int, default=None,
                        help="restore an absolute logical-pixel position")
    parser.add_argument("--position-bottom", type=int, default=None)
    parser.add_argument("--position-fd", type=int, default=-1,
                        help=argparse.SUPPRESS)
    parser.add_argument("--timeout", type=float, default=0, help="0 waits indefinitely")
    args = parser.parse_args()

    prompt = Prompt(args)
    # APPLICATION_ID is deliberately absent: two prompts at once is not a state
    # worth having, but the caller already guarantees that, and a registered
    # application id would make the second one silently activate the first.
    app = Gtk.Application(flags=0)
    app.connect("activate", prompt.build)
    app.run([])

    if prompt.text is None:
        return 1
    print(prompt.text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

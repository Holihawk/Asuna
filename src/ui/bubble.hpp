#pragma once

#include <gtk/gtk.h>

#include <deque>
#include <string>
#include <vector>

namespace asuna {

// How long a line stays up: base + perGlyph x its length, capped at max.
// Counted in codepoints rather than bytes - every line in the dialogue file is
// Chinese, where a byte count would triple the duration.
struct BubbleTiming {
    double base = 2.0;
    double perGlyph = 0.20;
    double max = 9.0;
};

// The speech bubble.
//
// A plain GTK label in an overlay over the GL area, rather than anything drawn
// into the scene: text rendering, wrapping and CJK shaping are all things Pango
// already does properly, and the bubble has no business being in the silhouette
// the input region is sampled from.
//
// It lives in a reserved band at the top of the strip. The band is part of the
// surface at all times rather than something the window grows into, because
// resizing a layer surface costs a compositor round trip and can drop the input
// region on the way through - a price worth paying once at startup and never per
// sentence. She is anchored to the *bottom* of the strip, so the band changes
// nothing about where she stands.
//
// --- one component -------------------------------------------------------
//
// Everything she says is a *message*, whether it came from the dialogue file
// all at once or from an endpoint a few characters at a time. There is no
// separate path for the second kind, and that is the point: the two used to be
// different enough that an ordinary line was silently dropped while a streamed
// one was up, which is not a rule anybody would have chosen.
//
// A message is text plus the times its pieces arrived. It appears by fading in
// per piece, which is what makes an arriving answer read as emerging rather
// than as being typed: a typewriter shows one character at a time at full
// strength, and the eye chases the cursor. Fading each piece up over ~320 ms
// means the sentence assembles itself and stays readable while it does. A line
// that arrives whole is the same thing with one piece, so it fades in as one.
class Bubble {
public:
    // Builds the widget and puts it in `overlay` as an overlay child. Call once.
    void attach(GtkWidget* overlay);

    // Applies to messages started from now on, so a `config reload` does not
    // cut short the sentence that is on screen while you are reading it.
    void setTiming(const BubbleTiming& t) { mTiming = t; }
    // How many wrapped rows a message may grow to before it scrolls.
    void setRows(int rows);
    // How tall the band above her head has to be for `rows` rows to fit, in
    // logical px. Measured from the label - a row is the font's line height plus
    // whatever the CSS padding and border add, and neither is ours to assume.
    //
    // This is what stops the two settings from disagreeing. They used to: rows
    // said four and the band was sized by hand, so asking for more rows grew the
    // text straight through the top of the strip, where the compositor clips it.
    int bandFor(int rows);

    // A complete line. `seconds` <= 0 picks a duration from the length of the
    // text. Empty text is ignored, so callers can pass a dialogue lookup that
    // found nothing without checking first.
    //
    // Queued behind a message that is still arriving rather than dropped: her
    // reaction to being poked mid-answer is worth hearing, just not over the
    // top of the answer.
    void say(const std::string& text, double seconds = 0);

    // A message that arrives in pieces. `begin` starts one (replacing whatever
    // is showing), `append` adds to it, `finish` says it is complete and starts
    // its reading time. Until then it has no countdown at all.
    void begin(const std::string& text);
    void append(const std::string& chunk);
    void finish();

    // The tell that something is coming: three dots breathing, in place of the
    // text, on a message that has not begun. A slow endpoint otherwise reads as
    // a hang, which is the one thing a pet must never look like. The first
    // begin() or append() takes it over; waiting(false) means nothing came.
    void waiting(bool on);
    // True while a message is still arriving - i.e. while something else is
    // writing into her and an ordinary line should wait its turn.
    bool arriving() const { return mArriving; }

    // Where the bubble should sit, in *logical* px: horizontally centred on
    // `centreX`, with its bottom edge just above `aboveY`.
    //
    // `aboveY` is the top of her box, not the bottom of the reserved band. The
    // two are the same number whenever the strip is the size we asked for - but
    // a layer surface does not always resize when asked, and anchoring to the
    // band left the bubble stranded at the top of the screen for as long as the
    // strip stayed too tall. Measuring down from her instead cannot drift.
    void place(int centreX, int aboveY, int stripWidth);

    // Drops everything and fades out whatever is showing.
    void clear();

    bool busy() const { return mState != State::Hidden; }

    // Where the bubble actually is, in the same logical px place() takes.
    // False if nothing is showing. Used to keep her own words out of a
    // screenshot she is about to be shown.
    bool rect(GdkRectangle* out) const;

private:
    enum class State { Hidden, Showing, FadingOut };

    // One piece of the current message: where it ends in mText, and when it
    // arrived. Everything younger than kEmergeS is still fading up.
    struct Piece {
        size_t end = 0;
        gint64 atUs = 0;
    };

    void showNext();
    void reposition();
    // Rebuilds the label from mText and mPieces. Returns whether anything is
    // still fading, which is what decides if the timer keeps running.
    bool render();
    // The tail of `text` that fits in mRows wrapped rows, as a byte offset into
    // it. Pango decides, because the wrap point depends on the font and the
    // glyph widths and this text is mostly Chinese.
    size_t visibleFrom(const std::string& text);
    void ensureTimer();
    // Re-armed by every piece that arrives. Whoever is writing into her is
    // another process that can be killed, and a message left arriving forever
    // would take the bubble with it: no countdown, and every ordinary line
    // queued behind it in silence. This is the one that gives up.
    void armWatchdog();
    double readingTime(const std::string& text) const;

    static gboolean onTick(gpointer self);
    static gboolean onWatchdog(gpointer self);
    // The overlay asking where this child goes. Returning TRUE means the
    // allocation we filled in is used verbatim.
    static gboolean onChildPosition(GtkOverlay* overlay, GtkWidget* child,
                                    GdkRectangle* allocation, gpointer self);

    GtkWidget* mOverlay = nullptr;
    GtkWidget* mLabel = nullptr;
    std::deque<std::pair<std::string, double>> mQueue;
    State mState = State::Hidden;
    double mOpacity = 0;
    double mRemaining = 0;   // s left of the current message, once complete
    guint mTimer = 0;
    guint mWatchdog = 0;
    int mCentreX = 0, mAboveY = 0, mStripWidth = 0;
    BubbleTiming mTiming;

    // The message on screen.
    std::string mText;
    std::vector<Piece> mPieces;
    bool mArriving = false;   // more of it is still expected
    bool mWaiting = false;    // the breathing dots, before any of it has come
    double mWaitPhase = 0;

    int mRows = 4;
    int mChrome = -1;   // padding + border, measured once; see visibleFrom
    GdkRectangle mAlloc = {0, 0, 0, 0};   // where onChildPosition last put it
};

}  // namespace asuna

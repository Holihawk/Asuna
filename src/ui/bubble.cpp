#include "ui/bubble.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace asuna {
namespace {

constexpr double kFadeIn = 0.14;    // s, the bubble itself
constexpr double kFadeOut = 0.30;
constexpr guint kTickMs = 16;

// How long a piece of text takes to come up, and the lowest opacity it starts
// from. Not zero: text that begins at nothing appears to *arrive* rather than
// to already be there, which is the flicker a typewriter has. Starting at a
// fifth means the line's shape is settled from the first frame and only its
// weight changes.
constexpr double kEmergeS = 0.32;
constexpr double kEmergeFloor = 0.20;

// Queue depth for complete lines. A backlog longer than this means something is
// spamming her, and showing a line from two minutes ago is worse than dropping
// it.
constexpr size_t kMaxQueue = 3;

// The ceiling on a *reply* - a message that arrived in pieces - as against
// bubble.max_seconds, which is the ceiling on a line she said by herself.
//
// They are different because the two are different things. An idle remark that
// hangs about is clutter, so it is capped at nine seconds; an answer to a
// question you asked should still be there when you look back at it, so it is
// capped at a minute and gets the full per-character reading time up to that.
constexpr double kReplyMax = 60.0;

// What is left of a message that has just been interrupted by another. Not zero:
// cutting instantly reads as a glitch, whereas a moment of the old line still
// there is a hand-over.
constexpr double kInterruptS = 0.7;

constexpr int kGap = 6;     // px between the bubble and the top of her head
constexpr int kEdge = 4;    // px the bubble keeps from the screen edges

// How long a message may go without another piece before it is treated as
// finished. Generous, because a slow endpoint can take this long to produce a
// first token and cutting it off would be worse than waiting; but finite,
// because the thing writing into her is a separate process that can be killed
// mid-sentence, and a message that never finishes has no countdown and blocks
// every ordinary line behind it.
constexpr guint kArrivalTimeoutMs = 45000;

// The wait: three dots whose brightness travels along the row. A sine rather
// than a set of frames, because the point is that it breathes - a stepped
// animation reads as a progress indicator, and there is no progress to report.
constexpr double kWaitPeriod = 1.6;   // s for one pass along the row
constexpr int kWaitDots = 3;

size_t glyphCount(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;   // count non-continuation bytes
    return n;
}

// Pango markup is XML, so text going into it has to be escaped - and this text
// comes from an endpoint, so "&" and "<" are not hypothetical.
void appendEscaped(std::string* out, const char* begin, const char* end) {
    for (const char* p = begin; p < end; ++p) {
        switch (*p) {
            case '&': *out += "&amp;"; break;
            case '<': *out += "&lt;"; break;
            case '>': *out += "&gt;"; break;
            default: *out += *p;
        }
    }
}

}  // namespace

void Bubble::attach(GtkWidget* overlay) {
    mOverlay = overlay;
    mLabel = gtk_label_new("");
    gtk_widget_add_css_class(mLabel, "asuna-bubble");
    gtk_label_set_wrap(GTK_LABEL(mLabel), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(mLabel), PANGO_WRAP_WORD_CHAR);
    // In *approximate* characters, which for this font is about 9 px, so this is
    // a bubble around 250 px wide - roughly sixteen Chinese glyphs to the row.
    // Twenty gave ten to the row, which is a column rather than a paragraph:
    // narrow enough that a four-character phrase wrapped in the middle.
    gtk_label_set_max_width_chars(GTK_LABEL(mLabel), 28);
    gtk_label_set_justify(GTK_LABEL(mLabel), GTK_JUSTIFY_LEFT);
    // Never hidden, only made transparent: a widget that comes and goes makes
    // GTK re-lay-out the whole strip every time she opens her mouth.
    gtk_widget_set_opacity(mLabel, 0.0);
    gtk_widget_set_can_target(mLabel, FALSE);   // never eat a pointer event

    // Positioned by the overlay's own placement hook rather than by margins or
    // a GtkFixed. Both of the obvious answers are wrong here:
    //
    //   - GtkFixed allocates its children their *minimum* size, and the minimum
    //     width of a wrapping label is one character - a vertical column of
    //     glyphs down the screen.
    //   - Margins are part of a widget's size *request*, so moving the bubble
    //     by changing them asks GTK to renegotiate the size of the toplevel. A
    //     layer surface anchored to both side edges has no width of its own to
    //     offer - the compositor decides it - and GDK's Wayland toplevel path
    //     says so every single time. One drag with a bubble up was a few
    //     hundred `compute_size: size.width > 0` warnings, which mattered the
    //     moment Phase 5 pointed stderr at a log file.
    //
    // get-child-position hands back an allocation directly, so following her
    // across the screen is a re-allocation of one child and nothing else.
    g_signal_connect(overlay, "get-child-position", G_CALLBACK(onChildPosition), this);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), mLabel);
}

gboolean Bubble::onChildPosition(GtkOverlay*, GtkWidget* child, GdkRectangle* alloc,
                                 gpointer data) {
    auto* self = static_cast<Bubble*>(data);
    // FALSE leaves the overlay to place it the usual way, which is what should
    // happen before the compositor has told us how wide the strip is.
    if (child != self->mLabel || self->mStripWidth <= 0) return FALSE;

    // Natural size, i.e. what this particular line actually needs. Measured
    // rather than assumed: the queue holds lines of very different lengths and a
    // fixed-width bubble would either clip them or float half empty. Measuring
    // here is free of the feedback loop the margin version had - the answer no
    // longer depends on where we last put it.
    int natW = 0, natH = 0;
    gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natW, nullptr, nullptr);
    natW = std::min(natW, std::max(1, self->mStripWidth - 2 * kEdge));
    gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, natW, nullptr, &natH, nullptr, nullptr);

    alloc->width = natW;
    alloc->height = natH;
    alloc->x = std::clamp(self->mCentreX - natW / 2, kEdge,
                          std::max(kEdge, self->mStripWidth - natW - kEdge));
    // Sits on her head: a tall two-line bubble grows upward, which keeps the
    // tail the same distance from her either way.
    alloc->y = std::max(0, self->mAboveY - natH - kGap);
    self->mAlloc = *alloc;

    static const bool debug = getenv("ASUNA_DEBUG_BUBBLE") != nullptr;
    if (debug)
        printf("asuna: bubble %dx%d at (%d,%d) centre=%d above=%d strip=%d\n", natW, natH,
               alloc->x, alloc->y, self->mCentreX, self->mAboveY, self->mStripWidth);
    return TRUE;
}

double Bubble::readingTime(const std::string& text) const {
    return std::min(mTiming.max, mTiming.base + mTiming.perGlyph * glyphCount(text));
}

// --- what she is saying -----------------------------------------------------

void Bubble::say(const std::string& text, double seconds) {
    if (!mLabel || text.empty()) return;
    if (seconds <= 0) seconds = readingTime(text);

    if (mQueue.size() >= kMaxQueue) mQueue.pop_front();
    mQueue.emplace_back(text, seconds);

    // Wait for a message that is still arriving; nothing is dropped - the queue
    // is what a reaction mid-answer goes into.
    if (!mArriving) {
        if (mState == State::Hidden || mState == State::FadingOut) showNext();
        // A finished message hands over rather than holding its full time: a
        // reply may be sitting there for a minute, and her reaction to being
        // poked in the middle of that minute is not worth waiting out.
        else if (mRemaining > kInterruptS) mRemaining = kInterruptS;
    }
    ensureTimer();
}

void Bubble::begin(const std::string& text) {
    if (!mLabel) return;
    // Whatever was queued is now stale: it was chosen before the thing that is
    // about to be said, and would follow it as a non sequitur.
    mQueue.clear();
    mWaiting = false;
    mArriving = true;
    mText.clear();
    mPieces.clear();
    mRemaining = 0;
    if (mState == State::Hidden || mState == State::FadingOut) mState = State::Showing;
    append(text);
    ensureTimer();
}

void Bubble::append(const std::string& chunk) {
    if (!mLabel || chunk.empty()) return;
    if (!mArriving) {
        begin(chunk);
        return;
    }
    mWaiting = false;
    mText += chunk;
    mPieces.push_back({mText.size(), g_get_monotonic_time()});
    render();
    ensureTimer();
    armWatchdog();
}

void Bubble::finish() {
    if (!mArriving) return;
    mArriving = false;
    armWatchdog();   // i.e. cancel it: nothing more is expected
    if (mWaiting) {
        // Nothing ever arrived. Take the wait down rather than leave it to time
        // out, which would read as her still listening.
        clear();
        return;
    }
    // Timed from the whole message under the reply ceiling rather than the
    // chatter one. It has been on screen for as long as it took to arrive, so
    // this is time to finish reading it rather than time to read it - but a long
    // answer that scrolled while it arrived needs the time back.
    mRemaining = std::min(kReplyMax, mTiming.base + mTiming.perGlyph * glyphCount(mText));
    ensureTimer();
}

void Bubble::waiting(bool on) {
    if (!mLabel) return;
    if (!on) {
        if (mWaiting) finish();
        return;
    }
    mQueue.clear();
    mArriving = true;
    mWaiting = true;
    mWaitPhase = 0;
    mText.clear();
    mPieces.clear();
    mRemaining = 0;
    if (mState == State::Hidden || mState == State::FadingOut) mState = State::Showing;
    ensureTimer();
    armWatchdog();
}

void Bubble::showNext() {
    if (mQueue.empty()) {
        mState = mOpacity > 0 ? State::FadingOut : State::Hidden;
        return;
    }
    auto [text, seconds] = mQueue.front();
    mQueue.pop_front();
    // A complete line is a message of one piece, so it emerges exactly the way
    // the first chunk of a streamed one does. One path, not two.
    mText = text;
    mPieces.assign(1, {mText.size(), g_get_monotonic_time()});
    mWaiting = false;
    mArriving = false;
    armWatchdog();
    mRemaining = seconds;
    mState = State::Showing;
    render();
}

void Bubble::clear() {
    mQueue.clear();
    mArriving = false;
    armWatchdog();
    mWaiting = false;
    mText.clear();
    mPieces.clear();
    if (mState != State::Hidden) mState = State::FadingOut;
    ensureTimer();
}

// --- drawing it -------------------------------------------------------------

// The label shows the *tail* of the message - the last mRows wrapped rows - so
// a long reply scrolls upward inside a bubble of bounded height instead of
// growing off the top of the strip.
//
// Measured with Pango rather than counted in characters: the wrap point depends
// on the font, the glyph widths and the bubble's own width, and this text is
// mostly Chinese, where a character count is not a width. The label is given
// the whole text first because that is what decides how wide the bubble wants
// to be, and the layout is then wrapped to exactly the width the text will get
// - which is the widget's width *minus its padding*. Wrapping to the widget's
// own width makes Pango count one row fewer than GTK draws.
size_t Bubble::visibleFrom(const std::string& text) {
    if (mRows <= 0 || mStripWidth <= 0 || text.empty()) return 0;
    if (mChrome < 0) {
        gtk_label_set_text(GTK_LABEL(mLabel), "");
        gtk_widget_measure(mLabel, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &mChrome, nullptr,
                           nullptr);
    }
    gtk_label_set_text(GTK_LABEL(mLabel), text.c_str());
    int natW = 0;
    gtk_widget_measure(mLabel, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natW, nullptr, nullptr);
    natW = std::max(1, std::min(natW, std::max(1, mStripWidth - 2 * kEdge)) - mChrome);

    PangoLayout* layout = gtk_widget_create_pango_layout(mLabel, text.c_str());
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_width(layout, natW * PANGO_SCALE);
    const int lines = pango_layout_get_line_count(layout);
    size_t from = 0;
    if (lines > mRows) {
        const PangoLayoutLine* first = pango_layout_get_line_readonly(layout, lines - mRows);
        from = static_cast<size_t>(first->start_index);
    }
    g_object_unref(layout);
    return from;
}

bool Bubble::render() {
    if (!mLabel) return false;
    const gint64 now = g_get_monotonic_time();

    if (mWaiting) {
        // Three dots with a wave of brightness passing along them. Markup
        // rather than three widgets: it is one label either way, and Pango can
        // give each dot its own alpha.
        std::string markup;
        for (int i = 0; i < kWaitDots; ++i) {
            const double phase = mWaitPhase - i * (kWaitPeriod / (kWaitDots * 2));
            const double wave = 0.5 + 0.5 * std::sin(phase * 2 * M_PI / kWaitPeriod);
            const int alpha = static_cast<int>((0.28 + 0.62 * wave) * 65535);
            char span[64];
            snprintf(span, sizeof(span), "<span alpha=\"%d\">\xc2\xb7</span>", alpha);
            markup += span;
            if (i + 1 < kWaitDots) markup += " ";
        }
        gtk_label_set_markup(GTK_LABEL(mLabel), markup.c_str());
        reposition();
        return true;
    }

    if (mText.empty()) {
        gtk_label_set_text(GTK_LABEL(mLabel), "");
        return false;
    }

    const size_t from = visibleFrom(mText);
    std::string markup;
    bool fading = false;
    size_t at = from;
    for (const Piece& piece : mPieces) {
        if (piece.end <= from) continue;
        const size_t begin = std::max(at, from);
        if (piece.end <= begin) continue;
        const double age = (now - piece.atUs) / 1e6;
        const char* text = mText.data();
        if (age >= kEmergeS) {
            // Settled: no span at all, so the common case of a bubble that has
            // finished arriving is plain text with no markup to parse.
            appendEscaped(&markup, text + begin, text + piece.end);
        } else {
            fading = true;
            const double t = std::clamp(age / kEmergeS, 0.0, 1.0);
            // Smoothstep, so it eases in and out rather than ramping linearly -
            // a linear fade has a visible start and stop.
            const double eased = t * t * (3.0 - 2.0 * t);
            const int alpha =
                static_cast<int>((kEmergeFloor + (1.0 - kEmergeFloor) * eased) * 65535);
            char open[40];
            snprintf(open, sizeof(open), "<span alpha=\"%d\">", alpha);
            markup += open;
            appendEscaped(&markup, text + begin, text + piece.end);
            markup += "</span>";
        }
        at = piece.end;
    }
    gtk_label_set_markup(GTK_LABEL(mLabel), markup.c_str());
    reposition();
    return fading;
}

void Bubble::setRows(int rows) {
    mRows = std::max(1, rows);
    if (!mText.empty()) render();
}

int Bubble::bandFor(int rows) {
    if (!mLabel || rows <= 0) return 0;
    const auto heightOf = [this](const char* text) {
        gtk_label_set_text(GTK_LABEL(mLabel), text);
        int natural = 0;
        gtk_widget_measure(mLabel, GTK_ORIENTATION_VERTICAL, -1, nullptr, &natural, nullptr,
                           nullptr);
        return natural;
    };
    // The rows themselves, measured as one block rather than as a line height
    // multiplied out. Pango rounds the height of a *layout*, not of each line in
    // it, so one row plus seven line heights came to two pixels less than eight
    // rows actually take - and two pixels is the difference between the gap over
    // her head being the one that was asked for and being whatever was left.
    std::string rowsProbe;
    for (int i = 0; i < rows; ++i) rowsProbe += (i ? "\n字" : "字");
    const int text = heightOf(rowsProbe.c_str());
    // Whatever was on screen has just been overwritten to measure with. Put it
    // back: this runs on every outfit change and every resize, and she may well
    // be in the middle of a sentence.
    if (mState != State::Hidden) render();
    else gtk_label_set_text(GTK_LABEL(mLabel), "");
    return text + kGap;
}

bool Bubble::rect(GdkRectangle* out) const {
    if (mState == State::Hidden || mAlloc.width <= 0) return false;
    if (out) *out = mAlloc;
    return true;
}

void Bubble::place(int centreX, int aboveY, int stripWidth) {
    mCentreX = centreX;
    mAboveY = aboveY;
    mStripWidth = stripWidth;
    if (mState != State::Hidden) reposition();
}

// Asks the overlay to place its children again. Deliberately queue_allocate and
// not queue_resize: the bubble's *size* has not changed, only where it goes, and
// a resize is the thing that walks up to the toplevel.
void Bubble::reposition() {
    if (mOverlay) gtk_widget_queue_allocate(mOverlay);
}

void Bubble::ensureTimer() {
    if (!mTimer && mState != State::Hidden) mTimer = g_timeout_add(kTickMs, onTick, this);
}

void Bubble::armWatchdog() {
    if (mWatchdog) g_source_remove(mWatchdog);
    mWatchdog = mArriving ? g_timeout_add(kArrivalTimeoutMs, onWatchdog, this) : 0;
}

gboolean Bubble::onWatchdog(gpointer data) {
    auto* self = static_cast<Bubble*>(data);
    self->mWatchdog = 0;
    if (!self->mArriving) return G_SOURCE_REMOVE;
    fprintf(stderr, "asuna: nothing more arrived for %u s - ending the line\n",
            kArrivalTimeoutMs / 1000);
    self->finish();
    return G_SOURCE_REMOVE;
}

gboolean Bubble::onTick(gpointer data) {
    auto* self = static_cast<Bubble*>(data);
    const double dt = kTickMs / 1000.0;

    if (self->mState == State::Hidden) {
        self->mTimer = 0;
        return G_SOURCE_REMOVE;
    }

    // The container fades as a whole; the text inside it emerges piece by
    // piece. Two different things, and they are meant to look different: the
    // bubble arriving is a shape appearing, and the text arriving is a sentence
    // assembling.
    if (self->mState == State::FadingOut) {
        self->mOpacity = std::max(0.0, self->mOpacity - dt / kFadeOut);
        if (self->mOpacity <= 0.0) {
            if (!self->mQueue.empty()) {
                self->showNext();
            } else {
                self->mState = State::Hidden;
                self->mText.clear();
                self->mPieces.clear();
                gtk_label_set_text(GTK_LABEL(self->mLabel), "");
                gtk_widget_set_opacity(self->mLabel, 0.0);
                self->mTimer = 0;
                return G_SOURCE_REMOVE;
            }
        }
        gtk_widget_set_opacity(self->mLabel, self->mOpacity);
        return G_SOURCE_CONTINUE;
    }

    self->mOpacity = std::min(1.0, self->mOpacity + dt / kFadeIn);
    gtk_widget_set_opacity(self->mLabel, self->mOpacity);

    if (self->mWaiting) {
        self->mWaitPhase += dt;
        self->render();
        return G_SOURCE_CONTINUE;
    }

    const bool fading = self->render();

    // A message still arriving has no countdown: it stays until whoever is
    // writing it says it is finished.
    if (!self->mArriving) {
        self->mRemaining -= dt;
        if (self->mRemaining <= 0) {
            // Straight into the next line if one is waiting, so a burst does
            // not blink the bubble in and out between every sentence.
            if (!self->mQueue.empty()) self->showNext();
            else self->mState = State::FadingOut;
            return G_SOURCE_CONTINUE;
        }
    }

    // Nothing is moving: the fade is done, the text has settled, and either
    // something else will wake this up or the countdown will. A held message
    // with no countdown would otherwise spin a 60 Hz timer for nothing.
    if (!fading && self->mArriving && self->mOpacity >= 1.0) {
        self->mTimer = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

}  // namespace asuna

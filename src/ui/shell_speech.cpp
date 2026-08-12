// Shell, the part that speaks: the hooks Behaviour reaches her through, the
// dialogue lookups behind them, and where the speech bubble is put.
#include "ui/shell.hpp"

#include <ctime>

#include "ui/shell_internal.hpp"

namespace asuna {
// --- personality ------------------------------------------------------------

void Shell::wireBehaviour() {
    mBehaviour.setHooks({
        [this](const std::string& name) { mPet.setExpression(name); },
        [this](const std::string& name) { mPet.startMotionNamed(name, 2); },
        [this](const std::string& group) { mPet.startRandomMotion(group, 1); },
        [this](const std::string& key) { say(key); },
    });
}

void Shell::say(const std::string& dialogueKey) {
    // An empty result is normal - a dialogue file may simply have nothing for a
    // trigger - and Bubble::say ignores it, so there is nothing to check here.
    placeBubble();
    mBubble.say(mDialogue.pick(dialogueKey));
}

void Shell::schedulePlaceBubble() {
    if (!mBubbleIdle) mBubbleIdle = g_idle_add(onPlaceBubbleIdle, this);
}

gboolean Shell::onPlaceBubbleIdle(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->mBubbleIdle = 0;
    // A resize we asked for has not landed yet, so the strip height on record
    // is the old one and she is momentarily too big (or too small) for it.
    // Placing now would flick the bubble to the top of the screen for a frame;
    // the configure arriving is itself a trigger, so simply wait for it. If it
    // never arrives the bubble keeps its last good position, which is the right
    // failure.
    if (self->mStripHeight > 0 &&
        self->mRequestedHeight != self->mStripHeight / self->mScaleFactor)
        return G_SOURCE_REMOVE;
    self->placeBubble();
    return G_SOURCE_REMOVE;
}

void Shell::placeBubble() {
    if (!mPlaced) return;
    const int centre = static_cast<int>(mMotion.placement().x + mPet.boxWidth() / 2.0);
    mBubble.place(centre / mScaleFactor, bodyRect().y, mStripWidth / mScaleFactor);
}


gboolean Shell::onGreetTimeout(gpointer data) {
    auto* self = static_cast<Shell*>(data);
    self->placeBubble();
    self->mBubble.say(self->mDialogue.greeting(std::time(nullptr)));
    return G_SOURCE_REMOVE;
}

gboolean Shell::onHideTimeout(gpointer data) {
    static_cast<Shell*>(data)->setHidden(true);
    return G_SOURCE_REMOVE;
}

void Shell::sayText(const std::string& text, double seconds) {
    placeBubble();
    mBubble.say(text, seconds);
}

}  // namespace asuna

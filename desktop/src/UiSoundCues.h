#pragma once

// The interface's own sounds: the small noises a click, a page change or an
// error makes, as distinct from the music an agent run plays (AgentSonifier.h).
//
// They share that file's synthesiser and its scale on purpose. Everything the
// app can make a sound with is one instrument playing one D major pentatonic,
// so a click landing while three agents are being listened to is a note in the
// same key rather than a beep over the top of the music. What separates the
// interface from the agents is the register and the room: cues sit high, above
// where the voices sing, and they play dry - no echo - so a click is over by
// the time the mouse is up.
//
// Nothing here opens a device or touches a widget; it is a pure function from
// "what just happened" to a handful of notes, so the whole vocabulary is
// checked in tests/test_agent_sonifier.cpp with no audio hardware at all.
// UiSounds.h is the part that plays them.

#include "AgentSonifier.h"

#include <QString>
#include <QVector>

namespace forkmesh::sonify {

// What just happened, in the only terms a sound can express. These are
// gestures, not widgets: several controls share a cue when pressing them means
// the same thing, and one control can raise different cues depending on what it
// did. The grammar matches the agents' - work going out rises, answers coming
// back fall - so the two layers say the same thing the same way.
enum class Cue {
    Keystroke, // a key typed into the agent prompt: barely a tick
    Click,     // a plain button: the flattest, quietest sound a control makes
    ToggleOn,  // a checkable control switched on: the pair rises
    ToggleOff, // and off: the same pair, falling
    Select,    // a row picked out of a list
    Navigate,  // a different page, tab or rail entry
    Open,      // a dialog, menu or panel appearing
    Close,     // and going away again
    Send,      // something committed and gone out: a prompt, a message
    Arrive,    // and something coming back
    Success,   // an operation finished and worked
    Warn,      // something wants looking at, but nothing broke
    Error,     // it did not work
    Delete,    // something destroyed: low, and the only cue that falls that far
    Merge,
};

inline constexpr int kCueCount = 15;

// Every cue, in the order they are declared: the settings page previews them
// in this order and the tests walk it, so a cue added above cannot be
// forgotten by either.
QVector<Cue> allCues();

// "click", "toggle-on" - the stable slug for a cue, for tests and logging.
// The settings page spells its own labels; these never need translating.
QString cueName(Cue cue);

// The interface's own voice. Celesta two octaves above the agents' tonic: it
// cuts through whatever they are playing without ever being in their way, and
// it is the one instrument in the set with no wooden body, which is what makes
// a cue audibly not an agent.
Voice interfaceVoice();

// The notes a cue plays, starting at `startSeconds` on the mixer's clock.
// `volume` is the preference, 0..1, applied to every gain; 0 returns nothing
// rather than silent notes, so a muted cue costs the mixer no slots at all.
QVector<Note> cueNotes(Cue cue, double startSeconds, double volume = 1.0);

// A control's words can say more than its shape: "Delete" is a delete however
// it is drawn, and "Send" is a send. Everything else keeps `fallback`. A label
// that ends in an ellipsis only opens something that asks first, so it stays
// the fallback - the sound of a delete belongs to the button that actually
// deletes, not the one that opens the confirmation.
Cue cueForLabel(const QString &label, Cue fallback);

// How much a cue says, low to high. When one gesture raises several cues - a
// menu item chosen and the menu closing, a button pressed and the dialog it
// opens - only the weightiest is played, so the gesture is one sound and it is
// the sound of what actually happened.
int cueWeight(Cue cue);

// The shortest gap between two cues worth sounding. Interfaces emit events in
// bursts - a click is a press and a release, a list rebuild moves the current
// row a dozen times - and past this the ear hears a rattle rather than any of
// the individual sounds, so everything inside the window collapses into one -
// unless the later cue is weightier, which is news worth the overlap.
inline constexpr int kCueThrottleMs = 45;

} // namespace forkmesh::sonify

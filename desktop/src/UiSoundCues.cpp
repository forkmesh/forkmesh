// What each interface gesture sounds like. Pure Qt Core, so the whole table is
// checked without an audio device (tests/test_agent_sonifier.cpp).
//
// The rules the table keeps to, which are what stop an app that talks back all
// day from becoming an app whose sound you switch off:
//
//  - Short. A click is fifty milliseconds. Only the three cues that report an
//    outcome - success, error, delete - are allowed to ring.
//  - Quiet, and quieter the more often it happens. A click is the most frequent
//    sound in the app and the softest; an error is rare and the loudest.
//  - Up for outgoing, down for returning, the same way round as the agents'
//    melody, so the two never contradict each other.
//  - On the scale. Every frequency comes from degreeFrequency(), so an
//    interface sound landing on top of a run in progress is still consonant.

#include "UiSounds.h"

#include <QStringList>
#include <QtGlobal>

#include <utility>

namespace forkmesh::sonify {

QVector<Cue> allCues()
{
    return {Cue::Keystroke, Cue::Click, Cue::ToggleOn, Cue::ToggleOff, Cue::Select,
            Cue::Navigate, Cue::Open, Cue::Close, Cue::Send,
            Cue::Arrive, Cue::Success, Cue::Warn, Cue::Error,
            Cue::Delete, Cue::Merge};
}

Voice interfaceVoice()
{
    Voice voice;
    voice.instrument = Instrument::Celesta; // bright and short: it cuts without carrying
    voice.octave = 2;                       // well above the agents' registers
    voice.pan = 0.5;                        // the interface is the whole room, not a seat in it
    voice.degreeOffset = 0;
    voice.slotOffset = 0; // the interface never waits for the grid
    voice.detune = 1.0;
    return voice;
}

QString cueName(Cue cue)
{
    switch (cue) {
    case Cue::Keystroke:
        return QStringLiteral("keystroke");
    case Cue::Click:
        return QStringLiteral("click");
    case Cue::ToggleOn:
        return QStringLiteral("toggle-on");
    case Cue::ToggleOff:
        return QStringLiteral("toggle-off");
    case Cue::Select:
        return QStringLiteral("select");
    case Cue::Navigate:
        return QStringLiteral("navigate");
    case Cue::Open:
        return QStringLiteral("open");
    case Cue::Close:
        return QStringLiteral("close");
    case Cue::Send:
        return QStringLiteral("send");
    case Cue::Arrive:
        return QStringLiteral("arrive");
    case Cue::Success:
        return QStringLiteral("success");
    case Cue::Warn:
        return QStringLiteral("warn");
    case Cue::Error:
        return QStringLiteral("error");
    case Cue::Delete:
        return QStringLiteral("delete");
    case Cue::Merge:
        return QStringLiteral("merge");
    }
    return QStringLiteral("click");
}

Cue cueForLabel(const QString &label, Cue fallback)
{
    QString text = label.trimmed();
    if (text.endsWith(QChar(0x2026)) || text.endsWith(QLatin1String("...")))
        return fallback;
    text.remove(QLatin1Char('&')); // mnemonics: "&Delete"
    const QString word =
        text.section(QLatin1Char(' '), 0, 0, QString::SectionSkipEmpty).toLower();
    static const QStringList destroys = {
        QStringLiteral("delete"), QStringLiteral("remove"),
        QStringLiteral("discard"), QStringLiteral("uninstall"),
        QStringLiteral("trash"), QStringLiteral("erase"),
        QStringLiteral("purge")};
    static const QStringList sends = {
        QStringLiteral("send"), QStringLiteral("post"),
        QStringLiteral("submit"), QStringLiteral("publish"),
        QStringLiteral("push")};
    if (destroys.contains(word))
        return Cue::Delete;
    if (sends.contains(word))
        return Cue::Send;
    return fallback;
}

int cueWeight(Cue cue)
{
    switch (cue) {
    case Cue::Keystroke:
        return 0; // anything else the same moment does says more
    case Cue::Select:
        return 1;
    case Cue::Close:
        return 2; // below a click: choosing a menu item is not "closing"
    case Cue::Click:
        return 3;
    case Cue::ToggleOn:
    case Cue::ToggleOff:
        return 4;
    case Cue::Navigate:
        return 5;
    case Cue::Open:
        return 6; // a click that opens a dialog sounds like the dialog
    case Cue::Arrive:
        return 7;
    case Cue::Send:
        return 8;
    case Cue::Merge:
        return 9;
    case Cue::Warn:
        return 10;
    case Cue::Success:
        return 11;
    case Cue::Delete:
        return 12;
    case Cue::Error:
        return 13;
    }
    return 0;
}

namespace {

// One step of a cue: a degree of the scale, how long it is held, how loud, and
// how far into the figure it starts. Cues are short enough that spelling them
// out like this is clearer than any amount of cleverness.
struct Step {
    int degree;
    double delay;   // seconds after the cue's start
    double seconds; // how long it is held
    double gain;    // before the user's volume
    Timbre timbre;
};

} // namespace

QVector<Note> cueNotes(Cue cue, double startSeconds, double volume)
{
    const Voice voice = interfaceVoice();
    QVector<Step> steps;

    switch (cue) {
    case Cue::Keystroke:
        // Typing is hundreds of these a minute, so it is the least of all: the
        // shortest, quietest tick, under the click. UiSounds shades each one
        // differently so a sentence sounds like typing rather than a metronome.
        steps = {{1, 0, 0.022, 0.04, Timbre::Wood}};
        break;
    case Cue::Click:
        // The commonest sound a control makes, so the smallest: one wooden tick.
        steps = {{0, 0, 0.05, 0.085, Timbre::Mallet}};
        break;
    case Cue::ToggleOn:
        // Two notes up, because something is now on.
        steps = {{0, 0, 0.05, 0.075, Timbre::Mallet},
                 {2, 0.045, 0.07, 0.085, Timbre::Glass}};
        break;
    case Cue::ToggleOff:
        // The same pair, the other way round.
        steps = {{2, 0, 0.05, 0.075, Timbre::Glass},
                 {0, 0.045, 0.07, 0.08, Timbre::Mallet}};
        break;
    case Cue::Select:
        // Rows go by in bursts while you arrow through a list; this one is
        // deliberately almost nothing.
        steps = {{-1, 0, 0.035, 0.05, Timbre::Wood}};
        break;
    case Cue::Navigate:
        // A page changed under you: higher than a click, still one note.
        steps = {{3, 0, 0.07, 0.09, Timbre::Glass}};
        break;
    case Cue::Open:
        // Something appeared: three notes up, opening out.
        steps = {{0, 0, 0.06, 0.07, Timbre::Glass},
                 {2, 0.04, 0.06, 0.075, Timbre::Glass},
                 {4, 0.08, 0.13, 0.085, Timbre::Bell}};
        break;
    case Cue::Close:
        // And the same figure closing.
        steps = {{4, 0, 0.05, 0.075, Timbre::Glass},
                 {2, 0.04, 0.05, 0.07, Timbre::Glass},
                 {0, 0.08, 0.11, 0.07, Timbre::Mallet}};
        break;
    case Cue::Send:
        // Work leaving: a quick run up, ending unresolved, because the answer
        // has not come back yet.
        steps = {{0, 0, 0.05, 0.085, Timbre::Mallet},
                 {3, 0.05, 0.05, 0.09, Timbre::Glass},
                 {5, 0.1, 0.16, 0.095, Timbre::Bell}};
        break;
    case Cue::Arrive:
        // And the answer: the same interval falling onto the root.
        steps = {{5, 0, 0.06, 0.085, Timbre::Bell},
                 {2, 0.06, 0.18, 0.09, Timbre::Bell}};
        break;
    case Cue::Success:
        // Allowed to ring: it is the sound of a piece of work being over.
        steps = {{0, 0, 0.09, 0.1, Timbre::Bell},
                 {3, 0.09, 0.09, 0.105, Timbre::Bell},
                 {5, 0.18, 0.45, 0.115, Timbre::Bell},
                 {10, 0.18, 0.45, 0.06, Timbre::Bell}}; // the octave over it
        break;
    case Cue::Warn:
        // Low, held, and breathy: it asks you to look without saying anything
        // broke. Pad is the one timbre with a slow attack, which is what keeps
        // a warning from startling the way an error is meant to.
        steps = {{-2, 0, 0.3, 0.1, Timbre::Pad},
                 {-2, 0.16, 0.26, 0.075, Timbre::Pad}};
        break;
    case Cue::Error:
        // The loudest thing the interface plays, and the only one that drops
        // below its own tonic twice.
        steps = {{-1, 0, 0.13, 0.13, Timbre::Wood},
                 {-4, 0.11, 0.42, 0.13, Timbre::Pad},
                 {-6, 0.11, 0.42, 0.08, Timbre::Pad}};
        break;
    case Cue::Delete:
        // Something is gone: a drop, dry, with nothing ringing after it.
        steps = {{2, 0, 0.05, 0.09, Timbre::Wood},
                 {-3, 0.055, 0.2, 0.1, Timbre::Wood}};
        break;
    case Cue::Merge:
        steps = {{0, 0, 0.03, 0.1, Timbre::Wood},
                 {7, 0, 0.025, 0.11, Timbre::Ping},
                 {-3, 0.03, 0.44, 0.045, Timbre::Swell},
                 {2, 0.1, 0.38, 0.05, Timbre::Swell},
                 {-1, 0.06, 0.03, 0.025, Timbre::Chirp},
                 {0, 0.11, 0.03, 0.03, Timbre::Chirp},
                 {1, 0.16, 0.03, 0.035, Timbre::Chirp},
                 {2, 0.21, 0.03, 0.04, Timbre::Chirp},
                 {3, 0.25, 0.03, 0.045, Timbre::Chirp},
                 {4, 0.29, 0.03, 0.05, Timbre::Chirp},
                 {5, 0.33, 0.03, 0.055, Timbre::Chirp},
                 {6, 0.36, 0.03, 0.06, Timbre::Chirp},
                 {7, 0.39, 0.03, 0.065, Timbre::Chirp},
                 {8, 0.42, 0.03, 0.07, Timbre::Chirp},
                 {10, 0.46, 0.34, 0.05, Timbre::Bell},
                 {5, 0.46, 0.34, 0.1, Timbre::Bell}};
        break;
    }

    const double level = qBound(0.0, volume, 1.0);
    QVector<Note> notes;
    notes.reserve(steps.size());
    for (const Step &step : std::as_const(steps)) {
        Note note;
        note.startSeconds = startSeconds + step.delay;
        note.seconds = step.seconds;
        note.frequency = degreeFrequency(step.degree, voice);
        note.gain = step.gain * level;
        note.pan = voice.pan;
        note.timbre = step.timbre;
        note.instrument = voice.instrument;
        note.bloom = 0;
        if (note.isRest())
            continue; // volume 0: the cue is silent rather than absent
        notes.append(note);
    }
    return notes;
}

} // namespace forkmesh::sonify

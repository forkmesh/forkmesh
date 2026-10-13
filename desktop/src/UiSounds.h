#pragma once

// Playing the interface's own sounds (UiSoundCues.h): the device, the
// preference that gates it, and the one application-wide event filter that
// notices a click without every button in the app having to be told to make a
// noise.
//
// That filter is the whole design. An interface with two hundred buttons
// cannot have two hundred call sites remembering to play a click, and the ones
// that get forgotten are exactly the ones that feel broken - so instead one
// filter watches the events Qt already sends and maps the handful that mean
// something to a cue. Widgets stay unaware they make sound. The cues that no
// event can express - a message sent, a job finished, a notice shown - are the
// only ones the app raises by hand, through play().
//
// The filter's cues are provisional. What it hears is held until the event has
// been handled, and a cue the handler raises by hand - a send, a delete, a
// notice - replaces it, as does a weightier cue from the same gesture (the
// dialog a button opened). So one gesture is one sound, and it is the sound of
// what the gesture did rather than of the control it happened on.
//
// On by default on a desktop with a screen; the Settings switch turns it off,
// and off means off: with the preference clear the filter is not installed at
// all, so the cost to someone who does not want this is one boolean read at
// startup and nothing afterwards.

#include "UiSoundCues.h"

#include <QObject>
#include <QString>

#include <memory>
#include <optional>

class QEvent;
class QTimer;

namespace forkmesh::sonify {

// Whether the interface makes any sound at all. On by default (see
// defaultEnabled()); the Settings page switches it off.
inline const QString kUiSoundsSetting = QStringLiteral("ui/interfaceSounds");
// How loud, 0..100. The cues are quiet at full volume by design - they sit
// under speech and under the agents' music - so this mostly exists to push
// them further down rather than up.
inline const QString kUiSoundVolumeSetting =
    QStringLiteral("ui/interfaceSoundVolume");
inline constexpr int kDefaultUiSoundVolume = 55;

// A text field with this dynamic property set true ticks as you type in it
// (Cue::Keystroke). Opt-in per field rather than everywhere: the agent prompt
// is where typing is the whole interaction, and a tick in every search box and
// settings field would be the sound people switch off.
inline constexpr char kTypingSoundProperty[] = "forkmeshTypingSound";
inline constexpr char kQuietSoundProperty[] = "forkmeshQuietSounds";

class UiSounds : public QObject
{
public:
    // Created on the first call and owned by the application object, so the
    // device is torn down with it rather than at some point during static
    // destruction. Null before a QCoreApplication exists.
    static UiSounds *instance();

    // The preference, cached: this is read on every mouse release in the app,
    // which is far too often to open a QSettings for.
    static bool enabled();
    // What the preference is before anyone has touched it: on, except where
    // nobody is listening - the offscreen and minimal platforms the headless
    // node and the window tests run on, and QStandardPaths test mode. A test
    // that clicks two hundred buttons must not play them through the speakers.
    static bool defaultEnabled();
    static void setEnabled(bool on);
    static int volumePercent();
    static void setVolumePercent(int percent);

    // Play one cue now, if sounds are on. Safe to call from the UI thread at
    // any time, including before a device has ever been opened and after one
    // has failed to open; cheap enough to sit in a hot path, because with
    // sounds off it returns on a cached boolean.
    static void play(Cue cue);

    // Plays the cue whether or not the preference is on: the settings page
    // previewing what a sound is like, which must work while you are deciding
    // whether to switch it on.
    void preview(Cue cue);

    QString backendName() const;

    // The cue an event delivered to `watched` stands for, or nothing for the
    // overwhelming majority that mean nothing. Pure: it plays nothing and
    // changes nothing, which is what lets tests/test_ui_sounds.cpp walk every
    // kind of control with synthetic events and no audio device.
    static std::optional<Cue> cueForEvent(QObject *watched, QEvent *event);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit UiSounds(QObject *parent);
    ~UiSounds() override;

    void playCue(Cue cue);
    // Holds a cue the filter heard until the event that raised it has been
    // handled; the weightiest one held wins, and play() drops it.
    void holdCue(Cue cue);
    void flushHeldCue();
    // Opens the device if it is not open, and returns false when this machine
    // has no way to make sound. Remembered, so a box with no audio does not
    // try to start a player on every click.
    bool ensurePlayer();
    void applyFilter();

    std::unique_ptr<Mixer> m_mixer;
    std::unique_ptr<Player> m_player;
    bool m_enabled = false;
    bool m_filterInstalled = false;
    bool m_deviceRefused = false;
    double m_volume = 0;
    qint64 m_lastCueMs = 0;
    Cue m_lastCue = Cue::Select;
    quint32 m_keystrokes = 0; // shades each keystroke differently
    std::optional<Cue> m_heldCue;
    QTimer *m_idleTimer = nullptr;
};

} // namespace forkmesh::sonify

#include "UiSounds.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRadioButton>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QWidget>

#include <cmath>

namespace forkmesh::sonify {

namespace {

// The device is held open this long after the last cue. Opening it costs a
// process launch, which would be audible as a swallowed first click if it
// happened per sound; leaving it open forever costs a pipe rendering silence
// for the rest of the session. A quarter of a minute covers a burst of work
// and closes behind someone who has wandered off.
constexpr int kIdleCloseMs = 15000;

UiSounds *g_instance = nullptr;

} // namespace

UiSounds::UiSounds(QObject *parent) : QObject(parent)
{
    QSettings settings;
    m_enabled = settings.value(kUiSoundsSetting, defaultEnabled()).toBool();
    const int percent =
        settings.value(kUiSoundVolumeSetting, kDefaultUiSoundVolume).toInt();
    m_volume = qBound(0, percent, 100) / 100.0;
    applyFilter();
}

UiSounds::~UiSounds()
{
    if (m_filterInstalled && qApp)
        qApp->removeEventFilter(this);
    if (m_player)
        m_player->stop();
    g_instance = nullptr;
}

UiSounds *UiSounds::instance()
{
    if (!qApp)
        return nullptr; // before the application object, and after it is gone
    if (!g_instance)
        g_instance = new UiSounds(qApp);
    return g_instance;
}

bool UiSounds::defaultEnabled()
{
    if (QStandardPaths::isTestModeEnabled())
        return false;
    const QString platform = QGuiApplication::platformName();
    return platform != QLatin1String("offscreen") &&
           platform != QLatin1String("minimal");
}

bool UiSounds::enabled()
{
    UiSounds *sounds = instance();
    return sounds && sounds->m_enabled;
}

void UiSounds::setEnabled(bool on)
{
    QSettings().setValue(kUiSoundsSetting, on);
    UiSounds *sounds = instance();
    if (!sounds || sounds->m_enabled == on)
        return;
    sounds->m_enabled = on;
    sounds->applyFilter();
    if (!on)
        sounds->m_heldCue.reset();
    if (!on && sounds->m_player) {
        // Switching it off has to be silent immediately, including whatever is
        // still ringing from the click that switched it off.
        if (sounds->m_mixer)
            sounds->m_mixer->clear();
        sounds->m_player->stop();
    }
}

int UiSounds::volumePercent()
{
    UiSounds *sounds = instance();
    if (!sounds)
        return kDefaultUiSoundVolume;
    return int(std::lround(sounds->m_volume * 100.0));
}

void UiSounds::setVolumePercent(int percent)
{
    const int bounded = qBound(0, percent, 100);
    QSettings().setValue(kUiSoundVolumeSetting, bounded);
    if (UiSounds *sounds = instance())
        sounds->m_volume = bounded / 100.0;
}

void UiSounds::play(Cue cue)
{
    // With sounds off this is a null check and a boolean, so it can sit in
    // any hot path.
    UiSounds *sounds = g_instance;
    if (!sounds || !sounds->m_enabled)
        return;
    // Said by hand, so it knows better than whatever the filter guessed from
    // the control: the Send button's click becomes the send.
    sounds->m_heldCue.reset();
    sounds->playCue(cue);
}

void UiSounds::preview(Cue cue)
{
    playCue(cue);
}

QString UiSounds::backendName() const
{
    return m_player ? m_player->backendName() : QStringLiteral("none");
}

void UiSounds::applyFilter()
{
    if (!qApp)
        return;
    if (m_enabled == m_filterInstalled)
        return;
    if (m_enabled)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);
    m_filterInstalled = m_enabled;
}

bool UiSounds::ensurePlayer()
{
    if (m_deviceRefused)
        return false;
    if (!m_mixer) {
        m_mixer = std::make_unique<Mixer>();
        // Dry: the agents' room would turn every click into three.
        m_mixer->setEchoDepth(0);
    }
    if (!m_player)
        m_player = std::make_unique<Player>(m_mixer.get(), this);
    if (!m_player->isRunning() && !m_player->start()) {
        // No audio on this machine. Remember it: a box with no player must not
        // try to spawn one on every button press for the rest of the session.
        m_deviceRefused = true;
        return false;
    }
    if (!m_idleTimer) {
        m_idleTimer = new QTimer(this);
        m_idleTimer->setSingleShot(true);
        connect(m_idleTimer, &QTimer::timeout, this, [this] {
            if (!m_player)
                return;
            if (m_mixer && !m_mixer->idle()) {
                m_idleTimer->start(1000); // something is still ringing
                return;
            }
            m_player->stop();
        });
    }
    m_idleTimer->start(kIdleCloseMs);
    return true;
}

void UiSounds::playCue(Cue cue)
{
    if (m_volume <= 0)
        return;
    // One rattle of events - a press and a release, a list rebuilding its
    // selection - is one sound. A weightier cue still gets through: an error
    // landing on the heels of the click that caused it is worth hearing.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastCueMs < kCueThrottleMs &&
        cueWeight(cue) <= cueWeight(m_lastCue))
        return;
    m_lastCueMs = now;
    m_lastCue = cue;
    if (!ensurePlayer())
        return;
    // A short lead, so the first note is not half-rendered by the time it is
    // added: the cue starts on the next buffer rather than in the middle of
    // the one being written.
    QVector<Note> notes = cueNotes(cue, m_mixer->now() + 0.02, m_volume);
    if (cue == Cue::Keystroke) {
        // No two keys quite alike - brighter, duller, a hair longer - the way
        // a real keyboard never strikes the same twice. Never 0: that is the
        // plain, unshaded instrument.
        const quint32 seed = ++m_keystrokes * 2654435761u | 1u;
        for (Note &note : notes)
            note.seed = seed;
    }
    m_mixer->addNotes(notes);
}

void UiSounds::holdCue(Cue cue)
{
    if (m_heldCue) {
        // Same gesture, second opinion: the weightier one is what happened.
        if (cueWeight(cue) > cueWeight(*m_heldCue))
            m_heldCue = cue;
        return;
    }
    m_heldCue = cue;
    // Runs once the event that raised it has been handled - or inside the
    // modal loop a handler opens, after that dialog's own cue has had its say.
    QTimer::singleShot(0, this, &UiSounds::flushHeldCue);
}

void UiSounds::flushHeldCue()
{
    if (!m_heldCue || !m_enabled)
        return;
    const Cue cue = *m_heldCue;
    m_heldCue.reset();
    playCue(cue);
}

namespace {

bool releaseIsInside(QWidget *widget, QMouseEvent *mouse)
{
    return widget->rect().contains(mouse->position().toPoint());
}

// A checkable button that is one of a set - the activity rail, a segmented
// switcher, radio buttons - picks a place rather than switching a thing on.
bool isOneOfASet(const QAbstractButton *button)
{
    if (button->autoExclusive())
        return true;
    const QButtonGroup *group = button->group();
    return group && group->exclusive();
}

// A button's cue, for what pressing it is about to do. The state has not
// flipped yet when this runs, so a switch sounds as what it becomes.
Cue buttonCue(const QAbstractButton *button)
{
    if (button->isCheckable()) {
        if (isOneOfASet(button))
            return qobject_cast<const QRadioButton *>(button) ? Cue::Select
                                                              : Cue::Navigate;
        return button->isChecked() ? Cue::ToggleOff : Cue::ToggleOn;
    }
    return cueForLabel(button->text(), Cue::Click);
}

std::optional<Cue> releaseCue(QWidget *widget, QMouseEvent *mouse)
{
    if (mouse->button() != Qt::LeftButton)
        return std::nullopt;
    // A release that wandered off the control cancels the press, and an
    // interface that clicks at you for a cancelled press feels broken.
    if (!releaseIsInside(widget, mouse) || !widget->isEnabled())
        return std::nullopt;
    const QPoint pos = mouse->position().toPoint();

    if (auto *button = qobject_cast<QAbstractButton *>(widget))
        return buttonCue(button);
    if (auto *tabs = qobject_cast<QTabBar *>(widget)) {
        const int tab = tabs->tabAt(pos);
        if (tab < 0 || !tabs->isTabEnabled(tab))
            return std::nullopt;
        return Cue::Navigate;
    }
    if (qobject_cast<QComboBox *>(widget))
        return Cue::Open;
    if (auto *menu = qobject_cast<QMenu *>(widget)) {
        QAction *action = menu->actionAt(pos);
        // A submenu announces itself when it opens; a separator is not a choice.
        if (!action || !action->isEnabled() || action->isSeparator() ||
            action->menu())
            return std::nullopt;
        if (action->isCheckable()) {
            const QActionGroup *group = action->actionGroup();
            if (group && group->isExclusive())
                return Cue::Select;
            return action->isChecked() ? Cue::ToggleOff : Cue::ToggleOn;
        }
        return cueForLabel(action->text(), Cue::Click);
    }
    // Scroll bars are how you look around, not something you do; a slider is
    // a setting being chosen.
    if (qobject_cast<QScrollBar *>(widget))
        return std::nullopt;
    if (qobject_cast<QAbstractSlider *>(widget))
        return Cue::Select;
    // The step arrows; the text part is a child line edit and never gets here.
    if (qobject_cast<QAbstractSpinBox *>(widget))
        return Cue::Click;
    // An item view delivers its mouse events to its viewport, whose parent
    // is the view: this is what makes picking a row in the agent list, the
    // repository tree or a settings table sound like picking a row. Blank
    // space under the last row picks nothing, so it says nothing.
    if (auto *view = qobject_cast<QAbstractItemView *>(widget->parentWidget())) {
        if (widget != view->viewport())
            return std::nullopt;
        if (auto *header = qobject_cast<QHeaderView *>(view))
            return header->logicalIndexAt(pos) >= 0 ? std::optional(Cue::Select)
                                                    : std::nullopt;
        return view->indexAt(pos).isValid() ? std::optional(Cue::Select)
                                            : std::nullopt;
    }
#ifndef QT_NO_CURSOR
    // Everything else that is clickable says so with the pointing hand: the
    // custom-painted cards and rows, a link in a label or a text browser
    // (both show the hand only while it is over the link). The cursor is
    // inherited, so the label inside a clickable card counts as the card.
    if (widget->cursor().shape() == Qt::PointingHandCursor)
        return Cue::Click;
#endif
    return std::nullopt;
}

bool isActivationKey(int key)
{
    return key == Qt::Key_Return || key == Qt::Key_Enter;
}

// A key that puts text into, or takes it out of, a field that asked to tick
// (kTypingSoundProperty). Return is not typing: it sends, and the send says so
// by hand. Shortcuts - Ctrl+C, Ctrl+V - carry control characters or no text at
// all, so they stay silent; AltGr letters are printable and tick.
bool isTypingKey(const QWidget *widget, const QKeyEvent *key)
{
    if (!widget->property(kTypingSoundProperty).toBool() ||
        widget->property("readOnly").toBool())
        return false;
    if (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete)
        return true;
    const QString text = key->text();
    return !text.isEmpty() && text.at(0).isPrint();
}

std::optional<Cue> keyCue(QWidget *widget, QKeyEvent *key, bool press)
{
    if (key->isAutoRepeat() || !widget->isEnabled())
        return std::nullopt;
    if (press && isTypingKey(widget, key))
        return Cue::Keystroke;
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
        // Driving the app from the keyboard sounds the same as driving it with
        // the mouse. Space clicks on release, as the mouse does; Return and
        // Enter click on the press.
        if (key->key() == Qt::Key_Space)
            return !press && button->isDown() ? std::optional(buttonCue(button))
                                              : std::nullopt;
        if (press && isActivationKey(key->key()))
            return buttonCue(button);
        return std::nullopt;
    }
    if (!press)
        return std::nullopt;
    if (auto *line = qobject_cast<QLineEdit *>(widget)) {
        // Return in a field commits it: a search run, a name saved. The fields
        // that send something (the chat box) say Send by hand, which wins.
        if (isActivationKey(key->key()) && !line->isReadOnly() &&
            !line->text().isEmpty())
            return Cue::Click;
        return std::nullopt;
    }
    if (qobject_cast<QAbstractItemView *>(widget)) {
        // Arrowing through a list, one row per press; holding the key repeats,
        // and repeats are dropped above, so a held arrow is one sound.
        switch (key->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
        case Qt::Key_Home:
        case Qt::Key_End:
            return Cue::Select;
        default:
            break;
        }
        return isActivationKey(key->key()) ? std::optional(Cue::Click)
                                           : std::nullopt;
    }
    return std::nullopt;
}

std::optional<Cue> windowCue(QWidget *widget, bool shown)
{
    // Only two kinds of window announce themselves: a dialog and a menu.
    // Filtering on those rather than on "is a window" is what keeps the app
    // from playing a chord at startup as its panels are first shown.
    if (!widget->isWindow())
        return std::nullopt;
    // A combo box opens its list on the press, and the release then belongs
    // to the list, so the list appearing is the combo's sound. Its going away
    // is silent: that is either the row just picked, which said Select, or
    // nothing picked at all.
    if (widget->windowType() == Qt::Popup &&
        qobject_cast<QComboBox *>(widget->parentWidget()))
        return shown ? std::optional(Cue::Open) : std::nullopt;
    if (!shown)
        return qobject_cast<QDialog *>(widget) || qobject_cast<QMenu *>(widget)
                   ? std::optional(Cue::Close)
                   : std::nullopt;
    // A message box says what kind of news it is before a word is read.
    if (auto *box = qobject_cast<QMessageBox *>(widget)) {
        switch (box->icon()) {
        case QMessageBox::Critical:
            return Cue::Error;
        case QMessageBox::Warning:
            return Cue::Warn;
        default:
            return Cue::Open;
        }
    }
    if (qobject_cast<QDialog *>(widget) || qobject_cast<QMenu *>(widget))
        return Cue::Open;
    return std::nullopt;
}

bool inQuietArea(const QWidget *widget)
{
    for (; widget; widget = widget->parentWidget()) {
        if (widget->property(kQuietSoundProperty).toBool())
            return true;
    }
    return false;
}

} // namespace

std::optional<Cue> UiSounds::cueForEvent(QObject *watched, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || inQuietArea(widget))
        return std::nullopt;
    switch (event->type()) {
    case QEvent::MouseButtonRelease:
        return releaseCue(widget, static_cast<QMouseEvent *>(event));
    case QEvent::KeyPress:
        return keyCue(widget, static_cast<QKeyEvent *>(event), true);
    case QEvent::KeyRelease:
        return keyCue(widget, static_cast<QKeyEvent *>(event), false);
    case QEvent::Show:
        return windowCue(widget, true);
    case QEvent::Hide:
        return windowCue(widget, false);
    default:
        return std::nullopt;
    }
}

bool UiSounds::eventFilter(QObject *watched, QEvent *event)
{
    // This runs for every event delivered anywhere in the application, so the
    // first thing it does is reject the overwhelming majority on one switch.
    // Nothing here ever consumes an event: it only listens.
    switch (event->type()) {
    case QEvent::MouseButtonRelease:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::Show:
    case QEvent::Hide:
        break;
    default:
        return false;
    }
    if (!m_enabled)
        return false;
    if (const std::optional<Cue> cue = cueForEvent(watched, event))
        holdCue(*cue);
    return false;
}

} // namespace forkmesh::sonify

#include "BackgroundSettings.h"

#include "AppPaths.h"
#include "BackgroundMode.h"
#include "BackgroundRunnerBadge.h"

#include <QCheckBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QProcess>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {

struct Outcome {
    forkmesh::background::StartStatus status;
    QString error;
};

QSet<BackgroundSettings *> &boxes()
{
    static QSet<BackgroundSettings *> all;
    return all;
}

}

BackgroundSettings::BackgroundSettings(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("backgroundSettings"));
    boxes().insert(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_keepRunning = new QCheckBox(QStringLiteral("Keep ForkMesh running without a window after I close it"));
    m_keepRunning->setToolTip(QStringLiteral(
        "When you close the window or log out, ForkMesh starts again without one, so your "
        "phone can list, start and prompt agents and use everything else the desktop does. "
        "Turns on lingering for your user so it keeps running after you log out. A tray icon "
        "shows while it runs. Opening ForkMesh takes over from it."));
    m_startAtBoot = new QCheckBox(QStringLiteral("Start ForkMesh without a window when the computer starts"));
    m_startAtBoot->setToolTip(QStringLiteral(
        "Registers ForkMesh with the system as a systemd user unit with lingering turned on, so "
        "it starts at boot before anyone logs in and restarts if it crashes. Without a user "
        "systemd it starts when you log in. Asks for administrator authentication once to "
        "keep this computer awake while ForkMesh runs before login."));
    m_noSleep = new QCheckBox(QStringLiteral("Never let this computer go to sleep"));
    m_noSleep->setToolTip(QStringLiteral(
        "Turns off suspend and hibernate for the whole computer (systemctl mask), so the login "
        "screen, which on GNOME suspends after 15 minutes, cannot put it to sleep while "
        "ForkMesh runs without a window. Asks for an administrator password. While ForkMesh "
        "runs without a window it also blocks sleep on its own whenever the system allows it."));
    m_allowRestart = new QCheckBox(QStringLiteral("Let my phones restart this computer"));
    m_allowRestart->setToolTip(QStringLiteral(
        "Restart computer on a paired phone works on its own only while your login is the "
        "one on the screen. This lets your account restart the computer when you are logged "
        "out, switched away to another user, or another user is logged in too; their "
        "unsaved work is lost. Applies to every program you run, agents included. Asks for "
        "an administrator password."));
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("modeHint"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_keepRunning);
    layout->addWidget(m_startAtBoot);
    layout->addWidget(m_noSleep);
    layout->addWidget(m_allowRestart);
    layout->addWidget(m_status);

    if (!forkmesh::background::supported()) {
        m_keepRunning->setEnabled(false);
        m_startAtBoot->setEnabled(false);
        m_noSleep->setEnabled(false);
        m_allowRestart->setEnabled(false);
#if defined(Q_OS_LINUX)
        m_status->setText(QStringLiteral(
            "Running without a window is available only for a project's own state folder."));
#else
        m_status->setText(QStringLiteral("Running without a window is available on Linux."));
#endif
        return;
    }

    m_keepRunning->setChecked(forkmesh::background::keepRunning());
    connect(m_keepRunning, &QCheckBox::toggled, this, [this](bool on) { setKeepRunning(on); });
    connect(m_startAtBoot, &QCheckBox::toggled, this, [this](bool on) { setStartAtBoot(on); });
    connect(m_noSleep, &QCheckBox::toggled, this, [this](bool on) { setSleepDisabled(on); });
    connect(m_allowRestart, &QCheckBox::toggled, this, [this](bool on) { setRestartAllowed(on); });
    refresh();
}

BackgroundSettings::~BackgroundSettings()
{
    boxes().remove(this);
}

void BackgroundSettings::refreshAll()
{
    for (BackgroundSettings *box : std::as_const(boxes())) {
        if (forkmesh::background::supported() && !box->m_busy)
            box->refresh();
    }
}

void BackgroundSettings::refresh()
{
    m_busy = true;
    m_startAtBoot->setEnabled(false);
    m_noSleep->setEnabled(false);
    m_allowRestart->setEnabled(false);
    const QString project = forkmesh::paths::projectSlug();
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        applyStatus(outcome.status, outcome.error);
    });
    watcher->setFuture(QtConcurrent::run([project] {
        return Outcome{forkmesh::background::StartManager::system().status(project), QString()};
    }));
}

void BackgroundSettings::setKeepRunning(bool on)
{
    forkmesh::background::setKeepRunning(on);
    const QString project = forkmesh::paths::projectSlug();
    const QString binary = forkmesh::background::desktopBinaryPath();
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        if (!m_busy)
            applyStatus(outcome.status, outcome.error);
    });
    watcher->setFuture(QtConcurrent::run([project, binary, on] {
        const forkmesh::background::StartManager manager =
            forkmesh::background::StartManager::system();
        Outcome outcome;
        manager.syncLogoutHandOver(on, binary, project, &outcome.error);
        outcome.status = manager.status(project);
        return outcome;
    }));
}

void BackgroundSettings::setStartAtBoot(bool on)
{
    if (m_busy)
        return;
    m_busy = true;
    m_startAtBoot->setEnabled(false);
    m_status->setText(on ? QStringLiteral("Registering ForkMesh with the system…")
                         : QStringLiteral("Removing ForkMesh from the system start…"));
    if (on && !m_keepRunning->isChecked())
        m_keepRunning->setChecked(true);
    const QString project = forkmesh::paths::projectSlug();
    const QString binary = forkmesh::background::desktopBinaryPath();
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        applyStatus(outcome.status, outcome.error);
    });
    watcher->setFuture(QtConcurrent::run([project, binary, on] {
        const forkmesh::background::StartManager manager =
            forkmesh::background::StartManager::system();
        Outcome outcome;
        if (on)
            manager.enable(binary, project, &outcome.error);
        else
            manager.disable(project, &outcome.error);
        outcome.status = manager.status(project);
        return outcome;
    }));
}

void BackgroundSettings::setSleepDisabled(bool on)
{
    if (m_busy)
        return;
    const QStringList command = forkmesh::background::systemSleepArguments(on);
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        m_sleepError = QStringLiteral("pkexec is not installed. Run in a terminal: sudo %1")
                           .arg(command.join(QLatin1Char(' ')));
        refresh();
        return;
    }
    m_busy = true;
    m_startAtBoot->setEnabled(false);
    m_noSleep->setEnabled(false);
    m_allowRestart->setEnabled(false);
    m_status->setText(on ? QStringLiteral("Turning sleep off for this computer…")
                         : QStringLiteral("Letting this computer sleep again…"));
    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this, [this, process, command](int code, QProcess::ExitStatus exit) {
        process->deleteLater();
        m_sleepError.clear();
        if (exit != QProcess::NormalExit || code != 0) {
            const QString output = QString::fromUtf8(process->readAll()).trimmed();
            m_sleepError = QStringLiteral("%1. Run in a terminal: sudo %2")
                               .arg(output.isEmpty() ? QStringLiteral("not approved") : output,
                                    command.join(QLatin1Char(' ')));
        }
        refresh();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, command](QProcess::ProcessError failure) {
        if (failure != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_sleepError = QStringLiteral("pkexec could not start. Run in a terminal: sudo %1")
                           .arg(command.join(QLatin1Char(' ')));
        refresh();
    });
    process->start(pkexec, command);
}

void BackgroundSettings::setRestartAllowed(bool on)
{
    if (m_busy)
        return;
    m_busy = true;
    m_startAtBoot->setEnabled(false);
    m_noSleep->setEnabled(false);
    m_allowRestart->setEnabled(false);
    m_status->setText(on ? QStringLiteral("Letting your phones restart this computer…")
                         : QStringLiteral("Taking back the restart permission…"));
    const QString project = forkmesh::paths::projectSlug();
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        applyStatus(outcome.status, outcome.error);
    });
    watcher->setFuture(QtConcurrent::run([project, on] {
        const forkmesh::background::StartManager manager =
            forkmesh::background::StartManager::system();
        Outcome outcome;
        manager.allowRestart(on, &outcome.error);
        outcome.status = manager.status(project);
        return outcome;
    }));
}

void BackgroundSettings::applyStatus(const forkmesh::background::StartStatus &status,
                                     const QString &error)
{
    m_busy = false;
    {
        const QSignalBlocker block(m_startAtBoot);
        m_startAtBoot->setChecked(status.enabled);
    }
    m_startAtBoot->setEnabled(true);
    {
        const QSignalBlocker block(m_noSleep);
        m_noSleep->setChecked(status.sleepDisabled);
    }
    m_noSleep->setEnabled(true);
    {
        const QSignalBlocker block(m_allowRestart);
        m_allowRestart->setChecked(status.restartAllowed);
    }
    m_allowRestart->setEnabled(true);
    QStringList lines;
    if (status.enabled) {
        if (status.mechanism.startsWith(QLatin1String("systemd")))
            lines << (status.atBoot
                          ? QStringLiteral("Starts when the computer starts, as the %1 %2.")
                          : QStringLiteral("Starts when you log in, as the %1 %2. To start it "
                                           "before anyone logs in, run: loginctl enable-linger"))
                         .arg(status.mechanism, status.location);
        else
            lines << QStringLiteral("Starts when you log in, from the %1 %2.")
                         .arg(status.mechanism, status.location);
        if (!status.binary.isEmpty() && status.binary != forkmesh::background::desktopBinaryPath())
            lines << QStringLiteral("It starts %1; turn this off and on to use this copy.")
                         .arg(status.binary);
    } else {
        lines << QStringLiteral("Not registered. Would be added as a %1 at %2.")
                     .arg(status.mechanism, status.location);
    }
    if (m_keepRunning->isChecked())
        lines << (status.afterLogout
                      ? QStringLiteral("Keeps running after you log out.")
                      : QStringLiteral("Stops when you log out. To keep it running, run: "
                                       "loginctl enable-linger"));
    if (status.sleepDisabled)
        lines << QStringLiteral("This computer never goes to sleep.");
    else if (status.bootSleepProtected && (status.enabled || m_keepRunning->isChecked()))
        lines << QStringLiteral("ForkMesh can keep this computer awake before anyone logs in. "
                                "Sleep is allowed again when ForkMesh stops.");
    else if (status.enabled || m_keepRunning->isChecked())
        lines << QStringLiteral("This computer can still go to sleep when nobody is logged in "
                                "(the login screen suspends it after 15 minutes), and ForkMesh "
                                "stops answering until it wakes. Turn on Never let this "
                                "computer go to sleep to prevent it.");
    if (!m_sleepError.isEmpty())
        lines << QStringLiteral("Could not change sleep: %1").arg(m_sleepError);
    if (!error.isEmpty())
        lines << QStringLiteral("Failed: %1").arg(error);
    m_status->setText(lines.join(QLatin1Char('\n')));
    BackgroundRunnerBadge::refreshAll();
}

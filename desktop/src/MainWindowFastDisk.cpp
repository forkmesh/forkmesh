#include "MainWindow.h"
#include "ClientErrorReports.h"
#include "FastScratchDisk.h"
#include "MainWindowInternal.h"
#include "UsbLinkCheck.h"

#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

namespace {

constexpr char kFastDiskSetting[] = "storage/useTmpDisk";
constexpr char kFastDiskBootDeclinedSetting[] = "storage/tmpBootMountDeclined";
constexpr int kFastDiskPollMs = 60 * 1000;
constexpr qint64 kFastDiskCleanEveryMs = 60LL * 60 * 1000;
constexpr qint64 kFastDiskTempMaxAgeMs = 24LL * 60 * 60 * 1000;

struct FastDiskSnapshot {
    forkmesh::fastdisk::Volume volume;
    forkmesh::fastdisk::Usage usage;
    forkmesh::fastdisk::BootMount boot;
    QVector<forkmesh::usblink::Finding> usb;
    forkmesh::fastdisk::CleanResult cleaned;
    bool cleanedRan = false;
};

bool fastDiskEnabled()
{
#ifdef FORKMESH_WINDOW_TESTS
    return false;
#else
    return QSettings().value(QLatin1String(kFastDiskSetting), true).toBool();
#endif
}

forkmesh::fastdisk::BootMount planFastDiskBootMount(const forkmesh::fastdisk::Volume &volume)
{
    QFile fstab(forkmesh::fastdisk::defaultFstabFile());
    const QByteArray text = fstab.open(QIODevice::ReadOnly) ? fstab.readAll() : QByteArray();
#if defined(Q_OS_UNIX)
    const uint uid = ::getuid();
    const uint gid = ::getgid();
#else
    const uint uid = 0;
    const uint gid = 0;
#endif
    return forkmesh::fastdisk::planBootMount(
        volume, text, forkmesh::fastdisk::defaultBootTarget(qEnvironmentVariable("USER")), uid,
        gid);
}

QString fastDiskBootText(const forkmesh::fastdisk::Volume &volume,
                         const forkmesh::fastdisk::BootMount &boot)
{
    if (!volume.present())
        return QString();
    if (boot.configured)
        return QStringLiteral("It is mounted automatically at startup (/etc/fstab).");
    if (boot.possible())
        return QStringLiteral("It is not mounted at startup yet. Mount at startup adds it to "
                              "/etc/fstab at %1 after asking for your password.")
            .arg(boot.target);
    return QStringLiteral("ForkMesh cannot set it up to mount at startup: its UUID or mount "
                          "folder could not be read.");
}

QString fastDiskStatusText(const forkmesh::fastdisk::Volume &volume,
                           const forkmesh::fastdisk::Usage &usage, bool enabled,
                           const QString &active)
{
    using namespace forkmesh::fastdisk;
    if (!volume.present())
        return QStringLiteral("No disk labelled %1 is connected. Agent scratch, build caches "
                              "and Action sandboxes stay on the system disk.")
            .arg(label());
    if (!volume.mounted())
        return QStringLiteral("A disk labelled %1 (%2) is connected but not mounted. Open it "
                              "once in the file manager, or mount it, and ForkMesh picks it up "
                              "within a minute.")
            .arg(label(), volume.device);
    if (!volume.writable)
        return QStringLiteral("The disk labelled %1 at %2 is read-only, so ForkMesh does not use "
                              "it.")
            .arg(label(), volume.mountPoint);
    const QString space = QStringLiteral("%1 free of %2")
                              .arg(formatBytes(volume.freeBytes), formatBytes(volume.totalBytes));
    const QString data = usage.hasData()
                             ? QStringLiteral("It holds %1.").arg(formatBytes(usage.usedBytes))
                             : QStringLiteral("It is empty.");
    if (!enabled || active.isEmpty())
        return QStringLiteral("The disk labelled %1 is mounted at %2 (%3) but turned off here. %4")
            .arg(label(), volume.mountPoint, space, data);
    return QStringLiteral("In use: %1 on %2 (%3). New agents put their scratch, build output "
                          "and ccache there, and Actions run their sandboxes there. %4")
        .arg(active, volume.device, space, data);
}

}

void MainWindow::initFastScratchDisk()
{
    m_fastDiskVolume = forkmesh::fastdisk::probe();
    const QString active = forkmesh::fastdisk::activate(m_fastDiskVolume, fastDiskEnabled());
    if (!active.isEmpty())
        logSystem(QStringLiteral("TMP disk: using %1 for agent scratch, build caches and "
                                 "Action sandboxes.")
                      .arg(active));
    m_fastDiskTimer = new QTimer(this);
    m_fastDiskTimer->setInterval(kFastDiskPollMs);
    connect(m_fastDiskTimer, &QTimer::timeout, this,
            [this] { refreshFastScratchDisk(false); });
    m_fastDiskTimer->start();
    QTimer::singleShot(0, this, [this] { refreshFastScratchDisk(true); });
}

void MainWindow::refreshFastScratchDisk(bool clean)
{
    if (m_fastDiskRefreshRunning)
        return;
    m_fastDiskRefreshRunning = true;
    const bool enabled = fastDiskEnabled();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool sweep = clean || nowMs - m_fastDiskLastCleanMs >= kFastDiskCleanEveryMs;
    if (sweep)
        m_fastDiskLastCleanMs = nowMs;
    runOffThread<FastDiskSnapshot>(
        [enabled, sweep, nowMs] {
            FastDiskSnapshot snapshot;
            snapshot.volume = forkmesh::fastdisk::probe();
            const QString root = enabled && snapshot.volume.usable()
                                     ? forkmesh::fastdisk::rootOn(snapshot.volume)
                                     : QString();
            if (sweep && !root.isEmpty()) {
                snapshot.cleaned =
                    forkmesh::fastdisk::cleanUnused(root, nowMs, kFastDiskTempMaxAgeMs);
                snapshot.cleanedRan = true;
            }
            snapshot.usage = forkmesh::fastdisk::usage(snapshot.volume);
            snapshot.boot = planFastDiskBootMount(snapshot.volume);
            snapshot.usb = forkmesh::usblink::scan();
            return snapshot;
        },
        [this, enabled](FastDiskSnapshot snapshot) {
            m_fastDiskRefreshRunning = false;
            const QString before = forkmesh::fastdisk::activeRoot();
            m_fastDiskVolume = snapshot.volume;
            m_fastDiskUsage = snapshot.usage;
            m_fastDiskBoot = snapshot.boot;
            const QString after = forkmesh::fastdisk::activate(snapshot.volume, enabled);
            if (before != after)
                logSystem(after.isEmpty()
                              ? QStringLiteral("TMP disk: no longer in use; agent scratch and "
                                               "Action sandboxes are back on the system disk.")
                              : QStringLiteral("TMP disk: using %1 for agent scratch, build "
                                               "caches and Action sandboxes.")
                                    .arg(after));
            if (snapshot.cleanedRan && snapshot.cleaned.removed > 0)
                logSystem(QStringLiteral("TMP disk: removed %1 unused folder%2.")
                              .arg(snapshot.cleaned.removed)
                              .arg(snapshot.cleaned.removed == 1 ? QString()
                                                                 : QStringLiteral("s")));
            if (snapshot.cleanedRan && !snapshot.cleaned.failures.isEmpty())
                logSystem(QStringLiteral("TMP disk: could not remove %1 folder%2: %3")
                              .arg(snapshot.cleaned.failures.size())
                              .arg(snapshot.cleaned.failures.size() == 1 ? QString()
                                                                         : QStringLiteral("s"))
                              .arg(snapshot.cleaned.failures.first()));
            m_usbFindings = snapshot.usb;
            for (const forkmesh::usblink::Finding &finding : snapshot.usb) {
                const QString key =
                    finding.id + QLatin1Char('|') + QString::number(finding.speedMbps);
                if (m_usbWarned.contains(key))
                    continue;
                m_usbWarned.insert(key);
                flashMessage(forkmesh::usblink::message(finding), true, QString(),
                             forkmesh::kLocalOperationNoticeKind);
            }
            updateFastDiskTiles();
            updateFastDiskSettingsPanel();
            if (enabled && m_fastDiskBoot.possible() &&
                !m_fastDiskBootAsked.contains(m_fastDiskBoot.uuid) &&
                QSettings().value(QLatin1String(kFastDiskBootDeclinedSetting)).toString() !=
                    m_fastDiskBoot.uuid)
                setupFastDiskBootMount(true);
        });
}

void MainWindow::setupFastDiskBootMount(bool automatic)
{
    using namespace forkmesh::fastdisk;
    const BootMount plan = m_fastDiskBoot;
    if (!plan.possible() || m_fastDiskBootRunning)
        return;
    m_fastDiskBootAsked.insert(plan.uuid);
    const QString manual = QStringLiteral("echo '%1' | sudo tee -a /etc/fstab && sudo mkdir -p "
                                          "%2 && sudo mount %2")
                               .arg(plan.fstabLine(), plan.target);
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        flashMessage(QStringLiteral("To mount the %1 disk at startup, run in a terminal: %2")
                         .arg(label(), manual),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    m_fastDiskBootRunning = true;
    updateFastDiskSettingsPanel();
    logSystem(QStringLiteral("TMP disk: asking to mount %1 at %2 at startup.")
                  .arg(m_fastDiskVolume.device, plan.target));
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this,
            [this, process, plan, automatic, manual](int exitCode, QProcess::ExitStatus status) {
                process->deleteLater();
                m_fastDiskBootRunning = false;
                if (status == QProcess::NormalExit && exitCode == 0) {
                    QSettings().remove(QLatin1String(kFastDiskBootDeclinedSetting));
                    logSystem(QStringLiteral("TMP disk: added to /etc/fstab, mounts at %1 at "
                                             "startup.")
                                  .arg(plan.target));
                    flashMessage(QStringLiteral("The %1 disk now mounts at %2 at startup.")
                                     .arg(label(), plan.target));
                } else if (status == QProcess::NormalExit && (exitCode == 126 || exitCode == 127)) {
                    if (automatic)
                        QSettings().setValue(QLatin1String(kFastDiskBootDeclinedSetting),
                                             plan.uuid);
                    flashMessage(QStringLiteral("The %1 disk was not set to mount at startup. "
                                                "Settings > TMP disk > Mount at startup asks "
                                                "again.")
                                     .arg(label()),
                                 true, QString(), forkmesh::kLocalOperationNoticeKind);
                } else {
                    const QString errors =
                        QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
                    flashMessage(QStringLiteral("Could not set the %1 disk to mount at startup%2. "
                                                "Run in a terminal: %3")
                                     .arg(label(),
                                          errors.isEmpty() ? QString()
                                                           : QStringLiteral(": ") + errors.right(200),
                                          manual),
                                 true, QString(), forkmesh::kLocalOperationNoticeKind);
                }
                refreshFastScratchDisk(false);
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, manual](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                m_fastDiskBootRunning = false;
                flashMessage(QStringLiteral("Could not launch the privilege helper (pkexec). Run "
                                            "in a terminal: %1")
                                 .arg(manual),
                             true, QString(), forkmesh::kLocalOperationNoticeKind);
                updateFastDiskSettingsPanel();
            });
    process->start(pkexec, bootMountArguments(plan));
}

void MainWindow::updateFastDiskTiles()
{
    using namespace forkmesh::fastdisk;
    const QString active = activeRoot();
    const Volume &volume = m_fastDiskVolume;
    if (auto *tile = static_cast<forkmesh::ui::ResourceSparkline *>(m_debugTmpTile)) {
        tile->setLoading(false);
        if (!active.isEmpty()) {
            tile->setHealthColorsEnabled(true);
            tile->setSamples({0.0}, 100.0, QStringLiteral("ON"));
            tile->setToolTip(QStringLiteral("TMP disk active at %1 (%2 free). Agent scratch, "
                                            "ccache and Action sandboxes go here.")
                                 .arg(volume.mountPoint, formatBytes(volume.freeBytes)));
        } else if (volume.usable()) {
            tile->setHealthColorsEnabled(true);
            tile->setSamples({60.0}, 100.0, QStringLiteral("OFF"));
            tile->setToolTip(QStringLiteral("TMP disk mounted at %1 but turned off in "
                                            "Settings > TMP disk")
                                 .arg(volume.mountPoint));
        } else if (volume.present()) {
            tile->setHealthColorsEnabled(true);
            tile->setSamples({100.0}, 100.0, QStringLiteral("ERR"));
            tile->setToolTip(volume.mounted()
                                 ? QStringLiteral("TMP disk at %1 is read-only")
                                       .arg(volume.mountPoint)
                                 : QStringLiteral("A disk labelled TMP is connected but not "
                                                  "mounted"));
        } else {
            tile->setHealthColorsEnabled(false);
            tile->setSamples({}, 100.0, QStringLiteral("\xE2\x80\x94"));
            tile->setToolTip(QStringLiteral("No disk labelled TMP is connected"));
        }
    }
    if (auto *tile = static_cast<forkmesh::ui::ResourceSparkline *>(m_debugTmpDataTile)) {
        tile->setLoading(false);
        if (!volume.mounted()) {
            tile->setHealthColorsEnabled(false);
            tile->setSamples({}, 100.0, QStringLiteral("\xE2\x80\x94"));
            tile->setToolTip(QStringLiteral("No TMP disk is mounted"));
        } else {
            const double percent = volume.totalBytes > 0 && m_fastDiskUsage.usedBytes >= 0
                                       ? 100.0 * double(m_fastDiskUsage.usedBytes) /
                                             double(volume.totalBytes)
                                       : 0.0;
            tile->setHealthColorsEnabled(true);
            tile->setSamples({percent}, 100.0,
                             m_fastDiskUsage.hasData()
                                 ? formatBytes(m_fastDiskUsage.usedBytes)
                                 : QStringLiteral("EMPTY"));
            tile->setToolTip(m_fastDiskUsage.hasData()
                                 ? QStringLiteral("TMP disk holds %1 (%2% full)")
                                       .arg(formatBytes(m_fastDiskUsage.usedBytes))
                                       .arg(qRound(percent))
                                 : QStringLiteral("TMP disk is empty"));
        }
    }
    if (auto *tile = static_cast<forkmesh::ui::ResourceSparkline *>(m_debugUsbTile)) {
        tile->setLoading(false);
        tile->setHealthColorsEnabled(true);
        if (m_usbFindings.isEmpty()) {
            tile->setSamples({0.0}, 100.0, QStringLiteral("OK"));
            tile->setToolTip(QStringLiteral("Every USB drive is connected at its full speed"));
        } else {
            QStringList lines;
            for (const forkmesh::usblink::Finding &finding : m_usbFindings)
                lines << forkmesh::usblink::message(finding);
            tile->setSamples({100.0}, 100.0, QStringLiteral("SLOW"));
            tile->setToolTip(lines.join(QStringLiteral("\n\n")));
        }
    }
}

QWidget *MainWindow::buildFastDiskTab()
{
    auto *page = new QWidget;
    auto *col = new QVBoxLayout(page);
    col->setContentsMargins(2, 14, 2, 14);
    col->setSpacing(10);

    auto *heading = new QLabel(QStringLiteral("TMP DISK"));
    heading->setObjectName(QStringLiteral("sectionLabel"));
    auto *hint = new QLabel(
        QStringLiteral("Give a spare disk the label TMP and ForkMesh uses it for the heavy, "
                       "throwaway writes: agent scratch folders and build output, the agents' "
                       "ccache, and Action sandboxes and caches. Your projects, settings and "
                       "history stay where they are. Unused folders on it are removed when "
                       "ForkMesh starts and every hour after."));
    hint->setObjectName(QStringLiteral("statusLine"));
    hint->setWordWrap(true);

    auto *enabled = new QCheckBox(
        QStringLiteral("Use a disk labelled TMP for agent scratch, build caches and Action "
                       "sandboxes"));
    m_fastDiskEnabledCheck = enabled;
    enabled->setChecked(fastDiskEnabled());
    connect(enabled, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QLatin1String(kFastDiskSetting), on);
        forkmesh::fastdisk::activate(m_fastDiskVolume, on);
        updateFastDiskTiles();
        updateFastDiskSettingsPanel();
        refreshFastScratchDisk(false);
    });

    auto *status = new QLabel;
    m_fastDiskStatusLabel = status;
    status->setObjectName(QStringLiteral("statusLine"));
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *refresh = new QPushButton(QStringLiteral("Check again"));
    refresh->setObjectName(QStringLiteral("ghostButton"));
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QPushButton::clicked, this, [this] { refreshFastScratchDisk(false); });

    auto *erase = new QPushButton(QStringLiteral("Erase TMP disk\xE2\x80\xA6"));
    m_fastDiskEraseButton = erase;
    erase->setObjectName(QStringLiteral("dangerButton"));
    erase->setCursor(Qt::PointingHandCursor);
    erase->setToolTip(QStringLiteral("Delete everything on the disk labelled TMP"));
    connect(erase, &QPushButton::clicked, this, &MainWindow::eraseFastScratchDisk);

    auto *boot = new QPushButton(QStringLiteral("Mount at startup\xE2\x80\xA6"));
    m_fastDiskBootButton = boot;
    boot->setObjectName(QStringLiteral("ghostButton"));
    boot->setCursor(Qt::PointingHandCursor);
    boot->setToolTip(QStringLiteral("Add the disk labelled TMP to /etc/fstab so it mounts when "
                                    "the computer starts"));
    connect(boot, &QPushButton::clicked, this, [this] { setupFastDiskBootMount(false); });

    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(refresh);
    buttons->addWidget(boot);
    buttons->addWidget(erase);
    buttons->addStretch(1);

    col->addWidget(heading);
    col->addWidget(hint);
    col->addWidget(enabled);
    col->addWidget(status);
    col->addLayout(buttons);
    col->addStretch(1);
    updateFastDiskSettingsPanel();
    return page;
}

void MainWindow::updateFastDiskSettingsPanel()
{
    if (m_fastDiskStatusLabel) {
        const QString boot = fastDiskBootText(m_fastDiskVolume, m_fastDiskBoot);
        m_fastDiskStatusLabel->setText(
            fastDiskStatusText(m_fastDiskVolume, m_fastDiskUsage, fastDiskEnabled(),
                               forkmesh::fastdisk::activeRoot()) +
            (boot.isEmpty() ? QString() : QStringLiteral(" ") + boot));
    }
    if (m_fastDiskBootButton) {
        m_fastDiskBootButton->setVisible(m_fastDiskBoot.possible());
        m_fastDiskBootButton->setEnabled(!m_fastDiskBootRunning);
    }
    if (m_fastDiskEraseButton)
        m_fastDiskEraseButton->setEnabled(m_fastDiskVolume.usable());
}

void MainWindow::eraseFastScratchDisk()
{
    using namespace forkmesh::fastdisk;
    const Volume volume = probe();
    if (!volume.usable()) {
        flashMessage(QStringLiteral("There is no mounted, writable disk labelled %1 to erase.")
                         .arg(label()),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        updateFastDiskSettingsPanel();
        return;
    }
    const Usage held = usage(volume);
    const QString question =
        QStringLiteral("Erase everything on the disk labelled %1?\n\n%2, mounted at %3, holds "
                       "%4.\n\nAgent scratch folders, build output, ccache and Action sandboxes "
                       "on it are deleted, along with any other files on that disk. Agents and "
                       "Actions running now lose their scratch files. This cannot be undone.")
            .arg(label(), volume.device, volume.mountPoint,
                 held.hasData() ? formatBytes(held.usedBytes) : QStringLiteral("nothing"));
    if (QMessageBox::question(this, QStringLiteral("Erase TMP disk"), question,
                              QMessageBox::Yes | QMessageBox::Cancel,
                              QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (m_fastDiskEraseButton)
        m_fastDiskEraseButton->setEnabled(false);
    runOffThread<EraseResult>([volume] { return eraseAll(volume); },
                              [this](EraseResult result) {
                                  activate(probe(), fastDiskEnabled());
                                  if (result.ok) {
                                      logSystem(QStringLiteral("TMP disk: erased %1 item%2.")
                                                    .arg(result.removed)
                                                    .arg(result.removed == 1
                                                             ? QString()
                                                             : QStringLiteral("s")));
                                      flashMessage(QStringLiteral("Erased the TMP disk."));
                                  } else {
                                      flashMessage(QStringLiteral("Could not erase the TMP "
                                                                  "disk: ") +
                                                       result.error,
                                                   true, QString(),
                                                   forkmesh::kLocalOperationNoticeKind);
                                  }
                                  refreshFastScratchDisk(false);
                              });
}

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace forkmesh::background {

inline constexpr char kKeepRunningSetting[] = "background/keepRunning";
inline constexpr int kLockWaitMs = 60000;
inline constexpr int kLogoutWaitMs = 45000;

struct CommandResult {
    bool ok = false;
    QByteArray output;
};

using CommandRunner =
    std::function<CommandResult(const QString &program, const QStringList &arguments)>;

struct StartStatus {
    bool enabled = false;
    QString mechanism;
    QString location;
    QString binary;
    bool atBoot = false;
    bool afterLogout = false;
    bool sleepDisabled = false;
};

struct LegacyEngine {
    bool found = false;
    bool startedAtLogin = false;
    QStringList steps;
};

class StartManager
{
public:
    QString home;
    QString configHome;
    qint64 uid = 0;
    CommandRunner run;

    static StartManager system();

    StartStatus status(const QString &project) const;
    bool enable(const QString &binary, const QString &project, QString *error = nullptr) const;
    bool disable(const QString &project, QString *error = nullptr) const;
    bool unitInstalled(const QString &project) const;
    bool refreshUnit(const QString &binary, const QString &project) const;
    bool sleepDisabled() const;
    bool keepAcrossLogout(const QString &binary, const QString &project,
                          QString *error = nullptr) const;
    void dropLogoutHandOver(const QString &project) const;
    bool syncLogoutHandOver(bool keep, const QString &binary, const QString &project,
                            QString *error = nullptr) const;
    bool shuttingDown() const;
    bool hasSystemd() const;
    bool lingering() const;
    QString systemdUnitPath(const QString &project) const;
    QString logoutUnitPath(const QString &project) const;
    QString headlessEntryPath(const QString &project) const;
    QString trayEntryPath(const QString &project) const;
    QString lingerMarkerPath() const;
    LegacyEngine removeLegacyEngine(const QString &project) const;

private:
    CommandResult call(const QString &program, const QStringList &arguments) const;
    void enableLinger() const;
    void releaseLinger() const;
};

QString label(const QString &project);
QString unitName(const QString &project);
QString trayLabel(const QString &project);
QStringList headlessArguments(const QString &project);
QStringList trayArguments(const QString &project);
QString logoutUnitName(const QString &project);
QStringList logoutArguments(const QString &project);
QStringList serviceVariables();
QStringList sleepTargets();
QStringList systemSleepArguments(bool disable);
QString desktopBinaryPath();

QString systemdUnit(const QString &binary, const QString &project);
QString headlessDesktopEntry(const QString &binary, const QString &project);
QString trayDesktopEntry(const QString &binary, const QString &project);
QString logoutUnit(const QString &binary, const QString &project);
QString execStartBinary(const QString &unit);
QString execStopBinary(const QString &unit);
QString desktopExecBinary(const QString &entry);

bool supported();
bool keepRunning();
void setKeepRunning(bool on);

void noteStopSignal();
void noteSessionEnding();
bool sessionEnding();
bool handOverAfterWindowClose(bool restarting);
bool startHeadless();
bool launchTray();
void migrateLegacyEngine();
void syncLogoutHandOver();
void refreshStartUnit();
QString handOverAtLogout();

} // namespace forkmesh::background

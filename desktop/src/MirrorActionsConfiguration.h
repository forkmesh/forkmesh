#pragma once

#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>

namespace forkmesh::mirror_actions {

QJsonObject applyConfiguration(const QJsonObject &request,
                               QSettings &settings);

bool recoverPendingConfiguration(QSettings &settings,
                                 QString *errorCode = nullptr);

QString configurationRecoveryJournalPath(const QSettings &settings);
QString configurationRecoveryLockPath(const QSettings &settings);

// Startup recovery is never fatal: a failure suspends mirror Actions for this
// process (see actionsSuspendedByRecoveryFailure) and the app opens anyway.
struct StartupRecoveryStatus {
    bool attempted = false;
    bool ok = true;
    QString errorCode;
    QString lockPath;
    QString journalPath;
};
StartupRecoveryStatus recoverAtStartup(QSettings &settings);
StartupRecoveryStatus startupRecoveryStatus();
bool actionsSuspendedByRecoveryFailure();

// "Reset Actions configuration": removes exactly the transaction lock and
// journal files by name (and their pre-fix Windows location), nothing else.
// Refuses with "configuration_busy" while a live helper holds the lock.
// Returns the paths removed.
QStringList resetConfigurationRecoveryFiles(const QSettings &settings,
                                            QString *errorCode = nullptr);

namespace detail {
// False for a Windows registry key name, which QFileInfo calls absolute.
bool settingsNameIsFilePath(const QString &fileName, QSettings::Format format);
QString transactionDirectoryFor(const QString &settingsFileName,
                                QSettings::Format format,
                                const QString &dataRoot);
void setConfigurationLockTimeoutMsForTesting(int timeoutMs);
void resetStartupRecoveryStatusForTesting();
} // namespace detail

int runConfigurationStdin(int argc, char *argv[]);

inline constexpr auto kEnabledSetting = "actions/mirrorEnabled";
inline constexpr auto kGenerationSetting = "actions/mirrorConfigGeneration";
inline constexpr auto kNodeSetting = "actions/mirrorNode";
inline constexpr auto kSummaryPathSetting = "actions/mirrorSummaryPath";

} // namespace forkmesh::mirror_actions

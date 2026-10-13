#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

// The "fleet." local control family (WS16; docs/native/DESKTOP_PARITY.md
// section 19): host operations and the Vultr fleet beyond hosts.*. This is the policy half, kept free of widgets so the
// tests can drive it; the requests are answered in
// MainWindowLocalControlFleet.cpp. Gate 2 creates it empty; WS16 owns it.
namespace forkmesh::localcontrol::fleet {

// The method prefix this family answers.
QString familyPrefix();

QStringList mutatingActions();
bool isMutatingAction(const QString &action);

QString subjectFor(const QString &action, const QJsonObject &params,
                   QString *error = nullptr);

QString typedConfirmation(const QString &action, const QString &subject);
bool typedConfirmationMatches(const QString &typed, const QString &required);

QString actionSummary(const QString &action, const QString &subject);

QStringList deployModes();

bool validHostAddress(const QString &address);
bool validSshUser(const QString &user);
bool validLinkCode(const QString &code);
bool validRepoKey(const QString &key);
bool protectedNode(const QString &node);

QJsonObject costSummary(const QJsonArray &hosts);

QStringList provisionStageNames();
QJsonObject provisionStatus(bool active, const QString &node, int stage,
                            const QString &state, const QString &detail,
                            const QString &message, const QString &gap,
                            const QStringList &stageDetails, bool resumable);

QStringList tailLines(const QByteArray &tail, int maxLines, int maxLineChars);
QString redactSecrets(const QString &line);

QJsonArray gatewayRows(const QStringList &records, qint64 nowMs,
                       qint64 retiredAfterMs, int maxRows = 50);

QString plainText(const QString &html, int maxChars = 400);

bool validRemotePath(const QString &path);
QString diskUsageCommand(const QString &path);
QJsonArray parseDiskUsage(const QString &output, const QString &path,
                          int maxRows = 60);

} // namespace forkmesh::localcontrol::fleet

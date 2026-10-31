#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

struct ClaudePeerSession {
    qint64 pid = 0;
    QString sessionId;
    QString cwd;
    QString name;
    QString status;
    QString entrypoint;
    QString socketPath;
    QString configDir;
};

struct ClaudeInboundPolicy {
    QString policy;
    QString cause;
};

struct ClaudePeerMessage {
    QString text;
    QString fromName;
    QString fromMode;
    QString from;
    QString msgId;
    QString priority = QStringLiteral("next");
};

namespace ClaudePeerMessaging {

inline constexpr int kPeerProtocol = 1;

QString forkMeshSenderName();
QString steerLead();
QString desktopSteerLead();
QString steerBody(const QString &text);
QString steerText(const QString &body);
QStringList configDirs(const QString &home, const QString &accountsRoot, const QStringList &extra);
QString procStart(qint64 pid);
bool isSocket(const QString &path);
QList<ClaudePeerSession> readSessions(const QString &configDir);
QList<ClaudePeerSession> liveSessions(const QStringList &configDirs);
const ClaudePeerSession *find(const QList<ClaudePeerSession> &sessions,
                              const QList<qint64> &pids, const QString &sessionId);
QString keyPath(const QString &configDir, qint64 pid, const QString &socketPath);
QString peerToken(const QString &keyPath);
QString permissionClass(const QByteArrayList &argv, const QString &transcriptPath);
QString escapeBody(const QString &text);
QString envelope(const ClaudePeerMessage &message);
QByteArray jsonString(const QString &value);
QByteArray authLine(const QString &token);
QByteArray userLine(const ClaudePeerMessage &message);
QByteArray frame(const QString &token, const ClaudePeerMessage &message);
bool send(const QString &socketPath, qint64 expectPid, const QByteArray &data, QString *error);
QString inboundSetting(const QByteArray &settingsJson);
QString inboundSettingFile(const QString &path);
QString flagInboundSetting(const QByteArrayList &argv);
QString managedSettingsPath();
QString gitRoot(const QString &cwd);
ClaudeInboundPolicy inboundPolicy(const QString &managed, const QString &flag, const QString &user,
                                  const QString &local, const QString &project,
                                  const QString &receiverClass);
ClaudeInboundPolicy predictInbound(const ClaudePeerSession &session, const QByteArrayList &argv,
                                   const QString &receiverClass, const QString &managedPath);
bool deliver(const ClaudePeerSession &session, const QString &text, QString *error);
bool deliverFromDesktop(const ClaudePeerSession &session, const QString &text, QString *error);

} // namespace ClaudePeerMessaging

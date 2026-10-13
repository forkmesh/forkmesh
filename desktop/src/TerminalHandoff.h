#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>

#include <deque>
#include <functional>

struct TerminalHandoffSession {
    qint64 pid = 0;
    QString start;
    qint64 child = 0;
    QString cli;
    QString cwd;
    QString socketPath;
    bool remote = false;
    qint64 startedMs = 0;
    QString tty;
};

struct TerminalHandoffReply {
    bool ok = false;
    QString code;
    QString detail;
};

namespace TerminalHandoff {

inline constexpr int kProtocolVersion = 1;
inline constexpr int kMaxTextChars = 8000;
inline constexpr int kMaxTypedPerWindow = 10;
inline constexpr qint64 kRateWindowMs = 60 * 1000;
inline constexpr qint64 kMinGapMs = 1000;

QString runtimeDir();
QList<TerminalHandoffSession> readSessions(const QString &dir);
QList<TerminalHandoffSession> liveSessions(const QString &dir);
qint64 parentPid(qint64 pid);
const TerminalHandoffSession *find(const QList<TerminalHandoffSession> &sessions,
                                   const QList<qint64> &pids,
                                   const std::function<qint64(qint64)> &parentOf = parentPid);
QString sanitizeText(const QString &text);
QString sanitizeLabel(const QString &label);
QString interruptKey(const QString &cli);
QByteArray requestFrame(const QString &verb, const QString &from, const QByteArray &payload);
TerminalHandoffReply parseReply(const QByteArray &line);
TerminalHandoffReply exchange(const TerminalHandoffSession &session, const QByteArray &frame);
TerminalHandoffReply type(const TerminalHandoffSession &session, const QString &from,
                          const QString &text);
TerminalHandoffReply key(const TerminalHandoffSession &session, const QString &from,
                         const QString &key);
QString errorCode(const TerminalHandoffReply &reply);
QString launchCommand(const QString &cli);
QString aliasLine(const QString &cli);

class TypingGate
{
public:
    QString check(const QString &deviceKey, int chars, qint64 nowMs);

private:
    QHash<QString, std::deque<qint64>> m_times;
};

QString admit(TypingGate &gate, bool paired, const QString &deviceKey, int chars, qint64 nowMs);

} // namespace TerminalHandoff

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <memory>

class AgentLoginPty;

namespace forkmesh::term {
class PasteModeTracker;
}

namespace forkmesh::localcontrol::terminals {

QString familyPrefix();
QString capability();
QString resumeCapability();
QString accountsCapability();

inline constexpr int kMaxTerminals = 6;
inline constexpr int kReplayBytes = 512 * 1024;
inline constexpr int kReplayLineSearchBytes = 8 * 1024;
inline constexpr int kMaxInputBytes = 16 * 1024;
inline constexpr int kMaxEventBytes = 16 * 1024;
inline constexpr int kMaxPendingBytes = 256 * 1024;
inline constexpr int kEnterDelayMs = 120;
inline constexpr int kIdChars = 32;

QString hostShellProvider();
QString cliProgram(const QString &provider);
QString cliLabel(const QString &provider);
bool validMode(const QString &mode);
QStringList cliArguments(const QString &provider, const QString &mode);
QString accountEnvVariable(const QString &provider);
bool validTerminalId(const QString &id);
QString newTerminalId();
int clampCols(int cols);
int clampRows(int rows);

void appendReplay(QByteArray &replay, const QByteArray &chunk, int cap = kReplayBytes);
QByteArray decodeInput(const QJsonValue &data, QString *error);

struct Outbox {
    QByteArray pending;
    bool reset = false;
    qint64 sentAtMs = 0;
};

void queueOutput(Outbox &box, const QByteArray &chunk, int maxPending = kMaxPendingBytes);
bool hasOutput(const Outbox &box);
QJsonObject takeEvent(Outbox &box, const QString &terminal, const QByteArray &replay,
                      int maxBytes = kMaxEventBytes);

struct Terminal {
    QString id;
    QString provider;
    QString cli;
    QString owner;
    QString name;
    QString cwd;
    QString device;
    QString mode;
    QString account;
    QString host;
    qint64 startedAtMs = 0;
    qint64 pid = 0;
    int session = 0;
    int cols = 80;
    int rows = 24;
    AgentLoginPty *pty = nullptr;
    std::shared_ptr<forkmesh::term::PasteModeTracker> paste;
    QByteArray replay;
    QHash<int, Outbox> viewers;
};

QString terminalLabel(const Terminal &terminal);
QJsonObject terminalJson(const Terminal &terminal);
QJsonObject outsideRow(const Terminal &terminal, int id);

} // namespace forkmesh::localcontrol::terminals

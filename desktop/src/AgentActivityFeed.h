#pragma once

#include "AgentSonifier.h"

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

namespace forkmesh::control {
struct CloudflareTailEvent;
}

namespace forkmesh::agentfeed {

enum class RunState { Idle, Running, Waiting, Done, Failed };

struct Agent {
    int id = 0;
    QString name;
    QString task;
    QString prompt;
    QString lastReply;
    RunState state = RunState::Idle;
    int turns = 0;
    qint64 tokens = 0;
    QString faceKey;
    QIcon face;
    bool listening = false;
    sonify::Voice voice;
    int files = -1;
    int linesAdded = -1;
    int linesRemoved = -1;
    bool conflicted = false;
    bool merged = false;
    bool stopped = false;
};

struct ProcessRoot {
    int sessionId = 0;
    qint64 pid = 0;
    bool withDescendants = true;
};

struct ExternalRun {
    int sessionId = 0;
    QString uuid;
    QString cwd;
};

struct RelayClient {
    QString id;
    QString name;
    QString platform;
    QString accountKind;
    QString nodeName;
    QString version;
};

struct LogStyle {
    QString badge;
    QString accent;
    QString message;
};

struct TaskEvent {
    quint64 id = 0;
    QString word;
    QString detail;
    int sessionId = 0;
    bool started = false;
    bool failed = false;
};

using CloudflareReply = std::function<void(QJsonObject, QString)>;

struct Host {
    std::function<QList<Agent>()> agents;
    std::function<bool(int, Agent *)> agent;
    std::function<bool(int)> isExternal;
    std::function<void(QList<ProcessRoot> *, QList<ExternalRun> *)> processRoots;
    std::function<void(int)> showAgent;
    std::function<void()> showLogs;
    std::function<bool(int)> listening;
    std::function<void(int)> toggleListening;
    std::function<bool(int)> listen;
    std::function<bool(bool)> listenToEverything;
    std::function<bool(sonify::Motion)> muted;
    std::function<void(sonify::Motion, bool)> setMuted;
    std::function<bool(int, sonify::Motion, quint32)> playCue;
    std::function<void(int, qint64, qint64)> playNetwork;
    std::function<QStringList()> recentLogLines;
    std::function<LogStyle(const QString &)> logStyle;
    std::function<QHash<quint64, QString>()> runningTasks;
    std::function<QList<RelayClient>()> relayClients;
    std::function<QJsonObject()> relayDiagnostics;
    std::function<QJsonArray()> savedHosts;
    std::function<bool()> replySpeech;
    std::function<bool(bool, QWidget *)> setReplySpeech;
    std::function<void(int, const QString &, const QString &)> speakReply;
    std::function<void(const QString &, bool)> ping;
    std::function<void(const QString &)> log;
    std::function<QString()> cloudflareToken;
    std::function<QString()> cloudflareZoneName;
    std::function<void(const QString &, const QString &, const QByteArray &,
                       const QJsonObject &, CloudflareReply)>
        cloudflare;
};

class Listener
{
public:
    virtual ~Listener();
    virtual void agentEvent(int, const QJsonObject &) {}
    virtual void noteScheduled(int, sonify::Motion, double, double, quint32) {}
    virtual void logLine(const QString &) {}
    virtual void backgroundTask(const TaskEvent &) {}
    virtual void relayRosterChanged() {}
    virtual void relayFrame(bool, const QString &, const QString &, qint64) {}
    virtual void webEvent(const control::CloudflareTailEvent &) {}
    virtual void replySpeechChanged(bool) {}
};

void setHost(const void *owner, Host host);
void clearHost(const void *owner);
bool hasHost();
const Host &host();

void addListener(Listener *listener);
void removeListener(Listener *listener);
void setWatching(Listener *listener, bool watching);
bool watched();

void publishAgentEvent(int sessionId, const QJsonObject &event);
void publishNote(int sessionId, sonify::Motion motion, double delaySeconds, double gain,
                 quint32 seed);
void publishLogLine(const QString &storedLine);
void publishTask(const TaskEvent &task);
void publishRelayRoster();
void publishRelayFrame(bool outgoing, const QString &type, const QString &peerId,
                       qint64 bytes);
void publishWebEvent(const control::CloudflareTailEvent &event);
void publishReplySpeech(bool on);

} // namespace forkmesh::agentfeed

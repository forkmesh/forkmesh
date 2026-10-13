#include "AgentActivityFeed.h"

#include <QSet>

#include <utility>

namespace forkmesh::agentfeed {
namespace {

struct Feed {
    const void *owner = nullptr;
    Host host;
    QList<Listener *> listeners;
    QSet<Listener *> watching;
};

Feed &feed()
{
    static Feed instance;
    return instance;
}

template <typename Call>
void each(Call call)
{
    const QList<Listener *> listeners = feed().listeners;
    for (Listener *listener : listeners) {
        if (feed().listeners.contains(listener))
            call(listener);
    }
}

} // namespace

Listener::~Listener()
{
    removeListener(this);
}

void setHost(const void *owner, Host host)
{
    feed().owner = owner;
    feed().host = std::move(host);
}

void clearHost(const void *owner)
{
    if (feed().owner != owner)
        return;
    feed().owner = nullptr;
    feed().host = Host();
}

bool hasHost()
{
    return feed().owner != nullptr;
}

const Host &host()
{
    return feed().host;
}

void addListener(Listener *listener)
{
    if (listener && !feed().listeners.contains(listener))
        feed().listeners.append(listener);
}

void removeListener(Listener *listener)
{
    feed().listeners.removeAll(listener);
    feed().watching.remove(listener);
}

void setWatching(Listener *listener, bool watching)
{
    if (watching && feed().listeners.contains(listener))
        feed().watching.insert(listener);
    else
        feed().watching.remove(listener);
}

bool watched()
{
    return !feed().watching.isEmpty();
}

void publishAgentEvent(int sessionId, const QJsonObject &event)
{
    each([&](Listener *listener) { listener->agentEvent(sessionId, event); });
}

void publishNote(int sessionId, sonify::Motion motion, double delaySeconds, double gain,
                 quint32 seed)
{
    each([&](Listener *listener) {
        listener->noteScheduled(sessionId, motion, delaySeconds, gain, seed);
    });
}

void publishLogLine(const QString &storedLine)
{
    each([&](Listener *listener) { listener->logLine(storedLine); });
}

void publishTask(const TaskEvent &task)
{
    each([&](Listener *listener) { listener->backgroundTask(task); });
}

void publishRelayRoster()
{
    each([](Listener *listener) { listener->relayRosterChanged(); });
}

void publishRelayFrame(bool outgoing, const QString &type, const QString &peerId,
                       qint64 bytes)
{
    each([&](Listener *listener) { listener->relayFrame(outgoing, type, peerId, bytes); });
}

void publishWebEvent(const control::CloudflareTailEvent &event)
{
    each([&](Listener *listener) { listener->webEvent(event); });
}

void publishReplySpeech(bool on)
{
    each([on](Listener *listener) { listener->replySpeechChanged(on); });
}

} // namespace forkmesh::agentfeed

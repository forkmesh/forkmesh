#include "BackupHostFeed.h"

#include <utility>

namespace forkmesh::backupfeed {
namespace {

struct Feed {
    const void *owner = nullptr;
    Host host;
    QList<Listener *> listeners;
};

Feed &feed()
{
    static Feed instance;
    return instance;
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
}

void publishStatus(const QString &text, bool error)
{
    const QList<Listener *> listeners = feed().listeners;
    for (Listener *listener : listeners) {
        if (feed().listeners.contains(listener))
            listener->backupStatus(text, error);
    }
}

} // namespace forkmesh::backupfeed

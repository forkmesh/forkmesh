#pragma once

#include <QList>
#include <QString>

#include <functional>

namespace forkmesh::backupfeed {

struct Host {
    std::function<QString()> snapshotRoot;
    std::function<bool()> hourlyEnabled;
    std::function<void(bool)> setHourlyEnabled;
    std::function<int()> keepCount;
    std::function<void(int)> setKeepCount;
    std::function<bool()> running;
    std::function<void()> backUpNow;
    std::function<void(const QString &)> restore;
    std::function<void()> snapshotsChanged;
};

class Listener
{
public:
    virtual ~Listener();
    virtual void backupStatus(const QString &, bool) {}
};

void setHost(const void *owner, Host host);
void clearHost(const void *owner);
bool hasHost();
const Host &host();
void addListener(Listener *listener);
void removeListener(Listener *listener);
void publishStatus(const QString &text, bool error);

} // namespace forkmesh::backupfeed

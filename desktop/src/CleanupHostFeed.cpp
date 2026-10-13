#include "CleanupHostFeed.h"

#include <utility>

namespace forkmesh::cleanupfeed {
namespace {

struct Feed {
    const void *owner = nullptr;
    Host host;
};

Feed &feed()
{
    static Feed instance;
    return instance;
}

} // namespace

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

} // namespace forkmesh::cleanupfeed

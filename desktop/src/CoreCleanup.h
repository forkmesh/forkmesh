#pragma once

#include "CleanupHostFeed.h"

#include <functional>

class CoreClient;
class QObject;

namespace forkmesh::corecleanup {

void install(forkmesh::cleanupfeed::Host &host, QObject *owner,
             std::function<CoreClient *()> client);

} // namespace forkmesh::corecleanup

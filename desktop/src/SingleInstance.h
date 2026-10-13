#pragma once

#include <QString>

#include <functional>

namespace forkmesh {

bool acquireSingleInstance(const QString &activationTarget = QString());

void onSingleInstanceActivation(
    std::function<void(const QString &activationTarget)> handler);

void releaseSingleInstance();

// Moves the held lock from the legacy ~/.forkmesh into the project's state
// folder once the layout migration allows it. The new lock is taken before the
// old one is dropped, so there is never a moment without one.
void rehomeSingleInstanceLock();

} // namespace forkmesh

#pragma once

#include <QString>

#include <functional>

namespace forkmesh {

bool acquireSingleInstance(const QString &activationTarget = QString(),
                           const std::function<void()> &whileWaiting = {});

void setBackgroundInstance();

bool isBackgroundInstance();

void onSingleInstanceYield(std::function<void()> handler);

void requestBackgroundQuit();

qint64 backgroundInstancePid();

QString backgroundInstanceMarkerPath();

void onSingleInstanceActivation(
    std::function<void(const QString &activationTarget)> handler);

void releaseSingleInstance();

QString singleInstanceLockPath();

// Moves the held lock from the legacy ~/.forkmesh into the project's state
// folder once the layout migration allows it. The new lock is taken before the
// old one is dropped, so there is never a moment without one.
void rehomeSingleInstanceLock();

} // namespace forkmesh

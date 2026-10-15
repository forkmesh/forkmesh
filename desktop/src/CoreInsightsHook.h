#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace forkmesh::coreinsights {

struct Hooks {
    std::function<bool(const QString &path)> owns;
    std::function<bool(const QString &method, const QJsonObject &params)> notify;
    std::function<bool()> deliveringCoreEvent;
    bool toolsLocalDirty = false;
    bool guardrailsLocalDirty = false;
};

inline Hooks &hooks()
{
    static Hooks instance;
    return instance;
}

inline bool owns(const QString &path)
{
    const Hooks &h = hooks();
    return !path.isEmpty() && h.owns && h.owns(path);
}

inline bool deliveringCoreEvent()
{
    const Hooks &h = hooks();
    return h.deliveringCoreEvent && h.deliveringCoreEvent();
}

inline void forward(const QString &method, const QJsonObject &params)
{
    const Hooks &h = hooks();
    if (h.notify)
        h.notify(method, params);
}

} // namespace forkmesh::coreinsights

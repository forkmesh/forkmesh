#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QJsonObject>
#include <QString>
#include <QVariant>

#include <functional>

namespace forkmesh::coreinsights {

struct Hooks {
    std::function<bool(const QString &path)> owns;
    std::function<bool(const QString &method, const QJsonObject &params)> notify;
    std::function<bool()> deliveringCoreEvent;
    bool toolsLocalDirty = false;
    bool guardrailsLocalDirty = false;
    bool shortcutRunning = false;
    QString shortcutPath;
    int coreRunId = 0;
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

inline QVariant decodeVariant(const QString &base64)
{
    QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
    QDataStream stream(&bytes, QIODevice::ReadOnly);
    stream.setVersion(QDataStream::Qt_4_0);
    QVariant value;
    stream >> value;
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
        return {};
    return value;
}

} // namespace forkmesh::coreinsights

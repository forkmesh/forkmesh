#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

namespace forkmesh::coreops {

class State : public QObject
{
public:
    explicit State(QObject *parent) : QObject(parent) {}

    bool runActive = false;
    bool bypass = false;
    bool echo = false;
    bool cancelled = false;
    int run = -1;
    QString phase;
    int pass = -1;
    int percent = -1;
    QJsonObject lastDesktopState;
    bool backupRunning = false;
    qint64 failureAtMs = -1;
    bool unpacking = false;
    bool exporting = false;
    QStringList capabilities;
    bool prebuilt = false;
    bool prebuiltBypass = false;
};

struct Slot {
    QPointer<State> instance;
    const QObject *owner = nullptr;
};

inline Slot &slot()
{
    static Slot value;
    return value;
}

inline State *peek(const QObject *owner)
{
    const Slot &current = slot();
    return owner && current.owner == owner ? current.instance.data() : nullptr;
}

inline State *state(QObject *owner)
{
    if (!owner)
        return nullptr;
    if (State *existing = peek(owner))
        return existing;
    Slot &current = slot();
    current.instance = new State(owner);
    current.owner = owner;
    return current.instance;
}

inline bool can(const QObject *owner, const QString &capability)
{
    const State *current = peek(owner);
    return current && current->capabilities.contains(capability);
}

inline QJsonObject desktopState(bool running, const QString &kind, int percent,
                                const QString &phase, const QString &activity)
{
    return {{QStringLiteral("running"), running},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("percent"), percent},
            {QStringLiteral("phase"), phase},
            {QStringLiteral("activity"), activity}};
}

} // namespace forkmesh::coreops

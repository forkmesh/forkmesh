#include "CoreDiag.h"

#include "CoreClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>

namespace forkmesh::corediag {

namespace {

QPointer<CoreClient> &client()
{
    static QPointer<CoreClient> current;
    return current;
}

void sync(CoreClient *core, const QStringList &stalls)
{
    if (!core || !core->isConnected())
        return;
    core->notify(QStringLiteral("core.diag.stalls"),
                 {{QStringLiteral("entries"), QJsonArray::fromStringList(stalls)}});
}

}

void follow(CoreClient *core, std::function<QStringList()> stalls)
{
    if (!core)
        return;
    client() = core;
    QObject::connect(core, &CoreClient::connectedChanged, core, [core, stalls](bool connected) {
        if (connected && stalls)
            sync(core, stalls());
    });
    if (core->isConnected() && stalls)
        sync(core, stalls());
}

void pushStall(const QString &entry)
{
    CoreClient *core = client();
    if (core && core->isConnected())
        core->notify(QStringLiteral("core.diag.stall"), {{QStringLiteral("entry"), entry}});
}

void clearStalls()
{
    CoreClient *core = client();
    if (core && core->isConnected())
        core->notify(QStringLiteral("core.diag.stall"), {{QStringLiteral("clear"), true}});
}

}

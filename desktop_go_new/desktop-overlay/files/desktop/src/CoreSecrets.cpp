#include "CoreSecrets.h"

#include "ActionStore.h"
#include "ActionVariablesBackend.h"
#include "CoreClient.h"
#include "LocalControlSecrets.h"

#include <QMap>
#include <QSettings>

namespace forkmesh::coresecrets {

namespace secretsctl = forkmesh::localcontrol::secrets;

QString apply(const QJsonObject &params)
{
    const QString action = params.value(QStringLiteral("action")).toString();
    const QString name = params.value(QStringLiteral("name")).toString();
    const QString value = params.value(QStringLiteral("value")).toString();
    if (!secretsctl::isMutatingAction(action))
        return QStringLiteral("bad_request");
    if (!secretsctl::validName(name))
        return QStringLiteral("invalid_name");
    const bool setting = action == QLatin1String("set");
    if (setting && !secretsctl::validValue(value))
        return QStringLiteral("value_too_large");
    const QMap<QString, QString> current = ActionStore::variables();
    if (!setting && !current.contains(name))
        return QStringLiteral("not_found");
    if (!forkmesh::actionvars::write(setting ? secretsctl::applySet(current, name, value)
                                             : secretsctl::applyDelete(current, name)))
        return QStringLiteral("vault_unavailable");
    QSettings().sync();
    return {};
}

void registerHandlers(CoreClient *client, std::function<void()> changed)
{
    if (!client)
        return;
    client->handle(QStringLiteral("ui.secrets.apply"),
                   [changed](const QJsonObject &params, const CoreClient::Responder &respond) {
                       const QString error = apply(params);
                       if (error.isEmpty() && changed)
                           changed();
                       respond(error.isEmpty() ? QJsonObject{{QStringLiteral("ok"), true}}
                                               : QJsonObject(),
                               error);
                   });
}

} // namespace forkmesh::coresecrets

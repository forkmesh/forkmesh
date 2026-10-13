#include "ActionStore.h"
#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlSecrets.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QDateTime>

namespace {

namespace secretsctl = forkmesh::localcontrol::secrets;
namespace hostctl = forkmesh::localcontrol::hosts;

class SecretsState : public QObject
{
public:
    explicit SecretsState(QObject *parent) : QObject(parent)
    {
        setObjectName(QStringLiteral("forkmeshSecretsState"));
    }

    hostctl::ConfirmationStore confirmations;
};

SecretsState *secretsState(QObject *window)
{
    QObject *found = window->findChild<QObject *>(QStringLiteral("forkmeshSecretsState"),
                                                  Qt::FindDirectChildrenOnly);
    if (auto *state = dynamic_cast<SecretsState *>(found))
        return state;
    return new SecretsState(window);
}

} // namespace

void MainWindow::handleLocalControlSecretsRequest(int connectionId, int requestId,
                                                  const QString &method,
                                                  const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    SecretsState *state = secretsState(this);
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    const QString holder = secretsctl::confirmationHolder(device.key, connectionId);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto fail = [server, connectionId, requestId](const QString &code) {
        server->respondError(connectionId, requestId, code);
    };
    const QString name = params.value(QStringLiteral("name")).toString();

    if (method == QLatin1String("secrets.list")) {
        server->respond(connectionId, requestId,
                        {{QStringLiteral("variables"),
                          secretsctl::listRows(ActionStore::variables())}});
        return;
    }

    if (method == QLatin1String("secrets.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        if (!secretsctl::isMutatingAction(action)) {
            fail(QStringLiteral("bad_request"));
            return;
        }
        if (!secretsctl::validName(name)) {
            fail(QStringLiteral("invalid_name"));
            return;
        }
        if (action == QLatin1String("delete") && !ActionStore::variables().contains(name)) {
            fail(QStringLiteral("not_found"));
            return;
        }
        const QString token = state->confirmations.issue(holder, action, name, now);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("token"), token},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("summary"), secretsctl::actionSummary(action, name)},
                         {QStringLiteral("expiresInMs"),
                          int(hostctl::ConfirmationStore::kLifetimeMs)}});
        return;
    }

    if (method == QLatin1String("secrets.set") || method == QLatin1String("secrets.delete")) {
        const bool setting = method == QLatin1String("secrets.set");
        const QString value = params.value(QStringLiteral("value")).toString();
        if (!secretsctl::validName(name)) {
            fail(QStringLiteral("invalid_name"));
            return;
        }
        if (setting && !secretsctl::validValue(value)) {
            fail(QStringLiteral("value_too_large"));
            return;
        }
        const auto result = state->confirmations.consume(
            params.value(QStringLiteral("token")).toString(), holder,
            setting ? QStringLiteral("set") : QStringLiteral("delete"), name, now);
        if (result != hostctl::ConfirmationStore::Result::Ok) {
            fail(hostctl::confirmationError(result));
            return;
        }
        const QMap<QString, QString> current = ActionStore::variables();
        if (!setting && !current.contains(name)) {
            fail(QStringLiteral("not_found"));
            return;
        }
        ActionStore::setVariables(setting ? secretsctl::applySet(current, name, value)
                                          : secretsctl::applyDelete(current, name));
        reloadVariablesList();
        logSystem(QStringLiteral("%1 the Actions variable %2, requested by %3.")
                      .arg(setting ? QStringLiteral("Set") : QStringLiteral("Deleted"), name,
                           localControlDeviceLabel(connectionId)));
        server->respond(connectionId, requestId, {{QStringLiteral("ok"), true}});
        server->broadcast(QStringLiteral("secrets.changed"));
        return;
    }

    fail(QStringLiteral("unknown_method"));
}

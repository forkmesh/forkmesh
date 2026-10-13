#include "CoreMirrorActions.h"

#include "CoreClient.h"
#include "CoreGit.h"
#include "MirrorActionsConfiguration.h"
#include "VaultFile.h"

#include <QSet>
#include <QSettings>
#include <QString>

namespace forkmesh::coremirroractions {

namespace {

constexpr qint64 kApplyTimeoutMs = 180000;

const QSet<QString> &nativeFallbackErrors()
{
    static const QSet<QString> errors{
        QStringLiteral("core_unavailable"),
        QStringLiteral("unsupported"),
        QStringLiteral("desktop_unavailable"),
        QStringLiteral("forbidden"),
    };
    return errors;
}

}

bool delegateConfiguration(const QJsonObject &request, QJsonObject *response)
{
    if (forkmesh::core::disabledByEnvironment() ||
        !QSettings().value(QStringLiteral("core/enabled"), true).toBool())
        return false;
    forkmesh::coregit::attach(forkmesh::core::defaultStateDirectory());
    QJsonObject result;
    QString error;
    bool sent = false;
    const bool answered = forkmesh::coregit::callCore(
        QStringLiteral("core.actions.mirrorConfig.apply"),
        {{QStringLiteral("request"), request}}, kApplyTimeoutMs, &result, &error, &sent);
    forkmesh::coregit::detach();
    if (answered && result.value(QStringLiteral("type")).isString()) {
        *response = result;
        return true;
    }
    if (sent && !nativeFallbackErrors().contains(error)) {
        *response = forkmesh::mirror_actions::refusedConfiguration(
            request, QStringLiteral("configuration_core_failed"));
        return true;
    }
    if (request.contains(QStringLiteral("variables")) &&
        forkmesh::vaultfile::exists(forkmesh::vaultfile::defaultPath())) {
        *response = forkmesh::mirror_actions::refusedConfiguration(
            request, QStringLiteral("variables_vault_unavailable"));
        return true;
    }
    return false;
}

void install()
{
    forkmesh::mirror_actions::setConfigurationDelegate(delegateConfiguration);
}

}

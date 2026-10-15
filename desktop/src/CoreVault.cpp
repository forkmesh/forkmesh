#include "CoreVault.h"

#include "ActionVariablesBackend.h"
#include "CoreClient.h"
#include "CoreGit.h"
#include "SettingsSecretBackend.h"
#include "VaultFile.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QPointer>
#include <QSettings>

namespace forkmesh::corevault {

namespace {

bool actionVariablesFinalized()
{
    const QJsonObject stored =
        QJsonDocument::fromJson(QSettings().value(QStringLiteral("actions/variables")).toByteArray())
            .object();
    const QString marker = forkmesh::actionvars::marker();
    for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
        if (it.value().toString() == marker)
            return true;
    return false;
}

bool secretFinalized(const QString &key)
{
    return QSettings().value(key).toString() == forkmesh::settingsecrets::marker();
}

constexpr qint64 kCacheMs = 30000;
constexpr qint64 kReadTimeoutMs = 3000;
constexpr qint64 kWriteTimeoutMs = 8000;
constexpr qint64 kLogRepeatMs = 600000;
const QString kNamespace = QStringLiteral("actions");

const QString kDesktopNamespace = QStringLiteral("desktop");
const QString kSealKeySetting = QStringLiteral("chat/e2ee/x25519PrivateV2");

struct State {
    QMutex mutex;
    QPointer<QObject> context;
    std::function<void(const QString &)> log;
    QMap<QString, QString> cache;
    qint64 cacheUntilMs = 0;
    quint64 generation = 0;
    QHash<QString, qint64> logged;
    QHash<QString, QPair<QString, qint64>> secrets;
};

State &state()
{
    static State instance;
    return instance;
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

void wipe(QMap<QString, QString> *values)
{
    for (auto it = values->begin(); it != values->end(); ++it)
        it.value().fill(QChar(0));
    values->clear();
}

void logLine(const QString &text)
{
    QPointer<QObject> context;
    std::function<void(const QString &)> log;
    {
        QMutexLocker lock(&state().mutex);
        const qint64 last = state().logged.value(text, 0);
        if (last && nowMs() - last < kLogRepeatMs)
            return;
        state().logged.insert(text, nowMs());
        context = state().context;
        log = state().log;
    }
    if (!context || !log)
        return;
    QMetaObject::invokeMethod(context, [log, text] { log(text); }, Qt::QueuedConnection);
}

bool cached(QMap<QString, QString> *out)
{
    QMutexLocker lock(&state().mutex);
    if (state().cacheUntilMs <= nowMs()) {
        wipe(&state().cache);
        return false;
    }
    *out = state().cache;
    return true;
}

void remember(const QMap<QString, QString> &values, quint64 generation)
{
    QMutexLocker lock(&state().mutex);
    if (generation != state().generation)
        return;
    wipe(&state().cache);
    state().cache = values;
    state().cacheUntilMs = nowMs() + kCacheMs;
}

quint64 currentGeneration()
{
    QMutexLocker lock(&state().mutex);
    return state().generation;
}

bool coreFailure(const QString &error)
{
    return error == QLatin1String("core_unavailable") || error == QLatin1String("unsupported") ||
           error == QLatin1String("core_gone") || error == QLatin1String("core_timeout") ||
           error == QLatin1String("forbidden");
}

qint64 settingsModifiedMs()
{
    return QFileInfo(QSettings().fileName()).lastModified().toMSecsSinceEpoch();
}

bool readFromFile(QMap<QString, QString> *out)
{
    const QString path = forkmesh::vaultfile::defaultPath();
    if (!forkmesh::vaultfile::exists(path))
        return false;
    QMap<QString, forkmesh::vaultfile::Entry> entries;
    QString error;
    if (!forkmesh::vaultfile::readNamespace(path, forkmesh::vaultfile::identityPath(), kNamespace,
                                            &entries, &error)) {
        logLine(QStringLiteral("Actions variables: the vault could not be read without forkmesh-core (%1); "
                               "only variables still in ForkMesh.conf are available").arg(error));
        return false;
    }
    const qint64 settingsAt = settingsModifiedMs();
    const QString marker = forkmesh::actionvars::marker();
    const QMap<QString, QString> plain = forkmesh::actionvars::storedPlaintext();
    out->clear();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        out->insert(it.key(), it.value().value);
        it.value().value.fill(QChar(0));
    }
    for (auto it = plain.constBegin(); it != plain.constEnd(); ++it) {
        if (it.value() == marker)
            continue;
        const auto entry = entries.constFind(it.key());
        if (entry == entries.constEnd() || entry.value().at < settingsAt)
            out->insert(it.key(), it.value());
    }
    logLine(QStringLiteral("Actions variables: forkmesh-core is not running, so they are read from the "
                           "encrypted vault read-only; changes cannot be saved until it runs"));
    return true;
}

bool read(QMap<QString, QString> *out)
{
    if (cached(out))
        return true;
    const quint64 generation = currentGeneration();
    if (forkmesh::coregit::attached()) {
        QJsonObject result;
        QString error;
        if (forkmesh::coregit::callCore(QStringLiteral("core.vault.get"),
                                        {{QStringLiteral("namespace"), kNamespace}}, kReadTimeoutMs,
                                        &result, &error)) {
            out->clear();
            const QJsonObject values = result.value(QStringLiteral("values")).toObject();
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
                out->insert(it.key(), it.value().toString());
            remember(*out, generation);
            return true;
        }
        if (!coreFailure(error)) {
            logLine(QStringLiteral("Actions variables: forkmesh-core could not open the vault (%1); "
                                   "only variables still in ForkMesh.conf are available").arg(error));
            return false;
        }
    }
    return readFromFile(out);
}

forkmesh::actionvars::WriteResult write(const QMap<QString, QString> &variables)
{
    using forkmesh::actionvars::WriteResult;
    if (forkmesh::coregit::attached()) {
        QJsonObject values;
        for (auto it = variables.constBegin(); it != variables.constEnd(); ++it)
            values.insert(it.key(), it.value());
        QJsonObject result;
        QString error;
        const bool ok = forkmesh::coregit::callCore(
            QStringLiteral("core.vault.set"),
            {{QStringLiteral("namespace"), kNamespace},
             {QStringLiteral("values"), values},
             {QStringLiteral("replace"), true}},
            kWriteTimeoutMs, &result, &error);
        for (auto it = values.begin(); it != values.end(); ++it)
            it.value() = QString();
        if (ok) {
            invalidate();
            if (!result.value(QStringLiteral("finalized")).toBool())
                return WriteResult::Unhandled;
            forkmesh::actionvars::writeMarkers(variables.keys());
            return WriteResult::Written;
        }
        if (!coreFailure(error)) {
            logLine(QStringLiteral("Actions variables were not saved: the vault refused the change (%1)").arg(error));
            return WriteResult::Refused;
        }
    }
    if (forkmesh::vaultfile::exists(forkmesh::vaultfile::defaultPath()) && actionVariablesFinalized()) {
        logLine(QStringLiteral("Actions variables were not saved: they live in the encrypted vault and "
                               "forkmesh-core is not running; ForkMesh does not write them to "
                               "ForkMesh.conf in plaintext again"));
        return WriteResult::Refused;
    }
    return WriteResult::Unhandled;
}

bool cachedSecret(const QString &key, QString *out)
{
    QMutexLocker lock(&state().mutex);
    const auto found = state().secrets.constFind(key);
    if (found == state().secrets.constEnd() || found->second <= nowMs())
        return false;
    *out = found->first;
    return true;
}

void rememberSecret(const QString &key, const QString &value, quint64 generation)
{
    QMutexLocker lock(&state().mutex);
    if (generation != state().generation)
        return;
    state().secrets.insert(key, {value, nowMs() + kCacheMs});
}

bool readSecretFromFile(const QString &key, const QString &plain, QString *out)
{
    const QString path = forkmesh::vaultfile::defaultPath();
    if (!forkmesh::vaultfile::exists(path))
        return false;
    QMap<QString, forkmesh::vaultfile::Entry> entries;
    QString error;
    if (!forkmesh::vaultfile::readNamespace(path, forkmesh::vaultfile::identityPath(), kDesktopNamespace,
                                            &entries, &error)) {
        logLine(QStringLiteral("%1: the vault could not be read without forkmesh-core (%2); only a value "
                               "still in ForkMesh.conf is available").arg(key, error));
        return false;
    }
    const auto entry = entries.constFind(key);
    if (!plain.isEmpty() && (entry == entries.constEnd() || entry.value().at < settingsModifiedMs()))
        *out = plain;
    else
        *out = entry == entries.constEnd() ? QString() : entry.value().value;
    for (auto it = entries.begin(); it != entries.end(); ++it)
        it.value().value.fill(QChar(0));
    return true;
}

bool readSecret(const QString &key, QString *out)
{
    if (cachedSecret(key, out))
        return true;
    const quint64 generation = currentGeneration();
    const QString plain = forkmesh::settingsecrets::plaintext(QSettings(), key);
    if (forkmesh::coregit::attached()) {
        QJsonObject result;
        QString error;
        if (forkmesh::coregit::callCore(QStringLiteral("core.vault.get"),
                                        {{QStringLiteral("namespace"), kDesktopNamespace},
                                         {QStringLiteral("names"), QJsonArray{key}}},
                                        kReadTimeoutMs, &result, &error)) {
            const QJsonObject values = result.value(QStringLiteral("values")).toObject();
            *out = values.contains(key) ? values.value(key).toString() : plain;
            rememberSecret(key, *out, generation);
            return true;
        }
        if (!coreFailure(error)) {
            logLine(QStringLiteral("%1: forkmesh-core could not open the vault (%2); only a value still in "
                                   "ForkMesh.conf is available").arg(key, error));
            *out = plain;
            return true;
        }
    }
    return readSecretFromFile(key, plain, out);
}

forkmesh::settingsecrets::WriteResult writeSecret(const QString &key, const QString &value)
{
    using forkmesh::settingsecrets::WriteResult;
    if (forkmesh::coregit::attached()) {
        QJsonObject result;
        QString error;
        const bool ok =
            value.isEmpty()
                ? forkmesh::coregit::callCore(QStringLiteral("core.vault.delete"),
                                              {{QStringLiteral("namespace"), kDesktopNamespace},
                                               {QStringLiteral("names"), QJsonArray{key}}},
                                              kWriteTimeoutMs, &result, &error)
                : forkmesh::coregit::callCore(QStringLiteral("core.vault.set"),
                                              {{QStringLiteral("namespace"), kDesktopNamespace},
                                               {QStringLiteral("values"), QJsonObject{{key, value}}}},
                                              kWriteTimeoutMs, &result, &error);
        if (ok) {
            invalidate();
            return result.value(QStringLiteral("finalized")).toBool() ? WriteResult::Written
                                                                       : WriteResult::Unhandled;
        }
        if (!coreFailure(error)) {
            logLine(QStringLiteral("%1 was not saved: the vault refused the change (%2)").arg(key, error));
            return WriteResult::Refused;
        }
    }
    if (!forkmesh::vaultfile::exists(forkmesh::vaultfile::defaultPath()) || !secretFinalized(key))
        return WriteResult::Unhandled;
    if (key == kSealKeySetting) {
        logLine(QStringLiteral("%1: forkmesh-core is not running, so the new chat device key is kept in "
                               "ForkMesh.conf until the next forkmesh-core run moves it into the vault").arg(key));
        return WriteResult::Unhandled;
    }
    logLine(QStringLiteral("%1 was not saved: it lives in the encrypted vault and forkmesh-core is not "
                           "running; ForkMesh does not write it to ForkMesh.conf in plaintext again").arg(key));
    return WriteResult::Refused;
}

}

void install(QObject *context, std::function<void(const QString &)> log)
{
    {
        QMutexLocker lock(&state().mutex);
        state().context = context;
        state().log = std::move(log);
    }
    forkmesh::actionvars::setBackend(read, write);
    forkmesh::settingsecrets::setBackend({readSecret, writeSecret});
}

void invalidate()
{
    QMutexLocker lock(&state().mutex);
    ++state().generation;
    state().cacheUntilMs = 0;
    wipe(&state().cache);
    for (auto it = state().secrets.begin(); it != state().secrets.end(); ++it)
        it.value().first.fill(QChar(0));
    state().secrets.clear();
}

void follow(CoreClient *client)
{
    if (!client)
        return;
    QObject::connect(client, &CoreClient::coreEvent, client,
                     [](const QString &event, const QJsonObject &) {
                         if (event == QLatin1String("vault.changed"))
                             invalidate();
                     });
    QObject::connect(client, &CoreClient::connectedChanged, client, [client](bool connected) {
        invalidate();
        if (connected && client->hasNativeRoute(QStringLiteral("core.vault.migrate")))
            client->request(QStringLiteral("core.vault.migrate"), {},
                            [](bool, const QJsonValue &, const QString &) { invalidate(); }, 60000);
    });
}

}

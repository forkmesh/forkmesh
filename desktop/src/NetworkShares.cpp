#include "NetworkShares.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>
#include <QUuid>

namespace forkmesh::smb {

namespace {

const QString kSharesSetting = QStringLiteral("files/networkShares");


QJsonObject toJson(const SavedShare &share)
{
    return {
        {QStringLiteral("id"), share.id},
        {QStringLiteral("name"), share.name},
        {QStringLiteral("host"), share.host},
        {QStringLiteral("port"), int(share.port)},
        {QStringLiteral("share"), share.share},
        {QStringLiteral("user"), share.user},
        {QStringLiteral("guest"), share.guest},
        {QStringLiteral("remember"), share.remember},
    };
}

SavedShare fromJson(const QJsonObject &object)
{
    SavedShare share;
    share.id = object.value(QStringLiteral("id")).toString();
    share.name = object.value(QStringLiteral("name")).toString();
    share.host = object.value(QStringLiteral("host")).toString();
    share.port = quint16(object.value(QStringLiteral("port")).toInt(kDefaultPort));
    share.share = object.value(QStringLiteral("share")).toString();
    share.user = object.value(QStringLiteral("user")).toString();
    share.guest = object.value(QStringLiteral("guest")).toBool();
    share.remember = object.value(QStringLiteral("remember")).toBool();
    return share;
}

}

QString SavedShare::title() const
{
    return name.isEmpty() ? QStringLiteral("%1 on %2").arg(share, host) : name;
}

Location SavedShare::location(const QString &path) const
{
    Location out;
    out.host = host;
    out.port = port;
    out.share = share;
    out.path = path;
    return out;
}

QString SavedShare::url(const QString &path) const
{
    return formatUrl(location(path));
}

bool SavedShare::sameTarget(const SavedShare &other) const
{
    return host.compare(other.host, Qt::CaseInsensitive) == 0 && port == other.port
        && share.compare(other.share, Qt::CaseInsensitive) == 0;
}

ShareStore::ShareStore(QString secretsPath) : m_secretsPath(std::move(secretsPath)) {}

QList<SavedShare> ShareStore::shares() const
{
    QList<SavedShare> out;
    const QJsonArray array = QJsonDocument::fromJson(QSettings().value(kSharesSetting).toByteArray()).array();
    for (const QJsonValue &value : array) {
        const SavedShare share = fromJson(value.toObject());
        if (!share.id.isEmpty() && !share.host.isEmpty() && !share.share.isEmpty())
            out.append(share);
    }
    return out;
}

SavedShare ShareStore::share(const QString &id) const
{
    for (const SavedShare &share : shares()) {
        if (share.id == id)
            return share;
    }
    return {};
}

SavedShare ShareStore::save(SavedShare share)
{
    if (share.id.isEmpty())
        share.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<SavedShare> all = shares();
    bool replaced = false;
    for (SavedShare &existing : all) {
        if (existing.id == share.id) {
            existing = share;
            replaced = true;
        }
    }
    if (!replaced)
        all.append(share);
    QJsonArray array;
    for (const SavedShare &entry : std::as_const(all))
        array.append(toJson(entry));
    QSettings().setValue(kSharesSetting, QJsonDocument(array).toJson(QJsonDocument::Compact));
    return share;
}

void ShareStore::remove(const QString &id)
{
    QJsonArray array;
    for (const SavedShare &entry : shares()) {
        if (entry.id != id)
            array.append(toJson(entry));
    }
    QSettings().setValue(kSharesSetting, QJsonDocument(array).toJson(QJsonDocument::Compact));
    setPassword(id, {});
}

QString ShareStore::password(const QString &id) const
{
    return readSecrets().value(id);
}

bool ShareStore::setPassword(const QString &id, const QString &password)
{
    QHash<QString, QString> secrets = readSecrets();
    if (password.isEmpty()) {
        if (!secrets.contains(id))
            return true;
        secrets.remove(id);
    } else {
        secrets.insert(id, password);
    }
    return writeSecrets(secrets);
}

QHash<QString, QString> ShareStore::readSecrets() const
{
    QHash<QString, QString> out;
    QFile file(m_secretsPath);
    if (m_secretsPath.isEmpty() || !file.open(QIODevice::ReadOnly))
        return out;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = object.begin(); it != object.end(); ++it)
        out.insert(it.key(), it.value().toString());
    return out;
}

bool ShareStore::writeSecrets(const QHash<QString, QString> &secrets) const
{
    if (m_secretsPath.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(m_secretsPath).absolutePath());
    QFile file(m_secretsPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    QJsonObject object;
    for (auto it = secrets.begin(); it != secrets.end(); ++it)
        object.insert(it.key(), it.value());
    return file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) >= 0;
}

struct ConnectionState {
    SavedShare share;
    Credentials credentials;
    TransportFactory factory;
    std::unique_ptr<Client> client;
};

Connection::Connection(SavedShare share, Credentials credentials, TransportFactory factory)
    : m_share(share), m_state(std::make_shared<ConnectionState>())
{
    m_state->share = std::move(share);
    m_state->credentials = std::move(credentials);
    m_state->factory = std::move(factory);
    m_thread = new QThread;
    m_thread->setObjectName(QStringLiteral("forkmesh-smb"));
    m_context = new QObject;
    m_context->moveToThread(m_thread);
    QObject::connect(m_thread, &QThread::finished, m_context, &QObject::deleteLater);
    m_thread->start();
}

Connection::~Connection()
{
    *m_cancel = true;
    QMetaObject::invokeMethod(m_context, [state = m_state] { state->client.reset(); }, Qt::QueuedConnection);
    m_thread->quit();
    if (m_thread->wait(2000)) {
        delete m_thread;
    } else {
        QThread *thread = m_thread;
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    }
}

void Connection::setCredentials(const Credentials &credentials)
{
    QMetaObject::invokeMethod(
        m_context,
        [state = m_state, credentials] {
            state->credentials = credentials;
            if (state->client)
                state->client->close();
        },
        Qt::QueuedConnection);
}

void Connection::post(Job job)
{
    QMetaObject::invokeMethod(
        m_context,
        [state = m_state, job = std::move(job)] {
            if (!state->client)
                state->client = std::make_unique<Client>(state->factory);
            if (!state->client->isOpen())
                state->client->open(state->share.host, state->share.port, state->credentials, state->share.share);
            job(*state->client);
        },
        Qt::QueuedConnection);
}

}

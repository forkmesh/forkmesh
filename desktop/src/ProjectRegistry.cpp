#include "ProjectRegistry.h"
#include "SafeDelete.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>

namespace forkmesh::projects {

namespace {

constexpr int kSchema = 1;
constexpr qint64 kMaxProjectFileBytes = 256 * 1024;
constexpr qint64 kMaxLogoBytes = 16 * 1024 * 1024;
constexpr int kMaxInstanceChoices = 32;

const QStringList &logoNames()
{
    static const QStringList names{QStringLiteral("logo.png"), QStringLiteral("logo.webp"),
                                   QStringLiteral("logo.jpg"), QStringLiteral("logo.jpeg")};
    return names;
}

const QStringList &projectRoots()
{
    static const QStringList roots{QStringLiteral("data/core"),
                                   QStringLiteral("data/core/config"),
                                   QStringLiteral("data/core/cache"),
                                   QStringLiteral("data/core/state"),
                                   QStringLiteral("data/core/logs"),
                                   QStringLiteral("data/core/ssh")};
    return roots;
}

QStringList stringList(const QJsonValue &value)
{
    QStringList list;
    const QJsonArray array = value.toArray();
    for (const QJsonValue &entry : array) {
        const QString text = entry.toString().trimmed();
        if (!text.isEmpty() && !list.contains(text))
            list.append(text);
    }
    return list;
}

Project readProject(const QString &dir, bool isHome)
{
    Project project;
    project.dir = QDir::cleanPath(dir);
    project.slug = isHome ? QLatin1String(forkmesh::paths::kHomeProjectSlug)
                          : QFileInfo(project.dir).fileName();
    project.isHome = isHome;
    project.name = isHome ? QStringLiteral("ForkMesh") : project.slug;
    project.logoPath = logoPathIn(project.dir);
    // ForkMesh's own logo ships at the root of its checkout.
    if (project.logoPath.isEmpty() && isHome) {
        project.logoPath = logoPathIn(QDir(project.dir).filePath(QStringLiteral("src")));
        if (project.logoPath.isEmpty())
            project.logoPath = QStringLiteral(":/app/forkmesh.png");
    }

    QFile file(QDir(project.dir).filePath(QLatin1String(forkmesh::paths::kProjectFileName)));
    if (file.size() > kMaxProjectFileBytes || !file.open(QIODevice::ReadOnly))
        return project;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    const QString name = object.value(QStringLiteral("name")).toString().trimmed();
    if (!name.isEmpty())
        project.name = name.left(80);
    project.description =
        object.value(QStringLiteral("description")).toString().trimmed().left(400);
    project.packages = stringList(object.value(QStringLiteral("packages")));
    project.offline = object.value(QStringLiteral("offline")).toBool();
    project.created = QDateTime::fromString(
        object.value(QStringLiteral("created")).toString(), Qt::ISODate);
    project.ownsFolder = !isHome && project.created.isValid();
    project.instancesRecorded = object.contains(QStringLiteral("instances"));
    project.instances = stringList(object.value(QStringLiteral("instances")));
    return project;
}

QStringList settingsInstances(const QString &projectDir)
{
    const QString path = settingsFilePath(projectDir);
    if (!QFileInfo(path).isFile())
        return {};
    QSettings settings(path, QSettings::IniFormat);
    if (settings.value(kInstancePendingSetting, false).toBool())
        return {};
    QStringList keys;
    const QJsonArray servers = QJsonDocument::fromJson(
        settings.value(QStringLiteral("servers/items")).toString().toUtf8()).array();
    for (const QJsonValue &server : servers) {
        const QString key = instanceKey(server.toObject().value(QStringLiteral("url")).toString());
        if (!key.isEmpty() && !keys.contains(key))
            keys.append(key);
    }
    if (keys.isEmpty()) {
        const QString key = instanceKey(settings.value(QStringLiteral("server/url")).toString());
        if (!key.isEmpty())
            keys.append(key);
    }
    return keys;
}

bool readObject(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        object->insert(QStringLiteral("schema"), kSchema);
        return true;
    }
    if (file.size() > kMaxProjectFileBytes || !file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("%1 could not be read, so it was left as it is.")
                     .arg(QDir::toNativeSeparators(path));
        return false;
    }
    *object = document.object();
    return true;
}

bool writeJson(const QString &path, const QJsonObject &object, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0 ||
        !file.commit()) {
        *error = QStringLiteral("Could not write %1.").arg(QDir::toNativeSeparators(path));
        return false;
    }
    return true;
}

} // namespace

QString logoPathIn(const QString &projectDir)
{
    for (const QString &name : logoNames()) {
        const QString path = QDir(projectDir).filePath(name);
        if (QFileInfo(path).isFile())
            return path;
    }
    return QString();
}

QVector<Project> list(const QString &projectsRoot, const QString &homeDir)
{
    QVector<Project> projects;
    projects.append(readProject(homeDir, /*isHome=*/true));
    const QString home = QDir::cleanPath(homeDir);
    const QFileInfoList entries = QDir(projectsRoot).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const QFileInfo &entry : entries) {
        const QString dir = QDir::cleanPath(entry.absoluteFilePath());
        if (dir == home || !forkmesh::paths::isValidSlug(entry.fileName()) ||
            entry.fileName() == QLatin1String(forkmesh::paths::kHomeProjectSlug))
            continue;
        const QString projectFile =
            QDir(dir).filePath(QLatin1String(forkmesh::paths::kProjectFileName));
        if (!QFileInfo(projectFile).isFile() &&
            QFileInfo(projectFile + QStringLiteral(".removed")).exists())
            continue;
        projects.append(readProject(dir, /*isHome=*/false));
    }
    return projects;
}

QString encodeInstanceChoices(const QVector<InstanceChoice> &choices)
{
    QJsonArray array;
    QStringList seen;
    for (const InstanceChoice &choice : choices) {
        const QString url = choice.url.trimmed();
        if (url.isEmpty() || seen.contains(url) || array.size() >= kMaxInstanceChoices)
            continue;
        seen.append(url);
        array.append(QJsonObject{{QStringLiteral("name"), choice.name.trimmed().left(80)},
                                 {QStringLiteral("url"), url}});
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QVector<InstanceChoice> decodeInstanceChoices(const QString &json)
{
    QVector<InstanceChoice> choices;
    QStringList seen;
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        InstanceChoice choice;
        choice.url = object.value(QStringLiteral("url")).toString().trimmed();
        choice.name = object.value(QStringLiteral("name")).toString().trimmed().left(80);
        if (choice.url.isEmpty() || seen.contains(choice.url) ||
            choices.size() >= kMaxInstanceChoices)
            continue;
        seen.append(choice.url);
        choices.append(choice);
    }
    return choices;
}

QString instanceKey(const QString &url)
{
    QString raw = url.trimmed();
    if (raw.isEmpty())
        return {};
    if (!raw.contains(QStringLiteral("://")))
        raw.prepend(QStringLiteral("https://"));
    const QUrl parsed(raw);
    const QString host = parsed.host().toLower();
    if (!parsed.isValid() || host.isEmpty())
        return {};
    const QString scheme = parsed.scheme().toLower();
    int port = parsed.port(-1);
    if (((scheme == QLatin1String("https") || scheme == QLatin1String("wss")) && port == 443) ||
        ((scheme == QLatin1String("http") || scheme == QLatin1String("ws")) && port == 80))
        port = -1;
    static const QStringList production{QStringLiteral("app.forkmesh.com"),
                                        QStringLiteral("forkmesh.com"),
                                        QStringLiteral("api.forkmesh.com"),
                                        QStringLiteral("app.next.forkmesh.com")};
    if (port < 0 && (production.contains(host) || host.endsWith(QLatin1String(".next.forkmesh.com"))))
        return production.first();
    return port < 0 ? host : QStringLiteral("%1:%2").arg(host).arg(port);
}

bool recordInstances(const QString &projectDir, const QStringList &urls, QString *error)
{
    QString localError;
    if (!error)
        error = &localError;
    error->clear();
    QStringList keys;
    for (const QString &url : urls) {
        const QString key = instanceKey(url);
        if (!key.isEmpty() && !keys.contains(key))
            keys.append(key);
    }
    const QString path =
        QDir(projectDir).filePath(QLatin1String(forkmesh::paths::kProjectFileName));
    QJsonObject object;
    if (!readObject(path, &object, error))
        return false;
    const QJsonArray array = QJsonArray::fromStringList(keys);
    if (object.value(QStringLiteral("instances")).toArray() == array)
        return true;
    object.insert(QStringLiteral("instances"), array);
    return writeJson(path, object, error);
}

const Project *instanceOwner(const QVector<Project> &projects, const QString &url,
                             const QString &exceptDir)
{
    const QString key = instanceKey(url);
    if (key.isEmpty())
        return nullptr;
    const QString except = QDir::cleanPath(exceptDir);
    for (const Project &project : projects) {
        if (QDir::cleanPath(project.dir) == except)
            continue;
        const QStringList keys =
            project.instancesRecorded ? project.instances : settingsInstances(project.dir);
        if (keys.contains(key))
            return &project;
    }
    return nullptr;
}

QString settingsFilePath(const QString &projectDir)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    const QString file = QStringLiteral("config/ForkMesh/ForkMesh.conf");
#else
    const QString file = QStringLiteral("config/ForkMesh/ForkMesh.ini");
#endif
    const QString organized = QDir(projectDir).filePath(QStringLiteral("data/core/") + file);
    const QString previous = QDir(projectDir).filePath(file);
    if (!QFileInfo::exists(organized) && QFileInfo(previous).isFile() &&
        !QFileInfo(QDir(projectDir).filePath(QStringLiteral("config"))).isSymLink())
        return previous;
    return organized;
}

bool create(const forkmesh::paths::Layout &current, const CreateRequest &request,
            Project *created, QString *error)
{
    QString localError;
    if (!error)
        error = &localError;
    error->clear();

    const QString name = request.name.trimmed().left(80);
    const QString slug = forkmesh::paths::slugify(name);
    if (!forkmesh::paths::isValidSlug(slug)) {
        *error = QStringLiteral("Give the project a name with at least one letter or number.");
        return false;
    }
    if (slug == QLatin1String(forkmesh::paths::kHomeProjectSlug)) {
        *error = QStringLiteral("\xE2\x80\x9C%1\xE2\x80\x9D is the ForkMesh project itself. "
                                "Pick another name.")
                     .arg(name);
        return false;
    }
    const QString dir = QDir(current.projectsRoot).filePath(slug);
    if (QFileInfo::exists(dir) || QFileInfo(dir).isSymLink()) {
        *error = QStringLiteral("%1 already exists. A project only ever uses a folder it "
                                "created itself.")
                     .arg(QDir::toNativeSeparators(dir));
        return false;
    }
    if (!request.logoSource.isEmpty()) {
        const QFileInfo logo(request.logoSource);
        const QString suffix = logo.suffix().toLower();
        if (!logo.isFile() || logo.size() > kMaxLogoBytes ||
            !logoNames().contains(QStringLiteral("logo.") + suffix)) {
            *error = QStringLiteral("The logo must be a PNG, WebP or JPEG image under 16 MB.");
            return false;
        }
    }

    const QFileDevice::Permissions ownerOnly =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QDir().mkpath(dir)) {
        *error = QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(dir));
        return false;
    }
    QFile::setPermissions(dir, ownerOnly);
    for (const QString &root : projectRoots()) {
        const QString path = QDir(dir).filePath(root);
        if (!QDir().mkpath(path)) {
            *error = QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(path));
            return false;
        }
        QFile::setPermissions(path, ownerOnly);
    }

    if (!request.logoSource.isEmpty()) {
        const QString suffix = QFileInfo(request.logoSource).suffix().toLower();
        QFile::copy(request.logoSource, QDir(dir).filePath(QStringLiteral("logo.") + suffix));
    }

    // Same account in the new project: the identity key *is* the account, so
    // without it the project would start as a stranger to every relay.
    if (request.shareIdentity) {
        const QString source = QDir(current.data).filePath(QStringLiteral("identity/ed25519.pem"));
        if (QFileInfo(source).isFile()) {
            const QString identityDir = QDir(dir).filePath(QStringLiteral("data/core/identity"));
            QDir().mkpath(identityDir);
            QFile::setPermissions(identityDir, ownerOnly);
            const QString target = QDir(identityDir).filePath(QStringLiteral("ed25519.pem"));
            if (!QFile::copy(source, target)) {
                *error = QStringLiteral("Could not copy the identity key into the project.");
                return false;
            }
            QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }

    // Seed settings the project's own process reads on first start.
    {
        const QString settingsPath = settingsFilePath(dir);
        QDir().mkpath(QFileInfo(settingsPath).absolutePath());
        QFile::setPermissions(QFileInfo(settingsPath).absolutePath(), ownerOnly);
        QSettings seed(settingsPath, QSettings::IniFormat);
        for (const QString &id : request.removedPackages) {
            if (request.packages.contains(id))
                continue;
            seed.setValue(QStringLiteral("market/packages/%1/state").arg(id),
                          QStringLiteral("removed"));
        }
        for (const QString &id : request.packages) {
            seed.setValue(QStringLiteral("market/packages/%1/state").arg(id),
                          QStringLiteral("installed"));
        }
        seed.setValue(QStringLiteral("network/offlineMode"), true);
        seed.setValue(QStringLiteral("network/offlineModeAutomatic"), false);
        seed.setValue(kInstancePendingSetting, true);
        seed.setValue(kInstanceChoicesSetting, encodeInstanceChoices(request.instances));
        seed.setValue(QStringLiteral("project/name"), name);
        seed.sync();
        if (seed.status() != QSettings::NoError) {
            *error = QStringLiteral("Could not write the project's settings.");
            return false;
        }
        QFile::setPermissions(settingsPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    QJsonObject object;
    object.insert(QStringLiteral("schema"), kSchema);
    object.insert(QStringLiteral("slug"), slug);
    object.insert(QStringLiteral("name"), name);
    object.insert(QStringLiteral("description"), request.description.trimmed().left(400));
    object.insert(QStringLiteral("created"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    object.insert(QStringLiteral("offline"), true);
    object.insert(QStringLiteral("packages"), QJsonArray::fromStringList(request.packages));
    // project.json is written last: a folder without it is not a project, so
    // a creation that failed half-way never shows up in the picker.
    if (!writeJson(QDir(dir).filePath(QLatin1String(forkmesh::paths::kProjectFileName)),
                   object, error))
        return false;

    if (created)
        *created = readProject(dir, /*isHome=*/false);
    return true;
}

bool updateDetails(const QString &projectDir, const QString &name,
                   const QString &description, QString *error)
{
    QString localError;
    if (!error)
        error = &localError;
    error->clear();
    const QString trimmedName = name.trimmed().left(80);
    if (trimmedName.isEmpty()) {
        *error = QStringLiteral("Give the project a name.");
        return false;
    }
    const QString path =
        QDir(projectDir).filePath(QLatin1String(forkmesh::paths::kProjectFileName));
    QJsonObject object;
    if (!readObject(path, &object, error))
        return false;
    object.insert(QStringLiteral("name"), trimmedName);
    object.insert(QStringLiteral("description"), description.trimmed().left(400));
    return writeJson(path, object, error);
}

bool remove(const QString &projectsRoot, const QString &slug, bool keepFolder,
            QString *error, QString *movedTo)
{
    QString localError;
    if (!error)
        error = &localError;
    error->clear();
    if (movedTo)
        movedTo->clear();
    if (slug == QLatin1String(forkmesh::paths::kHomeProjectSlug)) {
        *error = QStringLiteral("The ForkMesh project itself cannot be removed.");
        return false;
    }
    const QString dir = QDir::cleanPath(QDir(projectsRoot).filePath(slug));
    const QString projectFile =
        QDir(dir).filePath(QLatin1String(forkmesh::paths::kProjectFileName));
    const QString setAside = projectFile + QStringLiteral(".removed");
    const bool hasProjectFile = QFileInfo(projectFile).isFile();
    const QFileInfo info(dir);
    if (!forkmesh::paths::isValidSlug(slug) || info.isSymLink() || !info.isDir() ||
        (!hasProjectFile && QFileInfo(setAside).exists())) {
        *error = QStringLiteral("That project no longer exists.");
        return false;
    }
    if (!keepFolder && !(hasProjectFile && readProject(dir, /*isHome=*/false).ownsFolder)) {
        *error = QStringLiteral("ForkMesh did not create %1, so it will not move it aside. "
                                "Keep the folder to stop listing it.")
                     .arg(QDir::toNativeSeparators(dir));
        return false;
    }
    // The same lock a project's process holds while it runs (SingleInstance);
    // a stale one left by a crash is taken over.
    for (const QString &state : {QDir(dir).filePath(QStringLiteral("data/core/state")),
                                 QDir(dir).filePath(QStringLiteral("state"))}) {
        QLockFile running(QDir(state).filePath(QStringLiteral("forkmesh.lock")));
        if (QFileInfo(state).isDir() && !running.tryLock(0)) {
            *error = QStringLiteral("The project is open in another window. Close it "
                                    "first, then remove it.");
            return false;
        }
    }

    // project.json goes first either way: a folder without it is not a project,
    // so a deletion that fails half-way never leaves a broken entry behind.
    QFile::remove(setAside);
    if (hasProjectFile ? !QFile::rename(projectFile, setAside)
                       : !writeJson(setAside, QJsonObject(), error)) {
        *error = QStringLiteral("Could not change %1.").arg(QDir::toNativeSeparators(projectFile));
        return false;
    }
    if (keepFolder)
        return true;
    const QString aside = forkmesh::safedelete::moveAside(dir, QStringLiteral("deleted"));
    if (aside.isEmpty()) {
        *error = QStringLiteral("The project is removed, but %1 could not be moved aside.")
                     .arg(QDir::toNativeSeparators(dir));
        return false;
    }
    if (movedTo)
        *movedTo = aside;
    return true;
}

QStringList launchArguments(const QStringList &currentArguments, const QString &slug)
{
    QStringList arguments;
    for (int i = 1; i < currentArguments.size(); ++i) {
        const QString &argument = currentArguments.at(i);
        if (argument == QLatin1String("--project")) {
            ++i;  // and its value
            continue;
        }
        if (argument.startsWith(QLatin1String("--project=")))
            continue;
        // A link the first process was launched to open belongs to it alone.
        if (argument.startsWith(QLatin1String("forkmesh:")) ||
            argument.startsWith(QLatin1String("http")))
            continue;
        arguments.append(argument);
    }
    // Always explicit, the home project included: the child inherits this
    // process's FORKMESH_PROJECT, and only the argument outranks it.
    arguments << QStringLiteral("--project") << slug;
    return arguments;
}

} // namespace forkmesh::projects

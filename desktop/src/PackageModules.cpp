#include "PackageModules.h"

#include "NativePackageExtension.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QRecursiveMutex>
#include <QRegularExpression>
#include <QtDebug>

#ifndef FORKMESH_PACKAGE_MODULE_API
#define FORKMESH_PACKAGE_MODULE_API 1
#endif
#ifndef FORKMESH_PACKAGE_MODULE_ABI
#define FORKMESH_PACKAGE_MODULE_ABI ""
#endif
#ifndef FORKMESH_PACKAGE_MODULE_RELDIR
#define FORKMESH_PACKAGE_MODULE_RELDIR "../lib/forkmesh/packages"
#endif

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace forkmesh::market::modules {
namespace {

QRecursiveMutex &lock()
{
    static QRecursiveMutex mutex;
    return mutex;
}

QList<Module> &registry()
{
    static QList<Module> list;
    return list;
}

bool safeFile(const QString &path, QString *why)
{
#ifdef Q_OS_UNIX
    for (QString current = path; !current.isEmpty();) {
        struct stat info;
        if (::stat(QFile::encodeName(current).constData(), &info) != 0) {
            *why = QStringLiteral("cannot stat %1").arg(current);
            return false;
        }
        const bool foreignGroupWrite = (info.st_mode & S_IWGRP) && info.st_gid != ::getgid();
        if (((info.st_mode & S_IWOTH) || foreignGroupWrite) && !(S_ISDIR(info.st_mode) && (info.st_mode & S_ISVTX))) {
            *why = QStringLiteral("%1 is writable by other users").arg(current);
            return false;
        }
        if (info.st_uid != 0 && info.st_uid != ::getuid()) {
            *why = QStringLiteral("%1 belongs to another user").arg(current);
            return false;
        }
        if (current == path) {
            current = QFileInfo(path).absolutePath();
            continue;
        }
        break;
    }
#else
    Q_UNUSED(path);
    Q_UNUSED(why);
#endif
    return true;
}

Module readSidecar(const QString &directory, const QString &name)
{
    static const QRegularExpression idPattern(QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,62}[a-z0-9])?$"));
    Module module;
    QFile file(QDir(directory).filePath(name));
    if (file.size() > 64 * 1024 || !file.open(QIODevice::ReadOnly)) {
        module.error = QStringLiteral("unreadable sidecar");
        return module;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    module.id = object.value(QStringLiteral("package")).toString();
    module.api = object.value(QStringLiteral("api")).toInt();
    module.abi = object.value(QStringLiteral("abi")).toString();
    const QString library = object.value(QStringLiteral("file")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("entrypoints")).toArray())
        module.entrypoints << value.toString();
    if (!idPattern.match(module.id).hasMatch() || library.contains(QLatin1Char('/')) ||
        library.contains(QLatin1Char('\\')) || !library.startsWith(module.id))
        module.error = QStringLiteral("invalid sidecar");
    module.path = QDir(directory).filePath(library);
    return module;
}

} // namespace

QStringList searchDirectories()
{
    const QDir app(QCoreApplication::applicationDirPath());
    QStringList out{
        app.filePath(QStringLiteral("packages")),
        QDir::cleanPath(app.filePath(QStringLiteral(FORKMESH_PACKAGE_MODULE_RELDIR))),
        QDir::cleanPath(app.filePath(QStringLiteral("../lib/forkmesh/packages"))),
        QDir::cleanPath(app.filePath(QStringLiteral("../PlugIns/forkmesh-packages"))),
    };
    out.removeDuplicates();
    return out;
}

void scan(const QStringList &directories)
{
    QList<Module> found;
    QStringList seen;
    for (const QString &directory : directories) {
        const QDir dir(directory);
        if (!dir.exists())
            continue;
        for (const QString &name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            Module module = readSidecar(directory, name);
            if (module.id.isEmpty() || seen.contains(module.id))
                continue;
            seen << module.id;
            if (module.error.isEmpty() && module.api != FORKMESH_PACKAGE_MODULE_API)
                module.error = QStringLiteral("module API %1, the app speaks %2")
                                   .arg(module.api)
                                   .arg(FORKMESH_PACKAGE_MODULE_API);
            if (module.error.isEmpty() && module.abi != QLatin1String(FORKMESH_PACKAGE_MODULE_ABI))
                module.error = QStringLiteral("built for a different app (%1)").arg(module.abi);
            if (module.error.isEmpty() && !QFileInfo::exists(module.path))
                module.error = QStringLiteral("module file missing");
            found << module;
        }
    }
    QMutexLocker locker(&lock());
    registry() = found;
}

void install()
{
    scan(searchDirectories());
    setNativePackageLoader([](const QString &entrypoint) { return loadForEntrypoint(entrypoint); });
}

bool providesEntrypoint(const QString &entrypoint)
{
    QMutexLocker locker(&lock());
    for (const Module &module : std::as_const(registry()))
        if (module.entrypoints.contains(entrypoint))
            return true;
    return false;
}

bool loadForEntrypoint(const QString &entrypoint)
{
    QMutexLocker locker(&lock());
    for (Module &module : registry()) {
        if (!module.entrypoints.contains(entrypoint))
            continue;
        if (module.attempted)
            return module.loaded;
        module.attempted = true;
        if (!module.error.isEmpty()) {
            qWarning().noquote() << QStringLiteral("[package module] %1 not loaded: %2").arg(module.id, module.error);
            return false;
        }
        QString why;
        if (!safeFile(module.path, &why)) {
            module.error = why;
            qWarning().noquote() << QStringLiteral("[package module] %1 refused: %2").arg(module.id, why);
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        auto *library = new QLibrary(module.path);
        if (!library->load()) {
            module.error = library->errorString();
            delete library;
            qWarning().noquote() << QStringLiteral("[package module] %1 failed to load: %2").arg(module.id, module.error);
            return false;
        }
        using IntFn = int (*)();
        using TextFn = const char *(*)();
        const auto api = reinterpret_cast<IntFn>(library->resolve("forkmesh_package_module_api"));
        const auto abi = reinterpret_cast<TextFn>(library->resolve("forkmesh_package_module_abi"));
        const auto id = reinterpret_cast<TextFn>(library->resolve("forkmesh_package_module_id"));
        if (!api || !abi || !id || api() != FORKMESH_PACKAGE_MODULE_API ||
            QLatin1String(abi()) != QLatin1String(FORKMESH_PACKAGE_MODULE_ABI) ||
            QLatin1String(id()) != module.id) {
            module.error = QStringLiteral("module identity does not match its sidecar");
            for (const QString &provided : std::as_const(module.entrypoints))
                unregisterNativePackageEntrypoint(provided);
            qWarning().noquote() << QStringLiteral("[package module] %1 rejected: %2").arg(module.id, module.error);
            return false;
        }
        module.loaded = true;
        module.loadMs = timer.elapsed();
        qInfo().noquote() << QStringLiteral("[package module] loaded %1 in %2ms").arg(module.id).arg(module.loadMs);
        return true;
    }
    return false;
}

QList<Module> modules()
{
    QMutexLocker locker(&lock());
    return registry();
}

QJsonArray status()
{
    QJsonArray out;
    for (const Module &module : modules())
        out.append(QJsonObject{{QStringLiteral("id"), module.id},
                               {QStringLiteral("path"), module.path},
                               {QStringLiteral("loaded"), module.loaded},
                               {QStringLiteral("error"), module.error},
                               {QStringLiteral("loadMs"), double(module.loadMs)}});
    return out;
}

} // namespace forkmesh::market::modules

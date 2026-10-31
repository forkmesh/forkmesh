#pragma once

#include "AppPaths.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// Projects: the top-level unit ForkMesh operates on. A project is a folder in
// the projects root holding project.json, an optional logo, and its own data,
// config, cache, state, logs and ssh folders. Nothing is shared between two
// projects except the ForkMesh home's source, toolchains and build trees, so
// a project can start with no repository, no relay and almost no plugins.
//
// The running process serves exactly one project. Opening another starts
// another process for it; see MainWindow::openProject().
namespace forkmesh::projects {

struct Project {
    QString slug;
    QString name;
    QString description;
    QString dir;
    QString logoPath;       // empty when the project has no logo file
    QStringList packages;   // plugin ids the project started with
    bool offline = false;   // created without a relay
    bool isHome = false;
    bool ownsFolder = false;
    QDateTime created;
    QStringList instances;
    bool instancesRecorded = false;
};

// Conventional logo at the project root, in the order repositories use.
QString logoPathIn(const QString &projectDir);

// The home project first, then every folder in the projects root, by name.
QVector<Project> list(const QString &projectsRoot, const QString &homeDir);

struct InstanceChoice {
    QString name;
    QString url;
};

inline const QString kInstancePendingSetting = QStringLiteral("project/instancePending");
inline const QString kInstanceChoicesSetting = QStringLiteral("project/instanceChoices");
inline const QString kInstanceChosenSetting = QStringLiteral("project/instanceChosen");

QString encodeInstanceChoices(const QVector<InstanceChoice> &choices);
QVector<InstanceChoice> decodeInstanceChoices(const QString &json);

enum class StarterPack { Minimal, Standard, Everything, Empty };

QStringList starterPackages(StarterPack pack, const QStringList &available);

struct DirectoryInstance {
    QString name;
    QString host;
    QString logoUrl;
    bool online = false;
};

QVector<DirectoryInstance> directoryInstances(const QJsonArray &rows, const QString &origin);

bool isForkMeshVersionReply(int status, bool workerHeader, const QJsonObject &body);

struct CreateRequest {
    QString name;
    QString description;
    QString logoSource;          // image to copy in as the logo; optional
    QVector<InstanceChoice> instances;
    InstanceChoice instance;
    bool shareIdentity = true;   // copy this project's identity key, so the
                                 // new project signs in as the same account
    QStringList packages;        // plugins to start with
    QStringList removedPackages; // default-installed plugins to start without
};

// Creates ~/projects/<slug>/ with its isolated roots, project.json, logo and
// seed settings. Refuses an existing folder: a project never adopts files it
// did not create.
bool create(const forkmesh::paths::Layout &current, const CreateRequest &request,
            Project *created, QString *error);

// Renames a project and rewrites its description in project.json, keeping
// every other key. The folder and slug never change.
bool updateDetails(const QString &projectDir, const QString &name,
                   const QString &description, QString *error);

bool remove(const QString &projectsRoot, const QString &slug, bool keepFolder,
            QString *error, QString *movedTo = nullptr);

QString instanceKey(const QString &url);

bool recordInstances(const QString &projectDir, const QStringList &urls, QString *error);

const Project *instanceOwner(const QVector<Project> &projects, const QString &url,
                             const QString &exceptDir);

// Settings file a project's process will read, for seeding before first run.
QString settingsFilePath(const QString &projectDir);

// Arguments that make a new process open `slug`, given this process's own.
QStringList launchArguments(const QStringList &currentArguments, const QString &slug);

} // namespace forkmesh::projects

#pragma once

#include "AppPaths.h"

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

// Projects: the top-level unit ForkMesh operates on. A project is a folder in
// the projects root holding project.json and an optional logo; its own data,
// config, cache, state, logs and ssh folders live in forkmesh_data/<slug>
// beside it, and the project folder gains other folders only when a plugin
// needs one. Nothing is shared between two
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

QString encodeInstanceChoices(const QVector<InstanceChoice> &choices);
QVector<InstanceChoice> decodeInstanceChoices(const QString &json);

struct CreateRequest {
    QString name;
    QString description;
    QString logoSource;          // image to copy in as the logo; optional
    QVector<InstanceChoice> instances;
    bool shareIdentity = true;   // copy this project's identity key, so the
                                 // new project signs in as the same account
    QStringList packages;        // plugins to start with
    QStringList removedPackages; // default-installed plugins to start without
};

// Creates ~/projects/<slug>/ with project.json and logo, and
// ~/projects/forkmesh_data/<slug>/ with its isolated roots and seed settings.
// Refuses an existing folder: a project never adopts files it did not create.
bool create(const forkmesh::paths::Layout &current, const CreateRequest &request,
            Project *created, QString *error);

// Renames a project and rewrites its description in project.json, keeping
// every other key. The folder and slug never change.
bool updateDetails(const QString &projectDir, const QString &name,
                   const QString &description, QString *error);

bool remove(const QString &projectsRoot, const QString &slug, bool keepFolder,
            QString *error, QString *movedTo = nullptr);

// Settings file a project's process will read, for seeding before first run.
QString settingsFilePath(const QString &projectDir);

QString storeDir(const QString &projectDir);

// Arguments that make a new process open `slug`, given this process's own.
QStringList launchArguments(const QStringList &currentArguments, const QString &slug);

} // namespace forkmesh::projects

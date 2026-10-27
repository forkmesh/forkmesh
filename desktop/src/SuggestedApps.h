#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace forkmesh::environment {

// An external command-line program that makes one or more ForkMesh features
// available. The settings UI keeps package-manager knowledge out of the window
// implementation by sharing this catalog.
struct SuggestedApp {
    QString id;
    QString name;
    // These are alternatives. An audio recorder, for example, may be ffmpeg,
    // arecord or parecord depending on the operating system.
    QStringList executableNames;
    QStringList versionArguments;
    QString description;
    bool optional = false;
};

struct SuggestedAppInstallPlan {
    QString program;
    QStringList arguments;
    QString managerName;
    QString unavailableReason;
    bool requiresAdministrator = false;

    bool isAvailable() const { return !program.isEmpty(); }
};

// jq and ripgrep intentionally live alongside direct ForkMesh dependencies:
// agents and workflow users benefit from both even when the GUI has another
// implementation of the same task.
const QVector<SuggestedApp> &suggestedApps();
const SuggestedApp *suggestedAppById(const QString &id);
const SuggestedApp &embeddedBrowserComponent();

// Builds an installation command from package-manager executables currently
// available on PATH. This module never executes a command itself.
SuggestedAppInstallPlan suggestedAppInstallPlan(
    const SuggestedApp &app, const QStringList &availableInstallers);

} // namespace forkmesh::environment

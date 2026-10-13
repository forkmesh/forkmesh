#pragma once

#include <QString>
#include <QStringList>

namespace forkmesh::runners {

struct RunnerSetupHandoff {
    QString token;
    QString name;
    QString appUrl;

    bool isValid() const;
};

RunnerSetupHandoff parseRunnerSetupHandoff(const QString &role,
                                           const QString &token,
                                           const QString &name,
                                           const QString &appUrl);
QString runnerInstallScriptUrl(const QString &installScriptUrl);
QString runnerInstallRemoteCommand(const RunnerSetupHandoff &handoff,
                                   const QString &scriptUrl,
                                   const QString &sshUser,
                                   bool sudoPassword);
QString redactRunnerToken(const QString &text);
QStringList parseHostKeyFingerprints(const QString &keygenOutput);
QString hostKeyConfirmationText(const QString &host,
                                const QStringList &fingerprints);

}

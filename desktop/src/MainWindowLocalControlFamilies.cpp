// Gate 2 (docs/native/DESKTOP_PARITY.md section 19.3): every local control
// method family, registered once on the control server. Each family's
// requests go to its own MainWindowLocalControl<Family>.cpp, owned by one
// workstream, so no two workstreams edit the same dispatch code.
//
// Families are owner-only: the device must be paired at this desktop (the
// channel checks that) and name the account this desktop is signed in as.

#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlAgentOps.h"
#include "LocalControlMergeQueue.h"
#include "LocalControlPulls.h"
#include "LocalControlFleet.h"
#include "LocalControlFiles.h"
#include "LocalControlDiagnostics.h"
#include "LocalControlTeammates.h"
#include "LocalControlInsights.h"
#include "LocalControlDiscord.h"
#include "LocalControlSecrets.h"
#include "LocalControlRepos.h"
#include "LocalControlStats.h"
#include "LocalControlTerminals.h"
#include "LocalControlUpdate.h"
#include "LocalControlWallet.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "PingFixes.h"

namespace hostctl = forkmesh::localcontrol::hosts;

void MainWindow::registerLocalControlFamilies()
{
    if (!m_localControl)
        return;
    using Handler = void (MainWindow::*)(int, int, const QString &, const QJsonObject &);
    auto registerOwnerFamily = [this](const QString &prefix, Handler handler) {
        m_localControl->registerFamily(
            prefix, [this, handler](int connectionId, const LocalControlServer::Device &device,
                                    int requestId, const QString &method,
                                    const QJsonObject &params) {
                if (!m_localControl)
                    return;
                if (device.key.isEmpty() ||
                    (!hostctl::ownerMayManageHosts(device.user, nodeOwnerDisplayName()) &&
                     !hostctl::ownerMayManageHosts(device.user, accountOwner()))) {
                    m_localControl->respondError(connectionId, requestId,
                                                 QStringLiteral("not_owner"));
                    return;
                }
                (this->*handler)(connectionId, requestId, method, params);
            });
    };
    registerOwnerFamily(forkmesh::localcontrol::agentops::familyPrefix(),
                        &MainWindow::handleLocalControlAgentOpsRequest);
    registerOwnerFamily(forkmesh::localcontrol::mergequeue::familyPrefix(),
                        &MainWindow::handleLocalControlMergeQueueRequest);
    registerOwnerFamily(forkmesh::localcontrol::pulls::familyPrefix(),
                        &MainWindow::handleLocalControlPullsRequest);
    registerOwnerFamily(forkmesh::localcontrol::fleet::familyPrefix(),
                        &MainWindow::handleLocalControlFleetRequest);
    registerOwnerFamily(forkmesh::localcontrol::filesbrowse::familyPrefix(),
                        &MainWindow::handleLocalControlFilesRequest);
    registerOwnerFamily(forkmesh::localcontrol::diag::familyPrefix(),
                        &MainWindow::handleLocalControlDiagnosticsRequest);
    registerOwnerFamily(forkmesh::localcontrol::teammates::familyPrefix(),
                        &MainWindow::handleLocalControlTeammatesRequest);
    registerOwnerFamily(forkmesh::localcontrol::insights::familyPrefix(),
                        &MainWindow::handleLocalControlInsightsRequest);
    registerOwnerFamily(forkmesh::localcontrol::discord::familyPrefix(),
                        &MainWindow::handleLocalControlDiscordRequest);
    registerOwnerFamily(forkmesh::localcontrol::wallet::familyPrefix(),
                        &MainWindow::handleLocalControlWalletRequest);
    registerOwnerFamily(forkmesh::localcontrol::secrets::familyPrefix(),
                        &MainWindow::handleLocalControlSecretsRequest);
    registerOwnerFamily(forkmesh::localcontrol::stats::familyPrefix(),
                        &MainWindow::handleLocalControlStatsRequest);
    registerOwnerFamily(forkmesh::localcontrol::repos::familyPrefix(),
                        &MainWindow::handleLocalControlReposRequest);
    registerOwnerFamily(forkmesh::localcontrol::update::familyPrefix(),
                        &MainWindow::handleLocalControlUpdateRequest);
    registerOwnerFamily(forkmesh::localcontrol::terminals::familyPrefix(),
                        &MainWindow::handleLocalControlTerminalRequest);
    registerOwnerFamily(forkmesh::pingfixes::familyPrefix(),
                        &MainWindow::handleLocalControlPingsRequest);
}

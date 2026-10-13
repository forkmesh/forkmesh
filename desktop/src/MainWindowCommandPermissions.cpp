// The approval chip on every command in an agent transcript, and the dialog it
// opens. The chip says whether the command could run without the user — from
// the run's permission mode, the project's agent access (ProjectAgentAccess.h)
// and the external-call gate (AgentNetworkGate.h) — and the dialog lists the
// run's commands beside every permission that applies to them.

#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentNetworkGate.h"
#include "AgentScratch.h"
#include "AgentToolCatalog.h"
#include "AgentToolPolicy.h"
#include "ClaudeTranscriptView.h"
#include "CodexTranscriptStyle.h"
#include "ProjectAgentAccess.h"
#include "VirtualMachineRuntime.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QTreeWidget>

using namespace forkmesh::ui;

namespace {

using Approval = ClaudeTranscriptView::CommandApproval;
using Access = forkmesh::projects::AgentAccess;

Approval approval(Approval::State state, const QString &label, const QString &reason)
{
    Approval a;
    a.state = state;
    a.label = label;
    a.reason = reason;
    return a;
}

QString modeMeaning(const QString &modeLabel)
{
    const QString id = agentPermissionModeId(modeLabel);
    if (id == QLatin1String("bypassPermissions"))
        return QStringLiteral("runs local commands without asking");
    if (id == QLatin1String("plan"))
        return QStringLiteral("plans first; commands that change things wait for the "
                              "plan to be approved");
    if (id == QLatin1String("acceptEdits"))
        return QStringLiteral("applies file edits without asking, asks before commands");
    return QStringLiteral("asks before every command not on the agent's allow list");
}

QString networkMeaning(Access::Network network)
{
    switch (network) {
    case Access::Network::Allow:
        return QStringLiteral("External calls run without asking.");
    case Access::Network::Block:
        return QStringLiteral("External calls are refused, approved destinations "
                              "excepted.");
    case Access::Network::Ask:
        break;
    }
    return QStringLiteral("Every external call waits for your approval, approved "
                          "destinations excepted.");
}

QString htmlList(const QStringList &items, const QString &empty)
{
    if (items.isEmpty())
        return QStringLiteral("<i>%1</i>").arg(empty.toHtmlEscaped());
    QStringList escaped;
    for (const QString &item : items)
        escaped << QStringLiteral("<code>%1</code>").arg(item.toHtmlEscaped());
    return escaped.join(QStringLiteral(", "));
}

QString stateColor(const Approval &a)
{
    switch (a.state) {
    case Approval::Allowed:
        return QStringLiteral("#1a7f37");
    case Approval::Asks:
        return QStringLiteral("#9a6700");
    case Approval::Blocked:
    case Approval::Denied:
        return QStringLiteral("#cf222e");
    case Approval::Unknown:
        break;
    }
    return QString();
}

} // namespace

void MainWindow::installAgentCommandApprovals()
{
    if (!m_agentTranscript)
        return;
    m_agentTranscript->setCommandApprovalResolver(
        [this](const QString &command, const QString &cwd) -> Approval {
            const int sid = m_selectedAgentSessionId;
            QString mode;
            QString workdir = cwd;
            {
                const AgentSession *session = findAgentSession(sid);
                if (!session || isExternalSession(sid))
                    return {}; // not launched here: its permissions are not ours to know
                mode = session->mode;
                if (workdir.isEmpty()) {
                    const int ri = repoIndexFor(session->owner, session->name);
                    if (ri >= 0) {
                        const QString repoLocal = m_repositories.at(ri).localPath;
                        workdir = cachedSessionWorktreeForUi(sid, repoLocal,
                                                             session->branchName);
                        if (workdir.isEmpty() || !QDir(workdir).exists())
                            workdir = repoLocal;
                    }
                }
            }

            if (AgentToolPolicy::disabledFamiliesFromOptions(
                    agentRunLaunchOptions(sid, m_codexStreams.contains(sid)))
                    .contains(QStringLiteral("bash")))
                return approval(Approval::Blocked, QStringLiteral("Blocked"),
                                QStringLiteral("The Shell tool family is switched off "
                                               "in Tool efficiency or outside the global "
                                               "tool allowlist."));

            const forkmesh::agentnet::ExternalCall call = forkmesh::agentnet::classifyCommand(
                command, workdir);
            if (!forkmesh::vm::active()) {
                forkmesh::commandvars::Environment environment;
                environment.values.insert(QStringLiteral("HOME"), QDir::homePath());
                if (!workdir.isEmpty())
                    environment.values.insert(forkmesh::commandvars::ScratchName,
                                              forkmesh::agentscratch::folderFor(workdir));
                const forkmesh::agentrules::Decision decision = forkmesh::agentrules::decide(
                    agentRunRules(sid), call,
                    forkmesh::agentnet::classifyCommandRisk(command, environment),
                    agentRunApprovals(sid));
                if (decision.verdict == forkmesh::agentrules::Verdict::Deny)
                    return approval(Approval::Blocked, QStringLiteral("Blocked"),
                                    decision.reason);
                if (decision.verdict == forkmesh::agentrules::Verdict::Ask)
                    return approval(Approval::Asks, QStringLiteral("Asks first"),
                                    decision.reason);
            }

            if (mode.isEmpty())
                mode = QSettings().value(kClaudeAutoModeSetting, true).toBool()
                           ? kClaudeAutoModeLabel
                           : kAgentAskModeLabel;
            const QString where = call.external ? QStringLiteral("External call (%1)")
                                                      .arg(call.target)
                                                : QStringLiteral("Local command");
            if (agentModeSkipsPermissions(mode))
                return approval(Approval::Allowed, QStringLiteral("Allowed"),
                                QStringLiteral("%1; %2 mode %3.")
                                    .arg(where, mode, modeMeaning(mode)));
            return approval(Approval::Asks, QStringLiteral("Asks first"),
                            QStringLiteral("%1; %2 mode %3.")
                                .arg(where, mode, modeMeaning(mode)));
        });
    connect(m_agentTranscript, &ClaudeTranscriptView::commandApprovalActivated, this,
            &MainWindow::showAgentCommandPermissions);
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testAgentCommandApproval(int sessionId, const QString &command,
                                             const QString &cwd)
{
    if (!m_agentTranscript)
        ensureRepoDetailTabBuilt(3);
    if (!m_agentTranscript)
        return QString();
    const int selected = m_selectedAgentSessionId;
    m_selectedAgentSessionId = sessionId;
    const Approval a = m_agentTranscript->resolveCommandApprovalForTest(command, cwd);
    m_selectedAgentSessionId = selected;
    return a.label + QLatin1Char('|') + a.reason;
}
#endif

void MainWindow::showAgentCommandPermissions(const QString &toolUseId)
{
    if (!m_agentTranscript)
        return;
    if (m_agentCommandPermissionsDialog)
        m_agentCommandPermissionsDialog->close();
    const int sid = m_selectedAgentSessionId;
    const AgentSession *session = findAgentSession(sid);

    auto *dialog = new QDialog(this);
    m_agentCommandPermissionsDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("agentCommandPermissionsDialog"));
    dialog->setWindowTitle(QStringLiteral("Commands and permissions"));
    dialog->resize(820, 620);
    auto *layout = new QVBoxLayout(dialog);
    layout->setSpacing(8);

    QString runName = QStringLiteral("Agent #%1").arg(sid);
    if (session && !session->name.isEmpty())
        runName += QStringLiteral(" · %1").arg(session->name);
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(runName.toHtmlEscaped()));
    layout->addWidget(title);

    // The run's commands, oldest first, each with the chip's state.
    const QVector<ClaudeTranscriptView::CommandApprovalRow> rows =
        m_agentTranscript->commandApprovals();
    int allowed = 0, asked = 0, refused = 0;
    auto *list = new QTreeWidget;
    list->setObjectName(QStringLiteral("agentCommandPermissionsList"));
    list->setColumnCount(4);
    list->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Approval"),
                           QStringLiteral("Command"), QStringLiteral("Why")});
    list->setRootIsDecorated(false);
    list->setAlternatingRowColors(true);
    list->setUniformRowHeights(true);
    QTreeWidgetItem *current = nullptr;
    for (int i = 0; i < rows.size(); ++i) {
        const auto &row = rows.at(i);
        auto *item = new QTreeWidgetItem;
        item->setData(0, Qt::DisplayRole, i + 1);
        QString state = row.approval.label.isEmpty() ? QStringLiteral("—")
                                                     : row.approval.label;
        if (row.finished && row.failed && row.approval.state != Approval::Denied)
            state += QStringLiteral(" · failed");
        item->setText(1, state);
        if (const QString color = stateColor(row.approval); !color.isEmpty())
            item->setForeground(1, QColor(color));
        item->setText(2, CodexTranscriptStyle::singleLineCommand(row.command));
        item->setToolTip(2, row.command);
        item->setFont(2, QFontDatabase::systemFont(QFontDatabase::FixedFont));
        item->setText(3, row.approval.reason);
        item->setToolTip(3, row.approval.reason);
        list->addTopLevelItem(item);
        if (row.toolUseId == toolUseId)
            current = item;
        switch (row.approval.state) {
        case Approval::Allowed:
            ++allowed;
            break;
        case Approval::Asks:
            ++asked;
            break;
        case Approval::Blocked:
        case Approval::Denied:
            ++refused;
            break;
        case Approval::Unknown:
            break;
        }
    }
    list->setSortingEnabled(true);
    list->sortByColumn(0, Qt::AscendingOrder);
    list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list->header()->setSectionResizeMode(2, QHeaderView::Interactive);
    list->header()->setStretchLastSection(true);
    list->setColumnWidth(2, 380);
    if (current) {
        list->setCurrentItem(current);
        list->scrollToItem(current, QAbstractItemView::PositionAtCenter);
    }
    auto *counts = new QLabel(
        QStringLiteral("<b>Commands in this run</b> — %1 · %2 allowed · %3 asked first · "
                       "%4 blocked or denied")
            .arg(rows.size())
            .arg(allowed)
            .arg(asked)
            .arg(refused));
    layout->addWidget(counts);
    layout->addWidget(list, 3);

    // Every permission that decides those chips.
    const forkmesh::agentrules::RunRules runRules = agentRunRules(sid);
    const Access access = runRules.access;
    const forkmesh::projects::GlobalAgentRules globalRules = globalAgentRules();
    QString mode = session ? session->mode : QString();
    if (mode.isEmpty())
        mode = QSettings().value(kClaudeAutoModeSetting, true).toBool() ? kClaudeAutoModeLabel
                                                                       : kAgentAskModeLabel;
    QStringList runHosts;
    const QString prefix = QString::number(sid) + QLatin1Char(':');
    for (auto it = m_agentGateSessionHosts.cbegin(); it != m_agentGateSessionHosts.cend();
         ++it)
        if (it.key().startsWith(prefix))
            for (const QString &host : it.value())
                runHosts << host;
    runHosts.sort();
    QStringList offFamilies;
    for (const QString &id : AgentToolPolicy::disabledFamiliesFromOptions(
             agentRunLaunchOptions(sid, m_codexStreams.contains(sid))))
        if (const AgentToolFamily *family = agentToolFamily(id))
            offFamilies << family->displayName;

    QString folders;
    if (access.folders == Access::Folders::Unrestricted)
        folders = QStringLiteral("Unrestricted: no folder boundary beyond the repository.");
    else if (!forkmesh::projects::folderLockSupported() || forkmesh::vm::active())
        folders = QStringLiteral("Held to the project folder where the platform can "
                                 "enforce it; not enforced here.");
    else
        folders = QStringLiteral("Held to the worktree and the project folder%1.")
                      .arg(access.sharedTools
                               ? QStringLiteral(", plus shared tools and scratch folders")
                               : QString());
    QString html = QStringLiteral("<table cellspacing='0' cellpadding='3'>");
    const auto addRow = [&html](const QString &name, const QString &value) {
        html += QStringLiteral("<tr><td valign='top'><b>%1</b></td><td>%2</td></tr>")
                    .arg(name.toHtmlEscaped(), value);
    };
    addRow(QStringLiteral("Permission mode"),
           QStringLiteral("%1 — %2.").arg(mode.toHtmlEscaped(), modeMeaning(mode)));
    addRow(QStringLiteral("Folders"), folders.toHtmlEscaped());
    if (!access.readOnlyFolders.isEmpty())
        addRow(QStringLiteral("Extra read-only"), htmlList(access.readOnlyFolders, QString()));
    if (!access.readWriteFolders.isEmpty())
        addRow(QStringLiteral("Extra read-write"),
               htmlList(access.readWriteFolders, QString()));
    addRow(QStringLiteral("External calls"),
           (networkMeaning(access.network) +
            (forkmesh::vm::active() ? QStringLiteral(" The VM is its own boundary, so the "
                                                     "gate is off.")
                                    : QString()))
               .toHtmlEscaped());
    addRow(QStringLiteral("Approved destinations"),
           htmlList(access.allowedHosts, QStringLiteral("none")));
    addRow(QStringLiteral("Allowed for this run"),
           htmlList(runHosts, QStringLiteral("none")));
    addRow(QStringLiteral("Project rules"),
           access.followGlobal ? QStringLiteral("Follows the global agent rules.")
                               : QStringLiteral("Its own settings, plus the global extra "
                                                "folders and destinations."));
    addRow(QStringLiteral("Blocked destinations"),
           htmlList(access.blockedHosts, QStringLiteral("none")));
    addRow(QStringLiteral("Destructive commands"),
           globalRules.destructive == forkmesh::projects::GlobalAgentRules::Destructive::Block
               ? QStringLiteral("Blocked.")
           : globalRules.destructive == forkmesh::projects::GlobalAgentRules::Destructive::Allow
               ? QStringLiteral("Run without asking.")
               : QStringLiteral("Wait for your approval."));
    if (globalRules.destructive == forkmesh::projects::GlobalAgentRules::Destructive::Ask)
    {
        addRow(QStringLiteral("Always allowed destructive"),
               htmlList(access.allowedDestructive, QStringLiteral("none")));
        addRow(QStringLiteral("Recursive deletes allowed in"),
               htmlList(access.allowedDeletePaths, QStringLiteral("none")));
    }
    if (!runRules.teammateName.isEmpty())
        addRow(QStringLiteral("Teammate permissions not granted"),
               htmlList(runRules.deniedPermissions, QStringLiteral("none")));
    addRow(QStringLiteral("Tools switched off"),
           htmlList(offFamilies, QStringLiteral("none")));
    html += QStringLiteral("</table>");
    auto *permissionsTitle = new QLabel(QStringLiteral("<b>Permissions</b>"));
    auto *permissions = new QLabel(html);
    permissions->setObjectName(QStringLiteral("agentCommandPermissionsSummary"));
    permissions->setTextFormat(Qt::RichText);
    permissions->setWordWrap(true);
    permissions->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(permissionsTitle);
    layout->addWidget(permissions);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton *settings = buttons->addButton(QStringLiteral("Project settings…"),
                                               QDialogButtonBox::ActionRole);
    settings->setObjectName(QStringLiteral("agentCommandPermissionsProjectSettings"));
    settings->setToolTip(QStringLiteral(
        "Change the folders agents may reach, how external calls are handled and "
        "the approved destinations"));
    QPushButton *globalRulesButton = buttons->addButton(QStringLiteral("Global rules…"),
                                                        QDialogButtonBox::ActionRole);
    globalRulesButton->setObjectName(QStringLiteral("agentCommandPermissionsGlobalRules"));
    connect(globalRulesButton, &QPushButton::clicked, this, [this, dialog] {
        dialog->close();
        showGlobalAgentRules();
    });
    QPushButton *usage = buttons->addButton(QStringLiteral("Tool usage…"),
                                            QDialogButtonBox::ActionRole);
    usage->setObjectName(QStringLiteral("agentCommandPermissionsToolUsage"));
    usage->setToolTip(QStringLiteral(
        "Every tool and command the agents ran, sortable by calls, errors, time and "
        "context cost; switch tool families on or off"));
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(settings, &QPushButton::clicked, this, [this, dialog] {
        dialog->close();
        showProjectSettings(forkmesh::paths::projectSlug());
    });
    connect(usage, &QPushButton::clicked, this, [this, dialog] {
        dialog->close();
        setAgentPageTab(1);
        showAgentToolUsageTable(true);
    });
    layout->addWidget(buttons);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

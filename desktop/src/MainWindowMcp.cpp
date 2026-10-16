
#include "ControlNode.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "McpConnector.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

using namespace forkmesh::ui;

namespace {

QString pythonCommand()
{
    const QString found = QStandardPaths::findExecutable(QStringLiteral("python3"));
    return found.isEmpty() ? QStringLiteral("python3") : found;
}

QString mcpAppDataDir()
{
    return forkmesh::paths::dataRoot();
}

const QRegularExpression &genieTaskLineRe()
{
    static const QRegularExpression re(
        QStringLiteral("FORKMESH_TASK[ \\t]+([A-Za-z0-9_-]{1,32})[ \\t]*"
                       "[:\\-\\x{2013}\\x{2014}][ \\t]*([^\\n]{1,200})"));
    return re;
}

} // namespace

QString MainWindow::mcpServerScriptPath() const
{
    return forkmesh::control::findMcpServerScript(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
}

QWidget *MainWindow::buildMcpConnectorTab()
{
    auto *body = new QWidget;
    auto *col = new QVBoxLayout(body);
    col->setContentsMargins(2, 14, 2, 14);
    col->setSpacing(10);

    auto *headLabel = new QLabel("MCP CONNECTOR");
    headLabel->setObjectName("sectionLabel");

    auto *hint = new QLabel(
        "Connect any MCP-capable agent \xe2\x80\x94 Claude Code, Codex, an "
        "editor extension, your own script \xe2\x80\x94 to this node. The agent "
        "gets narrow tools for repository code, signed Markdown notes, "
        "World chat, issues, plans, and pull requests. Generate a connector "
        "token below and paste the config into the agent; from then on it works "
        "your tasks with this node's identity, without a shell or general "
        "filesystem access.");
    hint->setObjectName("statusLine");
    hint->setWordWrap(true);

    m_mcpStatusLabel = new QLabel;
    m_mcpStatusLabel->setObjectName("modeHint");
    m_mcpStatusLabel->setWordWrap(true);
    m_mcpStatusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *tokenLabel = new QLabel("CONNECTOR TOKEN");
    tokenLabel->setObjectName("sectionLabel");

    m_mcpTokenEdit = new QLineEdit;
    m_mcpTokenEdit->setReadOnly(true);
    m_mcpTokenEdit->setPlaceholderText(
        "No connector token \xe2\x80\x94 agents get read-only tools");
    m_mcpTokenEdit->setToolTip(
        "The secret an agent presents as FORKMESH_MCP_TOKEN. Anything holding "
        "it can write signed issues, comments and pull requests as this node.");

    m_mcpGenerateButton = new QPushButton("Generate token");
    m_mcpGenerateButton->setObjectName("primaryButton");
    m_mcpGenerateButton->setCursor(Qt::PointingHandCursor);
    connect(m_mcpGenerateButton, &QPushButton::clicked, this,
            &MainWindow::generateMcpConnector);

    auto *tokenStatus = new QLabel;
    tokenStatus->setObjectName(QStringLiteral("mcpTokenStatus"));
    tokenStatus->setWordWrap(true);
    tokenStatus->hide();

    auto *copyTokenButton = new QPushButton("Copy token");
    copyTokenButton->setCursor(Qt::PointingHandCursor);
    connect(copyTokenButton, &QPushButton::clicked, this, [this, tokenStatus] {
        const QString token =
            forkmesh::mcp::loadConnector(mcpAppDataDir()).token;
        if (token.isEmpty()) {
            showLocalNotice(tokenStatus, "Generate a connector token first.", true);
            return;
        }
        QApplication::clipboard()->setText(token);
        showLocalNotice(tokenStatus, "Connector token copied.", false);
    });

    m_mcpRevokeButton = new QPushButton("Revoke");
    m_mcpRevokeButton->setCursor(Qt::PointingHandCursor);
    m_mcpRevokeButton->setToolTip(
        "Delete the token. Every agent still configured with it drops to "
        "read-only immediately.");
    connect(m_mcpRevokeButton, &QPushButton::clicked, this,
            &MainWindow::revokeMcpConnector);

    auto *tokenRow = new QHBoxLayout;
    tokenRow->setContentsMargins(0, 0, 0, 0);
    tokenRow->addWidget(m_mcpTokenEdit, 1);
    tokenRow->addWidget(m_mcpGenerateButton);
    tokenRow->addWidget(copyTokenButton);
    tokenRow->addWidget(m_mcpRevokeButton);

    auto *configLabel = new QLabel("AGENT CONFIGURATION");
    configLabel->setObjectName("sectionLabel");

    auto *configHint = new QLabel(
        "Paste this into the agent's MCP configuration \xe2\x80\x94 "
        "<code>.mcp.json</code> at the root of a project for Claude Code, or "
        "<code>claude_desktop_config.json</code> for Claude Desktop. Restart "
        "the agent afterwards so it picks the server up.");
    configHint->setObjectName("statusLine");
    configHint->setTextFormat(Qt::RichText);
    configHint->setWordWrap(true);

    m_mcpConfigEdit = new QPlainTextEdit;
    m_mcpConfigEdit->setReadOnly(true);
    m_mcpConfigEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_mcpConfigEdit->setMinimumHeight(170);
    applyLogFont(m_mcpConfigEdit);

    auto *copyConfigButton = new QPushButton("Copy config");
    copyConfigButton->setObjectName("primaryButton");
    copyConfigButton->setCursor(Qt::PointingHandCursor);
    connect(copyConfigButton, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_mcpConfigEdit->toPlainText());
        showLocalNotice(m_mcpTestLabel, "MCP configuration copied.", false);
    });

    auto *copyCommandButton = new QPushButton("Copy CLI command");
    copyCommandButton->setCursor(Qt::PointingHandCursor);
    copyCommandButton->setToolTip(
        "One-liner that registers the connector for every Claude Code project "
        "on this machine.");
    connect(copyCommandButton, &QPushButton::clicked, this, [this] {
        const QString script = mcpServerScriptPath();
        if (script.isEmpty()) {
            showLocalNotice(m_mcpTestLabel,
                            "Could not find forkmesh_mcp_server.py.", true);
            return;
        }
        QApplication::clipboard()->setText(forkmesh::mcp::cliCommand(
            pythonCommand(), script, repositoryMirrorRoot(),
            forkmesh::mcp::loadConnector(mcpAppDataDir()).token));
        showLocalNotice(m_mcpTestLabel, "Claude Code command copied.", false);
    });

    m_mcpTestButton = new QPushButton("Test connection");
    m_mcpTestButton->setCursor(Qt::PointingHandCursor);
    m_mcpTestButton->setToolTip(
        "Launch the server exactly as the agent would and list the tools it "
        "answers with.");
    connect(m_mcpTestButton, &QPushButton::clicked, this,
            &MainWindow::testMcpConnector);

    m_mcpTestLabel = new QLabel;
    m_mcpTestLabel->setObjectName("modeHint");
    m_mcpTestLabel->setWordWrap(true);
    m_mcpTestLabel->hide();

    auto *configButtonRow = new QHBoxLayout;
    configButtonRow->setContentsMargins(0, 0, 0, 0);
    configButtonRow->addWidget(copyConfigButton);
    configButtonRow->addWidget(copyCommandButton);
    configButtonRow->addWidget(m_mcpTestButton);
    configButtonRow->addStretch();

    auto *usageLabel = new QLabel("HOW AGENTS USE IT");
    usageLabel->setObjectName("sectionLabel");

    auto *usage = new QLabel(QString::fromUtf8(
        "<b>1. Connect.</b> Generate a token, copy the config into the agent, "
        "restart it. Ask the agent to call <code>whoami</code> \xe2\x80\x94 it "
        "answers with this node's public key and whether it may write.<br>"
        "<b>2. Inspect.</b> <code>list_repositories</code>, "
        "<code>list_files</code>, <code>search_code</code>, and "
        "<code>read_file</code> browse tracked code at a Git ref; "
        "<code>search_issues</code> finds open tasks.<br>"
        "<b>3. Do the work.</b> The agent edits a checkout and commits on a "
        "branch the way it always does \xe2\x80\x94 the connector is for mesh "
        "operations, not for editing files.<br>"
        "<b>4. Keep context.</b> <code>create_note</code>, "
        "<code>search_notes</code>, and <code>update_note</code> keep signed "
        "Markdown context; <code>create_cloud_note</code> adds a note to "
        "the Notes page; <code>read_chat_messages</code> and "
        "<code>post_chat_message</code> connect only to the World chat.<br>"
        "<b>5. Report back.</b> <code>comment_on_issue</code> posts progress, "
        "<code>create_issue</code> files follow-ups, "
        "<code>create_milestone</code> / <code>create_project</code> / "
        "<code>update_project</code> keep the plan current, and "
        "<code>open_pr_from_branch</code> opens a pull request when you ask for one. "
        "<code>get_pr_diff</code> reads one back for review.<br>"
        "<b>6. Work the CRM.</b> <code>crm_get_pipeline</code>, "
        "<code>crm_list_contacts</code>, <code>crm_get_contact</code>, and "
        "<code>crm_list_activity</code> read an organization's CRM as your "
        "signed-in account; <code>crm_update_contact</code> moves a contact "
        "one stage at a time with a dated evidence note, and "
        "<code>crm_find_duplicates</code> and <code>crm_merge_contacts</code> "
        "fold a person's duplicate record into the one kept. CRM tools need the "
        "token and never send email.<br><br>"
        "Every state change uses normal ForkMesh storage or transport; "
        "identity-bearing records use the same local signing key. There is no "
        "privileged side door. Reads work without a token, so handing out the "
        "config with the <code>FORKMESH_MCP_TOKEN</code> line removed gives an "
        "agent browse-only access; revoking demotes every agent still holding "
        "the old token to exactly that."));
    usage->setObjectName("statusLine");
    usage->setTextFormat(Qt::RichText);
    usage->setWordWrap(true);

    // --- genie ---------------------------------------------------------------
    // The other direction: instead of handing an outside agent this
    // node's mesh, this hands a local agent the *website's* task list. Nothing
    // here has to be filled in any more: the first press of the
    // prompt bar's "genie" button has the relay mint this node's own task-only
    // credential, signed with the account key this install already holds, and
    // saves it into these fields.
    auto *genieLabel = new QLabel("GENIE (WEBSITE TASK LIST)");
    genieLabel->setObjectName("sectionLabel");

    auto *genieHint = new QLabel(QString::fromUtf8(
        "The <b>genie</b> button in the prompt bar starts an agent against "
        "<code>%1</code>: it calls <code>list_org_tasks</code>, takes the "
        "highest-priority unassigned task, works it in an isolated worktree, "
        "and reports back with <code>complete_org_task</code> \xe2\x80\x94 as a "
        "normal running agent session, titled with the task it picked. You do "
        "not need to fill anything in below: pressing genie mints this node's "
        "own revocable task-only credential from the account it is signed in "
        "as. A credential generated on the website (Organization Admin "
        "\xe2\x86\x92 <b>Remote ForkMesh MCP</b>) still works if you paste it "
        "here, and revoking it there cuts every agent still holding it.")
            .arg(genieMcpUrl().toString()));
    genieHint->setObjectName("statusLine");
    genieHint->setTextFormat(Qt::RichText);
    genieHint->setWordWrap(true);

    m_genieTokenEdit = new QLineEdit;
    m_genieTokenEdit->setEchoMode(QLineEdit::Password);
    m_genieTokenEdit->setPlaceholderText(
        "Empty \xe2\x80\x94 pressing genie mints this node's own credential");
    m_genieTokenEdit->setToolTip(
        "The organization bot token this node minted (or one pasted from the "
        "website). Anything holding it can read and complete that "
        "organization's tasks.");

    m_genieOrgEdit = new QLineEdit;
    m_genieOrgEdit->setPlaceholderText("organization");
    m_genieOrgEdit->setToolTip(
        "Named in the prompt so the agent knows whose task list it is working. "
        "The credential itself is what scopes access.");
    m_genieOrgEdit->setMaximumWidth(220);

    m_genieWorkflowCombo = new QComboBox;
    m_genieWorkflowCombo->addItem(QStringLiteral("Finish at a pull request"),
                                  QStringLiteral("pr"));
    m_genieWorkflowCombo->addItem(QStringLiteral("Deploy and verify"),
                                  QStringLiteral("deploy"));
    m_genieWorkflowCombo->setToolTip(
        "Which of the website's two workflows the run follows once the work is "
        "committed.");

    auto *genieSaveButton = new QPushButton("Save genie setup");
    genieSaveButton->setObjectName("primaryButton");
    genieSaveButton->setCursor(Qt::PointingHandCursor);
    connect(genieSaveButton, &QPushButton::clicked, this,
            &MainWindow::saveGenieSettings);

    m_genieStatusLabel = new QLabel;
    m_genieStatusLabel->setObjectName("modeHint");
    m_genieStatusLabel->setWordWrap(true);

    auto *genieRow = new QHBoxLayout;
    genieRow->setContentsMargins(0, 0, 0, 0);
    genieRow->addWidget(m_genieTokenEdit, 1);
    genieRow->addWidget(m_genieOrgEdit);
    genieRow->addWidget(m_genieWorkflowCombo);
    genieRow->addWidget(genieSaveButton);

    col->addWidget(headLabel);
    col->addWidget(hint);
    col->addWidget(m_mcpStatusLabel);
    col->addSpacing(6);
    col->addWidget(tokenLabel);
    col->addLayout(tokenRow);
    col->addWidget(tokenStatus);
    col->addSpacing(6);
    col->addWidget(configLabel);
    col->addWidget(configHint);
    col->addWidget(m_mcpConfigEdit);
    col->addLayout(configButtonRow);
    col->addWidget(m_mcpTestLabel);
    col->addSpacing(6);
    col->addWidget(genieLabel);
    col->addWidget(genieHint);
    col->addLayout(genieRow);
    col->addWidget(m_genieStatusLabel);
    col->addSpacing(6);
    col->addWidget(usageLabel);
    col->addWidget(usage);
    col->addStretch();

    refreshMcpConnectorTab();
    return body;
}

void MainWindow::refreshMcpConnectorTab()
{
    if (!m_mcpStatusLabel || !m_mcpTokenEdit || !m_mcpConfigEdit)
        return;

    const QString script = mcpServerScriptPath();
    const forkmesh::mcp::Connector connector =
        forkmesh::mcp::loadConnector(mcpAppDataDir());

    QStringList status;
    status << (script.isEmpty()
                   ? QString::fromUtf8(
                         "\xe2\x9a\xa0 forkmesh_mcp_server.py was not found in "
                         "this installation \xe2\x80\x94 the configuration "
                         "below cannot be used until it is.")
                   : QStringLiteral("Server: %1").arg(script));
    status << QStringLiteral("Identity: %1")
                  .arg(m_profileIdentity.publicKey().isEmpty()
                           ? QStringLiteral("not loaded")
                           : m_profileIdentity.publicKey());
    status << (connector.isValid()
                   ? QStringLiteral("Access: read + write, granted %1")
                         .arg(QDateTime::fromMSecsSinceEpoch(connector.createdMs)
                                  .toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                   : QString::fromUtf8(
                         "Access: read-only \xe2\x80\x94 no token has been "
                         "generated yet"));
    m_mcpStatusLabel->setText(status.join(QLatin1Char('\n')));

    m_mcpTokenEdit->setText(forkmesh::mcp::maskToken(connector.token));
    if (m_mcpGenerateButton)
        m_mcpGenerateButton->setText(connector.isValid() ? "Regenerate token"
                                                         : "Generate token");
    if (m_mcpRevokeButton)
        m_mcpRevokeButton->setEnabled(connector.isValid());

    m_mcpConfigEdit->setPlainText(forkmesh::mcp::configJson(
        pythonCommand(),
        script.isEmpty() ? QStringLiteral("/path/to/forkmesh_mcp_server.py")
                         : script,
        repositoryMirrorRoot(), connector.token));

    // Genie: show the saved website credential and workflow.
    const QSettings settings;
    const QString genieToken =
        settings.value(kGenieTokenSetting).toString().trimmed();
    if (m_genieTokenEdit && m_genieTokenEdit->text().trimmed() != genieToken)
        m_genieTokenEdit->setText(genieToken);
    if (m_genieOrgEdit)
        m_genieOrgEdit->setText(settings.value(kGenieOrgSetting).toString());
    if (m_genieWorkflowCombo) {
        const int idx = m_genieWorkflowCombo->findData(
            settings.value(kGenieWorkflowSetting, QStringLiteral("pr")).toString());
        if (idx >= 0)
            m_genieWorkflowCombo->setCurrentIndex(idx);
    }
    if (m_genieStatusLabel)
        m_genieStatusLabel->setText(
            genieToken.isEmpty()
                ? QString::fromUtf8("Genie: no credential yet \xe2\x80\x94 the "
                                 "prompt bar's genie button mints one against "
                                 "%1 on its first press.")
                      .arg(genieMcpUrl().toString())
                : QStringLiteral("Genie: ready against %1.")
                      .arg(genieMcpUrl().toString()));
}

void MainWindow::generateMcpConnector()
{
    forkmesh::mcp::Connector connector;
    connector.token = forkmesh::mcp::generateToken();
    connector.node = m_profileIdentity.publicKey();
    connector.label = machineNodeName();
    connector.createdMs = QDateTime::currentMSecsSinceEpoch();

    QString error;
    auto *tokenStatus = findChild<QLabel *>(QStringLiteral("mcpTokenStatus"));
    if (!forkmesh::mcp::saveConnector(mcpAppDataDir(), connector, &error)) {
        showLocalNotice(tokenStatus, error, true);
        return;
    }
    refreshMcpConnectorTab();
    QApplication::clipboard()->setText(connector.token);
    showLocalNotice(tokenStatus,
                    "Connector token generated and copied to the clipboard. "
                    "Paste the configuration below into your agent.",
                    false);
}

void MainWindow::revokeMcpConnector()
{
    QString error;
    auto *tokenStatus = findChild<QLabel *>(QStringLiteral("mcpTokenStatus"));
    if (!forkmesh::mcp::revokeConnector(mcpAppDataDir(), &error)) {
        showLocalNotice(tokenStatus, error, true);
        return;
    }
    refreshMcpConnectorTab();
    showLocalNotice(tokenStatus,
                    "Connector revoked. Agents holding the old token are now "
                    "read-only.",
                    false);
}

void MainWindow::testMcpConnector()
{
    if (!m_mcpTestLabel || !m_mcpTestButton)
        return;
    if (m_mcpTestProcess)
        return; // a probe is already running

    const QString script = mcpServerScriptPath();
    if (script.isEmpty()) {
        m_mcpTestLabel->setText(
            "Could not find forkmesh_mcp_server.py in this installation.");
        m_mcpTestLabel->show();
        return;
    }

    m_mcpTestButton->setEnabled(false);
    m_mcpTestLabel->setStyleSheet(QString());
    m_mcpTestLabel->setText(QString::fromUtf8("Starting the server\xe2\x80\xa6"));
    m_mcpTestLabel->show();

    auto *proc = new QProcess(this);
    m_mcpTestProcess = proc;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString reposDir = repositoryMirrorRoot();
    if (!reposDir.isEmpty())
        env.insert(QStringLiteral("FORKMESH_REPOS_DIR"), reposDir);
    const QString token = forkmesh::mcp::loadConnector(mcpAppDataDir()).token;
    if (!token.isEmpty())
        env.insert(QStringLiteral("FORKMESH_MCP_TOKEN"), token);
    proc->setProcessEnvironment(env);

    // Drive the same stdio handshake an agent performs: initialize, then ask
    // for the tool list. Anything less would be testing a different code path
    // than the one that actually has to work.
    connect(proc, &QProcess::finished, this,
            [this, proc](int, QProcess::ExitStatus) {
                proc->deleteLater();
                m_mcpTestProcess = nullptr;
                if (m_mcpTestButton)
                    m_mcpTestButton->setEnabled(true);
                if (!m_mcpTestLabel)
                    return;
                QStringList tools;
                bool writable = false;
                const QList<QByteArray> lines =
                    proc->readAllStandardOutput().split('\n');
                for (const QByteArray &line : lines) {
                    const QJsonObject result =
                        QJsonDocument::fromJson(line).object()
                            .value(QStringLiteral("result")).toObject();
                    const QJsonArray listed =
                        result.value(QStringLiteral("tools")).toArray();
                    for (const QJsonValue &tool : listed)
                        tools << tool.toObject()
                                     .value(QStringLiteral("name")).toString();
                    writable = writable ||
                               result.value(QStringLiteral("instructions"))
                                   .toString()
                                   .contains(QStringLiteral("may write"));
                }
                if (tools.isEmpty()) {
                    const QString err =
                        QString::fromUtf8(proc->readAllStandardError()).trimmed();
                    m_mcpTestLabel->setText(
                        QStringLiteral("The server did not answer. %1")
                            .arg(err.isEmpty()
                                     ? QStringLiteral(
                                           "Check that python3 and the "
                                           "'cryptography' package are installed.")
                                     : err.section(QLatin1Char('\n'), -1)));
                    return;
                }
                m_mcpTestLabel->setText(
                    QStringLiteral("Connected. %1 tools available (%2); this "
                                   "connector may %3.")
                        .arg(tools.size())
                        .arg(tools.join(QStringLiteral(", ")),
                             writable ? QStringLiteral("read and write")
                                      : QStringLiteral("read only")));
            });
    connect(proc, &QProcess::errorOccurred, this, [this, proc] {
        if (m_mcpTestProcess != proc)
            return;
        proc->deleteLater();
        m_mcpTestProcess = nullptr;
        if (m_mcpTestButton)
            m_mcpTestButton->setEnabled(true);
        if (m_mcpTestLabel)
            m_mcpTestLabel->setText(
                "Could not launch python3. Install Python 3 and the "
                "'cryptography' package, then try again.");
    });

    proc->start(pythonCommand(), {script});
    const QByteArray handshake =
        QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"
                          "\"initialize\",\"params\":{\"protocolVersion\":"
                          "\"2025-06-18\"}}\n"
                          "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":"
                          "\"tools/list\"}\n");
    proc->write(handshake);
    proc->closeWriteChannel(); // the server exits when stdin closes

    QTimer::singleShot(15000, proc, [proc] {
        if (proc->state() != QProcess::NotRunning)
            proc->kill();
    });
}

// --- Genie -----------------------------------------------------
// The website's Organization Admin page can mint a revocable, task-only bearer
// credential for the relay's own Streamable HTTP MCP server (<origin>/mcp),
// which exposes list_org_tasks / get_org_task / complete_org_task. Pasting that
// setup into an agent by hand is exactly the friction the "genie" button
// removes: press it and this app starts a normal tracked agent session whose
// opening prompt is that same setup block, so the agent picks its own work off
// the shared task list instead of running something typed here.

QUrl MainWindow::genieMcpUrl() const
{
    const QString configured =
        QSettings().value(kGenieUrlSetting).toString().trimmed();
    if (!configured.isEmpty()) {
        const QUrl url(configured);
        if (url.isValid() && !url.host().isEmpty())
            return url;
    }
    // Default to the active relay: the credential the website mints is scoped to
    // that same origin, so pointing anywhere else could only fail to authorize.
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/mcp"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QString MainWindow::genieSetupPrompt(const QString &extraInstruction) const
{
    const QSettings settings;
    const QString token = settings.value(kGenieTokenSetting).toString().trimmed();
    const QString org = settings.value(kGenieOrgSetting).toString().trimmed();
    const bool deploy =
        settings.value(kGenieWorkflowSetting, QStringLiteral("pr")).toString() ==
        QLatin1String("deploy");

    const QJsonObject config{
        {QStringLiteral("mcpServers"),
         QJsonObject{
             {QStringLiteral("forkmesh-tasks"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("http")},
                  {QStringLiteral("url"), genieMcpUrl().toString()},
                  {QStringLiteral("headers"),
                   QJsonObject{{QStringLiteral("Authorization"),
                                QStringLiteral("Bearer ") + token}}},
              }},
         }},
    };

    QStringList lines;
    lines << QStringLiteral(
        "Configure the following remote Streamable HTTP MCP server named "
        "forkmesh-tasks, then immediately use it for this assignment. Add it as "
        "a new server: leave any existing MCP server this checkout configures "
        "(including one named forkmesh) exactly as it is.")
          << QString()
          << QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Indented))
          << QStringLiteral("Work the shared task list for the %1 organization.")
                 .arg(org.isEmpty() ? QStringLiteral("connected") : org)
          << QStringLiteral(
                 "1. Call list_org_tasks on forkmesh-tasks. Select the "
                 "highest-priority unfinished task routed to an agent or "
                 "explicitly unassigned. Never take a task assigned to a "
                 "person.")
          << QStringLiteral(
                 "2. The moment you have chosen, print one line of its own in "
                 "exactly this form, before doing anything else:\n"
                 "   FORKMESH_TASK <task id>: <task title>\n"
                 "   ForkMesh reads that line and shows the title live beside "
                 "this run, so print it again if you ever switch tasks.")
          << QStringLiteral(
                 "3. Call get_org_task for that id and read its repository "
                 "instructions before planning.")
          << QStringLiteral(
                 "4. ForkMesh has already prepared the isolated worktree and "
                 "branch you are running in. Work only here: never stash, reset, "
                 "clean, or commit in any other checkout, and preserve unrelated "
                 "changes.")
          << QStringLiteral(
                 "5. Implement the smallest complete fix, then run focused tests "
                 "plus the repository's required checks.")
          << QStringLiteral(
                 "6. Commit only the task's files with a clear message. Never "
                 "force-push or bypass failing checks or conflicts.");
    if (deploy) {
        lines << QStringLiteral(
            "7. Integrate the latest main into this branch without "
            "force-pushing, merge only when the merge and required checks are "
            "clean, then deploy with the repository's documented deploy command "
            "and verify the live revision.");
        lines << QStringLiteral(
            "8. Call complete_org_task with the commit, deployed revision, the "
            "tests you ran, and the exact QA steps.");
    } else {
        lines << QStringLiteral(
            "7. Leave the work on this branch. Do not open a pull request, "
            "do not merge, and do not deploy.");
        lines << QStringLiteral(
            "8. Call complete_org_task with the branch, the tests you ran, and "
            "the exact QA steps.");
    }
    lines << QStringLiteral(
        "If credentials, authorization, infrastructure, tests, or conflict "
        "resolution block safe completion, report the blocker and do not mark "
        "the task complete. Treat the bearer credential above as a secret: never "
        "print it in logs, commits, pull requests, task notes, or chat.");
    const QString extra = extraInstruction.trimmed();
    if (!extra.isEmpty())
        lines << QString()
              << QStringLiteral("Additional instruction from the operator:")
              << extra;
    return lines.join(QLatin1Char('\n'));
}

void MainWindow::startGenieAgent()
{
    const int repoIndex = issuesRepoIndex();
    if (repoIndex < 0) {
        setIssueInlineNotice("Open a repository first \xE2\x80\x94 genie runs "
                             "in its worktree.",
                             true);
        return;
    }
    const QString typed =
        m_issueQuickAdd ? m_issueQuickAdd->toPlainText().trimmed() : QString();
    const QString token =
        QSettings().value(kGenieTokenSetting).toString().trimmed();
    if (token.isEmpty()) {
        // Nothing to configure by hand: this install is already signed in as its
        // account, so it mints its own task-only credential and then launches.
        requestGenieCredential(repoIndex, typed);
        return;
    }
    launchGenieRun(repoIndex, typed);
}

// Ask the relay for this desktop's task-only remote-MCP bearer, signed with the
// account key, then start the run. Nothing is pasted, nothing is
// generated on the website, and the credential is stored exactly where a pasted
// one would be — so a later press reuses it and Settings → MCP can still show
// and clear it.
void MainWindow::requestGenieCredential(int repoIndex,
                                        const QString &typedGuidance)
{
    if (m_genieCredentialPending)
        return;
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    if (!m_networkAccess) {
        setIssueInlineNotice("Genie needs the relay to mint its task credential.",
                             true);
        return;
    }
    const QString repoOwner = m_repositories.at(repoIndex).owner;
    const QString repoName = m_repositories.at(repoIndex).name;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/genie/credential"));
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest request;
    if (!authenticateOrgTaskRequest(url, request, kGenieCredentialProof,
                                    QString())) {
        setIssueInlineNotice(QString::fromUtf8(
                                 "Genie needs an account on this node \xE2\x80\x94 "
                                 "sign in (or import your key) and press genie "
                                 "again."),
                             true);
        return;
    }
    m_genieCredentialPending = true;
    setIssueInlineNotice(
        QString::fromUtf8("Genie is getting its own task credential\xE2\x80\xA6"));
    const QJsonObject body{
        {QStringLiteral("deviceName"), machineNodeName()},
    };
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repoOwner, repoName, typedGuidance] {
                const QByteArray payload = reply->readAll();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                reply->deleteLater();
                m_genieCredentialPending = false;
                const QJsonObject obj =
                    QJsonDocument::fromJson(payload).object();
                const QString token =
                    obj.value(QStringLiteral("token")).toString().trimmed();
                if (status < 200 || status >= 300 || token.isEmpty()) {
                    const QString error =
                        obj.value(QStringLiteral("error")).toString();
                    setIssueInlineNotice(
                        error == QLatin1String("forbidden")
                            ? QString::fromUtf8(
                                  "Genie needs an owner or admin of the task "
                                  "board's organization; this account is not "
                                  "one.")
                            : QStringLiteral("Genie could not mint its task "
                                             "credential (relay replied %1).")
                                  .arg(status),
                        true);
                    return;
                }
                QSettings settings;
                settings.setValue(kGenieTokenSetting, token);
                const QString org =
                    obj.value(QStringLiteral("organization")).toString().trimmed();
                if (!org.isEmpty())
                    settings.setValue(kGenieOrgSetting, org);
                // The relay names the origin the credential is scoped to; honour
                // it over the default guess when it is a usable URL, since a
                // credential minted for one origin authorizes nowhere else.
                const QUrl mcpUrl(
                    obj.value(QStringLiteral("mcpUrl")).toString().trimmed());
                if (mcpUrl.isValid() && !mcpUrl.host().isEmpty())
                    settings.setValue(kGenieUrlSetting, mcpUrl.toString());
                refreshMcpConnectorTab(); // Settings shows the minted credential
                int repoIndex = -1;
                for (int i = 0; i < m_repositories.size(); ++i) {
                    if (m_repositories.at(i).owner == repoOwner &&
                        m_repositories.at(i).name == repoName) {
                        repoIndex = i;
                        break;
                    }
                }
                if (repoIndex < 0) {
                    setIssueInlineNotice(
                        QString::fromUtf8("Genie has its credential, but "
                                          "%1/%2 is no longer open \xE2\x80"
                                          "\x94 press genie again.")
                            .arg(repoOwner, repoName),
                        true);
                    return;
                }
                launchGenieRun(repoIndex, typedGuidance);
            });
}

void MainWindow::launchGenieRun(int repoIndex, const QString &typedGuidance)
{
    if (!typedGuidance.isEmpty())
        recordQuickAddHistory(typedGuidance);

    // Register the embedded bearer with localProviderCredentialValues() so it
    // cannot enter transcripts, logs, or owner-sealed snapshots.

    const int sessionId = startAdHocAgentForRepo(
        repoIndex, genieSetupPrompt(typedGuidance),
        QStringLiteral("claude-code"),
        /*createPr=*/false,
        QSettings().value(kClaudeCodeModelSetting).toString(),
        QString::fromUtf8("Genie \xE2\x80\x94 picking a task\xE2\x80\xA6"),
        /*genie=*/true);
    if (sessionId <= 0)
        return;
    if (m_issueQuickAdd)
        m_issueQuickAdd->clear();
    clearQuickAddImages();
    setIssueInlineNotice(
        QString::fromUtf8("Genie is running as agent session #%1 \xE2\x80\x94 "
                          "the task title appears on that session once it has "
                          "chosen.")
            .arg(sessionId));
}

void MainWindow::applyGenieTaskTitle(int sessionId, const QString &assistantText)
{
    if (sessionId <= 0 || !m_agentStore ||
        !assistantText.contains(QLatin1String("FORKMESH_TASK")))
        return;
    const QRegularExpressionMatch match = genieTaskLineRe().match(assistantText);
    if (!match.hasMatch())
        return;
    QString title = match.captured(2).trimmed();
    while (!title.isEmpty() &&
           (title.endsWith(QLatin1Char('*')) || title.endsWith(QLatin1Char('`')) ||
            title.endsWith(QLatin1Char('_'))))
        title.chop(1);
    title = title.trimmed();
    if (title.isEmpty())
        return;
    title = QString::fromUtf8("Genie \xE2\x80\x94 ") + title;
    if (title.size() > 80)
        title = title.left(77) + QString::fromUtf8("\xE2\x80\xA6");

    {
        // Scoped: reloadAgents() below rebuilds m_agentSessions, so the pointer
        // must not outlive this block (the git-pump UAF family).
        AgentSession *session = findAgentSession(sessionId);
        if (!session || session->issueTitle == title)
            return;
        session->issueTitle = title;
        session->titledBy.clear();
        m_agentStore->saveSession(*session);
    }
    if (m_streamSessionInfo.contains(sessionId)) {
        AgentSession info = m_streamSessionInfo.value(sessionId);
        info.issueTitle = title;
        info.titledBy.clear();
        m_streamSessionInfo[sessionId] = info;
    }
    logSystem(QStringLiteral("Genie: session #%1 is working \"%2\" (task %3).")
                  .arg(sessionId)
                  .arg(title, match.captured(1)));
    reloadAgents();
    scheduleAgentSessionsPush(); // the website's session list shows it too
}

void MainWindow::saveGenieSettings()
{
    if (!m_genieTokenEdit || !m_genieOrgEdit || !m_genieWorkflowCombo)
        return;
    QSettings settings;
    const QString token = m_genieTokenEdit->text().trimmed();
    settings.setValue(kGenieTokenSetting, token);
    settings.setValue(kGenieOrgSetting, m_genieOrgEdit->text().trimmed());
    settings.setValue(kGenieWorkflowSetting,
                      m_genieWorkflowCombo->currentData().toString());
    refreshMcpConnectorTab();
    showLocalNotice(m_genieStatusLabel,
                    token.isEmpty()
                        ? QStringLiteral("Genie credential cleared.")
                        : QStringLiteral("Genie is set up. The \"genie\" button "
                                         "in the prompt bar now starts a run."),
                    false);
}

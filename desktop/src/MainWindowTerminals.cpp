#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AgentHub.h"
#include "AgentIntake.h"
#include "McpConnector.h"
#include "TerminalBridge.h"
#include "TerminalTabsPage.h"
#include "TerminalWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

using namespace forkmesh::ui;
namespace bridge = forkmesh::terminalbridge;
namespace intake = forkmesh::agentintake;

namespace {

const QString kLastKindSetting = QStringLiteral("terminals/lastKind");
const QString kLastFolderSetting = QStringLiteral("terminals/lastFolder");
const QString kShareSetting = QStringLiteral("terminals/shareWithAgents");

QJsonObject terminalError(const QString &code, const QString &message = QString())
{
    return intake::errorReply(code, message.isEmpty() ? bridge::errorMessage(code) : message);
}

QJsonObject okReply(QJsonObject reply)
{
    reply.insert(QStringLiteral("ok"), true);
    return reply;
}

bool windowsHost()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

}

QWidget *MainWindow::buildTerminalsSection()
{
    auto *host = new QWidget;
    host->setObjectName(QStringLiteral("terminalsSection"));
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(0);
    auto *note = new QLabel(QStringLiteral(
        "Your terminals are in the side pane on the right. Use the terminal button in the top "
        "bar to show or hide them."));
    note->setObjectName(QStringLiteral("terminalsSectionNote"));
    note->setAlignment(Qt::AlignCenter);
    note->setWordWrap(true);
    layout->addWidget(note, 1);
    m_terminalsSectionHost = host;
    m_terminalsSectionNote = note;
    return host;
}

TerminalTabsPage *MainWindow::ensureTerminalsPage()
{
    if (m_terminalsPage)
        return m_terminalsPage;
    auto *page = new TerminalTabsPage;
    page->setNewTabHandler([this](const QString &kind) {
        if (kind.isEmpty()) {
            showTerminalLauncher();
            return;
        }
        TerminalTabsPage::Info current;
        const bool here = m_terminalsPage &&
                          m_terminalsPage->info(m_terminalsPage->currentId(), &current) &&
                          !current.cwd.isEmpty();
        openUserTerminal(kind, here ? current.cwd : defaultTerminalFolder());
    });
    page->setClosedHandler([this](const QString &id) {
        forgetTerminal(id);
        if (m_terminalsPage && m_terminalsPage->count() == 0)
            setSideRailTerminalsOpen(false);
    });
    const QColor muted = Theme::color(Theme::Role::FgMuted);
    TerminalTabsPage::Icons icons;
    icons.add = QIcon(tintedOcticonPixmap(QStringLiteral("plus"), muted, 14));
    icons.more = QIcon(tintedOcticonPixmap(QStringLiteral("chevron-down"), muted, 14));
    icons.hide = QIcon(tintedOcticonPixmap(QStringLiteral("x"), muted, 12));
    page->setIcons(icons);
    m_terminalsPage = page;
    dockTerminals(m_sideRailTerminalHost != nullptr);
    return page;
}

void MainWindow::dockTerminals(bool sidePane)
{
    if (!m_terminalsPage)
        return;
    if (sidePane && !m_sideRailTerminalHost)
        sidePane = false;
    if (!sidePane)
        ensureSectionBuilt(kTerminalsSectionIndex);
    QWidget *host = sidePane ? m_sideRailTerminalPanel : m_terminalsSectionHost;
    auto *layout = sidePane ? m_sideRailTerminalHost
                            : (m_terminalsSectionHost
                                   ? qobject_cast<QBoxLayout *>(m_terminalsSectionHost->layout())
                                   : nullptr);
    if (!host || !layout)
        return;
    if (m_terminalsPage->parentWidget() != host) {
        layout->addWidget(m_terminalsPage, 1);
        m_terminalsPage->show();
    }
    if (sidePane)
        m_terminalsPage->setHideHandler([this] { setSideRailTerminalsOpen(false); });
    else
        m_terminalsPage->setHideHandler({});
    if (m_terminalsSectionNote)
        m_terminalsSectionNote->setVisible(m_terminalsPage->parentWidget() !=
                                           m_terminalsSectionHost);
}

void MainWindow::showTerminalsPage()
{
    revealTerminals();
}

QString MainWindow::defaultTerminalFolder() const
{
    const int active = activeTeammateRepoIndex();
    if (active >= 0 && active < m_repositories.size()) {
        const QString path = m_repositories.at(active).localPath;
        if (!path.isEmpty() && QDir(path).exists())
            return path;
    }
    const QString last = QSettings().value(kLastFolderSetting).toString();
    if (!last.isEmpty() && QDir(last).exists())
        return last;
    return QDir::homePath();
}

QString MainWindow::openUserTerminal(const QString &kind, const QString &cwd)
{
    QString error;
    const QString id = openTerminalTab(kind, QString(), cwd, QString(), QString(),
                                       QSettings().value(kShareSetting, true).toBool(), &error);
    if (id.isEmpty())
        flashMessage(error, true);
    return id;
}

void MainWindow::toggleTerminals()
{
    if (sideRailTerminalsOpen() && m_sideRail && m_sideRail->isVisible()) {
        setSideRailTerminalsOpen(false);
        return;
    }
    if (!m_terminalsPage || m_terminalsPage->count() == 0)
        openUserTerminal(bridge::ShellKind, defaultTerminalFolder());
    else
        revealTerminals();
    setSideRailTerminalsOpen(sideRailTerminalsOpen());
}

void MainWindow::showTerminalLauncher()
{
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("terminalLauncher"));
    dialog->setWindowTitle(QStringLiteral("Open a terminal"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);
    auto *intro = new QLabel(QStringLiteral(
        "Opens a terminal as a tab in the side pane. Agents this desktop launched, and "
        "Claude Code or Codex started here, can read it and type into it with the ForkMesh "
        "terminal tools while it is shared."));
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("statusLine"));
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    auto *kind = new QComboBox;
    kind->setObjectName(QStringLiteral("terminalLauncherKind"));
    for (const QString &value : bridge::kinds())
        kind->addItem(value == bridge::CommandKind ? QStringLiteral("Command…")
                                                   : bridge::kindLabel(value),
                      value);
    const int lastKind = kind->findData(QSettings().value(kLastKindSetting).toString());
    kind->setCurrentIndex(lastKind >= 0 ? lastKind : 0);
    form->addRow(QStringLiteral("Run"), kind);

    auto *command = new QLineEdit;
    command->setObjectName(QStringLiteral("terminalLauncherCommand"));
    command->setPlaceholderText(QStringLiteral("npm test, htop, ssh host…"));
    command->setMaxLength(bridge::kMaxCommandChars);
    form->addRow(QStringLiteral("Command"), command);

    auto *folder = new QComboBox;
    folder->setObjectName(QStringLiteral("terminalLauncherFolder"));
    QStringList seen;
    const auto addFolder = [folder, &seen](const QString &label, const QString &path) {
        if (path.isEmpty() || seen.contains(path) || !QDir(path).exists())
            return;
        seen << path;
        folder->addItem(label, path);
        folder->setItemData(folder->count() - 1, QDir::toNativeSeparators(path), Qt::ToolTipRole);
    };
    if (m_selectedAgentSessionId > 0)
        addFolder(QStringLiteral("Selected agent's worktree"), sessionWorkdir(m_selectedAgentSessionId));
    const int active = activeTeammateRepoIndex();
    if (active >= 0 && active < m_repositories.size())
        addFolder(shortcutRepoKey(active), m_repositories.at(active).localPath);
    for (int index = 0; index < m_repositories.size(); ++index) {
        const RepositoryRecord &repo = m_repositories.at(index);
        if (!repo.previewOnly && !repo.localPath.isEmpty())
            addFolder(repo.owner + QLatin1Char('/') + repo.name, repo.localPath);
    }
    addFolder(QStringLiteral("Home"), QDir::homePath());
    const int lastFolder = folder->findData(QSettings().value(kLastFolderSetting).toString());
    if (lastFolder >= 0)
        folder->setCurrentIndex(lastFolder);
    form->addRow(QStringLiteral("In"), folder);
    layout->addLayout(form);

    auto *share = new QCheckBox(QStringLiteral("Share with agents"));
    share->setObjectName(QStringLiteral("terminalLauncherShare"));
    share->setChecked(QSettings().value(kShareSetting, true).toBool());
    share->setToolTip(QStringLiteral(
        "Agents can list, read and type into shared terminals. You can change it later "
        "from the Terminals page."));
    layout->addWidget(share);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *open = buttons->addButton(QStringLiteral("Open terminal"),
                                           QDialogButtonBox::AcceptRole);
    open->setObjectName(QStringLiteral("terminalLauncherOpen"));
    open->setDefault(true);
    layout->addWidget(buttons);

    const auto sync = [kind, command, open] {
        const bool custom = kind->currentData().toString() == bridge::CommandKind;
        command->setEnabled(custom);
        open->setEnabled(!custom || !command->text().trimmed().isEmpty());
    };
    connect(kind, &QComboBox::currentIndexChanged, dialog, sync);
    connect(command, &QLineEdit::textChanged, dialog, sync);
    sync();
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, kind, command, folder, share] {
                const QString chosen = kind->currentData().toString();
                const QString path = folder->currentData().toString();
                QSettings settings;
                settings.setValue(kLastKindSetting, chosen);
                settings.setValue(kLastFolderSetting, path);
                settings.setValue(kShareSetting, share->isChecked());
                QString error;
                const QString id = openTerminalTab(
                    chosen, chosen == bridge::CommandKind ? command->text().trimmed() : QString(),
                    path, QString(), QString(), share->isChecked(), &error);
                if (id.isEmpty()) {
                    flashMessage(error, true);
                    return;
                }
                dialog->accept();
            });
    dialog->open();
}

QString MainWindow::openTerminalTab(const QString &kind, const QString &command,
                                    const QString &cwd, const QString &title,
                                    const QString &openedBy, bool shared, QString *error)
{
    if (!ensureTerminalsPage()) {
        if (error)
            *error = QStringLiteral("The terminals pane could not be built.");
        return {};
    }
    if (m_terminalsPage->count() >= bridge::kMaxTabs) {
        if (error)
            *error = QStringLiteral("At most %1 terminals can be open; close one first.")
                         .arg(bridge::kMaxTabs);
        return {};
    }
    const QString chosen = bridge::kinds().contains(kind) ? kind : bridge::ShellKind;
    TerminalTabsPage::Spec spec;
    spec.id = bridge::newTerminalId();
    spec.kind = chosen;
    spec.cwd = !cwd.isEmpty() && QDir(cwd).exists() ? cwd : QDir::homePath();
    spec.title = title;
    spec.openedBy = openedBy;
    spec.shared = shared;
    spec.keepShell = chosen != bridge::ShellKind;
    spec.env = {intake::SocketEnv, intake::TokenEnv, bridge::TerminalIdEnv};
    ensureAgentIntakeServer();
    if (m_agentIntake) {
        const QString token = bridge::newToken();
        m_terminalIntakeTokens.insert(spec.id, token);
        spec.env = {QStringLiteral("%1=%2").arg(intake::SocketEnv, m_agentIntake->socketPath()),
                    QStringLiteral("%1=%2").arg(intake::TokenEnv, token),
                    QStringLiteral("%1=%2").arg(bridge::TerminalIdEnv, spec.id)};
    }
    const bool agentCli = chosen == bridge::ClaudeKind || chosen == bridge::CodexKind;
    const QString config = agentCli ? terminalMcpConfigPath(spec.id) : QString();
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    spec.command = bridge::launchCommand(chosen, command, config, windowsHost(),
                                         python.isEmpty() ? QStringLiteral("python3") : python,
                                         mcpServerScriptPath());
    if (openedBy.isEmpty())
        revealTerminals();
    const QString id = m_terminalsPage->openTab(spec);
    if (openedBy.isEmpty())
        m_terminalsPage->focusCurrent();
    else
        flashMessage(QStringLiteral("%1 opened a terminal tab: %2")
                         .arg(openedBy, bridge::kindLabel(chosen)));
    return id;
}

QString MainWindow::terminalMcpConfigPath(const QString &terminalId)
{
    const QString token = m_terminalIntakeTokens.value(terminalId);
    if (token.isEmpty() || !m_agentIntake)
        return {};
    return writeForkmeshMcpConfig(QStringLiteral("terminals"),
                                  QStringLiteral("mcp-%1.json").arg(terminalId),
                                  {{intake::SocketEnv, m_agentIntake->socketPath()},
                                   {intake::TokenEnv, token},
                                   {bridge::TerminalIdEnv, terminalId}});
}

QString MainWindow::writeForkmeshMcpConfig(const QString &folder, const QString &fileName,
                                           const QJsonObject &extraEnv)
{
    const QString script = mcpServerScriptPath();
    if (script.isEmpty())
        return {};
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    QJsonObject root =
        QJsonDocument::fromJson(forkmesh::mcp::configJson(
                                    python.isEmpty() ? QStringLiteral("python3") : python, script,
                                    repositoryMirrorRoot(),
                                    forkmesh::mcp::loadConnector(forkmesh::paths::dataRoot()).token)
                                    .toUtf8())
            .object();
    QJsonObject servers = root.value(QStringLiteral("mcpServers")).toObject();
    QJsonObject server = servers.value(QStringLiteral("forkmesh")).toObject();
    QJsonObject env = server.value(QStringLiteral("env")).toObject();
    for (auto it = extraEnv.constBegin(); it != extraEnv.constEnd(); ++it)
        env.insert(it.key(), it.value());
    server.insert(QStringLiteral("env"), env);
    servers.insert(QStringLiteral("forkmesh"), server);
    root.insert(QStringLiteral("mcpServers"), servers);
    const QDir dir(forkmesh::paths::dataPath(folder));
    if (!QDir().mkpath(dir.absolutePath()))
        return {};
    const QString path = dir.filePath(fileName);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return {};
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return path;
}

QString MainWindow::terminalCallerForToken(const QString &token) const
{
    if (!token.startsWith(QLatin1String("term.")))
        return {};
    for (auto it = m_terminalIntakeTokens.cbegin(); it != m_terminalIntakeTokens.cend(); ++it)
        if (intake::tokenMatches(it.value(), token))
            return it.key();
    return {};
}

void MainWindow::forgetTerminal(const QString &terminalId)
{
    m_terminalIntakeTokens.remove(terminalId);
    const QString path = QDir(forkmesh::paths::dataPath(QStringLiteral("terminals")))
                             .filePath(QStringLiteral("mcp-%1.json").arg(terminalId));
    if (QFileInfo(path).isFile())
        QFile::remove(path);
}

QJsonObject MainWindow::terminalJson(const QString &terminalId, const QString &callerTerminal) const
{
    TerminalTabsPage::Info info;
    if (!m_terminalsPage || !m_terminalsPage->info(terminalId, &info))
        return {};
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject row{{QStringLiteral("terminal"), info.id},
                    {QStringLiteral("title"), info.title},
                    {QStringLiteral("kind"), info.kind},
                    {QStringLiteral("cwd"), info.cwd},
                    {QStringLiteral("running"), info.running},
                    {QStringLiteral("shared"), info.shared},
                    {QStringLiteral("current"), info.current},
                    {QStringLiteral("cols"), info.cols},
                    {QStringLiteral("rows"), info.rows},
                    {QStringLiteral("opened_by_agent"), !info.openedBy.isEmpty()},
                    {QStringLiteral("idle_ms"),
                     double(now - qMax(info.lastOutputMs, info.startedAtMs))}};
    if (!info.running && info.exitCode >= 0)
        row.insert(QStringLiteral("exit_code"), info.exitCode);
    if (!callerTerminal.isEmpty() && callerTerminal == info.id)
        row.insert(QStringLiteral("self"), true);
    return row;
}

QJsonObject MainWindow::handleTerminalRequest(int sessionId, const QString &terminalId,
                                              const QString &method, const QJsonObject &params)
{
    const bool agent = sessionId > 0;
    if (!agent && terminalId.isEmpty())
        return intake::unauthorizedReply(method);
    QString who;
    if (agent) {
        const QString label = agentWindowLabel(sessionId);
        who = label.isEmpty() ? QStringLiteral("Agent #%1").arg(sessionId)
                              : QStringLiteral("Agent #%1 “%2”").arg(sessionId).arg(label);
    } else {
        TerminalTabsPage::Info caller;
        who = m_terminalsPage && m_terminalsPage->info(terminalId, &caller)
                  ? QStringLiteral("The program in “%1”").arg(caller.title)
                  : QStringLiteral("A ForkMesh terminal");
    }
    const bool writes = method == bridge::OpenMethod || method == bridge::InputMethod ||
                        method == bridge::CloseMethod;
    if (writes && agent) {
        const AgentSession *session = findAgentSession(sessionId);
        const QString mode = session ? agentEffectiveModeLabel(*session) : QString();
        if (forkmesh::agenthub::permissionRank(mode, 0) < bridge::kAgentRankNeeded)
            return terminalError(
                QStringLiteral("mode_too_low"),
                QStringLiteral("This agent runs in %1 mode. Typing into a terminal runs commands "
                               "with your account, so only agents in Auto mode may open, type "
                               "into or close terminals; reading them is allowed.")
                    .arg(mode.isEmpty() ? QStringLiteral("an unknown") : mode));
    }
    if (method == bridge::OpenMethod) {
        bridge::OpenRequest request;
        QString code;
        if (!bridge::parseOpenRequest(params, &request, &code))
            return terminalError(code);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!m_terminalOpenLimiter.allow(now))
            return terminalError(QStringLiteral("rate_limited"),
                                 QStringLiteral("Agents may open at most %1 terminals every %2 "
                                                "minutes.")
                                     .arg(bridge::kOpenLimit)
                                     .arg(bridge::kOpenWindowMs / 60000));
        QString cwd;
        if (!request.owner.isEmpty()) {
            const int index = repoIndexForKey(request.owner + QLatin1Char('/') + request.name);
            if (index < 0)
                return terminalError(QStringLiteral("bad_repository"),
                                     QStringLiteral("%1/%2 has no checkout on this desktop.")
                                         .arg(request.owner, request.name));
            cwd = m_repositories.at(index).localPath;
        } else if (agent) {
            cwd = sessionWorkdir(sessionId);
        } else {
            TerminalTabsPage::Info caller;
            if (m_terminalsPage && m_terminalsPage->info(terminalId, &caller))
                cwd = caller.cwd;
        }
        QString error;
        const QString id = openTerminalTab(request.kind, request.command, cwd, request.title, who,
                                           true, &error);
        if (id.isEmpty())
            return terminalError(QStringLiteral("too_many_terminals"), error);
        m_terminalsPage->noteAgentAction(id, QStringLiteral("%1 opened this terminal").arg(who));
        return okReply(terminalJson(id, terminalId));
    }
    if (!m_terminalsPage) {
        if (method == bridge::ListMethod)
            return okReply({{QStringLiteral("terminals"), QJsonArray()}});
        return terminalError(QStringLiteral("not_found"),
                             QStringLiteral("No terminal with that id is open."));
    }
    if (method == bridge::ListMethod) {
        QJsonArray rows;
        int hidden = 0;
        for (const TerminalTabsPage::Info &info : m_terminalsPage->tabs()) {
            if (!info.shared && info.id != terminalId) {
                ++hidden;
                continue;
            }
            rows.append(terminalJson(info.id, terminalId));
        }
        QJsonObject reply{{QStringLiteral("terminals"), rows}};
        if (hidden > 0)
            reply.insert(QStringLiteral("private_terminals"), hidden);
        return okReply(reply);
    }
    const QString target = bridge::terminalParam(params);
    if (!bridge::validTerminalId(target))
        return terminalError(QStringLiteral("bad_terminal"));
    TerminalTabsPage::Info info;
    TerminalWidget *terminal = m_terminalsPage->terminal(target);
    if (!terminal || !m_terminalsPage->info(target, &info))
        return terminalError(QStringLiteral("not_found"),
                             QStringLiteral("No terminal with that id is open."));
    if (!info.shared && target != terminalId)
        return terminalError(QStringLiteral("not_shared"),
                             QStringLiteral("The user keeps this terminal private. Ask them to "
                                            "turn on Shared with agents on the Terminals page."));
    if (method == bridge::ReadMethod) {
        bridge::ReadRequest request;
        QString code;
        if (!bridge::parseReadRequest(params, &request, &code))
            return terminalError(code);
        QJsonObject reply = terminalJson(target, terminalId);
        reply.insert(QStringLiteral("text"), bridge::tailLines(terminal->plainText(), request.lines));
        return okReply(reply);
    }
    if (method == bridge::InputMethod) {
        bridge::InputRequest request;
        QString code;
        if (!bridge::parseInputRequest(params, terminal->applicationCursorKeys(), &request, &code))
            return terminalError(code);
        if (!info.running)
            return terminalError(QStringLiteral("not_running"),
                                 QStringLiteral("The program in this terminal has exited."));
        if (!m_terminalInputLimiter.allow(QDateTime::currentMSecsSinceEpoch()))
            return terminalError(QStringLiteral("rate_limited"),
                                 QStringLiteral("At most %1 inputs a minute; wait and read the "
                                                "terminal first.")
                                     .arg(bridge::kInputLimit));
        terminal->sendInput(request.bytes);
        m_terminalsPage->noteAgentAction(target, QStringLiteral("%1 %2").arg(who, request.summary));
        return okReply(terminalJson(target, terminalId));
    }
    if (method == bridge::CloseMethod) {
        if (info.openedBy.isEmpty())
            return terminalError(QStringLiteral("user_terminal"),
                                 QStringLiteral("The user opened this terminal, so only they "
                                                "can close it."));
        m_terminalsPage->closeTab(target);
        return okReply({{QStringLiteral("terminal"), target}, {QStringLiteral("closed"), true}});
    }
    return terminalError(QStringLiteral("unknown_method"), QStringLiteral("Unknown method."));
}

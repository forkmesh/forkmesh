// Project settings > Agents at launch time: every agent this process starts
// is held to the project folder (ProjectAgentAccess.h) and asks before any
// external call (AgentNetworkGate.h), unless the project says otherwise.
// When a tool call still fails to reach something (a folder outside the lock,
// a host that will not resolve), the agent waits on a prompt that lets the
// user grant the access (AgentAccessFailure.h).

#include "MainWindow.h"
#include "MessageBoxClicks.h"
#include "MainWindowInternal.h"
#include "AgentNetworkGate.h"
#include "AgentRunRules.h"
#include "AgentToolPolicy.h"
#include "ClaudeTranscriptView.h"
#include "CodexAppServerSession.h"
#include "CommandVariables.h"
#include "DestructiveCommandReview.h"
#include "NetworkLogArchive.h"
#include "ProjectAgentAccess.h"
#include "ProjectRegistry.h"
#include "VirtualMachineRuntime.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include <QAbstractTextDocumentLayout>
#include <QFontDatabase>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStyle>
#include <QUrlQuery>

using namespace forkmesh::ui;

namespace {

forkmesh::projects::AgentAccess savedProjectAccess()
{
    return forkmesh::projects::loadAgentAccess(forkmesh::paths::projectDir());
}

QStringList agentPrivateRoots()
{
    return {forkmesh::paths::dataRoot(),   forkmesh::paths::localDataRoot(),
            forkmesh::paths::configRoot(), forkmesh::paths::settingsDir(),
            forkmesh::paths::cacheRoot(),  forkmesh::paths::stateRoot(),
            forkmesh::paths::logsDir(),    forkmesh::paths::sshDir()};
}

QString currentProjectName()
{
    const QString slug = forkmesh::paths::projectSlug();
    for (const forkmesh::projects::Project &project : forkmesh::projects::list(
             forkmesh::paths::projectsRoot(), forkmesh::paths::homeDir()))
        if (project.slug == slug)
            return project.name;
    return slug;
}

// The session a gate token belongs to: "<sid>:<random>".
int gateSessionId(const QString &token)
{
    return token.section(QLatin1Char(':'), 0, 0).toInt();
}

QString gateTargetLabel(const QString &target)
{
    return target == QLatin1String("web-search") ? QStringLiteral("web search") : target;
}

void openDeleteRuleDialog(QWidget *parent, const QStringList &paths, const QString &scratch,
                          const QString &projectName,
                          const std::function<void(const QString &, bool)> &onSave)
{
    auto *dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("agentDeleteRuleDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setWindowTitle(QStringLiteral("Allow recursive deletes by folder"));
    auto *layout = new QVBoxLayout(dialog);
    layout->setSpacing(10);
    auto *intro = new QLabel(QStringLiteral(
        "Agents run a recursive delete without asking when every path it deletes, with its "
        "variables filled in, matches one of your rules. <code>*</code> matches one name and "
        "<code>**</code> any number of folders. Start with <code>/</code>, <code>~</code> or "
        "<code>$FORKMESH_AGENT_SCRATCH</code> (each agent’s own scratch folder). ForkMesh "
        "data, <code>.git</code>, worktree folders themselves, your home folder and "
        "<code>/</code> are never covered."));
    intro->setTextFormat(Qt::RichText);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    QString deleted = QStringLiteral("<b>This command deletes</b><ul style=\"margin-left:-24px; "
                                     "margin-top:2px\">");
    for (const QString &path : paths)
        deleted += QStringLiteral("<li><code>%1</code></li>").arg(path.toHtmlEscaped());
    auto *deletedLabel = new QLabel(deleted + QStringLiteral("</ul>"));
    deletedLabel->setTextFormat(Qt::RichText);
    deletedLabel->setWordWrap(true);
    deletedLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(deletedLabel);
    auto *pattern = new QComboBox;
    pattern->setObjectName(QStringLiteral("agentDeleteRulePattern"));
    pattern->setEditable(true);
    pattern->setInsertPolicy(QComboBox::NoInsert);
    pattern->addItems(forkmesh::commandvars::suggestedDeletePatterns(paths, scratch));
    layout->addWidget(pattern);
    auto *status = new QLabel;
    status->setObjectName(QStringLiteral("agentDeleteRuleStatus"));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto *project = new QRadioButton(QStringLiteral("In this project (%1)").arg(projectName));
    auto *everywhere = new QRadioButton(QStringLiteral("In every project"));
    project->setChecked(true);
    layout->addWidget(project);
    layout->addWidget(everywhere);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *save =
        buttons->addButton(QStringLiteral("Save rule and allow"), QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("agentDeleteRuleSave"));
    layout->addWidget(buttons);
    const auto update = [pattern, status, save, paths, scratch] {
        const QString text = pattern->currentText().trimmed();
        QString problem = forkmesh::commandvars::deletePatternProblem(text);
        if (problem.isEmpty()) {
            const QString uncovered =
                forkmesh::commandvars::uncoveredDeletePath({text}, paths, scratch);
            if (!uncovered.isEmpty())
                problem = forkmesh::commandvars::protectedDeletePath(uncovered)
                              ? QStringLiteral("%1 is never covered by a rule.").arg(uncovered)
                              : QStringLiteral("Does not cover %1.").arg(uncovered);
        }
        status->setText(problem.isEmpty()
                            ? QStringLiteral("Covers everything this command deletes.")
                            : problem);
        save->setEnabled(problem.isEmpty());
    };
    QObject::connect(pattern, &QComboBox::currentTextChanged, dialog, update);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog,
                     [dialog, pattern, everywhere, onSave] {
                         const QString text = pattern->currentText().trimmed();
                         const bool global = everywhere->isChecked();
                         dialog->accept();
                         onSave(text, global);
                     });
    update();
    dialog->resize(qMin(640, parent->width()), dialog->sizeHint().height());
    dialog->open();
}

} // namespace

forkmesh::projects::GlobalAgentRules MainWindow::globalAgentRules() const
{
    static QString cachedPath;
    static QDateTime cachedModified;
    static qint64 cachedSize = -1;
    static forkmesh::projects::GlobalAgentRules cached;
    const QString path = forkmesh::projects::globalRulesPath(forkmesh::paths::homeDir());
    const QFileInfo info(path);
    const QDateTime modified = info.exists() ? info.lastModified() : QDateTime();
    const qint64 size = info.exists() ? info.size() : -1;
    if (path != cachedPath || modified != cachedModified || size != cachedSize) {
        cached = forkmesh::projects::loadGlobalRules(path);
        cachedPath = path;
        cachedModified = modified;
        cachedSize = size;
    }
    return cached;
}

forkmesh::projects::AgentAccess MainWindow::effectiveProjectAgentAccess() const
{
    return forkmesh::projects::effectiveAgentAccess(savedProjectAccess(), globalAgentRules());
}

void MainWindow::applyProjectAgentAccess(forkmesh::agentguard::Config &guardrails) const
{
    forkmesh::projects::applyAgentAccess(guardrails, effectiveProjectAgentAccess(),
                                         forkmesh::paths::projectDir(), agentPrivateRoots(),
                                         QDir::homePath(), forkmesh::vm::active());
    if (guardrails.projectLocked)
        guardrails.projectName = currentProjectName();
}

forkmesh::agentrules::RunRules MainWindow::agentRunRules(int sid) const
{
    const forkmesh::projects::GlobalAgentRules global = globalAgentRules();
    forkmesh::agentrules::RunRules rules;
    rules.access = forkmesh::projects::effectiveAgentAccess(savedProjectAccess(), global);
    rules.destructive = global.destructive;
    const auto grant = m_teammateRunGrants.constFind(sid);
    if (grant != m_teammateRunGrants.cend() || m_startingTeammate) {
        const QPair<QString, QStringList> &teammate =
            grant != m_teammateRunGrants.cend() ? grant.value() : m_startingTeammateGrant;
        rules.teammateName = teammate.first;
        rules.deniedPermissions = forkmesh::agentrules::deniedTeammatePermissions(teammate.second);
        rules.externalBlocked = rules.deniedPermissions.contains(QStringLiteral("external-tools"));
        rules.allowedToolRules = forkmesh::agentrules::teammateAllowedToolRules(teammate.second);
        if (teammate.second.contains(forkmesh::agentrules::kReadAppLogsGrant))
            rules.allowedToolRules += forkmesh::agentrules::appLogReadRules(
                networkLogPath(), forkmesh::netlog::archiveDir(networkLogPath()));
        rules.access.allowedHosts += forkmesh::agentrules::teammateAllowedHosts(teammate.second);
    }
    return rules;
}

bool MainWindow::agentRunGatesCommands(int sid) const
{
    return !forkmesh::vm::active() && forkmesh::agentrules::gatesCommands(agentRunRules(sid));
}

QSet<QString> MainWindow::agentRunApprovals(int sid) const
{
    QSet<QString> approved;
    const QString prefix = QString::number(sid) + QLatin1Char(':');
    for (auto it = m_agentGateSessionHosts.cbegin(); it != m_agentGateSessionHosts.cend(); ++it)
        if (it.key().startsWith(prefix))
            approved.unite(it.value());
    return approved;
}

QJsonObject MainWindow::agentRunLaunchOptions(int sid, bool codex) const
{
    const forkmesh::projects::GlobalAgentRules global = globalAgentRules();
    const forkmesh::agentrules::RunRules rules = agentRunRules(sid);
    const bool vm = forkmesh::vm::active();
    QStringList disabled = disabledAgentToolFamilies();
    QJsonObject options;
    if (global.toolAllowlist) {
        disabled += AgentToolPolicy::familiesOutsideAllowlist(global.allowedToolFamilies);
        options = AgentToolPolicy::optionsWithAllowedFamilies(options, true,
                                                              global.allowedToolFamilies);
    }
    if (codex && !vm &&
        (!forkmesh::projects::webSearchAllowed(rules.access) || rules.externalBlocked))
        disabled << QStringLiteral("web_search");
    options = AgentToolPolicy::optionsWithDisabledFamilies(options, disabled);
    const QStringList names = forkmesh::agentrules::teammateDisallowedTools(rules.deniedPermissions);
    if (!names.isEmpty())
        options.insert(QStringLiteral("disallowedToolNames"), QJsonArray::fromStringList(names));
    if (!codex)
        return options;
    if (!vm && (forkmesh::agentrules::gatesCommands(rules) ||
                AgentToolPolicy::deniesProcessExecution(
                    AgentToolPolicy::disabledFamiliesFromOptions(options))))
        options.insert(QStringLiteral("externalCallGate"), true);
    if (!vm && rules.access.folders == forkmesh::projects::AgentAccess::Folders::Project &&
        !forkmesh::projects::folderLockSupported()) {
        QStringList roots = forkmesh::projects::projectAgentFolders(
            forkmesh::paths::projectDir(), agentPrivateRoots(), QDir::homePath());
        roots += rules.access.readWriteFolders;
        if (rules.access.sharedTools)
            roots += forkmesh::projects::sharedScratchFolders(QDir::homePath());
        roots.removeDuplicates();
        options.insert(QStringLiteral("workspaceWriteRoots"), QJsonArray::fromStringList(roots));
    }
    return options;
}

QStringList MainWindow::agentRunToolEnvironment(int sid) const
{
    const QJsonObject options = agentRunLaunchOptions(sid, false);
    QStringList env{QStringLiteral("FORKMESH_AGENT_DISABLED_TOOL_FAMILIES"),
                    QStringLiteral("FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES")};
    const QStringList disabled = AgentToolPolicy::disabledFamiliesFromOptions(options);
    if (!disabled.isEmpty())
        env << QStringLiteral("FORKMESH_AGENT_DISABLED_TOOL_FAMILIES=%1")
                   .arg(disabled.join(QLatin1Char(',')));
    bool allowlist = false;
    const QStringList allowed = AgentToolPolicy::allowedFamiliesFromOptions(options, &allowlist);
    if (allowlist)
        env << QStringLiteral("FORKMESH_AGENT_ALLOWED_TOOL_FAMILIES=%1")
                   .arg(allowed.join(QLatin1Char(',')));
    return env;
}

void MainWindow::startAgentRunner(const AgentSession &session, const Issue &issue,
                                  const QString &repoPath, AgentRunner::Config config)
{
    config.extraEnvironment += agentRunToolEnvironment(session.id);
    config.extraEnvironment += agentNetworkGateEnv(session.id);
    acquireAgentRunner()->start(session, issue, repoPath, config);
}

QStringList MainWindow::agentNetworkGateEnv(int sessionId)
{
    // Inside the KVM guest the hook cannot reach this socket and would refuse
    // every command; the guest is its own boundary. Outside it the socket is
    // always up: even a project that lets external calls through hears about
    // the ones that fail, so the user can grant the access.
    if (forkmesh::vm::active())
        return {};
    const bool gateCalls = agentRunGatesCommands(sessionId);
    if (!m_agentNetworkGate) {
        m_agentNetworkGate = std::make_unique<forkmesh::agentnet::Server>();
        QString error;
        if (!m_agentNetworkGate->listen(&error)) {
            m_agentNetworkGate.reset();
            if (!gateCalls)
                return {};
            // The hook fails closed without a socket, so the run still asks
            // for nothing it cannot get; say why its external calls fail.
            noteAgentSessionNotice(
                sessionId,
                QStringLiteral("External calls and destructive commands will be refused: "
                               "the permission gate could not listen (%1).")
                    .arg(error),
                /*error=*/true);
            return {QStringLiteral("%1=/nonexistent").arg(forkmesh::agentnet::SocketEnv)};
        }
        m_agentNetworkGate->setRequestHandler(
            [this](const forkmesh::agentnet::Request &request) {
                handleAgentExternalCall(request);
            });
        m_agentNetworkGate->setFailureHandler(
            [this](const forkmesh::agentnet::Request &request) {
                handleAgentAccessFailure(request);
            });
        m_agentNetworkGate->setCancelHandler([this](const QString &id) {
            if (QPointer<QDialog> box = m_agentGatePrompts.take(id))
                box->close();
        });
    }
    QString label = QStringLiteral("Agent #%1").arg(sessionId);
    if (const AgentSession *session = findAgentSession(sessionId)) {
        if (session->issueNumber > 0 && !session->name.isEmpty())
            label += QStringLiteral(" on %1 #%2").arg(session->name).arg(session->issueNumber);
        else if (!session->name.isEmpty())
            label += QStringLiteral(" on %1").arg(session->name);
    }
    const QString token = QStringLiteral("%1:%2").arg(sessionId).arg(
        QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'));
    return {QStringLiteral("%1=%2").arg(forkmesh::agentnet::SocketEnv,
                                        m_agentNetworkGate->socketPath()),
            QStringLiteral("%1=%2").arg(forkmesh::agentnet::SessionEnv, token),
            QStringLiteral("%1=%2").arg(forkmesh::agentnet::LabelEnv, label)};
}

void MainWindow::handleAgentExternalCall(const forkmesh::agentnet::Request &request)
{
    if (!m_agentNetworkGate)
        return;
    const int sid = gateSessionId(request.session);
    answerAgentGateRequest(request,
                           forkmesh::agentrules::decide(agentRunRules(sid), request.call,
                                                        request.risk,
                                                        m_agentGateSessionHosts.value(
                                                            request.session)));
}

void MainWindow::answerAgentGateRequest(const forkmesh::agentnet::Request &request,
                                        const forkmesh::agentrules::Decision &decision)
{
    using Decision = forkmesh::agentnet::Server::Decision;
    using Verdict = forkmesh::agentrules::Verdict;
    if (!m_agentNetworkGate)
        return;
    const QString target = request.call.target;
    const QString host = forkmesh::projects::normalizeHost(target);
    const int sid = gateSessionId(request.session);
    const QString shown = gateTargetLabel(target);

    if (decision.verdict == Verdict::Run) {
        m_agentNetworkGate->answer(request.id, Decision::Allow);
        return;
    }
    if (decision.verdict == Verdict::Deny) {
        m_agentNetworkGate->answer(request.id, Decision::Deny, decision.reason);
        if (sid > 0)
            noteAgentSessionNotice(
                sid,
                request.call.external && request.risk.summary.isEmpty()
                    ? QStringLiteral("External call to %1 refused.").arg(shown)
                    : QStringLiteral("Command refused: %1").arg(decision.reason),
                /*error=*/false);
        return;
    }
    if (decision.runKey.startsWith(QLatin1String("destructive:"))) {
        promptAgentDestructiveCommand(request, decision);
        return;
    }

    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(QStringLiteral("External call"));
    box->setText(QStringLiteral("%1 wants to reach <b>%2</b>.")
                     .arg(request.label.isEmpty() ? QStringLiteral("An agent")
                                                  : request.label.toHtmlEscaped(),
                          shown.toHtmlEscaped()));
    box->setTextFormat(Qt::RichText);
    box->setInformativeText(QStringLiteral("<code>%1</code><br><br>The agent waits until "
                                           "you answer.")
                                .arg(request.call.summary.toHtmlEscaped()));
    QPushButton *once = box->addButton(QStringLiteral("Allow once"), QMessageBox::AcceptRole);
    QPushButton *run = box->addButton(QStringLiteral("Allow for this run"),
                                      QMessageBox::AcceptRole);
    QPushButton *always = nullptr;
    if (!host.isEmpty())
        always = box->addButton(QStringLiteral("Always allow %1").arg(shown),
                                QMessageBox::AcceptRole);
    QPushButton *deny = box->addButton(QStringLiteral("Deny"), QMessageBox::RejectRole);
    box->setDefaultButton(deny);
    box->setEscapeButton(deny);

    const QString id = request.id;
    const QString session = request.session;
    m_agentGatePrompts.insert(id, box);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, id, session, target, host, shown, sid, once, run,
             always](QAbstractButton *button) {
                m_agentGatePrompts.remove(id);
                if (!m_agentNetworkGate)
                    return;
                if (button == run) {
                    m_agentGateSessionHosts[session].insert(target);
                } else if (always && button == always) {
                    forkmesh::projects::AgentAccess saved = savedProjectAccess();
                    if (!saved.allowedHosts.contains(host))
                        saved.allowedHosts.append(host);
                    QString error;
                    if (!forkmesh::projects::saveAgentAccess(forkmesh::paths::projectDir(),
                                                             saved, &error))
                        flashMessage(error, true);
                }
                const bool allowed = button == once || button == run ||
                                     (always && button == always);
                m_agentNetworkGate->answer(
                    id, allowed ? forkmesh::agentnet::Server::Decision::Allow
                                : forkmesh::agentnet::Server::Decision::Deny,
                    allowed ? QString()
                            : QStringLiteral("The user denied this call to %1. Do not try "
                                             "another way to reach it; continue without it "
                                             "or ask the user.")
                                  .arg(shown));
                if (m_agentTranscript)
                    m_agentTranscript->refreshCommandApprovals();
                if (sid > 0)
                    noteAgentSessionNotice(
                        sid,
                        QStringLiteral("External call to %1 %2.")
                            .arg(shown, allowed ? QStringLiteral("allowed")
                                                : QStringLiteral("denied")),
                        /*error=*/false);
            });
    // Closing the prompt any other way counts as a denial. finished, not
    // destroyed: the window's own teardown deletes open prompts after its
    // members are gone.
    connect(box, &QDialog::finished, this, [this, id] {
        if (m_agentGatePrompts.take(id) && m_agentNetworkGate)
            m_agentNetworkGate->answer(id, forkmesh::agentnet::Server::Decision::Deny,
                                       QStringLiteral("The user dismissed the request."));
    });
    box->show();
    box->raise();
    box->activateWindow();
    QApplication::alert(this);
}

void MainWindow::promptAgentDestructiveCommand(const forkmesh::agentnet::Request &request,
                                               const forkmesh::agentrules::Decision &decision)
{
    using Lean = forkmesh::destructive::Review::Lean;
    const QString rule = decision.runKey.mid(QStringLiteral("destructive:").size());
    const QString command =
        request.risk.command.isEmpty() ? request.risk.summary : request.risk.command;
    const bool matched = request.risk.matchStart >= 0 && request.risk.matchLength > 0 &&
                         request.risk.matchStart + request.risk.matchLength <= command.size();
    const QString scratch = forkmesh::commandvars::trustedScratch(request.environment);
    const forkmesh::destructive::Review review = forkmesh::destructive::review(
        command, rule, request.risk.matchStart, request.risk.matchLength,
        forkmesh::commandvars::valuesAt(command, request.environment,
                                        matched ? request.risk.matchStart : command.size()),
        scratch);

    auto *box = new QDialog(this);
    box->setObjectName(QStringLiteral("agentDestructiveCommandPrompt"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setWindowTitle(QStringLiteral("Destructive command"));
    const QRect screen = this->screen()->availableGeometry();
    const int width = qMin(820, screen.width() - 80);

    auto *outer = new QVBoxLayout(box);
    outer->setSpacing(10);
    outer->setSizeConstraint(QLayout::SetNoConstraint);
    auto *header = new QHBoxLayout;
    header->setSpacing(14);
    auto *icon = new QLabel;
    const int iconSize = box->style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, box);
    icon->setPixmap(box->style()
                        ->standardIcon(QStyle::SP_MessageBoxWarning, nullptr, box)
                        .pixmap(iconSize, iconSize));
    icon->setAlignment(Qt::AlignTop);
    header->addWidget(icon, 0, Qt::AlignTop);
    auto *headerText = new QVBoxLayout;
    auto *title = new QLabel(
        QStringLiteral("%1 wants to run a destructive command (<b>%2</b>).")
            .arg(request.label.isEmpty() ? QStringLiteral("An agent")
                                         : request.label.toHtmlEscaped(),
                 rule.toHtmlEscaped()));
    title->setTextFormat(Qt::RichText);
    title->setWordWrap(true);
    headerText->addWidget(title);
    const QString said = request.input.value(QStringLiteral("description")).toString().trimmed();
    if (!said.isEmpty()) {
        auto *intent = new QLabel(QStringLiteral("The agent says: <i>%1</i>")
                                      .arg(said.toHtmlEscaped()));
        intent->setObjectName(QStringLiteral("agentDestructiveCommandIntent"));
        intent->setTextFormat(Qt::RichText);
        intent->setWordWrap(true);
        headerText->addWidget(intent);
    }
    header->addLayout(headerText, 1);
    outer->addLayout(header);

    auto *view = new QTextBrowser(box);
    view->setObjectName(QStringLiteral("agentDestructiveCommandView"));
    view->setOpenLinks(false);
    view->setLineWrapMode(QTextEdit::WidgetWidth);
    view->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    view->setFont(fixed);
    view->document()->setDefaultFont(fixed);
    const bool dark = view->palette().color(QPalette::Base).lightness() < 128;
    view->setHtml(forkmesh::destructive::commandHtml(
        command, matched ? request.risk.matchStart : -1, request.risk.matchLength, request.cwd,
        QDir::homePath(), dark));
    view->setToolTip(QStringLiteral("Links open the documentation for that program, or the "
                                    "file or folder it names."));
    const int cap = qMax(160, screen.height() * 55 / 100);
    const auto fit = [view, cap] {
        const int height = int(std::ceil(view->document()->size().height())) +
                           2 * view->frameWidth() + 2;
        view->setFixedHeight(qMin(height, cap));
    };
    {
        const QMargins margins = outer->contentsMargins();
        std::unique_ptr<QTextDocument> sized(view->document()->clone());
        sized->setTextWidth(width - margins.left() - margins.right() - 2 * view->frameWidth());
        view->setFixedHeight(qMin(int(std::ceil(sized->size().height())) +
                                      2 * view->frameWidth() + 2,
                                  cap));
    }
    connect(view->document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged, view, fit);
    connect(view, &QTextBrowser::anchorClicked, this,
            [](const QUrl &url) { QDesktopServices::openUrl(url); });
    outer->addWidget(view);

    const auto bullets = [](const QStringList &items, const QString &none) {
        if (items.isEmpty())
            return QStringLiteral("<br><i>%1</i>").arg(none.toHtmlEscaped());
        QString html = QStringLiteral("<ul style=\"margin-left:-24px; margin-top:2px\">");
        for (const QString &item : items)
            html += QStringLiteral("<li>%1</li>").arg(item);
        return html + QStringLiteral("</ul>");
    };

    const QList<forkmesh::commandvars::Variable> variables =
        forkmesh::commandvars::variables(command, request.environment);
    if (!variables.isEmpty()) {
        using Source = forkmesh::commandvars::Variable::Source;
        const auto shown = [](const QString &text) {
            return QStringLiteral("<code>%1</code>").arg(text.toHtmlEscaped());
        };
        QStringList lines;
        for (const forkmesh::commandvars::Variable &variable : variables) {
            const QString name = shown(variable.name);
            switch (variable.source) {
            case Source::Command:
                if (!variable.known)
                    lines << QStringLiteral("%1 is set in the command to %2; part of it is not "
                                            "known until the command runs.")
                                 .arg(name, shown(variable.assigned));
                else if (variable.value == variable.assigned)
                    lines << QStringLiteral("%1 is set in the command to %2.")
                                 .arg(name, shown(variable.value));
                else
                    lines << QStringLiteral("%1 is set in the command to %2, which is %3.")
                                 .arg(name, shown(variable.assigned), shown(variable.value));
                break;
            case Source::Environment:
                lines << QStringLiteral("%1 is %2 in the agent’s environment%3.")
                             .arg(name, shown(variable.value),
                                  variable.name == forkmesh::commandvars::ScratchName &&
                                          !scratch.isEmpty()
                                      ? QStringLiteral(" (its own scratch folder)")
                                      : QString());
                break;
            case Source::Hidden:
                lines << QStringLiteral("%1 is set in the agent’s environment; its name looks "
                                        "secret, so the value is not shown.")
                             .arg(name);
                break;
            case Source::Unset:
                lines << QStringLiteral("%1 is not set, so it expands to nothing.").arg(name);
                break;
            case Source::Unknown:
                lines << QStringLiteral("%1 comes from the agent’s environment; its value is "
                                        "not known.")
                             .arg(name);
                break;
            }
        }
        auto *variablesLabel = new QLabel(
            QStringLiteral("<b>Variables</b>%1").arg(bullets(lines, QString())));
        variablesLabel->setObjectName(QStringLiteral("agentDestructiveCommandVariables"));
        variablesLabel->setTextFormat(Qt::RichText);
        variablesLabel->setWordWrap(true);
        variablesLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        outer->addWidget(variablesLabel);
    }
    QString why = QStringLiteral("<b>Why this is destructive</b><br>%1").arg(review.what);
    if (matched)
        why += QStringLiteral(" The highlighted part is what counts as <b>%1</b>.")
                   .arg(rule.toHtmlEscaped());
    if (!review.undo.isEmpty())
        why += QStringLiteral("<br><b>Undo:</b> %1").arg(review.undo);
    auto *whyLabel = new QLabel(why);
    whyLabel->setObjectName(QStringLiteral("agentDestructiveCommandWhy"));
    whyLabel->setTextFormat(Qt::RichText);
    whyLabel->setWordWrap(true);
    whyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(whyLabel);

    auto *reasons = new QLabel(
        QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr>"
                       "<td width=\"50%\" valign=\"top\" style=\"padding-right:12px\">"
                       "<b style=\"color:%1\">Reasons to allow</b>%2</td>"
                       "<td width=\"50%\" valign=\"top\" style=\"padding-left:12px\">"
                       "<b style=\"color:%3\">Reasons to deny</b>%4</td></tr></table>")
            .arg(dark ? QStringLiteral("#7ee787") : QStringLiteral("#1a7f37"),
                 bullets(review.forAllow, QStringLiteral("None found in the command.")),
                 dark ? QStringLiteral("#ff7b72") : QStringLiteral("#cf222e"),
                 bullets(review.forDeny, QStringLiteral("None found in the command."))));
    reasons->setObjectName(QStringLiteral("agentDestructiveCommandReasons"));
    reasons->setTextFormat(Qt::RichText);
    reasons->setWordWrap(true);
    reasons->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(reasons);
    auto *lean = new QLabel(
        review.lean == Lean::Allow
            ? QStringLiteral("<b>Leaning allow:</b> %1").arg(review.leanReason.toHtmlEscaped())
        : review.lean == Lean::Deny
            ? QStringLiteral("<b>Leaning deny:</b> %1").arg(review.leanReason.toHtmlEscaped())
            : QStringLiteral("<b>No clear lean:</b> read the highlighted part and the reasons "
                             "above."));
    lean->setObjectName(QStringLiteral("agentDestructiveCommandLean"));
    lean->setTextFormat(Qt::RichText);
    lean->setWordWrap(true);
    outer->addWidget(lean);

    auto *note = new QLabel(QStringLiteral(
        "The agent waits until you answer. The <a href=\"global\">global agent rules</a> "
        "decide which commands count as destructive; they and the "
        "<a href=\"project\">project settings</a> list the kinds that are always allowed."));
    note->setObjectName(QStringLiteral("modeHint"));
    note->setTextFormat(Qt::RichText);
    note->setWordWrap(true);
    connect(note, &QLabel::linkActivated, this, [this](const QString &link) {
        if (link == QLatin1String("global"))
            showGlobalAgentRules();
        else
            showProjectSettings(forkmesh::paths::projectSlug());
    });
    outer->addWidget(note);

    auto *buttons = new QHBoxLayout;
    auto *always = new QPushButton(QStringLiteral("Always allow %1").arg(rule));
    always->setObjectName(QStringLiteral("agentDestructiveCommandAlways"));
    auto *alwaysMenu = new QMenu(always);
    QAction *alwaysProject = alwaysMenu->addAction(
        QStringLiteral("In this project (%1)").arg(currentProjectName()));
    QAction *alwaysGlobal = alwaysMenu->addAction(QStringLiteral("In every project"));
    QAction *alwaysFolder = nullptr;
    if (request.risk.onlyRemoves && (rule == QLatin1String("recursive delete") ||
                                     rule == QLatin1String("scratch delete"))) {
        alwaysMenu->addSeparator();
        const bool known = !request.risk.removedPaths.isEmpty();
        alwaysFolder = alwaysMenu->addAction(
            known ? QStringLiteral("In folders matching a rule…")
                  : QStringLiteral("In folders matching a rule (this command’s paths can’t be "
                                   "worked out)"));
        alwaysFolder->setObjectName(QStringLiteral("agentDestructiveCommandFolderRule"));
        alwaysFolder->setEnabled(known);
    }
    always->setMenu(alwaysMenu);
    auto *run = new QPushButton(QStringLiteral("Allow %1 for this run").arg(rule));
    auto *deny = new QPushButton(QStringLiteral("Deny"));
    auto *once = new QPushButton(QStringLiteral("Allow once"));
    run->setAutoDefault(false);
    once->setAutoDefault(false);
    always->setAutoDefault(false);
    deny->setDefault(true);
    buttons->addWidget(always);
    buttons->addStretch(1);
    buttons->addWidget(run);
    buttons->addWidget(deny);
    buttons->addWidget(once);
    outer->addLayout(buttons);

    const QString id = request.id;
    const QString key = decision.runKey;
    m_agentGatePrompts.insert(id, box);
    enum class Answer { Deny, Once, Run, Project, Global, Rule };
    const auto answer = [this, box, request, key, rule](Answer choice) {
        m_agentGatePrompts.remove(request.id);
        box->close();
        if (!m_agentNetworkGate)
            return;
        const int sid = gateSessionId(request.session);
        if (choice == Answer::Deny) {
            m_agentNetworkGate->answer(
                request.id, forkmesh::agentnet::Server::Decision::Deny,
                QStringLiteral("The user denied this destructive command. Do not try "
                               "another way to do it; continue without it or ask "
                               "the user."));
            if (sid > 0)
                noteAgentSessionNotice(sid, QStringLiteral("Destructive command denied."),
                                       /*error=*/false);
            return;
        }
        if (choice == Answer::Run)
            m_agentGateSessionHosts[request.session].insert(key);
        QString error;
        if (choice == Answer::Project) {
            forkmesh::projects::AgentAccess saved = savedProjectAccess();
            if (!saved.allowedDestructive.contains(rule))
                saved.allowedDestructive.append(rule);
            if (!forkmesh::projects::saveAgentAccess(forkmesh::paths::projectDir(), saved,
                                                     &error))
                flashMessage(error, true);
            else
                flashMessage(QStringLiteral("Agents in this project now run %1 commands "
                                            "without asking.")
                                 .arg(rule));
        } else if (choice == Answer::Global) {
            const QString path =
                forkmesh::projects::globalRulesPath(forkmesh::paths::homeDir());
            forkmesh::projects::GlobalAgentRules rules = forkmesh::projects::loadGlobalRules(path);
            if (!rules.defaults.allowedDestructive.contains(rule))
                rules.defaults.allowedDestructive.append(rule);
            if (!forkmesh::projects::saveGlobalRules(path, rules, &error))
                flashMessage(error, true);
            else
                flashMessage(QStringLiteral("Agents in every project now run %1 commands "
                                            "without asking.")
                                 .arg(rule));
        }
        QSet<QString> approved = m_agentGateSessionHosts.value(request.session);
        approved.insert(key);
        answerAgentGateRequest(request, forkmesh::agentrules::decide(agentRunRules(sid),
                                                                     request.call, request.risk,
                                                                     approved));
        if (m_agentTranscript)
            m_agentTranscript->refreshCommandApprovals();
    };
    connect(deny, &QPushButton::clicked, box, [answer] { answer(Answer::Deny); });
    connect(once, &QPushButton::clicked, box, [answer] { answer(Answer::Once); });
    connect(run, &QPushButton::clicked, box, [answer] { answer(Answer::Run); });
    connect(alwaysProject, &QAction::triggered, box, [answer] { answer(Answer::Project); });
    connect(alwaysGlobal, &QAction::triggered, box, [answer] { answer(Answer::Global); });
    if (alwaysFolder)
        connect(alwaysFolder, &QAction::triggered, box, [this, box, answer, request] {
            openDeleteRuleDialog(
                box, request.risk.removedPaths, request.risk.scratch, currentProjectName(),
                [this, answer](const QString &pattern, bool everywhere) {
                    QString error;
                    bool saved = false;
                    if (everywhere) {
                        const QString path =
                            forkmesh::projects::globalRulesPath(forkmesh::paths::homeDir());
                        forkmesh::projects::GlobalAgentRules rules =
                            forkmesh::projects::loadGlobalRules(path);
                        if (!rules.defaults.allowedDeletePaths.contains(pattern))
                            rules.defaults.allowedDeletePaths.append(pattern);
                        saved = forkmesh::projects::saveGlobalRules(path, rules, &error);
                    } else {
                        forkmesh::projects::AgentAccess access = savedProjectAccess();
                        if (!access.allowedDeletePaths.contains(pattern))
                            access.allowedDeletePaths.append(pattern);
                        saved = forkmesh::projects::saveAgentAccess(forkmesh::paths::projectDir(),
                                                                    access, &error);
                    }
                    if (!saved) {
                        flashMessage(error, true);
                        return;
                    }
                    flashMessage(QStringLiteral("Agents %1 now run recursive deletes matching "
                                                "%2 without asking.")
                                     .arg(everywhere ? QStringLiteral("in every project")
                                                     : QStringLiteral("in this project"),
                                          pattern));
                    answer(Answer::Rule);
                });
        });
    connect(box, &QDialog::finished, this, [this, id] {
        if (m_agentGatePrompts.take(id) && m_agentNetworkGate)
            m_agentNetworkGate->answer(id, forkmesh::agentnet::Server::Decision::Deny,
                                       QStringLiteral("The user dismissed the request."));
    });
    const int fitted = qMin(screen.height() - 40, outer->totalHeightForWidth(width));
    box->setMinimumSize(qMin(640, width), qMin(fitted, 420));
    box->resize(width, fitted);
    box->show();
    if (matched && view->document()->size().height() > cap)
        view->scrollToAnchor(QStringLiteral("match"));
    box->raise();
    box->activateWindow();
    deny->setFocus();
    QApplication::alert(this);
}

std::optional<bool> MainWindow::projectDecisionForCommand(int sid, const QString &command)
{
    using Verdict = forkmesh::agentrules::Verdict;
    if (command.trimmed().isEmpty() || !m_codexStreams.contains(sid) ||
        !agentRunGatesCommands(sid))
        return std::nullopt;
    const forkmesh::agentrules::Decision decision = forkmesh::agentrules::decideCommand(
        agentRunRules(sid), command, sessionWorkdir(sid), agentRunApprovals(sid));
    if (decision.verdict == Verdict::Deny) {
        noteAgentSessionNotice(sid, QStringLiteral("Command refused: %1").arg(decision.reason),
                               /*error=*/false);
        return false;
    }
    if (decision.verdict == Verdict::Ask)
        return std::nullopt;
    const AgentSession *session = findAgentSession(sid);
    if (session && agentPermissionModeId(session->mode) == QLatin1String("default"))
        return std::nullopt;
    return true;
}

void MainWindow::reportCodexAccessFailure(int sid, const QJsonObject &item)
{
    if (forkmesh::vm::active())
        return;
    const forkmesh::agentnet::AccessFailure failure = forkmesh::agentnet::detectAccessFailure(
        item.value(QStringLiteral("tool")).toString(),
        item.value(QStringLiteral("input")).toObject(),
        item.value(QStringLiteral("output")).toString());
    if (!failure.found())
        return;
    forkmesh::agentnet::Request request;
    request.id = QStringLiteral("codex:%1:%2").arg(sid).arg(++m_codexAccessFailureSeq);
    request.session = QStringLiteral("%1:codex").arg(sid);
    request.tool = item.value(QStringLiteral("tool")).toString();
    request.failure = failure;
    request.label = QStringLiteral("Agent #%1").arg(sid);
    if (const AgentSession *session = findAgentSession(sid); session && !session->name.isEmpty())
        request.label += QStringLiteral(" on %1").arg(session->name);
    handleAgentAccessFailure(request);
}

void MainWindow::answerAgentAccessFailure(const QString &id, const QString &context)
{
    if (!id.startsWith(QLatin1String("codex:"))) {
        if (m_agentNetworkGate)
            m_agentNetworkGate->answerFailure(id, context);
        return;
    }
    const int sid = id.section(QLatin1Char(':'), 1, 1).toInt();
    AgentRuntimeSession *live = m_codexStreams.value(sid);
    if (live && live->running() && !context.isEmpty())
        live->sendUserText(context);
}

namespace {

QString declinedAccessContext(const QString &target)
{
    return QStringLiteral("ForkMesh asked the user for access to %1 and they did not grant it. "
                          "Do not work around it (another resolver, a mirror, a copied file, a "
                          "different path); continue without it, or stop and ask the user.")
        .arg(target);
}

// The folder a grant for `path` should name: the path itself when it is a
// folder, else the nearest folder above it that exists.
QString grantFolderFor(const QString &path)
{
    QFileInfo info(QDir::cleanPath(path));
    if (info.isDir())
        return info.absoluteFilePath();
    QDir dir = info.absoluteDir();
    while (!dir.exists() && !dir.isRoot())
        if (!dir.cdUp())
            break;
    return dir.absolutePath();
}

// Whether this process, which runs without the folder lock, could have done
// what the agent was refused. When it could not either, the refusal is the
// file's own permissions and no grant would help.
bool reachableWithoutLock(const QString &path, bool write)
{
    const QFileInfo info(QDir::cleanPath(path));
    if (info.exists())
        return write ? info.isWritable() : info.isReadable();
    const QFileInfo folder(grantFolderFor(path));
    return write ? folder.isWritable() : folder.isReadable() && folder.isExecutable();
}

bool withinFolder(const QString &path, const QString &folder)
{
    return path == folder || path.startsWith(folder + QLatin1Char('/'));
}

} // namespace

void MainWindow::handleAgentAccessFailure(const forkmesh::agentnet::Request &request)
{
    using Kind = forkmesh::agentnet::AccessFailure::Kind;
    if (!m_agentNetworkGate && !request.id.startsWith(QLatin1String("codex:")))
        return;
    const forkmesh::agentnet::AccessFailure failure = request.failure;
    const QString id = request.id;
    const int sid = gateSessionId(request.session);
    const QString key =
        forkmesh::agentnet::accessFailureKindKey(failure.kind) + QLatin1Char(':') + failure.target;
    if (m_agentAccessDeclined.value(request.session).contains(key)) {
        answerAgentAccessFailure(id, declinedAccessContext(failure.target));
        return;
    }
    const QString who = request.label.isEmpty() ? QStringLiteral("An agent")
                                                : request.label.toHtmlEscaped();
    const QString evidence =
        QStringLiteral("<code>%1</code>").arg(failure.evidence.toHtmlEscaped());

    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->setIcon(QMessageBox::Warning);
    box->setTextFormat(Qt::RichText);
    QPushButton *deny = nullptr;
    // What the agent hears for each answer; set by the button handlers below.
    std::function<QString(QAbstractButton *)> contextFor;
    // Set when an answer is still in progress after the prompt closes.
    std::function<bool(QAbstractButton *)> answersLater;

    if (failure.kind == Kind::Folder) {
        forkmesh::projects::AgentAccess access = effectiveProjectAgentAccess();
        const bool locked = access.folders == forkmesh::projects::AgentAccess::Folders::Project &&
                            forkmesh::projects::folderLockSupported();
        const QString folder = grantFolderFor(failure.target);
        // Without the lock, or somewhere this process cannot reach either,
        // the refusal is the file's own permissions: nothing to grant.
        if (!locked || !reachableWithoutLock(failure.target, failure.write) ||
            folder == QLatin1String("/") || folder == QDir::homePath()) {
            box->deleteLater();
            answerAgentAccessFailure(id, QString());
            return;
        }
        const bool readWriteGranted = std::any_of(
            access.readWriteFolders.cbegin(), access.readWriteFolders.cend(),
            [&](const QString &granted) { return withinFolder(folder, granted); });
        const bool readGranted =
            readWriteGranted ||
            std::any_of(access.readOnlyFolders.cbegin(), access.readOnlyFolders.cend(),
                        [&](const QString &granted) { return withinFolder(folder, granted); });
        if (failure.write ? readWriteGranted : readGranted) {
            // Granted after this run started: the lock is fixed at launch.
            box->deleteLater();
            answerAgentAccessFailure(
                id, QStringLiteral("ForkMesh: the project already grants %1, but the folder "
                                   "lock is set when an agent starts, so this run cannot use "
                                   "it. Finish what you can without it, then tell the user "
                                   "this run needs a restart. Do not work around the lock.")
                        .arg(folder));
            return;
        }
        box->setWindowTitle(QStringLiteral("Agent folder access"));
        box->setText(QStringLiteral("%1 was refused <b>%2</b>: it is outside the "
                                    "folders this project lets agents %3.")
                         .arg(who, failure.target.toHtmlEscaped(),
                              failure.write ? QStringLiteral("change")
                                            : QStringLiteral("read")));
        box->setInformativeText(
            QStringLiteral("%1<br><br>Grant <b>%2</b> to this project's agents? The lock is "
                           "set when an agent starts, so the grant takes effect on its next "
                           "run. The agent waits until you answer.")
                .arg(evidence, folder.toHtmlEscaped()));
        QPushButton *read = box->addButton(QStringLiteral("Allow reading"),
                                           QMessageBox::AcceptRole);
        QPushButton *write = box->addButton(QStringLiteral("Allow reading and writing"),
                                            QMessageBox::AcceptRole);
        deny = box->addButton(QStringLiteral("Deny"), QMessageBox::RejectRole);
        box->setDefaultButton(failure.write ? write : read);
        contextFor = [this, folder, read, write](QAbstractButton *button) -> QString {
            if (button != read && button != write)
                return QString();
            forkmesh::projects::AgentAccess saved = savedProjectAccess();
            QStringList &list = button == write ? saved.readWriteFolders : saved.readOnlyFolders;
            if (!list.contains(folder))
                list.append(folder);
            if (button == write)
                saved.readOnlyFolders.removeAll(folder);
            QString error;
            if (!forkmesh::projects::saveAgentAccess(forkmesh::paths::projectDir(), saved,
                                                     &error)) {
                flashMessage(error, true);
                return QStringLiteral("ForkMesh could not save the user's grant for %1 (%2). "
                                      "Continue without it and tell the user.")
                    .arg(folder, error);
            }
            return QStringLiteral("ForkMesh: the user granted %1 access to %2 for this "
                                  "project. The folder lock is set when an agent starts, so "
                                  "this run still cannot reach it: finish what you can without "
                                  "it, then tell the user this run needs a restart to use it. "
                                  "Do not work around the lock.")
                .arg(button == write ? QStringLiteral("read-write") : QStringLiteral("read"),
                     folder);
        };
    } else {
        const QString host = failure.target;
        const bool resolve = failure.kind == Kind::Resolve;
        box->setWindowTitle(QStringLiteral("Agent network access"));
        box->setText(resolve ? QStringLiteral("%1 could not resolve <b>%2</b>.")
                                   .arg(who, host.toHtmlEscaped())
                             : QStringLiteral("%1 could not connect to <b>%2</b>.")
                                   .arg(who, host.toHtmlEscaped()));
        const QString waits = QStringLiteral("<br><br>The agent waits until you answer; "
                                             "Retry tells it to run the call again.");
        box->setInformativeText(
            evidence +
            (resolve ? QStringLiteral("<br><br>Checking the name from ForkMesh…")
                     : QStringLiteral("<br><br>A firewall (OpenSnitch, ufw) or the network "
                                      "may be blocking it. Allow it there, then Retry.")) +
            waits);
        QPushButton *retry = box->addButton(QStringLiteral("Retry"), QMessageBox::AcceptRole);
        QPushButton *pin = nullptr;
        deny = box->addButton(QStringLiteral("Deny"), QMessageBox::RejectRole);
        box->setDefaultButton(retry);
        auto pinnedAddress = std::make_shared<QString>();
        if (resolve) {
            pin = box->addButton(QStringLiteral("Add to /etc/hosts"), QMessageBox::AcceptRole);
            pin->hide();
            checkAgentHostResolution(host, box, pin, evidence, waits, pinnedAddress);
        }
        contextFor = [host, retry](QAbstractButton *button) -> QString {
            if (button != retry)
                return QString();
            return QStringLiteral("ForkMesh: the user was asked about reaching %1 and chose "
                                  "Retry. Run the same call again; if it fails the same way, "
                                  "stop and tell the user rather than trying another route.")
                .arg(host);
        };
        answersLater = [this, id, sid, host, pin, pinnedAddress](QAbstractButton *button) {
            if (!pin || button != pin || pinnedAddress->isEmpty())
                return false;
            pinAgentHost(id, sid, host, *pinnedAddress);
            return true;
        };
    }

    const QString session = request.session;
    const QString target = failure.target;
    m_agentGatePrompts.insert(id, box);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, id, session, key, target, sid, deny, contextFor,
             answersLater](QAbstractButton *button) {
                m_agentGatePrompts.remove(id);
                if (!m_agentNetworkGate && !id.startsWith(QLatin1String("codex:")))
                    return;
                if (answersLater && answersLater(button))
                    return;
                QString context = button == deny ? QString() : contextFor(button);
                const bool granted = !context.isEmpty();
                if (!granted) {
                    m_agentAccessDeclined[session].insert(key);
                    context = declinedAccessContext(target);
                }
                answerAgentAccessFailure(id, context);
                if (sid > 0)
                    noteAgentSessionNotice(
                        sid,
                        QStringLiteral("Access to %1 %2.")
                            .arg(target, granted ? QStringLiteral("granted")
                                                 : QStringLiteral("not granted")),
                        /*error=*/false);
            });
    connect(box, &QDialog::finished, this, [this, id, target] {
        if (m_agentGatePrompts.take(id))
            answerAgentAccessFailure(id, declinedAccessContext(target));
    });
    box->show();
    box->raise();
    box->activateWindow();
    QApplication::alert(this);
}

void MainWindow::checkAgentHostResolution(const QString &host, QPointer<QMessageBox> box,
                                          QPointer<QPushButton> pin, const QString &evidence,
                                          const QString &waits,
                                          std::shared_ptr<QString> pinnedAddress)
{
    // This process's resolver first: when it finds the name, the agent's
    // lookup was blocked on its way out (a per-program firewall such as
    // OpenSnitch), not missing. When it does not, ask Cloudflare's DNS over
    // HTTPS whether the name exists at all, which is what a pin needs.
    auto show = [box, evidence, waits](const QString &finding) {
        if (box)
            box->setInformativeText(evidence + QStringLiteral("<br><br>") + finding + waits);
    };
    QHostInfo::lookupHost(host, this, [this, host, pin, show,
                                       pinnedAddress](const QHostInfo &info) {
        if (info.error() == QHostInfo::NoError && !info.addresses().isEmpty()) {
            show(QStringLiteral("ForkMesh resolves it (%1), so something on this machine "
                                "blocked the agent's lookup: a per-program firewall such as "
                                "OpenSnitch is the usual cause. Allow the agent's program "
                                "there, then Retry.")
                     .arg(info.addresses().first().toString().toHtmlEscaped()));
            return;
        }
        QUrl url(QStringLiteral("https://cloudflare-dns.com/dns-query"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("name"), host);
        query.addQueryItem(QStringLiteral("type"), QStringLiteral("A"));
        url.setQuery(query);
        QNetworkRequest request(url);
        request.setRawHeader("Accept", "application/dns-json");
        request.setTransferTimeout(10000);
        QNetworkReply *reply = m_networkAccess->get(request);
        connect(reply, &QNetworkReply::finished, this, [reply, host, pin, show, pinnedAddress] {
            reply->deleteLater();
            QString address;
            const QJsonArray answers = QJsonDocument::fromJson(reply->readAll())
                                           .object()
                                           .value(QStringLiteral("Answer"))
                                           .toArray();
            for (const QJsonValue &answer : answers) {
                const QJsonObject record = answer.toObject();
                if (record.value(QStringLiteral("type")).toInt() == 1 &&
                    !QHostAddress(record.value(QStringLiteral("data")).toString()).isNull()) {
                    address = record.value(QStringLiteral("data")).toString();
                    break;
                }
            }
            if (reply->error() != QNetworkReply::NoError) {
                show(QStringLiteral("This machine's DNS resolver cannot find it either, and "
                                    "Cloudflare DNS could not be asked (%1).")
                         .arg(reply->errorString().toHtmlEscaped()));
            } else if (address.isEmpty()) {
                show(QStringLiteral("Neither this machine's DNS resolver nor Cloudflare DNS "
                                    "knows the name; check it is spelled right."));
            } else {
                *pinnedAddress = address;
                show(QStringLiteral("This machine's DNS resolver cannot find it, but "
                                    "Cloudflare DNS resolves it to <b>%1</b>. Add it to "
                                    "/etc/hosts (asks for your administrator password) and "
                                    "the agent retries.")
                         .arg(address.toHtmlEscaped()));
                if (pin)
                    pin->show();
            }
        });
    });
}

void MainWindow::pinAgentHost(const QString &id, int sid, const QString &host,
                              const QString &address)
{
    // Both come from a pattern match and a parsed address; the shell sees
    // them only as positional parameters all the same.
    static const QRegularExpression hostName(QStringLiteral("^[a-z0-9]([a-z0-9.-]*[a-z0-9])?$"));
    auto answer = [this, id, sid, host](const QString &context, bool pinned) {
        answerAgentAccessFailure(id, context);
        if (sid > 0)
            noteAgentSessionNotice(sid,
                                   pinned ? QStringLiteral("%1 added to /etc/hosts.").arg(host)
                                          : QStringLiteral("Could not add %1 to /etc/hosts.")
                                                .arg(host),
                                   /*error=*/!pinned);
    };
    if (!hostName.match(host).hasMatch() || QHostAddress(address).isNull()) {
        answer(declinedAccessContext(host), false);
        return;
    }
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this,
            [this, process, host, address, answer](int exitCode, QProcess::ExitStatus status) {
                process->deleteLater();
                if (status == QProcess::NormalExit && exitCode == 0) {
                    answer(QStringLiteral("ForkMesh: the user added %1 (%2) to /etc/hosts. "
                                          "Run the same call again.")
                               .arg(host, address),
                           true);
                    return;
                }
                const QString errors =
                    QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
                flashMessage(QStringLiteral("Could not add %1 to /etc/hosts%2")
                                 .arg(host, errors.isEmpty() ? QString()
                                                             : QStringLiteral(": ") +
                                                                   errors.right(200)),
                             true);
                answer(declinedAccessContext(host), false);
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, host, answer](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                flashMessage(QStringLiteral("Could not launch the privilege helper (pkexec) to "
                                            "add %1 to /etc/hosts.")
                                 .arg(host),
                             true);
                answer(declinedAccessContext(host), false);
            });
    process->start(QStringLiteral("pkexec"),
                   {QStringLiteral("sh"), QStringLiteral("-c"),
                    QStringLiteral("printf '%s\\t%s  # added by ForkMesh for an agent\\n' "
                                   "\"$1\" \"$2\" >> /etc/hosts"),
                    QStringLiteral("sh"), address, host});
}

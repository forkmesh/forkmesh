
#include "Tooltips.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowNetworkInternal.h"
#include "SystemPressure.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "AgentWorktree.h"
#include "RepoStatsStore.h"
#include "ActionStore.h"
#include "ControlNode.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "D1QuerySource.h"
#include "DatabaseManagerWidget.h"
#include "KebabHeaderView.h"
#include "LogTimelineChart.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PromptDraftStore.h"
#include "PromptSpellCheckHighlighter.h"
#include "PublicMirrorRuntime.h"
#include "QtRuntimeDiagnostics.h"
#include "RewardPoolSigner.h"
#include "TerminalWidget.h"
#include "WorldSpeechBridge.h"
#include "FontSizing.h"

#include <QBrush>
#include <QCheckBox>
#include <QScrollArea>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUrlQuery>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <signal.h>
#endif

using namespace forkmesh::ui;

void MainWindow::refreshRelayNodes()
{
    if (!m_nodesTable)
        return;
    QHash<QString, forkmesh::nodedir::RelayProbe> probes;
    for (const ServerConfig &server : std::as_const(m_servers)) {
        forkmesh::nodedir::RelayProbe probe;
        probe.state = QString::fromUtf8("Checking\xE2\x80\xA6");
        probes.insert(serverHost(server.url), probe);
    }
    m_relayProbes = probes;
    m_relayProbesUpdatedAt.clear();
    m_relayProbesInFlight = m_servers.size();
    updateNetworkCounts(m_servers.size(), -1, -1);
    if (m_nodesTable && m_nodesTable->isVisible())
        refreshNodesTable();
    for (int i = 0; i < m_servers.size(); ++i)
        probeRelayRow(i);
}

void MainWindow::probeRelayRow(int row)
{
    if (row < 0 || row >= m_servers.size())
        return;
    const QString host = serverHost(m_servers.at(row).url);
    auto finish = [this, host](const forkmesh::nodedir::RelayProbe &probe) {
        if (m_relayProbes.contains(host))
            m_relayProbes.insert(host, probe);
        if (--m_relayProbesInFlight <= 0) {
            m_relayProbesInFlight = 0;
            m_relayProbesUpdatedAt =
                QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
        }
        updateRelayNodeRows();
    };

    QUrl url(m_servers.at(row).url);
    if (url.scheme() == "ws")
        url.setScheme(QStringLiteral("http"));
    else if (url.scheme() == "wss")
        url.setScheme(QStringLiteral("https"));
    url.setPath(QStringLiteral("/api/version"));
    url.setQuery(QString());
    url.setFragment(QString());

    if (!m_networkAccess || !url.isValid() || url.host().isEmpty()) {
        forkmesh::nodedir::RelayProbe probe;
        probe.state = QStringLiteral("Offline");
        probe.updatedMs = QDateTime::currentMSecsSinceEpoch();
        finish(probe);
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("accept", "application/json");
    request.setTransferTimeout(10000);

    auto *clock = new QElapsedTimer;
    clock->start();
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, clock, host, finish] {
        const qint64 elapsed = clock->elapsed();
        delete clock;
        reply->deleteLater();
        setRelayLinkSpeed(host, reply->error() == QNetworkReply::NoError
                                    ? static_cast<int>(elapsed)
                                    : -1);
        forkmesh::nodedir::RelayProbe probe;
        probe.updatedMs = QDateTime::currentMSecsSinceEpoch();
        if (reply->error() == QNetworkReply::NoError) {
            const QJsonObject obj =
                QJsonDocument::fromJson(reply->readAll()).object();
            noteAppRelease(obj);
            probe.state = QStringLiteral("Online");
            probe.latencyMs = static_cast<int>(elapsed);
            probe.version = forkmesh::nodedir::relayVersionText(
                obj.value(QStringLiteral("version")).toString(),
                obj.value(QStringLiteral("rev")).toString());
        } else {
            probe.state = QStringLiteral("Offline");
            probe.failure = forkmesh::networkReplyFailureSummary(reply);
        }
        finish(probe);
    });
}

namespace {
int requestFirewallPort(const QUrl &url)
{
    if (url.port() > 0)
        return url.port();
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("https") || scheme == QLatin1String("wss"))
        return 443;
    if (scheme == QLatin1String("http") || scheme == QLatin1String("ws"))
        return 80;
    return -1;
}

QString requestFirewallDestination(const QUrl &url)
{
    const QString host = url.host().toLower();
    const int port = requestFirewallPort(url);
    return port > 0 ? QStringLiteral("%1:%2").arg(host).arg(port) : host;
}

QString requestFirewallUser()
{
#ifndef Q_OS_WIN
    return QString::number(getuid());
#else
    const QString user = qEnvironmentVariable("USERNAME");
    return user.isEmpty() ? QStringLiteral("current user") : user;
#endif
}

BackoffNetworkAccessManager *requestFirewallManager(QNetworkAccessManager *manager)
{
    return qobject_cast<BackoffNetworkAccessManager *>(manager);
}

constexpr int kFirewallRuleDeniedRole = Qt::UserRole + 1;
constexpr int kFirewallDenyDialogResult = 2;
// On a decision row's first cell: the request it was about.
constexpr int kFirewallHistoryMethodRole = Qt::UserRole + 2;
constexpr int kFirewallHistoryUrlRole = Qt::UserRole + 3;

enum FirewallRuleColumn {
    kFirewallRuleColDecision = 0,
    kFirewallRuleColRule,
    kFirewallRuleColStored,
    kFirewallRuleColRequests,
    kFirewallRuleColBlocked,
    kFirewallRuleColLog, // the per-row "open the log filtered to this rule"
    kFirewallRuleColCount
};

// The text that finds a rule's own traffic in the log. The log's text filter
// reads * and ? as wildcards, so a rule written back out the way it is typed
// -- "*.vultr.com", "app.forkmesh.com/api/chat/*" -- is already the pattern.
QString firewallRuleLogFilter(const QString &rule)
{
    const QString canonical =
        BackoffNetworkAccessManager::canonicalFirewallRule(rule);
    // "all destinations" and "every https request" cover the whole log, so a
    // filter built from them would single out nothing.
    if (canonical.isEmpty() || canonical == QLatin1String("*") ||
        canonical.startsWith(QLatin1String("scheme:")))
        return QString();
    return BackoffNetworkAccessManager::firewallRuleEditText(canonical);
}

// A rule naming this machine never decides anything: loopback is exempt from
// the firewall, so its counts stay at zero however busy the port is.
bool firewallRuleIsOnThisMachine(const QString &rule)
{
    const QString canonical =
        BackoffNetworkAccessManager::canonicalFirewallRule(rule);
    QString host;
    if (canonical.startsWith(QLatin1String("hostport:")))
        host = canonical.mid(9);
    else if (canonical.startsWith(QLatin1String("host:")))
        host = canonical.mid(5);
    if (host.isEmpty())
        return false;
    return BackoffNetworkAccessManager::isOnThisMachine(
        QUrl(QStringLiteral("https://") + host));
}
const QString kFirewallRuleSyntaxHint = QStringLiteral(
    "host, host:port, *.domain, host/path/*, https://host/exact/url, scheme:https, *");

// One endpoint catalog source, "desktop/src/File.cpp: Class::function", with
// the line the function is defined on and the file itself when this machine
// has the source tree the build came from.
struct EndpointSourceLocation {
    QString text;      // "desktop/src/File.cpp:123: Class::function"
    QString localPath; // empty without a checkout
};

EndpointSourceLocation endpointSourceLocation(const QString &source)
{
    const qsizetype separator = source.indexOf(QLatin1String(": "));
    const QString path = (separator >= 0 ? source.left(separator) : source).trimmed();
    const QString function =
        separator >= 0 ? source.mid(separator + 2).trimmed() : QString();
    EndpointSourceLocation location;
    location.text = source;
    // FORKMESH_SOURCE_DIR is desktop/; catalog paths start at the repo root.
    const QString local = QDir::cleanPath(
        QStringLiteral(FORKMESH_SOURCE_DIR "/../") + path);
    QFile file(local);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        return location;
    location.localPath = local;
    if (function.isEmpty())
        return location;
    // A definition starts in the first column; a call is indented. Fall back
    // to the first call, then to the first mention of a name that is not a
    // function (a URL constant or member).
    const QByteArray name = function.toUtf8();
    const QByteArray needle = name + '(';
    int lineNumber = 0;
    int firstMention = 0;
    int firstCall = 0;
    int definition = 0;
    while (!file.atEnd() && definition == 0) {
        const QByteArray line = file.readLine();
        ++lineNumber;
        if (!line.contains(name))
            continue;
        if (firstMention == 0)
            firstMention = lineNumber;
        if (!line.contains(needle))
            continue;
        if (firstCall == 0)
            firstCall = lineNumber;
        if (!line.isEmpty() && line.at(0) != ' ' && line.at(0) != '\t')
            definition = lineNumber;
    }
    const int found = definition > 0 ? definition
                      : firstCall > 0 ? firstCall
                                      : firstMention;
    if (found > 0)
        location.text = QStringLiteral("%1:%2: %3").arg(path).arg(found).arg(function);
    return location;
}

// The sources as lines of rich text, each linked to its file when it is here.
QString endpointSourcesHtml(const QStringList &sources)
{
    QStringList lines;
    for (const QString &source : sources) {
        const EndpointSourceLocation location = endpointSourceLocation(source);
        lines.append(location.localPath.isEmpty()
                         ? location.text.toHtmlEscaped()
                         : QStringLiteral("<a href=\"%1\">%2</a>")
                               .arg(QUrl::fromLocalFile(location.localPath)
                                        .toString()
                                        .toHtmlEscaped(),
                                    location.text.toHtmlEscaped()));
    }
    return lines.join(QStringLiteral("<br>"));
}

void persistFirewallRules(const BackoffNetworkAccessManager *manager)
{
    QSettings settings;
    settings.setValue(kRequestFirewallWhitelistSetting, manager->firewallRules());
    settings.setValue(kRequestFirewallDenylistSetting, manager->firewallDenyRules());
}

// Puts every shipped default back on the allow list, including ones the
// person removed or denied: they asked for it by pressing the button.
int restoreDefaultApprovedFirewallRules(BackoffNetworkAccessManager *manager)
{
    int restored = 0;
    for (const BackoffNetworkAccessManager::FirewallRuleEntry &entry :
         defaultApprovedFirewallRules()) {
        if (manager->addFirewallRule(entry.rule))
            ++restored;
    }
    persistFirewallRules(manager);
    return restored;
}

// One open prompt per host. The prompt runs inside the request that reached
// the unlisted host, and the app often fires several requests at one host
// together (startup's relay reads); each used to stack its own dialog for the
// same question. Later requests now wait for the open prompt's answer.
struct PendingFirewallPrompt {
    QPointer<QDialog> dialog;
    bool decided = false;
    BackoffNetworkAccessManager::FirewallVerdict verdict =
        BackoffNetworkAccessManager::FirewallDenyOnce;
    QString rule;
};

QHash<QString, std::shared_ptr<PendingFirewallPrompt>> &pendingFirewallPrompts()
{
    static QHash<QString, std::shared_ptr<PendingFirewallPrompt>> prompts;
    return prompts;
}
} // namespace

QWidget *MainWindow::buildFirewallSection()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 20, 24, 24);
    outer->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("Firewall"));
    title->setObjectName("sectionTitle");
    QFont titleFont = title->font();
    titleFont = forkmesh::resizedFont(titleFont, 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    outer->addWidget(title);

    auto *summary = new QLabel(QStringLiteral(
        "Every external destination needs a decision. Allowed and denied "
        "destinations are remembered; anything unlisted asks before ForkMesh "
        "connects. Only this machine (loopback) is exempt."));
    summary->setObjectName("mutedLabel");
    summary->setWordWrap(true);
    outer->addWidget(summary);

    auto *controls = new QHBoxLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    m_requestFirewallEnabledCheck = new QCheckBox(QStringLiteral("Whitelist only"));
    m_requestFirewallEnabledCheck->setCursor(Qt::PointingHandCursor);
    const bool enabled =
        QSettings().value(kRequestFirewallEnabledSetting, true).toBool();
    m_requestFirewallEnabledCheck->setChecked(enabled);
    connect(m_requestFirewallEnabledCheck, &QCheckBox::toggled, this,
            [this](bool on) {
                QSettings().setValue(kRequestFirewallEnabledSetting, on);
                if (auto *manager = requestFirewallManager(m_networkAccess))
                    manager->setFirewallEnabled(on);
                logSystem(on ? QStringLiteral("Firewall: whitelist-only mode enabled.")
                             : QStringLiteral("Firewall: whitelist-only mode disabled."));
                refreshFirewallTables();
            });
    controls->addWidget(m_requestFirewallEnabledCheck);
    m_requestFirewallStatus = new QLabel;
    m_requestFirewallStatus->setObjectName("mutedLabel");
    controls->addWidget(m_requestFirewallStatus, 1);
    auto *clearHistory = new QPushButton(QStringLiteral("Clear history"));
    clearHistory->setObjectName("repoAction");
    clearHistory->setCursor(Qt::PointingHandCursor);
    setOcticon(clearHistory, "trash", 16);
    connect(clearHistory, &QPushButton::clicked, this,
            &MainWindow::clearFirewallHistory);
    controls->addWidget(clearHistory);
    outer->addLayout(controls);

    auto *ruleRow = new QHBoxLayout;
    ruleRow->setContentsMargins(0, 0, 0, 0);
    m_requestFirewallRuleEdit = new QLineEdit;
    m_requestFirewallRuleEdit->setPlaceholderText(kFirewallRuleSyntaxHint);
    ruleRow->addWidget(m_requestFirewallRuleEdit, 1);
    auto *addRule = new QPushButton(QStringLiteral("Allow"));
    addRule->setObjectName("repoAction");
    addRule->setCursor(Qt::PointingHandCursor);
    setOcticon(addRule, "check-circle", 16);
    connect(addRule, &QPushButton::clicked, this,
            [this] { addFirewallRuleFromEdit(false); });
    connect(m_requestFirewallRuleEdit, &QLineEdit::returnPressed, this,
            [this] { addFirewallRuleFromEdit(false); });
    ruleRow->addWidget(addRule);
    auto *denyRule = new QPushButton(QStringLiteral("Deny"));
    denyRule->setObjectName("repoAction");
    denyRule->setCursor(Qt::PointingHandCursor);
    setOcticon(denyRule, "x", 16);
    connect(denyRule, &QPushButton::clicked, this,
            [this] { addFirewallRuleFromEdit(true); });
    ruleRow->addWidget(denyRule);
    m_requestFirewallRemoveButton = new QPushButton(QStringLiteral("Remove"));
    m_requestFirewallRemoveButton->setObjectName("repoAction");
    m_requestFirewallRemoveButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_requestFirewallRemoveButton, "trash", 16);
    connect(m_requestFirewallRemoveButton, &QPushButton::clicked, this,
            &MainWindow::removeSelectedFirewallRules);
    ruleRow->addWidget(m_requestFirewallRemoveButton);
    auto *restoreDefaults = new QPushButton(QStringLiteral("Restore defaults"));
    restoreDefaults->setObjectName("repoAction");
    restoreDefaults->setCursor(Qt::PointingHandCursor);
    restoreDefaults->setToolTip(QStringLiteral(
        "Allow again the URLs ForkMesh ships as approved: sign-in, sync, "
        "updates and the relay sockets"));
    setOcticon(restoreDefaults, "history", 16);
    connect(restoreDefaults, &QPushButton::clicked, this, [this] {
        auto *manager = requestFirewallManager(m_networkAccess);
        if (!manager)
            return;
        const int restored = restoreDefaultApprovedFirewallRules(manager);
        refreshFirewallTables();
        flashMessage(restored > 0
                         ? QStringLiteral("Restored %1 default firewall rule(s).").arg(restored)
                         : QStringLiteral("Every default firewall rule is already allowed."));
    });
    ruleRow->addWidget(restoreDefaults);
    outer->addLayout(ruleRow);

    auto *rulesLabel = new QLabel(QStringLiteral("Decisions"));
    rulesLabel->setObjectName("sectionLabel");
    outer->addWidget(rulesLabel);

    m_requestFirewallRulesTable = new QTableWidget(0, kFirewallRuleColCount);
    installColumnHeaderMenu(m_requestFirewallRulesTable, true);
    m_requestFirewallRulesTable->setObjectName("issueTable");
    m_requestFirewallRulesTable->setHorizontalHeaderLabels(
        {QStringLiteral("Decision"), QStringLiteral("Rule"),
         QStringLiteral("Stored as"), QStringLiteral("Requests"),
         QStringLiteral("Blocked"), QStringLiteral("Log")});
    connect(m_requestFirewallRulesTable, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column != kFirewallRuleColLog)
                    return;
                if (QTableWidgetItem *item =
                        m_requestFirewallRulesTable->item(row, column))
                    openLogForFirewallRule(item->data(Qt::UserRole).toString());
            });
    m_requestFirewallRulesTable->verticalHeader()->setVisible(false);
    m_requestFirewallRulesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_requestFirewallRulesTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_requestFirewallRulesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_requestFirewallRulesTable->setShowGrid(false);
    // Clicking a header sorts by it. Until then Decision orders the rows:
    // denied rules first, each group in its stored order.
    m_requestFirewallRulesTable->setSortingEnabled(true);
    m_requestFirewallRulesTable->horizontalHeader()->setSortIndicator(
        kFirewallRuleColDecision, Qt::DescendingOrder);
    m_requestFirewallRulesTable->horizontalHeader()->setSectionResizeMode(
        kFirewallRuleColDecision, QHeaderView::ResizeToContents);
    m_requestFirewallRulesTable->horizontalHeader()->setSectionResizeMode(
        kFirewallRuleColRule, QHeaderView::Stretch);
    m_requestFirewallRulesTable->horizontalHeader()->setSectionResizeMode(
        kFirewallRuleColStored, QHeaderView::Stretch);
    for (int c : {kFirewallRuleColRequests, kFirewallRuleColBlocked,
                  kFirewallRuleColLog})
        m_requestFirewallRulesTable->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_requestFirewallRulesTable);
    outer->addWidget(m_requestFirewallRulesTable, 1);

    auto *historyLabel = new QLabel(QStringLiteral("Recent requests"));
    historyLabel->setObjectName("sectionLabel");
    outer->addWidget(historyLabel);

    m_requestFirewallHistoryTable = new QTableWidget(0, 5);
    installColumnHeaderMenu(m_requestFirewallHistoryTable);
    m_requestFirewallHistoryTable->setObjectName("issueTable");
    m_requestFirewallHistoryTable->setHorizontalHeaderLabels(
        {QStringLiteral("Decision"), QStringLiteral("Method"),
         QStringLiteral("Destination"), QStringLiteral("Rule"),
         QStringLiteral("When")});
    m_requestFirewallHistoryTable->verticalHeader()->setVisible(false);
    m_requestFirewallHistoryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_requestFirewallHistoryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_requestFirewallHistoryTable->setShowGrid(false);
    m_requestFirewallHistoryTable->setSortingEnabled(true);
    m_requestFirewallHistoryTable->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    for (int c : {0, 1, 3, 4})
        m_requestFirewallHistoryTable->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_requestFirewallHistoryTable);
    outer->addWidget(m_requestFirewallHistoryTable, 1);
    installFirewallRuleEditing();

    refreshFirewallTables();
    return page;
}

void MainWindow::openLogForFirewallRule(const QString &rule)
{
    const QString filter = firewallRuleLogFilter(rule);
    if (filter.isEmpty()) {
        flashMessage(QStringLiteral(
            "\"%1\" covers every destination, so there is no narrower log "
            "filter to open.")
                         .arg(BackoffNetworkAccessManager::firewallRuleLabel(rule)));
        return;
    }
    openNetworkLogForUrl(filter);
}

void MainWindow::refreshFirewallTables()
{
    auto *manager = requestFirewallManager(m_networkAccess);
    const QStringList rules =
        manager ? manager->firewallRules() : requestFirewallWhitelist();
    const QStringList denyRules =
        manager ? manager->firewallDenyRules() : requestFirewallDenylist();
    const bool enabled =
        manager ? manager->firewallEnabled()
                : QSettings().value(kRequestFirewallEnabledSetting, true).toBool();

    if (m_requestFirewallEnabledCheck) {
        QSignalBlocker block(m_requestFirewallEnabledCheck);
        m_requestFirewallEnabledCheck->setChecked(enabled);
    }

    if (m_requestFirewallRulesTable) {
        TableRepaintGuard repaintGuard(m_requestFirewallRulesTable);
        // Off while rows go in, or each setItem would re-sort under the row
        // index being filled.
        m_requestFirewallRulesTable->setSortingEnabled(false);
        m_requestFirewallRulesTable->setRowCount(0);
        QHash<QString, QString> defaultPurposes;
        for (const BackoffNetworkAccessManager::FirewallRuleEntry &entry :
             defaultApprovedFirewallRules())
            defaultPurposes.insert(entry.rule, entry.purpose);
        const auto addRuleRow = [this, &defaultPurposes](const QString &rule,
                                                         bool denied) {
            const int row = m_requestFirewallRulesTable->rowCount();
            m_requestFirewallRulesTable->insertRow(row);
            const bool shipped = defaultPurposes.contains(rule);
            auto *decision = new QTableWidgetItem(
                denied    ? QStringLiteral("Denied")
                : shipped ? QStringLiteral("Allowed (default)")
                          : QStringLiteral("Allowed"));
            decision->setForeground(denied ? QColor(QStringLiteral("#f85149"))
                                           : QColor(QStringLiteral("#3fb950")));
            auto *label = new QTableWidgetItem(
                BackoffNetworkAccessManager::firewallRuleLabel(rule));
            auto *raw = new QTableWidgetItem(rule);
            // What this rule is deciding right now. The text lands in
            // updateFirewallRuleCounts(), which also keeps it current between
            // rebuilds as traffic arrives.
            QTableWidgetItem *requests = new SortTableWidgetItem;
            QTableWidgetItem *blocked = new SortTableWidgetItem;
            requests->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            blocked->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            // A cell, not a cell widget: a widget would be left stranded over
            // another row when the table is rebuilt under it.
            const QString filter = firewallRuleLogFilter(rule);
            auto *logCell = new QTableWidgetItem;
            logCell->setTextAlignment(Qt::AlignCenter);
            if (!filter.isEmpty()) {
                logCell->setIcon(themedOcticon(QStringLiteral("list-unordered"),
                                               QColor(QStringLiteral("#8b949e")),
                                               14));
                logCell->setToolTip(
                    QStringLiteral("Open the log filtered to %1").arg(filter));
            }
            for (QTableWidgetItem *item :
                 {decision, label, raw, requests, blocked, logCell}) {
                item->setData(Qt::UserRole, rule);
                item->setData(kFirewallRuleDeniedRole, denied);
            }
            // The count and log cells explain themselves; the rest carry what
            // a shipped rule is there for.
            if (shipped) {
                for (QTableWidgetItem *item : {decision, label, raw})
                    item->setToolTip(QStringLiteral("Shipped with ForkMesh: %1")
                                         .arg(defaultPurposes.value(rule)));
            }
            m_requestFirewallRulesTable->setItem(row, kFirewallRuleColDecision,
                                                 decision);
            m_requestFirewallRulesTable->setItem(row, kFirewallRuleColRule,
                                                 label);
            m_requestFirewallRulesTable->setItem(row, kFirewallRuleColStored,
                                                 raw);
            m_requestFirewallRulesTable->setItem(row, kFirewallRuleColRequests,
                                                 requests);
            m_requestFirewallRulesTable->setItem(row, kFirewallRuleColBlocked,
                                                 blocked);
            m_requestFirewallRulesTable->setItem(row, kFirewallRuleColLog,
                                                 logCell);
        };
        for (const QString &rule : denyRules)
            addRuleRow(rule, true);
        for (const QString &rule : rules)
            addRuleRow(rule, false);
        m_requestFirewallRulesTable->setSortingEnabled(true);
        m_requestFirewallRulesTable->resizeColumnsToContents();
        m_requestFirewallRulesTable->resizeRowsToContents();
    }

    if (m_requestFirewallHistoryTable) {
        TableRepaintGuard repaintGuard(m_requestFirewallHistoryTable);
        m_requestFirewallHistoryTable->setSortingEnabled(false);
        m_requestFirewallHistoryTable->setRowCount(0);
        for (const FirewallHistoryEntry &entry : std::as_const(m_requestFirewallHistory)) {
            const int row = m_requestFirewallHistoryTable->rowCount();
            m_requestFirewallHistoryTable->insertRow(row);
            auto *decision =
                new QTableWidgetItem(entry.allowed ? QStringLiteral("Allowed")
                                                   : QStringLiteral("Blocked"));
            decision->setForeground(entry.allowed ? QColor(QStringLiteral("#3fb950"))
                                                  : QColor(QStringLiteral("#f85149")));
            decision->setData(kFirewallHistoryMethodRole, entry.method);
            decision->setData(kFirewallHistoryUrlRole, entry.url);
            decision->setData(Qt::UserRole, entry.rule);
            decision->setData(kFirewallRuleDeniedRole, !entry.allowed);
            m_requestFirewallHistoryTable->setItem(row, 0, decision);
            m_requestFirewallHistoryTable->setItem(row, 1,
                new QTableWidgetItem(entry.method));
            auto *dest = new QTableWidgetItem(entry.destination);
            dest->setToolTip(entry.url);
            m_requestFirewallHistoryTable->setItem(row, 2, dest);
            m_requestFirewallHistoryTable->setItem(row, 3,
                new QTableWidgetItem(
                    entry.rule.isEmpty()
                        ? QStringLiteral("-")
                        : BackoffNetworkAccessManager::firewallRuleLabel(entry.rule)));
            auto *when = new QTableWidgetItem(formatRepoDate(entry.timestampMs));
            when->setData(Qt::UserRole, static_cast<qlonglong>(entry.timestampMs));
            m_requestFirewallHistoryTable->setItem(row, 4, when);
        }
        m_requestFirewallHistoryTable->setSortingEnabled(true);
        m_requestFirewallHistoryTable->sortItems(4, Qt::DescendingOrder);
        m_requestFirewallHistoryTable->resizeColumnsToContents();
        m_requestFirewallHistoryTable->resizeRowsToContents();
    }

    updateFirewallRuleCounts();
}

// The counts only, written into the rows already on screen. Traffic arrives
// several times a second on a busy page, and rebuilding the grid that often
// would drop the selection out from under the Remove rule button -- and scroll
// the reader back to the top while they were reading it.
void MainWindow::updateFirewallRuleCounts()
{
    auto *manager = requestFirewallManager(m_networkAccess);
    using Hits = BackoffNetworkAccessManager::FirewallRuleHits;
    const QHash<QString, Hits> traffic =
        manager ? manager->firewallRuleHits() : QHash<QString, Hits>();

    if (m_requestFirewallStatus) {
        int decided = 0;
        int blockedByRule = 0;
        for (const Hits &counts : traffic) {
            decided += counts.requests;
            blockedByRule += counts.blocked;
        }
        // The prompt's own answers are for destinations no rule covered yet,
        // so the per-rule counts above cannot account for them.
        int deniedAtPrompt = 0;
        for (const FirewallHistoryEntry &entry :
             std::as_const(m_requestFirewallHistory))
            if (!entry.allowed)
                ++deniedAtPrompt;
        const bool enabled =
            manager ? manager->firewallEnabled()
                    : QSettings()
                          .value(kRequestFirewallEnabledSetting, true)
                          .toBool();
        m_requestFirewallStatus->setText(
            QStringLiteral("%1 - %2 allowed, %3 denied rule(s) - %4 request(s) "
                           "decided by a rule this session, %5 blocked; %6 "
                           "blocked at the prompt")
                .arg(enabled ? QStringLiteral("Enabled")
                             : QStringLiteral("Disabled"))
                .arg(manager ? manager->firewallRules().size()
                             : requestFirewallWhitelist().size())
                .arg(manager ? manager->firewallDenyRules().size()
                             : requestFirewallDenylist().size())
                .arg(formatCount(decided), formatCount(blockedByRule),
                     formatCount(deniedAtPrompt)));
    }

    if (!m_requestFirewallRulesTable)
        return;
    // A sorted Requests or Blocked column would move rows under this loop;
    // re-enabling afterwards re-sorts once with the new counts.
    const bool wasSorting = m_requestFirewallRulesTable->isSortingEnabled();
    m_requestFirewallRulesTable->setSortingEnabled(false);
    for (int row = 0; row < m_requestFirewallRulesTable->rowCount(); ++row) {
        QTableWidgetItem *requests =
            m_requestFirewallRulesTable->item(row, kFirewallRuleColRequests);
        QTableWidgetItem *blocked =
            m_requestFirewallRulesTable->item(row, kFirewallRuleColBlocked);
        if (!requests || !blocked)
            continue;
        const QString rule = requests->data(Qt::UserRole).toString();
        const Hits counts = traffic.value(rule);
        requests->setText(formatCount(counts.requests));
        blocked->setText(formatCount(counts.blocked));
        requests->setData(kTableSortRole, counts.requests);
        blocked->setData(kTableSortRole, counts.blocked);
        if (counts.blocked > 0)
            blocked->setForeground(QColor(QStringLiteral("#f85149")));
        else
            blocked->setData(Qt::ForegroundRole, QVariant());
        const QString tip =
            counts.requests > 0
                ? QStringLiteral("%1 request(s) this session were decided by "
                                 "this rule, %2 of them blocked.")
                      .arg(counts.requests)
                      .arg(counts.blocked)
            : firewallRuleIsOnThisMachine(rule)
                ? QStringLiteral("Nothing yet -- this machine is exempt from "
                                 "the firewall, so a loopback rule never "
                                 "decides a request.")
                : QStringLiteral("No request this session was decided by this "
                                 "rule.");
        requests->setToolTip(tip);
        blocked->setToolTip(tip);
    }
    m_requestFirewallRulesTable->setSortingEnabled(wasSorting);
}

namespace {
constexpr int kNetworkEndpointKindRole = Qt::UserRole + 100;
constexpr int kNetworkEndpointMethodRole = Qt::UserRole + 101;
constexpr int kNetworkEndpointUrlRole = Qt::UserRole + 102;

QString networkDiagText(const QJsonObject &object, const char *key,
                        const QString &fallback = QString())
{
    const QJsonValue value = object.value(QString::fromLatin1(key));
    const QString text = value.toString();
    return text.isEmpty() ? fallback : text;
}

qint64 networkDiagInt(const QJsonObject &object, const char *key)
{
    return qRound64(object.value(QString::fromLatin1(key)).toDouble());
}

QString networkDiagBytes(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("0 B");
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2")
        .arg(QString::number(value, 'f', value >= 10.0 ? 1 : 2),
             QString::fromLatin1(units[unit]));
}

QString networkDiagTraffic(const QJsonObject &row, const char *framesKey,
                           const char *bytesKey, const char *controlKey)
{
    const qint64 frames = networkDiagInt(row, framesKey);
    const qint64 bytes = networkDiagInt(row, bytesKey);
    const qint64 control = networkDiagInt(row, controlKey);
    QString text = QStringLiteral("%1 frame(s), %2")
                       .arg(frames)
                       .arg(networkDiagBytes(bytes));
    if (control > 0)
        text += QStringLiteral(" + %1 control").arg(control);
    return text;
}

QString networkDiagLast(const QJsonObject &row, bool outgoing)
{
    const QString type = networkDiagText(row, outgoing ? "lastTxType" : "lastRxType");
    const QString op = networkDiagText(row, outgoing ? "lastTxOp" : "lastRxOp");
    const QString scope =
        networkDiagText(row, outgoing ? "lastTxScope" : "lastRxScope");
    const qint64 bytes =
        networkDiagInt(row, outgoing ? "lastTxBytes" : "lastRxBytes");
    const qint64 when = networkDiagInt(row, outgoing ? "lastTxMs" : "lastRxMs");

    QStringList parts;
    QString label = type;
    if (!op.isEmpty())
        label += QStringLiteral("/") + op;
    if (!label.isEmpty())
        parts << label;
    if (!scope.isEmpty())
        parts << scope;
    if (bytes > 0)
        parts << networkDiagBytes(bytes);
    if (when > 0)
        parts << formatRepoDate(when);
    return parts.isEmpty() ? QStringLiteral("-") : parts.join(QStringLiteral(" - "));
}

QTableWidgetItem *networkDiagItem(const QString &text,
                                  const QString &tooltip = QString())
{
    auto *item = new QTableWidgetItem(text);
    if (!tooltip.isEmpty())
        item->setToolTip(tooltip);
    return item;
}

QTableWidgetItem *networkDiagNumberItem(qint64 value, const QString &suffix = QString())
{
    auto *item = new QTableWidgetItem(
        suffix.isEmpty() ? QString::number(value)
                         : QStringLiteral("%1 %2").arg(value).arg(suffix));
    item->setData(Qt::UserRole, static_cast<qlonglong>(value));
    return item;
}

QString networkEndpointStatus(const BackoffNetworkAccessManager::EndpointStats &stats)
{
    if (!stats.lastError.isEmpty())
        return stats.lastStatus > 0
                   ? QStringLiteral("ERR %1").arg(stats.lastStatus)
                   : stats.lastError;
    if (stats.lastStatus > 0)
        return QString::number(stats.lastStatus);
    return QStringLiteral("Called");
}

QString networkEndpointFirewall(const BackoffNetworkAccessManager::EndpointStats &stats)
{
    if (stats.blocked > 0)
        return QStringLiteral("%1 allowed / %2 blocked")
            .arg(stats.allowed)
            .arg(stats.blocked);
    if (stats.allowed > 0)
        return QStringLiteral("Allowed");
    return stats.firewallDecision.isEmpty() ? QStringLiteral("-")
                                            : stats.firewallDecision;
}

QString networkRequestStatus(
    const BackoffNetworkAccessManager::RequestRecord &record)
{
    if (record.blocked)
        return QStringLiteral("Blocked");
    if (!record.error.isEmpty())
        return record.status > 0 ? QStringLiteral("ERR %1").arg(record.status)
                                 : record.error;
    if (record.status > 0)
        return QString::number(record.status);
    return record.finishedMs > 0 ? QStringLiteral("Done")
                                 : QStringLiteral("Pending");
}

constexpr qint64 kNetworkEndpointFlashMs = 2200;

QColor networkEndpointFlashColor(qint64 lastMs, qint64 nowMs)
{
    if (lastMs <= 0 || nowMs < lastMs)
        return QColor();
    const qint64 age = nowMs - lastMs;
    if (age >= kNetworkEndpointFlashMs)
        return QColor();
    const double t = 1.0 - double(age) / double(kNetworkEndpointFlashMs);
    QColor color(QStringLiteral("#238636"));
    color.setAlpha(qBound(0, int(92.0 * t), 92));
    return color;
}

bool networkApplyEndpointFlash(QTableWidget *table, int row, qint64 lastMs,
                               qint64 nowMs)
{
    const QColor color = networkEndpointFlashColor(lastMs, nowMs);
    if (!color.isValid())
        return false;
    for (int col = 0; col < table->columnCount(); ++col) {
        if (auto *item = table->item(row, col))
            item->setBackground(QBrush(color));
    }
    return true;
}

QWidget *networkTabPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    return page;
}

enum WebRequestsColumn {
    kWebRequestsColGroup = 0,
    kWebRequestsColLogs, // the per-row "open the log filtered to this group"
    kWebRequestsColRequests,
    kWebRequestsColShare,
    kWebRequestsCol2xx,
    kWebRequestsCol3xx,
    kWebRequestsCol4xx,
    kWebRequestsCol5xx,
    kWebRequestsColAvg,
    kWebRequestsColMax,
    kWebRequestsColCount,
};

class WebRequestsNumberItem final : public QTableWidgetItem
{
public:
    WebRequestsNumberItem(qlonglong value, const QString &text)
        : QTableWidgetItem(text)
    {
        setData(Qt::UserRole, value);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    bool operator<(const QTableWidgetItem &other) const override
    {
        return data(Qt::UserRole).toLongLong() <
               other.data(Qt::UserRole).toLongLong();
    }
};

class WebRequestsBarChart final : public QWidget
{
public:
    struct Entry {
        QString group;
        qlonglong requests = 0;
        QString valueLabel;
        QString tooltip;
    };

    explicit WebRequestsBarChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("webRequestsChart"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setFixedHeight(kRowHeight + 2 * kPadding);
    }

    void setEntries(const QVector<Entry> &entries, const QString &emptyText)
    {
        m_entries = entries;
        m_emptyText = emptyText;
        m_hovered = -1;
        setFixedHeight(qMax(1, int(m_entries.size())) * kRowHeight +
                       2 * kPadding);
        setCursor(Qt::ArrowCursor);
        update();
    }

    QString groupAt(int row) const
    {
        return row >= 0 && row < m_entries.size() ? m_entries.at(row).group
                                                 : QString();
    }

    // Clicking a label (or its bar) opens the log filtered to that route group.
    std::function<void(const QString &)> onGroupClicked;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QColor ink = palette().color(QPalette::WindowText);
        if (m_entries.isEmpty()) {
            QColor muted = ink;
            muted.setAlpha(150);
            painter.setPen(muted);
            painter.drawText(rect(), Qt::AlignCenter, m_emptyText);
            return;
        }

        QFont valueFont = font();
        valueFont.setPointSizeF(qMax(7.0, valueFont.pointSizeF() - 1.0));
        const QFontMetrics labelMetrics(font());
        const QFontMetrics valueMetrics(valueFont);

        qlonglong maxRequests = 1;
        int labelWidth = 0;
        int valueWidth = 0;
        for (const Entry &entry : m_entries) {
            maxRequests = qMax(maxRequests, entry.requests);
            labelWidth = qMax(labelWidth,
                              labelMetrics.horizontalAdvance(entry.group));
            valueWidth = qMax(
                valueWidth, valueMetrics.horizontalAdvance(entry.valueLabel));
        }
        labelWidth = qMin(labelWidth, width() / 3);
        const qreal barLeft = labelWidth + 12.0;
        const qreal barSpan =
            qMax(1.0, width() - barLeft - valueWidth - 12.0);

        for (int row = 0; row < m_entries.size(); ++row) {
            const Entry &entry = m_entries.at(row);
            const qreal top = kPadding + row * kRowHeight;

            if (row == m_hovered) {
                QColor highlight = ink;
                highlight.setAlpha(14);
                painter.fillRect(QRectF(0, top, width(), kRowHeight),
                                 highlight);
            }

            painter.setFont(font());
            painter.setPen(ink);
            painter.drawText(
                QRectF(0, top, labelWidth, kRowHeight),
                Qt::AlignRight | Qt::AlignVCenter,
                labelMetrics.elidedText(entry.group, Qt::ElideMiddle,
                                        labelWidth));

            const qreal barWidth = qMax(
                2.0, barSpan * (qreal(entry.requests) / qreal(maxRequests)));
            const QRectF bar(barLeft, top + (kRowHeight - kBarHeight) / 2.0,
                             barWidth, kBarHeight);
            QPainterPath path;
            path.setFillRule(Qt::WindingFill);
            path.addRoundedRect(bar, 4.0, 4.0);
            path.addRect(bar.left(), bar.top(),
                         qMin(4.0, bar.width() / 2.0), bar.height());
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#3987e5")));
            painter.drawPath(path);

            QColor valueInk = ink;
            valueInk.setAlpha(190);
            painter.setFont(valueFont);
            painter.setPen(valueInk);
            painter.drawText(
                QRectF(bar.right() + 6.0, top,
                       qMax(0.0, width() - bar.right() - 6.0), kRowHeight),
                Qt::AlignLeft | Qt::AlignVCenter, entry.valueLabel);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int row = rowAt(event->pos());
        if (row != m_hovered) {
            m_hovered = row;
            update();
        }
        setCursor(row >= 0 && onGroupClicked ? Qt::PointingHandCursor
                                             : Qt::ArrowCursor);
        if (row >= 0 && row < m_entries.size())
            QToolTip::showText(event->globalPosition().toPoint(),
                               m_entries.at(row).tooltip, this);
        else
            QToolTip::hideText();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int row = rowAt(event->pos());
        if (event->button() != Qt::LeftButton || row < 0 || !onGroupClicked) {
            QWidget::mousePressEvent(event);
            return;
        }
        onGroupClicked(m_entries.at(row).group);
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = -1;
        setCursor(Qt::ArrowCursor);
        update();
    }

private:
    int rowAt(const QPoint &pos) const
    {
        const int row = (pos.y() - kPadding) / kRowHeight;
        return row >= 0 && row < m_entries.size() ? row : -1;
    }

    static constexpr int kRowHeight = 26;
    static constexpr int kBarHeight = 12;
    static constexpr int kPadding = 4;
    QVector<Entry> m_entries;
    QString m_emptyText;
    int m_hovered = -1;
};
} // namespace

namespace {
void nextComposeNetworkSummary(QLabel *label)
{
    if (!label)
        return;
    QStringList parts;
    for (const char *key : {"nextNodesPart", "nextDiagPart"}) {
        const QString part = label->property(key).toString();
        if (!part.isEmpty())
            parts.append(part);
    }
    label->setText(parts.join(QString::fromUtf8(" \xC2\xB7 ")));
}
} // namespace

void MainWindow::updateNetworkCounts(int relays, int nodes, int hosts)
{
    if (relays >= 0)
        m_networkRelayCount = relays;
    if (nodes >= 0)
        m_networkNodeCount = nodes;
    if (hosts >= 0)
        m_networkHostCount = hosts;

    if (m_networkTabs) {
        const auto label = [](const QString &name, int count) {
            return count > 0 ? QStringLiteral("%1 (%2)").arg(name).arg(count)
                             : name;
        };
        if (m_networkTabs->count() > kNetworkNodesTab)
            m_networkTabs->setTabText(
                kNetworkNodesTab,
                label(QStringLiteral("Nodes"),
                      m_networkNodeCount + m_networkRelayCount));
        if (m_networkTabs->count() > kNetworkHostsTab)
            m_networkTabs->setTabText(
                kNetworkHostsTab,
                label(QStringLiteral("Hosts"), m_networkHostCount));
    }

    if (auto *railButton =
            dynamic_cast<ActivityRailButton *>(m_networkNavButton))
        railButton->setBadgeCount(m_networkRelayCount + m_networkNodeCount +
                                  m_networkHostCount);
}

void MainWindow::updateNetworkDeployBlink()
{
    // Standing a new mirror node up happens on the Hosts page inside Network,
    // and it runs for many minutes with nothing visible from other sections.
    // Blink the same green lamp the Control rail uses during a site deploy:
    // a Vultr mirror being created, a single-host install, or a fleet-wide
    // deploy pass all count.
    const bool installing = m_hostInstallDeploying && m_hostInstallProcess &&
                            m_hostInstallProcess->state() != QProcess::NotRunning;
    const bool deploying =
        m_vultrProvisionActive || installing || m_hostDeployRemaining > 0;
    if (auto *railButton =
            dynamic_cast<ActivityRailButton *>(m_networkNavButton))
        railButton->setActivityBlink(deploying);
}

QWidget *MainWindow::buildNetworkDiagnosticsSection()
{
    const bool next = Theme::isNext();
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 12, 24, 14);
    outer->setSpacing(next ? 10 : 8);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(10);
    QHBoxLayout *titleRow = header;
    QVBoxLayout *titleColumn = nullptr;
    if (next) {
        header->setSpacing(6);
        titleColumn = new QVBoxLayout;
        titleColumn->setContentsMargins(0, 0, 0, 0);
        titleColumn->setSpacing(2);
        titleRow = new QHBoxLayout;
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(6);
        titleColumn->addLayout(titleRow);
        header->addLayout(titleColumn, 1);
    }

    auto *title = new QLabel(QStringLiteral("Network"));
    title->setObjectName("sectionTitle");
    QFont titleFont = title->font();
    titleFont = forkmesh::resizedFont(titleFont, 4);
    titleFont.setBold(true);
    if (next) {
        titleFont = title->font();
        titleFont.setPixelSize(15);
        titleFont.setWeight(QFont::DemiBold);
    }
    title->setFont(titleFont);
    titleRow->addWidget(title);

    titleRow->addWidget(makeInlineHelpButton(
        QStringLiteral("About network diagnostics"), QStringLiteral(
            "The relays, nodes and hosts this client talks to, plus live endpoint "
            "usage, websocket Durable Object details and the outbound request "
            "firewall in one place.")));

    m_networkDiagnosticsStatus = new QLabel;
    m_networkDiagnosticsStatus->setObjectName("mutedLabel");
    if (next) {
        titleRow->addStretch(1);
        m_networkDiagnosticsStatus->setProperty("networkRole", QStringLiteral("summary"));
        m_networkDiagnosticsStatus->setTextFormat(Qt::PlainText);
        m_networkDiagnosticsStatus->setSizePolicy(QSizePolicy::Ignored,
                                                  QSizePolicy::Preferred);
        titleColumn->addWidget(m_networkDiagnosticsStatus);
    } else {
        header->addWidget(m_networkDiagnosticsStatus, 1);
    }

    // Every tab used to carry its own Refresh, which put two of them a row
    // apart in the same corner. This is the page's one refresh control, and it
    // reloads whichever tab is open.
    m_networkDiagnosticsRefreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_networkDiagnosticsRefreshButton->setObjectName("repoAction");
    m_networkDiagnosticsRefreshButton->setCursor(Qt::PointingHandCursor);
    m_networkDiagnosticsRefreshButton->setToolTip(
        QStringLiteral("Reload the open tab."));
    setOcticon(m_networkDiagnosticsRefreshButton, "sync", 14);
    connect(m_networkDiagnosticsRefreshButton, &QPushButton::clicked, this,
            &MainWindow::refreshCurrentNetworkTab);
    header->addWidget(m_networkDiagnosticsRefreshButton);
    if (next) {
        m_networkDiagnosticsRefreshButton->setProperty("networkRole", QStringLiteral("ghost"));
        auto *addNode = new QPushButton(QStringLiteral("Add node"));
        addNode->setObjectName(QStringLiteral("networkAddNodeButton"));
        addNode->setCursor(Qt::PointingHandCursor);
        addNode->setToolTip(QStringLiteral(
            "Add a host on the Hosts tab: its address, SSH login and node name, "
            "then Add host and Install ForkMesh. Hosts also creates Vultr mirrors."));
        addNode->setIcon(QIcon(kit::tintedIcon(QStringLiteral("plus"),
                                               Theme::color(Theme::Role::Bg), 14,
                                               page->devicePixelRatioF())));
        addNode->setIconSize(QSize(14, 14));
        connect(addNode, &QPushButton::clicked, this, [this] {
            showNetworkTab(kNetworkHostsTab);
            QTimer::singleShot(0, this, [this] {
                if (!m_hostIpEdit || !m_hostIpEdit->isVisible())
                    return;
                for (QWidget *w = m_hostIpEdit->parentWidget(); w; w = w->parentWidget()) {
                    if (auto *scroll = qobject_cast<QScrollArea *>(w)) {
                        scroll->ensureWidgetVisible(m_hostIpEdit, 0, 64);
                        break;
                    }
                }
                m_hostIpEdit->setFocus(Qt::OtherFocusReason);
            });
        });
        header->addWidget(addNode);
    }
    outer->addLayout(header);

    auto *tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("networkTabs"));
    tabs->setDocumentMode(true);
    tabs->tabBar()->setDrawBase(false); // the tabs are buttons, not a strip
    tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    outer->addWidget(tabs, 1);
    m_networkTabs = tabs;

    tabs->addTab(buildNodesSection(), QStringLiteral("Nodes"));
    tabs->addTab(buildHostsSection(), QStringLiteral("Hosts"));
    connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
        refreshNetworkTab(index);
        scheduleNavRecord();
    });

    auto *endpointsPage = networkTabPage();
    auto *endpointsLayout = qobject_cast<QVBoxLayout *>(endpointsPage->layout());
    m_networkEndpointsTable = new QTableWidget(0, 15);
    installColumnHeaderMenu(m_networkEndpointsTable);
    m_networkEndpointsTable->setObjectName("issueTable");
    m_networkEndpointsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Endpoint"), QStringLiteral("Kind"),
         QStringLiteral("Calls"), QStringLiteral("Rate /h"),
         QStringLiteral("Modest /h"), QStringLiteral("Importance"),
         QStringLiteral("Request needed"), QStringLiteral("Purpose"),
         QStringLiteral("Status"), QStringLiteral("Sent"),
         QStringLiteral("Received"), QStringLiteral("Last"),
         QStringLiteral("Request data"), QStringLiteral("Response data"),
         QStringLiteral("Firewall")});
    m_networkEndpointsTable->verticalHeader()->setVisible(false);
    m_networkEndpointsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_networkEndpointsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_networkEndpointsTable->setShowGrid(false);
    m_networkEndpointsTable->setSortingEnabled(true);
    networkPrepareFullTable(m_networkEndpointsTable);
    for (int c = 0; c < m_networkEndpointsTable->columnCount(); ++c)
        m_networkEndpointsTable->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_networkEndpointsTable);
    // The URL and its hit count open the log filtered to that URL; the other
    // cells open the request details.
    connect(m_networkEndpointsTable, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
        const QTableWidgetItem *endpointItem = m_networkEndpointsTable->item(row, 0);
        if ((column == 0 || column == 2) && endpointItem) {
            const bool http = endpointItem->data(kNetworkEndpointKindRole).toString() ==
                              QLatin1String("http");
            const QString endpoint =
                http ? endpointItem->data(kNetworkEndpointUrlRole).toString()
                     : endpointItem->text();
            if (endpoint.contains(QLatin1String("://"))) {
                openNetworkLogForUrl(http ? forkmesh::networkLogUrl(QUrl(endpoint))
                                          : endpoint);
                return;
            }
        }
        showEndpointRequestDetails(row, column);
    });
    // Right-click an HTTP endpoint to allow or deny that path, everything
    // beside and below it, its host or its whole domain.
    m_networkEndpointsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_networkEndpointsTable, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
        const QTableWidgetItem *hit = m_networkEndpointsTable->itemAt(pos);
        if (!hit || !requestFirewallManager(m_networkAccess))
            return;
        const QTableWidgetItem *endpointItem =
            m_networkEndpointsTable->item(hit->row(), 0);
        if (!endpointItem ||
            endpointItem->data(kNetworkEndpointKindRole).toString() !=
                QLatin1String("http"))
            return;
        const QUrl url(endpointItem->data(kNetworkEndpointUrlRole).toString());
        if (!url.isValid() || url.host().isEmpty())
            return;
        const QList<BackoffNetworkAccessManager::EndpointProfile> &catalog =
            networkEndpointCatalog();
        const int profileIndex = BackoffNetworkAccessManager::matchEndpointProfile(
            catalog, endpointItem->data(kNetworkEndpointMethodRole).toString(), url);
        // The same ladder the firewall prompt offers, less "every HTTPS request",
        // which is one right-click away from turning the firewall off.
        QList<BackoffNetworkAccessManager::FirewallRuleSuggestion> rules =
            BackoffNetworkAccessManager::firewallRuleSuggestions(
                url, profileIndex >= 0 ? catalog.at(profileIndex).pattern : QString());
        rules.removeIf([](const BackoffNetworkAccessManager::FirewallRuleSuggestion &rule) {
            return rule.rule.startsWith(QLatin1String("scheme:"));
        });
        QMenu menu(this);
        menu.setToolTipsVisible(true);
        const QString logUrl = forkmesh::networkLogUrl(url);
        connect(menu.addAction(QStringLiteral("View in logs")), &QAction::triggered,
                this, [this, logUrl] { openNetworkLogForUrl(logUrl); });
        for (const bool deny : {false, true}) {
            menu.addSection(deny ? QStringLiteral("Deny") : QStringLiteral("Allow"));
            for (const BackoffNetworkAccessManager::FirewallRuleSuggestion &suggestion :
                 std::as_const(rules)) {
                const QString rule = suggestion.rule;
                QAction *action = menu.addAction(
                    QStringLiteral("%1 %2  (%3)")
                        .arg(deny ? QStringLiteral("Deny") : QStringLiteral("Allow"),
                             BackoffNetworkAccessManager::firewallRuleEditText(rule),
                             suggestion.scope));
                action->setToolTip(suggestion.explanation);
                connect(action, &QAction::triggered, this, [this, rule, deny] {
                    auto *manager = requestFirewallManager(m_networkAccess);
                    if (!manager)
                        return;
                    if (deny)
                        manager->addFirewallDenyRule(rule);
                    else
                        manager->addFirewallRule(rule);
                    persistFirewallRules(manager);
                    refreshFirewallTables();
                    refreshNetworkDiagnostics();
                    flashMessage(QStringLiteral("Firewall %1 %2")
                                     .arg(deny ? QStringLiteral("denies")
                                               : QStringLiteral("allows"),
                                          BackoffNetworkAccessManager::firewallRuleLabel(rule)));
                });
            }
        }
        menu.exec(m_networkEndpointsTable->viewport()->mapToGlobal(pos));
    });
    connect(m_networkEndpointsTable->horizontalHeader(),
            &QHeaderView::sortIndicatorChanged, this,
            [this] { m_networkEndpointsUserSorted = true; });
    endpointsLayout->addWidget(m_networkEndpointsTable, 1);
    tabs->addTab(endpointsPage, QStringLiteral("Endpoints"));

    auto *webRequestsPage = networkTabPage();
    auto *webRequestsLayout =
        qobject_cast<QVBoxLayout *>(webRequestsPage->layout());
    auto *webRequestsRow = new QHBoxLayout;
    webRequestsRow->setContentsMargins(0, 0, 0, 0);
    webRequestsRow->setSpacing(8);
    m_networkWebRequestsStatus = new QLabel(QStringLiteral(
        "Counted live from the core nodes' logs while this app runs."));
    m_networkWebRequestsStatus->setObjectName("mutedLabel");
    m_networkWebRequestsStatus->setWordWrap(true);
    m_networkWebRequestsStatus->setTextInteractionFlags(
        Qt::TextSelectableByMouse);
    webRequestsRow->addWidget(m_networkWebRequestsStatus, 1);
    m_networkWebRequestsRange = new QComboBox;
    m_networkWebRequestsRange->setCursor(Qt::PointingHandCursor);
    m_networkWebRequestsRange->addItem(QStringLiteral("Last hour"), 60);
    m_networkWebRequestsRange->addItem(QStringLiteral("Last 6 hours"), 360);
    m_networkWebRequestsRange->addItem(QStringLiteral("Last 24 hours"), 1440);
    connect(m_networkWebRequestsRange, &QComboBox::currentIndexChanged, this,
            [this](int) {
                m_networkWebRequestsMinutes =
                    m_networkWebRequestsRange->currentData().toInt();
                refreshNetworkWebRequests();
            });
    webRequestsRow->addWidget(m_networkWebRequestsRange);
    webRequestsLayout->addLayout(webRequestsRow);

    auto *webRequestsChart = new WebRequestsBarChart;
    webRequestsChart->onGroupClicked = [this](const QString &group) {
        openFullLogForText(group);
    };
    m_networkWebRequestsChart = webRequestsChart;
    webRequestsLayout->addWidget(m_networkWebRequestsChart);

    m_networkWebRequestsTable = new QTableWidget(0, kWebRequestsColCount);
    installColumnHeaderMenu(m_networkWebRequestsTable);
    m_networkWebRequestsTable->setObjectName("issueTable");
    m_networkWebRequestsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Route group"), QStringLiteral("Log"),
         QStringLiteral("Requests"), QStringLiteral("Share"),
         QStringLiteral("2xx"), QStringLiteral("3xx"), QStringLiteral("4xx"),
         QStringLiteral("5xx"), QStringLiteral("Avg ms"),
         QStringLiteral("Max ms")});
    m_networkWebRequestsTable->verticalHeader()->setVisible(false);
    m_networkWebRequestsTable->setSelectionBehavior(
        QAbstractItemView::SelectRows);
    m_networkWebRequestsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_networkWebRequestsTable->setShowGrid(false);
    m_networkWebRequestsTable->setSortingEnabled(true);
    networkPrepareFullTable(m_networkWebRequestsTable);
    for (int c = 0; c < m_networkWebRequestsTable->columnCount(); ++c)
        m_networkWebRequestsTable->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_networkWebRequestsTable);
    connect(m_networkWebRequestsTable->horizontalHeader(),
            &QHeaderView::sortIndicatorChanged, this,
            [this] { m_networkWebRequestsUserSorted = true; });
    m_networkWebRequestsTable->setToolTip(QStringLiteral(
        "Click a route group, or the button beside it, to open the log "
        "filtered to that group's traffic."));
    connect(m_networkWebRequestsTable, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column == kWebRequestsColGroup ||
                    column == kWebRequestsColLogs)
                    openLogForWebRequestsRow(row);
            });
    webRequestsLayout->addWidget(m_networkWebRequestsTable, 1);
    m_networkWebRequestsTabIndex =
        tabs->addTab(webRequestsPage, QStringLiteral("Web Requests"));
    m_networkWebRequestsRefreshTimer = new QTimer(this);
    m_networkWebRequestsRefreshTimer->setSingleShot(true);
    m_networkWebRequestsRefreshTimer->setInterval(1000);
    connect(m_networkWebRequestsRefreshTimer, &QTimer::timeout, this,
            [this] { refreshNetworkWebRequests(); });

    auto *socketsPage = networkTabPage();
    auto *socketsLayout = qobject_cast<QVBoxLayout *>(socketsPage->layout());
    m_networkDiagnosticsTable = new QTableWidget(0, 9);
    installColumnHeaderMenu(m_networkDiagnosticsTable);
    m_networkDiagnosticsTable->setObjectName("issueTable");
    m_networkDiagnosticsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Connection"), QStringLiteral("Durable object"),
         QStringLiteral("Endpoint"), QStringLiteral("State"),
         QStringLiteral("Sent"), QStringLiteral("Received"),
         QStringLiteral("Last sent"), QStringLiteral("Last received"),
         QStringLiteral("Data carried")});
    m_networkDiagnosticsTable->verticalHeader()->setVisible(false);
    m_networkDiagnosticsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_networkDiagnosticsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_networkDiagnosticsTable->setShowGrid(false);
    m_networkDiagnosticsTable->setSortingEnabled(true);
    networkPrepareFullTable(m_networkDiagnosticsTable);
    for (int c = 0; c < m_networkDiagnosticsTable->columnCount(); ++c)
        m_networkDiagnosticsTable->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_networkDiagnosticsTable);
    socketsLayout->addWidget(m_networkDiagnosticsTable, 1);
    tabs->addTab(socketsPage, QStringLiteral("WebSocket Durable Objects"));

    auto *whitelistPage = networkTabPage();
    auto *whitelistLayout = qobject_cast<QVBoxLayout *>(whitelistPage->layout());
    auto *firewallRow = new QHBoxLayout;
    firewallRow->setContentsMargins(0, 0, 0, 0);
    firewallRow->setSpacing(8);
    m_requestFirewallEnabledCheck = new QCheckBox(QStringLiteral("Whitelist only"));
    m_requestFirewallEnabledCheck->setCursor(Qt::PointingHandCursor);
    m_requestFirewallEnabledCheck->setChecked(
        QSettings().value(kRequestFirewallEnabledSetting, true).toBool());
    connect(m_requestFirewallEnabledCheck, &QCheckBox::toggled, this,
            [this](bool on) {
                QSettings().setValue(kRequestFirewallEnabledSetting, on);
                if (auto *manager = requestFirewallManager(m_networkAccess))
                    manager->setFirewallEnabled(on);
                logSystem(on ? QStringLiteral("Firewall: whitelist-only mode enabled.")
                             : QStringLiteral("Firewall: whitelist-only mode disabled."));
                refreshFirewallTables();
                refreshNetworkDiagnostics();
            });
    firewallRow->addWidget(m_requestFirewallEnabledCheck);

    m_requestFirewallStatus = new QLabel;
    m_requestFirewallStatus->setObjectName("mutedLabel");
    firewallRow->addWidget(m_requestFirewallStatus, 1);
    whitelistLayout->addLayout(firewallRow);

    auto *ruleRow = new QHBoxLayout;
    ruleRow->setContentsMargins(0, 0, 0, 0);
    ruleRow->setSpacing(8);
    m_requestFirewallRuleEdit = new QLineEdit;
    m_requestFirewallRuleEdit->setPlaceholderText(
        QStringLiteral("Add firewall rule: ") + kFirewallRuleSyntaxHint);
    ruleRow->addWidget(m_requestFirewallRuleEdit, 1);
    auto *addRule = new QPushButton(QStringLiteral("Allow"));
    addRule->setObjectName("repoAction");
    addRule->setCursor(Qt::PointingHandCursor);
    setOcticon(addRule, "check-circle", 14);
    connect(addRule, &QPushButton::clicked, this,
            [this] { addFirewallRuleFromEdit(false); });
    connect(m_requestFirewallRuleEdit, &QLineEdit::returnPressed, this,
            [this] { addFirewallRuleFromEdit(false); });
    ruleRow->addWidget(addRule);
    auto *denyRule = new QPushButton(QStringLiteral("Deny"));
    denyRule->setObjectName("repoAction");
    denyRule->setCursor(Qt::PointingHandCursor);
    setOcticon(denyRule, "x", 14);
    connect(denyRule, &QPushButton::clicked, this,
            [this] { addFirewallRuleFromEdit(true); });
    ruleRow->addWidget(denyRule);
    m_requestFirewallRemoveButton = new QPushButton(QStringLiteral("Remove rule"));
    m_requestFirewallRemoveButton->setObjectName("repoAction");
    m_requestFirewallRemoveButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_requestFirewallRemoveButton, "trash", 14);
    connect(m_requestFirewallRemoveButton, &QPushButton::clicked, this,
            &MainWindow::removeSelectedFirewallRules);
    ruleRow->addWidget(m_requestFirewallRemoveButton);
    auto *restoreDefaults = new QPushButton(QStringLiteral("Restore defaults"));
    restoreDefaults->setObjectName("repoAction");
    restoreDefaults->setCursor(Qt::PointingHandCursor);
    restoreDefaults->setToolTip(QStringLiteral(
        "Allow again the URLs ForkMesh ships as approved: sign-in, sync, "
        "updates and the relay sockets"));
    setOcticon(restoreDefaults, "history", 14);
    connect(restoreDefaults, &QPushButton::clicked, this, [this] {
        auto *manager = requestFirewallManager(m_networkAccess);
        if (!manager)
            return;
        const int restored = restoreDefaultApprovedFirewallRules(manager);
        refreshFirewallTables();
        flashMessage(restored > 0
                         ? QStringLiteral("Restored %1 default firewall rule(s).").arg(restored)
                         : QStringLiteral("Every default firewall rule is already allowed."));
    });
    ruleRow->addWidget(restoreDefaults);
    whitelistLayout->addLayout(ruleRow);

    m_requestFirewallRulesTable = new QTableWidget(0, kFirewallRuleColCount);
    installColumnHeaderMenu(m_requestFirewallRulesTable, true);
    m_requestFirewallRulesTable->setObjectName("issueTable");
    m_requestFirewallRulesTable->setHorizontalHeaderLabels(
        {QStringLiteral("Decision"), QStringLiteral("Rule"),
         QStringLiteral("Stored as"), QStringLiteral("Requests"),
         QStringLiteral("Blocked"), QStringLiteral("Log")});
    m_requestFirewallRulesTable->setToolTip(QStringLiteral(
        "Requests and Blocked count what each rule decided this session. Click "
        "a row's log button to read that rule's traffic in the log; double-click "
        "a row to edit its rule."));
    connect(m_requestFirewallRulesTable, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column != kFirewallRuleColLog)
                    return;
                if (QTableWidgetItem *item =
                        m_requestFirewallRulesTable->item(row, column))
                    openLogForFirewallRule(item->data(Qt::UserRole).toString());
            });
    m_requestFirewallRulesTable->verticalHeader()->setVisible(false);
    m_requestFirewallRulesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_requestFirewallRulesTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_requestFirewallRulesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_requestFirewallRulesTable->setShowGrid(false);
    // Clicking a header sorts by it. Until then Decision orders the rows:
    // denied rules first, each group in its stored order.
    m_requestFirewallRulesTable->setSortingEnabled(true);
    m_requestFirewallRulesTable->horizontalHeader()->setSortIndicator(
        kFirewallRuleColDecision, Qt::DescendingOrder);
    networkPrepareFullTable(m_requestFirewallRulesTable);
    for (int c = 0; c < m_requestFirewallRulesTable->columnCount(); ++c)
        m_requestFirewallRulesTable->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_requestFirewallRulesTable);
    whitelistLayout->addWidget(m_requestFirewallRulesTable, 1);
    m_networkFirewallRulesTabIndex =
        tabs->addTab(whitelistPage, QStringLiteral("Firewall Rules"));

    auto *decisionsPage = networkTabPage();
    auto *decisionsLayout = qobject_cast<QVBoxLayout *>(decisionsPage->layout());
    auto *decisionsRow = new QHBoxLayout;
    decisionsRow->setContentsMargins(0, 0, 0, 0);
    decisionsRow->setSpacing(8);
    auto *decisionsHint = new QLabel(QStringLiteral(
        "Double-click a decision to edit the rule behind it."));
    decisionsHint->setObjectName("mutedLabel");
    decisionsRow->addWidget(decisionsHint);
    decisionsRow->addStretch();
    auto *editDecisionRule = new QPushButton(QStringLiteral("Edit rule"));
    editDecisionRule->setObjectName("repoAction");
    editDecisionRule->setCursor(Qt::PointingHandCursor);
    editDecisionRule->setToolTip(QStringLiteral(
        "Allow, deny or rescope the rule that decides the selected request"));
    setOcticon(editDecisionRule, "pencil", 14);
    decisionsRow->addWidget(editDecisionRule);
    auto *clearHistory = new QPushButton(QStringLiteral("Clear decisions"));
    clearHistory->setObjectName("repoAction");
    clearHistory->setCursor(Qt::PointingHandCursor);
    setOcticon(clearHistory, "trash", 14);
    connect(clearHistory, &QPushButton::clicked, this,
            &MainWindow::clearFirewallHistory);
    decisionsRow->addWidget(clearHistory);
    decisionsLayout->addLayout(decisionsRow);

    m_requestFirewallHistoryTable = new QTableWidget(0, 5);
    installColumnHeaderMenu(m_requestFirewallHistoryTable);
    m_requestFirewallHistoryTable->setObjectName("issueTable");
    m_requestFirewallHistoryTable->setHorizontalHeaderLabels(
        {QStringLiteral("Decision"), QStringLiteral("Method"),
         QStringLiteral("Destination"), QStringLiteral("Rule"),
         QStringLiteral("When")});
    m_requestFirewallHistoryTable->verticalHeader()->setVisible(false);
    m_requestFirewallHistoryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_requestFirewallHistoryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_requestFirewallHistoryTable->setShowGrid(false);
    m_requestFirewallHistoryTable->setSortingEnabled(true);
    networkPrepareFullTable(m_requestFirewallHistoryTable);
    for (int c = 0; c < m_requestFirewallHistoryTable->columnCount(); ++c)
        m_requestFirewallHistoryTable->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_requestFirewallHistoryTable);
    decisionsLayout->addWidget(m_requestFirewallHistoryTable, 1);
    tabs->addTab(decisionsPage, QStringLiteral("Recent Firewall Decisions"));
    installFirewallRuleEditing(editDecisionRule);

    if (next) {
        tabs->setTabText(m_networkWebRequestsTabIndex, QStringLiteral("Web requests"));
        tabs->setTabText(m_networkFirewallRulesTabIndex, QStringLiteral("Firewall rules"));
        tabs->setTabText(tabs->indexOf(decisionsPage),
                         QStringLiteral("Recent firewall decisions"));
        for (int index = 0; index < tabs->count(); ++index) {
            if (QLayout *tabLayout = tabs->widget(index)->layout()) {
                const QMargins margins = tabLayout->contentsMargins();
                tabLayout->setContentsMargins(margins.left(), 16, margins.right(),
                                              margins.bottom());
            }
        }
        for (QTableWidget *grid :
             {m_networkEndpointsTable, m_networkWebRequestsTable, m_networkDiagnosticsTable,
              m_requestFirewallRulesTable, m_requestFirewallHistoryTable}) {
            grid->verticalHeader()->setDefaultSectionSize(36);
            grid->setProperty("networkGrid", true);
        }
        for (QWidget *tabPage : {endpointsPage, webRequestsPage, socketsPage, whitelistPage,
                                 decisionsPage})
            if (auto *tabLayout = qobject_cast<QVBoxLayout *>(tabPage->layout()))
                tabLayout->setSpacing(12);
    }

    // Every grid on this page is a wide table of short columns, and a sortable
    // header otherwise reserves an arrow slot -- about a caption's worth of
    // width -- in each one, for an arrow only the sorted column ever draws.
    // The node directory alone was giving away close to a third of its width.
    for (QTableWidget *grid : page->findChildren<QTableWidget *>()) {
        if (auto *header =
                qobject_cast<KebabHeaderView *>(grid->horizontalHeader()))
            header->setCompactSections(true);
    }

    refreshFirewallTables();
    refreshNetworkDiagnostics();
    syncPackageSurfaces();
    return page;
}

void MainWindow::refreshNetworkDiagnostics()
{
    if (!m_networkDiagnosticsTable)
        return;
    if (m_sectionStack &&
        m_sectionStack->currentIndex() != kNetworkDiagnosticsSectionIndex)
        return;

    QList<QJsonObject> rows;
    if (m_backend)
        rows += m_backend->networkDiagnostics();
    QList<BackoffNetworkAccessManager::EndpointStats> endpointStats;
    if (auto *manager = requestFirewallManager(m_networkAccess))
        endpointStats = manager->endpointStats();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool endpointFadeActive = false;

    int connected = 0;
    qint64 txBytes = 0;
    qint64 rxBytes = 0;
    qint64 txFrames = 0;
    qint64 rxFrames = 0;
    for (const QJsonObject &row : std::as_const(rows)) {
        if (row.value(QStringLiteral("connected")).toBool())
            ++connected;
        txBytes += networkDiagInt(row, "txBytes");
        rxBytes += networkDiagInt(row, "rxBytes");
        txFrames += networkDiagInt(row, "txFrames");
        rxFrames += networkDiagInt(row, "rxFrames");
    }
    if (m_networkDiagnosticsStatus) {
        const QString status =
            QStringLiteral("%1 endpoint(s) - %2/%3 websocket(s) connected - "
                           "%4 sent in %5 frame(s), %6 received in %7 frame(s)")
                .arg(endpointStats.size() + rows.size())
                .arg(connected)
                .arg(rows.size())
                .arg(networkDiagBytes(txBytes))
                .arg(txFrames)
                .arg(networkDiagBytes(rxBytes))
                .arg(rxFrames);
        if (Theme::isNext()) {
            const qsizetype endpoints = endpointStats.size() + rows.size();
            m_networkDiagnosticsStatus->setToolTip(status);
            m_networkDiagnosticsStatus->setProperty(
                "nextDiagPart",
                QString::fromUtf8("%1 endpoint%2 \xC2\xB7 %3 / %4 websocket%5 connected "
                                  "\xC2\xB7 %6 sent, %7 received")
                    .arg(endpoints)
                    .arg(endpoints == 1 ? QString() : QStringLiteral("s"))
                    .arg(connected)
                    .arg(rows.size())
                    .arg(rows.size() == 1 ? QString() : QStringLiteral("s"))
                    .arg(networkDiagBytes(txBytes), networkDiagBytes(rxBytes)));
            nextComposeNetworkSummary(m_networkDiagnosticsStatus);
            QWidget *nodesPage =
                m_networkTabs ? m_networkTabs->widget(kNetworkNodesTab) : nullptr;
            if (auto *tile = nodesPage ? nodesPage->findChild<QFrame *>(
                                             QStringLiteral("networkStatTraffic"))
                                       : nullptr) {
                if (auto *value = tile->findChild<QLabel *>(QStringLiteral("networkStatValue")))
                    value->setText(networkDiagBytes(txBytes + rxBytes));
                if (auto *hint = tile->findChild<QLabel *>(QStringLiteral("networkStatHint")))
                    hint->setText(QString::fromUtf8("%1 sent \xC2\xB7 %2 received")
                                      .arg(networkDiagBytes(txBytes), networkDiagBytes(rxBytes)));
                tile->setToolTip(QStringLiteral(
                    "Bytes this app sent and received over its relay websockets"));
            }
        } else {
            m_networkDiagnosticsStatus->setText(status);
        }
    }

    if (m_networkTabs && m_networkTabs->currentIndex() <= kNetworkHostsTab)
        return;

    if (m_networkEndpointsTable) {
        TableRepaintGuard repaintGuard(m_networkEndpointsTable);
        const int sortColumn =
            m_networkEndpointsTable->horizontalHeader()->sortIndicatorSection();
        const Qt::SortOrder sortOrder =
            m_networkEndpointsTable->horizontalHeader()->sortIndicatorOrder();
        m_networkEndpointsTable->setSortingEnabled(false);
        m_networkEndpointsTable->setRowCount(0);

        // What each call is for and how much of it is reasonable, from the
        // shipped endpoint catalog, beside how often it is really happening.
        const QList<BackoffNetworkAccessManager::EndpointProfile> &catalog =
            networkEndpointCatalog();
        const auto setProfileCells = [this, &catalog](int row, const QString &method,
                                                      const QUrl &url,
                                                      double observedPerHour,
                                                      bool rateKnown) {
            const int index =
                BackoffNetworkAccessManager::matchEndpointProfile(catalog, method, url);
            const BackoffNetworkAccessManager::EndpointProfile *profile =
                index >= 0 ? &catalog.at(index) : nullptr;
            auto *rate = rateKnown ? networkDiagNumberItem(qRound64(observedPerHour))
                                   : networkDiagItem(QStringLiteral("-"));
            auto *modest = profile ? networkDiagNumberItem(qRound64(profile->modestPerHour))
                                   : networkDiagItem(QStringLiteral("-"));
            auto *importance = profile
                                   ? networkDiagNumberItem(profile->importance,
                                                           QStringLiteral("/ 10"))
                                   : networkDiagItem(QStringLiteral("-"));
            auto *needed = profile ? networkDiagNumberItem(profile->requestNeeded,
                                                           QStringLiteral("%"))
                                   : networkDiagItem(QStringLiteral("-"));
            auto *purpose = networkDiagItem(
                profile ? profile->description
                        : QStringLiteral("Not in the endpoint catalog"));
            if (profile) {
                const QString detail =
                    QStringLiteral("%1\n\nTriggered by: %2\nImportance: %3/10\n"
                                   "Modest rate: about %4 per hour\n"
                                   "Likelihood it has to be a request: %5%\n"
                                   "Push, event or refresh instead: %6")
                        .arg(profile->description,
                             profile->trigger.isEmpty() ? QStringLiteral("-")
                                                        : profile->trigger)
                        .arg(profile->importance)
                        .arg(profile->modestPerHour)
                        .arg(profile->requestNeeded)
                        .arg(profile->alternative.isEmpty() ? QStringLiteral("none")
                                                            : profile->alternative) +
                    // The raw catalog text: the table refreshes too often to
                    // read source files for line numbers here.
                    (profile->sources.isEmpty()
                         ? QString()
                         : QStringLiteral("\nCode: ") +
                               profile->sources.join(QStringLiteral("\n      ")));
                for (QTableWidgetItem *item : {rate, modest, importance, needed, purpose})
                    item->setToolTip(detail);
                if (rateKnown && observedPerHour > profile->modestPerHour) {
                    rate->setForeground(QColor(QStringLiteral("#d29922")));
                    rate->setToolTip(QStringLiteral("Above the modest rate of about %1 "
                                                    "per hour\n\n")
                                         .arg(profile->modestPerHour) +
                                     detail);
                }
            } else {
                purpose->setForeground(QColor(QStringLiteral("#8b949e")));
            }
            m_networkEndpointsTable->setItem(row, 3, rate);
            m_networkEndpointsTable->setItem(row, 4, modest);
            m_networkEndpointsTable->setItem(row, 5, importance);
            m_networkEndpointsTable->setItem(row, 6, needed);
            m_networkEndpointsTable->setItem(row, 7, purpose);
        };

        for (const BackoffNetworkAccessManager::EndpointStats &stats :
             std::as_const(endpointStats)) {
            const int row = m_networkEndpointsTable->rowCount();
            m_networkEndpointsTable->insertRow(row);
            auto *endpoint = networkDiagItem(stats.endpoint, stats.endpoint);
            endpoint->setIcon(themedOcticon(QStringLiteral("link"),
                                            QColor(QStringLiteral("#8b949e")), 15));
            endpoint->setData(kNetworkEndpointKindRole, QStringLiteral("http"));
            endpoint->setData(kNetworkEndpointMethodRole, stats.method);
            endpoint->setData(kNetworkEndpointUrlRole, stats.endpoint);
            endpoint->setToolTip(stats.endpoint +
                                 QStringLiteral("\nClick to view this URL in the logs"));
            m_networkEndpointsTable->setItem(row, 0, endpoint);
            m_networkEndpointsTable->setItem(row, 1,
                                             networkDiagItem(stats.method));
            auto *calls = networkDiagNumberItem(stats.calls);
            calls->setToolTip(
                QStringLiteral("%1 allowed, %2 blocked. Click to view these hits in the logs")
                    .arg(stats.allowed)
                    .arg(stats.blocked));
            m_networkEndpointsTable->setItem(row, 2, calls);
            setProfileCells(row, stats.method,
                            QUrl(stats.lastFullUrl.isEmpty() ? stats.endpoint
                                                             : stats.lastFullUrl),
                            BackoffNetworkAccessManager::observedCallsPerHour(stats, nowMs),
                            true);
            auto *status = networkDiagItem(networkEndpointStatus(stats),
                                           stats.lastError);
            if (stats.blocked > 0)
                status->setForeground(QColor(QStringLiteral("#f85149")));
            else if (stats.failures > 0)
                status->setForeground(QColor(QStringLiteral("#d29922")));
            else
                status->setForeground(QColor(QStringLiteral("#3fb950")));
            m_networkEndpointsTable->setItem(row, 8, status);
            m_networkEndpointsTable->setItem(
                row, 9, networkDiagItem(networkDiagBytes(stats.uploadBytes)));
            m_networkEndpointsTable->setItem(
                row, 10, networkDiagItem(networkDiagBytes(stats.downloadBytes)));
            auto *last = networkDiagItem(formatRepoDate(stats.lastCalledMs));
            last->setData(Qt::UserRole, static_cast<qlonglong>(stats.lastCalledMs));
            m_networkEndpointsTable->setItem(row, 11, last);
            m_networkEndpointsTable->setItem(
                row, 12,
                networkDiagItem(stats.lastRequestData, stats.lastFullUrl));
            m_networkEndpointsTable->setItem(
                row, 13,
                networkDiagItem(stats.lastResponseData, stats.lastResponseData));
            m_networkEndpointsTable->setItem(
                row, 14, networkDiagItem(networkEndpointFirewall(stats)));
            endpointFadeActive |= networkApplyEndpointFlash(
                m_networkEndpointsTable, row, stats.lastCalledMs, nowMs);
        }

        for (const QJsonObject &rowObject : std::as_const(rows)) {
            const int row = m_networkEndpointsTable->rowCount();
            m_networkEndpointsTable->insertRow(row);
            const QString endpointText =
                networkDiagText(rowObject, "endpoint", QStringLiteral("-"));
            const QString kind = networkDiagText(rowObject, "kind",
                                                 QStringLiteral("websocket"));
            auto *endpoint = networkDiagItem(endpointText, endpointText);
            endpoint->setIcon(themedOcticon(
                kind == QLatin1String("repo-host") ? QStringLiteral("repo")
                                                   : QStringLiteral("broadcast"),
                QColor(QStringLiteral("#8b949e")), 15));
            endpoint->setData(kNetworkEndpointKindRole, QStringLiteral("websocket"));
            m_networkEndpointsTable->setItem(row, 0, endpoint);
            m_networkEndpointsTable->setItem(
                row, 1,
                networkDiagItem(kind == QLatin1String("repo-host")
                                    ? QStringLiteral("WS repo")
                                    : QStringLiteral("WS mainnode")));
            const qint64 frameCalls = networkDiagInt(rowObject, "txFrames") +
                                      networkDiagInt(rowObject, "rxFrames") +
                                      networkDiagInt(rowObject, "txControlFrames") +
                                      networkDiagInt(rowObject, "rxControlFrames");
            m_networkEndpointsTable->setItem(row, 2,
                                             networkDiagNumberItem(frameCalls));
            setProfileCells(row, QStringLiteral("WS"), QUrl(endpointText), 0, false);
            auto *state = networkDiagItem(
                networkDiagText(rowObject, "state", QStringLiteral("Unknown")));
            if (rowObject.value(QStringLiteral("connected")).toBool())
                state->setForeground(QColor(QStringLiteral("#3fb950")));
            else
                state->setForeground(QColor(QStringLiteral("#8b949e")));
            m_networkEndpointsTable->setItem(row, 8, state);
            m_networkEndpointsTable->setItem(
                row, 9,
                networkDiagItem(networkDiagBytes(networkDiagInt(rowObject, "txBytes"))));
            m_networkEndpointsTable->setItem(
                row, 10,
                networkDiagItem(networkDiagBytes(networkDiagInt(rowObject, "rxBytes"))));
            const qint64 lastMs = qMax(networkDiagInt(rowObject, "lastTxMs"),
                                      networkDiagInt(rowObject, "lastRxMs"));
            auto *last = networkDiagItem(formatRepoDate(lastMs));
            last->setData(Qt::UserRole, static_cast<qlonglong>(lastMs));
            m_networkEndpointsTable->setItem(row, 11, last);
            m_networkEndpointsTable->setItem(row, 12,
                                             networkDiagItem(networkDiagLast(
                                                 rowObject, true)));
            m_networkEndpointsTable->setItem(row, 13,
                                             networkDiagItem(networkDiagLast(
                                                 rowObject, false)));
            m_networkEndpointsTable->setItem(row, 14,
                                             networkDiagItem(QStringLiteral("Allowed")));
            endpointFadeActive |= networkApplyEndpointFlash(
                m_networkEndpointsTable, row, lastMs, nowMs);
        }
        m_networkEndpointsTable->setSortingEnabled(true);
        if (m_networkEndpointsUserSorted)
            m_networkEndpointsTable->sortItems(sortColumn, sortOrder);
        else
            m_networkEndpointsTable->sortItems(11, Qt::DescendingOrder);
    }
    if (endpointFadeActive && !m_networkEndpointFadeScheduled) {
        m_networkEndpointFadeScheduled = true;
        QTimer::singleShot(120, this, [this] {
            m_networkEndpointFadeScheduled = false;
            if (m_sectionStack &&
                m_sectionStack->currentIndex() == kNetworkDiagnosticsSectionIndex)
                refreshNetworkDiagnostics();
        });
    }

    TableRepaintGuard repaintGuard(m_networkDiagnosticsTable);
    m_networkDiagnosticsTable->setSortingEnabled(false);
    m_networkDiagnosticsTable->setRowCount(0);
    for (const QJsonObject &rowObject : std::as_const(rows)) {
        const int row = m_networkDiagnosticsTable->rowCount();
        m_networkDiagnosticsTable->insertRow(row);

        const QString data = networkDiagText(rowObject, "data");
        auto *connection = networkDiagItem(
            networkDiagText(rowObject, "connection", QStringLiteral("WebSocket")),
            data);
        const QString kind = networkDiagText(rowObject, "kind");
        connection->setIcon(themedOcticon(
            kind == QLatin1String("repo-host") ? QStringLiteral("repo")
                                               : QStringLiteral("broadcast"),
            QColor(QStringLiteral("#8b949e")), 15));
        m_networkDiagnosticsTable->setItem(row, 0, connection);

        m_networkDiagnosticsTable->setItem(
            row, 1, networkDiagItem(networkDiagText(rowObject, "durableObject",
                                                    QStringLiteral("-"))));
        m_networkDiagnosticsTable->setItem(
            row, 2, networkDiagItem(networkDiagText(rowObject, "endpoint",
                                                    QStringLiteral("-"))));

        const QString stateText = networkDiagText(rowObject, "state",
                                                  QStringLiteral("Unknown"));
        auto *state = networkDiagItem(
            stateText,
            QStringLiteral("Connected since %1")
                .arg(formatRepoDate(networkDiagInt(rowObject, "connectedAtMs"))));
        if (rowObject.value(QStringLiteral("connected")).toBool())
            state->setForeground(QColor(QStringLiteral("#3fb950")));
        else if (stateText == QLatin1String("Connecting"))
            state->setForeground(QColor(QStringLiteral("#d29922")));
        else
            state->setForeground(QColor(QStringLiteral("#8b949e")));
        m_networkDiagnosticsTable->setItem(row, 3, state);

        m_networkDiagnosticsTable->setItem(
            row, 4,
            networkDiagItem(networkDiagTraffic(rowObject, "txFrames", "txBytes",
                                               "txControlFrames")));
        m_networkDiagnosticsTable->setItem(
            row, 5,
            networkDiagItem(networkDiagTraffic(rowObject, "rxFrames", "rxBytes",
                                               "rxControlFrames")));
        m_networkDiagnosticsTable->setItem(
            row, 6, networkDiagItem(networkDiagLast(rowObject, true)));
        m_networkDiagnosticsTable->setItem(
            row, 7, networkDiagItem(networkDiagLast(rowObject, false)));
        m_networkDiagnosticsTable->setItem(row, 8, networkDiagItem(data, data));
    }
    m_networkDiagnosticsTable->setSortingEnabled(true);
}

void MainWindow::refreshNetworkWebRequests()
{
    if (!m_networkWebRequestsTable)
        return;
    if (m_sectionStack &&
        m_sectionStack->currentIndex() != kNetworkDiagnosticsSectionIndex)
        return;
    if (m_networkTabs &&
        m_networkTabs->currentIndex() != m_networkWebRequestsTabIndex)
        return;

    if (m_webRequestTally.isEmpty() && m_networkWebRequestsStatus) {
        QString why;
        if (!m_coreLogIdleReason.isEmpty())
            why = m_coreLogIdleReason;
        else if (!m_coreLogFeeds.isEmpty())
            why = QStringLiteral(
                "Following the core nodes' logs; no /api request has been "
                "reported yet. Each node reports its requests every 10 "
                "seconds.");
        else
            why = QStringLiteral(
                "The core nodes' logs are not being followed. Turn on \"Follow "
                "the core nodes' logs\" in Settings to count web requests.");
        m_networkWebRequestsStatus->setText(
            why + QStringLiteral(" Requests are counted live from those logs "
                                 "and are not stored anywhere."));
    }
    if (m_webRequestTally.isEmpty()) {
        renderNetworkWebRequests(QJsonObject{
            {QStringLiteral("ok"), true},
            {QStringLiteral("windowMinutes"), m_networkWebRequestsMinutes},
            {QStringLiteral("groups"), QJsonArray{}},
            {QStringLiteral("quietStatus"), true}});
        return;
    }
    renderNetworkWebRequests(m_webRequestTally.summary(
        m_networkWebRequestsMinutes, QDateTime::currentMSecsSinceEpoch()));
}

#ifdef FORKMESH_WINDOW_TESTS
QJsonObject MainWindow::testWebRequestsSummary(int windowMinutes,
                                               qint64 nowMs) const
{
    return m_webRequestTally.summary(windowMinutes, nowMs);
}
#endif // FORKMESH_WINDOW_TESTS

void MainWindow::renderNetworkWebRequests(const QJsonObject &payload)
{
    if (!m_networkWebRequestsTable || !m_networkWebRequestsChart)
        return;

    QVector<QJsonObject> groups;
    const QJsonArray groupsPayload =
        payload.value(QStringLiteral("groups")).toArray();
    groups.reserve(groupsPayload.size());
    for (const QJsonValue &value : groupsPayload)
        groups.append(value.toObject());
    std::sort(groups.begin(), groups.end(),
              [](const QJsonObject &a, const QJsonObject &b) {
                  return a.value(QStringLiteral("requests")).toDouble() >
                         b.value(QStringLiteral("requests")).toDouble();
              });
    qlonglong totalRequests = 0;
    qlonglong totalErrors = 0;
    for (const QJsonObject &group : std::as_const(groups)) {
        totalRequests +=
            qlonglong(group.value(QStringLiteral("requests")).toDouble());
        totalErrors +=
            qlonglong(group.value(QStringLiteral("errors")).toDouble());
    }
    const auto share = [totalRequests](qlonglong requests) {
        return totalRequests > 0 ? 100.0 * requests / totalRequests : 0.0;
    };

    QVector<WebRequestsBarChart::Entry> chartEntries;
    for (const QJsonObject &group : std::as_const(groups)) {
        if (chartEntries.size() >= 12)
            break;
        const QJsonObject classes =
            group.value(QStringLiteral("classes")).toObject();
        WebRequestsBarChart::Entry entry;
        entry.group = group.value(QStringLiteral("group")).toString();
        entry.requests =
            qlonglong(group.value(QStringLiteral("requests")).toDouble());
        entry.valueLabel = formatCount(entry.requests);
        entry.tooltip =
            QStringLiteral("%1\n%2 request(s) - %3% of the window\n"
                           "avg %4 ms - max %5 ms\n2xx %6 - 3xx %7 - 4xx %8 - "
                           "5xx %9")
                .arg(entry.group, formatCount(entry.requests),
                     QString::number(share(entry.requests), 'f', 1),
                     QString::number(qlonglong(
                         group.value(QStringLiteral("avg_ms")).toDouble())),
                     QString::number(qlonglong(
                         group.value(QStringLiteral("dur_ms_max")).toDouble())),
                     formatCount(qlonglong(
                         classes.value(QStringLiteral("2xx")).toDouble())),
                     formatCount(qlonglong(
                         classes.value(QStringLiteral("3xx")).toDouble())),
                     formatCount(qlonglong(
                         classes.value(QStringLiteral("4xx")).toDouble())))
                .arg(formatCount(qlonglong(
                    classes.value(QStringLiteral("5xx")).toDouble())));
        chartEntries.append(entry);
    }
    static_cast<WebRequestsBarChart *>(m_networkWebRequestsChart)
        ->setEntries(chartEntries,
                     QStringLiteral(
                         "No web requests reached the worker in this window."));
    // refreshNetworkWebRequests already said why an empty tally is empty.
    const bool quietStatus =
        payload.value(QStringLiteral("quietStatus")).toBool();

    {
        TableRepaintGuard repaintGuard(m_networkWebRequestsTable);
        const int sortColumn = m_networkWebRequestsTable->horizontalHeader()
                                   ->sortIndicatorSection();
        const Qt::SortOrder sortOrder =
            m_networkWebRequestsTable->horizontalHeader()->sortIndicatorOrder();
        m_networkWebRequestsTable->setSortingEnabled(false);
        m_networkWebRequestsTable->setRowCount(0);
        for (const QJsonObject &group : std::as_const(groups)) {
            const QJsonObject classes =
                group.value(QStringLiteral("classes")).toObject();
            const qlonglong requests =
                qlonglong(group.value(QStringLiteral("requests")).toDouble());
            const int row = m_networkWebRequestsTable->rowCount();
            m_networkWebRequestsTable->insertRow(row);
            const QString name =
                group.value(QStringLiteral("group")).toString();
            auto *groupItem = networkDiagItem(
                name, QStringLiteral("%1\nClick to open the log filtered to "
                                     "this route group.")
                          .arg(name));
            // The table's stylesheet owns ::item colour, so the label reads as
            // a link by its underline rather than by a foreground brush.
            QFont groupFont = m_networkWebRequestsTable->font();
            groupFont.setUnderline(true);
            groupItem->setFont(groupFont);
            m_networkWebRequestsTable->setItem(row, kWebRequestsColGroup,
                                               groupItem);
            // A cell, not a cell widget: widgets do not follow their items when
            // the table re-sorts, and a removed row leaves its widget stranded
            // over the first row of the next render.
            auto *logItem = networkDiagItem(
                QString(),
                QStringLiteral("Open the log filtered to %1").arg(name));
            logItem->setIcon(themedOcticon(QStringLiteral("list-unordered"),
                                           QColor(QStringLiteral("#8b949e")),
                                           14));
            logItem->setTextAlignment(Qt::AlignCenter);
            m_networkWebRequestsTable->setItem(row, kWebRequestsColLogs,
                                               logItem);
            m_networkWebRequestsTable->setItem(
                row, kWebRequestsColRequests,
                new WebRequestsNumberItem(requests, formatCount(requests)));
            const double sharePercent = share(requests);
            m_networkWebRequestsTable->setItem(
                row, kWebRequestsColShare,
                new WebRequestsNumberItem(
                    qlonglong(sharePercent * 10.0),
                    QStringLiteral("%1%").arg(sharePercent, 0, 'f', 1)));
            const struct {
                WebRequestsColumn column;
                const char *key;
            } classColumns[] = {{kWebRequestsCol2xx, "2xx"},
                                {kWebRequestsCol3xx, "3xx"},
                                {kWebRequestsCol4xx, "4xx"},
                                {kWebRequestsCol5xx, "5xx"}};
            for (const auto &classColumn : classColumns) {
                const qlonglong count = qlonglong(
                    classes.value(QLatin1String(classColumn.key)).toDouble());
                auto *item =
                    new WebRequestsNumberItem(count, formatCount(count));
                if (classColumn.column == kWebRequestsCol5xx && count > 0)
                    item->setForeground(QColor(QStringLiteral("#f85149")));
                m_networkWebRequestsTable->setItem(row, classColumn.column,
                                                   item);
            }
            const qlonglong avgMs =
                qlonglong(group.value(QStringLiteral("avg_ms")).toDouble());
            const qlonglong maxMs = qlonglong(
                group.value(QStringLiteral("dur_ms_max")).toDouble());
            m_networkWebRequestsTable->setItem(
                row, kWebRequestsColAvg,
                new WebRequestsNumberItem(avgMs, formatCount(avgMs)));
            m_networkWebRequestsTable->setItem(
                row, kWebRequestsColMax,
                new WebRequestsNumberItem(maxMs, formatCount(maxMs)));
        }
        m_networkWebRequestsTable->setSortingEnabled(true);
        if (m_networkWebRequestsUserSorted)
            m_networkWebRequestsTable->sortItems(sortColumn, sortOrder);
        else
            m_networkWebRequestsTable->sortItems(kWebRequestsColRequests,
                                                 Qt::DescendingOrder);
        m_networkWebRequestsTable->resizeColumnsToContents();
        m_networkWebRequestsTable->resizeRowsToContents();
    }

    if (m_networkWebRequestsStatus && !quietStatus) {
        const int windowMinutes =
            payload.value(QStringLiteral("windowMinutes"))
                .toInt(m_networkWebRequestsMinutes);
        const QString window =
            windowMinutes % 60 == 0
                ? QStringLiteral("%1 hour(s)").arg(windowMinutes / 60)
                : QStringLiteral("%1 minute(s)").arg(windowMinutes);
        QString text =
            QStringLiteral(
                "%1 request(s) across %2 route group(s) in the last %3 - "
                "%4 5xx")
                .arg(formatCount(totalRequests), formatCount(groups.size()),
                     window, formatCount(totalErrors));
        const qint64 countingSinceMs = qint64(
            payload.value(QStringLiteral("countingSinceMs")).toDouble());
        const qint64 nowMs = qint64(payload.value(QStringLiteral("now"))
                                        .toDouble(double(
                                            QDateTime::currentMSecsSinceEpoch())));
        if (countingSinceMs > 0 &&
            countingSinceMs > nowMs - qint64(windowMinutes) * 60000)
            text += QStringLiteral(" - counted from the core nodes' logs since %1")
                        .arg(QDateTime::fromMSecsSinceEpoch(countingSinceMs)
                                 .toString(QStringLiteral("HH:mm")));
        m_networkWebRequestsStatus->setText(text);
    }
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testWebRequestsChartGroupAt(int row) const
{
    auto *chart = static_cast<WebRequestsBarChart *>(m_networkWebRequestsChart);
    return chart ? chart->groupAt(row) : QString();
}

QString MainWindow::testClickWebRequestsChartLabel(int row)
{
    auto *chart = static_cast<WebRequestsBarChart *>(m_networkWebRequestsChart);
    if (!chart)
        return QString();
    const QString group = chart->groupAt(row);
    if (group.isEmpty() || !chart->onGroupClicked)
        return QString();
    chart->onGroupClicked(group);
    return group;
}
#endif

void MainWindow::openLogForWebRequestsRow(int row)
{
    if (!m_networkWebRequestsTable)
        return;
    QTableWidgetItem *groupItem =
        m_networkWebRequestsTable->item(row, kWebRequestsColGroup);
    if (groupItem && !groupItem->text().isEmpty())
        openFullLogForText(groupItem->text());
}

void MainWindow::showEndpointRequestDetails(int row, int column)
{
    Q_UNUSED(column);
    if (!m_networkEndpointsTable)
        return;

    QTableWidgetItem *endpointItem = m_networkEndpointsTable->item(row, 0);
    if (!endpointItem ||
        endpointItem->data(kNetworkEndpointKindRole).toString() !=
            QLatin1String("http"))
        return;

    const QString method =
        endpointItem->data(kNetworkEndpointMethodRole).toString();
    const QString endpoint =
        endpointItem->data(kNetworkEndpointUrlRole).toString();
    if (method.isEmpty() || endpoint.isEmpty())
        return;

    auto *manager = requestFirewallManager(m_networkAccess);
    if (!manager)
        return;
    const QList<BackoffNetworkAccessManager::RequestRecord> requests =
        manager->endpointRequests(method, endpoint);
    if (requests.isEmpty())
        return;

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Endpoint requests"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    auto *summary = new QLabel(
        QStringLiteral("%1 request(s) for %2 %3")
            .arg(requests.size())
            .arg(method, endpoint));
    summary->setObjectName("mutedLabel");
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *table = new QTableWidget(0, 8, &dialog);
    installColumnHeaderMenu(table);
    table->setObjectName("issueTable");
    table->setHorizontalHeaderLabels(
        {QStringLiteral("When"), QStringLiteral("Method"),
         QStringLiteral("Status"), QStringLiteral("Sent"),
         QStringLiteral("Received"), QStringLiteral("Full URL"),
         QStringLiteral("Request data"), QStringLiteral("Response data")});
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    networkPrepareFullTable(table);
    table->setSortingEnabled(false);

    for (const BackoffNetworkAccessManager::RequestRecord &record :
         std::as_const(requests)) {
        const int detailRow = table->rowCount();
        table->insertRow(detailRow);

        const qint64 whenMs = qMax(record.finishedMs, record.startedMs);
        auto *when = networkDiagItem(formatRepoDate(whenMs));
        when->setData(Qt::UserRole, static_cast<qlonglong>(whenMs));
        table->setItem(detailRow, 0, when);
        table->setItem(detailRow, 1, networkDiagItem(record.method));

        auto *status = networkDiagItem(networkRequestStatus(record),
                                       record.error);
        if (record.blocked || !record.error.isEmpty())
            status->setForeground(QColor(QStringLiteral("#f85149")));
        else if (record.status >= 400)
            status->setForeground(QColor(QStringLiteral("#d29922")));
        else if (record.finishedMs <= 0)
            status->setForeground(QColor(QStringLiteral("#8b949e")));
        else
            status->setForeground(QColor(QStringLiteral("#3fb950")));
        table->setItem(detailRow, 2, status);

        auto *sent = networkDiagItem(networkDiagBytes(record.uploadBytes));
        sent->setData(Qt::UserRole, static_cast<qlonglong>(record.uploadBytes));
        table->setItem(detailRow, 3, sent);
        auto *received = networkDiagItem(networkDiagBytes(record.downloadBytes));
        received->setData(Qt::UserRole,
                          static_cast<qlonglong>(record.downloadBytes));
        table->setItem(detailRow, 4, received);
        table->setItem(detailRow, 5,
                       networkDiagItem(record.url, record.url));
        table->setItem(detailRow, 6,
                       networkDiagItem(record.requestData, record.requestData));
        table->setItem(detailRow, 7,
                       networkDiagItem(record.responseData,
                                       record.responseData));
    }

    table->setSortingEnabled(true);
    table->sortItems(0, Qt::DescendingOrder);
    table->resizeColumnsToContents();
    table->resizeRowsToContents();
    layout->addWidget(table, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.resize(1200, 720);
    dialog.exec();
}

void MainWindow::addFirewallRuleFromEdit(bool deny)
{
    if (!m_requestFirewallRuleEdit)
        return;
    const QString raw = m_requestFirewallRuleEdit->text().trimmed();
    const QString rule = BackoffNetworkAccessManager::canonicalFirewallRule(raw);
    if (rule.isEmpty()) {
        flashMessage(QStringLiteral("Enter a valid firewall rule."), true);
        return;
    }
    // Without the live manager, edit the stored lists through a scratch one so
    // a rule still lives in only one of them.
    BackoffNetworkAccessManager scratch;
    auto *manager = requestFirewallManager(m_networkAccess);
    if (!manager) {
        scratch.setFirewallRules(requestFirewallWhitelist());
        scratch.setFirewallDenyRules(requestFirewallDenylist());
        manager = &scratch;
    }
    const bool added = deny ? manager->addFirewallDenyRule(rule)
                            : manager->addFirewallRule(rule);
    QSettings settings;
    settings.setValue(kRequestFirewallWhitelistSetting, manager->firewallRules());
    settings.setValue(kRequestFirewallDenylistSetting, manager->firewallDenyRules());
    m_requestFirewallRuleEdit->clear();
    refreshFirewallTables();
    flashMessage(!added ? QStringLiteral("Firewall rule already exists.")
                 : deny ? QStringLiteral("Firewall deny rule added.")
                        : QStringLiteral("Firewall allow rule added."));
}

void MainWindow::removeSelectedFirewallRules()
{
    if (!m_requestFirewallRulesTable)
        return;
    QSet<QString> selectedAllowed;
    QSet<QString> selectedDenied;
    const auto ranges = m_requestFirewallRulesTable->selectedRanges();
    for (const QTableWidgetSelectionRange &range : ranges) {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row) {
            if (auto *item = m_requestFirewallRulesTable->item(row, 0)) {
                const QString rule = item->data(Qt::UserRole).toString();
                if (item->data(kFirewallRuleDeniedRole).toBool())
                    selectedDenied.insert(rule);
                else
                    selectedAllowed.insert(rule);
            }
        }
    }
    if (selectedAllowed.isEmpty() && selectedDenied.isEmpty())
        return;
    BackoffNetworkAccessManager scratch;
    auto *manager = requestFirewallManager(m_networkAccess);
    if (!manager) {
        scratch.setFirewallRules(requestFirewallWhitelist());
        scratch.setFirewallDenyRules(requestFirewallDenylist());
        manager = &scratch;
    }
    for (const QString &rule : std::as_const(selectedAllowed))
        manager->removeFirewallRule(rule);
    for (const QString &rule : std::as_const(selectedDenied))
        manager->removeFirewallDenyRule(rule);
    QSettings settings;
    settings.setValue(kRequestFirewallWhitelistSetting, manager->firewallRules());
    settings.setValue(kRequestFirewallDenylistSetting, manager->firewallDenyRules());
    refreshFirewallTables();
    flashMessage(QStringLiteral("Firewall rule removed."));
}

void MainWindow::editFirewallRule(const QString &rule, bool denied,
                                  const QString &method, const QUrl &url)
{
    BackoffNetworkAccessManager scratch;
    auto *manager = requestFirewallManager(m_networkAccess);
    if (!manager) {
        scratch.setFirewallRules(requestFirewallWhitelist());
        scratch.setFirewallDenyRules(requestFirewallDenylist());
        manager = &scratch;
    }
    const bool forRequest = url.isValid() && !url.host().isEmpty();
    QString current = BackoffNetworkAccessManager::canonicalFirewallRule(rule);
    bool currentDenied = denied;
    // A decision names the rule that made it then; edit the one that decides
    // the request now, which a later edit may have changed. A "Deny once"
    // answer left no rule at all.
    if (forRequest) {
        QString deciding;
        const BackoffNetworkAccessManager::FirewallMatch match =
            manager->firewallDecision(url, &deciding);
        current = deciding;
        currentDenied = match == BackoffNetworkAccessManager::FirewallDenied;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit Firewall Rule"));
    dialog.resize(720, forRequest ? 560 : 260);
    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(12);

    if (forRequest) {
        auto *request = new QLabel(
            QStringLiteral("<b>%1</b> %2")
                .arg(method.toHtmlEscaped(),
                     url.toDisplayString(QUrl::RemoveUserInfo).toHtmlEscaped()));
        request->setWordWrap(true);
        request->setTextInteractionFlags(Qt::TextSelectableByMouse);
        outer->addWidget(request);
    }
    auto *status = new QLabel(
        current.isEmpty()
            ? QStringLiteral("No rule covers this destination now; ForkMesh asks "
                             "the next time it connects.")
            : QStringLiteral("%1 %2 by %3.")
                  .arg(forRequest ? QStringLiteral("Now") : QStringLiteral("Currently"),
                       currentDenied ? QStringLiteral("denied")
                                     : QStringLiteral("allowed"),
                       BackoffNetworkAccessManager::firewallRuleLabel(current)));
    status->setObjectName("mutedLabel");
    status->setWordWrap(true);
    outer->addWidget(status);

    auto *ruleEdit = new QLineEdit;
    ruleEdit->setObjectName("firewallRuleEdit");
    ruleEdit->setPlaceholderText(kFirewallRuleSyntaxHint);
    ruleEdit->setToolTip(kFirewallRuleSyntaxHint);

    // For a request, the prompt's ladder of scopes, narrowest first.
    QTreeWidget *ruleList = nullptr;
    if (forRequest) {
        const QList<BackoffNetworkAccessManager::EndpointProfile> &catalog =
            networkEndpointCatalog();
        const int profileIndex =
            BackoffNetworkAccessManager::matchEndpointProfile(catalog, method, url);
        const QList<BackoffNetworkAccessManager::FirewallRuleSuggestion> suggestions =
            BackoffNetworkAccessManager::firewallRuleSuggestions(
                url, profileIndex >= 0 ? catalog.at(profileIndex).pattern : QString());
        ruleList = new QTreeWidget;
        ruleList->setObjectName("firewallRuleSuggestions");
        ruleList->setColumnCount(2);
        ruleList->setHeaderLabels({QStringLiteral("Scope"), QStringLiteral("Rule")});
        ruleList->setRootIsDecorated(false);
        ruleList->setUniformRowHeights(true);
        ruleList->setAllColumnsShowFocus(true);
        ruleList->header()->setStretchLastSection(true);
        ruleList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        for (const BackoffNetworkAccessManager::FirewallRuleSuggestion &suggestion :
             suggestions) {
            auto *item = new QTreeWidgetItem(
                ruleList, {suggestion.scope,
                           BackoffNetworkAccessManager::firewallRuleEditText(
                               suggestion.rule)});
            item->setData(0, Qt::UserRole, suggestion.rule);
            item->setData(1, Qt::UserRole, suggestion.explanation);
            item->setToolTip(0, suggestion.explanation);
            item->setToolTip(1, suggestion.explanation);
        }
        outer->addWidget(ruleList, 1);
        connect(ruleList, &QTreeWidget::currentItemChanged, ruleEdit,
                [ruleEdit](QTreeWidgetItem *item) {
                    if (item)
                        ruleEdit->setText(BackoffNetworkAccessManager::firewallRuleEditText(
                            item->data(0, Qt::UserRole).toString()));
                });
    }
    ruleEdit->setText(BackoffNetworkAccessManager::firewallRuleEditText(
        !current.isEmpty() ? current
        : forRequest       ? BackoffNetworkAccessManager::firewallRuleForUrl(url)
                           : rule));
    if (ruleList) {
        const QString initial =
            BackoffNetworkAccessManager::canonicalFirewallRule(ruleEdit->text());
        for (int i = 0; i < ruleList->topLevelItemCount(); ++i) {
            if (ruleList->topLevelItem(i)->data(0, Qt::UserRole).toString() ==
                initial) {
                ruleList->setCurrentItem(ruleList->topLevelItem(i));
                break;
            }
        }
    }

    auto *ruleForm = new QFormLayout;
    ruleForm->setLabelAlignment(Qt::AlignLeft);
    ruleForm->addRow(QStringLiteral("Rule"), ruleEdit);
    auto *ruleMeaning = new QLabel;
    ruleMeaning->setObjectName("mutedLabel");
    ruleMeaning->setWordWrap(true);
    ruleForm->addRow(QString(), ruleMeaning);
    outer->addLayout(ruleForm);

    // Rescoping a rule replaces it; unticked, the old rule stays beside the
    // new one.
    QCheckBox *replace = nullptr;
    if (!current.isEmpty()) {
        replace = new QCheckBox(
            QStringLiteral("Replace %1")
                .arg(BackoffNetworkAccessManager::firewallRuleEditText(current)));
        replace->setChecked(true);
        outer->addWidget(replace);
    }

    auto *buttons = new QDialogButtonBox;
    buttons->addButton(QDialogButtonBox::Cancel);
    auto *deny = buttons->addButton(QStringLiteral("Deny"),
                                    QDialogButtonBox::DestructiveRole);
    deny->setCursor(Qt::PointingHandCursor);
    setOcticon(deny, "x", 16);
    auto *allow = buttons->addButton(QStringLiteral("Allow"),
                                     QDialogButtonBox::AcceptRole);
    allow->setCursor(Qt::PointingHandCursor);
    allow->setDefault(true);
    setOcticon(allow, "check-circle", 16);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(deny, &QPushButton::clicked, &dialog,
            [&dialog] { dialog.done(kFirewallDenyDialogResult); });
    outer->addWidget(buttons);

    const auto checkRule = [ruleMeaning, ruleList, replace, allow, deny,
                            current](const QString &text) {
        const QString canonical =
            BackoffNetworkAccessManager::canonicalFirewallRule(text);
        allow->setEnabled(!canonical.isEmpty());
        deny->setEnabled(!canonical.isEmpty());
        if (replace)
            replace->setEnabled(!canonical.isEmpty() && canonical != current);
        const QTreeWidgetItem *suggested = ruleList ? ruleList->currentItem() : nullptr;
        if (suggested && suggested->data(0, Qt::UserRole).toString() != canonical)
            suggested = nullptr;
        ruleMeaning->setText(
            canonical.isEmpty()
                ? QStringLiteral("Not a valid rule")
                : QStringLiteral("Remembers: %1.%2")
                      .arg(BackoffNetworkAccessManager::firewallRuleLabel(canonical),
                           suggested ? QStringLiteral(" ") +
                                           suggested->data(1, Qt::UserRole).toString()
                                     : QString()));
    };
    connect(ruleEdit, &QLineEdit::textChanged, &dialog, checkRule);
    checkRule(ruleEdit->text());

    const int result = dialog.exec();
    if (result != QDialog::Accepted && result != kFirewallDenyDialogResult)
        return;
    const bool denyRule = result == kFirewallDenyDialogResult;
    const QString next =
        BackoffNetworkAccessManager::canonicalFirewallRule(ruleEdit->text());
    if (next.isEmpty())
        return;
    // The dialog is modal, not exclusive: a prompt may have moved the lists
    // while it was open, and removing a rule that is gone is harmless.
    if (replace && replace->isChecked() && next != current) {
        if (currentDenied)
            manager->removeFirewallDenyRule(current);
        else
            manager->removeFirewallRule(current);
    }
    // Adding to one list takes the rule out of the other, so this also flips
    // an unchanged rule between allow and deny.
    if (denyRule)
        manager->addFirewallDenyRule(next);
    else
        manager->addFirewallRule(next);
    persistFirewallRules(manager);
    refreshFirewallTables();
    refreshNetworkDiagnostics();

    // A more specific rule of the other kind still wins; say which.
    if (forRequest) {
        QString deciding;
        const BackoffNetworkAccessManager::FirewallMatch after =
            manager->firewallDecision(url, &deciding);
        const bool overruled =
            denyRule ? after == BackoffNetworkAccessManager::FirewallAllowed
                     : after == BackoffNetworkAccessManager::FirewallDenied;
        if (overruled && !deciding.isEmpty()) {
            flashMessage(QStringLiteral("Saved, but %1 is still %2 by the more "
                                        "specific rule %3. Edit or remove that "
                                        "rule too.")
                             .arg(requestFirewallDestination(url),
                                  denyRule ? QStringLiteral("allowed")
                                           : QStringLiteral("denied"),
                                  BackoffNetworkAccessManager::firewallRuleLabel(
                                      deciding)),
                         true);
            return;
        }
    }
    flashMessage(QStringLiteral("Firewall %1 %2")
                     .arg(denyRule ? QStringLiteral("denies")
                                   : QStringLiteral("allows"),
                          BackoffNetworkAccessManager::firewallRuleLabel(next)));
}

void MainWindow::editFirewallRuleForDecision(int row)
{
    if (!m_requestFirewallHistoryTable)
        return;
    const QTableWidgetItem *item = m_requestFirewallHistoryTable->item(row, 0);
    if (!item)
        return;
    editFirewallRule(item->data(Qt::UserRole).toString(),
                     item->data(kFirewallRuleDeniedRole).toBool(),
                     item->data(kFirewallHistoryMethodRole).toString(),
                     QUrl(item->data(kFirewallHistoryUrlRole).toString()));
}

void MainWindow::installFirewallRuleEditing(QPushButton *editDecisionButton)
{
    if (QTableWidget *rules = m_requestFirewallRulesTable) {
        const auto editRow = [this, rules](int row) {
            if (const QTableWidgetItem *item = rules->item(row, 0))
                editFirewallRule(item->data(Qt::UserRole).toString(),
                                 item->data(kFirewallRuleDeniedRole).toBool());
        };
        connect(rules, &QTableWidget::cellDoubleClicked, this,
                [editRow](int row, int column) {
                    if (column != kFirewallRuleColLog)
                        editRow(row);
                });
        rules->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(rules, &QWidget::customContextMenuRequested, this,
                [this, rules, editRow](const QPoint &pos) {
            const QTableWidgetItem *hit = rules->itemAt(pos);
            if (!hit)
                return;
            const int row = hit->row();
            if (!rules->selectionModel()->isRowSelected(row, QModelIndex()))
                rules->selectRow(row);
            QMenu menu(this);
            connect(menu.addAction(QStringLiteral("Edit rule...")),
                    &QAction::triggered, this, [editRow, row] { editRow(row); });
            connect(menu.addAction(QStringLiteral("Remove rule")),
                    &QAction::triggered, this,
                    &MainWindow::removeSelectedFirewallRules);
            menu.exec(rules->viewport()->mapToGlobal(pos));
        });
    }

    QTableWidget *history = m_requestFirewallHistoryTable;
    if (!history)
        return;
    connect(history, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { editFirewallRuleForDecision(row); });
    history->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(history, &QWidget::customContextMenuRequested, this,
            [this, history](const QPoint &pos) {
        const QTableWidgetItem *hit = history->itemAt(pos);
        if (!hit)
            return;
        const int row = hit->row();
        history->selectRow(row);
        const QTableWidgetItem *first = history->item(row, 0);
        const QUrl url(first ? first->data(kFirewallHistoryUrlRole).toString()
                             : QString());
        QMenu menu(this);
        connect(menu.addAction(QStringLiteral("Edit rule...")), &QAction::triggered,
                this, [this, row] { editFirewallRuleForDecision(row); });
        if (url.isValid() && !url.host().isEmpty()) {
            const QString logUrl = forkmesh::networkLogUrl(url);
            connect(menu.addAction(QStringLiteral("View in logs")),
                    &QAction::triggered, this,
                    [this, logUrl] { openNetworkLogForUrl(logUrl); });
        }
        menu.exec(history->viewport()->mapToGlobal(pos));
    });
    if (editDecisionButton) {
        editDecisionButton->setEnabled(false);
        connect(history, &QTableWidget::itemSelectionChanged, editDecisionButton,
                [history, editDecisionButton] {
                    editDecisionButton->setEnabled(
                        !history->selectionModel()->selectedRows().isEmpty());
                });
        connect(editDecisionButton, &QPushButton::clicked, this, [this, history] {
            const QModelIndexList rows = history->selectionModel()->selectedRows();
            if (!rows.isEmpty())
                editFirewallRuleForDecision(rows.first().row());
        });
    }
}

void MainWindow::clearFirewallHistory()
{
    m_requestFirewallHistory.clear();
    refreshFirewallTables();
}

BackoffNetworkAccessManager::FirewallVerdict
MainWindow::promptFirewallRequest(const QString &method, const QUrl &url,
                                  QString *ruleOut)
{
    if (!url.isValid() || url.host().isEmpty())
        return BackoffNetworkAccessManager::FirewallAllow;

    const QString defaultRule = BackoffNetworkAccessManager::firewallRuleForUrl(url);
    if (ruleOut)
        *ruleOut = defaultRule;

    if (!requestFirewallCanPrompt())
        return BackoffNetworkAccessManager::FirewallDenyOnce;

    const QString host = url.host().toLower();
    if (const std::shared_ptr<PendingFirewallPrompt> open =
            pendingFirewallPrompts().value(host)) {
        if (!open->decided && open->dialog) {
            QEventLoop wait;
            connect(open->dialog.data(), &QDialog::finished, &wait, &QEventLoop::quit);
            connect(open->dialog.data(), &QObject::destroyed, &wait, &QEventLoop::quit);
            wait.exec();
        }
        if (!open->decided)
            return BackoffNetworkAccessManager::FirewallDenyOnce;
        if (ruleOut && open->verdict != BackoffNetworkAccessManager::FirewallDenyOnce &&
            !open->rule.isEmpty())
            *ruleOut = open->rule;
        return open->verdict;
    }

    QApplication::alert(this, 0);
    if (!isActiveWindow())
        postNotification(QStringLiteral("ForkMesh firewall"),
                         QStringLiteral("%1 wants to connect to %2")
                             .arg(method, requestFirewallDestination(url)),
                         true, QStringLiteral("dialog-warning"));

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("ForkMesh Firewall"));
    dialog.setModal(true);
    dialog.resize(760, 760);

    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(18, 16, 18, 16);
    outer->setSpacing(12);

    auto *heading = new QLabel(
        QStringLiteral("<b>ForkMesh</b> is connecting to <b>%1</b> on TCP port %2")
            .arg(url.host().toHtmlEscaped())
            .arg(requestFirewallPort(url) > 0
                     ? QString::number(requestFirewallPort(url))
                     : QStringLiteral("unknown")));
    heading->setWordWrap(true);
    outer->addWidget(heading);

    auto *details = new QFormLayout;
    details->setLabelAlignment(Qt::AlignLeft);
    details->addRow(QStringLiteral("Request"),
                    new QLabel(method + QStringLiteral(" ") +
                               url.toDisplayString(QUrl::RemoveUserInfo)));
    details->addRow(QStringLiteral("Executed from"),
                    new QLabel(QCoreApplication::applicationFilePath()));
    details->addRow(QStringLiteral("Destination host"),
                    new QLabel(url.host().toLower()));
    details->addRow(QStringLiteral("Destination port"),
                    new QLabel(requestFirewallPort(url) > 0
                                   ? QString::number(requestFirewallPort(url))
                                   : QStringLiteral("unknown")));
    details->addRow(QStringLiteral("User ID"), new QLabel(requestFirewallUser()));
    details->addRow(QStringLiteral("Process ID"),
                    new QLabel(QString::number(QCoreApplication::applicationPid())));
    outer->addLayout(details);

    // What the call is for and where it is made, from the endpoint catalog.
    const QList<BackoffNetworkAccessManager::EndpointProfile> &catalog =
        networkEndpointCatalog();
    const int profileIndex =
        BackoffNetworkAccessManager::matchEndpointProfile(catalog, method, url);
    const BackoffNetworkAccessManager::EndpointProfile *profile =
        profileIndex >= 0 ? &catalog.at(profileIndex) : nullptr;
    const auto wrapped = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return label;
    };
    auto *purpose = new QFormLayout;
    purpose->setLabelAlignment(Qt::AlignLeft);
    if (profile) {
        purpose->addRow(QStringLiteral("Purpose"), wrapped(profile->description));
        if (!profile->trigger.isEmpty())
            purpose->addRow(QStringLiteral("Triggered by"), wrapped(profile->trigger));
        purpose->addRow(
            QStringLiteral("Importance"),
            wrapped(QStringLiteral("%1/10%2")
                        .arg(profile->importance)
                        .arg(profile->importance >= 9
                                 ? QStringLiteral(": ForkMesh does not work without it")
                             : profile->importance <= 3
                                 ? QStringLiteral(": low, denying it costs a minor feature")
                                 : QString())));
        purpose->addRow(
            QStringLiteral("Has to be a request"),
            wrapped(QStringLiteral("%1% likely, about %2 an hour is normal%3")
                        .arg(profile->requestNeeded)
                        .arg(qRound64(profile->modestPerHour))
                        .arg(profile->alternative.isEmpty()
                                 ? QString()
                                 : QStringLiteral(". Instead: ") + profile->alternative)));
        auto *code = new QLabel(profile->sources.isEmpty()
                                    ? QStringLiteral("Not recorded in the catalog")
                                    : endpointSourcesHtml(profile->sources));
        code->setObjectName("firewallCodeLocation");
        code->setWordWrap(true);
        code->setTextFormat(Qt::RichText);
        code->setOpenExternalLinks(true);
        code->setTextInteractionFlags(Qt::TextBrowserInteraction);
        code->setToolTip(QStringLiteral(
            "Where the request is made. Click a file to open it in this checkout."));
        purpose->addRow(QStringLiteral("Code"), code);
        purpose->addRow(QStringLiteral("Catalog pattern"),
                        wrapped(BackoffNetworkAccessManager::firewallRuleEditText(
                                    profile->pattern) +
                                (profile->method.isEmpty()
                                     ? QString()
                                     : QStringLiteral(" (%1)").arg(profile->method))));
    } else {
        auto *unknown = wrapped(QStringLiteral(
            "Not in the endpoint catalog, so nothing in ForkMesh documents this call. "
            "It may come from a package, a mirror or a host you configured; if you do "
            "not recognise %1, deny it once and see what stops working.")
                                    .arg(url.host().toLower()));
        unknown->setObjectName("mutedLabel");
        purpose->addRow(QStringLiteral("Purpose"), unknown);
        purpose->addRow(QStringLiteral("Code"),
                        wrapped(QStringLiteral("Unknown: search the source for \"%1\"")
                                    .arg(url.host().toLower())));
    }
    outer->addLayout(purpose);

    // Pick a scope from narrowest to broadest, then edit the rule if none fits.
    const QList<BackoffNetworkAccessManager::FirewallRuleSuggestion> suggestions =
        BackoffNetworkAccessManager::firewallRuleSuggestions(
            url, profile ? profile->pattern : QString());
    auto *ruleList = new QTreeWidget;
    ruleList->setObjectName("firewallRuleSuggestions");
    ruleList->setColumnCount(2);
    ruleList->setHeaderLabels({QStringLiteral("Remember for"), QStringLiteral("Rule")});
    ruleList->setRootIsDecorated(false);
    ruleList->setUniformRowHeights(true);
    ruleList->setAllColumnsShowFocus(true);
    ruleList->header()->setStretchLastSection(true);
    ruleList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    QTreeWidgetItem *defaultItem = nullptr;
    const QString hostRule = BackoffNetworkAccessManager::firewallRuleForUrl(url);
    for (const BackoffNetworkAccessManager::FirewallRuleSuggestion &suggestion :
         suggestions) {
        auto *item = new QTreeWidgetItem(
            ruleList, {suggestion.scope,
                       BackoffNetworkAccessManager::firewallRuleEditText(suggestion.rule)});
        item->setData(0, Qt::UserRole, suggestion.rule);
        item->setData(1, Qt::UserRole, suggestion.explanation);
        item->setToolTip(0, suggestion.explanation);
        item->setToolTip(1, suggestion.explanation);
        if (suggestion.rule == hostRule)
            defaultItem = item;
    }
    ruleList->setMinimumHeight(qMin(suggestions.size(), 9) * 24 + 30);
    outer->addWidget(ruleList, 1);

    auto *ruleEdit = new QLineEdit;
    ruleEdit->setObjectName("firewallRuleEdit");
    ruleEdit->setPlaceholderText(kFirewallRuleSyntaxHint);
    ruleEdit->setToolTip(kFirewallRuleSyntaxHint);
    const auto showRuleOption = [ruleEdit](QTreeWidgetItem *item) {
        if (item)
            ruleEdit->setText(BackoffNetworkAccessManager::firewallRuleEditText(
                item->data(0, Qt::UserRole).toString()));
    };
    connect(ruleList, &QTreeWidget::currentItemChanged, ruleEdit,
            [showRuleOption](QTreeWidgetItem *item) { showRuleOption(item); });
    ruleList->setCurrentItem(defaultItem ? defaultItem : ruleList->topLevelItem(0));
    showRuleOption(ruleList->currentItem());
    auto *ruleForm = new QFormLayout;
    ruleForm->setLabelAlignment(Qt::AlignLeft);
    ruleForm->addRow(QStringLiteral("Rule"), ruleEdit);
    auto *ruleMeaning = new QLabel;
    ruleMeaning->setObjectName("mutedLabel");
    ruleMeaning->setWordWrap(true);
    ruleForm->addRow(QString(), ruleMeaning);
    outer->addLayout(ruleForm);

    auto *buttons = new QDialogButtonBox;
    auto *denyOnce = buttons->addButton(QStringLiteral("Deny once"),
                                        QDialogButtonBox::RejectRole);
    denyOnce->setCursor(Qt::PointingHandCursor);
    auto *deny = buttons->addButton(QStringLiteral("Deny"),
                                    QDialogButtonBox::DestructiveRole);
    deny->setCursor(Qt::PointingHandCursor);
    setOcticon(deny, "x", 16);
    auto *allow = buttons->addButton(QStringLiteral("Allow"),
                                     QDialogButtonBox::AcceptRole);
    allow->setCursor(Qt::PointingHandCursor);
    setOcticon(allow, "check-circle", 16);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(deny, &QPushButton::clicked, &dialog,
            [&dialog] { dialog.done(kFirewallDenyDialogResult); });
    outer->addWidget(buttons);
    // Double-clicking a suggestion allows it.
    connect(ruleList, &QTreeWidget::itemDoubleClicked, &dialog,
            [&dialog, showRuleOption, allow](QTreeWidgetItem *item) {
                showRuleOption(item);
                if (allow->isEnabled())
                    dialog.accept();
            });
    const auto checkRule = [ruleMeaning, ruleList, allow, deny](const QString &text) {
        const QString canonical =
            BackoffNetworkAccessManager::canonicalFirewallRule(text);
        allow->setEnabled(!canonical.isEmpty());
        deny->setEnabled(!canonical.isEmpty());
        const QTreeWidgetItem *current = ruleList->currentItem();
        const bool suggested =
            current && current->data(0, Qt::UserRole).toString() == canonical;
        ruleMeaning->setText(
            canonical.isEmpty()
                ? QStringLiteral("Not a valid rule")
                : QStringLiteral("Remembers: %1. %2")
                      .arg(BackoffNetworkAccessManager::firewallRuleLabel(canonical),
                           suggested ? current->data(1, Qt::UserRole).toString()
                                     : QStringLiteral("A rule you wrote yourself.")));
    };
    connect(ruleEdit, &QLineEdit::textChanged, &dialog, checkRule);
    checkRule(ruleEdit->text());

    auto pending = std::make_shared<PendingFirewallPrompt>();
    pending->dialog = &dialog;
    // Record the answer as the dialog closes, before exec() unwinds, so the
    // requests waiting on this host read it on their way out.
    connect(&dialog, &QDialog::finished, &dialog,
            [pending, ruleEdit](int result) {
        pending->verdict = result == QDialog::Accepted
                               ? BackoffNetworkAccessManager::FirewallAllow
                           : result == kFirewallDenyDialogResult
                               ? BackoffNetworkAccessManager::FirewallDeny
                               : BackoffNetworkAccessManager::FirewallDenyOnce;
        pending->rule =
            BackoffNetworkAccessManager::canonicalFirewallRule(ruleEdit->text());
        pending->decided = true;
    });
    pendingFirewallPrompts().insert(host, pending);
    dialog.exec();
    if (pendingFirewallPrompts().value(host) == pending)
        pendingFirewallPrompts().remove(host);

    if (ruleOut && pending->verdict != BackoffNetworkAccessManager::FirewallDenyOnce &&
        !pending->rule.isEmpty())
        *ruleOut = pending->rule;
    return pending->verdict;
}

// Every network refusal this build makes for being out of date ends here:
// the HTTP chokepoint signals it, the WebSocket gate below calls it, and the
// window calls it once at startup. Shown once a session -- the pollers would
// otherwise raise a box a minute -- and always deferred, because a poller or a
// click is on the stack when a blocked request signals this.
void MainWindow::warnOutdatedClientVersion()
{
    auto *manager = requestFirewallManager(m_networkAccess);
    if (!manager || !manager->blockedByClientVersion())
        return;
    if (m_outdatedClientVersionWarned)
        return;
    m_outdatedClientVersionWarned = true;
    const QString summary = forkmesh::outdatedClientSummary(
        manager->clientVersion(), manager->minimumNetworkVersion());
    const QString advice = forkmesh::outdatedClientUpdateAdvice();
    logSystem(QStringLiteral("%1 - network requests and connections are "
                             "blocked. %2")
                  .arg(summary, advice));
    QTimer::singleShot(0, this, [this, summary, advice] {
        auto *box = new QMessageBox(this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setIcon(QMessageBox::Warning);
        box->setWindowTitle(QStringLiteral("Update ForkMesh"));
        box->setText(summary +
                     QStringLiteral(", so it is not sending network requests."));
        box->setInformativeText(advice);
        box->setStandardButtons(QMessageBox::Ok);
        // open(), not exec(): a modal event loop here would re-enter whatever
        // made the request that raised this.
        box->open();
    });
}

bool MainWindow::authorizeFirewallConnection(const QString &method, const QUrl &url)
{
    auto *manager = requestFirewallManager(m_networkAccess);
    // The version gate decides before the firewall does, exactly as it does
    // for HTTP: a retired build opens no connection off this machine either,
    // whatever the rules say.
    if (manager && manager->blockedByClientVersion() &&
        !BackoffNetworkAccessManager::isOnThisMachine(url)) {
        warnOutdatedClientVersion();
        return false;
    }
    if (!manager || !manager->firewallEnabled())
        return true;
    // A rule that covers the destination has already decided; a remembered
    // denial blocks without asking again. Recording the match credits the
    // rule on the Firewall Rules page, as an HTTP request would.
    const BackoffNetworkAccessManager::FirewallMatch match =
        manager->recordFirewallMatch(url);
    if (match == BackoffNetworkAccessManager::FirewallAllowed)
        return true;
    if (match == BackoffNetworkAccessManager::FirewallDenied)
        return false;

    QString rule = BackoffNetworkAccessManager::firewallRuleForUrl(url);
    const BackoffNetworkAccessManager::FirewallVerdict verdict =
        promptFirewallRequest(method, url, &rule);
    const QString canonical =
        BackoffNetworkAccessManager::canonicalFirewallRule(rule);
    if (!canonical.isEmpty() &&
        verdict != BackoffNetworkAccessManager::FirewallDenyOnce) {
        if (verdict == BackoffNetworkAccessManager::FirewallAllow)
            manager->addFirewallRule(canonical);
        else
            manager->addFirewallDenyRule(canonical);
        QSettings settings;
        settings.setValue(kRequestFirewallWhitelistSetting,
                          manager->firewallRules());
        settings.setValue(kRequestFirewallDenylistSetting,
                          manager->firewallDenyRules());
    }
    const bool allowed = verdict == BackoffNetworkAccessManager::FirewallAllow;
    recordFirewallRequest(method, url, canonical, allowed);
    return allowed;
}

void MainWindow::openFirewallRules(const QString &rule)
{
    if (m_networkFirewallRulesTabIndex < 0)
        showSection(kNetworkDiagnosticsSectionIndex);
    showNetworkTab(m_networkFirewallRulesTabIndex);
    if (m_requestFirewallRulesTable && !rule.isEmpty()) {
        for (int row = 0; row < m_requestFirewallRulesTable->rowCount(); ++row) {
            QTableWidgetItem *item =
                m_requestFirewallRulesTable->item(row, kFirewallRuleColRule);
            QTableWidgetItem *stored =
                m_requestFirewallRulesTable->item(row, kFirewallRuleColStored);
            if ((item && item->text() == rule) ||
                (stored && stored->text() == rule)) {
                m_requestFirewallRulesTable->selectRow(row);
                m_requestFirewallRulesTable->scrollToItem(item ? item : stored);
                break;
            }
        }
    }
    if (m_requestFirewallRuleEdit && !rule.isEmpty()) {
        m_requestFirewallRuleEdit->setText(rule);
        m_requestFirewallRuleEdit->setFocus();
        m_requestFirewallRuleEdit->selectAll();
    }
}

void MainWindow::recordFirewallRequest(const QString &method, const QUrl &url,
                                       const QString &rule, bool allowed)
{
    FirewallHistoryEntry entry;
    entry.method = method;
    entry.url = url.toDisplayString(QUrl::RemoveUserInfo);
    entry.destination = requestFirewallDestination(url);
    entry.rule = rule;
    entry.allowed = allowed;
    entry.timestampMs = QDateTime::currentMSecsSinceEpoch();
    m_requestFirewallHistory.prepend(entry);
    while (m_requestFirewallHistory.size() > kRequestFirewallHistoryLimit)
        m_requestFirewallHistory.removeLast();

    const QString message =
        QStringLiteral("Firewall %1 %2 %3")
            .arg(allowed ? QStringLiteral("allowed") : QStringLiteral("blocked"),
                 method, forkmesh::networkLogUrl(url));
    logSystem(message);
    if (!allowed) {
        NotificationLink link;
        link.kind = QStringLiteral("firewall");
        link.ref = rule.isEmpty() ? entry.destination : rule;
        addNotification(QStringLiteral("Firewall blocked"), message, false,
                        link, QStringLiteral("desktop"), QString());
    }
    refreshFirewallTables();
    if (m_sectionStack &&
        m_sectionStack->currentIndex() == kNetworkDiagnosticsSectionIndex)
        refreshNetworkDiagnostics();
}

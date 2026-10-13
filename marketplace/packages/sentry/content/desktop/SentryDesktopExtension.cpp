// The Sentry package's desktop contribution:
// the Log page's Sentry tab, which aggregates the connected organization's
// unresolved issues, the connection dialog that holds the auth token, and the
// lifecycle that attaches both while the package is installed and leaves no
// trace once it is removed.
//
// The package's own log lines ("Sentry: new issue ACME-7 ...") reach the Log
// page's Activity tab, its filter chips and the activity lights through the
// generic package host callback. The package supplies its badge explicitly, so
// an issue title such as "TypeError" is never classified as a host error.

#include "SentryClient.h"
#include "SentryDesktopExtension.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace sentry = forkmesh::sentry;
namespace {

QString plural(qint64 count, const char *singular, const char *pluralForm)
{
    return QStringLiteral("%1 %2")
        .arg(count)
        .arg(count == 1 ? QLatin1String(singular) : QLatin1String(pluralForm));
}

QString levelColor(const QString &level, bool dark)
{
    if (level == QLatin1String("fatal") || level == QLatin1String("error"))
        return dark ? QStringLiteral("#f85149") : QStringLiteral("#cf222e");
    if (level == QLatin1String("warning"))
        return dark ? QStringLiteral("#d29922") : QStringLiteral("#9a6700");
    if (level == QLatin1String("info"))
        return dark ? QStringLiteral("#58a6ff") : QStringLiteral("#0969da");
    return dark ? QStringLiteral("#8b949e") : QStringLiteral("#6e7781");
}

QString shareText(qint64 part, qint64 whole)
{
    if (whole <= 0)
        return QStringLiteral("0%");
    return QStringLiteral("%1%").arg(
        QString::number(100.0 * double(part) / double(whole), 'f', 1));
}

// The one line under the tab's header: what the page is showing and how old
// it is, or why it is showing nothing.
QString sentryStatusText(const SentryClient *client, qint64 nowMs)
{
    if (!client || !client->isConfigured()) {
        return QStringLiteral(
            "Not connected. Connect a Sentry organization with an API key "
            "to see its unresolved issues here and in the activity lights.");
    }
    if (client->hasError()) {
        QString error = client->lastError();
        if (error.startsWith(sentry::kLogPrefix))
            error = error.mid(sentry::kLogPrefix.size());
        return error;
    }
    if (!client->hasSynced())
        return client->status() + QString::fromUtf8("…");
    const sentry::Aggregate total = client->aggregate();
    const sentry::Connection connection = client->connection();
    QString text =
        QString::fromUtf8("Synced %1 · %2 · %3 in the %4 · %5")
            .arg(sentry::relativeTimeText(client->lastSyncMs(), nowMs),
                 plural(total.issueCount, "unresolved issue",
                        "unresolved issues"),
                 plural(total.eventCount, "event", "events"),
                 sentry::statsPeriodLabel(connection.statsPeriod),
                 plural(total.userCount, "user affected", "users affected"));
    if (!connection.project.isEmpty())
        text += QString::fromUtf8(" · project %1").arg(connection.project);
    return text;
}

// The aggregated view. Qt rich text sizes tables from width attributes, not
// CSS, and draws a CSS border around every cell, so rows are told apart by
// background instead.
QString sentryReportHtml(const SentryClient *client, qint64 nowMs, bool dark)
{
    const QString text = dark ? QStringLiteral("#adbac7") : QStringLiteral("#1f2328");
    const QString muted = dark ? QStringLiteral("#8b949e") : QStringLiteral("#6e7781");
    const QString headBg = dark ? QStringLiteral("#161b22") : QStringLiteral("#f6f8fa");
    const QString rowBg = dark ? QStringLiteral("#0d1117") : QStringLiteral("#ffffff");
    const QString altBg = dark ? QStringLiteral("#10151c") : QStringLiteral("#f6f8fa");
    const QString accent = QStringLiteral("#e1567c");
    QString html = QStringLiteral("<div style='color:%1'>").arg(text);

    if (!client || !client->isConfigured()) {
        html += QStringLiteral(
                    "<p><b>Sentry in the Log</b></p>"
                    "<p style='color:%1'>Connect a Sentry organization and this "
                    "tab aggregates its unresolved issues: events and affected "
                    "users in total, by project and by level, with every issue "
                    "linked to Sentry. New and growing issues also land in the "
                    "Activity tab as SENTRY lines and light the Sentry lane of "
                    "the activity lights.</p>"
                    "<ol>"
                    "<li>In Sentry, create an API key (auth token) with the "
                    "<b>org:read</b> and <b>project:read</b> scopes.</li>"
                    "<li>Press <b>Connect Sentry</b> above and enter the "
                    "organization slug and API key. A project slug narrows "
                    "the read to one project; a self-hosted Sentry takes its "
                    "own address.</li>"
                    "</ol>"
                    "<p style='color:%1'>The token stays in this app's "
                    "device-local settings, owned by the package: removing the "
                    "package from Market clears it along with the cached "
                    "issues. ForkMesh only ever reads from Sentry.</p>")
                    .arg(muted);
        return html + QStringLiteral("</div>");
    }

    const sentry::Connection connection = client->connection();
    const QString period = sentry::statsPeriodLabel(connection.statsPeriod);
    if (!client->hasSynced()) {
        html += QStringLiteral("<p style='color:%1'>%2</p>")
                    .arg(muted, client->hasError()
                                    ? sentryStatusText(client, nowMs).toHtmlEscaped()
                                    : QStringLiteral("Waiting for the first read "
                                                     "from %1…")
                                          .arg(connection.baseUrl.host()
                                                   .toHtmlEscaped()));
        return html + QStringLiteral("</div>");
    }

    const QList<sentry::Issue> &issues = client->issues();
    const sentry::Aggregate total = client->aggregate();
    html += QStringLiteral(
                "<p><span style='font-size:20px;font-weight:700;color:%1'>%2"
                "</span> unresolved %3 &nbsp;·&nbsp; <b>%4</b> %5 in the "
                "%6 &nbsp;·&nbsp; <b>%7</b> %8 affected &nbsp;·&nbsp; "
                "<b>%9</b> unhandled</p>")
                .arg(accent)
                .arg(total.issueCount)
                .arg(total.issueCount == 1 ? QStringLiteral("issue")
                                           : QStringLiteral("issues"))
                .arg(total.eventCount)
                .arg(total.eventCount == 1 ? QStringLiteral("event")
                                           : QStringLiteral("events"))
                .arg(period.toHtmlEscaped())
                .arg(total.userCount)
                .arg(total.userCount == 1 ? QStringLiteral("user")
                                          : QStringLiteral("users"))
                .arg(total.unhandledCount);
    if (issues.isEmpty()) {
        html += QStringLiteral("<p style='color:%1'>No unresolved issues for %2 "
                               "in the %3.</p>")
                    .arg(muted, client->organizationLabel().toHtmlEscaped(),
                         period.toHtmlEscaped());
        return html + QStringLiteral("</div>");
    }

    auto heading = [&](const QString &title) {
        return QStringLiteral(
                   "<p style='margin-top:10px'><b>%1</b></p>").arg(title);
    };
    auto tableOpen = [&](const QStringList &headers) {
        QString table = QStringLiteral(
            "<table width='100%' cellspacing='0' cellpadding='4'><tr>");
        for (const QString &header : headers) {
            table += QStringLiteral(
                         "<th align='left' bgcolor='%1' style='color:%2; "
                         "font-weight:600'>%3</th>")
                         .arg(headBg, muted, header.toHtmlEscaped());
        }
        return table + QStringLiteral("</tr>");
    };
    auto cell = [](const QString &content, const QString &align = QString()) {
        return align.isEmpty()
                   ? QStringLiteral("<td>%1</td>").arg(content)
                   : QStringLiteral("<td align='%1'>%2</td>").arg(align, content);
    };

    // By project, busiest first.
    {
        QStringList projects = total.eventsByProject.keys();
        std::stable_sort(projects.begin(), projects.end(),
                         [&](const QString &left, const QString &right) {
                             return total.eventsByProject.value(left) >
                                    total.eventsByProject.value(right);
                         });
        html += heading(QStringLiteral("By project"));
        html += tableOpen({QStringLiteral("Project"), QStringLiteral("Issues"),
                           QStringLiteral("Events"), QStringLiteral("Share")});
        int row = 0;
        for (const QString &project : std::as_const(projects)) {
            const qint64 events = total.eventsByProject.value(project);
            html += QStringLiteral("<tr bgcolor='%1'>")
                        .arg(row++ % 2 ? altBg : rowBg);
            html += cell(project.toHtmlEscaped()) +
                    cell(QString::number(total.issuesByProject.value(project)),
                         QStringLiteral("right")) +
                    cell(QString::number(events), QStringLiteral("right")) +
                    cell(shareText(events, total.eventCount),
                         QStringLiteral("right"));
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }

    // By level, most severe first.
    {
        static const QStringList order{
            QStringLiteral("fatal"), QStringLiteral("error"),
            QStringLiteral("warning"), QStringLiteral("info"),
            QStringLiteral("debug")};
        QStringList levels = total.eventsByLevel.keys();
        std::stable_sort(levels.begin(), levels.end(),
                         [&](const QString &left, const QString &right) {
                             const int l = order.indexOf(left);
                             const int r = order.indexOf(right);
                             return (l < 0 ? order.size() : l) <
                                    (r < 0 ? order.size() : r);
                         });
        html += heading(QStringLiteral("By level"));
        html += tableOpen({QStringLiteral("Level"), QStringLiteral("Issues"),
                           QStringLiteral("Events"), QStringLiteral("Share")});
        int row = 0;
        for (const QString &level : std::as_const(levels)) {
            const qint64 events = total.eventsByLevel.value(level);
            html += QStringLiteral("<tr bgcolor='%1'>")
                        .arg(row++ % 2 ? altBg : rowBg);
            html += cell(QStringLiteral("<span style='color:%1; font-weight:600'>"
                                        "%2</span>")
                             .arg(levelColor(level, dark),
                                  sentry::levelLabel(level).toHtmlEscaped())) +
                    cell(QString::number(total.issuesByLevel.value(level)),
                         QStringLiteral("right")) +
                    cell(QString::number(events), QStringLiteral("right")) +
                    cell(shareText(events, total.eventCount),
                         QStringLiteral("right"));
            html += QStringLiteral("</tr>");
        }
        html += QStringLiteral("</table>");
    }

    // Every issue, most events first (the order parseIssues() settled on).
    html += heading(QStringLiteral("Issues"));
    html += tableOpen({QStringLiteral("Issue"), QStringLiteral("Title"),
                       QStringLiteral("Project"), QStringLiteral("Level"),
                       QStringLiteral("Events"), QStringLiteral("Share"),
                       QStringLiteral("Users"), QStringLiteral("First seen"),
                       QStringLiteral("Last seen")});
    int row = 0;
    for (const sentry::Issue &issue : issues) {
        const QString label = sentry::issueLabel(issue).toHtmlEscaped();
        const QString link =
            issue.permalink.isValid()
                ? QStringLiteral("<a href='%1' style='color:%2; font-weight:600; "
                                 "text-decoration:none'>%3</a>")
                      .arg(issue.permalink.toString(QUrl::FullyEncoded)
                               .toHtmlEscaped(),
                           accent, label)
                : QStringLiteral("<b>%1</b>").arg(label);
        QString title = issue.title.toHtmlEscaped();
        if (!issue.culprit.isEmpty()) {
            title += QStringLiteral("<br><span style='color:%1'>%2</span>")
                         .arg(muted, issue.culprit.toHtmlEscaped());
        }
        QString level = QStringLiteral("<span style='color:%1; font-weight:600'>"
                                       "%2</span>")
                            .arg(levelColor(issue.level, dark),
                                 sentry::levelLabel(issue.level).toHtmlEscaped());
        if (issue.unhandled) {
            level += QStringLiteral("<br><span style='color:%1'>unhandled</span>")
                         .arg(muted);
        }
        QString events = QString::number(issue.count);
        if (issue.recentKnown && issue.recentCount != issue.count) {
            events += QStringLiteral("<br><span style='color:%1'>%2 recent</span>")
                          .arg(muted)
                          .arg(issue.recentCount);
        }
        html += QStringLiteral("<tr bgcolor='%1'>").arg(row++ % 2 ? altBg : rowBg);
        html += cell(link) + cell(title) +
                cell(issue.projectSlug.toHtmlEscaped()) + cell(level) +
                cell(events, QStringLiteral("right")) +
                cell(shareText(issue.count, total.eventCount),
                     QStringLiteral("right")) +
                cell(QString::number(issue.userCount), QStringLiteral("right")) +
                cell(sentry::relativeTimeText(issue.firstSeenMs, nowMs)
                         .toHtmlEscaped()) +
                cell(sentry::relativeTimeText(issue.lastSeenMs, nowMs)
                         .toHtmlEscaped());
        html += QStringLiteral("</tr>");
    }
    html += QStringLiteral("</table>");
    html += QStringLiteral(
                "<p style='color:%1'>Read from %2 as %3: up to %4 unresolved "
                "issues, most events first, once a minute. Users are counted "
                "per issue, the way Sentry counts them.</p>")
                .arg(muted, connection.baseUrl.host().toHtmlEscaped(),
                     client->organizationLabel().toHtmlEscaped())
                .arg(sentry::kMaxIssues);
    return html + QStringLiteral("</div>");
}

} // namespace

QString SentryDesktopExtension::packageId() const
{
    return sentry::kPackageId;
}

QList<forkmesh::market::DesktopLogCategory>
SentryDesktopExtension::logCategories() const
{
    return {{sentry::kLogBadge, QStringLiteral("#e1567c"),
             QStringLiteral(":/forkmesh-market/packages/sentry/content/desktop/icons/bug.svg")}};
}

QString SentryDesktopExtension::logPageTitle() const
{
    return QStringLiteral("Sentry");
}

QString SentryDesktopExtension::logPageToolTip() const
{
    return QStringLiteral("Unresolved issues from the connected Sentry "
                          "organization, aggregated by issue, project and level");
}

sentry::Connection SentryDesktopExtension::storedConnection() const
{
    QSettings settings;
    sentry::Connection connection;
    connection.baseUrl = sentry::normalizeBaseUrl(
        settings.value(sentry::kBaseUrlSetting).toString());
    connection.organization = sentry::normalizeSlug(
        settings.value(sentry::kOrganizationSetting).toString());
    connection.project = sentry::normalizeSlug(
        settings.value(sentry::kProjectSetting).toString());
    connection.statsPeriod = sentry::normalizeStatsPeriod(
        settings.value(sentry::kStatsPeriodSetting).toString());
    connection.token = sentry::normalizeAuthToken(
        settings.value(sentry::kAuthTokenSetting).toString());
    return connection;
}

void SentryDesktopExtension::activate(
    const forkmesh::market::DesktopPackageContext &context)
{
    if (m_active)
        return;
    m_context = context;
    m_active = true;
    startClient();
}

// Removing the package must leave no trace: the runtime, the auth token and
// connection details, and the cached issue snapshot. The Market registry
// separately drops market/packages/sentry/data, which is where the token
// lives, so even a crash between the two leaves nothing secret behind.
void SentryDesktopExtension::deactivate()
{
    if (!m_active)
        return;
    m_active = false;
    if (m_client) {
        // Replies still in flight must not repopulate the tab or log a
        // summary after the package is gone.
        disconnect(m_client, nullptr, this, nullptr);
        m_client->stop();
        delete m_client;
        m_client = nullptr;
    }
    {
        QSettings settings;
        settings.remove(sentry::kSettingsPrefix);
        settings.sync();
    }
    updateConnectButton();
    refresh();
    if (m_context.logActivity)
        m_context.logActivity(
            QStringLiteral("Sentry: package removed; its token, connection "
                           "details, and cached issues were cleared from this device."),
            sentry::kLogBadge);
    m_context = {};
}

void SentryDesktopExtension::startClient()
{
    if (!m_active || !m_context.network)
        return;
    if (!m_client) {
        m_client = new SentryClient(m_context.network, this);
        connect(m_client, &SentryClient::activity, this,
                [this](const QString &text) {
                    if (m_context.logActivity)
                        m_context.logActivity(text, sentry::kLogBadge);
                });
        connect(m_client, &SentryClient::issuesChanged, this,
                &SentryDesktopExtension::refresh);
        connect(m_client, &SentryClient::statusChanged, this,
                &SentryDesktopExtension::updateConnectButton);
    }
    const sentry::Connection connection = storedConnection();
    m_client->setConnection(connection);
    if (connection.isValid())
        m_client->start();
    updateConnectButton();
    refresh();
}

void SentryDesktopExtension::updateConnectButton(const QString &status,
                                                  bool connected, bool error)
{
    const bool configured = m_client && m_client->isConfigured();
    // A call without a status (attach, detach, a settings change) reports the
    // client's standing state rather than resetting it to "connecting".
    const bool isConnected =
        status.isEmpty() ? (m_client && m_client->isConnected())
                         : connected;
    const bool isError = status.isEmpty()
                             ? (m_client && m_client->hasError())
                             : error;
    if (m_connectButton) {
        if (!configured) {
            m_connectButton->setText(QStringLiteral("Connect Sentry"));
            m_connectButton->setToolTip(QStringLiteral(
                "Connect a Sentry organization with an API key and show "
                "its unresolved issues here and in the activity lights"));
        } else if (isConnected) {
            m_connectButton->setText(
                QString::fromUtf8("Sentry · %1")
                    .arg(m_client->organizationName()));
            m_connectButton->setToolTip(
                status.isEmpty()
                    ? QStringLiteral("Connected; reading unresolved issues once "
                                     "a minute. Click for connection settings.")
                    : status);
        } else {
            m_connectButton->setText(
                isError ? QString::fromUtf8("Sentry · fix connection")
                        : QString::fromUtf8("Sentry · connecting"));
            m_connectButton->setToolTip(
                status.isEmpty()
                    ? QStringLiteral("Open the Sentry connection settings")
                    : status);
        }
        m_connectButton->setProperty("connected", isConnected);
        m_connectButton->setProperty("bad", isError);
        m_connectButton->style()->unpolish(m_connectButton);
        m_connectButton->style()->polish(m_connectButton);
    }
    if (m_refreshButton)
        m_refreshButton->setEnabled(configured);
    if (m_browserButton)
        m_browserButton->setEnabled(configured);
    if (m_logStatus) {
        m_logStatus->setText(sentryStatusText(
            m_active ? m_client : nullptr,
            QDateTime::currentMSecsSinceEpoch()));
    }
}

QJsonObject SentryDesktopExtension::remoteSnapshot() const
{
    const SentryClient *client = m_active ? m_client : nullptr;
    if (!client || !client->isConfigured())
        return {{QStringLiteral("connected"), false}};
    QJsonArray issues;
    for (const sentry::Issue &issue : client->issues()) {
        if (issues.size() >= sentry::kMaxIssues)
            break;
        issues.append(QJsonObject{
            {QStringLiteral("id"), issue.id},
            {QStringLiteral("title"), sentry::boundedText(issue.title, sentry::kMaxTitleChars)},
            {QStringLiteral("project"),
             issue.projectName.isEmpty() ? issue.projectSlug : issue.projectName},
            {QStringLiteral("level"), issue.level},
            {QStringLiteral("count"), double(issue.count)},
            {QStringLiteral("lastSeen"), double(issue.lastSeenMs)},
        });
    }
    return {{QStringLiteral("connected"), true},
            {QStringLiteral("issues"), issues},
            {QStringLiteral("lastSyncMs"), double(client->lastSyncMs())}};
}

void SentryDesktopExtension::refresh()
{
    if (m_context.icon) {
        if (m_refreshButton)
            m_refreshButton->setIcon(m_context.icon(QStringLiteral("sync"), 14));
        if (m_browserButton)
            m_browserButton->setIcon(m_context.icon(QStringLiteral("link"), 14));
        if (m_connectButton)
            m_connectButton->setIcon(m_context.icon(
                QStringLiteral(":/forkmesh-market/packages/sentry/content/desktop/icons/bug.svg"),
                14));
    }
    const SentryClient *client = m_active ? m_client : nullptr;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_logStatus)
        m_logStatus->setText(sentryStatusText(client, nowMs));
    if (m_periodGroup) {
        const QString period = client && client->isConfigured()
                                   ? client->connection().statsPeriod
                                   : storedConnection().statsPeriod;
        const QList<QAbstractButton *> buttons =
            m_periodGroup->buttons();
        for (QAbstractButton *button : buttons) {
            button->setChecked(
                button->property("sentryPeriod").toString() == period);
        }
    }
    if (m_logView) {
        const bool dark = m_context.darkTheme && m_context.darkTheme();
        m_logView->setHtml(sentryReportHtml(client, nowMs, dark));
    }
}

void SentryDesktopExtension::setStatsPeriod(const QString &period)
{
    const QString normalized = sentry::normalizeStatsPeriod(period);
    {
        QSettings settings;
        settings.setValue(sentry::kStatsPeriodSetting, normalized);
    }
    if (m_client && m_client->isConfigured()) {
        sentry::Connection connection = m_client->connection();
        if (connection.statsPeriod != normalized) {
            connection.statsPeriod = normalized;
            // Restarts the read with the new window; the first reply logs a
            // fresh summary line, which is the point of changing it.
            m_client->setConnection(connection);
            if (!m_client->isActive())
                m_client->start();
        }
    }
    updateConnectButton();
    refresh();
}

QWidget *SentryDesktopExtension::createLogPage(QWidget *parent)
{
    if (m_logPage)
        return m_logPage;
    auto *page = new QWidget(parent);
    m_logPage = page;
    page->setObjectName(QStringLiteral("logSentryPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);
    auto *hint = new QLabel(QStringLiteral(
        "Unresolved issues from the connected Sentry organization, read once "
        "a minute. New and growing issues also land in the Activity tab as "
        "SENTRY lines and light the Sentry lane of the activity lights."));
    hint->setObjectName(QStringLiteral("logFrequencySummary"));
    hint->setWordWrap(true);
    header->addWidget(hint, 1);

    m_periodGroup = new QButtonGroup(page);
    m_periodGroup->setExclusive(true);
    const QString storedPeriod = storedConnection().statsPeriod;
    const QStringList periods = sentry::statsPeriods();
    for (const QString &period : periods) {
        auto *button = new QPushButton(period);
        button->setObjectName(QStringLiteral("logRangeButton"));
        button->setProperty("logRangeContext", QStringLiteral("sentry"));
        button->setProperty("sentryPeriod", period);
        button->setCheckable(true);
        button->setChecked(period == storedPeriod);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QStringLiteral("Count events over the %1")
                               .arg(sentry::statsPeriodLabel(period)));
        m_periodGroup->addButton(button);
        header->addWidget(button);
        connect(button, &QPushButton::clicked, this,
                [this, period] { setStatsPeriod(period); });
    }

    m_refreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_refreshButton->setObjectName(QStringLiteral("ghostButton"));
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    m_refreshButton->setToolTip(
        QStringLiteral("Read the unresolved issues again now"));
    if (m_context.icon)
        m_refreshButton->setIcon(m_context.icon(QStringLiteral("sync"), 14));
    connect(m_refreshButton, &QPushButton::clicked, this, [this] {
        if (m_client && m_client->isConfigured()) {
            m_client->refresh();
            if (m_logStatus)
                m_logStatus->setText(
                    QString::fromUtf8("Reading unresolved issues…"));
        } else {
            showConnectionDialog();
        }
    });
    header->addWidget(m_refreshButton);

    m_browserButton = new QPushButton(QStringLiteral("Open in Sentry"));
    m_browserButton->setObjectName(QStringLiteral("ghostButton"));
    m_browserButton->setCursor(Qt::PointingHandCursor);
    m_browserButton->setToolTip(
        QStringLiteral("Open this organization's issue list in your browser"));
    if (m_context.icon)
        m_browserButton->setIcon(m_context.icon(QStringLiteral("link"), 14));
    connect(m_browserButton, &QPushButton::clicked, this, [this] {
        if (!m_client || !m_client->isConfigured())
            return;
        QDesktopServices::openUrl(
            sentry::browserUrlFor(m_client->connection()));
    });
    header->addWidget(m_browserButton);

    // Styled as the chat's integration connector: the same connected/bad
    // states, the same place to click for the connection settings.
    m_connectButton = new QPushButton(QStringLiteral("Connect Sentry"));
    m_connectButton->setObjectName(QStringLiteral("chatIntegrationButton"));
    m_connectButton->setCursor(Qt::PointingHandCursor);
    if (m_context.icon)
        m_connectButton->setIcon(m_context.icon(
            QStringLiteral(":/forkmesh-market/packages/sentry/content/desktop/icons/bug.svg"),
            14));
    connect(m_connectButton, &QPushButton::clicked, this,
            &SentryDesktopExtension::showConnectionDialog);
    header->addWidget(m_connectButton);
    layout->addLayout(header);

    m_logStatus = new QLabel;
    m_logStatus->setObjectName(QStringLiteral("packageLogStatus"));
    m_logStatus->setWordWrap(true);
    m_logStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_logStatus);

    m_logView = new QTextBrowser;
    m_logView->setObjectName(QStringLiteral("packageLogView"));
    m_logView->setReadOnly(true);
    m_logView->setOpenExternalLinks(true);
    m_logView->setFrameShape(QFrame::NoFrame);
    layout->addWidget(m_logView, 1);

    updateConnectButton();
    refresh();
    return page;
}

void SentryDesktopExtension::showConnectionDialog()
{
    if (!m_active) {
        if (m_context.notify)
            m_context.notify(QStringLiteral("The package is not active."), true);
        return;
    }
    startClient();
    const sentry::Connection current = storedConnection();

    auto *dialog = new QDialog(m_context.window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("sentryConnectionDialog"));
    dialog->setWindowTitle(QStringLiteral("Connect Sentry"));
    dialog->setMinimumWidth(560);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 20, 20, 18);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("SENTRY IN THE FORKMESH LOG"));
    title->setObjectName(QStringLiteral("sectionLabel"));
    layout->addWidget(title);

    auto *explanation = new QLabel(QStringLiteral(
        "ForkMesh reads the unresolved issues of one Sentry organization "
        "once a minute. New and growing issues become SENTRY lines in the "
        "Log and light its activity lane; the Sentry tab aggregates them by "
        "issue, project and level. Nothing is ever written to Sentry."));
    explanation->setWordWrap(true);
    explanation->setObjectName(QStringLiteral("statusLine"));
    layout->addWidget(explanation);

    auto *steps = new QLabel(QStringLiteral(
        "1. Create an API key (auth token) under <b>Settings → Auth Tokens</b> "
        "in Sentry with the <b>org:read</b> and <b>project:read</b> scopes."
        "<br>2. Enter the organization slug (the name in its Sentry URL) and, "
        "to follow one project only, that project's slug.<br>"
        "3. A self-hosted Sentry takes its own address; leave it empty for "
        "sentry.io."));
    steps->setWordWrap(true);
    steps->setTextFormat(Qt::RichText);
    layout->addWidget(steps);

    auto *baseUrlEdit = new QLineEdit;
    baseUrlEdit->setObjectName(QStringLiteral("sentryBaseUrlEdit"));
    baseUrlEdit->setPlaceholderText(sentry::kDefaultBaseUrl);
    baseUrlEdit->setMaxLength(200);
    if (current.baseUrl.isValid() &&
        current.baseUrl.toString() != sentry::kDefaultBaseUrl)
        baseUrlEdit->setText(current.baseUrl.toString());
    baseUrlEdit->setToolTip(QStringLiteral(
        "The Sentry host to read from. Empty means https://sentry.io; a "
        "self-hosted Sentry needs https."));
    layout->addWidget(baseUrlEdit);

    auto *slugRow = new QHBoxLayout;
    auto *organizationEdit = new QLineEdit;
    organizationEdit->setObjectName(QStringLiteral("sentryOrganizationEdit"));
    organizationEdit->setPlaceholderText(QStringLiteral("organization slug"));
    organizationEdit->setMaxLength(64);
    organizationEdit->setText(current.organization);
    slugRow->addWidget(organizationEdit, 1);
    auto *projectEdit = new QLineEdit;
    projectEdit->setObjectName(QStringLiteral("sentryProjectEdit"));
    projectEdit->setPlaceholderText(QStringLiteral("project slug (optional)"));
    projectEdit->setMaxLength(64);
    projectEdit->setText(current.project);
    slugRow->addWidget(projectEdit, 1);
    layout->addLayout(slugRow);

    auto *tokenEdit = new QLineEdit;
    tokenEdit->setObjectName(QStringLiteral("sentryApiKeyEdit"));
    tokenEdit->setEchoMode(QLineEdit::Password);
    tokenEdit->setPlaceholderText(QStringLiteral("Sentry API key"));
    tokenEdit->setMaxLength(512);
    tokenEdit->setText(current.token);
    tokenEdit->setToolTip(QStringLiteral(
        "Stored only in this app's device-local settings, owned by the Sentry "
        "package; never placed in logs, action variables, command lines, or "
        "chat history"));
    layout->addWidget(tokenEdit);

    auto *privacy = new QLabel(QStringLiteral(
        "The API key stays in device-local app settings and is only ever sent "
        "to the Sentry host above, as a bearer token on read requests. "
        "Removing the package from Market clears it."));
    privacy->setWordWrap(true);
    privacy->setObjectName(QStringLiteral("modeHint"));
    layout->addWidget(privacy);

    auto *statusLabel = new QLabel;
    statusLabel->setObjectName(QStringLiteral("modeHint"));
    statusLabel->setWordWrap(true);
    if (m_client && m_client->isConfigured()) {
        statusLabel->setText(
            m_client->isConnected()
                ? QStringLiteral("Connected to %1.")
                      .arg(m_client->organizationLabel())
                : m_client->hasError()
                      ? sentryStatusText(m_client,
                                         QDateTime::currentMSecsSinceEpoch())
                      : QStringLiteral("Configured; connecting."));
    } else {
        statusLabel->setText(QStringLiteral("Not connected."));
    }
    layout->addWidget(statusLabel);

    auto *buttons = new QHBoxLayout;
    auto *tokens = new QPushButton(QStringLiteral("Open Sentry API keys"));
    tokens->setObjectName(QStringLiteral("ghostButton"));
    tokens->setCursor(Qt::PointingHandCursor);
    connect(tokens, &QPushButton::clicked, dialog, [baseUrlEdit] {
        sentry::Connection target;
        target.baseUrl = sentry::normalizeBaseUrl(baseUrlEdit->text());
        QDesktopServices::openUrl(sentry::authTokenPageFor(target));
    });
    buttons->addWidget(tokens);
    buttons->addStretch();

    if (m_client && m_client->isConfigured()) {
        auto *disconnectButton = new QPushButton(QStringLiteral("Disconnect"));
        disconnectButton->setObjectName(QStringLiteral("dangerButton"));
        disconnectButton->setCursor(Qt::PointingHandCursor);
        connect(disconnectButton, &QPushButton::clicked, dialog,
                [this, dialog] {
                    disconnectAccount();
                    if (!m_client || !m_client->isConfigured())
                        dialog->accept();
                });
        buttons->addWidget(disconnectButton);
    }

    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    cancel->setObjectName(QStringLiteral("ghostButton"));
    connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    buttons->addWidget(cancel);
    auto *connectButton = new QPushButton(QStringLiteral("Connect"));
    connectButton->setObjectName(QStringLiteral("primaryButton"));
    connectButton->setCursor(Qt::PointingHandCursor);
    connect(connectButton, &QPushButton::clicked, dialog,
            [this, dialog, baseUrlEdit, organizationEdit, projectEdit,
             tokenEdit, statusLabel] {
                auto refuse = [statusLabel](const QString &why) {
                    statusLabel->setText(why);
                    statusLabel->setProperty("bad", true);
                    statusLabel->style()->unpolish(statusLabel);
                    statusLabel->style()->polish(statusLabel);
                };
                sentry::Connection connection;
                connection.baseUrl = sentry::normalizeBaseUrl(baseUrlEdit->text());
                if (connection.baseUrl.isEmpty()) {
                    refuse(QStringLiteral(
                        "Enter the Sentry host as an https address without a "
                        "path, or leave it empty for sentry.io."));
                    return;
                }
                connection.organization =
                    sentry::normalizeSlug(organizationEdit->text());
                if (connection.organization.isEmpty()) {
                    refuse(QStringLiteral(
                        "Enter the organization slug: lowercase letters, "
                        "digits, dots, dashes or underscores."));
                    return;
                }
                if (!projectEdit->text().trimmed().isEmpty()) {
                    connection.project =
                        sentry::normalizeSlug(projectEdit->text());
                    if (connection.project.isEmpty()) {
                        refuse(QStringLiteral(
                            "Enter the project slug as it appears in Sentry, "
                            "or leave it empty to read the whole organization."));
                        return;
                    }
                }
                connection.token = sentry::normalizeAuthToken(tokenEdit->text());
                if (connection.token.isEmpty()) {
                    refuse(QStringLiteral(
                        "Enter a valid Sentry API key without spaces or line breaks."));
                    return;
                }
                if (!m_active || !m_client) {
                    // Removed from Market while this dialog was open: the
                    // token must not be written back after the wipe.
                    refuse(QStringLiteral(
                        "The Sentry package was removed. Reinstall it from "
                        "Market → Extensions to connect again."));
                    return;
                }
                connection.statsPeriod = storedConnection().statsPeriod;
                QSettings settings;
                settings.setValue(sentry::kBaseUrlSetting,
                                  connection.baseUrl.toString());
                settings.setValue(sentry::kOrganizationSetting,
                                  connection.organization);
                if (connection.project.isEmpty())
                    settings.remove(sentry::kProjectSetting);
                else
                    settings.setValue(sentry::kProjectSetting, connection.project);
                settings.setValue(sentry::kAuthTokenSetting, connection.token);
                settings.sync();
                m_client->setConnection(connection);
                m_client->start();
                updateConnectButton(
                    QString::fromUtf8("Sentry · connecting"));
                refresh();
                dialog->accept();
            });
    buttons->addWidget(connectButton);
    layout->addLayout(buttons);

    dialog->show();
}

void SentryDesktopExtension::disconnectAccount()
{
    const int answer = QMessageBox::question(
        m_context.window, QStringLiteral("Disconnect Sentry"),
        QStringLiteral("Disconnect Sentry and remove its API key and cached "
                       "issues from this device?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;
    {
        QSettings settings;
        settings.remove(sentry::kSettingsPrefix);
        settings.sync();
    }
    if (m_client)
        m_client->setConnection(sentry::Connection());
    updateConnectButton();
    refresh();
    if (m_context.logActivity)
        m_context.logActivity(
            QStringLiteral("Sentry: disconnected and removed the device-local "
                           "API key and cached issues."),
            sentry::kLogBadge);
}

namespace forkmesh::market {

DesktopPackageExtension *createSentryDesktopExtension()
{
    return new SentryDesktopExtension;
}

} // namespace forkmesh::market

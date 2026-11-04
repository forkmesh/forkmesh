#include "BugReportsFeature.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <utility>

namespace forkmesh::bugreports {
namespace {

const QString kReportPath = QStringLiteral("/api/bug-reports");
const QString kMinePath = QStringLiteral("/api/bug-reports/mine");
const QString kInboxPath = QStringLiteral("/api/bug-reports/inbox");

QLabel *label(const QString &value, const char *objectName, bool wrap = true)
{
    auto *widget = new QLabel(value);
    widget->setObjectName(QLatin1String(objectName));
    widget->setTextFormat(Qt::PlainText);
    widget->setWordWrap(wrap);
    widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return widget;
}

QLabel *badge(const QString &value, const QString &tone)
{
    auto *widget = new QLabel(value);
    widget->setObjectName(QStringLiteral("badge"));
    widget->setTextFormat(Qt::PlainText);
    widget->setAlignment(Qt::AlignCenter);
    widget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
    widget->setProperty("tone", tone);
    return widget;
}

QPushButton *button(const QString &text, const char *objectName, const QString &tooltip)
{
    auto *widget = new QPushButton(text);
    widget->setObjectName(QLatin1String(objectName));
    widget->setCursor(Qt::PointingHandCursor);
    widget->setToolTip(tooltip);
    return widget;
}

QScrollArea *scrollArea(const char *objectName)
{
    auto *scroll = new QScrollArea;
    scroll->setObjectName(QLatin1String(objectName));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

QString whenText(qint64 ms)
{
    if (ms <= 0)
        return {};
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::ShortFormat);
}

QVBoxLayout *freshList(QScrollArea *scroll)
{
    const int scrolled = scroll->verticalScrollBar()->value();
    auto *content = new QWidget;
    content->setObjectName(QStringLiteral("bugReportsList"));
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (QWidget *old = scroll->takeWidget())
        old->deleteLater();
    scroll->setWidget(content);
    scroll->verticalScrollBar()->setValue(scrolled);
    return layout;
}

}

BugReportsFeature::BugReportsFeature(const forkmesh::market::NativePackageHost &host,
                                     QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host)
{
    setObjectName(QStringLiteral("bugReportsSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(8);
    heading->addWidget(label(QStringLiteral("Bugs"), "pageTitle", false));
    heading->addStretch();
    m_refresh = button(QStringLiteral("Refresh"), "ghostButton",
                       QStringLiteral("Read the reports again"));
    heading->addWidget(m_refresh);
    outer->addLayout(heading);

    auto *tabs = new QFrame;
    tabs->setObjectName(QStringLiteral("segmentedControl"));
    auto *tabRow = new QHBoxLayout(tabs);
    tabRow->setContentsMargins(2, 2, 2, 2);
    tabRow->setSpacing(2);
    auto *group = new QButtonGroup(this);
    group->setExclusive(true);
    const auto addTab = [&](const QString &text, Tab tab, const char *name) {
        QPushButton *control = button(text, "segmentedButton", QString());
        control->setCheckable(true);
        control->setProperty("bugReportsTab", QLatin1String(name));
        group->addButton(control);
        tabRow->addWidget(control);
        connect(control, &QPushButton::clicked, this, [this, tab] { showTab(tab); });
        return control;
    };
    m_tabReport = addTab(QStringLiteral("Report"), Tab::Report, "report");
    m_tabMine = addTab(QStringLiteral("My reports"), Tab::Mine, "mine");
    m_tabReported = addTab(QStringLiteral("Reported"), Tab::Reported, "reported");
    m_tabReported->hide();
    m_tabReport->setChecked(true);
    auto *tabLine = new QHBoxLayout;
    tabLine->addWidget(tabs);
    tabLine->addStretch();
    outer->addLayout(tabLine);

    m_stack = new QStackedWidget;
    m_stack->setObjectName(QStringLiteral("bugReportsStack"));
    m_stack->addWidget(buildReportPane());

    auto *mine = new QWidget;
    auto *mineLayout = new QVBoxLayout(mine);
    mineLayout->setContentsMargins(0, 0, 0, 0);
    mineLayout->setSpacing(8);
    m_mineNotice = label(QString(), "statusLine");
    m_mineNotice->hide();
    mineLayout->addWidget(m_mineNotice);
    m_mineScroll = scrollArea("bugReportsMineScroll");
    mineLayout->addWidget(m_mineScroll, 1);
    m_stack->addWidget(mine);

    auto *inbox = new QWidget;
    auto *inboxLayout = new QVBoxLayout(inbox);
    inboxLayout->setContentsMargins(0, 0, 0, 0);
    inboxLayout->setSpacing(8);
    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(label(QStringLiteral("Show"), "settingsRowDetail", false));
    m_inboxFilter = new QComboBox;
    m_inboxFilter->setObjectName(QStringLiteral("bugReportsInboxFilter"));
    m_inboxFilter->addItem(QStringLiteral("All"), QString());
    for (const QString &status : inboxStatuses())
        m_inboxFilter->addItem(inboxStatusLabel(status), status);
    m_inboxFilter->setCurrentIndex(m_inboxFilter->findData(m_inboxStatus));
    filterRow->addWidget(m_inboxFilter);
    filterRow->addStretch();
    inboxLayout->addLayout(filterRow);
    m_inboxNotice = label(QString(), "statusLine");
    m_inboxNotice->hide();
    inboxLayout->addWidget(m_inboxNotice);
    m_inboxScroll = scrollArea("bugReportsInboxScroll");
    inboxLayout->addWidget(m_inboxScroll, 1);
    m_stack->addWidget(inbox);

    outer->addWidget(m_stack, 1);

    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_inboxFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        setInboxFilter(m_inboxFilter->itemData(index).toString());
    });
    renderMine();
    renderInbox();
}

QWidget *BugReportsFeature::buildReportPane()
{
    auto *pane = new QWidget;
    pane->setObjectName(QStringLiteral("bugReportsReport"));
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(label(QStringLiteral("Sent privately to ForkMesh administrators. "
                                           "Only they read it; nothing is published."),
                            "settingsRowDetail"));

    m_message = new QPlainTextEdit;
    m_message->setObjectName(QStringLiteral("bugReportsMessage"));
    m_message->setPlaceholderText(
        QStringLiteral("What happened, and what did you expect instead?"));
    m_message->setMinimumHeight(140);
    layout->addWidget(m_message, 1);

    auto *form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    m_severity = new QComboBox;
    m_severity->setObjectName(QStringLiteral("bugReportsSeverity"));
    for (const Severity &severity : severities())
        m_severity->addItem(severity.label, severity.value);
    m_severity->setCurrentIndex(m_severity->findData(QStringLiteral("normal")));
    form->addRow(QStringLiteral("How bad is it?"), m_severity);
    m_contact = new QLineEdit;
    m_contact->setObjectName(QStringLiteral("bugReportsContact"));
    m_contact->setMaxLength(kMaxContact);
    m_contact->setPlaceholderText(QStringLiteral("Optional email or handle for a reply"));
    form->addRow(QStringLiteral("Contact"), m_contact);
    layout->addLayout(form);

    auto *actions = new QHBoxLayout;
    m_reportNotice = label(QString(), "statusLine");
    actions->addWidget(m_reportNotice, 1);
    m_send = button(QStringLiteral("Send report"), "primaryButton",
                    QStringLiteral("Send this report to ForkMesh administrators"));
    actions->addWidget(m_send);
    layout->addLayout(actions);

    connect(m_send, &QPushButton::clicked, this, [this] { submit(); });
    connect(m_message, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_sending)
            m_send->setEnabled(true);
    });
    return pane;
}

void BugReportsFeature::request(const QByteArray &method, const QString &path,
                                const QJsonObject &body,
                                forkmesh::market::NativePackageReplyHandler handler)
{
    if (!m_host.request) {
        handler(false, {}, QStringLiteral("network access is unavailable"));
        return;
    }
    const QPointer<BugReportsFeature> guard(this);
    m_host.request(method, path, body,
                   [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                         const QString &error) {
        if (guard)
            handler(ok, payload, error);
    });
}

void BugReportsFeature::refresh()
{
    loadMine();
    loadInbox();
}

void BugReportsFeature::activate()
{
    if (!m_mineLoaded || !m_inboxLoaded)
        refresh();
}

void BugReportsFeature::showTab(Tab tab)
{
    if (tab == Tab::Reported && !m_admin)
        tab = Tab::Mine;
    m_tab = tab;
    m_stack->setCurrentIndex(static_cast<int>(tab));
    renderTabs();
    if (tab == Tab::Mine && !m_mineLoaded && !m_mineLoading)
        loadMine();
    if (tab == Tab::Reported && !m_inboxLoaded && !m_inboxLoading)
        loadInbox();
    if (tab == Tab::Report)
        m_message->setFocus(Qt::OtherFocusReason);
}

void BugReportsFeature::renderTabs()
{
    m_tabReported->setVisible(m_admin);
    if (!m_admin && m_tab == Tab::Reported) {
        m_tab = Tab::Mine;
        m_stack->setCurrentIndex(static_cast<int>(m_tab));
    }
    m_tabReport->setChecked(m_tab == Tab::Report);
    m_tabMine->setChecked(m_tab == Tab::Mine);
    m_tabReported->setChecked(m_tab == Tab::Reported);
    const int mineOpen = m_mine.counts.fresh + m_mine.counts.triaged;
    m_tabMine->setText(mineOpen > 0 ? QStringLiteral("My reports (%1)").arg(mineOpen)
                                    : QStringLiteral("My reports"));
    m_tabReported->setText(m_inbox.counts.fresh > 0
                               ? QStringLiteral("Reported (%1)").arg(m_inbox.counts.fresh)
                               : QStringLiteral("Reported"));
}

void BugReportsFeature::setNotice(QLabel *target, const QString &text, const QString &tone)
{
    target->setText(text);
    target->setProperty("tone", tone);
    target->setVisible(!text.isEmpty());
}

void BugReportsFeature::submit()
{
    if (m_sending)
        return;
    const QString message = m_message->toPlainText();
    if (!messageLongEnough(message)) {
        setNotice(m_reportNotice, QStringLiteral("Write a few words about what went wrong."),
                  QStringLiteral("danger"));
        m_message->setFocus(Qt::OtherFocusReason);
        return;
    }
    Draft draft;
    draft.message = message;
    draft.contact = m_contact->text();
    draft.severity = m_severity->currentData().toString();
    draft.appVersion = QCoreApplication::applicationVersion();
    m_sending = true;
    m_send->setEnabled(false);
    setNotice(m_reportNotice, QString::fromUtf8("Sending\xE2\x80\xA6"), QString());
    request(QByteArrayLiteral("POST"), kReportPath, reportPayload(draft),
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_sending = false;
        m_send->setEnabled(true);
        if (!ok) {
            setNotice(m_reportNotice, sendErrorText(error), QStringLiteral("danger"));
            return;
        }
        m_message->clear();
        m_contact->clear();
        m_severity->setCurrentIndex(m_severity->findData(QStringLiteral("normal")));
        setNotice(m_reportNotice,
                  QStringLiteral("Thanks. Your report reached the ForkMesh team."),
                  QStringLiteral("success"));
        if (m_host.logActivity)
            m_host.logActivity(QStringLiteral("Sent a bug report"));
        loadMine();
        if (m_admin)
            loadInbox();
    });
}

void BugReportsFeature::loadMine()
{
    if (m_mineLoading)
        return;
    m_mineLoading = true;
    if (!m_mineLoaded)
        setNotice(m_mineNotice, QString::fromUtf8("Loading your reports\xE2\x80\xA6"), QString());
    request(QByteArrayLiteral("GET"), kMinePath, {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_mineLoading = false;
        if (!ok) {
            m_signedOut = signedOutError(error);
            m_mineError = loadErrorText(error);
            if (m_signedOut)
                m_mine = {};
        } else {
            m_signedOut = false;
            m_mineError.clear();
            m_mine = parseReportList(payload);
            m_mineLoaded = true;
        }
        renderMine();
        renderTabs();
    });
}

void BugReportsFeature::loadInbox()
{
    if (m_inboxLoading)
        return;
    m_inboxLoading = true;
    QString path = kInboxPath;
    if (!m_inboxStatus.isEmpty())
        path += QStringLiteral("?status=") + m_inboxStatus;
    request(QByteArrayLiteral("GET"), path, {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_inboxLoading = false;
        if (!ok) {
            if (adminRequiredError(error) || signedOutError(error)) {
                m_admin = false;
                m_inbox = {};
                m_inboxError.clear();
                setBadge(0);
            } else {
                m_inboxError = loadErrorText(error);
            }
        } else {
            m_admin = true;
            m_inboxError.clear();
            m_inbox = parseReportList(payload);
            m_inboxLoaded = true;
            setBadge(m_inbox.counts.fresh);
        }
        renderInbox();
        renderTabs();
    });
}

int BugReportsFeature::navigationBadge() const
{
    return m_badge;
}

void BugReportsFeature::prepareNavigationBadge()
{
    if (!m_inboxLoaded && !m_inboxLoading)
        loadInbox();
}

void BugReportsFeature::setBadge(int count)
{
    if (count == m_badge)
        return;
    m_badge = count;
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

void BugReportsFeature::setInboxFilter(const QString &status)
{
    if (status == m_inboxStatus)
        return;
    m_inboxStatus = status;
    m_inboxLoaded = false;
    loadInbox();
}

void BugReportsFeature::addState(QVBoxLayout *layout, const QString &title, const QString &text)
{
    auto *empty = new QWidget;
    empty->setObjectName(QStringLiteral("emptyState"));
    auto *column = new QVBoxLayout(empty);
    column->setContentsMargins(24, 48, 24, 24);
    column->setSpacing(6);
    QLabel *heading = label(title, "emptyStateTitle");
    heading->setAlignment(Qt::AlignHCenter);
    column->addWidget(heading);
    QLabel *detail = label(text, "emptyStateBody");
    detail->setAlignment(Qt::AlignHCenter);
    column->addWidget(detail);
    layout->addWidget(empty);
    layout->addStretch();
}

void BugReportsFeature::renderMine()
{
    setNotice(m_mineNotice, m_mineLoading && !m_mineLoaded ? m_mineNotice->text() : m_mineError,
              m_mineError.isEmpty() ? QString() : QStringLiteral("danger"));
    QVBoxLayout *layout = freshList(m_mineScroll);
    if (m_signedOut) {
        addState(layout, QStringLiteral("Sign in to follow your reports"),
                 QStringLiteral("Reports you send while signed in show here with their status."));
        return;
    }
    if (m_mine.reports.isEmpty()) {
        if (m_mineLoaded)
            addState(layout, QStringLiteral("No reports yet"),
                     QStringLiteral("Bugs you report from any device show here, with what "
                                    "the team did about them."));
        else
            layout->addStretch();
        return;
    }
    for (const BugReport &report : std::as_const(m_mine.reports))
        layout->addWidget(reportCard(report, false));
    if (m_mine.more)
        layout->addWidget(label(QStringLiteral("Older reports are not shown."),
                                "settingsRowDetail"));
    layout->addStretch();
}

void BugReportsFeature::renderInbox()
{
    setNotice(m_inboxNotice, m_inboxError,
              m_inboxError.isEmpty() ? QString() : QStringLiteral("danger"));
    QVBoxLayout *layout = freshList(m_inboxScroll);
    if (m_inbox.reports.isEmpty()) {
        if (m_inboxLoaded)
            addState(layout, QStringLiteral("Nothing here"),
                     m_inboxStatus == QLatin1String("new")
                         ? QStringLiteral("Every reported bug has been looked at.")
                         : QStringLiteral("No reports match this filter."));
        else
            layout->addStretch();
        return;
    }
    for (const BugReport &report : std::as_const(m_inbox.reports))
        layout->addWidget(reportCard(report, true));
    if (m_inbox.more)
        layout->addWidget(label(QStringLiteral("Older reports are on the admin console."),
                                "settingsRowDetail"));
    layout->addStretch();
}

QWidget *BugReportsFeature::reportCard(const BugReport &report, bool inbox)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("settingsRow"));
    frame->setProperty("bugReport", report.id);
    frame->setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(0, 10, 0, 10);
    layout->setSpacing(4);

    const bool busy = m_busyReport == report.id;
    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    const QString status = inbox ? inboxStatusLabel(report.status) : mineStatusLabel(report);
    QLabel *state = badge(status, report.confirmedFixedAt > 0 ? QStringLiteral("success")
                                                              : statusTone(report.status));
    state->setProperty("bugReportStatus", report.status);
    top->addWidget(state);
    if (report.severity == QLatin1String("crash") || report.severity == QLatin1String("high"))
        top->addWidget(badge(report.severity == QLatin1String("crash") ? QStringLiteral("Crash")
                                                                       : QStringLiteral("Serious"),
                             QStringLiteral("danger")));
    if (inbox && !report.account.isEmpty())
        top->addWidget(label(report.account, "settingsRowTitle", false));
    top->addStretch();
    top->addWidget(label(whenText(report.createdAt), "settingsRowDetail", false));
    layout->addLayout(top);

    QLabel *message = label(report.message, "settingsRowTitle");
    message->setProperty("bugReportMessage", true);
    layout->addWidget(message);
    QString details = detailsLine(report);
    if (inbox && !report.package.isEmpty())
        details += (details.isEmpty() ? QString() : QStringLiteral(" · ")) +
                   QStringLiteral("plugin ") + report.package;
    if (inbox && !report.contact.isEmpty())
        details += (details.isEmpty() ? QString() : QStringLiteral(" · ")) +
                   QStringLiteral("contact ") + report.contact;
    if (!details.isEmpty())
        layout->addWidget(label(details, "settingsRowDetail"));

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addStretch();
    if (inbox) {
        for (const QString &next : inboxStatuses()) {
            if (next == report.status)
                continue;
            QPushButton *control = button(
                next == QLatin1String("new")       ? QStringLiteral("Mark new")
                : next == QLatin1String("triaged") ? QStringLiteral("Mark triaged")
                                                   : QStringLiteral("Close"),
                "ghostButton", QStringLiteral("Set this report to %1").arg(inboxStatusLabel(next)));
            control->setProperty("bugReportAction", next);
            control->setEnabled(!busy);
            const QString id = report.id;
            connect(control, &QPushButton::clicked, this,
                    [this, id, next] { setInboxStatus(id, next); });
            actions->addWidget(control);
        }
    } else if (canConfirmFixed(report)) {
        QPushButton *control = button(QStringLiteral("Confirm it's fixed"), "ghostButton",
                                      QStringLiteral("Tell the team this bug is gone for you"));
        control->setProperty("bugReportAction", QStringLiteral("fixed"));
        control->setEnabled(!busy);
        const QString id = report.id;
        connect(control, &QPushButton::clicked, this, [this, id] { confirmFixed(id); });
        actions->addWidget(control);
    }
    if (actions->count() > 1)
        layout->addLayout(actions);
    else
        delete actions;
    return frame;
}

void BugReportsFeature::confirmFixed(const QString &id)
{
    if (!m_busyReport.isEmpty())
        return;
    m_busyReport = id;
    renderMine();
    request(QByteArrayLiteral("POST"), kMinePath, confirmFixedPayload(id),
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        m_busyReport.clear();
        if (!ok) {
            m_mineError = QStringLiteral("Could not confirm the fix: %1").arg(error);
        } else {
            m_mineError.clear();
            const BugReport updated = parseReport(payload.value(QStringLiteral("report")).toObject());
            for (BugReport &report : m_mine.reports) {
                if (report.id != id)
                    continue;
                if (updated.id == id) {
                    report = updated;
                } else {
                    report.status = QStringLiteral("closed");
                    report.confirmedFixedAt = qMax<qint64>(report.confirmedFixedAt, 1);
                }
            }
            if (payload.contains(QStringLiteral("counts")))
                m_mine.counts = parseCounts(payload.value(QStringLiteral("counts")).toObject());
        }
        renderMine();
        renderTabs();
    });
}

void BugReportsFeature::setInboxStatus(const QString &id, const QString &status)
{
    if (!m_busyReport.isEmpty())
        return;
    m_busyReport = id;
    renderInbox();
    request(QByteArrayLiteral("POST"), kInboxPath, statusPayload(id, status),
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_busyReport.clear();
        if (!ok) {
            m_inboxError = QStringLiteral("Could not change the report: %1").arg(error);
            renderInbox();
            return;
        }
        m_inboxLoaded = false;
        loadInbox();
    });
}

QList<forkmesh::market::NativePackageSearchHit>
BugReportsFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return hits;
    const auto scan = [&](const ReportList &list, const QString &prefix, bool inbox) {
        for (const BugReport &report : list.reports) {
            if (hits.size() >= limit)
                return;
            if (!report.message.contains(needle, Qt::CaseInsensitive))
                continue;
            hits.append({prefix + report.id, report.message.section(QLatin1Char('\n'), 0, 0).left(120),
                         inbox ? inboxStatusLabel(report.status) : mineStatusLabel(report)});
        }
    };
    scan(m_mine, QStringLiteral("mine:"), false);
    if (m_admin)
        scan(m_inbox, QStringLiteral("inbox:"), true);
    return hits;
}

void BugReportsFeature::activateSearchHit(const QString &id)
{
    showTab(id.startsWith(QLatin1String("inbox:")) ? Tab::Reported : Tab::Mine);
}

void BugReportsFeature::prepareSearch()
{
    if (!m_mineLoaded)
        loadMine();
}

namespace {

const forkmesh::market::NativePackageRegistration bugReportsRegistration({
    QStringLiteral("builtin:bug-reports"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Bugs"),
    QStringLiteral("bug"),
    QStringLiteral("/api/bug-reports"),
    QStringLiteral("forkmesh-bug-reports-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new BugReportsFeature(host, parent);
    },
});

}

}

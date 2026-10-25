
#include "Tooltips.h"
#include "MainWindow.h"
#include "AgentActivityFeed.h"
#include "AgentResourceGovernor.h"
#include "AgentResources.h"
#include "MainWindowInternal.h"
#include "SystemPressure.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "AgentWorktree.h"
#include "ActionStore.h"
#include "AppPaths.h"
#include "ControlNode.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "D1QuerySource.h"
#include "DeployLogs.h"
#include "DatabaseManagerWidget.h"
#include "DesktopProcessView.h"
#include "FleetVisualizer.h"
#include "IpCountry.h"
#include "IpWhois.h"
#include "KebabHeaderView.h"
#include "QueryInspector.h"
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

#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTimeEdit>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFormLayout>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTabWidget>
#include <QUrlQuery>
#include <QtConcurrentRun>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <signal.h>
#endif

using namespace forkmesh::ui;

namespace {
QString cloudLogMonitorMissingTokenReason()
{
    return QStringLiteral(
        "waiting for a Cloudflare API token (Settings > Secrets)");
}

QString cloudLogMonitorOldNodeReason()
{
    return QStringLiteral("Node.js %1+ is required to run Wrangler")
        .arg(forkmesh::control::kWranglerMinimumNodeMajor);
}

QString cloudLogMonitorMissingWorkerReason(const QString &worker)
{
    return QStringLiteral(
               "the Worker %1 is not deployed on this Cloudflare account "
               "(turn the setting off and on again after deploying it)")
        .arg(worker);
}

qint64 fastStoredLogTimestampMs(const QString &line, QString *cachedDate,
                                qint64 *cachedMidnightMs)
{
    if (line.size() < 19 || line.at(4) != QLatin1Char('-') ||
        line.at(7) != QLatin1Char('-') || line.at(10) != QLatin1Char(' ') ||
        line.at(13) != QLatin1Char(':') || line.at(16) != QLatin1Char(':'))
        return -1;
    auto digits = [&line](int offset, int length) {
        int value = 0;
        for (int i = 0; i < length; ++i) {
            const ushort digit = line.at(offset + i).unicode();
            if (digit < '0' || digit > '9')
                return -1;
            value = value * 10 + int(digit - '0');
        }
        return value;
    };
    const QString dateText = line.left(10);
    if (*cachedDate != dateText) {
        const QDate date(digits(0, 4), digits(5, 2), digits(8, 2));
        if (!date.isValid())
            return -1;
        *cachedDate = dateText;
        *cachedMidnightMs = QDateTime(date, QTime(0, 0)).toMSecsSinceEpoch();
    }
    const int hour = digits(11, 2);
    const int minute = digits(14, 2);
    const int second = digits(17, 2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59)
        return -1;
    return *cachedMidnightMs + qint64(hour * 3600 + minute * 60 + second) * 1000;
}

QString completeJsonValueInText(const QString &text)
{
    for (int start = 0; start < text.size(); ++start) {
        const QChar opening = text.at(start);
        if (opening != QLatin1Char('{') && opening != QLatin1Char('['))
            continue;

        QVector<QChar> closing;
        closing.append(opening == QLatin1Char('{') ? QLatin1Char('}')
                                                   : QLatin1Char(']'));
        bool inString = false;
        bool escaped = false;
        for (int end = start + 1; end < text.size(); ++end) {
            const QChar character = text.at(end);
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (character == QLatin1Char('\\')) {
                    escaped = true;
                } else if (character == QLatin1Char('"')) {
                    inString = false;
                }
                continue;
            }
            if (character == QLatin1Char('"')) {
                inString = true;
                continue;
            }
            if (character == QLatin1Char('{')) {
                closing.append(QLatin1Char('}'));
                continue;
            }
            if (character == QLatin1Char('[')) {
                closing.append(QLatin1Char(']'));
                continue;
            }
            if (character != QLatin1Char('}') && character != QLatin1Char(']'))
                continue;
            if (closing.isEmpty() || closing.last() != character)
                break;
            closing.removeLast();
            if (!closing.isEmpty())
                continue;

            const QString candidate = text.mid(start, end - start + 1);
            QJsonParseError error;
            const QJsonDocument document =
                QJsonDocument::fromJson(candidate.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError &&
                (document.isObject() || document.isArray()))
                return candidate;
            break;
        }
    }
    return QString();
}

class LogFrequencyBarChart final : public QWidget
{
public:
    struct Entry {
        QString message;
        QString category;
        int count = 0;
        double share = 0.0;
    };

    explicit LogFrequencyBarChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("logFrequencyChart"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        setAccessibleName(QStringLiteral("Most frequent log entries"));
        setFixedHeight(kRowHeight + 2 * kPadding);
    }

    // Each row carries its own "send to prompt" button, so the chart needs a
    // way back to the prompt box the window owns.
    void setPromptHandler(std::function<void(const Entry &)> handler)
    {
        m_promptHandler = std::move(handler);
    }

    void setEntries(QVector<Entry> entries)
    {
        m_entries = std::move(entries);
        m_hovered = -1;
        setFixedHeight(qMax(1, m_entries.size()) * kRowHeight + 2 * kPadding);
        syncPromptButtons();
        update();
    }

    QString messageAt(int row) const
    {
        return row >= 0 && row < m_entries.size()
                   ? m_entries.at(row).message
                   : QString();
    }

    int countAt(int row) const
    {
        return row >= 0 && row < m_entries.size() ? m_entries.at(row).count : 0;
    }

    QPushButton *promptButtonAt(int row) const
    {
        return row >= 0 && row < m_entries.size() &&
                       row < m_promptButtons.size()
                   ? m_promptButtons.at(row)
                   : nullptr;
    }

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
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("No detailed log entries in this timeframe."));
            return;
        }

        QFont valueFont = font();
        valueFont.setPointSizeF(qMax(7.0, valueFont.pointSizeF() - 1.0));
        const QFontMetrics labelMetrics(font());
        const QFontMetrics valueMetrics(valueFont);
        int maxCount = 1;
        int valueWidth = 0;
        for (const Entry &entry : std::as_const(m_entries)) {
            maxCount = qMax(maxCount, entry.count);
            valueWidth = qMax(valueWidth,
                              valueMetrics.horizontalAdvance(
                                  QStringLiteral("%1 · %2%")
                                      .arg(entry.count)
                                      .arg(entry.share, 0, 'f', 1)));
        }
        const int labelLeft = promptColumnWidth();
        const int labelWidth = qBound(120, width() / 3, 360);
        const qreal barLeft = labelLeft + labelWidth + 12.0;
        const qreal barSpan = qMax(1.0, width() - barLeft - valueWidth - 14.0);

        for (int row = 0; row < m_entries.size(); ++row) {
            const Entry &entry = m_entries.at(row);
            const qreal top = kPadding + row * kRowHeight;
            if (row == m_hovered) {
                QColor highlight = ink;
                highlight.setAlpha(14);
                painter.fillRect(QRectF(0, top, width(), kRowHeight), highlight);
            }
            painter.setFont(font());
            painter.setPen(ink);
            painter.drawText(
                QRectF(labelLeft, top, labelWidth, kRowHeight),
                Qt::AlignRight | Qt::AlignVCenter,
                labelMetrics.elidedText(entry.message, Qt::ElideMiddle,
                                        labelWidth));

            const qreal barWidth = qMax(
                2.0, barSpan * (qreal(entry.count) / qreal(maxCount)));
            const QRectF bar(barLeft, top + (kRowHeight - kBarHeight) / 2.0,
                             barWidth, kBarHeight);
            QPainterPath path;
            path.addRoundedRect(bar, 4.0, 4.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(LogActivityLights::accentForBadge(
                entry.category)));
            painter.drawPath(path);

            painter.setFont(valueFont);
            QColor valueInk = ink;
            valueInk.setAlpha(190);
            painter.setPen(valueInk);
            painter.drawText(
                QRectF(bar.right() + 6.0, top,
                       qMax(0.0, width() - bar.right() - 6.0), kRowHeight),
                Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("%1 · %2%")
                    .arg(entry.count)
                    .arg(entry.share, 0, 'f', 1));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int row = rowAt(event->pos());
        if (row != m_hovered) {
            m_hovered = row;
            update();
        }
        if (row >= 0) {
            const Entry &entry = m_entries.at(row);
            QToolTip::showText(
                event->globalPosition().toPoint(),
                QStringLiteral("%1\n%2 occurrence%3 · %4% of detailed logs\n%5")
                    .arg(entry.category)
                    .arg(entry.count)
                    .arg(entry.count == 1 ? QString() : QStringLiteral("s"))
                    .arg(entry.share, 0, 'f', 1)
                    .arg(entry.message),
                this);
        } else {
            QToolTip::hideText();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = -1;
        QToolTip::hideText();
        update();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        layoutPromptButtons();
    }

private:
    int rowAt(const QPoint &pos) const
    {
        if (pos.y() < kPadding)
            return -1;
        const int row = (pos.y() - kPadding) / kRowHeight;
        return row >= 0 && row < m_entries.size() ? row : -1;
    }

    static int promptColumnWidth() { return kButtonSize + 6; }

    void syncPromptButtons()
    {
        while (m_promptButtons.size() < m_entries.size()) {
            auto *button = new QPushButton(this);
            button->setObjectName(
                QStringLiteral("logFrequencyPromptButton"));
            button->setCursor(Qt::PointingHandCursor);
            button->setFlat(true);
            button->setFixedSize(kButtonSize, kButtonSize);
            setOcticon(button, QStringLiteral("paper-airplane"), 12);
            const int row = m_promptButtons.size();
            connect(button, &QPushButton::clicked, button, [this, row] {
                if (m_promptHandler && row >= 0 && row < m_entries.size())
                    m_promptHandler(m_entries.at(row));
            });
            m_promptButtons.append(button);
        }
        for (int row = 0; row < m_promptButtons.size(); ++row) {
            QPushButton *button = m_promptButtons.at(row);
            const bool used = row < m_entries.size();
            button->setVisible(used);
            if (!used)
                continue;
            const QString label = m_entries.at(row).message;
            button->setToolTip(
                QStringLiteral("Send this log message to the prompt"));
            button->setAccessibleName(
                QStringLiteral("Send log message to prompt"));
            button->setAccessibleDescription(label);
        }
        layoutPromptButtons();
    }

    void layoutPromptButtons()
    {
        for (int row = 0; row < m_promptButtons.size(); ++row) {
            if (row >= m_entries.size())
                continue;
            m_promptButtons.at(row)->move(
                0, kPadding + row * kRowHeight + (kRowHeight - kButtonSize) / 2);
        }
    }

    static constexpr int kRowHeight = 26;
    static constexpr int kBarHeight = 12;
    static constexpr int kPadding = 4;
    static constexpr int kButtonSize = 20;
    QVector<Entry> m_entries;
    QVector<QPushButton *> m_promptButtons;
    std::function<void(const Entry &)> m_promptHandler;
    int m_hovered = -1;
};
} // namespace

QWidget *MainWindow::buildLogSection()
{
    auto *page = new QWidget;

    auto *label = new QLabel(QStringLiteral("LOG ACTIVITY"));
    label->setObjectName("sectionLabel");
    auto *clearButton = new QPushButton("Clear");
    clearButton->setObjectName("ghostButton");
    clearButton->setCursor(Qt::PointingHandCursor);
    clearButton->setToolTip("Clear all saved logs");
    setOcticon(clearButton, "trash", 14);
    m_logSearchEdit = new QLineEdit;
    m_logSearchEdit->setObjectName(QStringLiteral("logSearchEdit"));
    m_logSearchEdit->setClearButtonEnabled(true);
    m_logSearchEdit->setPlaceholderText(QStringLiteral("Filter log text"));
    m_logSearchEdit->setToolTip(QStringLiteral(
        "Show only lines containing this text. * and ? are wildcards, so a "
        "route group from Network > Web Requests (accounts/*) filters as it "
        "reads. Combines with the category chips above."));
    m_logSearchEdit->setMaximumWidth(260);
    m_logSearchEdit->setText(m_logTextFilter); // a filter set before this page existed
    // Applying a filter re-renders the whole retained log, and a 1.1 s stall was
    // sampled inside rebuildNetworkLogView() from a keystroke. Typing therefore
    // arms a short timer instead of rebuilding per character; textEdited fires
    // for typing only, so a programmatic setLogTextFilter() still applies at once.
    m_logSearchDebounce = new QTimer(this);
    m_logSearchDebounce->setSingleShot(true);
    m_logSearchDebounce->setInterval(120);
    connect(m_logSearchDebounce, &QTimer::timeout, this, [this] {
        if (m_logSearchEdit)
            setLogTextFilter(m_logSearchEdit->text());
    });
    connect(m_logSearchEdit, &QLineEdit::textEdited, this,
            [this] { m_logSearchDebounce->start(); });

    auto *popoutButton = new QPushButton("Pop out");
    popoutButton->setObjectName(QStringLiteral("logPopoutButton"));
    popoutButton->setCursor(Qt::PointingHandCursor);
    popoutButton->setToolTip(
        QStringLiteral("Open the complete log — every category, every retained "
                       "line — in its own window"));
    setOcticon(popoutButton, "screen-full", 14);
    connect(popoutButton, &QPushButton::clicked, this,
            &MainWindow::showNetworkLogPopout);

    auto *formatJsonButton = new QPushButton("Format JSON");
    formatJsonButton->setObjectName(QStringLiteral("logFormatJsonButton"));
    formatJsonButton->setCursor(Qt::PointingHandCursor);
    formatJsonButton->setToolTip(
        QStringLiteral("Format selected JSON, or detect JSON in the current log "
                       "entry, as an indented, copyable preview"));
    setOcticon(formatJsonButton, "code", 14);
    connect(formatJsonButton, &QPushButton::clicked, this,
            &MainWindow::showFormattedLogJson);

    auto *logTypesButton = new QPushButton(QStringLiteral("Log types"));
    logTypesButton->setObjectName(QStringLiteral("logTypesButton"));
    logTypesButton->setCursor(Qt::PointingHandCursor);
    logTypesButton->setToolTip(QStringLiteral(
        "Turn a log category off so its lines stop being recorded, or delete "
        "every line one category has already recorded"));
    setOcticon(logTypesButton, "gear", 14);
    connect(logTypesButton, &QPushButton::clicked, this,
            &MainWindow::showLogTypesDialog);

    m_loggingOffButton = new QPushButton;
    m_loggingOffButton->setObjectName(QStringLiteral("logAllOffButton"));
    m_loggingOffButton->setCheckable(true);
    m_loggingOffButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_loggingOffButton, "circle-slash", 14);
    connect(m_loggingOffButton, &QPushButton::toggled, this,
            [this](bool off) { setLoggingEnabled(!off); });
    updateLoggingOffButton();

    auto *noWrapButton = new QPushButton(QStringLiteral("No wrap"));
    noWrapButton->setObjectName(QStringLiteral("logNoWrapButton"));
    noWrapButton->setCheckable(true);
    noWrapButton->setCursor(Qt::PointingHandCursor);
    noWrapButton->setToolTip(QStringLiteral(
        "Keep each log entry on one line and scroll horizontally to see the rest"));

    m_settingsLog = new QTextBrowser(page);
    m_settingsLog->setReadOnly(true);
    m_settingsLog->setObjectName("networkLog");
    m_settingsLog->setOpenExternalLinks(true);
    m_settingsLog->setLineWrapMode(QTextEdit::WidgetWidth);
    m_settingsLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_settingsLog->viewport()->installEventFilter(this);
    connect(m_settingsLog->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &MainWindow::onNetworkLogScrolled);
    // Relayouts grow the document after the insert too (wrap changes, a
    // favicon arriving, lazy layout); a view that follows the tail stays on it.
    connect(m_settingsLog->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int maximum) {
                if (!m_logFollowTail || !m_settingsLog)
                    return;
                const QScopedValueRollback<bool> guard(m_logScrollingToTail,
                                                       true);
                m_settingsLog->verticalScrollBar()->setValue(maximum);
            });

    m_logFollowButton = new QPushButton(QStringLiteral("Live"));
    m_logFollowButton->setObjectName(QStringLiteral("logFollowButton"));
    m_logFollowButton->setCheckable(true);
    m_logFollowButton->setChecked(m_logFollowTail);
    m_logFollowButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_logFollowButton, "broadcast", 14);
    connect(m_logFollowButton, &QPushButton::toggled, this,
            &MainWindow::setNetworkLogFollowing);
    updateNetworkLogFollowButton();
    connect(noWrapButton, &QPushButton::toggled, m_settingsLog,
            [view = m_settingsLog](bool noWrap) {
                view->setLineWrapMode(noWrap ? QTextEdit::NoWrap
                                             : QTextEdit::WidgetWidth);
            });

    m_logTimelineChart = new LogTimelineChart(page);
    m_logTimelineChart->setFixedHeight(68);
    m_logTimelineSummary = new QLabel;
    m_logTimelineSummary->setObjectName(QStringLiteral("logTimelineSummary"));
    m_logTimelineSummary->setAccessibleName(QStringLiteral("Visible log summary"));
    m_logTimelineChart->viewChanged = [this] {
        // Every logged line lands here (coalesced by the chart). Off-screen,
        // the visible-count walk and the frequency re-rank are wasted work:
        // remember that the page fell behind and catch up on showSection(4).
        if (!m_logTimelineChart->isVisible()) {
            m_logTimelineViewStale = true;
            return;
        }
        updateLogTimelineSummary();
        if (m_logViewTabs &&
            m_logViewTabs->currentIndex() == m_logAggregationTabIndex &&
            m_logFrequencyRefreshTimer)
            m_logFrequencyRefreshTimer->start();
    };

    auto makeRangeRow = [this, page](QButtonGroup **groupOut,
                                     const QString &context) {
        auto *group = new QButtonGroup(page);
        group->setExclusive(true);
        *groupOut = group;
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        auto addPreset = [this, row, group,
                          context](const QString &text, int minutes) {
            auto *button = new QPushButton(text);
            button->setObjectName(QStringLiteral("logRangeButton"));
            button->setProperty("logRangeContext", context);
            button->setCheckable(true);
            button->setCursor(Qt::PointingHandCursor);
            button->setToolTip(QStringLiteral("Show logs from the past %1")
                                   .arg(text));
            group->addButton(button, minutes);
            row->addWidget(button);
            connect(button, &QPushButton::clicked, this,
                    [this, minutes] { setLogTimelinePresetMinutes(minutes); });
            return button;
        };
        addPreset(QStringLiteral("30 min"), 30);
        addPreset(QStringLiteral("24h"), 24 * 60)->setChecked(true);
        addPreset(QStringLiteral("7 days"), 7 * 24 * 60);
        addPreset(QStringLiteral("30 days"), 30 * 24 * 60);
        auto *customButton = new QPushButton(QStringLiteral("Custom..."));
        customButton->setObjectName(QStringLiteral("logRangeButton"));
        customButton->setProperty("logRangeContext", context);
        customButton->setCheckable(true);
        customButton->setCursor(Qt::PointingHandCursor);
        customButton->setToolTip(
            QStringLiteral("Choose exact start and end times"));
        group->addButton(customButton, 0);
        row->addWidget(customButton);
        connect(customButton, &QPushButton::clicked, this,
                &MainWindow::chooseCustomLogTimelineRange);
        row->addStretch();
        return row;
    };

    auto *rangeRow = makeRangeRow(&m_logTimelineRangeGroup,
                                  QStringLiteral("activity"));
    m_logTimelineResetZoom = new QPushButton(QStringLiteral("Reset zoom"));
    m_logTimelineResetZoom->setObjectName(QStringLiteral("ghostButton"));
    m_logTimelineResetZoom->setCursor(Qt::PointingHandCursor);
    m_logTimelineResetZoom->setEnabled(false);
    setOcticon(m_logTimelineResetZoom, "screen-full", 14);
    connect(m_logTimelineResetZoom, &QPushButton::clicked,
            m_logTimelineChart, &LogTimelineChart::resetZoom);
    rangeRow->addWidget(m_logTimelineResetZoom);

    auto *activityPage = new QWidget;
    auto *activityLayout = new QVBoxLayout(activityPage);
    activityLayout->setContentsMargins(8, 8, 8, 8);
    activityLayout->setSpacing(8);
    activityLayout->addLayout(rangeRow);
    activityLayout->addWidget(m_logTimelineChart);
    activityLayout->addWidget(m_settingsLog, 1);

    auto *frequencyPage = new QWidget;
    auto *frequencyLayout = new QVBoxLayout(frequencyPage);
    frequencyLayout->setContentsMargins(8, 8, 8, 8);
    frequencyLayout->setSpacing(8);
    frequencyLayout->addLayout(makeRangeRow(&m_logFrequencyRangeGroup,
                                            QStringLiteral("frequency")));
    m_logFrequencySummary = new QLabel;
    m_logFrequencySummary->setObjectName(
        QStringLiteral("logFrequencySummary"));
    m_logFrequencySummary->setWordWrap(true);
    m_logFrequencySummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    frequencyLayout->addWidget(m_logFrequencySummary);
    auto *frequencyScroll = new QScrollArea;
    frequencyScroll->setObjectName(QStringLiteral("logFrequencyScroll"));
    frequencyScroll->setFrameShape(QFrame::NoFrame);
    frequencyScroll->setWidgetResizable(true);
    auto *frequencyChart = new LogFrequencyBarChart;
    frequencyChart->setPromptHandler(
        [this](const LogFrequencyBarChart::Entry &entry) {
            sendLogFrequencyEntryToPrompt(entry.message, entry.category,
                                          entry.count, entry.share);
        });
    m_logFrequencyChart = frequencyChart;
    frequencyScroll->setWidget(m_logFrequencyChart);
    frequencyLayout->addWidget(frequencyScroll, 1);

    auto *usagePage = new QWidget;
    auto *usageLayout = new QVBoxLayout(usagePage);
    usageLayout->setContentsMargins(8, 8, 8, 8);
    usageLayout->setSpacing(8);
    auto *usageRefresh = new QPushButton(QStringLiteral("Refresh"));
    usageRefresh->setObjectName(QStringLiteral("ghostButton"));
    usageRefresh->setCursor(Qt::PointingHandCursor);
    usageRefresh->setToolTip(QStringLiteral(
        "Re-read today's usage from Cloudflare analytics"));
    setOcticon(usageRefresh, "sync", 14);
    connect(usageRefresh, &QPushButton::clicked, this, [this] {
        m_logUsageFetchedAtMs = 0; // an explicit ask bypasses the throttle
        fetchAdminUsageReport();
    });
    auto *usageHeader = new QHBoxLayout;
    usageHeader->setContentsMargins(0, 0, 0, 0);
    auto *usageHint = new QLabel(QStringLiteral(
        "What is spending the Cloudflare free-plan allowances, by Durable "
        "Object class and by live object."));
    usageHint->setObjectName(QStringLiteral("logFrequencySummary"));
    usageHint->setWordWrap(true);
    usageHeader->addWidget(usageHint, 1);
    usageHeader->addWidget(usageRefresh);
    usageLayout->addLayout(usageHeader);
    m_logUsageView = new QTextBrowser;
    m_logUsageView->setObjectName(QStringLiteral("logUsageView"));
    m_logUsageView->setReadOnly(true);
    m_logUsageView->setOpenExternalLinks(true);
    m_logUsageView->setFrameShape(QFrame::NoFrame);
    usageLayout->addWidget(m_logUsageView, 1);

    auto *d1UsagePage = new QWidget;
    auto *d1UsageLayout = new QVBoxLayout(d1UsagePage);
    d1UsageLayout->setContentsMargins(8, 8, 8, 8);
    d1UsageLayout->setSpacing(8);
    auto *d1UsageRefresh = new QPushButton(QStringLiteral("Refresh"));
    d1UsageRefresh->setObjectName(QStringLiteral("ghostButton"));
    d1UsageRefresh->setCursor(Qt::PointingHandCursor);
    d1UsageRefresh->setToolTip(
        QStringLiteral("Re-read today's D1 usage and database storage"));
    setOcticon(d1UsageRefresh, "sync", 14);
    connect(d1UsageRefresh, &QPushButton::clicked, this, [this] {
        m_logD1UsageFetchedAtMs = 0; // an explicit ask bypasses the throttle
        // A checkout can move on between reads, so an explicit ask also
        // re-locates the statements in the ranking below.
        m_d1QuerySourceIndex.clear();
        m_d1QuerySourceIndexReady = false;
        m_d1LiveQuerySites.clear();
        fetchAdminD1UsageReport();
    });
    // The analytics this tab reads are account-wide and minutes behind; the
    // provider's own D1 view is where a query shows up as it runs.
    auto *d1UsageLiveTail = new QPushButton(QStringLiteral("Live tail"));
    d1UsageLiveTail->setObjectName(QStringLiteral("ghostButton"));
    d1UsageLiveTail->setCursor(Qt::PointingHandCursor);
    d1UsageLiveTail->setToolTip(
        QStringLiteral("Open Cloudflare's live D1 view in a browser, where "
                       "queries appear as they run"));
    setOcticon(d1UsageLiveTail, "broadcast", 14);
    connect(d1UsageLiveTail, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(
            QUrl(forkmesh::control::cloudflareD1DashboardUrl(
                reportedD1DatabaseId())));
    });
    auto *d1UsageHeader = new QHBoxLayout;
    d1UsageHeader->setContentsMargins(0, 0, 0, 0);
    auto *d1UsageHint = new QLabel(QStringLiteral(
        "Live rqlite statements from the core nodes' journals, then today's "
        "D1 rows read and written against the Cloudflare Free allowances, "
        "plus bound-database storage and per-database traffic."));
    d1UsageHint->setObjectName(QStringLiteral("logFrequencySummary"));
    d1UsageHint->setWordWrap(true);
    d1UsageHeader->addWidget(d1UsageHint, 1);
    d1UsageHeader->addWidget(d1UsageLiveTail);
    d1UsageHeader->addWidget(d1UsageRefresh);
    d1UsageLayout->addLayout(d1UsageHeader);
    m_logD1UsageView = new QTextBrowser;
    m_logD1UsageView->setObjectName(QStringLiteral("logD1UsageView"));
    m_logD1UsageView->setReadOnly(true);
    // Query rankings carry checkout links, so navigation is handled here
    // rather than handed to the browser.
    m_logD1UsageView->setOpenLinks(false);
    connect(m_logD1UsageView, &QTextBrowser::anchorClicked, this,
            [this](const QUrl &url) {
                // The read guard panel's mode and threshold links.
                if (url.scheme() == QLatin1String("forkmesh-d1-guard")) {
                    const QUrlQuery guard(url);
                    setAdminD1ReadGuard(
                        guard.queryItemValue(QStringLiteral("mode")),
                        guard.queryItemValue(QStringLiteral("threshold"))
                            .toInt());
                    return;
                }
                QString sourcePath;
                int sourceLine = 0;
                if (logSourceAnchorTarget(url.toString(), &sourcePath,
                                          &sourceLine)) {
                    revealLogSourceInExplorer(sourcePath, sourceLine);
                    return;
                }
                if (url.scheme().startsWith(QLatin1String("http")))
                    QDesktopServices::openUrl(url);
            });
    m_logD1UsageView->setFrameShape(QFrame::NoFrame);
    d1UsageLayout->addWidget(m_logD1UsageView, 1);

    auto *workerFootprintPage = new QWidget;
    auto *workerFootprintLayout = new QVBoxLayout(workerFootprintPage);
    workerFootprintLayout->setContentsMargins(8, 8, 8, 8);
    workerFootprintLayout->setSpacing(8);
    auto *workerFootprintRefresh = new QPushButton(QStringLiteral("Refresh"));
    workerFootprintRefresh->setObjectName(QStringLiteral("ghostButton"));
    workerFootprintRefresh->setCursor(Qt::PointingHandCursor);
    workerFootprintRefresh->setToolTip(QStringLiteral(
        "Re-read the source and static-asset footprint from the deployment"));
    setOcticon(workerFootprintRefresh, "sync", 14);
    connect(workerFootprintRefresh, &QPushButton::clicked, this, [this] {
        m_logWorkerFootprintFetchedAtMs = 0;
        fetchWorkerFootprintReport();
    });
    auto *workerFootprintHeader = new QHBoxLayout;
    workerFootprintHeader->setContentsMargins(0, 0, 0, 0);
    auto *workerFootprintHint = new QLabel(QStringLiteral(
        "The deployed Python startup, initial World module graph, and static "
        "assets against the growth budgets that can stop a deployment."));
    workerFootprintHint->setObjectName(QStringLiteral("logFrequencySummary"));
    workerFootprintHint->setWordWrap(true);
    workerFootprintHeader->addWidget(workerFootprintHint, 1);
    workerFootprintHeader->addWidget(workerFootprintRefresh);
    workerFootprintLayout->addLayout(workerFootprintHeader);
    m_logWorkerFootprintView = new QTextBrowser;
    m_logWorkerFootprintView->setObjectName(
        QStringLiteral("logWorkerFootprintView"));
    m_logWorkerFootprintView->setReadOnly(true);
    m_logWorkerFootprintView->setFrameShape(QFrame::NoFrame);
    workerFootprintLayout->addWidget(m_logWorkerFootprintView, 1);

    // The Usage tab answers "what is spending the allowance"; the busiest
    // spender is the maintenance loop, and its own tab answers what that loop
    // is scheduled to do, what the retained ticks say it did, and on what
    // terms. It reads one admin-signed relay endpoint, never the Durable
    // Objects themselves.
    auto *cronPage = new QWidget;
    auto *cronLayout = new QVBoxLayout(cronPage);
    cronLayout->setContentsMargins(8, 8, 8, 8);
    cronLayout->setSpacing(8);
    auto *cronRefresh = new QPushButton(QStringLiteral("Refresh"));
    cronRefresh->setObjectName(QStringLiteral("ghostButton"));
    cronRefresh->setCursor(Qt::PointingHandCursor);
    cronRefresh->setToolTip(
        QStringLiteral("Re-read the maintenance schedule and tick history"));
    setOcticon(cronRefresh, "sync", 14);
    connect(cronRefresh, &QPushButton::clicked, this, [this] {
        m_logCronFetchedAtMs = 0; // an explicit ask bypasses the throttle
        fetchAdminCronReport();
    });
    auto *cronHeader = new QHBoxLayout;
    cronHeader->setContentsMargins(0, 0, 0, 0);
    auto *cronHint = new QLabel(QStringLiteral(
        "What the maintenance cron runs each minute, what it costs in Durable "
        "Object invocations, and the settings that decide what a tick "
        "delivers."));
    cronHint->setObjectName(QStringLiteral("logFrequencySummary"));
    cronHint->setWordWrap(true);
    cronHeader->addWidget(cronHint, 1);
    cronHeader->addWidget(cronRefresh);
    cronLayout->addLayout(cronHeader);
    m_logCronView = new QTextBrowser;
    m_logCronView->setObjectName(QStringLiteral("logCronView"));
    m_logCronView->setReadOnly(true);
    m_logCronView->setOpenExternalLinks(true);
    m_logCronView->setFrameShape(QFrame::NoFrame);
    cronLayout->addWidget(m_logCronView, 1);

    m_logViewTabs = new QTabWidget(page);
    m_logViewTabs->setObjectName(QStringLiteral("logViewTabs"));
    m_logViewTabs->setDocumentMode(true);
    m_logViewTabs->addTab(activityPage, QStringLiteral("Activity"));
    m_logAggregationTabIndex = m_logViewTabs->addTab(
        frequencyPage, QStringLiteral("Most frequent"));
    m_logUsageTabIndex = m_logViewTabs->addTab(usagePage,
                                               QStringLiteral("Usage"));
    m_logViewTabs->setTabToolTip(
        m_logUsageTabIndex,
        QStringLiteral("Durable Object and Worker usage against today's free "
                       "allowances, plus per-object live traffic"));
    m_logD1UsageTabIndex = m_logViewTabs->addTab(
        d1UsagePage, QStringLiteral("D1"));
    m_logViewTabs->setTabToolTip(
        m_logD1UsageTabIndex,
        QStringLiteral("Live rqlite statements from the core nodes, and D1 "
                       "rows read, rows written, and storage against the "
                       "Cloudflare Free limits"));
    // The D1 tab counts what the database costs; this one opens the tables
    // those counts are about. Every read and write is signed with the
    // administrator's device key, like the reports beside it.
    forkmesh::DatabaseManagerHooks databaseHooks;
    databaseHooks.network = [this] { return m_networkAccess; };
    databaseHooks.endpoint = [this] {
        return accountsApiUrl(QStringLiteral("admin-database"));
    };
    databaseHooks.administrator = [this](QString *reason) -> QString {
        const QString node = accountOwner();
        if (!m_isAdmin) {
            *reason = QStringLiteral(
                "Only platform administrators can open the database.");
            return QString();
        }
        if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
            !m_profileIdentity.isValid()) {
            *reason = QStringLiteral("This desktop cannot sign administrator "
                                     "requests for the account.");
            return QString();
        }
        return node;
    };
    databaseHooks.sign = [this](const QByteArray &canonical) {
        return m_profileIdentity.signData(canonical);
    };
    databaseHooks.d1UsageReport = [this] { return m_logD1UsageReport; };
    databaseHooks.decorateButton = [](QPushButton *button,
                                      const QString &icon) {
        setOcticon(button, icon, 14);
    };
    m_databaseManager =
        new forkmesh::DatabaseManagerWidget(std::move(databaseHooks));
    m_logDatabaseTabIndex = m_logViewTabs->addTab(
        m_databaseManager, QStringLiteral("Database"));
    m_logViewTabs->setTabToolTip(
        m_logDatabaseTabIndex,
        QStringLiteral("Browse, create, edit, delete and export D1 rows, with "
                       "the D1 audit's findings for every table"));
    m_logWorkerFootprintTabIndex = m_logViewTabs->addTab(
        workerFootprintPage, QStringLiteral("Build"));
    m_logViewTabs->setTabToolTip(
        m_logWorkerFootprintTabIndex,
        QStringLiteral("Deployed Python source and static-asset footprint against "
                       "the enforced growth budgets"));
    m_logCronTabIndex = m_logViewTabs->addTab(cronPage,
                                              QStringLiteral("Cron"));
    m_logViewTabs->setTabToolTip(
        m_logCronTabIndex,
        QStringLiteral("The maintenance schedule, what each recent tick ran "
                       "and cost, and the cron settings"));
    m_fleetVisualizer = new forkmesh::FleetVisualizer;
    m_fleetVisualizer->nodeSource = [this] { return m_coreLogTally.nodes(); };
    m_fleetVisualizer->idleReasonSource = [this] { return m_coreLogIdleReason; };
    m_fleetVisualizer->onTableClicked = [this](const QString &table) {
        const int index =
            m_databaseManager ? m_logViewTabs->indexOf(m_databaseManager) : -1;
        if (index < 0)
            return;
        m_logViewTabs->setCurrentIndex(index);
        m_databaseManager->showTable(table);
    };
    auto *visualizerScroll = new QScrollArea;
    visualizerScroll->setObjectName(QStringLiteral("fleetVisualizerScroll"));
    visualizerScroll->setFrameShape(QFrame::NoFrame);
    visualizerScroll->setWidgetResizable(true);
    m_queryInspector = new forkmesh::QueryInspector;
    m_desktopProcessView = new forkmesh::DesktopProcessView;
    m_desktopProcessView->onProcessClicked = [this](const auto &process) {
        showDesktopProcessDetails(process);
    };
    m_desktopProcessView->governanceSource = [](int memoryReserve, int cpuReserve) {
        namespace ar = forkmesh::agentresources;
        const QSettings saved;
        if (memoryReserve < 0)
            memoryReserve =
                saved.value(ar::kReservePercentSetting, ar::kDefaultReservePercent).toInt();
        if (cpuReserve < 0)
            cpuReserve = saved.value(ar::kCpuReservePercentSetting,
                                     ar::kDefaultCpuReservePercent)
                             .toInt();
        forkmesh::DesktopProcessView::Governance governance;
        governance.minReservePercent = ar::kMinReservePercent;
        governance.maxReservePercent = ar::kMaxReservePercent;
        governance.memoryReservePercent =
            qBound(ar::kMinReservePercent, memoryReserve, ar::kMaxReservePercent);
        governance.cpuReservePercent =
            qBound(ar::kMinReservePercent, cpuReserve, ar::kMaxReservePercent);
        const ar::Budget budget = ar::budgetFor(governance.memoryReservePercent,
                                                governance.cpuReservePercent);
        governance.available = budget.valid();
        governance.cpuText =
            QStringLiteral("%1 of %2 cores, %3 kept")
                .arg(budget.sliceCpuQuota / 100.0, 0, 'f', 1)
                .arg(budget.cpuCount)
                .arg(budget.cpuReserve / 100.0, 0, 'f', 1);
        governance.memoryText =
            QStringLiteral("%1 of %2, %3 kept")
                .arg(forkmesh::corelogs::compactByteSize(budget.sliceHighBytes),
                     forkmesh::corelogs::compactByteSize(budget.totalBytes),
                     forkmesh::corelogs::compactByteSize(budget.reserveBytes));
        const ar::Status status = ar::status();
        governance.note =
            status.available
                ? QStringLiteral("Agent limits apply at once to %1 and to agents "
                                 "already running. Each agent may use up to %2 on "
                                 "its own and runs %3 build jobs.")
                      .arg(ar::sliceName(),
                           forkmesh::corelogs::compactByteSize(budget.agentMaxBytes))
                      .arg(budget.jobsPerAgent)
                : QStringLiteral("Saved. Agents are not contained right now (%1), "
                                 "so these limits apply once they are.")
                      .arg(status.reason.isEmpty() ? QStringLiteral("still starting")
                                                   : status.reason);
        return governance;
    };
    m_desktopProcessView->onGovernanceChanged = [](int memoryReserve, int cpuReserve) {
        forkmesh::agentresources::setReservePercents(memoryReserve, cpuReserve);
        forkmesh::agentresources::applyBudget();
    };
    auto *desktopResourceScroll = new QScrollArea;
    desktopResourceScroll->setObjectName(QStringLiteral("desktopResourceScroll"));
    desktopResourceScroll->setFrameShape(QFrame::NoFrame);
    desktopResourceScroll->setWidgetResizable(true);
    desktopResourceScroll->setWidget(m_desktopProcessView);
    m_logDesktopResourceTabIndex = m_logViewTabs->addTab(
        desktopResourceScroll, QStringLiteral("Desktop"));
    m_logViewTabs->setTabToolTip(
        m_logDesktopResourceTabIndex,
        QStringLiteral("Live CPU, memory, disk and process diagnostics for this "
                       "desktop. Select a process to inspect or stop it."));
    auto *visualizerPage = new QWidget;
    visualizerPage->setObjectName(QStringLiteral("fleetVisualizerPage"));
    auto *visualizerLayout = new QVBoxLayout(visualizerPage);
    visualizerLayout->setContentsMargins(0, 0, 0, 0);
    visualizerLayout->setSpacing(0);
    visualizerLayout->addWidget(m_fleetVisualizer);
    visualizerLayout->addWidget(m_queryInspector);
    visualizerLayout->addStretch(1);
    visualizerScroll->setWidget(visualizerPage);
    m_logFleetVisualizerTabIndex = m_logViewTabs->addTab(
        visualizerScroll, QStringLiteral("Visualizer"));
    m_logViewTabs->setTabToolTip(
        m_logFleetVisualizerTabIndex,
        QStringLiteral("rqlite statements flying from each node to its "
                       "tables, every query's caller, code and time, every "
                       "node's memory, disk, files and CPU"));
    ensureDesktopPackageLogPages();
    connect(m_logViewTabs, &QTabWidget::currentChanged, this,
            [this](int index) {
                if (index == m_logAggregationTabIndex)
                    refreshLogFrequencyChart();
                if (index == m_logUsageTabIndex) {
                    refreshLogUsageView();
                    fetchAdminUsageReport();
                }
                if (index == m_logD1UsageTabIndex) {
                    refreshLogD1UsageView();
                    fetchAdminD1UsageReport();
                }
                if (index == m_logDatabaseTabIndex && m_databaseManager) {
                    // Today's query ranking names each table's hot queries.
                    fetchAdminD1UsageReport();
                    m_databaseManager->activate();
                }
                if (index == m_logWorkerFootprintTabIndex) {
                    refreshLogWorkerFootprintView();
                    fetchWorkerFootprintReport();
                }
                // Opening the tab reads the report once; Refresh re-reads it.
                // Nothing polls: each read is a signed round trip.
                if (index == m_logCronTabIndex) {
                    refreshLogCronView();
                    fetchAdminCronReport();
                }
                refreshCurrentDesktopPackageLogPage();
            });
    // The tail only feeds the tally while the page is open, so redrawing off
    // screen is wasted paint; a second is as often as the counts can usefully
    // move, and switching to the tab redraws immediately.
    m_logUsageRefreshTimer = new QTimer(this);
    m_logUsageRefreshTimer->setSingleShot(true);
    m_logUsageRefreshTimer->setInterval(1000);
    connect(m_logUsageRefreshTimer, &QTimer::timeout, this,
            &MainWindow::refreshLogUsageView);
    m_logFrequencyRefreshTimer = new QTimer(this);
    m_logFrequencyRefreshTimer->setSingleShot(true);
    // Re-ranking walks every retained line in the timeframe (up to the
    // 20,000-line buffer). While the log streams, once a second is as often
    // as a ranking can usefully change on screen; the tab switch and the
    // timeframe buttons still refresh immediately.
    m_logFrequencyRefreshTimer->setInterval(1000);
    connect(m_logFrequencyRefreshTimer, &QTimer::timeout, this,
            &MainWindow::refreshLogFrequencyChart);

    auto *filterRowWidget = new QWidget;
    m_logFilterRow = new QHBoxLayout(filterRowWidget);
    m_logFilterRow->setContentsMargins(0, 0, 0, 0);
    m_logFilterRow->setSpacing(6);
    auto *filterScroll = new QScrollArea;
    filterScroll->setObjectName("logFilterScroll");
    filterScroll->setWidget(filterRowWidget);
    filterScroll->setWidgetResizable(true);
    filterScroll->setFrameShape(QFrame::NoFrame);
    filterScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    filterScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    filterScroll->setFixedHeight(80);

    m_logFilterCounts.clear();
    for (const QString &line : std::as_const(m_networkLog))
        ++m_logFilterCounts[logBadgeFor(line)];
    rebuildLogFilterButtons();
    m_networkLogViewStale = !m_networkLog.isEmpty();

    connect(clearButton, &QPushButton::clicked, this, [this] {
        m_networkLogClearedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_networkLog.clear();
        clearLogStyleCache();
        m_lastLogRenderDate.clear();
        m_logFilters.clear();
        m_logTextFilter.clear();
        m_logTextFilterIsGlob = false;
        m_logTextFilterRegex = QRegularExpression();
        if (m_logSearchEdit) {
            QSignalBlocker block(m_logSearchEdit); // nothing left to re-filter
            m_logSearchEdit->clear();
        }
        m_logFilterCounts.clear();
        m_logRenderFrom = 0; // nothing left to page back into once cleared
        m_logFilterEmptyNotice = false;
        m_logLivePending.clear();
        m_logPopoutLivePending.clear();
        m_logUnseenLines = 0;
        updateNetworkLogFollowButton();
        if (m_settingsLog)
            m_settingsLog->clear();
        if (m_logPopoutView)
            m_logPopoutView->clear();
        m_logPopoutDate.clear();
        m_logPopoutPending.clear();
        updateNetworkLogPopoutStatus();
        if (m_logActivityLights)
            m_logActivityLights->reset();
        if (m_logActivityHeader)
            m_logActivityHeader->reset();
        m_debugAllLogCount = 0;
        if (auto *allButton =
                dynamic_cast<ActivityRailButton *>(m_debugLogAllButton))
            allButton->setBadgeCount(0);
        m_cloudUsageTally.clear();
        refreshLogUsageView();
        saveNetworkLog(); // truncate the live file too
        // Clear means clear: the rotated segments and the chart's digest are
        // the rest of the record, so they go with it.
        flushNetworkLogWrites();
        flushNetworkLogDigestWrites();
        forkmesh::netlog::clearArchive(networkLogPath());
        m_networkLogDigest.clear();
        m_networkLogDigestBucket = -1;
        m_networkLogDigestDirty = false;
        rebuildLogFilterButtons(); // drop the category chips, re-check "All"
        refreshLogTimelineChart();
    });

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->addWidget(label);
    headerRow->addWidget(m_logTimelineSummary);
    headerRow->addStretch();
    m_nodeLogFilterButton = new QPushButton(QStringLiteral("Nodes"));
    m_nodeLogFilterButton->setObjectName(QStringLiteral("logNodeFilterButton"));
    m_nodeLogFilterButton->setCheckable(true);
    m_nodeLogFilterButton->setCursor(Qt::PointingHandCursor);
    m_nodeLogFilterButton->setToolTip(QStringLiteral(
        "Show only fleet node lines: pick nodes (n1, r1, ...), units "
        "(caddy, forkmesh-app, ...) or node errors only"));
    setOcticon(m_nodeLogFilterButton, "server", 14);
    connect(m_nodeLogFilterButton, &QPushButton::clicked, this, [this] {
        updateNodeLogFilterButton();
        showNodeLogFilterMenu();
    });
    headerRow->addWidget(m_logSearchEdit);
    headerRow->addWidget(m_nodeLogFilterButton);
    headerRow->addWidget(logTypesButton);
    headerRow->addWidget(m_loggingOffButton);
    headerRow->addWidget(m_logFollowButton);
    headerRow->addWidget(noWrapButton);
    headerRow->addWidget(formatJsonButton);
    headerRow->addWidget(popoutButton);
    headerRow->addWidget(clearButton);

    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(8);
    layout->addLayout(headerRow);
    layout->addWidget(filterScroll);
    setLogTimelinePresetHours(24);
    layout->addWidget(m_logViewTabs, 1);
    updateLogAggregationAvailability();
    return page;
}

void MainWindow::showFormattedLogJson()
{
    if (!m_settingsLog)
        return;

    const QTextCursor cursor = m_settingsLog->textCursor();
    QString source = cursor.selectedText();
    source.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    source = source.trimmed();
    if (source.isEmpty())
        source = cursor.block().text().trimmed();

    QJsonParseError parseError;
    QJsonDocument document = QJsonDocument::fromJson(source.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError ||
        (!document.isObject() && !document.isArray())) {
        const QString detected = completeJsonValueInText(source);
        if (detected.isEmpty()) {
            flashMessage(QStringLiteral(
                "No complete JSON object or array was found in this log entry."),
                true, QString(),
                forkmesh::kLocalOperationNoticeKind);
            return;
        }
        document = QJsonDocument::fromJson(detected.toUtf8(), &parseError);
    }

    const QString formatted =
        QString::fromUtf8(document.toJson(QJsonDocument::Indented));
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("formattedLogJsonDialog"));
    dialog.setWindowTitle(QStringLiteral("Formatted JSON"));
    dialog.resize(680, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *preview = new QPlainTextEdit(&dialog);
    preview->setObjectName(QStringLiteral("formattedLogJsonPreview"));
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    applyLogFont(preview);
    preview->setPlainText(formatted);
    layout->addWidget(preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton *copyButton = buttons->addButton(
        QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
    copyButton->setObjectName(QStringLiteral("copyFormattedLogJsonButton"));
    connect(copyButton, &QPushButton::clicked, &dialog, [formatted, copyButton] {
        QApplication::clipboard()->setText(formatted);
        copyButton->setText(QStringLiteral("Copied!"));
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::refreshLogTimelineChart()
{
    if (!m_logTimelineChart)
        return;
    const qint64 fromMs = m_logTimelineChart->viewFromMs();
    const qint64 toMs = m_logTimelineChart->viewToMs();
    const QString from = QDateTime::fromMSecsSinceEpoch(fromMs)
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QString to = QDateTime::fromMSecsSinceEpoch(toMs)
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const auto first = std::lower_bound(
        m_networkLog.cbegin(), m_networkLog.cend(), from,
        [](const QString &line, const QString &timestamp) {
            return line.left(19) < timestamp;
        });
    const auto last = std::upper_bound(
        first, m_networkLog.cend(), to,
        [](const QString &timestamp, const QString &line) {
            return timestamp < line.left(19);
        });
    QVector<LogTimelineEntry> entries;
    entries.reserve(int(std::distance(first, last)));
    QString cachedDate;
    qint64 cachedMidnightMs = 0;
    for (auto it = first; it != last; ++it) {
        const QString &line = *it;
        const qint64 timestampMs =
            fastStoredLogTimestampMs(line, &cachedDate, &cachedMidnightMs);
        if (timestampMs < 0)
            continue;
        // The category filter only dims its bars; a text filter is a hard cut,
        // so the chart counts what the view below it actually shows.
        if (!logLineMatchesTextFilter(line))
            continue;
        LogTimelineEntry entry;
        entry.timestampMs = timestampMs;
        entry.category = logBadgeFor(line);
        entries.append(entry);
    }
    m_logTimelineChart->setEntries(std::move(entries));
    // Older than the buffer, the digest is all there is. A text filter cannot
    // be applied to bucket counts, so history drops out while one is set -
    // matching the promise above that the chart counts what the view shows.
    m_logTimelineChart->setHistoryBuckets(
        m_logTextFilter.isEmpty() ? networkLogHistoryBuckets(fromMs, toMs)
                                  : QVector<LogTimelineBucket>());
    m_logTimelineChart->setSeries(
        logTimelineSeries(), hasLogCategoryFilter(),
        QColor(QStringLiteral("#58a6ff")));
    updateLogTimelineSummary();
    if (m_logViewTabs &&
        m_logViewTabs->currentIndex() == m_logAggregationTabIndex)
        refreshLogFrequencyChart();
}

void MainWindow::updateLogAggregationAvailability()
{
    if (!m_logViewTabs || m_logAggregationTabIndex < 0)
        return;
    const bool allCategories = !hasLogCategoryFilter();
    m_logViewTabs->setTabEnabled(m_logAggregationTabIndex, allCategories);
    m_logViewTabs->setTabToolTip(
        m_logAggregationTabIndex,
        allCategories
            ? QStringLiteral(
                  "Rank repeated detailed log messages in the selected timeframe")
            : QStringLiteral(
                  "Choose All categories to compare repeated log messages"));
    if (!allCategories &&
        m_logViewTabs->currentIndex() == m_logAggregationTabIndex)
        m_logViewTabs->setCurrentIndex(0);
}

void MainWindow::refreshLogUsageView()
{
    if (m_logUsageRefreshTimer && m_logUsageRefreshTimer->isActive())
        m_logUsageRefreshTimer->stop();
    if (!m_logUsageView)
        return;
    // Re-rendering resets the scroll position, which is maddening while
    // reading a long table as the tail ticks; put it back where it was.
    const int scroll = m_logUsageView->verticalScrollBar()->value();
    m_logUsageView->setHtml(forkmesh::control::renderCloudUsageHtml(
        m_logUsageReport, m_cloudUsageTally.ranked(40),
        m_cloudUsageTally.sinceMs(), m_cloudUsageTally.totalEvents(),
        QDateTime::currentMSecsSinceEpoch(), currentThemeIsDark()));
    m_logUsageView->verticalScrollBar()->setValue(scroll);
}

void MainWindow::fetchAdminUsageReport()
{
    // The report is a Cloudflare GraphQL round trip behind an admin
    // signature, and the numbers it reports only move as analytics ingests;
    // once a minute is as fresh as it can usefully be, and the Refresh button
    // clears the stamp when the reader wants it now.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_logUsageFetchInFlight ||
        (m_logUsageFetchedAtMs > 0 && now - m_logUsageFetchedAtMs < 60'000))
        return;
    const QString node = accountOwner();
    if (!m_isAdmin || node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid()) {
        m_logUsageReport = QJsonObject{{QStringLiteral("ok"), false},
                                       {QStringLiteral("error"),
                                        QStringLiteral("forbidden")}};
        refreshLogUsageView();
        return;
    }
    const QString ts = QString::number(now);
    const QByteArray canonical =
        ("forkmesh-admin-usage-report-v1\n" + node + "\n" + ts).toUtf8();
    QUrl url = accountsApiUrl("admin-usage-report");
    QUrlQuery query;
    query.addQueryItem("node", node);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", m_profileIdentity.signData(canonical));
    url.setQuery(query);
    m_logUsageFetchInFlight = true;
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, now]() {
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();
        m_logUsageFetchInFlight = false;
        m_logUsageFetchedAtMs = now;
        QJsonObject parsed = QJsonDocument::fromJson(body).object();
        // A relay too old to know the route, or one that could not be
        // reached, still has to say so in the tab rather than sit blank.
        if (parsed.isEmpty())
            parsed = QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"),
                 error == QNetworkReply::NoError
                     ? QStringLiteral("unreadable relay response")
                     : QStringLiteral("relay unreachable")}};
        m_logUsageReport = parsed;
        refreshLogUsageView();
    });
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testLogUsageHtml() const
{
    return m_logUsageView ? m_logUsageView->toHtml() : QString();
}

QString MainWindow::testLogD1UsageHtml() const
{
    return m_logD1UsageView ? m_logD1UsageView->toHtml() : QString();
}

QString MainWindow::testLogWorkerFootprintHtml() const
{
    return m_logWorkerFootprintView ? m_logWorkerFootprintView->toHtml()
                                    : QString();
}
#endif

// The one database today's report attributes usage to, which is what the
// provider's live view should open on. Anything else (no report yet, several
// databases in the account) leaves the destination at the D1 list.
QString MainWindow::reportedD1DatabaseId() const
{
    const QJsonArray databases =
        m_logD1UsageReport.value(QStringLiteral("databases")).toArray();
    if (databases.size() != 1)
        return QString();
    return databases.at(0)
        .toObject()
        .value(QStringLiteral("databaseId"))
        .toString()
        .trimmed();
}

// Reading every Worker module takes long enough to be felt on the GUI thread,
// so the statement index is built once per checkout off-thread; the tab
// repaints with its links when it lands.
void MainWindow::ensureD1QuerySourceIndex()
{
    if (m_d1QuerySourceIndexReady || m_d1QuerySourceIndexBuilding)
        return;
    QStringList roots{forkmesh::logSourceBuildRoot()};
    const QString repo = repoGitDir();
    if (!repo.isEmpty())
        roots << repo;
    m_d1QuerySourceIndexBuilding = true;
    using IndexWatcher = QFutureWatcher<QList<forkmesh::D1QueryStatement>>;
    auto *watcher = new IndexWatcher(this);
    connect(watcher, &IndexWatcher::finished, this, [this, watcher] {
        m_d1QuerySourceIndex = watcher->result();
        m_d1QuerySourceIndexReady = true;
        m_d1LiveQuerySites.clear();
        m_d1QuerySourceIndexBuilding = false;
        watcher->deleteLater();
        refreshLogD1UsageView();
    });
    watcher->setFuture(QtConcurrent::run([roots] {
        return forkmesh::buildD1QuerySourceIndex(roots);
    }));
}

void MainWindow::refreshLogD1UsageView()
{
    if (!m_logD1UsageView)
        return;
    const QJsonArray insights =
        m_logD1UsageReport.value(QStringLiteral("queryInsights")).toArray();
    QHash<QString, forkmesh::control::D1QuerySourceLink> querySources;
    if (!insights.isEmpty()) {
        ensureD1QuerySourceIndex();
        for (const QJsonValue &value : insights) {
            const QString query =
                value.toObject().value(QStringLiteral("query")).toString();
            if (query.isEmpty() || querySources.contains(query))
                continue;
            const forkmesh::D1QuerySite site =
                forkmesh::matchD1QuerySite(query, m_d1QuerySourceIndex);
            if (!site.isValid())
                continue;
            forkmesh::control::D1QuerySourceLink link;
            link.href = logSourceAnchorHref(site.path, site.line);
            link.label = QStringLiteral("%1:%2").arg(site.path).arg(site.line);
            if (site.otherSites > 0)
                link.note = QStringLiteral("+%1 more place%2")
                                .arg(site.otherSites)
                                .arg(site.otherSites == 1
                                         ? QString()
                                         : QStringLiteral("s"));
            querySources.insert(query, link);
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QList<forkmesh::corelogs::StatementRow> coreStatements =
        m_coreLogTally.topStatements(40);
    QHash<QString, forkmesh::corelogs::StatementLink> coreLinks;
    for (const forkmesh::corelogs::StatementRow &row : coreStatements) {
        const forkmesh::D1QuerySite site =
            d1LiveQuerySite(row.sql, row.originFile);
        if (!site.isValid())
            continue;
        coreLinks.insert(
            forkmesh::corelogs::statementLinkKey(row.originFile, row.sql),
            {logSourceAnchorHref(site.path, site.line),
             QStringLiteral("%1:%2").arg(site.path).arg(site.line)});
    }
    const QString coreIdle = m_coreLogIdleReason;
    const int scroll = m_logD1UsageView->verticalScrollBar()->value();
    m_logD1UsageView->setHtml(
        forkmesh::corelogs::renderActivityHtml(m_coreLogTally, now,
                                               currentThemeIsDark(), coreIdle,
                                               coreLinks) +
        forkmesh::control::renderD1UsageHtml(m_logD1UsageReport, now,
                                             currentThemeIsDark(),
                                             querySources));
    m_logD1UsageView->verticalScrollBar()->setValue(scroll);
}

void MainWindow::fetchAdminD1UsageReport()
{
    // D1 analytics arrives on the same delayed cadence as the broader account
    // report. A minute floor avoids turning tab switches into GraphQL traffic;
    // Refresh explicitly clears it when an operator needs a newer sample.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_logD1UsageFetchInFlight ||
        (m_logD1UsageFetchedAtMs > 0 &&
         now - m_logD1UsageFetchedAtMs < 60'000))
        return;
    const QString node = accountOwner();
    if (!m_isAdmin || node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid()) {
        m_logD1UsageReport =
            QJsonObject{{QStringLiteral("ok"), false},
                        {QStringLiteral("error"), QStringLiteral("forbidden")}};
        refreshLogD1UsageView();
        return;
    }
    const QString ts = QString::number(now);
    const QByteArray canonical =
        ("forkmesh-admin-d1-usage-report-v1\n" + node + "\n" + ts).toUtf8();
    QUrl url = accountsApiUrl("admin-d1-usage-report");
    QUrlQuery query;
    query.addQueryItem("node", node);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", m_profileIdentity.signData(canonical));
    url.setQuery(query);
    m_logD1UsageFetchInFlight = true;
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, now]() {
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();
        m_logD1UsageFetchInFlight = false;
        m_logD1UsageFetchedAtMs = now;
        QJsonObject parsed = QJsonDocument::fromJson(body).object();
        if (parsed.isEmpty())
            parsed = QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"),
                 error == QNetworkReply::NoError
                     ? QStringLiteral("unreadable relay response")
                     : QStringLiteral("relay unreachable")}};
        m_logD1UsageReport = parsed;
        refreshLogD1UsageView();
    });
}

void MainWindow::setAdminD1ReadGuard(const QString &mode, int threshold)
{
    // Every Worker isolate re-reads the switch within a minute; the reply
    // carries the new state so the panel updates without a GraphQL refetch.
    const QString node = accountOwner();
    if (!m_isAdmin || node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid())
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString thresholdText = QString::number(threshold);
    const QByteArray canonical =
        ("forkmesh-admin-d1-read-guard-v1\n" + node + "\n" + ts + "\n" + mode +
         "\n" + thresholdText)
            .toUtf8();
    QNetworkRequest request(accountsApiUrl("admin-d1-read-guard"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QByteArray body =
        QJsonDocument(
            QJsonObject{{QStringLiteral("node"), node},
                        {QStringLiteral("ts"), ts},
                        {QStringLiteral("sig"),
                         m_profileIdentity.signData(canonical)},
                        {QStringLiteral("mode"), mode},
                        {QStringLiteral("threshold"), thresholdText}})
            .toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_networkAccess->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QJsonObject parsed =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        const QJsonObject guard =
            parsed.value(QStringLiteral("readGuard")).toObject();
        if (!parsed.value(QStringLiteral("ok")).toBool() || guard.isEmpty()) {
            logSystem(QStringLiteral("Could not change the D1 read guard (%1).")
                          .arg(parsed.value(QStringLiteral("error"))
                                   .toString(
                                       QStringLiteral("relay unreachable"))));
            return;
        }
        m_logD1UsageReport.insert(QStringLiteral("readGuard"), guard);
        refreshLogD1UsageView();
    });
}

void MainWindow::refreshLogWorkerFootprintView()
{
    if (!m_logWorkerFootprintView)
        return;
    const int scroll = m_logWorkerFootprintView->verticalScrollBar()->value();
    m_logWorkerFootprintView->setHtml(
        forkmesh::control::renderWorkerFootprintHtml(
            m_logWorkerFootprintReport, currentThemeIsDark()));
    m_logWorkerFootprintView->verticalScrollBar()->setValue(scroll);
}

void MainWindow::fetchWorkerFootprintReport()
{
    // The file is immutable within one deployment but changes on the next
    // upload. A short local throttle keeps tab switches cheap while the
    // no-cache request and response headers prevent an old deploy from being
    // mistaken for the one currently serving this node.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_logWorkerFootprintFetchInFlight ||
        (m_logWorkerFootprintFetchedAtMs > 0 &&
         now - m_logWorkerFootprintFetchedAtMs < 60'000))
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/worker-footprint.json"));
    url.setQuery(QString());
    QNetworkRequest request(url);
    request.setRawHeader("Cache-Control", "no-cache");
    m_logWorkerFootprintFetchInFlight = true;
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, now]() {
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();
        m_logWorkerFootprintFetchInFlight = false;
        m_logWorkerFootprintFetchedAtMs = now;
        QJsonObject parsed = QJsonDocument::fromJson(body).object();
        if (parsed.isEmpty())
            parsed = QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"),
                 error == QNetworkReply::NoError
                     ? QStringLiteral("deployment report unavailable")
                     : QStringLiteral("relay unreachable")}};
        m_logWorkerFootprintReport = parsed;
        refreshLogWorkerFootprintView();
    });
}

void MainWindow::refreshLogCronView()
{
    if (!m_logCronView)
        return;
    // Re-rendering resets the scroll position, which is maddening a minute
    // into reading the phase table; put it back where it was.
    const int scroll = m_logCronView->verticalScrollBar()->value();
    m_logCronView->setHtml(forkmesh::control::renderCronActivityHtml(
        m_logCronReport, QDateTime::currentMSecsSinceEpoch(),
        currentThemeIsDark()));
    m_logCronView->verticalScrollBar()->setValue(scroll);
}

void MainWindow::fetchAdminCronReport()
{
    // The report is two D1 reads behind an admin signature, and the history
    // it reports only moves once a minute; the Refresh button clears the
    // stamp when the reader wants it now.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_logCronFetchInFlight ||
        (m_logCronFetchedAtMs > 0 && now - m_logCronFetchedAtMs < 30'000))
        return;
    const QString node = accountOwner();
    if (!m_isAdmin || node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid()) {
        m_logCronReport = QJsonObject{{QStringLiteral("ok"), false},
                                      {QStringLiteral("error"),
                                       QStringLiteral("forbidden")}};
        refreshLogCronView();
        return;
    }
    const QString ts = QString::number(now);
    const QByteArray canonical =
        ("forkmesh-admin-cron-report-v1\n" + node + "\n" + ts).toUtf8();
    QUrl url = accountsApiUrl("admin-cron-report");
    QUrlQuery query;
    query.addQueryItem("node", node);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", m_profileIdentity.signData(canonical));
    url.setQuery(query);
    m_logCronFetchInFlight = true;
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, now]() {
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        reply->deleteLater();
        m_logCronFetchInFlight = false;
        m_logCronFetchedAtMs = now;
        QJsonObject parsed = QJsonDocument::fromJson(body).object();
        // A relay too old to know the route, or one that could not be
        // reached, still has to say so in the tab rather than sit blank.
        if (parsed.isEmpty())
            parsed = QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"),
                 error == QNetworkReply::NoError
                     ? QStringLiteral("unreadable relay response")
                     : QStringLiteral("relay unreachable")}};
        m_logCronReport = parsed;
        refreshLogCronView();
    });
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testLogCronHtml() const
{
    return m_logCronView ? m_logCronView->toHtml() : QString();
}
#endif

void MainWindow::refreshLogFrequencyChart()
{
    if (m_logFrequencyRefreshTimer && m_logFrequencyRefreshTimer->isActive())
        m_logFrequencyRefreshTimer->stop();
    auto *chart = static_cast<LogFrequencyBarChart *>(m_logFrequencyChart);
    if (!chart || !m_logTimelineChart || !m_logFrequencySummary)
        return;
    if (hasLogCategoryFilter()) {
        chart->setEntries({});
        m_logFrequencySummary->setText(QStringLiteral(
            "Choose All categories to compare repeated log messages."));
        return;
    }

    const qint64 fromMs = m_logTimelineChart->viewFromMs();
    const qint64 toMs = m_logTimelineChart->viewToMs();
    const QString from = QDateTime::fromMSecsSinceEpoch(fromMs)
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const QString to = QDateTime::fromMSecsSinceEpoch(toMs)
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    const auto first = std::lower_bound(
        m_networkLog.cbegin(), m_networkLog.cend(), from,
        [](const QString &line, const QString &timestamp) {
            return line.left(19) < timestamp;
        });
    const auto last = std::upper_bound(
        first, m_networkLog.cend(), to,
        [](const QString &timestamp, const QString &line) {
            return timestamp < line.left(19);
        });

    struct Tally {
        int count = 0;
        QString category;
    };
    QHash<QString, Tally> tallies;
    int total = 0;
    for (auto it = first; it != last; ++it) {
        const QString &line = *it;
        if (!logLineMatchesTextFilter(line))
            continue;
        const LogLineStyle &style = logStyleFor(line);
        const QString message = style.message.trimmed();
        if (message.isEmpty())
            continue;
        Tally &tally = tallies[message];
        ++tally.count;
        if (tally.category.isEmpty())
            tally.category = style.badge;
        ++total;
    }

    QVector<LogFrequencyBarChart::Entry> entries;
    entries.reserve(tallies.size());
    for (auto it = tallies.cbegin(); it != tallies.cend(); ++it) {
        LogFrequencyBarChart::Entry entry;
        entry.message = it.key();
        entry.category = it->category;
        entry.count = it->count;
        entry.share = total > 0 ? 100.0 * entry.count / total : 0.0;
        entries.append(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(),
              [](const LogFrequencyBarChart::Entry &left,
                 const LogFrequencyBarChart::Entry &right) {
                  if (left.count != right.count)
                      return left.count > right.count;
                  return left.message.compare(right.message,
                                              Qt::CaseInsensitive) < 0;
              });
    constexpr int kVisibleFrequencyRows = 20;
    if (entries.size() > kVisibleFrequencyRows)
        entries.resize(kVisibleFrequencyRows);
    chart->setEntries(std::move(entries));

    QString summary = QStringLiteral(
                          "%1 detailed log%2 · %3 distinct message%4 · ranked by "
                          "exact message text")
                          .arg(total)
                          .arg(total == 1 ? QString() : QStringLiteral("s"))
                          .arg(tallies.size())
                          .arg(tallies.size() == 1 ? QString()
                                                   : QStringLiteral("s"));
    if (!m_logTextFilter.isEmpty())
        summary += QStringLiteral(" · matching \"%1\"").arg(m_logTextFilter);
    summary += QStringLiteral(
        ". Older archive buckets only retain category totals, so this ranking "
        "uses the detailed retained log.");
    m_logFrequencySummary->setText(summary);
}

// The row buttons on Most frequent hand the whole entry over, so the prompt
// keeps the numbers that made the row worth asking about.
void MainWindow::sendLogFrequencyEntryToPrompt(const QString &message,
                                              const QString &category,
                                              int count, double share)
{
    if (message.isEmpty())
        return;
    QString context = QStringLiteral("Log message repeated %1 time%2")
                          .arg(count)
                          .arg(count == 1 ? QString() : QStringLiteral("s"));
    if (share > 0.0)
        context += QStringLiteral(" (%1% of the detailed log in the selected "
                                 "timeframe)")
                       .arg(share, 0, 'f', 1);
    if (!category.isEmpty())
        context += QStringLiteral(" · %1").arg(category);
    appendTextToActivePrompt(context + QStringLiteral(":\n") + message);
}

void MainWindow::appendLogTimelineEntry(const QString &storedLine)
{
    if (!m_logTimelineChart)
        return;
    const QDateTime timestamp = QDateTime::fromString(
        storedLine.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    if (!timestamp.isValid())
        return;
    if (m_logTimelinePresetMinutes > 0 && !m_logTimelineChart->isZoomed()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_logTimelineChart->setRange(
            now - qint64(m_logTimelinePresetMinutes) * 60 * 1000, now);
    }
    const qint64 timestampMs = timestamp.toMSecsSinceEpoch();
    if (timestampMs < m_logTimelineChart->viewFromMs() ||
        timestampMs > m_logTimelineChart->viewToMs())
        return;
    if (!logLineMatchesTextFilter(storedLine))
        return;
    LogTimelineEntry entry;
    entry.timestampMs = timestampMs;
    entry.category = logBadgeFor(storedLine);
    m_logTimelineChart->appendEntry(entry);
}

void MainWindow::setLogTimelinePresetMinutes(int minutes)
{
    if (!m_logTimelineChart || minutes <= 0)
        return;
    m_logTimelinePresetMinutes = minutes;
    for (QButtonGroup *group :
         {m_logTimelineRangeGroup, m_logFrequencyRangeGroup}) {
        if (group && group->button(minutes))
            group->button(minutes)->setChecked(true);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_logTimelineChart->setRange(now - qint64(minutes) * 60 * 1000, now);
    refreshLogTimelineChart();
}

void MainWindow::setLogTimelinePresetHours(int hours)
{
    setLogTimelinePresetMinutes(hours * 60);
}

void MainWindow::chooseCustomLogTimelineRange()
{
    if (!m_logTimelineChart)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Custom log timeframe"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *fromEdit = new QDateTimeEdit(
        QDateTime::fromMSecsSinceEpoch(
            m_logTimelineCustomFromMs > 0 ? m_logTimelineCustomFromMs
                                          : now - 24LL * 60 * 60 * 1000));
    auto *toEdit = new QDateTimeEdit(QDateTime::fromMSecsSinceEpoch(
        m_logTimelineCustomToMs > 0 ? m_logTimelineCustomToMs : now));
    for (QDateTimeEdit *edit : {fromEdit, toEdit}) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("MMM d, yyyy  h:mm AP"));
        edit->setMinimumWidth(230);
    }
    fromEdit->setObjectName(QStringLiteral("logCustomFrom"));
    toEdit->setObjectName(QStringLiteral("logCustomTo"));
    form->addRow(QStringLiteral("From"), fromEdit);
    form->addRow(QStringLiteral("To"), toEdit);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel |
                                         QDialogButtonBox::Ok);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Apply"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        for (QButtonGroup *group :
             {m_logTimelineRangeGroup, m_logFrequencyRangeGroup}) {
            if (group && group->button(m_logTimelinePresetMinutes))
                group->button(m_logTimelinePresetMinutes)->setChecked(true);
        }
        return;
    }
    const qint64 from = fromEdit->dateTime().toMSecsSinceEpoch();
    const qint64 to = toEdit->dateTime().toMSecsSinceEpoch();
    if (to <= from) {
        QMessageBox::information(
            this, QStringLiteral("Custom log timeframe"),
            QStringLiteral("The end time must be later than the start time."));
        for (QButtonGroup *group :
             {m_logTimelineRangeGroup, m_logFrequencyRangeGroup}) {
            if (group && group->button(m_logTimelinePresetMinutes))
                group->button(m_logTimelinePresetMinutes)->setChecked(true);
        }
        return;
    }
    m_logTimelinePresetMinutes = 0;
    m_logTimelineCustomFromMs = from;
    m_logTimelineCustomToMs = to;
    for (QButtonGroup *group :
         {m_logTimelineRangeGroup, m_logFrequencyRangeGroup}) {
        if (group && group->button(0))
            group->button(0)->setChecked(true);
    }
    m_logTimelineChart->setRange(from, to);
    refreshLogTimelineChart();
}

void MainWindow::updateLogTimelineSummary()
{
    if (!m_logTimelineChart || !m_logTimelineSummary)
        return;
    const int count = m_logTimelineChart->visibleEntryCount();
    const qint64 fromMs = m_logTimelineChart->viewFromMs();
    const qint64 toMs = m_logTimelineChart->viewToMs();
    const qint64 span = toMs - fromMs;
    const QString format = span <= 48LL * 60 * 60 * 1000
                               ? QStringLiteral("MMM d, h:mm AP")
                               : QStringLiteral("MMM d, yyyy");
    // The chart calls this back on every appended entry, i.e. once per logged
    // line, and formatting these two instants resolves the local time zone and
    // the locale each time. The endpoints only move when the view does.
    if (fromMs != m_logTimelineSummaryFromMs ||
        toMs != m_logTimelineSummaryToMs ||
        format != m_logTimelineSummaryFormat) {
        m_logTimelineSummaryFromMs = fromMs;
        m_logTimelineSummaryToMs = toMs;
        m_logTimelineSummaryFormat = format;
        m_logTimelineSummaryFromText =
            QDateTime::fromMSecsSinceEpoch(fromMs).toString(format);
        m_logTimelineSummaryToText =
            QDateTime::fromMSecsSinceEpoch(toMs).toString(format);
    }
    QString text = QStringLiteral("%1 log%2 · %3 – %4")
                       .arg(count)
                       .arg(count == 1 ? QString() : QStringLiteral("s"))
                       .arg(m_logTimelineSummaryFromText,
                            m_logTimelineSummaryToText);
    if (hasLogCategoryFilter())
        text.prepend(logCategoryFilterSummary() + QStringLiteral(" · "));
    if (m_logTimelineChart->isZoomed())
        text += QStringLiteral(" · Zoomed");
    m_logTimelineSummary->setText(text);
    if (m_logTimelineResetZoom)
        m_logTimelineResetZoom->setEnabled(m_logTimelineChart->isZoomed());
}

#ifdef FORKMESH_WINDOW_TESTS
QWidget *MainWindow::testLogTimelineChart() const
{
    return m_logTimelineChart;
}

int MainWindow::testLogTimelineVisibleCount() const
{
    return m_logTimelineChart ? m_logTimelineChart->visibleEntryCount() : -1;
}

void MainWindow::testSetLogTimelineHours(int hours)
{
    setLogTimelinePresetHours(hours);
}

QString MainWindow::testLogTimelineSummary() const
{
    return m_logTimelineSummary ? m_logTimelineSummary->text() : QString();
}

QStringList MainWindow::testLogViewTabLabels() const
{
    QStringList labels;
    if (!m_logViewTabs)
        return labels;
    for (int index = 0; index < m_logViewTabs->count(); ++index)
        labels << m_logViewTabs->tabText(index);
    return labels;
}

bool MainWindow::testLogAggregationTabEnabled() const
{
    return m_logViewTabs && m_logAggregationTabIndex >= 0 &&
           m_logViewTabs->isTabEnabled(m_logAggregationTabIndex);
}

QString MainWindow::testLogFrequencyMessageAt(int row) const
{
    const auto *chart =
        static_cast<const LogFrequencyBarChart *>(m_logFrequencyChart);
    return chart ? chart->messageAt(row) : QString();
}

int MainWindow::testLogFrequencyCountAt(int row) const
{
    const auto *chart =
        static_cast<const LogFrequencyBarChart *>(m_logFrequencyChart);
    return chart ? chart->countAt(row) : 0;
}

bool MainWindow::testClickLogFrequencyPromptButton(int row)
{
    auto *chart = static_cast<LogFrequencyBarChart *>(m_logFrequencyChart);
    QPushButton *button = chart ? chart->promptButtonAt(row) : nullptr;
    if (!button)
        return false;
    button->click();
    return true;
}
#endif

// Everything the background Worker tail needs: the Worker directory, the pinned
// Wrangler invocation, and the credential — which may never reach argv.
bool MainWindow::prepareCloudflareTail(
    QString *token, QString *workerDirectory,
    forkmesh::control::CloudflareBootstrapCommand *command)
{
    if (!token || !workerDirectory || !command)
        return false;
    const auto scrub = [token] {
        token->fill(QChar(u'\0'));
        token->clear();
    };
    *token = m_cloudflareTokenEdit ? m_cloudflareTokenEdit->text().trimmed()
                                   : QString();
    // Fall back to the credential this node already stores for the deploy
    // workflow (Settings > Secrets & Coves) rather than asking for a second
    // token that authenticates against the same account.
    const QMap<QString, QString> storedVariables = ActionStore::variables();
    if (token->isEmpty()) {
        *token = forkmesh::control::cloudflareApiTokenFromVariables(
            storedVariables);
    }
    if (token->isEmpty()) {
        m_cloudLogMonitorIdleReason = cloudLogMonitorMissingTokenReason();
        flashMessage(
            QStringLiteral(
                "Cloud log monitoring needs a Cloudflare API token. Store "
                "CLOUDFLARE_API_TOKEN in Settings > Secrets."),
            true);
        return false;
    }

    *workerDirectory = forkmesh::control::findCloudflareWorkerDirectory(
        QStringLiteral(FORKMESH_SOURCE_DIR),
        QCoreApplication::applicationDirPath());
    const forkmesh::control::NodeToolchain node =
        m_cloudLogNodeToolchain.isValid()
            ? m_cloudLogNodeToolchain
            : forkmesh::control::findNodeToolchain(
                  forkmesh::control::kWranglerMinimumNodeMajor);
    if (!node.isValid()) {
        m_cloudLogMonitorIdleReason = cloudLogMonitorOldNodeReason();
        flashMessage(cloudLogMonitorNodeRequirement(), true);
        scrub();
        return false;
    }
    m_cloudLogNodeToolchain = node;
    QString account =
        m_cloudflareAccountEdit
            ? m_cloudflareAccountEdit->text().trimmed()
            : QString();
    if (account.isEmpty()) {
        account =
            QSettings()
                .value(QStringLiteral("control/cloudflareAccount"))
                .toString()
                .trimmed();
    }
    if (account.isEmpty()) {
        account = forkmesh::control::cloudflareAccountIdFromVariables(
            storedVariables);
    }
    *command = forkmesh::control::buildCloudflareTailCommand(
        *token, account, node.npx, node.binDir);
    if (workerDirectory->isEmpty() || command->program.isEmpty()) {
        flashMessage(
            workerDirectory->isEmpty()
                ? QStringLiteral(
                      "The installed Cloudflare Worker bundle is incomplete.")
                : QStringLiteral(
                      "Cloudflare live logs require a valid account ID."),
            true);
        scrub();
        return false;
    }
    if (command->arguments.join(QChar(u'\0')).contains(*token)) {
        flashMessage(
            QStringLiteral(
                "Refusing an unsafe Worker log command containing a "
                "credential."),
            true);
        scrub();
        return false;
    }
    if (m_cloudflareTokenEdit &&
        !m_cloudflareTokenEdit->text().isEmpty()) {
        m_cloudflareTokenEdit->clear();
        m_cloudflareTokenEdit->setPlaceholderText(
            QStringLiteral("token is in the log monitor's tail only"));
    }
    return true;
}


bool MainWindow::cloudLogMonitorRunning() const
{
    return m_cloudLogMonitorProcess &&
           m_cloudLogMonitorProcess->state() != QProcess::NotRunning;
}

bool MainWindow::cloudLogMonitorTokenAvailable() const
{
    if (m_cloudflareTokenEdit && !m_cloudflareTokenEdit->text().trimmed().isEmpty())
        return true;
    QString stored =
        forkmesh::control::cloudflareApiTokenFromVariables(ActionStore::variables());
    const bool have = !stored.isEmpty();
    stored.fill(QChar(u'\0'));
    return have;
}

QString MainWindow::cloudLogMonitorNodeRequirement() const
{
    return QStringLiteral(
               "Cloudflare live logs need Node.js %1 or newer. Install it (or "
               "point FORKMESH_NODE_BIN at a newer Node's bin directory) and "
               "restart.")
        .arg(forkmesh::control::kWranglerMinimumNodeMajor);
}

bool MainWindow::loggingEnabled() const
{
    if (!m_loggingOffLoaded) {
        m_loggingOffLoaded = true;
        m_loggingOff = QSettings().value(kLoggingOffSetting, false).toBool();
    }
    return !m_loggingOff;
}

void MainWindow::updateLoggingOffButton()
{
    if (!m_loggingOffButton)
        return;
    const bool on = loggingEnabled();
    const QSignalBlocker blocker(m_loggingOffButton);
    m_loggingOffButton->setChecked(!on);
    m_loggingOffButton->setText(on ? QStringLiteral("Stop logging")
                                   : QStringLiteral("Logging is off"));
    m_loggingOffButton->setToolTip(
        on ? QStringLiteral(
                 "Turn all logging off: nothing is recorded to the log, its "
                 "file or its charts, and the Worker, fleet node, host and "
                 "deploy log feeds stop, so logging makes no network traffic")
           : QStringLiteral("All logging is off. Click to record logs and "
                            "start the log feeds again"));
    m_loggingOffButton->setAccessibleName(
        on ? QStringLiteral("Turn all logging off")
           : QStringLiteral("Turn logging back on"));
}

void MainWindow::setLoggingEnabled(bool enabled)
{
    if (loggingEnabled() == enabled) {
        updateLoggingOffButton();
        return;
    }
    if (!enabled)
        logSystemFrom(QStringLiteral("Logs: all logging is off."),
                      forkmesh::logSourceRelativePath(__FILE__), __LINE__,
                      forkmesh::LogSeverity::Informational);
    m_loggingOff = !enabled;
    QSettings().setValue(kLoggingOffSetting, !enabled);
    updateLoggingOffButton();
    rebuildLogFilterButtons();
    if (enabled) {
        startCloudLogMonitorIfConfigured();
        syncCoreLogFeeds();
        syncHostLogFeeds();
        if (m_coreLogFeedsArmed)
            startDeployLogFollower();
        if (m_fleetInventoryTimer && !m_fleetInventoryTimer->isActive())
            scheduleFleetInventoryRefresh(
                m_fleetInventoryFetchedMs +
                forkmesh::corelogs::kVultrInventoryRefreshMs -
                QDateTime::currentMSecsSinceEpoch());
        logSystemFrom(QStringLiteral("Logs: logging is on again."),
                      forkmesh::logSourceRelativePath(__FILE__), __LINE__,
                      forkmesh::LogSeverity::Informational);
        return;
    }
    setCloudLogMonitorEnabled(false);
    stopCoreLogFeeds();
    if (m_fleetInventoryTimer)
        m_fleetInventoryTimer->stop();
    syncHostLogFeeds();
    if (m_deployLogFollower)
        m_deployLogFollower->stop();
    flushNetworkLogWrites();
}

void MainWindow::startCloudLogMonitorIfConfigured()
{
    if (m_closingDown || m_cloudLogMonitorProcess || !loggingEnabled())
        return;
    if (!QSettings().value(kCloudLogMonitorSetting, true).toBool())
        return;
    m_cloudLogMonitorIdleReason.clear();
    if (!cloudLogMonitorTokenAvailable()) {
        m_cloudLogMonitorIdleReason = cloudLogMonitorMissingTokenReason();
        updateCloudLogMonitorTooltip();
        return;
    }
    const QString missingWorker =
        QSettings().value(kCloudLogMonitorMissingWorkerSetting).toString();
    if (!missingWorker.isEmpty()) {
        const QString worker = forkmesh::control::cloudflareWorkerName(
            forkmesh::control::findCloudflareWorkerDirectory(
                QStringLiteral(FORKMESH_SOURCE_DIR),
                QCoreApplication::applicationDirPath()));
        if (worker == missingWorker) {
            m_cloudLogMonitorIdleReason =
                cloudLogMonitorMissingWorkerReason(worker);
            updateCloudLogMonitorTooltip();
            return;
        }
        QSettings().remove(kCloudLogMonitorMissingWorkerSetting);
    }
    if (m_cloudLogNodeToolchain.isValid()) {
        setCloudLogMonitorEnabled(true);
        return;
    }
    if (m_cloudLogNodeProbeRunning)
        return;
    m_cloudLogNodeProbeRunning = true;
    using ProbeWatcher = QFutureWatcher<forkmesh::control::NodeToolchain>;
    auto *watcher = new ProbeWatcher(this);
    connect(watcher, &ProbeWatcher::finished, this, [this, watcher] {
        m_cloudLogNodeProbeRunning = false;
        const forkmesh::control::NodeToolchain node = watcher->result();
        watcher->deleteLater();
        if (m_closingDown || m_cloudLogMonitorProcess || !loggingEnabled() ||
            !QSettings().value(kCloudLogMonitorSetting, true).toBool())
            return;
        if (!node.isValid()) {
            m_cloudLogMonitorIdleReason = cloudLogMonitorOldNodeReason();
            updateCloudLogMonitorTooltip();
            return;
        }
        m_cloudLogNodeToolchain = node;
        setCloudLogMonitorEnabled(true);
    });
    watcher->setFuture(QtConcurrent::run([] {
        return forkmesh::control::findNodeToolchain(
            forkmesh::control::kWranglerMinimumNodeMajor);
    }));
}

void MainWindow::stopCloudLogMonitorAfterFailure()
{
    if (m_cloudLogMonitorSettingCheck) {
        const QSignalBlocker blocker(*m_cloudLogMonitorSettingCheck);
        m_cloudLogMonitorSettingCheck->setChecked(false);
    }
    setCloudLogMonitorEnabled(false);
    updateCloudLogMonitorTooltip();
}

void MainWindow::setCloudLogMonitorEnabled(bool enabled)
{
    if (enabled && m_cloudLogMonitorProcess)
        return; // already monitoring (or still tearing the last one down)
    if (enabled && !loggingEnabled())
        return;

    if (!enabled) {
        cancelCloudLogMonitorRestart();
        m_cloudLogMonitorRestarts = 0;
        m_cloudLogMonitorBuffer.clear();
        m_cloudLogMonitorPendingRecords.clear();
        m_cloudLogMonitorDrainScheduled = false;
        if (!m_cloudLogMonitorProcess)
            return;
        releaseCloudLogMonitorProcess();
        logSystem(QStringLiteral(
                      "Cloud: stopped watching the Worker log (%1 event%2 "
                      "seen, %3 flagged).")
                      .arg(m_cloudLogMonitorEvents)
                      .arg(m_cloudLogMonitorEvents == 1 ? QString()
                                                        : QStringLiteral("s"))
                      .arg(m_cloudLogMonitorErrors));
        updateCloudLogMonitorTooltip();
        return;
    }

    cancelCloudLogMonitorRestart();
    QString token;
    QString workerDirectory;
    forkmesh::control::CloudflareBootstrapCommand command;
    if (!prepareCloudflareTail(&token, &workerDirectory, &command)) {
        stopCloudLogMonitorAfterFailure();
        return;
    }

    m_cloudLogMonitorBuffer.clear();
    m_cloudLogMonitorPendingRecords.clear();
    m_cloudLogMonitorDrainScheduled = false;
    // A restart carries the session's tallies over. Zeroing them would make the
    // CLOUD chip's counts fall back to nothing every time a tail session
    // expired, as though the Worker's traffic so far had been imagined.
    if (!m_cloudLogMonitorResuming) {
        m_cloudLogMonitorErrors = 0;
        m_cloudLogMonitorEvents = 0;
        m_cloudLogMonitorRestarts = 0;
    }
    m_cloudLogMonitorStopping = false;
    m_cloudLogMonitorIdleReason.clear();
    m_cloudLogMonitorWorkerMissing = false;
    m_cloudLogMonitorWorkerName =
        forkmesh::control::cloudflareWorkerName(workerDirectory);
    m_cloudLogMonitorProcess = new QProcess(this);
    m_cloudLogMonitorProcess->setWorkingDirectory(workerDirectory);
    m_cloudLogMonitorProcess->setProcessEnvironment(command.environment);
    m_cloudLogMonitorProcess->setProcessChannelMode(QProcess::MergedChannels);
    m_cloudLogMonitorProcess->setStandardInputFile(QProcess::nullDevice());
    connect(m_cloudLogMonitorProcess, &QProcess::readyReadStandardOutput, this,
            &MainWindow::readCloudLogMonitorOutput);
    connect(m_cloudLogMonitorProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                logSystem(QString::fromUtf8(
                    "Cloud: could not start the Worker log monitor \xE2\x80\x94 "
                    "Node.js and npx are required."));
                stopCloudLogMonitorAfterFailure();
            });
    connect(m_cloudLogMonitorProcess, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) {
                readCloudLogMonitorOutput();
                if (m_cloudLogMonitorStopping)
                    return;
                handleCloudLogMonitorEnded(
                    exitCode, m_cloudLogMonitorUptime.isValid()
                                  ? m_cloudLogMonitorUptime.elapsed()
                                  : 0);
            });
    if (!m_cloudLogMonitorResuming) {
        logSystem(QStringLiteral(
            "Cloud: watching the deployed Worker's live log."));
    }
    updateCloudLogMonitorTooltip();
    m_cloudLogMonitorUptime.start();
    m_cloudLogMonitorProcess->start(command.program, command.arguments);
    token.fill(QChar(u'\0'));
    token.clear();
}

void MainWindow::releaseCloudLogMonitorProcess()
{
    if (!m_cloudLogMonitorProcess)
        return;
    m_cloudLogMonitorStopping = true;
    if (m_cloudLogMonitorProcess->state() != QProcess::NotRunning) {
        // code-health: allow(blocking-wait) stop path; SIGTERM waits at most 1.5 s, then SIGKILL at most 1 s (already exited when called from finished)
        m_cloudLogMonitorProcess->terminate();
        if (!m_cloudLogMonitorProcess->waitForFinished(1500)) {
            m_cloudLogMonitorProcess->kill();
            m_cloudLogMonitorProcess->waitForFinished(1000);
        }
    }
    m_cloudLogMonitorProcess->setProcessEnvironment(QProcessEnvironment());
    m_cloudLogMonitorProcess->disconnect(this);
    m_cloudLogMonitorProcess->deleteLater();
    m_cloudLogMonitorProcess = nullptr;
    m_cloudLogMonitorStopping = false;
    m_cloudLogMonitorBuffer.clear();
    m_cloudLogMonitorPendingRecords.clear();
    m_cloudLogMonitorDrainScheduled = false;
    m_cloudLogMonitorUptime.invalidate();
}

void MainWindow::handleCloudLogMonitorEnded(int exitCode, qint64 uptimeMs)
{
    if (m_cloudLogMonitorWorkerMissing) {
        m_cloudLogMonitorWorkerMissing = false;
        m_cloudLogMonitorRestarts = 0;
        releaseCloudLogMonitorProcess();
        if (m_closingDown)
            return;
        QString worker = m_cloudLogMonitorWorkerName;
        if (worker.isEmpty())
            worker = forkmesh::control::cloudflareWorkerName(
                forkmesh::control::findCloudflareWorkerDirectory(
                    QStringLiteral(FORKMESH_SOURCE_DIR),
                    QCoreApplication::applicationDirPath()));
        if (worker.isEmpty())
            worker = QStringLiteral("in the Worker bundle");
        else
            QSettings().setValue(kCloudLogMonitorMissingWorkerSetting, worker);
        logSystem(QStringLiteral(
                      "Cloud: the Worker %1 is not deployed on this Cloudflare "
                      "account, so the Worker log monitor stopped and will not "
                      "retry at launch. Fleet node logs arrive under Nodes.")
                      .arg(worker));
        stopCloudLogMonitorAfterFailure();
        m_cloudLogMonitorIdleReason =
            cloudLogMonitorMissingWorkerReason(worker);
        updateCloudLogMonitorTooltip();
        return;
    }
    const forkmesh::control::CloudTailRestartPlan plan =
        forkmesh::control::planCloudflareTailRestart(m_cloudLogMonitorRestarts,
                                                     uptimeMs);
    m_cloudLogMonitorRestarts =
        uptimeMs >= forkmesh::control::kCloudTailHealthyUptimeMs
            ? 0
            : m_cloudLogMonitorRestarts + 1;
    releaseCloudLogMonitorProcess();

    if (m_closingDown)
        return;
    if (!loggingEnabled())
        return;
    if (!plan.restart ||
        !QSettings().value(kCloudLogMonitorSetting, true).toBool()) {
        m_cloudLogMonitorIdleReason =
            QStringLiteral("the Worker log tail would not stay connected");
        logSystem(QStringLiteral("Cloud: the Worker log monitor could not stay "
                                 "connected and stopped (exit %1).")
                      .arg(exitCode));
        stopCloudLogMonitorAfterFailure();
        return;
    }

    const int seconds = qMax(1, (plan.delayMs + 999) / 1000);
    m_cloudLogMonitorIdleReason =
        QString::fromUtf8("the tail ended \xC2\xB7 opening a new one in %1s")
            .arg(seconds);
    logSystem(QString::fromUtf8("Cloud: the Worker log tail ended (exit %1) "
                             "\xC2\xB7 opening a new one in %2s.")
                  .arg(exitCode)
                  .arg(seconds));
    scheduleCloudLogMonitorRestart(plan.delayMs);
    updateCloudLogMonitorTooltip();
}

void MainWindow::scheduleCloudLogMonitorRestart(int delayMs)
{
    if (!m_cloudLogMonitorRestartTimer) {
        m_cloudLogMonitorRestartTimer = new QTimer(this);
        m_cloudLogMonitorRestartTimer->setSingleShot(true);
        connect(m_cloudLogMonitorRestartTimer, &QTimer::timeout, this,
                &MainWindow::restartCloudLogMonitor);
    }
    m_cloudLogMonitorRestartTimer->start(qMax(0, delayMs));
}

void MainWindow::cancelCloudLogMonitorRestart()
{
    if (m_cloudLogMonitorRestartTimer)
        m_cloudLogMonitorRestartTimer->stop();
}

void MainWindow::restartCloudLogMonitor()
{
    if (m_closingDown || m_cloudLogMonitorProcess)
        return;
    if (!QSettings().value(kCloudLogMonitorSetting, true).toBool())
        return;
    m_cloudLogMonitorResuming = true;
    setCloudLogMonitorEnabled(true);
    m_cloudLogMonitorResuming = false;
}

void MainWindow::readCloudLogMonitorOutput()
{
    if (!m_cloudLogMonitorProcess)
        return;
    consumeCloudLogMonitorBytes(
        m_cloudLogMonitorProcess->readAllStandardOutput());
}

void MainWindow::consumeCloudLogMonitorBytes(const QByteArray &chunk)
{
    m_cloudLogMonitorBuffer += chunk;
    const QStringList records =
        forkmesh::control::takeCloudflareTailRecords(&m_cloudLogMonitorBuffer);
    for (const QString &record : records)
        m_cloudLogMonitorPendingRecords.enqueue(record);
    if (m_cloudLogMonitorPendingRecords.isEmpty() ||
        m_cloudLogMonitorDrainScheduled)
        return;
    m_cloudLogMonitorDrainScheduled = true;
    QTimer::singleShot(0, this, &MainWindow::drainCloudLogMonitorRecords);
}

void MainWindow::drainCloudLogMonitorRecords()
{
    m_cloudLogMonitorDrainScheduled = false;
    QElapsedTimer budget;
    budget.start();
    constexpr int kMaximumRecordsPerTurn = 24;
    constexpr qint64 kMaximumTurnMs = 8;
    int handled = 0;
    while (!m_cloudLogMonitorPendingRecords.isEmpty() &&
           handled < kMaximumRecordsPerTurn &&
           (handled == 0 || budget.elapsed() < kMaximumTurnMs)) {
        handleCloudLogMonitorLine(m_cloudLogMonitorPendingRecords.dequeue());
        ++handled;
    }
    if (m_cloudLogMonitorPendingRecords.isEmpty())
        return;
    m_cloudLogMonitorDrainScheduled = true;
    QTimer::singleShot(0, this, &MainWindow::drainCloudLogMonitorRecords);
}

void MainWindow::handleCloudLogMonitorLine(const QString &line)
{
    const forkmesh::control::CloudflareTailEvent event =
        forkmesh::control::parseCloudflareTailLine(line);
    if (!event.parsed && forkmesh::control::cloudflareTailWorkerMissing(line))
        m_cloudLogMonitorWorkerMissing = true;
    const QString rendered =
        forkmesh::control::redactProcessOutput(event.summary);
    if (rendered.isEmpty())
        return;
    if (event.parsed)
        ++m_cloudLogMonitorEvents;
    // Every event feeds the Usage tab's per-object ranking, including the
    // deploy handoffs and errors that return early below.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_cloudUsageTally.record(event, nowMs);
    if (event.parsed)
        forkmesh::agentfeed::publishWebEvent(event);
    if (m_logUsageRefreshTimer && m_logViewTabs && m_logUsageTabIndex >= 0 &&
        m_logViewTabs->currentIndex() == m_logUsageTabIndex &&
        !m_logUsageRefreshTimer->isActive())
        m_logUsageRefreshTimer->start();
    if (event.isDeployInterruption) {
        // A code replacement necessarily terminates hibernated Durable Object
        // WebSockets.  The runtime's exception text is the deployment signal;
        // clients already reconnect, so keep it in CLOUD without turning a
        // rollout into a red application incident.
        logSystem(QString::fromUtf8("Cloud: deploy handoff \xC2\xB7 %1")
                      .arg(rendered));
    } else if (!event.isError) {
        logSystem(QStringLiteral("Cloud hit: %1").arg(rendered));
    } else {
        ++m_cloudLogMonitorErrors;
        logSystem(QString::fromUtf8("Cloud: Worker error \xC2\xB7 %1")
                      .arg(rendered));
    }
    // Each D1 statement the invocation ran is its own DB line, straight after
    // the hit that issued it, so the DB chip filters to queries alone. A DB
    // line is read on its own once the chip filters out the hit, so it carries
    // the requester too, and its source link names the Worker line that issues
    // the statement rather than this loop.
    const QString requester = event.d1Statements.isEmpty()
        ? QString()
        : forkmesh::control::cloudflareTailRequester(event);
    for (const QString &statement : std::as_const(event.d1Statements)) {
        const forkmesh::control::D1TailStatement parsed =
            forkmesh::control::parseD1TailStatement(statement);
        QStringList fields{QStringLiteral("Cloud D1 %1: %2")
                               .arg(parsed.kind, parsed.sql)};
        if (!parsed.originFile.isEmpty()) {
            fields << (parsed.originFunction.isEmpty()
                           ? QStringLiteral("from %1").arg(parsed.originFile)
                           : QStringLiteral("from %1 in %2")
                                 .arg(parsed.originFile,
                                      parsed.originFunction));
        }
        if (!parsed.caller.isEmpty())
            fields << QStringLiteral("for %1").arg(parsed.caller);
        if (!requester.isEmpty())
            fields << requester;
        const QString text = forkmesh::control::redactProcessOutput(
            fields.join(QString::fromUtf8(" \xC2\xB7 ")));
        if (text.isEmpty())
            continue;
        const forkmesh::D1QuerySite site =
            d1LiveQuerySite(parsed.sql, parsed.originFile);
        logSystemFrom(text, site.path, site.line);
    }
    updateCloudLogMonitorTooltip();
}

// The checkout line that issues a live D1 statement. The Worker names only the
// module and function (its build's line numbers are not the checkout's), so the
// statement index supplies the line, preferring a match inside that module when
// the same SQL is written in more than one place. Answers are memoised per
// statement: a busy tail repeats a handful of queries hundreds of times.
forkmesh::D1QuerySite MainWindow::d1LiveQuerySite(const QString &sql,
                                                  const QString &originFile)
{
    static const QRegularExpression argCount(
        QStringLiteral(" \\[\\d+ args?\\]$"));
    QString query = sql;
    query.remove(argCount);
    if (query.isEmpty())
        return {};
    ensureD1QuerySourceIndex();
    if (!m_d1QuerySourceIndexReady)
        return {};
    const QString key = originFile + QLatin1Char('\n') + query;
    const auto cached = m_d1LiveQuerySites.constFind(key);
    if (cached != m_d1LiveQuerySites.constEnd())
        return cached.value();
    forkmesh::D1QuerySite site;
    if (!originFile.isEmpty()) {
        const QString suffix = QLatin1Char('/') + originFile;
        QList<forkmesh::D1QueryStatement> inModule;
        for (const forkmesh::D1QueryStatement &statement :
             std::as_const(m_d1QuerySourceIndex)) {
            if (statement.path.endsWith(suffix))
                inModule.append(statement);
        }
        site = forkmesh::matchD1QuerySite(query, inModule);
    }
    if (!site.isValid())
        site = forkmesh::matchD1QuerySite(query, m_d1QuerySourceIndex);
    if (m_d1LiveQuerySites.size() >= 2048)
        m_d1LiveQuerySites.clear();
    m_d1LiveQuerySites.insert(key, site);
    return site;
}

void MainWindow::updateCloudLogMonitorTooltip()
{
    updateCloudLogFilterChip();
}

struct MainWindow::CoreLogFeed {
    forkmesh::corelogs::CoreNode node;
    QPointer<QProcess> process;
    QByteArray stdoutTail;
    QByteArray stderrTail;
    QString handledCursor;
    QString state;
    quint64 generation = 0;
    qint64 startedMs = 0;
    int failures = 0;
    bool receiving = false;
    bool paused = false;
    bool reportedSetupError = false;

    void stop()
    {
        ++generation;
        if (process)
            process->kill();
    }
};

namespace {

constexpr int kCoreLogRetryBaseMs = 30 * 1000;
constexpr int kCoreLogRetryMaxMs = 10 * 60 * 1000;
constexpr qint64 kCoreLogHealthyMs = 3 * 60 * 1000;
constexpr int kCoreLogPendingHigh = 20000;
constexpr int kCoreLogPendingLow = 2000;
constexpr qint64 kCoreLogCursorSaveMs = 5000;
constexpr qint64 kFleetInventoryPoolChangeDelayMs = 2 * 60 * 1000;
constexpr qint64 kIpCountryMaxAgeMs = 30LL * 24 * 60 * 60 * 1000;
constexpr qint64 kIpCountryRecheckMs = 24LL * 60 * 60 * 1000;
constexpr qint64 kIpCountryRetryMs = 60LL * 60 * 1000;
constexpr int kAccessWhoisParallel = 2;
constexpr int kAccessWhoisQueueMax = 64;
constexpr int kAccessWhoisCacheMax = 4096;
constexpr int kAccessWhoisTimeoutMs = 20000;

QString ipCountryDir()
{
    return forkmesh::paths::cacheRoot() + QStringLiteral("/ip-country");
}

QString coreLogCursorPath()
{
    return forkmesh::paths::dataPath(QStringLiteral("fleet_logs")) +
           QStringLiteral("/cursors.json");
}

QString fleetInventoryPath()
{
    return forkmesh::paths::dataPath(QStringLiteral("fleet_logs")) +
           QStringLiteral("/vultr-inventory.json");
}

QString fleetLineBadge(const QString &text)
{
    const QString badge = forkmesh::corelogs::nodeLineBadge(text);
    return badge.isEmpty() ? QStringLiteral("CLOUD") : badge;
}

} // namespace

void MainWindow::syncCoreLogFeeds()
{
#ifdef FORKMESH_WINDOW_TESTS
    return;
#else
    if (!m_coreLogFeedsArmed || m_closingDown)
        return;
    namespace corelogs = forkmesh::corelogs;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!loggingEnabled()) {
        stopCoreLogFeeds();
        if (m_fleetInventoryTimer)
            m_fleetInventoryTimer->stop();
        m_coreLogIdleReason =
            QStringLiteral("All logging is switched off on the Logs page.");
        return;
    }
    if (!QSettings().value(kCoreLogFeedSetting, true).toBool()) {
        stopCoreLogFeeds();
        m_coreLogIdleReason = QStringLiteral(
            "Following the fleet nodes' logs is switched off in Settings.");
        return;
    }
    if (!m_coreLogCursorsLoaded) {
        m_coreLogCursorsLoaded = true;
        m_coreLogCursors = corelogs::loadCursors(coreLogCursorPath());
    }
    armFleetInventoryWatch();
    const QList<corelogs::CoreNode> nodes =
        corelogs::mergeFleetNodes(corelogs::loadFleetNodes(), m_vultrFleetNodes);
    if (nodes.isEmpty()) {
        stopCoreLogFeeds();
        m_coreLogIdleReason =
            QStringLiteral("No fleet nodes are listed in %1.")
                .arg(corelogs::fleetInstancesPath());
        return;
    }
    m_coreLogIdleReason.clear();
    QSet<QString> wanted;
    for (const corelogs::CoreNode &node : nodes) {
        wanted.insert(node.name);
        const QSharedPointer<CoreLogFeed> existing =
            m_coreLogFeeds.value(node.name);
        if (existing && existing->node.address == node.address) {
            existing->node = node;
            continue;
        }
        if (existing)
            existing->stop();
        auto feed = QSharedPointer<CoreLogFeed>::create();
        feed->node = node;
        feed->handledCursor = m_coreLogCursors.value(node.name);
        m_coreLogFeeds.insert(node.name, feed);
        m_coreLogTally.noteFeedState(node.name, false,
                                     QStringLiteral("connecting"), now);
        launchCoreLogFeed(node.name);
    }
    for (auto it = m_coreLogFeeds.begin(); it != m_coreLogFeeds.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        it.value()->stop();
        m_coreLogTally.noteFeedState(it.key(), false,
                                     QStringLiteral("left the fleet"), now);
        logSystemFrom(corelogs::feedNoteLine(it.value()->node,
                                             QStringLiteral("left the fleet; "
                                                            "no longer followed")),
                      forkmesh::logSourceRelativePath(FORKMESH_LOG_SOURCE_FILE),
                      FORKMESH_LOG_SOURCE_LINE, forkmesh::LogSeverity::Automatic,
                      QStringLiteral("CLOUD"));
        m_coreLogCursors.remove(it.key());
        m_coreLogCursorsDirty = true;
        it = m_coreLogFeeds.erase(it);
    }
#endif
}

void MainWindow::armFleetInventoryWatch()
{
    namespace corelogs = forkmesh::corelogs;
    if (!m_fleetInventoryWatcher) {
        m_fleetInventoryWatcher = new QFileSystemWatcher(this);
        connect(m_fleetInventoryWatcher, &QFileSystemWatcher::fileChanged, this,
                [this] { QTimer::singleShot(500, this, [this] {
                             armFleetInventoryWatch();
                             syncCoreLogFeeds();
                         }); });
        connect(m_fleetInventoryWatcher, &QFileSystemWatcher::directoryChanged,
                this, [this] { armFleetInventoryWatch(); });
        m_fleetInventoryTimer = new QTimer(this);
        m_fleetInventoryTimer->setSingleShot(true);
        connect(m_fleetInventoryTimer, &QTimer::timeout, this,
                &MainWindow::refreshFleetInventoryFromVultr);
        const corelogs::VultrInventory cached =
            corelogs::loadVultrInventory(fleetInventoryPath());
        m_vultrFleetNodes = cached.nodes;
        m_fleetInventoryFetchedMs = cached.fetchedMs;
        m_fleetInventoryRetryAtMs = cached.retryAtMs;
        m_fleetInventoryFailures = cached.failures;
        scheduleFleetInventoryRefresh(corelogs::vultrInventoryDueMs(cached) -
                                      QDateTime::currentMSecsSinceEpoch());
    }
    const QString file = corelogs::fleetInstancesPath();
    const QString dir = QFileInfo(file).absolutePath();
    if (QFileInfo::exists(dir) &&
        !m_fleetInventoryWatcher->directories().contains(dir))
        m_fleetInventoryWatcher->addPath(dir);
    for (const QString &watched : {file, corelogs::autoscaledInstancesPath()}) {
        if (QFileInfo::exists(watched) &&
            !m_fleetInventoryWatcher->files().contains(watched))
            m_fleetInventoryWatcher->addPath(watched);
    }
}

void MainWindow::scheduleFleetInventoryRefresh(qint64 delayMs)
{
    if (!m_fleetInventoryTimer || m_closingDown || !loggingEnabled())
        return;
    delayMs = qBound<qint64>(
        0, delayMs, forkmesh::corelogs::kVultrInventoryRetryAfterMaxMs);
    if (m_fleetInventoryTimer->isActive() &&
        m_fleetInventoryTimer->remainingTime() <= delayMs)
        return;
    m_fleetInventoryTimer->start(int(delayMs));
}

void MainWindow::saveFleetInventory()
{
    forkmesh::corelogs::VultrInventory inventory;
    inventory.fetchedMs = m_fleetInventoryFetchedMs;
    inventory.retryAtMs = m_fleetInventoryRetryAtMs;
    inventory.failures = m_fleetInventoryFailures;
    inventory.nodes = m_vultrFleetNodes;
    forkmesh::corelogs::saveVultrInventory(fleetInventoryPath(), inventory);
}

void MainWindow::refreshFleetInventoryFromVultr()
{
#ifndef FORKMESH_WINDOW_TESTS
    namespace corelogs = forkmesh::corelogs;
    if (m_closingDown || m_fleetInventoryInFlight || !m_networkAccess ||
        !loggingEnabled())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_fleetInventoryFailures > 0 && now < m_fleetInventoryRetryAtMs) {
        scheduleFleetInventoryRefresh(m_fleetInventoryRetryAtMs - now);
        return;
    }
    const QString key = corelogs::vultrApiKey();
    if (key.isEmpty()) {
        scheduleFleetInventoryRefresh(corelogs::kVultrInventoryRefreshMs);
        return;
    }
    m_fleetInventoryInFlight = true;
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://api.vultr.com/v2/instances?per_page=500&tag=forkmesh-next")));
    request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    request.setTransferTimeout(20000);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_fleetInventoryInFlight = false;
        if (m_closingDown)
            return;
        const qint64 finishedMs = QDateTime::currentMSecsSinceEpoch();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            bool retryAfterOk = false;
            const qint64 retryAfterSeconds =
                QString::fromLatin1(reply->rawHeader("Retry-After"))
                    .trimmed()
                    .toLongLong(&retryAfterOk);
            m_fleetInventoryFailures = qMin(m_fleetInventoryFailures + 1, 64);
            const qint64 delay = corelogs::vultrInventoryRetryDelayMs(
                m_fleetInventoryFailures,
                retryAfterOk && retryAfterSeconds > 0 ? retryAfterSeconds * 1000
                                                      : 0);
            m_fleetInventoryRetryAtMs = finishedMs + delay;
            saveFleetInventory();
            scheduleFleetInventoryRefresh(delay);
            return;
        }
        const QList<corelogs::CoreNode> nodes =
            corelogs::parseVultrInstances(reply->readAll());
        QStringList before;
        for (const auto &node : std::as_const(m_vultrFleetNodes))
            before.append(node.name + QLatin1Char('@') + node.address);
        QStringList after;
        for (const auto &node : nodes)
            after.append(node.name + QLatin1Char('@') + node.address);
        m_vultrFleetNodes = nodes;
        m_fleetInventoryFetchedMs = finishedMs;
        m_fleetInventoryRetryAtMs = 0;
        m_fleetInventoryFailures = 0;
        saveFleetInventory();
        scheduleFleetInventoryRefresh(corelogs::kVultrInventoryRefreshMs);
        if (before != after) {
            syncCoreLogFeeds();
            if (m_nodesTable && m_nodesTable->isVisible())
                refreshNodesTable();
        }
    });
#endif
}

void MainWindow::stopCoreLogFeeds()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_coreLogFeeds.cbegin(); it != m_coreLogFeeds.cend(); ++it) {
        it.value()->stop();
        m_coreLogTally.noteFeedState(it.key(), false, QStringLiteral("stopped"),
                                     now);
    }
    m_coreLogFeeds.clear();
    m_coreLogPending.clear();
    m_coreLogDrainScheduled = false;
    saveCoreLogCursors(true);
}

void MainWindow::startDeployLogFollower()
{
#ifdef FORKMESH_WINDOW_TESTS
    return;
#else
    if (m_closingDown)
        return;
    if (!loggingEnabled()) {
        if (m_deployLogFollower)
            m_deployLogFollower->stop();
        return;
    }
    if (!m_deployLogFollower) {
        m_deployLogFollower = new forkmesh::deploylogs::DeployLogFollower(
            forkmesh::paths::dataPath(QStringLiteral("fleet_logs")) +
                QStringLiteral("/deploy_cursor.json"),
            [this](const QString &text, const QString &category) {
                logSystemFrom(
                    forkmesh::control::redactProcessOutput(text),
                    forkmesh::logSourceRelativePath(FORKMESH_LOG_SOURCE_FILE),
                    FORKMESH_LOG_SOURCE_LINE,
                    category == QLatin1String("ERROR")
                        ? forkmesh::LogSeverity::Error
                        : forkmesh::LogSeverity::Informational,
                    category);
            },
            this);
    }
    m_deployLogFollower->start();
#endif
}

void MainWindow::saveCoreLogCursors(bool force)
{
    if (!m_coreLogCursorsDirty)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_closingDown) {
        m_coreLogCursorSaveFuture.waitForFinished();
        m_coreLogCursorsSavedMs = now;
        m_coreLogCursorsDirty = false;
        forkmesh::corelogs::saveCursors(coreLogCursorPath(), m_coreLogCursors);
        return;
    }
    if (m_coreLogCursorSaveFuture.isRunning() ||
        (!force && now - m_coreLogCursorsSavedMs < kCoreLogCursorSaveMs)) {
        if (!m_coreLogCursorSaveArmed) {
            m_coreLogCursorSaveArmed = true;
            QTimer::singleShot(int(kCoreLogCursorSaveMs), this, [this] {
                m_coreLogCursorSaveArmed = false;
                saveCoreLogCursors(true);
            });
        }
        return;
    }
    m_coreLogCursorsSavedMs = now;
    m_coreLogCursorsDirty = false;
    m_coreLogCursorSaveFuture = QtConcurrent::run(
        [path = coreLogCursorPath(), cursors = m_coreLogCursors] {
            return forkmesh::corelogs::saveCursors(path, cursors);
        });
}

void MainWindow::launchCoreLogFeed(const QString &node)
{
    const QSharedPointer<CoreLogFeed> feed = m_coreLogFeeds.value(node);
    if (!feed || m_closingDown ||
        (feed->process && feed->process->state() != QProcess::NotRunning))
        return;
    feed->paused = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const forkmesh::corelogs::FeedCommand command =
        forkmesh::corelogs::buildFeedCommand(feed->node, feed->handledCursor);
    if (command.program.isEmpty()) {
        m_coreLogIdleReason =
            QStringLiteral("Cannot follow the fleet nodes' logs: %1.")
                .arg(command.error);
        m_coreLogTally.noteFeedState(node, false, command.error, now);
        if (!feed->reportedSetupError) {
            feed->reportedSetupError = true;
            logSystemFrom(forkmesh::corelogs::feedNoteLine(
                              feed->node,
                              QStringLiteral("cannot follow the journal: ") +
                                  command.error),
                          forkmesh::logSourceRelativePath(
                              FORKMESH_LOG_SOURCE_FILE),
                          FORKMESH_LOG_SOURCE_LINE,
                          forkmesh::LogSeverity::Automatic,
                          QStringLiteral("CLOUD"));
        }
        return;
    }
    const quint64 generation = ++feed->generation;
    feed->stdoutTail.clear();
    feed->stderrTail.clear();
    feed->receiving = false;
    feed->startedMs = now;
    feed->state.clear();

    const auto ended = [this, node, feed, generation](const QString &how) {
        if (feed->generation != generation)
            return;
        ++feed->generation;
        feed->process = nullptr;
        if (m_closingDown || m_coreLogFeeds.value(node) != feed)
            return;
        const qint64 at = QDateTime::currentMSecsSinceEpoch();
        feed->failures =
            at - feed->startedMs >= kCoreLogHealthyMs ? 0 : feed->failures + 1;
        const int delay = int(qMin<qint64>(
            qint64(kCoreLogRetryBaseMs) << qMin(feed->failures, 5),
            kCoreLogRetryMaxMs));
        const QString why = feed->state.isEmpty() ? how : feed->state;
        m_coreLogTally.noteFeedState(
            node, false,
            QStringLiteral("retrying in %1 s (%2)").arg(delay / 1000).arg(why),
            at);
        logSystemFrom(
            forkmesh::corelogs::feedNoteLine(
                feed->node,
                QStringLiteral("journal stream closed (%1); reconnecting in "
                               "%2 s")
                    .arg(why)
                    .arg(delay / 1000)),
            forkmesh::logSourceRelativePath(FORKMESH_LOG_SOURCE_FILE),
            FORKMESH_LOG_SOURCE_LINE, forkmesh::LogSeverity::Automatic,
            QStringLiteral("CLOUD"));
        scheduleCoreLogD1Refresh();
        QTimer::singleShot(delay, this,
                           [this, node] { launchCoreLogFeed(node); });
    };

    auto *proc = new QProcess(this);
    feed->process = proc;
    proc->setStandardInputFile(QProcess::nullDevice());
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this, node] { readCoreLogFeedOutput(node); });
    connect(proc, &QProcess::readyReadStandardError, this,
            [this, feed, proc] {
                feed->stderrTail.append(proc->readAllStandardError());
                int start = 0;
                for (int newline = feed->stderrTail.indexOf('\n');
                     newline >= 0;
                     newline = feed->stderrTail.indexOf('\n', start)) {
                    const QString text =
                        QString::fromUtf8(feed->stderrTail.mid(
                                              start, newline - start))
                            .trimmed();
                    start = newline + 1;
                    if (text.isEmpty())
                        continue;
                    const QString state =
                        forkmesh::corelogs::readerFailureState(text);
                    if (!state.isEmpty())
                        feed->state = state;
                    logSystemFrom(
                        forkmesh::control::redactProcessOutput(
                            forkmesh::corelogs::feedNoteLine(
                                feed->node, QStringLiteral("[ssh] ") + text)),
                        forkmesh::logSourceRelativePath(
                            FORKMESH_LOG_SOURCE_FILE),
                        FORKMESH_LOG_SOURCE_LINE,
                        forkmesh::LogSeverity::Automatic,
                        QStringLiteral("CLOUD"));
                }
                feed->stderrTail.remove(0, start);
                if (feed->stderrTail.size() > 64 * 1024)
                    feed->stderrTail.clear();
            });
    connect(proc, &QProcess::errorOccurred, this,
            [proc, ended](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                proc->deleteLater();
                ended(QStringLiteral("ssh could not start"));
            });
    connect(proc, &QProcess::finished, this,
            [this, node, proc, ended](int code, QProcess::ExitStatus) {
                readCoreLogFeedOutput(node);
                proc->deleteLater();
                ended(QStringLiteral("exit %1").arg(code));
            });
    m_coreLogTally.noteFeedState(node, false, QStringLiteral("connecting"),
                                 now);
    proc->start(command.program, command.arguments);
}

void MainWindow::readCoreLogFeedOutput(const QString &node)
{
    const QSharedPointer<CoreLogFeed> feed = m_coreLogFeeds.value(node);
    if (!feed || !feed->process)
        return;
    feed->stdoutTail.append(feed->process->readAllStandardOutput());
    int start = 0;
    bool queued = false;
    for (int newline = feed->stdoutTail.indexOf('\n'); newline >= 0;
         newline = feed->stdoutTail.indexOf('\n', start)) {
        const QByteArray line = feed->stdoutTail.mid(start, newline - start);
        start = newline + 1;
        if (line.trimmed().isEmpty())
            continue;
        m_coreLogPending.enqueue({node, line});
        queued = true;
    }
    feed->stdoutTail.remove(0, start);
    if (feed->stdoutTail.size() > 1024 * 1024)
        feed->stdoutTail.clear();
    if (queued && !feed->receiving) {
        feed->receiving = true;
        m_coreLogTally.noteFeedState(node, true, QStringLiteral("following"),
                                     QDateTime::currentMSecsSinceEpoch());
    }
    if (m_coreLogPending.size() > kCoreLogPendingHigh) {
        for (auto it = m_coreLogFeeds.cbegin(); it != m_coreLogFeeds.cend();
             ++it) {
            const QSharedPointer<CoreLogFeed> &other = it.value();
            if (!other->process || other->paused)
                continue;
            other->paused = true;
            other->stop();
            other->process = nullptr;
            m_coreLogTally.noteFeedState(
                it.key(), false,
                QStringLiteral("paused while the log catches up"),
                QDateTime::currentMSecsSinceEpoch());
        }
    }
    if (m_coreLogPending.isEmpty() || m_coreLogDrainScheduled)
        return;
    m_coreLogDrainScheduled = true;
    QTimer::singleShot(0, this, &MainWindow::drainCoreLogFeedLines);
}

void MainWindow::drainCoreLogFeedLines()
{
    m_coreLogDrainScheduled = false;
    QElapsedTimer budget;
    budget.start();
    constexpr int kMaximumLinesPerTurn = 48;
    constexpr qint64 kMaximumTurnMs = 8;
    int handled = 0;
    while (!m_coreLogPending.isEmpty() && handled < kMaximumLinesPerTurn &&
           (handled == 0 || budget.elapsed() < kMaximumTurnMs)) {
        const QPair<QString, QByteArray> next = m_coreLogPending.dequeue();
        handleCoreLogFeedLine(next.first, next.second);
        ++handled;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const auto &stale : m_coreLogTracebacks.takeStale(now))
        emitCoreLogEntry(stale.first, stale.second);
    if (m_coreLogTracebacks.pending() && !m_coreLogTracebackTimerArmed) {
        m_coreLogTracebackTimerArmed = true;
        QTimer::singleShot(2500, this, [this] {
            m_coreLogTracebackTimerArmed = false;
            drainCoreLogFeedLines();
        });
    }
    saveCoreLogCursors(false);
    if (m_coreLogPending.size() < kCoreLogPendingLow) {
        for (auto it = m_coreLogFeeds.cbegin(); it != m_coreLogFeeds.cend();
             ++it)
            if (it.value()->paused)
                launchCoreLogFeed(it.key());
    }
    if (m_coreLogPending.isEmpty())
        return;
    m_coreLogDrainScheduled = true;
    QTimer::singleShot(0, this, &MainWindow::drainCoreLogFeedLines);
}

void MainWindow::noteCoreLogWebRequestCounted()
{
    if (m_networkWebRequestsRefreshTimer && m_networkTabs &&
        m_networkTabs->currentIndex() == m_networkWebRequestsTabIndex &&
        m_sectionStack &&
        m_sectionStack->currentIndex() == kNetworkDiagnosticsSectionIndex &&
        !m_networkWebRequestsRefreshTimer->isActive())
        m_networkWebRequestsRefreshTimer->start();
}

void MainWindow::handleCoreLogFeedLine(const QString &node,
                                       const QByteArray &line)
{
    namespace corelogs = forkmesh::corelogs;
    const QSharedPointer<CoreLogFeed> feed = m_coreLogFeeds.value(node);
    corelogs::CoreNode who;
    if (feed)
        who = feed->node;
    else
        who.name = node;
    const corelogs::ReaderControl control = corelogs::parseReaderControl(line);
    if (control.kind != corelogs::ReaderControl::None) {
        if (control.kind == corelogs::ReaderControl::Vitals) {
            if (m_fleetVisualizer)
                m_fleetVisualizer->noteVitals(
                    node, corelogs::parseNodeVitals(line),
                    QDateTime::currentMSecsSinceEpoch());
        } else if (control.kind == corelogs::ReaderControl::Failure) {
            if (feed)
                feed->state = QStringLiteral("reader: ") + control.error;
            logSystemFrom(corelogs::feedNoteLine(
                              who, QStringLiteral("the node's log reader "
                                                  "failed: ") +
                                       control.error),
                          forkmesh::logSourceRelativePath(
                              FORKMESH_LOG_SOURCE_FILE),
                          FORKMESH_LOG_SOURCE_LINE,
                          forkmesh::LogSeverity::Automatic,
                          QStringLiteral("ERROR"));
        } else if (control.kind == corelogs::ReaderControl::Header &&
                   !control.node.isEmpty() && control.node != who.name) {
            logSystemFrom(corelogs::feedNoteLine(
                              who, QStringLiteral("the node calls itself %1; "
                                                  "its lines stay under %2")
                                       .arg(control.node, who.name)),
                          forkmesh::logSourceRelativePath(
                              FORKMESH_LOG_SOURCE_FILE),
                          FORKMESH_LOG_SOURCE_LINE,
                          forkmesh::LogSeverity::Automatic,
                          QStringLiteral("CLOUD"));
        }
        return;
    }
    const corelogs::JournalEntry entry = corelogs::parseJournalLine(line);
    if (!entry.valid)
        return;
    if (feed && forkmesh::corelogs::validJournalCursor(entry.cursor)) {
        feed->handledCursor = entry.cursor;
        m_coreLogCursors.insert(node, entry.cursor);
        m_coreLogCursorsDirty = true;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString source = corelogs::journalSource(entry);
    if (corelogs::isPoolChange(entry))
        scheduleFleetInventoryRefresh(kFleetInventoryPoolChangeDelayMs);
    corelogs::AccessRecord access = corelogs::parseAccessRecord(entry, source);
    if (access.valid) {
        const corelogs::ClientDetails client =
            logClientDetails(access.clientIp, access.country.isEmpty(),
                             access.network.isEmpty());
        if (access.country.isEmpty())
            access.country = client.country;
        access.clientLabel = client.label;
        if (access.network.isEmpty())
            access.network = client.network;
        m_coreLogAccessNodes.insert(node);
        const bool failed = access.status >= 500 && !access.expectedDegraded;
        m_coreLogTally.noteLine(node, source, true, false, failed, now);
        const QString site = corelogs::accessSiteGroup(access);
        const QString group =
            site.isEmpty() ? forkmesh::control::webRequestRouteGroup(access.path)
                           : site;
        if (m_webRequestTally.recordBatch(
                group, corelogs::statusClassOf(access.status), 1,
                access.durationMs, access.durationMs,
                access.atMs > 0 ? access.atMs : now, now))
            noteCoreLogWebRequestCounted();
        const bool backlog = corelogs::isBacklogEntry(entry, now);
        const QString unit = corelogs::unitLabel(entry);
        m_coreLogUnitsSeen[node].insert(unit);
        const QString text = forkmesh::control::redactProcessOutput(
            corelogs::nodeLogLine(who, unit,
                                  failed ? QStringLiteral("err")
                                  : access.status >= 500 ? QStringLiteral("warn")
                                                         : QStringLiteral("info"),
                                  corelogs::accessLogMessage(access)) +
            (backlog ? corelogs::backlogNote(entry) : QString()));
        logSystemFrom(text,
                      forkmesh::logSourceRelativePath(FORKMESH_LOG_SOURCE_FILE),
                      FORKMESH_LOG_SOURCE_LINE,
                      forkmesh::LogSeverity::Automatic,
                      fleetLineBadge(text));
        return;
    }
    const corelogs::WebRequestBatch batch =
        corelogs::parseWebRequestBatch(entry, source);
    if (batch.valid) {
        m_coreLogTally.noteLine(node, source, false, false, false, now);
        if (m_coreLogAccessNodes.contains(node))
            return;
        bool counted = false;
        for (const corelogs::WebRequestRow &row : batch.rows)
            counted |= m_webRequestTally.recordBatch(
                row.group, row.statusClass, row.count, row.durationSumMs,
                row.durationMaxMs, batch.windowStartMs, now);
        if (counted)
            noteCoreLogWebRequestCounted();
        return;
    }
    const corelogs::StatementDone done =
        corelogs::parseStatementDone(entry, source);
    if (done.valid) {
        m_coreLogTally.noteLine(node, source, false, false, false, now);
        if (m_queryInspector)
            m_queryInspector->noteDone(node, done, now);
        return;
    }
    const corelogs::StatementLine statement =
        corelogs::parseStatementLine(entry, source);
    if (statement.valid) {
        m_coreLogTally.noteLine(node, source, true, false, false, now);
        m_coreLogTally.noteStatement(node, statement, now);
        if (m_fleetVisualizer)
            m_fleetVisualizer->noteStatement(node, statement.sql,
                                             statement.originFile, now);
        if (m_queryInspector)
            m_queryInspector->noteStatement(node, statement, now);
        const QString text = forkmesh::control::redactProcessOutput(
            withLogClientDetails(corelogs::mainLogStatementLine(who, statement)));
        const forkmesh::D1QuerySite site =
            d1LiveQuerySite(statement.sql, statement.originFile);
        logSystemFrom(text, site.path, site.line,
                      forkmesh::LogSeverity::Automatic, QStringLiteral("DB"));
        scheduleCoreLogD1Refresh();
        return;
    }
    for (const corelogs::JournalEntry &ready :
         m_coreLogTracebacks.feed(node, entry, now))
        emitCoreLogEntry(node, ready);
}

void MainWindow::ensureIpCountryTable()
{
#ifdef FORKMESH_WINDOW_TESTS
    return;
#else
    namespace ipcountry = forkmesh::ipcountry;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_ipCountryBusy || !m_networkAccess || m_closingDown ||
        (m_ipCountryCheckedMs > 0 &&
         now - m_ipCountryCheckedMs <
             (m_ipCountryTable ? kIpCountryRecheckMs : kIpCountryRetryMs)))
        return;
    m_ipCountryCheckedMs = now;
    const QString dir = ipCountryDir();
    QList<bool> stale;
    for (bool ipv6 : {false, true}) {
        const QFileInfo file(dir + QLatin1Char('/') + ipcountry::csvFileName(ipv6));
        if (!file.exists() || file.size() == 0 ||
            now - file.lastModified().toMSecsSinceEpoch() > kIpCountryMaxAgeMs)
            stale.append(ipv6);
    }
    if (stale.isEmpty()) {
        if (!m_ipCountryTable)
            loadIpCountryTable();
        return;
    }
    QDir().mkpath(dir);
    m_ipCountryBusy = true;
    auto remaining = std::make_shared<int>(int(stale.size()));
    for (bool ipv6 : stale) {
        QNetworkRequest request{QUrl(ipcountry::csvUrl(ipv6))};
        request.setTransferTimeout(180000);
        QNetworkReply *reply = m_networkAccess->get(request);
        const QString path = dir + QLatin1Char('/') + ipcountry::csvFileName(ipv6);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, path, remaining] {
                    reply->deleteLater();
                    const int status =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                            .toInt();
                    if (reply->error() == QNetworkReply::NoError && status == 200) {
                        const QByteArray body = reply->readAll();
                        if (ipcountry::csvLooksValid(body)) {
                            QSaveFile out(path);
                            if (out.open(QIODevice::WriteOnly) &&
                                out.write(body) == body.size())
                                out.commit();
                        }
                    }
                    if (--*remaining == 0)
                        loadIpCountryTable();
                });
    }
#endif
}

forkmesh::corelogs::ClientDetails
MainWindow::logClientDetails(const QString &address, bool wantCountry,
                             bool wantNetwork)
{
    namespace corelogs = forkmesh::corelogs;
    corelogs::ClientDetails details;
    ensureIpCountryTable();
    if (wantCountry && m_ipCountryTable)
        details.country = forkmesh::ipcountry::lookup(*m_ipCountryTable, address);
    QList<corelogs::CoreNode> fleet;
    fleet.reserve(m_coreLogFeeds.size());
    for (const auto &known : std::as_const(m_coreLogFeeds))
        fleet.append(known->node);
    details.label = corelogs::accessClientLabel(address, fleet);
    if (wantNetwork && details.label.isEmpty())
        details.network = accessWhoisLabel(address);
    return details;
}

QString MainWindow::withLogClientDetails(const QString &text)
{
    return forkmesh::corelogs::withClientDetails(
        text, [this](const QString &address) {
            return logClientDetails(address, true, true);
        });
}

QString MainWindow::accessWhoisLabel(const QString &address)
{
#ifdef FORKMESH_WINDOW_TESTS
    Q_UNUSED(address);
    return {};
#else
    namespace ipwhois = forkmesh::ipwhois;
    if (!ipwhois::worthLookingUp(address))
        return {};
    const QString key = ipwhois::cacheKey(address);
    const auto known = m_accessWhois.constFind(key);
    if (known != m_accessWhois.cend())
        return ipwhois::ownerLabel(*known);
    static const bool installed =
        !QStandardPaths::findExecutable(QStringLiteral("whois")).isEmpty();
    if (!installed || m_accessWhoisPending.contains(key) ||
        m_accessWhoisQueue.size() >= kAccessWhoisQueueMax ||
        m_accessWhois.size() >= kAccessWhoisCacheMax)
        return {};
    m_accessWhoisPending.insert(key);
    m_accessWhoisQueue.append(address);
    startAccessWhois();
    return {};
#endif
}

void MainWindow::startAccessWhois()
{
    namespace ipwhois = forkmesh::ipwhois;
    while (!m_closingDown && m_accessWhoisRunning < kAccessWhoisParallel &&
           !m_accessWhoisQueue.isEmpty()) {
        const QString address = m_accessWhoisQueue.takeFirst();
        ++m_accessWhoisRunning;
        auto *process = new QProcess(this);
        process->setProcessChannelMode(QProcess::MergedChannels);
        QTimer::singleShot(kAccessWhoisTimeoutMs, process, [process] { process->kill(); });
        const auto done = [this, process, address](bool ran) {
            const QString text =
                ran ? QString::fromUtf8(process->readAll()) : QString();
            process->deleteLater();
            --m_accessWhoisRunning;
            const QString key = ipwhois::cacheKey(address);
            m_accessWhoisPending.remove(key);
            const ipwhois::Info info = ipwhois::parse(text);
            m_accessWhois.insert(key, info);
            if (!info.isEmpty() && !m_closingDown)
                logSystemFrom(forkmesh::control::redactProcessOutput(
                                  ipwhois::describe(address, info)),
                              forkmesh::logSourceRelativePath(
                                  FORKMESH_LOG_SOURCE_FILE),
                              FORKMESH_LOG_SOURCE_LINE,
                              forkmesh::LogSeverity::Automatic,
                              QStringLiteral("CLOUD"));
            startAccessWhois();
        };
        connect(process, &QProcess::finished, this,
                [done](int, QProcess::ExitStatus) { done(true); });
        connect(process, &QProcess::errorOccurred, this,
                [done](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart)
                        done(false);
                });
        process->start(QStringLiteral("whois"), {address});
    }
}

void MainWindow::loadIpCountryTable()
{
    namespace ipcountry = forkmesh::ipcountry;
    m_ipCountryBusy = true;
    using TableWatcher = QFutureWatcher<QSharedPointer<const ipcountry::Table>>;
    auto *watcher = new TableWatcher(this);
    connect(watcher, &TableWatcher::finished, this, [this, watcher] {
        const QSharedPointer<const ipcountry::Table> table = watcher->result();
        if (table && !table->isEmpty())
            m_ipCountryTable = table;
        m_ipCountryBusy = false;
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([dir = ipCountryDir()] {
        auto table = QSharedPointer<ipcountry::Table>::create();
        for (bool ipv6 : {false, true}) {
            QFile file(dir + QLatin1Char('/') + ipcountry::csvFileName(ipv6));
            if (file.open(QIODevice::ReadOnly))
                ipcountry::appendCsv(*table, file.readAll());
        }
        ipcountry::finish(*table);
        return QSharedPointer<const ipcountry::Table>(table);
    }));
}

void MainWindow::emitCoreLogEntry(const QString &node,
                                  const forkmesh::corelogs::JournalEntry &entry)
{
    namespace corelogs = forkmesh::corelogs;
    const QSharedPointer<CoreLogFeed> feed = m_coreLogFeeds.value(node);
    corelogs::CoreNode who;
    if (feed)
        who = feed->node;
    else
        who.name = node;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString source = corelogs::journalSource(entry);
    const corelogs::Classified classified = corelogs::classifyEntry(entry, source);
    const bool error = classified.severity == corelogs::Severity::Error;
    m_coreLogUnitsSeen[node].insert(classified.unit);
    if (!classified.forward) {
        m_coreLogTally.noteLine(node, source, false, false, error, now);
        return;
    }
    const QString message = corelogs::displayMessage(entry);
    const corelogs::RepeatVerdict verdict =
        m_coreLogRepeats.admit(node, source, message, now);
    m_coreLogTally.noteLine(node, source, verdict.admit, !verdict.admit, error,
                            now);
    if (!verdict.admit)
        return;
    const bool backlog = corelogs::isBacklogEntry(entry, now);
    const QString text = forkmesh::control::redactProcessOutput(
        corelogs::nodeLogLine(who, classified.unit,
                              corelogs::severityWord(classified.severity),
                              withLogClientDetails(message),
                              verdict.foldedRepeats) +
        (backlog ? corelogs::backlogNote(entry) : QString()));
    logSystemFrom(text, forkmesh::logSourceRelativePath(FORKMESH_LOG_SOURCE_FILE),
                  FORKMESH_LOG_SOURCE_LINE, forkmesh::LogSeverity::Automatic,
                  fleetLineBadge(text));
    scheduleCoreLogD1Refresh();
}

void MainWindow::scheduleCoreLogD1Refresh()
{
    if (!m_logD1UsageView || !m_logViewTabs || m_logD1UsageTabIndex < 0 ||
        m_logViewTabs->currentIndex() != m_logD1UsageTabIndex ||
        !m_logD1UsageView->isVisible())
        return;
    if (!m_coreLogD1RefreshTimer) {
        m_coreLogD1RefreshTimer = new QTimer(this);
        m_coreLogD1RefreshTimer->setSingleShot(true);
        m_coreLogD1RefreshTimer->setInterval(2000);
        connect(m_coreLogD1RefreshTimer, &QTimer::timeout, this,
                &MainWindow::refreshLogD1UsageView);
    }
    if (!m_coreLogD1RefreshTimer->isActive())
        m_coreLogD1RefreshTimer->start();
}

void MainWindow::updateNodeLogFilterButton()
{
    if (!m_nodeLogFilterButton)
        return;
    QStringList parts;
    if (!m_nodeLogFilter.nodes.isEmpty()) {
        QStringList nodes(m_nodeLogFilter.nodes.cbegin(),
                          m_nodeLogFilter.nodes.cend());
        std::sort(nodes.begin(), nodes.end());
        parts.append(nodes.join(QLatin1Char(',')));
    }
    if (!m_nodeLogFilter.units.isEmpty()) {
        QStringList units(m_nodeLogFilter.units.cbegin(),
                          m_nodeLogFilter.units.cend());
        std::sort(units.begin(), units.end());
        parts.append(units.size() <= 2 ? units.join(QLatin1Char(','))
                                       : QStringLiteral("%1 units")
                                             .arg(units.size()));
    }
    if (m_nodeLogFilter.errorsOnly)
        parts.append(QStringLiteral("errors"));
    m_nodeLogFilterButton->setText(
        parts.isEmpty() ? QStringLiteral("Nodes")
                        : QStringLiteral("Nodes: ") + parts.join(QStringLiteral(" · ")));
    m_nodeLogFilterButton->setChecked(m_nodeLogFilter.active());
}

void MainWindow::showNodeLogFilterMenu()
{
    if (!m_nodeLogFilterButton)
        return;
    QMenu menu(this);
    auto apply = [this] {
        updateNodeLogFilterButton();
        m_networkLogViewStale = false;
        rebuildNetworkLogView();
        refreshLogTimelineChart();
    };
    QAction *all = menu.addAction(QStringLiteral("Every line (no node filter)"));
    all->setCheckable(true);
    all->setChecked(!m_nodeLogFilter.active());
    connect(all, &QAction::triggered, this, [this, apply] {
        m_nodeLogFilter = {};
        apply();
    });
    QAction *errors = menu.addAction(QStringLiteral("Node errors only"));
    errors->setCheckable(true);
    errors->setChecked(m_nodeLogFilter.errorsOnly);
    connect(errors, &QAction::toggled, this, [this, apply](bool on) {
        m_nodeLogFilter.errorsOnly = on;
        apply();
    });
    menu.addSection(QStringLiteral("Nodes"));
    QStringList nodes = m_coreLogFeeds.keys();
    for (auto it = m_coreLogUnitsSeen.cbegin(); it != m_coreLogUnitsSeen.cend();
         ++it)
        if (!nodes.contains(it.key()))
            nodes.append(it.key());
    std::sort(nodes.begin(), nodes.end(), [](const QString &a, const QString &b) {
        if (a.at(0) != b.at(0))
            return a.at(0) < b.at(0);
        return a.mid(1).toInt() < b.mid(1).toInt();
    });
    for (const QString &node : std::as_const(nodes)) {
        const QSharedPointer<CoreLogFeed> feed = m_coreLogFeeds.value(node);
        const QString role = feed ? feed->node.role : QString();
        QAction *action = menu.addAction(
            role.isEmpty() ? node : QStringLiteral("%1 (%2)").arg(node, role));
        action->setCheckable(true);
        action->setChecked(m_nodeLogFilter.nodes.contains(node));
        connect(action, &QAction::toggled, this, [this, node, apply](bool on) {
            if (on)
                m_nodeLogFilter.nodes.insert(node);
            else
                m_nodeLogFilter.nodes.remove(node);
            apply();
        });
    }
    QSet<QString> unitSet;
    for (auto it = m_coreLogUnitsSeen.cbegin(); it != m_coreLogUnitsSeen.cend();
         ++it)
        if (m_nodeLogFilter.nodes.isEmpty() ||
            m_nodeLogFilter.nodes.contains(it.key()))
            unitSet.unite(it.value());
    unitSet.unite(m_nodeLogFilter.units);
    QStringList units(unitSet.cbegin(), unitSet.cend());
    std::sort(units.begin(), units.end());
    if (!units.isEmpty()) {
        QMenu *unitMenu = menu.addMenu(QStringLiteral("Units"));
        for (const QString &unit : std::as_const(units)) {
            QAction *action = unitMenu->addAction(unit);
            action->setCheckable(true);
            action->setChecked(m_nodeLogFilter.units.contains(unit));
            connect(action, &QAction::toggled, this,
                    [this, unit, apply](bool on) {
                        if (on)
                            m_nodeLogFilter.units.insert(unit);
                        else
                            m_nodeLogFilter.units.remove(unit);
                        apply();
                    });
        }
    }
    menu.exec(m_nodeLogFilterButton->mapToGlobal(
        QPoint(0, m_nodeLogFilterButton->height())));
}

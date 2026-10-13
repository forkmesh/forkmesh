#include "SugarBreakFeature.h"

#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QStyle>
#include <QTableWidget>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::sugarbreak {
namespace {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

int integer(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toInt();
}

QLabel *mutedLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *sectionLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QString dateLabel(const QString &day)
{
    const QDate date = QDate::fromString(day, Qt::ISODate);
    if (!date.isValid())
        return day;
    if (date == QDate::currentDate())
        return QStringLiteral("today");
    return QLocale().toString(date, QStringLiteral("ddd d MMM"));
}

QString onDay(const QString &day)
{
    const QString label = dateLabel(day);
    return label == QLatin1String("today") ? label : QStringLiteral("on ") + label;
}

void fitTable(QTableWidget *table, int maxRows)
{
    int height = table->horizontalHeader()->sizeHint().height() + 2 * table->frameWidth();
    for (int row = 0; row < std::min(table->rowCount(), maxRows); ++row)
        height += table->rowHeight(row);
    table->setFixedHeight(height);
}

QString plural(int count, const QString &one, const QString &many)
{
    return count == 1 ? QStringLiteral("1 %1").arg(one) : QStringLiteral("%1 %2").arg(count).arg(many);
}

QString plusOnes(int count)
{
    return QStringLiteral("%1 × +1").arg(count);
}

QColor freeColor(bool dark) { return QColor(dark ? "#199e70" : "#1baf7a"); }
QColor slipColor(bool dark) { return QColor(dark ? "#e5675f" : "#d64545"); }

} // namespace

class SugarBreakDayStrip final : public QWidget
{
public:
    static constexpr int kColumns = 15;
    static constexpr int kCell = 20;
    static constexpr int kGap = 4;

    explicit SugarBreakDayStrip(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMouseTracking(true);
        setFixedHeight(kCell);
    }

    void setDays(const QJsonArray &days, const QString &today)
    {
        m_days = days;
        m_today = today;
        const int rows = std::max<int>(1, (int(days.size()) + kColumns - 1) / kColumns);
        setFixedHeight(rows * (kCell + kGap));
        setMinimumWidth(std::min<int>(kColumns, std::max<int>(1, int(days.size()))) * (kCell + kGap));
        int free = 0;
        for (const QJsonValue &value : days)
            free += text(value.toObject(), "status") == QLatin1String("sugar-free") ? 1 : 0;
        setAccessibleName(QStringLiteral("%1 of %2 days sugar-free").arg(free).arg(days.size()));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        QColor open = palette().color(QPalette::PlaceholderText);
        open.setAlphaF(0.35);
        QColor upcoming = palette().color(QPalette::PlaceholderText);
        upcoming.setAlphaF(0.45);
        for (int index = 0; index < m_days.size(); ++index) {
            const QJsonObject day = m_days.at(index).toObject();
            const QString status = text(day, "status");
            const QRectF cell = cellRect(index);
            painter.setPen(Qt::NoPen);
            if (status == QLatin1String("sugar-free")) {
                painter.setBrush(freeColor(dark));
            } else if (status == QLatin1String("slip")) {
                painter.setBrush(slipColor(dark));
            } else if (status == QLatin1String("open")) {
                painter.setBrush(open);
            } else {
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(upcoming, 1));
            }
            painter.drawRoundedRect(cell.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
            if (text(day, "day") == m_today) {
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(palette().color(QPalette::WindowText), 2));
                painter.drawRoundedRect(cell.adjusted(1, 1, -1, -1), 4, 4);
            }
            const int skips = integer(day, "skips");
            if (skips > 0 && status != QLatin1String("upcoming")) {
                painter.setPen(status == QLatin1String("open") ? palette().color(QPalette::WindowText)
                                                               : QColor(Qt::white));
                QFont font = painter.font();
                font.setPixelSize(9);
                painter.setFont(font);
                painter.drawText(cell, Qt::AlignCenter, skips > 99 ? QStringLiteral("99") : QString::number(skips));
            }
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
            return QWidget::event(event);
        const auto *help = static_cast<QHelpEvent *>(event);
        for (int index = 0; index < m_days.size(); ++index) {
            if (!cellRect(index).contains(help->pos()))
                continue;
            const QJsonObject day = m_days.at(index).toObject();
            QStringList lines{QStringLiteral("Day %1 · %2").arg(index + 1).arg(dateLabel(text(day, "day"))),
                              SugarBreakFeature::dayStatusLabel(text(day, "status"))};
            if (integer(day, "skips") > 0)
                lines << plusOnes(integer(day, "skips"));
            if (integer(day, "slips") > 0)
                lines << plural(integer(day, "slips"), QStringLiteral("slip"), QStringLiteral("slips"));
            QToolTip::showText(help->globalPos(), lines.join(QLatin1Char('\n')), this);
            return true;
        }
        QToolTip::hideText();
        return true;
    }

private:
    QRectF cellRect(int index) const
    {
        return QRectF((index % kColumns) * (kCell + kGap), (index / kColumns) * (kCell + kGap), kCell, kCell);
    }

    QJsonArray m_days;
    QString m_today;
};

QString SugarBreakFeature::lengthLabel(int days)
{
    if (days == 7)
        return QStringLiteral("1 week (7 days)");
    if (days == 14 || days == 21)
        return QStringLiteral("%1 weeks (%2 days)").arg(days / 7).arg(days);
    if (days == 30)
        return QStringLiteral("1 month (30 days)");
    if (days == 60 || days == 90)
        return QStringLiteral("%1 months (%2 days)").arg(days / 30).arg(days);
    if (days == 365)
        return QStringLiteral("1 year (365 days)");
    return days == 1 ? QStringLiteral("1 day") : QStringLiteral("%1 days").arg(days);
}

QString SugarBreakFeature::dayStatusLabel(const QString &status)
{
    if (status == QLatin1String("sugar-free"))
        return QStringLiteral("Sugar-free");
    if (status == QLatin1String("slip"))
        return QStringLiteral("Had sugar");
    if (status == QLatin1String("upcoming"))
        return QStringLiteral("Still to come");
    return QStringLiteral("Nothing logged");
}

QString SugarBreakFeature::withDay(const QString &path, const QString &day)
{
    return path + (path.contains(QLatin1Char('?')) ? QStringLiteral("&day=") : QStringLiteral("?day=")) + day;
}

QString SugarBreakFeature::errorText(const QJsonObject &payload, const QString &error)
{
    const QString code = text(payload, "error");
    if (code == QLatin1String("no_active_break"))
        return QStringLiteral("There is no sugar break running. Start one first.");
    if (code == QLatin1String("day_outside_break"))
        return QStringLiteral("That day is not part of the running sugar break.");
    if (code == QLatin1String("day_full"))
        return QStringLiteral("That day already holds as many entries as it can.");
    if (code == QLatin1String("break_not_found") || code == QLatin1String("event_not_found"))
        return QStringLiteral("It was already removed. Refreshing.");
    return error.isEmpty() ? code : error;
}

bool SugarBreakFeature::exportHasData(const QJsonObject &payload)
{
    return !payload.value(QStringLiteral("export")).toObject().value(QStringLiteral("breaks")).toArray().isEmpty();
}

SugarBreakFeature::SugarBreakFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("sugarBreakSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Sugar Break"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral("Skip sugar for a month or any length you choose, and tap +1 each time you resist")));
    heading->addStretch();
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_exportButton = new QPushButton(QStringLiteral("Export my data"));
    m_uninstallButton = new QPushButton(QStringLiteral("Uninstall…"));
    for (QPushButton *button : {refreshButton, m_exportButton, m_uninstallButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        heading->addWidget(button);
    }
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading Sugar Break…"));
    outer->addWidget(m_status);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    auto *installNote = new QLabel(QStringLiteral(
        "Sugar Break is private to your ForkMesh account. Install it, start a break of one month or any "
        "length you choose, and tap +1 every time you skip sugar. Your breaks and +1s show on the web, this "
        "desktop app and the ForkMesh iPhone app, which can also show the sugar Apple Health logged for each "
        "day. Everything is encrypted at rest with a key minted for your account alone."));
    installNote->setAlignment(Qt::AlignHCenter);
    installNote->setWordWrap(true);
    installLayout->addWidget(installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install Sugar Break"));
    m_installButton->setObjectName(QStringLiteral("primaryButton"));
    installRow->addWidget(m_installButton);
    installRow->addStretch();
    installLayout->addLayout(installRow);
    installLayout->addStretch();
    outer->addWidget(m_installPanel, 1);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_content = scroll;
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(12);

    m_startPanel = new QWidget;
    auto *startLayout = new QVBoxLayout(m_startPanel);
    startLayout->setContentsMargins(0, 0, 0, 0);
    startLayout->setSpacing(8);
    auto *startTitle = new QLabel(QStringLiteral("Start sugar break"));
    startTitle->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 600;"));
    startLayout->addWidget(startTitle);
    m_lastResult = mutedLabel(QString());
    startLayout->addWidget(m_lastResult);
    startLayout->addWidget(mutedLabel(QStringLiteral(
        "Pick how long to go without sugar. Each day you skip it counts as sugar-free; tap +1 every time "
        "you resist a craving.")));
    auto *startRow = new QHBoxLayout;
    startRow->addWidget(new QLabel(QStringLiteral("Length")));
    m_length = new QComboBox;
    m_length->setObjectName(QStringLiteral("sugarBreakLength"));
    startRow->addWidget(m_length);
    m_startButton = new QPushButton(QStringLiteral("Start sugar break"));
    m_startButton->setObjectName(QStringLiteral("primaryButton"));
    startRow->addWidget(m_startButton);
    startRow->addStretch();
    startLayout->addLayout(startRow);
    layout->addWidget(m_startPanel);

    m_activePanel = new QWidget;
    auto *active = new QVBoxLayout(m_activePanel);
    active->setContentsMargins(0, 0, 0, 0);
    active->setSpacing(10);
    m_dayLabel = new QLabel(QStringLiteral("—"));
    m_dayLabel->setObjectName(QStringLiteral("sugarBreakDay"));
    m_dayLabel->setStyleSheet(QStringLiteral("font-size: 28px; font-weight: 600;"));
    active->addWidget(m_dayLabel);
    m_dayDetail = mutedLabel(QString());
    active->addWidget(m_dayDetail);
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(10);
    m_progress->setRange(0, 30);
    active->addWidget(m_progress);
    m_progressLabel = mutedLabel(QString());
    active->addWidget(m_progressLabel);

    auto *logRow = new QHBoxLayout;
    m_skipButton = new QPushButton(QStringLiteral("+1  I skipped sugar"));
    m_skipButton->setObjectName(QStringLiteral("primaryButton"));
    m_skipButton->setAccessibleName(QStringLiteral("Plus one, I skipped sugar"));
    m_skipButton->setMinimumHeight(52);
    m_skipButton->setMinimumWidth(240);
    m_skipButton->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; padding: 8px 24px;"));
    logRow->addWidget(m_skipButton);
    m_slipButton = new QPushButton(QStringLiteral("I had sugar"));
    m_slipButton->setObjectName(QStringLiteral("ghostButton"));
    logRow->addWidget(m_slipButton, 0, Qt::AlignBottom);
    logRow->addStretch();
    active->addLayout(logRow);
    m_todayLabel = mutedLabel(QString());
    active->addWidget(m_todayLabel);

    m_events = new QTableWidget(0, 3);
    m_events->setHorizontalHeaderLabels({QStringLiteral("Time"), QStringLiteral("Logged"), QString()});
    m_events->verticalHeader()->setVisible(false);
    m_events->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_events->setSelectionMode(QAbstractItemView::NoSelection);
    m_events->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_events->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    active->addWidget(m_events);
    m_eventsEmpty = mutedLabel(QStringLiteral("Nothing logged today. Tap +1 each time you pass on sugar."));
    active->addWidget(m_eventsEmpty);

    auto *stats = new QGridLayout;
    stats->setHorizontalSpacing(24);
    m_skipsLabel = new QLabel;
    m_freeDaysLabel = new QLabel;
    m_streakLabel = new QLabel;
    m_bestLabel = new QLabel;
    m_leftLabel = new QLabel;
    m_slipDaysLabel = new QLabel;
    const QList<QPair<QString, QLabel *>> statRows = {
        {QStringLiteral("+1 total"), m_skipsLabel},
        {QStringLiteral("Sugar-free days"), m_freeDaysLabel},
        {QStringLiteral("Streak"), m_streakLabel},
        {QStringLiteral("Best streak"), m_bestLabel},
        {QStringLiteral("Days left"), m_leftLabel},
        {QStringLiteral("Days with sugar"), m_slipDaysLabel},
    };
    for (int index = 0; index < statRows.size(); ++index) {
        statRows.at(index).second->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600;"));
        stats->addWidget(mutedLabel(statRows.at(index).first), index / 3 * 2, index % 3);
        stats->addWidget(statRows.at(index).second, index / 3 * 2 + 1, index % 3);
    }
    stats->setColumnStretch(3, 1);
    active->addLayout(stats);

    active->addWidget(sectionLabel(QStringLiteral("Every day of the break")));
    m_strip = new SugarBreakDayStrip;
    active->addWidget(m_strip);
    active->addWidget(mutedLabel(QStringLiteral(
        "Green: sugar-free · Red: had sugar · Grey: nothing logged · Outline: still to come. The number is that day's +1s.")));
    auto *endRow = new QHBoxLayout;
    m_endButton = new QPushButton(QStringLiteral("End break early"));
    m_endButton->setObjectName(QStringLiteral("dangerButton"));
    endRow->addWidget(m_endButton);
    endRow->addStretch();
    active->addLayout(endRow);
    layout->addWidget(m_activePanel);

    layout->addWidget(sectionLabel(QStringLiteral("Past breaks")));
    m_history = new QTableWidget(0, 5);
    m_history->setHorizontalHeaderLabels({QStringLiteral("Started"), QStringLiteral("Length"),
                                          QStringLiteral("Sugar-free days"), QStringLiteral("+1s"),
                                          QStringLiteral("Result")});
    m_history->verticalHeader()->setVisible(false);
    m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_history->setSelectionMode(QAbstractItemView::NoSelection);
    m_history->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(m_history);
    m_historyEmpty = mutedLabel(QStringLiteral("No finished breaks yet."));
    layout->addWidget(m_historyEmpty);
    layout->addStretch();
    scroll->setWidget(body);
    outer->addWidget(m_content, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { load(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this] { exportMine(); });
    connect(m_uninstallButton, &QPushButton::clicked, this, [this] { uninstallPlugin(); });
    connect(m_startButton, &QPushButton::clicked, this, [this] { startBreak(); });
    connect(m_skipButton, &QPushButton::clicked, this, [this] { logEvent(QStringLiteral("skip")); });
    connect(m_slipButton, &QPushButton::clicked, this, [this] { logEvent(QStringLiteral("slip")); });
    connect(m_endButton, &QPushButton::clicked, this, [this] { endBreak(); });

    applyInstallState();
}

void SugarBreakFeature::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                                ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Sugar Break network access is unavailable."));
        return;
    }
    const QPointer<SugarBreakFeature> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                    const QString &error) mutable {
        if (!guard)
            return;
        handler(ok, payload, error);
        if (guard && !ok && text(payload, "error") == QLatin1String("plugin_not_installed"))
            guard->load();
    });
}

QString SugarBreakFeature::apiPath(const QString &suffix) const
{
    return QStringLiteral("/api/sugar-break") + suffix;
}

QString SugarBreakFeature::today() const
{
    return QDate::currentDate().toString(Qt::ISODate);
}

void SugarBreakFeature::refresh()
{
    load();
}

QList<forkmesh::market::NativePackageSearchHit> SugarBreakFeature::searchHits(const QString &query,
                                                                              int limit) const
{
    const QString needle = query.trimmed().toLower();
    if (!m_installed || limit <= 0 || needle.size() < 3 ||
        !(QStringLiteral("sugar break sweets candy craving streak").contains(needle)))
        return {};
    const QJsonObject current = m_overview.value(QStringLiteral("current")).toObject();
    const QJsonObject summary = m_overview.value(QStringLiteral("summary")).toObject();
    if (text(current, "status") != QLatin1String("active"))
        return {{QStringLiteral("start"), QStringLiteral("Start a sugar break"), QStringLiteral("Sugar Break")}};
    return {{QStringLiteral("current"),
             QStringLiteral("Sugar break: day %1 of %2").arg(integer(summary, "dayNumber")).arg(integer(summary, "lengthDays")),
             QStringLiteral("%1 sugar-free · streak %2 · %3")
                 .arg(plural(integer(summary, "sugarFreeDays"), QStringLiteral("day"), QStringLiteral("days")),
                      plural(integer(summary, "streak"), QStringLiteral("day"), QStringLiteral("days")),
                      plusOnes(integer(summary, "skips")))}};
}

void SugarBreakFeature::activateSearchHit(const QString &)
{
    load();
}

void SugarBreakFeature::applyInstallState()
{
    m_installPanel->setVisible(m_checked && !m_installed);
    m_content->setVisible(m_installed);
    m_exportButton->setVisible(m_installed);
    m_uninstallButton->setVisible(m_installed);
}

void SugarBreakFeature::installPlugin()
{
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/install")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Sugar Break was not installed: %1").arg(errorText(payload, error)), true);
        setStatus(QStringLiteral("Sugar Break installed for your account."));
        load();
    });
}

void SugarBreakFeature::load()
{
    if (m_loading) {
        m_reloadPending = true;
        return;
    }
    m_loading = true;
    m_reloadPending = false;
    const QString day = today();
    request(QByteArrayLiteral("GET"), withDay(apiPath(), day), {},
            [this, day](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (m_reloadPending)
            return load();
        if (!ok) {
            setStatus(QStringLiteral("Sugar Break could not load: %1").arg(errorText(payload, error)), true);
            return;
        }
        m_checked = true;
        m_installed = payload.value(QStringLiteral("installed")).toBool();
        applyInstallState();
        if (!m_installed) {
            m_overview = QJsonObject();
            setStatus(QStringLiteral("Sugar Break is not installed for your account yet."));
            return;
        }
        m_overview = payload;
        m_loadedDay = day;
        render();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
}

void SugarBreakFeature::renderLengths()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    const int fallback = profile.value(QStringLiteral("defaultLengthDays")).toInt(30);
    QList<int> lengths;
    for (const QJsonValue &value : m_overview.value(QStringLiteral("lengths")).toArray())
        if (value.toInt() > 0 && !lengths.contains(value.toInt()))
            lengths << value.toInt();
    if (lengths.isEmpty())
        lengths = {7, 14, 21, 30, 60, 90};
    if (fallback > 0 && !lengths.contains(fallback)) {
        lengths << fallback;
        std::sort(lengths.begin(), lengths.end());
    }
    const int chosen = m_length->count() > 0 ? m_length->currentData().toInt() : fallback;
    m_length->clear();
    for (int days : std::as_const(lengths))
        m_length->addItem(lengthLabel(days), days);
    const int index = m_length->findData(lengths.contains(chosen) ? chosen : fallback);
    m_length->setCurrentIndex(index < 0 ? 0 : index);
}

void SugarBreakFeature::renderEvents(const QJsonArray &events)
{
    m_events->setRowCount(0);
    for (int index = int(events.size()) - 1; index >= 0; --index) {
        const QJsonObject event = events.at(index).toObject();
        const int row = m_events->rowCount();
        m_events->insertRow(row);
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(qint64(event.value(QStringLiteral("at")).toDouble()));
        m_events->setItem(row, 0, new QTableWidgetItem(QLocale().toString(at.time(), QLocale::ShortFormat)));
        const bool slip = text(event, "kind") == QLatin1String("slip");
        QString label = slip ? QStringLiteral("Had sugar") : QStringLiteral("+1  Skipped sugar");
        if (!text(event, "note").isEmpty())
            label += QStringLiteral(" · ") + text(event, "note");
        m_events->setItem(row, 1, new QTableWidgetItem(label));
        auto *undo = new QPushButton(QStringLiteral("Undo"));
        undo->setObjectName(QStringLiteral("ghostButton"));
        const QString id = text(event, "id");
        connect(undo, &QPushButton::clicked, this, [this, id] { undoEvent(id); });
        m_events->setCellWidget(row, 2, undo);
    }
    fitTable(m_events, 6);
    m_events->setVisible(!events.isEmpty());
    m_eventsEmpty->setVisible(events.isEmpty());
}

void SugarBreakFeature::renderHistory()
{
    QJsonArray rows;
    const QJsonObject current = m_overview.value(QStringLiteral("current")).toObject();
    if (!current.isEmpty() && text(current, "status") != QLatin1String("active")) {
        QJsonObject finished = current;
        finished.insert(QStringLiteral("summary"), m_overview.value(QStringLiteral("summary")));
        rows.append(finished);
    }
    for (const QJsonValue &value : m_overview.value(QStringLiteral("history")).toArray())
        if (text(value.toObject(), "id") != text(current, "id"))
            rows.append(value);
    m_history->setRowCount(0);
    for (const QJsonValue &value : std::as_const(rows)) {
        const QJsonObject item = value.toObject();
        const QJsonObject summary = item.value(QStringLiteral("summary")).toObject();
        const int row = m_history->rowCount();
        m_history->insertRow(row);
        const int length = integer(item, "lengthDays");
        m_history->setItem(row, 0, new QTableWidgetItem(dateLabel(text(item, "startDay"))));
        m_history->setItem(row, 1, new QTableWidgetItem(lengthLabel(length)));
        m_history->setItem(row, 2, new QTableWidgetItem(
                                       QStringLiteral("%1 of %2").arg(integer(summary, "sugarFreeDays")).arg(length)));
        m_history->setItem(row, 3, new QTableWidgetItem(QString::number(integer(summary, "skips"))));
        const QString status = text(item, "status");
        const QString result = status == QLatin1String("ended")
            ? QStringLiteral("Ended early %1").arg(onDay(text(item, "endedDay")))
            : status == QLatin1String("completed") ? QStringLiteral("Completed · best streak %1")
                                                         .arg(plural(integer(summary, "bestStreak"),
                                                                     QStringLiteral("day"), QStringLiteral("days")))
                                                   : QStringLiteral("Running");
        m_history->setItem(row, 4, new QTableWidgetItem(result));
    }
    fitTable(m_history, 8);
    m_history->setVisible(!rows.isEmpty());
    m_historyEmpty->setVisible(rows.isEmpty());
}

void SugarBreakFeature::render()
{
    const QJsonObject current = m_overview.value(QStringLiteral("current")).toObject();
    const QJsonObject summary = m_overview.value(QStringLiteral("summary")).toObject();
    const QJsonObject todayInfo = m_overview.value(QStringLiteral("today")).toObject();
    const QString status = text(current, "status");
    const bool running = !current.isEmpty() && status == QLatin1String("active");
    const QString day = text(m_overview, "day").isEmpty() ? m_loadedDay : text(m_overview, "day");

    renderLengths();
    m_startPanel->setVisible(!running);
    m_activePanel->setVisible(running);
    m_startButton->setEnabled(!m_busy);
    m_endButton->setEnabled(!m_busy);
    if (!current.isEmpty() && !running) {
        const int length = integer(current, "lengthDays");
        m_lastResult->setText(
            status == QLatin1String("ended")
                ? QStringLiteral("Your last break ended early %1 with %2 of %3 days sugar-free.")
                      .arg(onDay(text(current, "endedDay")))
                      .arg(integer(summary, "sugarFreeDays"))
                      .arg(length)
                : QStringLiteral("Your %1 break finished %2 with %3 of %4 days sugar-free and %5.")
                      .arg(lengthLabel(length), onDay(text(current, "endDay")))
                      .arg(integer(summary, "sugarFreeDays"))
                      .arg(length)
                      .arg(plusOnes(integer(summary, "skips"))));
    }
    m_lastResult->setVisible(!current.isEmpty() && !running);

    if (running) {
        const int length = std::max(1, integer(summary, "lengthDays") > 0 ? integer(summary, "lengthDays")
                                                                          : integer(current, "lengthDays"));
        const int dayNumber = integer(summary, "dayNumber");
        const bool inBreak = todayInfo.value(QStringLiteral("inBreak")).toBool(dayNumber > 0);
        m_dayLabel->setText(dayNumber > 0 ? QStringLiteral("Day %1 of %2").arg(dayNumber).arg(length)
                                          : QStringLiteral("Starts %1").arg(dateLabel(text(current, "startDay"))));
        m_dayDetail->setText(QStringLiteral("%1 · started %2 · ends %3")
                                 .arg(lengthLabel(length), dateLabel(text(current, "startDay")),
                                      dateLabel(text(current, "endDay"))));
        const int freeDays = integer(summary, "sugarFreeDays");
        m_progress->setRange(0, length);
        m_progress->setValue(std::clamp(freeDays, 0, length));
        m_progressLabel->setText(QStringLiteral("%1 of %2 days sugar-free (%3%)")
                                     .arg(freeDays)
                                     .arg(length)
                                     .arg(integer(summary, "percent")));
        m_skipButton->setEnabled(inBreak);
        m_slipButton->setEnabled(inBreak);
        const int skips = integer(todayInfo, "skips");
        const int slips = integer(todayInfo, "slips");
        if (!inBreak)
            m_todayLabel->setText(QStringLiteral("Logging opens on the first day of the break."));
        else if (slips > 0)
            m_todayLabel->setText(QStringLiteral("Today: %1 and %2. Tomorrow is a fresh day.")
                                      .arg(plusOnes(skips),
                                           plural(slips, QStringLiteral("slip"), QStringLiteral("slips"))));
        else if (skips > 0)
            m_todayLabel->setText(QStringLiteral("Today: %1, sugar-free so far.")
                                      .arg(plusOnes(skips)));
        else
            m_todayLabel->setText(QStringLiteral("Nothing logged today yet. A day counts as sugar-free once it has a +1 and no sugar."));
        renderEvents(m_overview.value(QStringLiteral("events")).toArray());
        m_skipsLabel->setText(QString::number(integer(summary, "skips")));
        m_freeDaysLabel->setText(QString::number(freeDays));
        m_streakLabel->setText(plural(integer(summary, "streak"), QStringLiteral("day"), QStringLiteral("days")));
        m_bestLabel->setText(plural(integer(summary, "bestStreak"), QStringLiteral("day"), QStringLiteral("days")));
        m_leftLabel->setText(QString::number(integer(summary, "remainingDays")));
        m_slipDaysLabel->setText(QString::number(integer(summary, "slipDays")));
        m_strip->setDays(m_overview.value(QStringLiteral("days")).toArray(), day);
        setStatus(QStringLiteral("Day %1 of %2 · %3 · streak %4")
                      .arg(dayNumber)
                      .arg(length)
                      .arg(plural(freeDays, QStringLiteral("sugar-free day"), QStringLiteral("sugar-free days")),
                           plural(integer(summary, "streak"), QStringLiteral("day"), QStringLiteral("days"))));
    } else {
        setStatus(current.isEmpty() ? QStringLiteral("No sugar break yet. Pick a length and start one.")
                                    : QStringLiteral("No sugar break running. Start another whenever you are ready."));
    }
    renderHistory();
    if (!m_notice.isEmpty()) {
        setStatus(m_notice);
        m_notice.clear();
    }
}

void SugarBreakFeature::startBreak()
{
    const int length = m_length->currentData().toInt();
    if (length <= 0)
        return setStatus(QStringLiteral("Pick a length first."), true);
    const QString day = today();
    m_busy = true;
    m_startButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), withDay(apiPath(QStringLiteral("/breaks")), day),
            QJsonObject{{QStringLiteral("lengthDays"), length}, {QStringLiteral("startDay"), day}},
            [this, length](bool ok, const QJsonObject &payload, const QString &error) {
        m_busy = false;
        m_startButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("The sugar break did not start: %1").arg(errorText(payload, error)), true);
        m_notice = QStringLiteral("Your %1 sugar break has started. Tap +1 every time you skip sugar.")
                       .arg(lengthLabel(length));
        load();
    });
}

void SugarBreakFeature::logEvent(const QString &kind)
{
    const QString day = today();
    request(QByteArrayLiteral("POST"), withDay(apiPath(QStringLiteral("/events")), day),
            QJsonObject{{QStringLiteral("kind"), kind}, {QStringLiteral("day"), day}},
            [this, kind](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("That was not logged: %1").arg(errorText(payload, error)), true);
            return load();
        }
        m_notice = kind == QLatin1String("slip") ? QStringLiteral("Logged that you had sugar. Keep going tomorrow.")
                                                 : QStringLiteral("+1 logged. Nice one.");
        load();
    });
}

void SugarBreakFeature::undoEvent(const QString &id)
{
    request(QByteArrayLiteral("DELETE"), withDay(apiPath(QStringLiteral("/events/") + id), today()), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            setStatus(QStringLiteral("That entry was not removed: %1").arg(errorText(payload, error)), true);
        load();
    });
}

void SugarBreakFeature::endBreak()
{
    const QJsonObject current = m_overview.value(QStringLiteral("current")).toObject();
    const QString id = text(current, "id");
    if (id.isEmpty())
        return;
    const QJsonObject summary = m_overview.value(QStringLiteral("summary")).toObject();
    if (QMessageBox::question(
            this, QStringLiteral("End sugar break early"),
            QStringLiteral("End this sugar break now, on day %1 of %2?\n\nYour %3 and %4 stay in your history.")
                .arg(integer(summary, "dayNumber"))
                .arg(integer(current, "lengthDays"))
                .arg(plural(integer(summary, "sugarFreeDays"), QStringLiteral("sugar-free day"),
                            QStringLiteral("sugar-free days")),
                     plusOnes(integer(summary, "skips"))),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    m_busy = true;
    m_endButton->setEnabled(false);
    request(QByteArrayLiteral("PATCH"), withDay(apiPath(QStringLiteral("/breaks/") + id), today()),
            QJsonObject{{QStringLiteral("end"), true}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_busy = false;
        m_endButton->setEnabled(true);
        if (!ok) {
            setStatus(QStringLiteral("The sugar break was not ended: %1").arg(errorText(payload, error)), true);
            return load();
        }
        m_notice = QStringLiteral("Sugar break ended. Start another whenever you are ready.");
        load();
    });
}

bool SugarBreakFeature::saveExportBundle(const QJsonObject &payload)
{
    const QString suggested = text(payload, "filename").isEmpty()
        ? QStringLiteral("forkmesh-sugar-break-export.json") : text(payload, "filename");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save the Sugar Break export"),
                                                      suggested, QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty())
        return false;
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(payload.value(QStringLiteral("export")).toObject())
                                 .toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        setStatus(QStringLiteral("The export file could not be written."), true);
        return false;
    }
    setStatus(QStringLiteral("Exported to %1.").arg(path));
    return true;
}

void SugarBreakFeature::exportMine()
{
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The export failed: %1").arg(errorText(payload, error)), true);
        saveExportBundle(payload);
    });
}

bool SugarBreakFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    if (m_checked && !m_installed)
        return false;
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        QJsonObject result = payload;
        result.insert(QStringLiteral("hasData"), exportHasData(payload));
        done(ok, result, error);
    });
    return true;
}

void SugarBreakFeature::uninstallPlugin()
{
    if (QMessageBox::warning(
            this, QStringLiteral("Uninstall Sugar Break"),
            QStringLiteral("Uninstall Sugar Break from your account?\n\nYour breaks and +1s are saved to a file "
                           "first, then deleted from the web dashboard, this desktop app, and the mobile app. "
                           "Apple Health keeps its own sugar records."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Uninstall stopped — the export failed: %1").arg(errorText(payload, error)),
                             true);
        if (exportHasData(payload) && !saveExportBundle(payload))
            return setStatus(QStringLiteral("Uninstall cancelled: the data file was not saved."), true);
        request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/uninstall")), {},
                [this](bool removed, const QJsonObject &removePayload, const QString &removeError) {
            if (!removed)
                return setStatus(QStringLiteral("Sugar Break was not uninstalled: %1")
                                     .arg(errorText(removePayload, removeError)),
                                 true);
            m_checked = true;
            m_installed = false;
            m_overview = QJsonObject();
            applyInstallState();
            setStatus(QStringLiteral("Sugar Break uninstalled from your account. Install it again at any time."));
        });
    });
}

void SugarBreakFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::sugarbreak

namespace {
const forkmesh::market::NativePackageRegistration sugarBreakRegistration({
    QStringLiteral("builtin:sugar-break"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Sugar Break"),
    QStringLiteral("candy-off"),
    QStringLiteral("/api/sugar-break"),
    QStringLiteral("forkmesh-sugar-break-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::sugarbreak::SugarBreakFeature(host, parent);
    },
});
} // namespace

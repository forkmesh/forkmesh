#include "SleepFeature.h"

#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
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
#include <QSpinBox>
#include <QStyle>
#include <QTimeEdit>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::sleep {
namespace {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

double number(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toDouble();
}

QLabel *mutedLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QString nightLabel(const QString &night)
{
    const QDate date = QDate::fromString(night, Qt::ISODate);
    if (!date.isValid())
        return night;
    if (date == QDate::currentDate())
        return QStringLiteral("Last night");
    return QLocale().toString(date, QStringLiteral("ddd d MMM"));
}

QString clockAt(double ms)
{
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(ms)).time(), QLocale::ShortFormat);
}

QTime clockFrom(const QString &value, const QTime &fallback)
{
    const QTime parsed = QTime::fromString(value, QStringLiteral("HH:mm"));
    return parsed.isValid() ? parsed : fallback;
}

QColor barColor(bool dark) { return QColor(dark ? "#8a80f0" : "#6a5fd8"); }
QColor reachedColor(bool dark) { return QColor(dark ? "#199e70" : "#1baf7a"); }
QColor goalColor(bool dark) { return QColor(dark ? "#c98500" : "#eda100"); }

} // namespace

class SleepChart final : public QWidget
{
public:
    explicit SleepChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(190);
        setMouseTracking(true);
    }

    void setNights(const QJsonArray &nights, int goal)
    {
        m_nights = nights;
        m_goal = goal;
        setAccessibleName(QStringLiteral("Hours slept for the last %1 nights").arg(nights.size()));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        QColor grid = palette().color(QPalette::PlaceholderText);
        grid.setAlphaF(0.35);
        const QRectF plot(56, 10, qMax<qreal>(20, width() - 68), qMax<qreal>(20, height() - 36));
        double peak = qMax<double>(600.0, m_goal * 1.15);
        for (const QJsonValue &value : m_nights)
            peak = qMax(peak, number(value.toObject(), "asleepMinutes") * 1.05);
        for (int step = 0; step <= 4; ++step) {
            const qreal y = plot.bottom() - plot.height() * step / 4;
            painter.setPen(QPen(grid, 1));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(0, y - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("%1 h").arg(QLocale().toString(peak * step / 4 / 60.0, 'f', step ? 1 : 0)));
        }
        if (m_nights.isEmpty())
            return;
        const qreal slot = plot.width() / m_nights.size();
        const qreal barWidth = qMin<qreal>(22, qMax<qreal>(3, slot - 4));
        for (int index = 0; index < m_nights.size(); ++index) {
            const QJsonObject night = m_nights.at(index).toObject();
            const double minutes = number(night, "asleepMinutes");
            const qreal x = plot.left() + index * slot + (slot - barWidth) / 2;
            if (minutes > 0) {
                const qreal h = qMax<qreal>(2, plot.height() * minutes / peak);
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_goal > 0 && minutes >= m_goal ? reachedColor(dark) : barColor(dark));
                painter.drawRoundedRect(QRectF(x, plot.bottom() - h, barWidth, h), 3, 3);
            }
            if (index % 5 == 0 || index == m_nights.size() - 1) {
                painter.setPen(palette().color(QPalette::PlaceholderText));
                painter.drawText(QRectF(x + barWidth / 2 - 30, plot.bottom() + 4, 60, 16),
                                 Qt::AlignHCenter | Qt::AlignTop,
                                 QDate::fromString(text(night, "night"), Qt::ISODate).toString(QStringLiteral("M/d")));
            }
        }
        if (m_goal > 0) {
            const qreal y = plot.bottom() - plot.height() * m_goal / peak;
            painter.setPen(QPen(goalColor(dark), 2, Qt::DashLine));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(plot.right() - 160, y - 18, 160, 16), Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("Goal %1").arg(SleepFeature::durationText(m_goal)));
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
            return QWidget::event(event);
        const auto *help = static_cast<QHelpEvent *>(event);
        const qreal left = 56;
        const qreal plotWidth = qMax<qreal>(20, width() - 68);
        if (m_nights.isEmpty() || help->pos().x() < left || help->pos().x() > left + plotWidth)
            return true;
        const int index = std::clamp(int((help->pos().x() - left) / (plotWidth / m_nights.size())), 0,
                                     int(m_nights.size()) - 1);
        const QJsonObject night = m_nights.at(index).toObject();
        const qint64 minutes = qint64(number(night, "asleepMinutes"));
        QToolTip::showText(help->globalPos(),
                           minutes > 0
                               ? QStringLiteral("%1\n%2 · %3 – %4")
                                     .arg(nightLabel(text(night, "night")), SleepFeature::durationText(minutes),
                                          clockAt(number(night, "bedAt")), clockAt(number(night, "wakeAt")))
                               : QStringLiteral("%1\nNo sleep recorded").arg(nightLabel(text(night, "night"))),
                           this);
        return true;
    }

private:
    QJsonArray m_nights;
    int m_goal = 0;
};

QString SleepFeature::durationText(qint64 minutes)
{
    minutes = std::max<qint64>(0, minutes);
    const qint64 hours = minutes / 60;
    const qint64 rest = minutes % 60;
    if (hours == 0)
        return QStringLiteral("%1 min").arg(rest);
    if (rest == 0)
        return QStringLiteral("%1 h").arg(hours);
    return QStringLiteral("%1 h %2 min").arg(hours).arg(rest);
}

QString SleepFeature::clockText(int minutesOfDay)
{
    if (minutesOfDay < 0)
        return QStringLiteral("—");
    return QLocale().toString(QTime(minutesOfDay / 60 % 24, minutesOfDay % 60), QLocale::ShortFormat);
}

bool SleepFeature::exportHasData(const QJsonObject &payload)
{
    return !payload.value(QStringLiteral("export")).toObject().value(QStringLiteral("nights")).toArray().isEmpty();
}

QPair<qint64, qint64> SleepFeature::nightSpan(const QDate &night, const QTime &bed, const QTime &wake)
{
    const QDateTime wakeAt(night, wake);
    const QDateTime bedAt(bed >= wake ? night.addDays(-1) : night, bed);
    return {bedAt.toMSecsSinceEpoch(), wakeAt.toMSecsSinceEpoch()};
}

SleepFeature::SleepFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("sleepSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Sleep"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral("Worked out from your phone resting by your bed and from Apple Health")));
    heading->addStretch();
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_exportButton = new QPushButton(QStringLiteral("Export my data"));
    m_uninstallButton = new QPushButton(QStringLiteral("Uninstall…"));
    for (QPushButton *button : {refreshButton, m_exportButton, m_uninstallButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        heading->addWidget(button);
    }
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading Sleep…"));
    outer->addWidget(m_status);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    auto *installNote = new QLabel(QStringLiteral(
        "Sleep is private to your ForkMesh account. Install it, then turn on sleep tracking in the "
        "ForkMesh iPhone app and leave your phone by your bed at night. The stretch it lies still and "
        "unused counts as sleep; when your Apple Watch records sleep in Apple Health, that is used "
        "instead. Every night is encrypted at rest with a key minted for your account alone."));
    installNote->setAlignment(Qt::AlignHCenter);
    installNote->setWordWrap(true);
    installLayout->addWidget(installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install Sleep"));
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

    auto *lastRow = new QHBoxLayout;
    auto *lastColumn = new QVBoxLayout;
    m_lastNight = new QLabel(QStringLiteral("—"));
    m_lastNight->setObjectName(QStringLiteral("sleepLastNight"));
    m_lastNight->setStyleSheet(QStringLiteral("font-size: 28px; font-weight: 600;"));
    lastColumn->addWidget(m_lastNight);
    m_lastDetail = mutedLabel(QString());
    lastColumn->addWidget(m_lastDetail);
    m_goalBar = new QProgressBar;
    m_goalBar->setTextVisible(false);
    m_goalBar->setMaximumHeight(10);
    m_goalBar->setRange(0, 1000);
    lastColumn->addWidget(m_goalBar);
    m_sourceLabel = mutedLabel(QString());
    lastColumn->addWidget(m_sourceLabel);
    lastRow->addLayout(lastColumn, 2);
    auto *stats = new QGridLayout;
    stats->setHorizontalSpacing(24);
    m_weekLabel = new QLabel;
    m_averageLabel = new QLabel;
    m_bedtimeLabel = new QLabel;
    m_wakeLabel = new QLabel;
    const QList<QPair<QString, QLabel *>> statRows = {
        {QStringLiteral("Last 7 nights"), m_weekLabel},
        {QStringLiteral("30-night average"), m_averageLabel},
        {QStringLiteral("Average bedtime"), m_bedtimeLabel},
        {QStringLiteral("Average wake time"), m_wakeLabel},
    };
    for (int index = 0; index < statRows.size(); ++index) {
        stats->addWidget(mutedLabel(statRows.at(index).first), index / 2 * 2, index % 2);
        stats->addWidget(statRows.at(index).second, index / 2 * 2 + 1, index % 2);
    }
    lastRow->addLayout(stats, 3);
    layout->addLayout(lastRow);

    m_chart = new SleepChart;
    layout->addWidget(m_chart);

    auto *forms = new QHBoxLayout;
    forms->setSpacing(24);
    auto *manualBox = new QWidget;
    auto *manualForm = new QFormLayout(manualBox);
    manualForm->addRow(mutedLabel(QStringLiteral(
        "Add a night your phone was not by your bed. A night you add replaces what the phone or Apple Health recorded.")));
    m_manualNight = new QDateEdit(QDate::currentDate());
    m_manualNight->setCalendarPopup(true);
    m_manualNight->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_manualNight->setMaximumDate(QDate::currentDate());
    manualForm->addRow(QStringLiteral("Woke up on"), m_manualNight);
    m_manualBed = new QTimeEdit(QTime(23, 0));
    m_manualBed->setDisplayFormat(QStringLiteral("HH:mm"));
    manualForm->addRow(QStringLiteral("Went to bed"), m_manualBed);
    m_manualWake = new QTimeEdit(QTime(7, 0));
    m_manualWake->setDisplayFormat(QStringLiteral("HH:mm"));
    manualForm->addRow(QStringLiteral("Woke up"), m_manualWake);
    m_manualAwake = new QSpinBox;
    m_manualAwake->setRange(0, 600);
    m_manualAwake->setSingleStep(5);
    m_manualAwake->setSuffix(QStringLiteral(" min"));
    manualForm->addRow(QStringLiteral("Awake in bed"), m_manualAwake);
    auto *manualButtons = new QHBoxLayout;
    auto *saveManual = new QPushButton(QStringLiteral("Save night"));
    saveManual->setObjectName(QStringLiteral("primaryButton"));
    auto *clearManual = new QPushButton(QStringLiteral("Remove added night"));
    clearManual->setObjectName(QStringLiteral("ghostButton"));
    manualButtons->addWidget(saveManual);
    manualButtons->addWidget(clearManual);
    manualButtons->addStretch();
    manualForm->addRow(manualButtons);
    forms->addWidget(manualBox, 1);

    auto *goalBox = new QWidget;
    auto *goalForm = new QFormLayout(goalBox);
    goalForm->addRow(mutedLabel(QStringLiteral("Your phone looks for its longest still stretch around this schedule.")));
    m_goal = new QDoubleSpinBox;
    m_goal->setRange(0, 16);
    m_goal->setDecimals(2);
    m_goal->setSingleStep(0.25);
    m_goal->setSuffix(QStringLiteral(" h"));
    goalForm->addRow(QStringLiteral("Sleep goal"), m_goal);
    m_bedtime = new QTimeEdit(QTime(23, 0));
    m_bedtime->setDisplayFormat(QStringLiteral("HH:mm"));
    goalForm->addRow(QStringLiteral("Bedtime"), m_bedtime);
    m_wakeTime = new QTimeEdit(QTime(7, 0));
    m_wakeTime->setDisplayFormat(QStringLiteral("HH:mm"));
    goalForm->addRow(QStringLiteral("Wake time"), m_wakeTime);
    auto *saveGoal = new QPushButton(QStringLiteral("Save goal"));
    goalForm->addRow(saveGoal);
    forms->addWidget(goalBox, 1);
    layout->addLayout(forms);
    layout->addStretch();
    scroll->setWidget(body);
    outer->addWidget(m_content, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { load(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this] { exportMine(); });
    connect(m_uninstallButton, &QPushButton::clicked, this, [this] { uninstallPlugin(); });
    connect(saveManual, &QPushButton::clicked, this, [this] { this->saveManual(); });
    connect(clearManual, &QPushButton::clicked, this, [this] { this->clearManual(); });
    connect(saveGoal, &QPushButton::clicked, this, [this] { this->saveGoal(); });

    applyInstallState();
}

void SleepFeature::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                           ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Sleep network access is unavailable."));
        return;
    }
    const QPointer<SleepFeature> guard(this);
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

QString SleepFeature::apiPath(const QString &suffix) const
{
    return QStringLiteral("/api/sleep") + suffix;
}

void SleepFeature::refresh()
{
    load();
}

QList<forkmesh::market::NativePackageSearchHit> SleepFeature::searchHits(const QString &query,
                                                                         int limit) const
{
    const QString needle = query.trimmed().toLower();
    if (!m_installed || limit <= 0 || needle.size() < 3 ||
        !(QStringLiteral("sleep bedtime night rest wake").contains(needle)))
        return {};
    const QJsonObject last = m_overview.value(QStringLiteral("lastNight")).toObject();
    const qint64 minutes = qint64(number(last, "asleepMinutes"));
    return {{QStringLiteral("last-night"),
             minutes > 0 ? QStringLiteral("Slept %1 last night").arg(durationText(minutes))
                         : QStringLiteral("No sleep recorded last night"),
             minutes > 0 ? QStringLiteral("%1 – %2").arg(clockAt(number(last, "bedAt")), clockAt(number(last, "wakeAt")))
                         : QStringLiteral("Sleep")}};
}

void SleepFeature::activateSearchHit(const QString &)
{
    load();
}

void SleepFeature::applyInstallState()
{
    m_installPanel->setVisible(m_checked && !m_installed);
    m_content->setVisible(m_installed);
    m_exportButton->setVisible(m_installed);
    m_uninstallButton->setVisible(m_installed);
}

void SleepFeature::installPlugin()
{
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/install")),
            QJsonObject{{QStringLiteral("tzOffsetMinutes"), -QDateTime::currentDateTime().offsetFromUtc() / 60}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Sleep was not installed: %1").arg(error), true);
        setStatus(QStringLiteral("Sleep installed for your account."));
        load();
    });
}

void SleepFeature::load()
{
    if (m_loading)
        return;
    m_loading = true;
    request(QByteArrayLiteral("GET"),
            apiPath(QStringLiteral("?night=") + QDate::currentDate().toString(Qt::ISODate)), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            setStatus(QStringLiteral("Sleep could not load: %1").arg(error), true);
            return;
        }
        m_checked = true;
        m_installed = payload.value(QStringLiteral("installed")).toBool();
        applyInstallState();
        if (!m_installed) {
            m_overview = QJsonObject();
            setStatus(QStringLiteral("Sleep is not installed for your account yet."));
            return;
        }
        m_overview = payload;
        render();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
}

void SleepFeature::render()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    const QJsonObject last = m_overview.value(QStringLiteral("lastNight")).toObject();
    const QJsonObject summary = m_overview.value(QStringLiteral("summary")).toObject();
    const QJsonObject labels = m_overview.value(QStringLiteral("sourceLabels")).toObject();
    const QJsonArray nights = m_overview.value(QStringLiteral("nights")).toArray();
    const int goal = profile.value(QStringLiteral("goalMinutes")).toInt();
    const qint64 asleep = qint64(number(last, "asleepMinutes"));
    m_lastNight->setText(asleep > 0 ? durationText(asleep) : QStringLiteral("No sleep yet"));
    QStringList detail;
    if (asleep > 0) {
        detail << QStringLiteral("%1 – %2").arg(clockAt(number(last, "bedAt")), clockAt(number(last, "wakeAt")));
        if (number(last, "awakeMinutes") > 0)
            detail << QStringLiteral("%1 awake").arg(durationText(qint64(number(last, "awakeMinutes"))));
        const int interruptions = last.value(QStringLiteral("interruptions")).toInt();
        if (interruptions > 0)
            detail << (interruptions == 1 ? QStringLiteral("1 interruption")
                                          : QStringLiteral("%1 interruptions").arg(interruptions));
        if (number(last, "deepMinutes") > 0)
            detail << QStringLiteral("%1 deep").arg(durationText(qint64(number(last, "deepMinutes"))));
        if (number(last, "remMinutes") > 0)
            detail << QStringLiteral("%1 REM").arg(durationText(qint64(number(last, "remMinutes"))));
        if (goal > 0)
            detail << (asleep >= goal ? QStringLiteral("Goal reached")
                                      : QStringLiteral("%1 short of %2").arg(durationText(goal - asleep), durationText(goal)));
    }
    m_lastDetail->setText(detail.join(QStringLiteral(" · ")));
    m_goalBar->setValue(goal > 0 ? int(std::min<qint64>(1000, asleep * 1000 / goal)) : 0);
    const QString source = text(last, "source");
    if (source.isEmpty()) {
        m_sourceLabel->setText(QStringLiteral(
            "Nothing for last night yet. Open Sleep in the ForkMesh iPhone app in the morning and it fills in."));
    } else {
        const QJsonObject reading = last.value(QStringLiteral("sources")).toObject().value(source).toObject();
        QStringList parts{labels.value(source).toString(source)};
        if (source == QLatin1String("phone") && !text(last, "confidence").isEmpty())
            parts << QStringLiteral("%1 confidence").arg(text(last, "confidence"));
        if (number(reading, "updatedAt") > 0)
            parts << QStringLiteral("synced %1").arg(clockAt(number(reading, "updatedAt")));
        m_sourceLabel->setText(parts.join(QStringLiteral(" · ")));
    }
    const int weekNights = summary.value(QStringLiteral("weekNights")).toInt();
    m_weekLabel->setText(weekNights > 0
                             ? QStringLiteral("%1 a night%2").arg(
                                   durationText(qint64(number(summary, "weekAverageMinutes"))),
                                   goal > 0 ? QStringLiteral(" · %1 debt").arg(durationText(qint64(number(summary, "debtMinutes"))))
                                            : QString())
                             : QStringLiteral("—"));
    const int logged = summary.value(QStringLiteral("loggedNights")).toInt();
    m_averageLabel->setText(logged > 0 ? durationText(qint64(number(summary, "averageMinutes"))) : QStringLiteral("—"));
    const int spread = summary.value(QStringLiteral("bedtimeSpreadMinutes")).toInt();
    m_bedtimeLabel->setText(clockText(summary.value(QStringLiteral("averageBedtimeMinutes")).toInt(-1)) +
                            (spread > 0 ? QStringLiteral(" ± %1").arg(durationText(spread)) : QString()));
    m_wakeLabel->setText(clockText(summary.value(QStringLiteral("averageWakeMinutes")).toInt(-1)));
    m_chart->setNights(nights, goal);
    m_goal->setValue(goal / 60.0);
    m_bedtime->setTime(clockFrom(text(profile, "bedtime"), QTime(23, 0)));
    m_wakeTime->setTime(clockFrom(text(profile, "wakeTime"), QTime(7, 0)));
    m_manualNight->setMaximumDate(QDate::currentDate());
    const int streak = summary.value(QStringLiteral("streak")).toInt();
    setStatus(QStringLiteral("%1 of %2 recorded nights met your goal · goal streak %3")
                  .arg(summary.value(QStringLiteral("goalNights")).toInt())
                  .arg(logged)
                  .arg(streak == 1 ? QStringLiteral("1 night") : QStringLiteral("%1 nights").arg(streak)));
}

void SleepFeature::saveManual()
{
    const QDate night = m_manualNight->date();
    const QPair<qint64, qint64> span = nightSpan(night, m_manualBed->time(), m_manualWake->time());
    const QString day = night.toString(Qt::ISODate);
    request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/nights/") + day),
            QJsonObject{{QStringLiteral("bedAt"), double(span.first)},
                        {QStringLiteral("wakeAt"), double(span.second)},
                        {QStringLiteral("awakeMinutes"), m_manualAwake->value()}},
            [this, day](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The night was not saved: %1").arg(error), true);
        m_manualAwake->setValue(0);
        load();
        setStatus(QStringLiteral("Saved the night you added for %1.").arg(nightLabel(day)));
    });
}

void SleepFeature::clearManual()
{
    const QString day = m_manualNight->date().toString(Qt::ISODate);
    request(QByteArrayLiteral("DELETE"), apiPath(QStringLiteral("/nights/") + day + QStringLiteral("?source=manual")), {},
            [this, day](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The added night was not removed: %1").arg(error), true);
        load();
        setStatus(QStringLiteral("Removed the night you added for %1.").arg(nightLabel(day)));
    });
}

void SleepFeature::saveGoal()
{
    request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/profile")),
            QJsonObject{{QStringLiteral("goalMinutes"), qRound(m_goal->value() * 60)},
                        {QStringLiteral("bedtime"), m_bedtime->time().toString(QStringLiteral("HH:mm"))},
                        {QStringLiteral("wakeTime"), m_wakeTime->time().toString(QStringLiteral("HH:mm"))},
                        {QStringLiteral("tzOffsetMinutes"), -QDateTime::currentDateTime().offsetFromUtc() / 60}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The goal was not saved: %1").arg(error), true);
        load();
    });
}

bool SleepFeature::saveExportBundle(const QJsonObject &payload)
{
    const QString suggested = text(payload, "filename").isEmpty()
        ? QStringLiteral("forkmesh-sleep-export.json") : text(payload, "filename");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save the Sleep export"),
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

void SleepFeature::exportMine()
{
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The export failed: %1").arg(error), true);
        saveExportBundle(payload);
    });
}

bool SleepFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
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

void SleepFeature::uninstallPlugin()
{
    if (QMessageBox::warning(
            this, QStringLiteral("Uninstall Sleep"),
            QStringLiteral("Uninstall Sleep from your account?\n\nYour nights are saved to a file first, "
                           "then deleted from the web dashboard, this desktop app, and the mobile app. "
                           "Apple Health keeps its own copy of your sleep."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Uninstall stopped — the export failed: %1").arg(error), true);
        if (exportHasData(payload) && !saveExportBundle(payload))
            return setStatus(QStringLiteral("Uninstall cancelled: the data file was not saved."), true);
        request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/uninstall")), {},
                [this](bool removed, const QJsonObject &, const QString &removeError) {
            if (!removed)
                return setStatus(QStringLiteral("Sleep was not uninstalled: %1").arg(removeError), true);
            m_checked = true;
            m_installed = false;
            m_overview = QJsonObject();
            applyInstallState();
            setStatus(QStringLiteral("Sleep uninstalled from your account. Install it again at any time."));
        });
    });
}

void SleepFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::sleep

namespace {
const forkmesh::market::NativePackageRegistration sleepRegistration({
    QStringLiteral("builtin:sleep"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Sleep"),
    QStringLiteral("moon"),
    QStringLiteral("/api/sleep"),
    QStringLiteral("forkmesh-sleep-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::sleep::SleepFeature(host, parent);
    },
});
} // namespace

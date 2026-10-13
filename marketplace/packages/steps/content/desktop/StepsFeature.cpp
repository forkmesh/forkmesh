#include "StepsFeature.h"

#include <QComboBox>
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
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::steps {
namespace {

constexpr double kMetersPerMile = 1609.344;

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

QString dayLabel(const QString &day)
{
    const QDate date = QDate::fromString(day, Qt::ISODate);
    if (!date.isValid())
        return day;
    if (date == QDate::currentDate())
        return QStringLiteral("Today");
    if (date == QDate::currentDate().addDays(-1))
        return QStringLiteral("Yesterday");
    return QLocale().toString(date, QStringLiteral("ddd d MMM"));
}

QColor barColor(bool dark) { return QColor(dark ? "#3987e5" : "#2a78d6"); }
QColor reachedColor(bool dark) { return QColor(dark ? "#199e70" : "#1baf7a"); }
QColor goalColor(bool dark) { return QColor(dark ? "#c98500" : "#eda100"); }

} // namespace

class StepsChart final : public QWidget
{
public:
    explicit StepsChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(190);
        setMouseTracking(true);
    }

    void setDays(const QJsonArray &days, int goal, bool imperial)
    {
        m_days = days;
        m_goal = goal;
        m_imperial = imperial;
        setAccessibleName(QStringLiteral("Steps per day for the last %1 days").arg(days.size()));
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
        double peak = qMax<double>(2000.0, m_goal * 1.15);
        for (const QJsonValue &value : m_days)
            peak = qMax(peak, number(value.toObject(), "steps") * 1.05);
        for (int step = 0; step <= 4; ++step) {
            const qreal y = plot.bottom() - plot.height() * step / 4;
            painter.setPen(QPen(grid, 1));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(0, y - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                             StepsFeature::stepsText(qRound64(peak * step / 4)));
        }
        if (m_days.isEmpty())
            return;
        const qreal slot = plot.width() / m_days.size();
        const qreal barWidth = qMin<qreal>(22, qMax<qreal>(3, slot - 4));
        for (int index = 0; index < m_days.size(); ++index) {
            const QJsonObject day = m_days.at(index).toObject();
            const double steps = number(day, "steps");
            const qreal x = plot.left() + index * slot + (slot - barWidth) / 2;
            if (steps > 0) {
                const qreal h = qMax<qreal>(2, plot.height() * steps / peak);
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_goal > 0 && steps >= m_goal ? reachedColor(dark) : barColor(dark));
                painter.drawRoundedRect(QRectF(x, plot.bottom() - h, barWidth, h), 3, 3);
            }
            if (index % 5 == 0 || index == m_days.size() - 1) {
                painter.setPen(palette().color(QPalette::PlaceholderText));
                painter.drawText(QRectF(x + barWidth / 2 - 30, plot.bottom() + 4, 60, 16),
                                 Qt::AlignHCenter | Qt::AlignTop,
                                 QDate::fromString(text(day, "day"), Qt::ISODate).toString(QStringLiteral("M/d")));
            }
        }
        if (m_goal > 0) {
            const qreal y = plot.bottom() - plot.height() * m_goal / peak;
            painter.setPen(QPen(goalColor(dark), 2, Qt::DashLine));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(plot.right() - 160, y - 18, 160, 16), Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("Goal %1").arg(StepsFeature::stepsText(m_goal)));
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
            return QWidget::event(event);
        const auto *help = static_cast<QHelpEvent *>(event);
        const qreal left = 56;
        const qreal plotWidth = qMax<qreal>(20, width() - 68);
        if (m_days.isEmpty() || help->pos().x() < left || help->pos().x() > left + plotWidth)
            return true;
        const int index = std::clamp(int((help->pos().x() - left) / (plotWidth / m_days.size())), 0,
                                     int(m_days.size()) - 1);
        const QJsonObject day = m_days.at(index).toObject();
        QToolTip::showText(help->globalPos(),
                           QStringLiteral("%1\n%2 steps · %3")
                               .arg(dayLabel(text(day, "day")),
                                    StepsFeature::stepsText(qint64(number(day, "steps"))),
                                    StepsFeature::distanceText(number(day, "distanceM"), m_imperial)),
                           this);
        return true;
    }

private:
    QJsonArray m_days;
    int m_goal = 0;
    bool m_imperial = false;
};

QString StepsFeature::stepsText(qint64 steps)
{
    return QLocale().toString(steps);
}

QString StepsFeature::distanceText(double meters, bool imperial)
{
    if (imperial)
        return QStringLiteral("%1 mi").arg(QLocale().toString(meters / kMetersPerMile, 'f', 2));
    return QStringLiteral("%1 km").arg(QLocale().toString(meters / 1000.0, 'f', 2));
}

bool StepsFeature::exportHasData(const QJsonObject &payload)
{
    return !payload.value(QStringLiteral("export")).toObject().value(QStringLiteral("days")).toArray().isEmpty();
}

StepsFeature::StepsFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("stepsSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Pedometer"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral("Steps your phone counts, synced through Apple Health")));
    heading->addStretch();
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_exportButton = new QPushButton(QStringLiteral("Export my data"));
    m_uninstallButton = new QPushButton(QStringLiteral("Uninstall…"));
    for (QPushButton *button : {refreshButton, m_exportButton, m_uninstallButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        heading->addWidget(button);
    }
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading Pedometer…"));
    outer->addWidget(m_status);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    auto *installNote = new QLabel(QStringLiteral(
        "Pedometer is private to your ForkMesh account. Install it, then turn on step counting in "
        "the ForkMesh iPhone app: it reads your steps from Apple Health and the motion sensor and "
        "sends each day's total here, to the web dashboard, and back to your phone. Every day is "
        "encrypted at rest with a key minted for your account alone."));
    installNote->setAlignment(Qt::AlignHCenter);
    installNote->setWordWrap(true);
    installLayout->addWidget(installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install Pedometer"));
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

    auto *todayRow = new QHBoxLayout;
    auto *todayColumn = new QVBoxLayout;
    m_todaySteps = new QLabel(QStringLiteral("0"));
    m_todaySteps->setObjectName(QStringLiteral("stepsToday"));
    m_todaySteps->setStyleSheet(QStringLiteral("font-size: 28px; font-weight: 600;"));
    todayColumn->addWidget(m_todaySteps);
    m_todayDetail = mutedLabel(QString());
    todayColumn->addWidget(m_todayDetail);
    m_goalBar = new QProgressBar;
    m_goalBar->setTextVisible(false);
    m_goalBar->setMaximumHeight(10);
    m_goalBar->setRange(0, 1000);
    todayColumn->addWidget(m_goalBar);
    m_sourceLabel = mutedLabel(QString());
    todayColumn->addWidget(m_sourceLabel);
    todayRow->addLayout(todayColumn, 2);
    auto *stats = new QGridLayout;
    stats->setHorizontalSpacing(24);
    m_weekLabel = new QLabel;
    m_averageLabel = new QLabel;
    m_streakLabel = new QLabel;
    m_bestLabel = new QLabel;
    const QList<QPair<QString, QLabel *>> statRows = {
        {QStringLiteral("Last 7 days"), m_weekLabel},
        {QStringLiteral("30-day average"), m_averageLabel},
        {QStringLiteral("Goal streak"), m_streakLabel},
        {QStringLiteral("Best day"), m_bestLabel},
    };
    for (int index = 0; index < statRows.size(); ++index) {
        stats->addWidget(mutedLabel(statRows.at(index).first), index / 2 * 2, index % 2);
        stats->addWidget(statRows.at(index).second, index / 2 * 2 + 1, index % 2);
    }
    todayRow->addLayout(stats, 3);
    layout->addLayout(todayRow);

    m_chart = new StepsChart;
    layout->addWidget(m_chart);

    auto *forms = new QHBoxLayout;
    forms->setSpacing(24);
    auto *manualBox = new QWidget;
    auto *manualForm = new QFormLayout(manualBox);
    manualForm->addRow(mutedLabel(QStringLiteral("Add a walk your phone missed. Steps added by hand count on top of Apple Health's total for the day.")));
    m_manualDay = new QDateEdit(QDate::currentDate());
    m_manualDay->setCalendarPopup(true);
    m_manualDay->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_manualDay->setMaximumDate(QDate::currentDate());
    manualForm->addRow(QStringLiteral("Day"), m_manualDay);
    m_manualSteps = new QSpinBox;
    m_manualSteps->setRange(0, 200000);
    m_manualSteps->setSingleStep(500);
    manualForm->addRow(QStringLiteral("Steps"), m_manualSteps);
    auto *distanceRow = new QHBoxLayout;
    m_manualDistance = new QDoubleSpinBox;
    m_manualDistance->setRange(0, 300);
    m_manualDistance->setDecimals(2);
    m_manualDistance->setSingleStep(0.5);
    distanceRow->addWidget(m_manualDistance);
    m_manualUnit = new QLabel(QStringLiteral("km"));
    distanceRow->addWidget(m_manualUnit);
    distanceRow->addStretch();
    manualForm->addRow(QStringLiteral("Distance"), distanceRow);
    auto *manualButtons = new QHBoxLayout;
    auto *saveManual = new QPushButton(QStringLiteral("Save steps"));
    saveManual->setObjectName(QStringLiteral("primaryButton"));
    auto *clearManual = new QPushButton(QStringLiteral("Remove added steps"));
    clearManual->setObjectName(QStringLiteral("ghostButton"));
    manualButtons->addWidget(saveManual);
    manualButtons->addWidget(clearManual);
    manualButtons->addStretch();
    manualForm->addRow(manualButtons);
    forms->addWidget(manualBox, 1);

    auto *goalBox = new QWidget;
    auto *goalForm = new QFormLayout(goalBox);
    m_goal = new QSpinBox;
    m_goal->setRange(0, 200000);
    m_goal->setSingleStep(500);
    goalForm->addRow(QStringLiteral("Daily goal"), m_goal);
    m_units = new QComboBox;
    m_units->addItem(QStringLiteral("Automatic"), QStringLiteral("auto"));
    m_units->addItem(QStringLiteral("Kilometres"), QStringLiteral("metric"));
    m_units->addItem(QStringLiteral("Miles"), QStringLiteral("imperial"));
    goalForm->addRow(QStringLiteral("Distance in"), m_units);
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

void StepsFeature::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                           ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Pedometer network access is unavailable."));
        return;
    }
    const QPointer<StepsFeature> guard(this);
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

QString StepsFeature::apiPath(const QString &suffix) const
{
    return QStringLiteral("/api/steps") + suffix;
}

void StepsFeature::refresh()
{
    load();
}

QList<forkmesh::market::NativePackageSearchHit> StepsFeature::searchHits(const QString &query,
                                                                         int limit) const
{
    const QString needle = query.trimmed().toLower();
    if (!m_installed || limit <= 0 || needle.size() < 3 ||
        !(QStringLiteral("steps pedometer walking distance").contains(needle)))
        return {};
    const QJsonObject today = m_overview.value(QStringLiteral("today")).toObject();
    return {{QStringLiteral("today"),
             QStringLiteral("%1 steps today").arg(stepsText(qint64(number(today, "steps")))),
             distanceText(number(today, "distanceM"), imperial())}};
}

void StepsFeature::activateSearchHit(const QString &)
{
    load();
}

void StepsFeature::applyInstallState()
{
    m_installPanel->setVisible(m_checked && !m_installed);
    m_content->setVisible(m_installed);
    m_exportButton->setVisible(m_installed);
    m_uninstallButton->setVisible(m_installed);
}

void StepsFeature::installPlugin()
{
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/install")),
            QJsonObject{{QStringLiteral("tzOffsetMinutes"), -QDateTime::currentDateTime().offsetFromUtc() / 60}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Pedometer was not installed: %1").arg(error), true);
        setStatus(QStringLiteral("Pedometer installed for your account."));
        load();
    });
}

void StepsFeature::load()
{
    if (m_loading)
        return;
    m_loading = true;
    request(QByteArrayLiteral("GET"),
            apiPath(QStringLiteral("?day=") + QDate::currentDate().toString(Qt::ISODate)), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            setStatus(QStringLiteral("Pedometer could not load: %1").arg(error), true);
            return;
        }
        m_checked = true;
        m_installed = payload.value(QStringLiteral("installed")).toBool();
        applyInstallState();
        if (!m_installed) {
            m_overview = QJsonObject();
            setStatus(QStringLiteral("Pedometer is not installed for your account yet."));
            return;
        }
        m_overview = payload;
        render();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
}

bool StepsFeature::imperial() const
{
    return text(m_overview.value(QStringLiteral("profile")).toObject(), "units") == QLatin1String("imperial");
}

void StepsFeature::render()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    const QJsonObject today = m_overview.value(QStringLiteral("today")).toObject();
    const QJsonObject summary = m_overview.value(QStringLiteral("summary")).toObject();
    const QJsonArray days = m_overview.value(QStringLiteral("days")).toArray();
    const int goal = profile.value(QStringLiteral("goal")).toInt();
    const qint64 steps = qint64(number(today, "steps"));
    const bool miles = imperial();
    m_todaySteps->setText(QStringLiteral("%1 steps").arg(stepsText(steps)));
    QStringList detail;
    if (goal > 0)
        detail << (steps >= goal ? QStringLiteral("Goal of %1 reached").arg(stepsText(goal))
                                 : QStringLiteral("%1 to go of %2").arg(stepsText(goal - steps), stepsText(goal)));
    detail << distanceText(number(today, "distanceM"), miles);
    if (number(today, "floors") > 0)
        detail << QStringLiteral("%1 floors").arg(qint64(number(today, "floors")));
    if (number(today, "activeKcal") > 0)
        detail << QStringLiteral("%1 active kcal").arg(qRound(number(today, "activeKcal")));
    m_todayDetail->setText(detail.join(QStringLiteral(" · ")));
    m_goalBar->setValue(goal > 0 ? int(std::min<qint64>(1000, steps * 1000 / goal)) : 0);
    const QJsonObject sources = today.value(QStringLiteral("sources")).toObject();
    const QJsonObject health = sources.value(QStringLiteral("health")).toObject();
    if (health.isEmpty()) {
        m_sourceLabel->setText(QStringLiteral(
            "Nothing from your phone yet today. Open Steps in the ForkMesh iPhone app and turn on Apple Health."));
    } else {
        const QDateTime synced = QDateTime::fromMSecsSinceEpoch(qint64(number(health, "updatedAt")));
        m_sourceLabel->setText(QStringLiteral("From Apple Health, synced %1%2")
                                   .arg(QLocale().toString(synced.time(), QLocale::ShortFormat),
                                        sources.contains(QStringLiteral("manual"))
                                            ? QStringLiteral(", plus steps you added by hand")
                                            : QString()));
    }
    m_weekLabel->setText(QStringLiteral("%1 steps · %2").arg(
        stepsText(qint64(number(summary, "weekSteps"))), distanceText(number(summary, "weekDistanceM"), miles)));
    m_averageLabel->setText(QStringLiteral("%1 steps").arg(stepsText(qint64(number(summary, "averageSteps")))));
    const int streak = summary.value(QStringLiteral("streak")).toInt();
    m_streakLabel->setText(streak == 1 ? QStringLiteral("1 day") : QStringLiteral("%1 days").arg(streak));
    m_bestLabel->setText(text(summary, "bestDay").isEmpty()
                             ? QStringLiteral("—")
                             : QStringLiteral("%1 · %2 steps").arg(dayLabel(text(summary, "bestDay")),
                                                                    stepsText(qint64(number(summary, "bestSteps")))));
    m_chart->setDays(days, goal, miles);
    m_goal->setValue(goal);
    const int unitIndex = m_units->findData(text(profile, "unitsPreference"));
    m_units->setCurrentIndex(unitIndex < 0 ? 0 : unitIndex);
    m_manualUnit->setText(miles ? QStringLiteral("mi") : QStringLiteral("km"));
    m_manualDay->setMaximumDate(QDate::currentDate());
    setStatus(QStringLiteral("%1 of the last 30 days met your goal.")
                  .arg(summary.value(QStringLiteral("goalDays")).toInt()));
}

void StepsFeature::saveManual()
{
    const double distance = m_manualDistance->value() * (imperial() ? kMetersPerMile : 1000.0);
    const QString day = m_manualDay->date().toString(Qt::ISODate);
    request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/days/") + day),
            QJsonObject{{QStringLiteral("steps"), m_manualSteps->value()},
                        {QStringLiteral("distanceM"), std::round(distance * 10) / 10}},
            [this, day](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The steps were not saved: %1").arg(error), true);
        m_manualSteps->setValue(0);
        m_manualDistance->setValue(0);
        load();
        setStatus(QStringLiteral("Saved the steps you added for %1.").arg(dayLabel(day)));
    });
}

void StepsFeature::clearManual()
{
    const QString day = m_manualDay->date().toString(Qt::ISODate);
    request(QByteArrayLiteral("DELETE"), apiPath(QStringLiteral("/days/") + day + QStringLiteral("?source=manual")), {},
            [this, day](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The added steps were not removed: %1").arg(error), true);
        load();
        setStatus(QStringLiteral("Removed the steps you added for %1.").arg(dayLabel(day)));
    });
}

void StepsFeature::saveGoal()
{
    request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/profile")),
            QJsonObject{{QStringLiteral("goal"), m_goal->value()},
                        {QStringLiteral("unitsPreference"), m_units->currentData().toString()}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The goal was not saved: %1").arg(error), true);
        load();
    });
}

bool StepsFeature::saveExportBundle(const QJsonObject &payload)
{
    const QString suggested = text(payload, "filename").isEmpty()
        ? QStringLiteral("forkmesh-steps-export.json") : text(payload, "filename");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save the Pedometer export"),
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

void StepsFeature::exportMine()
{
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The export failed: %1").arg(error), true);
        saveExportBundle(payload);
    });
}

bool StepsFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
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

void StepsFeature::uninstallPlugin()
{
    if (QMessageBox::warning(
            this, QStringLiteral("Uninstall Pedometer"),
            QStringLiteral("Uninstall Pedometer from your account?\n\nYour step history is saved to a file "
                           "first, then deleted from the web dashboard, this desktop app, and the mobile app. "
                           "Apple Health keeps its own copy of your steps."),
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
                return setStatus(QStringLiteral("Pedometer was not uninstalled: %1").arg(removeError), true);
            m_checked = true;
            m_installed = false;
            m_overview = QJsonObject();
            applyInstallState();
            setStatus(QStringLiteral("Pedometer uninstalled from your account. Install it again at any time."));
        });
    });
}

void StepsFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::steps

namespace {
const forkmesh::market::NativePackageRegistration stepsRegistration({
    QStringLiteral("builtin:steps"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Steps"),
    QStringLiteral("footprints"),
    QStringLiteral("/api/steps"),
    QStringLiteral("forkmesh-steps-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::steps::StepsFeature(host, parent);
    },
});
} // namespace

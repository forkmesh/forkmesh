#include "WaterFeature.h"

#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
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
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QTableWidget>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace forkmesh::water {
namespace {

constexpr double kMlPerFlOz = 29.5735295625;
constexpr double kMlPerCup = 8 * kMlPerFlOz;

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

QString drinkLabel(const QString &drink)
{
    if (drink.isEmpty())
        return QStringLiteral("Water");
    return drink.left(1).toUpper() + drink.mid(1);
}

QString sourceLabel(const QString &source)
{
    if (source == QLatin1String("health"))
        return QStringLiteral("Apple Health");
    if (source == QLatin1String("quick"))
        return QStringLiteral("Quick add");
    return QStringLiteral("Added by hand");
}

QColor barColor(bool dark) { return QColor(dark ? "#3aa7e0" : "#1f8fd1"); }
QColor reachedColor(bool dark) { return QColor(dark ? "#199e70" : "#1baf7a"); }
QColor goalColor(bool dark) { return QColor(dark ? "#c98500" : "#eda100"); }

} // namespace

class WaterChart final : public QWidget
{
public:
    explicit WaterChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(170);
        setMouseTracking(true);
    }

    void setDays(const QJsonArray &days, int goal, const QString &units)
    {
        m_days = days;
        m_goal = goal;
        m_units = units;
        setAccessibleName(QStringLiteral("Water per day for the last %1 days").arg(days.size()));
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
        const QRectF plot(64, 10, qMax<qreal>(20, width() - 76), qMax<qreal>(20, height() - 36));
        double peak = qMax<double>(1000.0, m_goal * 1.15);
        for (const QJsonValue &value : m_days)
            peak = qMax(peak, number(value.toObject(), "totalMl") * 1.05);
        for (int step = 0; step <= 4; ++step) {
            const qreal y = plot.bottom() - plot.height() * step / 4;
            painter.setPen(QPen(grid, 1));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(0, y - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                             WaterFeature::amountText(peak * step / 4, m_units));
        }
        if (m_days.isEmpty())
            return;
        const qreal slot = plot.width() / m_days.size();
        const qreal barWidth = qMin<qreal>(22, qMax<qreal>(3, slot - 4));
        for (int index = 0; index < m_days.size(); ++index) {
            const QJsonObject day = m_days.at(index).toObject();
            const double total = number(day, "totalMl");
            const qreal x = plot.left() + index * slot + (slot - barWidth) / 2;
            if (total > 0) {
                const qreal h = qMax<qreal>(2, plot.height() * total / peak);
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_goal > 0 && total >= m_goal ? reachedColor(dark) : barColor(dark));
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
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
            return QWidget::event(event);
        const auto *help = static_cast<QHelpEvent *>(event);
        const qreal left = 64;
        const qreal plotWidth = qMax<qreal>(20, width() - 76);
        if (m_days.isEmpty() || help->pos().x() < left || help->pos().x() > left + plotWidth)
            return true;
        const int index = std::clamp(int((help->pos().x() - left) / (plotWidth / m_days.size())), 0,
                                     int(m_days.size()) - 1);
        const QJsonObject day = m_days.at(index).toObject();
        QToolTip::showText(help->globalPos(),
                           QStringLiteral("%1\n%2 · %3 drinks")
                               .arg(text(day, "day"), WaterFeature::amountText(number(day, "totalMl"), m_units))
                               .arg(day.value(QStringLiteral("entries")).toInt()),
                           this);
        return true;
    }

private:
    QJsonArray m_days;
    int m_goal = 0;
    QString m_units;
};

double WaterFeature::unitMilliliters(const QString &units)
{
    if (units == QLatin1String("imperial"))
        return kMlPerFlOz;
    if (units == QLatin1String("cups"))
        return kMlPerCup;
    return 1;
}

QString WaterFeature::amountText(double milliliters, const QString &units)
{
    if (units == QLatin1String("cups")) {
        const double cups = std::round(milliliters / kMlPerCup * 10) / 10;
        return QStringLiteral("%1 %2").arg(QLocale().toString(cups, 'g', 6),
                                           cups == 1 ? QStringLiteral("cup") : QStringLiteral("cups"));
    }
    if (units == QLatin1String("imperial"))
        return QStringLiteral("%1 fl oz").arg(QLocale().toString(milliliters / kMlPerFlOz, 'f', milliliters >= 10 * kMlPerFlOz ? 0 : 1));
    if (milliliters >= 1000)
        return QStringLiteral("%1 L").arg(QLocale().toString(milliliters / 1000.0, 'f', 2));
    return QStringLiteral("%1 ml").arg(qRound(milliliters));
}

double WaterFeature::promptMilliliters(const QString &value, const QString &units)
{
    const bool imperial = units == QLatin1String("imperial") || units == QLatin1String("cups");
    static const QRegularExpression amount(
        QStringLiteral("(\\d+(?:[.,]\\d+)?)\\s*(ml|millilit(?:er|re)s?|l|lit(?:er|re)s?|oz|fl\\s*oz|ounces?|cups?|glass(?:es)?)?\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QString lower = value.trimmed().toLower();
    const QRegularExpressionMatch match = amount.match(lower);
    if (!match.hasMatch()) {
        if (lower.contains(QStringLiteral("glass")) || lower.contains(QStringLiteral("cup")))
            return imperial ? std::round(8 * kMlPerFlOz) : 250;
        if (lower.contains(QStringLiteral("bottle")))
            return 500;
        return 0;
    }
    const double quantity = match.captured(1).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
    const QString unit = match.captured(2).simplified();
    double milliliters = 0;
    if (unit.startsWith(QLatin1String("ml")) || unit.startsWith(QLatin1String("millilit")))
        milliliters = quantity;
    else if (unit == QLatin1String("l") || unit.startsWith(QLatin1String("lit")))
        milliliters = quantity * 1000;
    else if (unit.contains(QLatin1String("oz")) || unit.startsWith(QLatin1String("ounce")))
        milliliters = quantity * kMlPerFlOz;
    else if (unit.startsWith(QLatin1String("cup")) || unit.startsWith(QLatin1String("glass")))
        milliliters = quantity * (imperial ? 8 * kMlPerFlOz : 250);
    else
        milliliters = quantity * unitMilliliters(units);
    return std::round(std::clamp(milliliters, 0.0, 5000.0));
}

bool WaterFeature::exportHasData(const QJsonObject &payload)
{
    return !payload.value(QStringLiteral("export")).toObject().value(QStringLiteral("entries")).toArray().isEmpty();
}

WaterFeature::WaterFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_promptTargetChanged(host.promptTargetChanged)
{
    setObjectName(QStringLiteral("waterSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Water Intake"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addWidget(mutedLabel(QStringLiteral("Every glass you log, shared with Apple Health on your iPhone")));
    heading->addStretch();
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_exportButton = new QPushButton(QStringLiteral("Export my data"));
    m_uninstallButton = new QPushButton(QStringLiteral("Uninstall…"));
    for (QPushButton *button : {refreshButton, m_exportButton, m_uninstallButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        heading->addWidget(button);
    }
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading Water Intake…"));
    outer->addWidget(m_status);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    auto *installNote = new QLabel(QStringLiteral(
        "Water Intake is private to your ForkMesh account. Install it once and the same log appears on "
        "the web dashboard, this desktop app, and the mobile app. On iPhone it writes every drink to "
        "Apple Health and brings in water other apps logged there. Every drink is encrypted at rest "
        "with a key minted for your account alone."));
    installNote->setAlignment(Qt::AlignHCenter);
    installNote->setWordWrap(true);
    installLayout->addWidget(installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install Water Intake"));
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

    m_total = new QLabel(QStringLiteral("0 ml"));
    m_total->setObjectName(QStringLiteral("waterToday"));
    m_total->setStyleSheet(QStringLiteral("font-size: 28px; font-weight: 600;"));
    layout->addWidget(m_total);
    m_detail = mutedLabel(QString());
    layout->addWidget(m_detail);
    m_goalBar = new QProgressBar;
    m_goalBar->setTextVisible(false);
    m_goalBar->setMaximumHeight(10);
    m_goalBar->setRange(0, 1000);
    layout->addWidget(m_goalBar);

    m_presetRow = new QHBoxLayout;
    m_presetRow->addWidget(mutedLabel(QStringLiteral("Quick add")));
    m_presetRow->addStretch();
    layout->addLayout(m_presetRow);

    auto *customRow = new QHBoxLayout;
    customRow->addWidget(mutedLabel(QStringLiteral("Or log")));
    m_amount = new QDoubleSpinBox;
    m_amount->setRange(0, 5000);
    m_amount->setDecimals(0);
    m_amount->setValue(250);
    customRow->addWidget(m_amount);
    m_amountUnit = new QLabel(QStringLiteral("ml"));
    customRow->addWidget(m_amountUnit);
    customRow->addWidget(mutedLabel(QStringLiteral("of")));
    m_drink = new QComboBox;
    for (const QString &drink : {QStringLiteral("water"), QStringLiteral("sparkling"), QStringLiteral("tea"),
                                 QStringLiteral("coffee"), QStringLiteral("juice"), QStringLiteral("milk"),
                                 QStringLiteral("other")})
        m_drink->addItem(drinkLabel(drink), drink);
    customRow->addWidget(m_drink);
    auto *addButton = new QPushButton(QStringLiteral("Add drink"));
    addButton->setObjectName(QStringLiteral("primaryButton"));
    customRow->addWidget(addButton);
    customRow->addStretch();
    layout->addLayout(customRow);

    m_entries = new QTableWidget(0, 5);
    m_entries->setHorizontalHeaderLabels({QStringLiteral("Time"), QStringLiteral("Drink"), QStringLiteral("Amount"),
                                          QStringLiteral("From"), QString()});
    m_entries->verticalHeader()->setVisible(false);
    m_entries->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_entries->setSelectionMode(QAbstractItemView::NoSelection);
    m_entries->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_entries->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_entries->setMinimumHeight(160);
    layout->addWidget(m_entries);
    m_entriesEmpty = mutedLabel(QStringLiteral("Nothing logged today. Tap a quick add button after each glass."));
    layout->addWidget(m_entriesEmpty);

    m_chart = new WaterChart;
    layout->addWidget(m_chart);
    m_summary = mutedLabel(QString());
    layout->addWidget(m_summary);

    auto *goalBox = new QWidget;
    auto *goalForm = new QFormLayout(goalBox);
    auto *goalRow = new QHBoxLayout;
    m_goal = new QSpinBox;
    m_goal->setRange(0, 10000);
    m_goal->setSingleStep(250);
    goalRow->addWidget(m_goal);
    m_goalUnit = new QLabel(QStringLiteral("ml"));
    goalRow->addWidget(m_goalUnit);
    goalRow->addStretch();
    goalForm->addRow(QStringLiteral("Daily goal"), goalRow);
    m_units = new QComboBox;
    m_units->addItem(QStringLiteral("Automatic"), QStringLiteral("auto"));
    m_units->addItem(QStringLiteral("Millilitres"), QStringLiteral("metric"));
    m_units->addItem(QStringLiteral("Fluid ounces"), QStringLiteral("imperial"));
    m_units->addItem(QStringLiteral("Cups"), QStringLiteral("cups"));
    goalForm->addRow(QStringLiteral("Units"), m_units);
    auto *saveGoal = new QPushButton(QStringLiteral("Save goal"));
    goalForm->addRow(saveGoal);
    layout->addWidget(goalBox);
    layout->addStretch();
    scroll->setWidget(body);
    outer->addWidget(m_content, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { load(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this] { exportMine(); });
    connect(m_uninstallButton, &QPushButton::clicked, this, [this] { uninstallPlugin(); });
    connect(addButton, &QPushButton::clicked, this, [this] { addCustom(); });
    connect(saveGoal, &QPushButton::clicked, this, [this] { this->saveGoal(); });

    applyInstallState();
}

void WaterFeature::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                           ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Water Intake network access is unavailable."));
        return;
    }
    const QPointer<WaterFeature> guard(this);
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

QString WaterFeature::apiPath(const QString &suffix) const
{
    return QStringLiteral("/api/water") + suffix;
}

void WaterFeature::refresh()
{
    load();
}

void WaterFeature::applyInstallState()
{
    m_installPanel->setVisible(m_checked && !m_installed);
    m_content->setVisible(m_installed);
    m_exportButton->setVisible(m_installed);
    m_uninstallButton->setVisible(m_installed);
    if (m_promptTargetChanged)
        m_promptTargetChanged();
}

void WaterFeature::installPlugin()
{
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/install")),
            QJsonObject{{QStringLiteral("tzOffsetMinutes"), -QDateTime::currentDateTime().offsetFromUtc() / 60}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Water Intake was not installed: %1").arg(error), true);
        setStatus(QStringLiteral("Water Intake installed for your account."));
        load();
    });
}

void WaterFeature::load()
{
    if (m_loading)
        return;
    m_loading = true;
    request(QByteArrayLiteral("GET"),
            apiPath(QStringLiteral("?day=") + QDate::currentDate().toString(Qt::ISODate)), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            setStatus(QStringLiteral("Water Intake could not load: %1").arg(error), true);
            return;
        }
        m_checked = true;
        m_installed = payload.value(QStringLiteral("installed")).toBool();
        applyInstallState();
        if (!m_installed) {
            m_overview = QJsonObject();
            setStatus(QStringLiteral("Water Intake is not installed for your account yet."));
            return;
        }
        m_overview = payload;
        render();
    });
}

QString WaterFeature::units() const
{
    const QString chosen = text(m_overview.value(QStringLiteral("profile")).toObject(), "units");
    return chosen.isEmpty() ? QStringLiteral("metric") : chosen;
}

void WaterFeature::rebuildPresets(const QJsonArray &presets)
{
    for (QPushButton *button : std::as_const(m_presetButtons)) {
        m_presetRow->removeWidget(button);
        button->deleteLater();
    }
    m_presetButtons.clear();
    const QString units = this->units();
    for (const QJsonValue &value : presets) {
        const int milliliters = value.toInt();
        if (milliliters <= 0)
            continue;
        auto *button = new QPushButton(QStringLiteral("+ %1").arg(amountText(milliliters, units)));
        button->setObjectName(QStringLiteral("waterPreset"));
        connect(button, &QPushButton::clicked, this, [this, milliliters] {
            addDrink(milliliters, QStringLiteral("water"), QStringLiteral("quick"));
        });
        m_presetRow->insertWidget(m_presetRow->count() - 1, button);
        m_presetButtons.append(button);
    }
}

void WaterFeature::render()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    const QJsonObject today = m_overview.value(QStringLiteral("today")).toObject();
    const QJsonObject summary = m_overview.value(QStringLiteral("summary")).toObject();
    const QJsonArray entries = m_overview.value(QStringLiteral("entries")).toArray();
    const QString units = this->units();
    const int goal = profile.value(QStringLiteral("goalMl")).toInt();
    const double total = number(today, "totalMl");
    m_total->setText(amountText(total, units));
    m_detail->setText(goal <= 0 ? QStringLiteral("No daily goal set")
                      : total >= goal ? QStringLiteral("Goal of %1 reached").arg(amountText(goal, units))
                                      : QStringLiteral("%1 to go of %2").arg(amountText(goal - total, units),
                                                                              amountText(goal, units)));
    m_goalBar->setValue(goal > 0 ? int(std::min(1000.0, total * 1000 / goal)) : 0);
    rebuildPresets(profile.value(QStringLiteral("presetsMl")).toArray());
    const bool cups = units == QLatin1String("cups");
    const bool ounces = units == QLatin1String("imperial");
    m_amountUnit->setText(cups ? QStringLiteral("cups") : ounces ? QStringLiteral("fl oz") : QStringLiteral("ml"));
    m_amount->setDecimals(cups ? 2 : ounces ? 1 : 0);
    m_amount->setMaximum(std::floor(5000 / unitMilliliters(units) * 100) / 100);
    m_entries->setRowCount(0);
    for (int index = entries.size() - 1; index >= 0; --index) {
        const QJsonObject entry = entries.at(index).toObject();
        const int row = m_entries->rowCount();
        m_entries->insertRow(row);
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(qint64(number(entry, "at")));
        m_entries->setItem(row, 0, new QTableWidgetItem(QLocale().toString(at.time(), QLocale::ShortFormat)));
        m_entries->setItem(row, 1, new QTableWidgetItem(drinkLabel(text(entry, "drink"))));
        m_entries->setItem(row, 2, new QTableWidgetItem(amountText(number(entry, "amountMl"), units)));
        m_entries->setItem(row, 3, new QTableWidgetItem(sourceLabel(text(entry, "source"))));
        auto *remove = new QPushButton(QStringLiteral("Delete"));
        remove->setObjectName(QStringLiteral("ghostButton"));
        const QString id = text(entry, "id");
        connect(remove, &QPushButton::clicked, this, [this, id] { deleteEntry(id); });
        m_entries->setCellWidget(row, 4, remove);
    }
    m_entries->setVisible(!entries.isEmpty());
    m_entriesEmpty->setVisible(entries.isEmpty());
    m_chart->setDays(m_overview.value(QStringLiteral("days")).toArray(), goal, units);
    const int streak = summary.value(QStringLiteral("streak")).toInt();
    m_summary->setText(QStringLiteral("30-day average %1 · goal met on %2 days · streak %3 day%4")
                           .arg(amountText(number(summary, "averageMl"), units))
                           .arg(summary.value(QStringLiteral("goalDays")).toInt())
                           .arg(streak)
                           .arg(streak == 1 ? QString() : QStringLiteral("s")));
    m_goalUnit->setText(QStringLiteral("ml"));
    m_goal->setValue(goal);
    const int unitIndex = m_units->findData(text(profile, "unitsPreference"));
    m_units->setCurrentIndex(unitIndex < 0 ? 0 : unitIndex);
    setStatus(QStringLiteral("%1 drink%2 today.").arg(entries.size()).arg(entries.size() == 1 ? QString() : QStringLiteral("s")));
}

void WaterFeature::addDrink(int milliliters, const QString &drink, const QString &source)
{
    if (milliliters <= 0)
        return setStatus(QStringLiteral("Enter an amount first."), true);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/entries")),
            QJsonObject{{QStringLiteral("day"), QDate::currentDate().toString(Qt::ISODate)},
                        {QStringLiteral("amountMl"), milliliters},
                        {QStringLiteral("drink"), drink},
                        {QStringLiteral("source"), source},
                        {QStringLiteral("at"), QDateTime::currentMSecsSinceEpoch()}},
            [this, milliliters, drink](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The drink was not logged: %1").arg(error), true);
        load();
        setStatus(QStringLiteral("Logged %1 of %2.").arg(amountText(milliliters, units()), drinkLabel(drink).toLower()));
    });
}

void WaterFeature::addCustom()
{
    const double shown = m_amount->value();
    const int milliliters = int(std::round(shown * unitMilliliters(units())));
    addDrink(milliliters, m_drink->currentData().toString(), QStringLiteral("manual"));
}

void WaterFeature::deleteEntry(const QString &id)
{
    request(QByteArrayLiteral("DELETE"), apiPath(QStringLiteral("/entries/") + id), {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The drink was not deleted: %1").arg(error), true);
        load();
    });
}

void WaterFeature::saveGoal()
{
    request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/profile")),
            QJsonObject{{QStringLiteral("goalMl"), m_goal->value()},
                        {QStringLiteral("unitsPreference"), m_units->currentData().toString()}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The goal was not saved: %1").arg(error), true);
        load();
    });
}

forkmesh::market::NativePackagePromptTarget WaterFeature::hostPromptTarget() const
{
    if (!m_installed)
        return {};
    return {QStringLiteral("Log a drink: “500 ml”, “a glass of water”, “12 oz tea”…"),
            QStringLiteral("Log to Water Intake")};
}

bool WaterFeature::submitHostPrompt(const QString &value, const QStringList &attachments)
{
    if (!m_installed || !attachments.isEmpty())
        return false;
    const int milliliters = int(promptMilliliters(value, units()));
    if (milliliters <= 0) {
        setStatus(QStringLiteral("Say how much, like “500 ml” or “a glass of water”."), true);
        return false;
    }
    QString drink = QStringLiteral("water");
    const QString lower = value.toLower();
    for (const QString &candidate : {QStringLiteral("sparkling"), QStringLiteral("tea"), QStringLiteral("coffee"),
                                     QStringLiteral("juice"), QStringLiteral("milk")}) {
        if (lower.contains(candidate)) {
            drink = candidate;
            break;
        }
    }
    addDrink(milliliters, drink, QStringLiteral("manual"));
    return true;
}

bool WaterFeature::saveExportBundle(const QJsonObject &payload)
{
    const QString suggested = text(payload, "filename").isEmpty()
        ? QStringLiteral("forkmesh-water-export.json") : text(payload, "filename");
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save the Water Intake export"),
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

void WaterFeature::exportMine()
{
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The export failed: %1").arg(error), true);
        saveExportBundle(payload);
    });
}

bool WaterFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
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

void WaterFeature::uninstallPlugin()
{
    if (QMessageBox::warning(
            this, QStringLiteral("Uninstall Water Intake"),
            QStringLiteral("Uninstall Water Intake from your account?\n\nYour drinks log is saved to a file "
                           "first, then deleted from the web dashboard, this desktop app, and the mobile app. "
                           "Apple Health keeps the water already written to it."),
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
                return setStatus(QStringLiteral("Water Intake was not uninstalled: %1").arg(removeError), true);
            m_checked = true;
            m_installed = false;
            m_overview = QJsonObject();
            applyInstallState();
            setStatus(QStringLiteral("Water Intake uninstalled from your account. Install it again at any time."));
        });
    });
}

void WaterFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::water

namespace {
const forkmesh::market::NativePackageRegistration waterRegistration({
    QStringLiteral("builtin:water"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Water"),
    QStringLiteral("drop"),
    QStringLiteral("/api/water"),
    QStringLiteral("forkmesh-water-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::water::WaterFeature(host, parent);
    },
});
} // namespace

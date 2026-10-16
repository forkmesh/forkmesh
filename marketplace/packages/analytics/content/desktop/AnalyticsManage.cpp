#include "AnalyticsFeature.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::analytics {
namespace {

constexpr qint64 kManageHourMs = 3600000;

QString field(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

double amount(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toDouble();
}

QLabel *noteLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *headingLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setStyleSheet(QStringLiteral("font-weight: 600;"));
    return label;
}

QTableWidget *listTable(const QStringList &headers, const QString &name, bool selectable)
{
    auto *table = new QTableWidget(0, int(headers.size()));
    table->setObjectName(name);
    table->setAccessibleName(headers.first());
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(selectable ? QAbstractItemView::SingleSelection : QAbstractItemView::NoSelection);
    table->setFocusPolicy(selectable ? Qt::StrongFocus : Qt::NoFocus);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setMinimumHeight(150);
    return table;
}

void setCell(QTableWidget *table, int row, int column, const QString &value, bool right = false,
             const QString &id = QString())
{
    auto *item = new QTableWidgetItem(value);
    item->setToolTip(value);
    if (right)
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (!id.isEmpty())
        item->setData(Qt::UserRole, id);
    table->setItem(row, column, item);
}

void emptyRow(QTableWidget *table, const QString &message)
{
    table->setRowCount(0);
    table->insertRow(0);
    auto *item = new QTableWidgetItem(message);
    item->setFlags(Qt::ItemIsEnabled);
    item->setForeground(table->palette().color(QPalette::PlaceholderText));
    table->setItem(0, 0, item);
    table->setSpan(0, 0, 1, table->columnCount());
}

void resetTable(QTableWidget *table)
{
    table->clearSpans();
    table->setRowCount(0);
}

QString rowId(const QTableWidget *table)
{
    const QList<QTableWidgetItem *> picked = table->selectedItems();
    return picked.isEmpty() ? QString() : table->item(picked.first()->row(), 0)->data(Qt::UserRole).toString();
}

void selectId(QTableWidget *table, const QString &id)
{
    int row = 0;
    for (int index = 0; index < table->rowCount(); ++index) {
        const QTableWidgetItem *item = table->item(index, 0);
        if (item && !id.isEmpty() && item->data(Qt::UserRole).toString() == id)
            row = index;
    }
    if (table->rowCount() && table->item(row, 0) && !table->item(row, 0)->data(Qt::UserRole).toString().isEmpty())
        table->selectRow(row);
}

QString dateText(double ms)
{
    return ms > 0 ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(ms)).date(), QLocale::ShortFormat)
                  : QStringLiteral("—");
}

QString filterValue(const QJsonValue &value)
{
    if (value.isArray()) {
        QStringList parts;
        for (const QJsonValue &item : value.toArray())
            parts << item.toVariant().toString();
        return parts.join(QStringLiteral(", "));
    }
    return value.toVariant().toString();
}

const QList<std::pair<QString, QString>> &filterOperators()
{
    static const QList<std::pair<QString, QString>> operators{
        {QStringLiteral("exact"), QStringLiteral("equals")},
        {QStringLiteral("icontains"), QStringLiteral("contains")},
        {QStringLiteral("regex"), QStringLiteral("matches regex")},
        {QStringLiteral("is_not"), QStringLiteral("does not equal")},
        {QStringLiteral("is_set"), QStringLiteral("is set")},
        {QStringLiteral("gt"), QStringLiteral("greater than")},
        {QStringLiteral("lt"), QStringLiteral("less than")}};
    return operators;
}

} // namespace

QJsonObject AnalyticsFeature::goalSeries(const QJsonObject &goal)
{
    return QJsonObject{{QStringLiteral("event"), field(goal, "event")},
                       {QStringLiteral("label"), field(goal, "name")},
                       {QStringLiteral("math"), QStringLiteral("dau")},
                       {QStringLiteral("properties"), goal.value(QStringLiteral("properties")).toArray()}};
}

QJsonObject AnalyticsFeature::conversionsQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes,
                                               const QJsonArray &goals)
{
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("conversions")},
        {QStringLiteral("from"), double(nowMs - rangeHours * kManageHourMs)},
        {QStringLiteral("to"), double(nowMs)},
        {QStringLiteral("tzOffsetMinutes"), tzOffsetMinutes},
        {QStringLiteral("filterTestAccounts"), true},
        {QStringLiteral("interval"), rangeHours <= 48 ? QStringLiteral("hour")
                                     : rangeHours > 24 * 62 ? QStringLiteral("week")
                                                            : QStringLiteral("day")},
        {QStringLiteral("goals"), goals},
    };
}

QJsonObject AnalyticsFeature::funnelQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes,
                                          const QJsonObject &stored)
{
    QJsonObject query = stored;
    query.insert(QStringLiteral("kind"), QStringLiteral("funnel"));
    query.insert(QStringLiteral("from"), double(nowMs - rangeHours * kManageHourMs));
    query.insert(QStringLiteral("to"), double(nowMs));
    query.insert(QStringLiteral("tzOffsetMinutes"), tzOffsetMinutes);
    query.insert(QStringLiteral("filterTestAccounts"), true);
    return query;
}

QString AnalyticsFeature::rateText(double rate)
{
    const double percent = rate * 100;
    return QLocale().toString(percent, 'f', std::abs(percent) < 10 ? 1 : 0) + QLatin1Char('%');
}

QString AnalyticsFeature::variantSplit(const QJsonObject &flag)
{
    QStringList parts;
    for (const QJsonValue &value : flag.value(QStringLiteral("variants")).toArray()) {
        const QJsonObject variant = value.toObject();
        parts << QStringLiteral("%1 %2%").arg(field(variant, "key")).arg(int(amount(variant, "percent")));
    }
    return parts.isEmpty() ? QStringLiteral("on/off") : parts.join(QStringLiteral(" · "));
}

QWidget *AnalyticsFeature::buildConversionsPage()
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(10);

    auto *toolbar = new QHBoxLayout;
    m_conversionsNote = noteLabel(QStringLiteral(
        "A goal is any event a visitor sends: a copied install command, a sign-up click, a finished form."));
    toolbar->addWidget(m_conversionsNote, 1);
    m_playbook = new QPushButton(QStringLiteral("Add website playbook"));
    m_playbook->setObjectName(QStringLiteral("ghostButton"));
    m_playbook->setToolTip(QStringLiteral(
        "Adds the site's conversion goals, the homepage funnels and the homepage headline experiment"));
    m_editGoal = new QPushButton(QStringLiteral("Edit"));
    m_editGoal->setObjectName(QStringLiteral("ghostButton"));
    m_deleteGoal = new QPushButton(QStringLiteral("Delete"));
    m_deleteGoal->setObjectName(QStringLiteral("dangerButton"));
    m_newGoal = new QPushButton(QStringLiteral("New goal"));
    m_newGoal->setObjectName(QStringLiteral("primaryButton"));
    for (QPushButton *button : {m_playbook, m_editGoal, m_deleteGoal, m_newGoal})
        toolbar->addWidget(button);
    layout->addLayout(toolbar);

    layout->addWidget(headingLabel(QStringLiteral("Goals")));
    m_goals = listTable({QStringLiteral("Goal"), QStringLiteral("Event"), QStringLiteral("Converted"),
                         QStringLiteral("Rate"), QStringLiteral("Change"), QStringLiteral("Sessions"),
                         QStringLiteral("Session rate")},
                        QStringLiteral("analyticsGoals"), true);
    layout->addWidget(m_goals);

    m_breakdownTitle = headingLabel(QStringLiteral("By channel and landing page"));
    layout->addWidget(m_breakdownTitle);
    auto *breakdowns = new QHBoxLayout;
    breakdowns->setSpacing(16);
    m_goalChannels = listTable({QStringLiteral("Channel"), QStringLiteral("Sessions"), QStringLiteral("Converted"),
                                QStringLiteral("Rate")},
                               QStringLiteral("analyticsGoalChannels"), false);
    m_goalLanding = listTable({QStringLiteral("Landing page"), QStringLiteral("Sessions"),
                               QStringLiteral("Converted"), QStringLiteral("Rate")},
                              QStringLiteral("analyticsGoalLanding"), false);
    breakdowns->addWidget(m_goalChannels, 2);
    breakdowns->addWidget(m_goalLanding, 3);
    layout->addLayout(breakdowns);

    auto *funnelRow = new QHBoxLayout;
    funnelRow->addWidget(headingLabel(QStringLiteral("Funnel")));
    m_funnel = new QComboBox;
    m_funnel->setObjectName(QStringLiteral("analyticsFunnel"));
    m_funnel->setAccessibleName(QStringLiteral("Saved funnel"));
    m_funnel->setMinimumWidth(220);
    funnelRow->addWidget(m_funnel);
    funnelRow->addStretch();
    layout->addLayout(funnelRow);
    m_funnelSteps = listTable({QStringLiteral("Step"), QStringLiteral("Visitors"), QStringLiteral("Of first step"),
                               QStringLiteral("From previous"), QStringLiteral("Dropped"),
                               QStringLiteral("Median time")},
                              QStringLiteral("analyticsFunnelSteps"), false);
    layout->addWidget(m_funnelSteps);
    layout->addStretch();
    scroll->setWidget(body);

    connect(m_goals, &QTableWidget::itemSelectionChanged, this, [this] { renderGoalBreakdown(); });
    connect(m_goals, &QTableWidget::itemDoubleClicked, this, [this] {
        if (!selectedGoal().isEmpty())
            editGoal(selectedGoal());
    });
    connect(m_newGoal, &QPushButton::clicked, this, [this] { editGoal({}); });
    connect(m_editGoal, &QPushButton::clicked, this, [this] {
        if (!selectedGoal().isEmpty())
            editGoal(selectedGoal());
    });
    connect(m_deleteGoal, &QPushButton::clicked, this, [this] { deleteGoal(); });
    connect(m_playbook, &QPushButton::clicked, this, [this] { applyPlaybook(); });
    connect(m_funnel, &QComboBox::currentIndexChanged, this, [this](int) { loadFunnel(); });
    return scroll;
}

QWidget *AnalyticsFeature::buildExperimentsPage()
{
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(10);

    auto *toolbar = new QHBoxLayout;
    m_experimentsNote = noteLabel(QStringLiteral(
        "An experiment splits visitors with a feature flag and compares how often each variant reaches a goal."));
    toolbar->addWidget(m_experimentsNote, 1);
    m_toggleExperiment = new QPushButton(QStringLiteral("Stop"));
    m_toggleExperiment->setObjectName(QStringLiteral("ghostButton"));
    m_newExperiment = new QPushButton(QStringLiteral("New experiment"));
    m_newExperiment->setObjectName(QStringLiteral("primaryButton"));
    toolbar->addWidget(m_toggleExperiment);
    toolbar->addWidget(m_newExperiment);
    layout->addLayout(toolbar);

    m_experimentTable = listTable({QStringLiteral("Experiment"), QStringLiteral("Flag"), QStringLiteral("Status"),
                                   QStringLiteral("Split"), QStringLiteral("Started"), QStringLiteral("Goal")},
                                  QStringLiteral("analyticsExperiments"), true);
    layout->addWidget(m_experimentTable);
    m_experimentDetail = noteLabel(QString());
    m_experimentDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_experimentDetail);
    layout->addWidget(headingLabel(QStringLiteral("Results")));
    m_variants = listTable({QStringLiteral("Variant"), QStringLiteral("Visitors"), QStringLiteral("Converted"),
                            QStringLiteral("Rate"), QStringLiteral("Lift"),
                            QStringLiteral("Chance to beat control"), QStringLiteral("Result")},
                           QStringLiteral("analyticsVariants"), false);
    layout->addWidget(m_variants);
    layout->addStretch();
    scroll->setWidget(body);

    connect(m_experimentTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QJsonObject experiment = selectedExperiment();
        m_toggleExperiment->setText(field(experiment, "status") == QLatin1String("running") ? QStringLiteral("Stop")
                                                                                            : QStringLiteral("Start"));
        m_toggleExperiment->setEnabled(canManage() && !experiment.isEmpty());
        loadExperimentResult();
    });
    connect(m_toggleExperiment, &QPushButton::clicked, this, [this] {
        setExperimentRunning(field(selectedExperiment(), "status") != QLatin1String("running"));
    });
    connect(m_newExperiment, &QPushButton::clicked, this, [this] { newExperiment(); });
    return scroll;
}

QString AnalyticsFeature::projectPath(const QString &suffix) const
{
    return organizationPath(QStringLiteral("/projects/") + m_project->currentData().toString() + suffix);
}

bool AnalyticsFeature::canManage() const
{
    const QString role = field(organizationRecord(), "role");
    return m_projects.value(QStringLiteral("canManage"))
        .toBool(role == QLatin1String("owner") || role == QLatin1String("admin"));
}

void AnalyticsFeature::showSection(int index)
{
    m_section = index;
    m_sections->setCurrentIndex(index);
    applyState();
    reloadSection();
}

void AnalyticsFeature::reloadSection()
{
    if (organization().isEmpty() || m_project->currentData().toString().isEmpty())
        return;
    if (m_section == 1)
        loadConversions();
    else if (m_section == 2)
        loadExperiments();
    else
        loadOverview();
}

void AnalyticsFeature::clearManaged()
{
    m_goalList = QJsonArray();
    m_insights = QJsonArray();
    m_flags = QJsonArray();
    m_experiments = QJsonArray();
    m_conversions = QJsonObject();
    ++m_conversionSerial;
    ++m_experimentSerial;
    for (QTableWidget *table : {m_goals, m_goalChannels, m_goalLanding, m_funnelSteps, m_experimentTable, m_variants})
        resetTable(table);
    m_funnel->blockSignals(true);
    m_funnel->clear();
    m_funnel->blockSignals(false);
    m_experimentDetail->clear();
}

void AnalyticsFeature::loadObjects(const QString &kind, std::function<void(const QJsonArray &)> done)
{
    request(QByteArrayLiteral("GET"), projectPath(QStringLiteral("/objects?kind=") + kind), {},
            [this, kind, done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            setStatus(QStringLiteral("Analytics could not load %1s: %2").arg(kind, error), true);
        done(ok ? payload.value(QStringLiteral("objects")).toArray() : QJsonArray());
    });
}

void AnalyticsFeature::loadConversions()
{
    const int serial = ++m_conversionSerial;
    setStatus(QStringLiteral("Loading conversions for the last %1…").arg(m_rangeGroup->button(m_rangeHours)->text()));
    loadObjects(QStringLiteral("insight"), [this, serial](const QJsonArray &insights) {
        if (serial != m_conversionSerial)
            return;
        m_insights = insights;
        renderFunnels();
    });
    loadObjects(QStringLiteral("goal"), [this, serial](const QJsonArray &goals) {
        if (serial != m_conversionSerial)
            return;
        QList<QJsonObject> ordered;
        for (const QJsonValue &value : goals)
            ordered.append(value.toObject());
        std::stable_sort(ordered.begin(), ordered.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("pinned")).toBool() && !b.value(QStringLiteral("pinned")).toBool();
        });
        m_goalList = QJsonArray();
        for (const QJsonObject &goal : ordered)
            m_goalList.append(goal);
        if (m_goalList.isEmpty()) {
            m_conversions = QJsonObject();
            renderGoals();
            return setStatus(canManage() ? QStringLiteral("No conversion goals yet. Add one, or add the website playbook.")
                                         : QStringLiteral("No conversion goals yet."));
        }
        QJsonArray series;
        for (int index = 0; index < m_goalList.size() && index < 10; ++index)
            series.append(goalSeries(m_goalList.at(index).toObject()));
        request(QByteArrayLiteral("POST"), projectPath(QStringLiteral("/query")),
                conversionsQuery(QDateTime::currentMSecsSinceEpoch(), m_rangeHours,
                                 QDateTime::currentDateTime().offsetFromUtc() / 60, series),
                [this, serial](bool ok, const QJsonObject &payload, const QString &error) {
            if (serial != m_conversionSerial)
                return;
            if (!ok)
                return setStatus(QStringLiteral("Conversions could not load: %1").arg(error), true);
            m_conversions = payload;
            renderGoals();
            const QJsonObject totals = payload.value(QStringLiteral("result")).toObject()
                                           .value(QStringLiteral("totals")).toObject();
            setStatus(QStringLiteral("%1 visitors and %2 sessions in the last %3%4")
                          .arg(countText(amount(totals, "visitors")), countText(amount(totals, "sessions")),
                               m_rangeGroup->button(m_rangeHours)->text(),
                               payload.value(QStringLiteral("truncated")).toBool()
                                   ? QStringLiteral(" · only the most recent events were scanned")
                                   : QString()));
        });
    });
}

void AnalyticsFeature::renderGoals()
{
    const QString previous = rowId(m_goals);
    const QJsonArray results = m_conversions.value(QStringLiteral("result")).toObject()
                                   .value(QStringLiteral("goals")).toArray();
    m_goals->blockSignals(true);
    resetTable(m_goals);
    for (int index = 0; index < m_goalList.size(); ++index) {
        const QJsonObject goal = m_goalList.at(index).toObject();
        const QJsonObject row = index < results.size() ? results.at(index).toObject() : QJsonObject();
        const QJsonObject before = row.value(QStringLiteral("previous")).toObject();
        const bool measured = !row.isEmpty();
        m_goals->insertRow(index);
        setCell(m_goals, index, 0, field(goal, "name").isEmpty() ? QStringLiteral("Untitled goal") : field(goal, "name"),
                false, field(goal, "id"));
        QString event = field(goal, "event");
        const QJsonArray filters = goal.value(QStringLiteral("properties")).toArray();
        if (!filters.isEmpty()) {
            const QJsonObject first = filters.first().toObject();
            event += QStringLiteral(" · %1 %2 %3").arg(field(first, "property"), field(first, "operator"),
                                                       filterValue(first.value(QStringLiteral("value"))));
        }
        setCell(m_goals, index, 1, event);
        setCell(m_goals, index, 2, measured ? countText(amount(row, "conversions")) : QStringLiteral("—"), true);
        setCell(m_goals, index, 3, measured ? rateText(amount(row, "rate")) : QStringLiteral("—"), true);
        const QString change = measured ? changeText(amount(row, "rate"), amount(before, "rate")) : QString();
        setCell(m_goals, index, 4, change, true);
        if (QTableWidgetItem *item = m_goals->item(index, 4); item && !change.isEmpty() && change != QLatin1String("+0%"))
            item->setForeground(QColor(change.startsWith(QLatin1Char('-')) ? "#d1453b" : "#1baf7a"));
        setCell(m_goals, index, 5, measured ? countText(amount(row, "sessions")) : QStringLiteral("—"), true);
        setCell(m_goals, index, 6, measured ? rateText(amount(row, "sessionRate")) : QStringLiteral("—"), true);
    }
    if (m_goalList.isEmpty())
        emptyRow(m_goals, QStringLiteral("No goals yet. A goal is any event, such as a sign-up click."));
    selectId(m_goals, previous);
    m_goals->blockSignals(false);
    m_editGoal->setEnabled(!m_goalList.isEmpty());
    m_deleteGoal->setEnabled(!m_goalList.isEmpty());
    renderGoalBreakdown();
}

void AnalyticsFeature::renderGoalBreakdown()
{
    const QJsonObject goal = selectedGoal();
    int position = -1;
    for (int index = 0; index < m_goalList.size() && index < 10; ++index)
        if (field(m_goalList.at(index).toObject(), "id") == field(goal, "id"))
            position = index;
    m_breakdownTitle->setText(goal.isEmpty() ? QStringLiteral("By channel and landing page")
                                             : QStringLiteral("%1 by channel and landing page").arg(field(goal, "name")));
    const QJsonObject result = m_conversions.value(QStringLiteral("result")).toObject();
    const auto fill = [&](QTableWidget *table, const char *key, const QString &none) {
        resetTable(table);
        const QJsonArray rows = result.value(QLatin1String(key)).toArray();
        if (position < 0 || rows.isEmpty())
            return emptyRow(table, none);
        for (const QJsonValue &value : rows) {
            const QJsonObject row = value.toObject();
            const int index = table->rowCount();
            table->insertRow(index);
            setCell(table, index, 0, field(row, "value"));
            setCell(table, index, 1, countText(amount(row, "sessions")), true);
            setCell(table, index, 2, countText(row.value(QStringLiteral("conversions")).toArray().at(position).toDouble()),
                    true);
            setCell(table, index, 3, rateText(row.value(QStringLiteral("rates")).toArray().at(position).toDouble()), true);
        }
    };
    fill(m_goalChannels, "channels", QStringLiteral("No sessions in this range"));
    fill(m_goalLanding, "landing", QStringLiteral("No sessions in this range"));
}

void AnalyticsFeature::renderFunnels()
{
    const QString previous = m_funnel->currentData().toString();
    m_funnel->blockSignals(true);
    m_funnel->clear();
    for (const QJsonValue &value : std::as_const(m_insights)) {
        const QJsonObject insight = value.toObject();
        if (field(insight.value(QStringLiteral("query")).toObject(), "kind") == QLatin1String("funnel"))
            m_funnel->addItem(field(insight, "name").isEmpty() ? QStringLiteral("Untitled funnel") : field(insight, "name"),
                              field(insight, "id"));
    }
    const int index = m_funnel->findData(previous);
    m_funnel->setCurrentIndex(index >= 0 ? index : 0);
    m_funnel->setEnabled(m_funnel->count() > 0);
    m_funnel->blockSignals(false);
    loadFunnel();
}

void AnalyticsFeature::loadFunnel()
{
    const QString id = m_funnel->currentData().toString();
    QJsonObject stored;
    for (const QJsonValue &value : std::as_const(m_insights))
        if (field(value.toObject(), "id") == id)
            stored = value.toObject().value(QStringLiteral("query")).toObject();
    if (stored.isEmpty())
        return emptyRow(m_funnelSteps, QStringLiteral(
            "No saved funnels. Add the website playbook, or save a funnel insight on the web dashboard."));
    const int serial = m_conversionSerial;
    request(QByteArrayLiteral("POST"), projectPath(QStringLiteral("/query")),
            funnelQuery(QDateTime::currentMSecsSinceEpoch(), m_rangeHours,
                        QDateTime::currentDateTime().offsetFromUtc() / 60, stored),
            [this, serial, id](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_conversionSerial || id != m_funnel->currentData().toString())
            return;
        if (!ok)
            return emptyRow(m_funnelSteps, QStringLiteral("The funnel could not load: %1").arg(error));
        const QJsonArray steps = payload.value(QStringLiteral("result")).toObject()
                                     .value(QStringLiteral("steps")).toArray();
        resetTable(m_funnelSteps);
        for (int index = 0; index < steps.size(); ++index) {
            const QJsonObject step = steps.at(index).toObject();
            m_funnelSteps->insertRow(index);
            setCell(m_funnelSteps, index, 0, QStringLiteral("%1. %2").arg(index + 1).arg(field(step, "name")));
            setCell(m_funnelSteps, index, 1, countText(amount(step, "count")), true);
            setCell(m_funnelSteps, index, 2, rateText(amount(step, "conversion")), true);
            setCell(m_funnelSteps, index, 3, index ? rateText(amount(step, "stepConversion")) : QString(), true);
            setCell(m_funnelSteps, index, 4, index ? countText(amount(step, "dropoff")) : QString(), true);
            setCell(m_funnelSteps, index, 5,
                    index ? durationText(qint64(amount(step, "medianTimeMs"))) : QString(), true);
        }
        if (steps.isEmpty())
            emptyRow(m_funnelSteps, QStringLiteral("This funnel needs at least two steps."));
    });
}

QJsonObject AnalyticsFeature::selectedGoal() const
{
    const QString id = rowId(m_goals);
    for (const QJsonValue &value : m_goalList)
        if (field(value.toObject(), "id") == id)
            return value.toObject();
    return {};
}

void AnalyticsFeature::editGoal(const QJsonObject &goal)
{
    const bool existing = !field(goal, "id").isEmpty();
    QDialog dialog(this);
    dialog.setWindowTitle(existing ? QStringLiteral("Edit goal") : QStringLiteral("New goal"));
    dialog.resize(560, 360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(field(goal, "name"));
    name->setMaxLength(120);
    name->setPlaceholderText(QStringLiteral("e.g. Install intent"));
    auto *description = new QLineEdit(field(goal, "description"));
    description->setMaxLength(1000);
    auto *event = new QComboBox;
    event->setEditable(true);
    QStringList events{QStringLiteral("forkmesh_product_action"), QStringLiteral("$pageview"),
                       QStringLiteral("$autocapture")};
    for (const QJsonValue &value : m_overview.value(QStringLiteral("result")).toObject()
                                       .value(QStringLiteral("breakdowns")).toObject()
                                       .value(QStringLiteral("events")).toArray())
        if (!events.contains(field(value.toObject(), "value")))
            events << field(value.toObject(), "value");
    event->addItems(events);
    event->setCurrentText(field(goal, "event").isEmpty() ? events.first() : field(goal, "event"));
    const QJsonArray filters = goal.value(QStringLiteral("properties")).toArray();
    const QJsonObject first = filters.isEmpty() ? QJsonObject() : filters.first().toObject();
    auto *property = new QLineEdit(field(first, "property"));
    property->setPlaceholderText(QStringLiteral("Property, e.g. stage (optional)"));
    auto *op = new QComboBox;
    for (const auto &[key, label] : filterOperators())
        op->addItem(label, key);
    op->setCurrentIndex(std::max(0, op->findData(field(first, "operator"))));
    auto *value = new QLineEdit(filterValue(first.value(QStringLiteral("value"))));
    value->setPlaceholderText(QStringLiteral("Value, e.g. install"));
    auto *filterRow = new QHBoxLayout;
    filterRow->addWidget(property, 2);
    filterRow->addWidget(op, 1);
    filterRow->addWidget(value, 2);
    auto *pinned = new QCheckBox(QStringLiteral("Pin to the top of the list"));
    pinned->setChecked(goal.value(QStringLiteral("pinned")).toBool());
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Event"), event);
    form->addRow(QStringLiteral("Only when"), filterRow);
    form->addRow(QString(), pinned);
    layout->addLayout(form);
    if (filters.size() > 1)
        layout->addWidget(noteLabel(QStringLiteral("%1 more conditions set on the web dashboard are kept.")
                                        .arg(filters.size() - 1)));
    auto *error = new QLabel;
    error->setObjectName(QStringLiteral("errorLabel"));
    error->setVisible(false);
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (name->text().trimmed().isEmpty() || event->currentText().trimmed().isEmpty()) {
            error->setText(QStringLiteral("Give the goal a name and an event."));
            error->setVisible(true);
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonArray properties;
    if (!property->text().trimmed().isEmpty()) {
        QJsonObject filter{{QStringLiteral("type"), QStringLiteral("event")},
                           {QStringLiteral("property"), property->text().trimmed()},
                           {QStringLiteral("operator"), op->currentData().toString()}};
        if (op->currentData().toString() != QLatin1String("is_set"))
            filter.insert(QStringLiteral("value"), value->text().trimmed());
        properties.append(filter);
    }
    for (int index = 1; index < filters.size(); ++index)
        properties.append(filters.at(index));
    QJsonObject body{{QStringLiteral("name"), name->text().trimmed()},
                     {QStringLiteral("description"), description->text().trimmed()},
                     {QStringLiteral("event"), event->currentText().trimmed()},
                     {QStringLiteral("properties"), properties},
                     {QStringLiteral("pinned"), pinned->isChecked()}};
    if (!existing)
        body.insert(QStringLiteral("kind"), QStringLiteral("goal"));
    request(existing ? QByteArrayLiteral("PATCH") : QByteArrayLiteral("POST"),
            projectPath(QStringLiteral("/objects") + (existing ? QStringLiteral("/") + field(goal, "id") : QString())),
            body, [this](bool ok, const QJsonObject &, const QString &message) {
        if (!ok)
            return setStatus(QStringLiteral("The goal was not saved: %1").arg(message), true);
        setStatus(QStringLiteral("Goal saved."));
        loadConversions();
    });
}

void AnalyticsFeature::deleteGoal()
{
    const QJsonObject goal = selectedGoal();
    if (goal.isEmpty()
        || QMessageBox::question(this, QStringLiteral("Delete goal"),
                                 QStringLiteral("Delete the goal “%1”? Its events are kept.").arg(field(goal, "name")))
               != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"), projectPath(QStringLiteral("/objects/") + field(goal, "id")), {},
            [this](bool ok, const QJsonObject &, const QString &message) {
        if (!ok)
            return setStatus(QStringLiteral("The goal was not deleted: %1").arg(message), true);
        setStatus(QStringLiteral("Goal deleted."));
        loadConversions();
    });
}

void AnalyticsFeature::applyPlaybook()
{
    if (QMessageBox::question(
            this, QStringLiteral("Add website playbook"),
            QStringLiteral("Add the website playbook to %1? It adds four conversion goals, the “Homepage → install” "
                           "and “Homepage → account” funnels, and turns on the homepage headline experiment "
                           "(flag home-hero-headline, 50/50).").arg(projectName(projectRecord())))
        != QMessageBox::Yes)
        return;
    m_playbook->setEnabled(false);
    request(QByteArrayLiteral("POST"), projectPath(QStringLiteral("/playbook")), {},
            [this](bool ok, const QJsonObject &payload, const QString &message) {
        m_playbook->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("The playbook was not added: %1").arg(message), true);
        const QJsonObject project = payload.value(QStringLiteral("project")).toObject();
        QJsonArray projects = m_projects.value(QStringLiteral("projects")).toArray();
        for (int index = 0; index < projects.size(); ++index)
            if (field(projects.at(index).toObject(), "id") == field(project, "id"))
                projects.replace(index, project);
        m_projects.insert(QStringLiteral("projects"), projects);
        const QJsonObject added = payload.value(QStringLiteral("added")).toObject();
        applyState();
        loadConversions();
        setStatus(QStringLiteral("Website playbook added: %1 goals, %2 funnels, %3 flag and %4 experiment.")
                      .arg(int(amount(added, "goal"))).arg(int(amount(added, "insight")))
                      .arg(int(amount(added, "flag"))).arg(int(amount(added, "experiment"))));
    });
}

QJsonObject AnalyticsFeature::flagFor(const QString &key) const
{
    for (const QJsonValue &value : m_flags)
        if (field(value.toObject(), "key") == key)
            return value.toObject();
    return {};
}

QJsonObject AnalyticsFeature::selectedExperiment() const
{
    const QString id = rowId(m_experimentTable);
    for (const QJsonValue &value : m_experiments)
        if (field(value.toObject(), "id") == id)
            return value.toObject();
    return {};
}

void AnalyticsFeature::loadExperiments()
{
    const int serial = ++m_experimentSerial;
    setStatus(QStringLiteral("Loading experiments…"));
    loadObjects(QStringLiteral("flag"), [this, serial](const QJsonArray &flags) {
        if (serial != m_experimentSerial)
            return;
        m_flags = flags;
        loadObjects(QStringLiteral("experiment"), [this, serial](const QJsonArray &experiments) {
            if (serial != m_experimentSerial)
                return;
            m_experiments = experiments;
            renderExperiments();
            setStatus(m_experiments.isEmpty() ? QStringLiteral("No experiments yet.")
                                              : QStringLiteral("%1 experiments").arg(m_experiments.size()));
        });
    });
}

void AnalyticsFeature::renderExperiments()
{
    QString previous = rowId(m_experimentTable);
    if (previous.isEmpty())
        for (const QJsonValue &value : std::as_const(m_experiments))
            if (previous.isEmpty() && field(value.toObject(), "status") == QLatin1String("running"))
                previous = field(value.toObject(), "id");
    m_experimentTable->blockSignals(true);
    resetTable(m_experimentTable);
    for (int index = 0; index < m_experiments.size(); ++index) {
        const QJsonObject experiment = m_experiments.at(index).toObject();
        const QJsonObject flag = flagFor(field(experiment, "flagKey"));
        const QString status = field(experiment, "status");
        const QJsonObject goal = experiment.value(QStringLiteral("goal")).toObject();
        m_experimentTable->insertRow(index);
        setCell(m_experimentTable, index, 0, field(experiment, "name").isEmpty() ? QStringLiteral("Untitled experiment")
                                                                                : field(experiment, "name"),
                false, field(experiment, "id"));
        setCell(m_experimentTable, index, 1, field(experiment, "flagKey"));
        setCell(m_experimentTable, index, 2,
                status == QLatin1String("running")   ? QStringLiteral("Running")
                : status == QLatin1String("stopped") ? QStringLiteral("Stopped")
                                                     : QStringLiteral("Draft"));
        setCell(m_experimentTable, index, 3,
                flag.isEmpty() ? QStringLiteral("flag missing")
                               : variantSplit(flag) + (flag.value(QStringLiteral("active")).toBool()
                                                           ? QString() : QStringLiteral(" · flag off")));
        setCell(m_experimentTable, index, 4, dateText(amount(experiment, "startedAt")));
        setCell(m_experimentTable, index, 5, field(goal, "label").isEmpty() ? field(goal, "event") : field(goal, "label"));
    }
    if (m_experiments.isEmpty())
        emptyRow(m_experimentTable, canManage() ? QStringLiteral("No experiments yet. Start one with New experiment.")
                                                : QStringLiteral("No experiments yet."));
    selectId(m_experimentTable, previous);
    m_experimentTable->blockSignals(false);
    const QJsonObject experiment = selectedExperiment();
    m_toggleExperiment->setText(field(experiment, "status") == QLatin1String("running") ? QStringLiteral("Stop")
                                                                                        : QStringLiteral("Start"));
    m_toggleExperiment->setEnabled(canManage() && !experiment.isEmpty());
    loadExperimentResult();
}

void AnalyticsFeature::loadExperimentResult()
{
    const QJsonObject experiment = selectedExperiment();
    if (experiment.isEmpty()) {
        m_experimentDetail->clear();
        return emptyRow(m_variants, QStringLiteral("Select an experiment to see its results."));
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double started = amount(experiment, "startedAt");
    const double ended = amount(experiment, "endedAt");
    const QJsonObject query{
        {QStringLiteral("kind"), QStringLiteral("experiment")},
        {QStringLiteral("flagKey"), field(experiment, "flagKey")},
        {QStringLiteral("goal"), experiment.value(QStringLiteral("goal")).toObject()},
        {QStringLiteral("from"), started > 0 ? started : double(now - m_rangeHours * kManageHourMs)},
        {QStringLiteral("to"), ended > started && field(experiment, "status") != QLatin1String("running")
                                   ? ended : double(now)},
        {QStringLiteral("filterTestAccounts"), true}};
    const int serial = m_experimentSerial;
    const QString id = field(experiment, "id");
    request(QByteArrayLiteral("POST"), projectPath(QStringLiteral("/query")), query,
            [this, serial, id, experiment, started](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_experimentSerial || id != field(selectedExperiment(), "id"))
            return;
        if (!ok)
            return emptyRow(m_variants, QStringLiteral("Results could not load: %1").arg(error));
        const QJsonObject result = payload.value(QStringLiteral("result")).toObject();
        const QJsonArray variants = result.value(QStringLiteral("variants")).toArray();
        QString detail = field(experiment, "description");
        detail += (detail.isEmpty() ? QString() : QStringLiteral("\n"))
                  + QStringLiteral("%1 visitors exposed%2. A result is significant at 95% with at least 100 visitors "
                                   "in each variant.")
                        .arg(countText(amount(result, "exposed")),
                             started > 0 ? QStringLiteral(" since ") + dateText(started) : QString());
        m_experimentDetail->setText(detail);
        resetTable(m_variants);
        for (int index = 0; index < variants.size(); ++index) {
            const QJsonObject row = variants.at(index).toObject();
            const bool control = field(row, "variant") == QLatin1String("control");
            const QJsonValue lift = row.value(QStringLiteral("lift"));
            const QJsonValue chance = row.value(QStringLiteral("probabilityToBeatControl"));
            m_variants->insertRow(index);
            setCell(m_variants, index, 0, field(row, "variant"));
            setCell(m_variants, index, 1, countText(amount(row, "exposures")), true);
            setCell(m_variants, index, 2, countText(amount(row, "conversions")), true);
            setCell(m_variants, index, 3, rateText(amount(row, "conversionRate")), true);
            setCell(m_variants, index, 4,
                    control || !lift.isDouble() ? QStringLiteral("—")
                                                : (lift.toDouble() >= 0 ? QStringLiteral("+") : QString())
                                                      + rateText(lift.toDouble()),
                    true);
            setCell(m_variants, index, 5, control || !chance.isDouble() ? QStringLiteral("—") : rateText(chance.toDouble()),
                    true);
            setCell(m_variants, index, 6,
                    control ? QStringLiteral("Baseline")
                    : row.value(QStringLiteral("significant")).toBool()
                        ? (chance.toDouble() >= 0.5 ? QStringLiteral("Winning") : QStringLiteral("Losing"))
                        : QStringLiteral("Not yet significant"));
        }
        if (variants.isEmpty())
            emptyRow(m_variants, QStringLiteral("No visitor has seen this experiment yet."));
    });
}

void AnalyticsFeature::setExperimentRunning(bool running)
{
    const QJsonObject experiment = selectedExperiment();
    if (experiment.isEmpty())
        return;
    const QJsonObject flag = flagFor(field(experiment, "flagKey"));
    const QString question = running
        ? QStringLiteral("Start “%1”? Its flag %2 is turned on, so visitors are split %3.")
              .arg(field(experiment, "name"), field(experiment, "flagKey"), variantSplit(flag))
        : QStringLiteral("Stop “%1”? Its flag %2 is turned off, so every visitor sees the control version. The "
                         "results so far are kept.").arg(field(experiment, "name"), field(experiment, "flagKey"));
    if (QMessageBox::question(this, running ? QStringLiteral("Start experiment") : QStringLiteral("Stop experiment"),
                              question) != QMessageBox::Yes)
        return;
    QJsonObject body{{QStringLiteral("status"), running ? QStringLiteral("running") : QStringLiteral("stopped")}};
    const double now = double(QDateTime::currentMSecsSinceEpoch());
    if (running && amount(experiment, "startedAt") <= 0)
        body.insert(QStringLiteral("startedAt"), now);
    if (!running)
        body.insert(QStringLiteral("endedAt"), now);
    request(QByteArrayLiteral("PATCH"), projectPath(QStringLiteral("/objects/") + field(experiment, "id")), body,
            [this, flag, running](bool ok, const QJsonObject &, const QString &message) {
        if (!ok)
            return setStatus(QStringLiteral("The experiment was not updated: %1").arg(message), true);
        if (flag.isEmpty())
            return loadExperiments();
        request(QByteArrayLiteral("PATCH"), projectPath(QStringLiteral("/objects/") + field(flag, "id")),
                QJsonObject{{QStringLiteral("active"), running}},
                [this, running](bool flagOk, const QJsonObject &, const QString &flagError) {
            if (!flagOk)
                setStatus(QStringLiteral("The experiment changed but its flag did not: %1").arg(flagError), true);
            else
                setStatus(running ? QStringLiteral("Experiment running.") : QStringLiteral("Experiment stopped."));
            loadExperiments();
        });
    });
}

void AnalyticsFeature::newExperiment()
{
    if (!canManage())
        return;
    loadObjects(QStringLiteral("goal"), [this](const QJsonArray &goals) {
        m_goalList = goals;
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("New experiment"));
        dialog.resize(580, 460);
        auto *layout = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        auto *name = new QLineEdit;
        name->setMaxLength(120);
        name->setPlaceholderText(QStringLiteral("e.g. Pricing page: annual first"));
        auto *hypothesis = new QPlainTextEdit;
        hypothesis->setPlaceholderText(QStringLiteral("What you expect to change, and why"));
        hypothesis->setMaximumHeight(90);
        auto *key = new QLineEdit;
        key->setMaxLength(80);
        key->setPlaceholderText(QStringLiteral("flag key, e.g. pricing-annual-first"));
        key->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[A-Za-z0-9_-]{1,80}")), key));
        auto *variant = new QLineEdit(QStringLiteral("test"));
        variant->setMaxLength(80);
        variant->setValidator(new QRegularExpressionValidator(
            QRegularExpression(QStringLiteral("[A-Za-z0-9_-]{1,80}")), variant));
        auto *split = new QSpinBox;
        split->setRange(1, 99);
        split->setValue(50);
        split->setSuffix(QStringLiteral("% of visitors"));
        auto *goal = new QComboBox;
        for (const QJsonValue &value : goals)
            goal->addItem(field(value.toObject(), "name"), field(value.toObject(), "id"));
        if (!goal->count())
            goal->addItem(QStringLiteral("Pageview (add goals on the Conversions tab)"), QString());
        form->addRow(QStringLiteral("Name"), name);
        form->addRow(QStringLiteral("Hypothesis"), hypothesis);
        form->addRow(QStringLiteral("Flag key"), key);
        form->addRow(QStringLiteral("Test variant"), variant);
        form->addRow(QStringLiteral("Test variant gets"), split);
        form->addRow(QStringLiteral("Goal"), goal);
        layout->addLayout(form);
        layout->addWidget(noteLabel(QStringLiteral(
            "The rest of the visitors get control. Show each variant where the page reads the flag; on ForkMesh's own "
            "pages, wrap the alternatives in an element with data-fm-experiment=\"<flag key>\".")));
        auto *error = new QLabel;
        error->setObjectName(QStringLiteral("errorLabel"));
        error->setVisible(false);
        layout->addWidget(error);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        auto *start = buttons->addButton(QStringLiteral("Start experiment"), QDialogButtonBox::AcceptRole);
        start->setObjectName(QStringLiteral("primaryButton"));
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            QString problem;
            if (name->text().trimmed().isEmpty() || key->text().isEmpty() || variant->text().isEmpty())
                problem = QStringLiteral("Give the experiment a name, a flag key and a variant name.");
            else if (variant->text() == QLatin1String("control"))
                problem = QStringLiteral("Name the test variant something other than control.");
            else if (!flagFor(key->text()).isEmpty())
                problem = QStringLiteral("A flag with this key already exists; choose another key.");
            if (!problem.isEmpty()) {
                error->setText(problem);
                error->setVisible(true);
                return;
            }
            dialog.accept();
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted)
            return;
        QJsonObject goalRecord;
        for (const QJsonValue &value : goals)
            if (field(value.toObject(), "id") == goal->currentData().toString())
                goalRecord = value.toObject();
        const QJsonObject flag{
            {QStringLiteral("kind"), QStringLiteral("flag")},
            {QStringLiteral("key"), key->text()},
            {QStringLiteral("name"), name->text().trimmed()},
            {QStringLiteral("active"), true},
            {QStringLiteral("conditions"), QJsonArray{QJsonObject{{QStringLiteral("properties"), QJsonArray()},
                                                                  {QStringLiteral("rolloutPercent"), 100},
                                                                  {QStringLiteral("variant"), QString()}}}},
            {QStringLiteral("variants"), QJsonArray{
                QJsonObject{{QStringLiteral("key"), QStringLiteral("control")},
                            {QStringLiteral("percent"), 100 - split->value()}},
                QJsonObject{{QStringLiteral("key"), variant->text()}, {QStringLiteral("percent"), split->value()}}}}};
        const QJsonObject experiment{
            {QStringLiteral("kind"), QStringLiteral("experiment")},
            {QStringLiteral("name"), name->text().trimmed()},
            {QStringLiteral("description"), hypothesis->toPlainText().trimmed()},
            {QStringLiteral("flagKey"), key->text()},
            {QStringLiteral("goal"), goalRecord.isEmpty()
                                         ? QJsonObject{{QStringLiteral("event"), QStringLiteral("$pageview")}}
                                         : goalSeries(goalRecord)},
            {QStringLiteral("status"), QStringLiteral("running")},
            {QStringLiteral("startedAt"), double(QDateTime::currentMSecsSinceEpoch())}};
        request(QByteArrayLiteral("POST"), projectPath(QStringLiteral("/objects")), flag,
                [this, experiment](bool ok, const QJsonObject &, const QString &message) {
            if (!ok)
                return setStatus(QStringLiteral("The flag was not created: %1").arg(message), true);
            request(QByteArrayLiteral("POST"), projectPath(QStringLiteral("/objects")), experiment,
                    [this](bool saved, const QJsonObject &, const QString &problem) {
                if (!saved)
                    return setStatus(QStringLiteral("The flag was created but the experiment was not: %1").arg(problem),
                                     true);
                setStatus(QStringLiteral("Experiment running."));
                loadExperiments();
            });
        });
    });
}

} // namespace forkmesh::analytics

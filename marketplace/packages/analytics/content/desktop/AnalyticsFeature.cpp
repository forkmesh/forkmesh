#include "AnalyticsFeature.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTableWidget>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace forkmesh::analytics {
namespace {

constexpr char kAnalyticsOrigin[] = "https://app.forkmesh.com";
constexpr qint64 kHourMs = 3600000;

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

QLabel *sectionLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setStyleSheet(QStringLiteral("font-weight: 600;"));
    return label;
}

QTableWidget *topTable(const QStringList &headers, const QString &name)
{
    auto *table = new QTableWidget(0, int(headers.size()));
    table->setObjectName(name);
    table->setAccessibleName(headers.first());
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->setAlternatingRowColors(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setMinimumHeight(180);
    return table;
}

QColor visitorColor(bool dark) { return QColor(dark ? "#3aa7e0" : "#1f8fd1"); }
QColor pageviewColor(bool dark) { return QColor(dark ? "#c98500" : "#d08a00"); }

} // namespace

class AnalyticsChart final : public QWidget
{
public:
    explicit AnalyticsChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("analyticsTrend"));
        setMinimumHeight(190);
        setMouseTracking(true);
    }

    void setSeries(const QJsonObject &timeseries)
    {
        m_labels = timeseries.value(QStringLiteral("labels")).toArray();
        m_hourly = text(timeseries, "interval") == QLatin1String("hour");
        m_visitors = QJsonArray();
        m_pageviews = QJsonArray();
        for (const QJsonValue &value : timeseries.value(QStringLiteral("series")).toArray()) {
            const QJsonObject series = value.toObject();
            if (text(series, "name") == QLatin1String("Visitors"))
                m_visitors = series.value(QStringLiteral("data")).toArray();
            else if (text(series, "name") == QLatin1String("Pageviews"))
                m_pageviews = series.value(QStringLiteral("data")).toArray();
        }
        setAccessibleName(QStringLiteral("Visitors and pageviews over the selected range"));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor muted = palette().color(QPalette::PlaceholderText);
        QColor grid = muted;
        grid.setAlphaF(0.35);
        const QRectF plot = plotRect();
        double peak = 4;
        for (const QJsonArray *series : {&m_visitors, &m_pageviews})
            for (const QJsonValue &value : *series)
                peak = std::max(peak, value.toDouble() * 1.1);
        peak = std::ceil(peak / 4) * 4;
        for (int step = 0; step <= 4; ++step) {
            const qreal y = plot.bottom() - plot.height() * step / 4;
            painter.setPen(QPen(grid, 1));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(muted);
            painter.drawText(QRectF(0, y - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                             AnalyticsFeature::countText(peak * step / 4));
        }
        const int count = int(m_labels.size());
        if (count == 0)
            return;
        const qreal slot = plot.width() / count;
        const qreal barWidth = std::min<qreal>(22, std::max<qreal>(2, slot - 3));
        painter.setPen(Qt::NoPen);
        painter.setBrush(visitorColor(dark));
        for (int index = 0; index < count && index < m_visitors.size(); ++index) {
            const double value = m_visitors.at(index).toDouble();
            if (value <= 0)
                continue;
            const qreal h = std::max<qreal>(2, plot.height() * value / peak);
            painter.drawRoundedRect(QRectF(plot.left() + index * slot + (slot - barWidth) / 2,
                                           plot.bottom() - h, barWidth, h), 2, 2);
        }
        if (!m_pageviews.isEmpty()) {
            QPainterPath path;
            for (int index = 0; index < count && index < m_pageviews.size(); ++index) {
                const QPointF point(plot.left() + index * slot + slot / 2,
                                    plot.bottom() - plot.height() * m_pageviews.at(index).toDouble() / peak);
                if (index == 0)
                    path.moveTo(point);
                else
                    path.lineTo(point);
            }
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(pageviewColor(dark), 2));
            painter.drawPath(path);
        }
        const int every = std::max(1, int(std::ceil(count / std::max<qreal>(1, plot.width() / 70))));
        painter.setPen(muted);
        for (int index = 0; index < count; index += every) {
            const qreal x = plot.left() + index * slot + slot / 2;
            painter.drawText(QRectF(x - 35, plot.bottom() + 4, 70, 16), Qt::AlignHCenter | Qt::AlignTop,
                             bucketLabel(index));
        }
        const QRectF legend(plot.right() - 190, 0, 190, 14);
        painter.setPen(Qt::NoPen);
        painter.setBrush(visitorColor(dark));
        painter.drawRoundedRect(QRectF(legend.left(), legend.top() + 3, 10, 10), 2, 2);
        painter.setPen(muted);
        painter.drawText(QRectF(legend.left() + 14, legend.top(), 70, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Visitors"));
        painter.setPen(QPen(pageviewColor(dark), 2));
        painter.drawLine(QPointF(legend.left() + 90, legend.top() + 8), QPointF(legend.left() + 102, legend.top() + 8));
        painter.setPen(muted);
        painter.drawText(QRectF(legend.left() + 106, legend.top(), 84, 16), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Pageviews"));
    }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
            return QWidget::event(event);
        const auto *help = static_cast<QHelpEvent *>(event);
        const QRectF plot = plotRect();
        if (m_labels.isEmpty() || help->pos().x() < plot.left() || help->pos().x() > plot.right())
            return true;
        const int index = std::clamp(int((help->pos().x() - plot.left()) / (plot.width() / m_labels.size())), 0,
                                     int(m_labels.size()) - 1);
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(qint64(m_labels.at(index).toDouble()));
        QToolTip::showText(help->globalPos(),
                           QStringLiteral("%1\n%2 visitors · %3 pageviews")
                               .arg(QLocale().toString(at, m_hourly ? QStringLiteral("MMM d, h AP")
                                                                    : QStringLiteral("ddd MMM d")),
                                    AnalyticsFeature::countText(m_visitors.at(index).toDouble()),
                                    AnalyticsFeature::countText(m_pageviews.at(index).toDouble())),
                           this);
        return true;
    }

private:
    QRectF plotRect() const
    {
        return QRectF(52, 22, std::max<qreal>(20, width() - 62), std::max<qreal>(20, height() - 46));
    }

    QString bucketLabel(int index) const
    {
        const QDateTime at = QDateTime::fromMSecsSinceEpoch(qint64(m_labels.at(index).toDouble()));
        return m_hourly ? QLocale().toString(at.time(), QStringLiteral("h AP"))
                        : at.date().toString(QStringLiteral("M/d"));
    }

    QJsonArray m_labels;
    QJsonArray m_visitors;
    QJsonArray m_pageviews;
    bool m_hourly = false;
};

QString AnalyticsFeature::countText(double value)
{
    const double rounded = std::round(value);
    if (std::abs(rounded) >= 1000000)
        return QLocale().toString(rounded / 1000000, 'f', 1) + QStringLiteral("M");
    if (std::abs(rounded) >= 10000)
        return QLocale().toString(rounded / 1000, 'f', 1) + QStringLiteral("k");
    return QLocale().toString(qint64(rounded));
}

QString AnalyticsFeature::durationText(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds / 1000);
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1h %2m").arg(seconds / 3600).arg(seconds % 3600 / 60);
}

QString AnalyticsFeature::changeText(double current, double previous)
{
    if (previous <= 0)
        return current > 0 ? QStringLiteral("new") : QString();
    const long change = std::lround((current - previous) / previous * 100);
    return (change >= 0 ? QStringLiteral("+") : QString()) + QString::number(change) + QLatin1Char('%');
}

QJsonObject AnalyticsFeature::overviewQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes)
{
    return QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("overview")},
        {QStringLiteral("from"), double(nowMs - rangeHours * kHourMs)},
        {QStringLiteral("to"), double(nowMs)},
        {QStringLiteral("tzOffsetMinutes"), tzOffsetMinutes},
        {QStringLiteral("filterTestAccounts"), true},
    };
}

QString AnalyticsFeature::snippetText(const QString &origin, const QString &writeKey)
{
    return QStringLiteral(
               "<script src=\"%1/analytics-sdk.js\" defer></script>\n"
               "<script>\n"
               "  window.addEventListener(\"DOMContentLoaded\", function () {\n"
               "    window.analytics = ForkMeshAnalyticsSDK.init({\n"
               "      key: \"%2\",\n"
               "      apiHost: \"%1\"\n"
               "    });\n"
               "  });\n"
               "</script>")
        .arg(origin, writeKey);
}

QString AnalyticsFeature::projectName(const QJsonObject &project)
{
    const QString name = text(project.value(QStringLiteral("settings")).toObject(), "name").trimmed();
    if (!name.isEmpty())
        return name;
    return project.value(QStringLiteral("siteDefault")).toBool() ? QStringLiteral("Website")
                                                                 : QStringLiteral("Project");
}

AnalyticsFeature::AnalyticsFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged), m_packageId(host.packageId)
{
    setObjectName(QStringLiteral("analyticsSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Analytics"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addStretch();
    m_organization = new QComboBox;
    m_organization->setObjectName(QStringLiteral("analyticsOrganization"));
    m_organization->setMinimumWidth(170);
    m_organization->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_organization->setAccessibleName(QStringLiteral("Organization"));
    heading->addWidget(m_organization);
    m_project = new QComboBox;
    m_project->setObjectName(QStringLiteral("analyticsProject"));
    m_project->setMinimumWidth(150);
    m_project->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_project->setAccessibleName(QStringLiteral("Project"));
    heading->addWidget(m_project);
    m_rangeGroup = new QButtonGroup(this);
    m_rangeGroup->setExclusive(true);
    static constexpr std::pair<const char *, int> ranges[] = {
        {"24h", 24}, {"7d", 24 * 7}, {"30d", 24 * 30}, {"90d", 24 * 90}};
    for (const auto &[label, hours] : ranges) {
        auto *button = new QPushButton(QString::fromLatin1(label));
        button->setCheckable(true);
        button->setChecked(hours == m_rangeHours);
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setAccessibleName(QStringLiteral("Last %1").arg(QString::fromLatin1(label)));
        m_rangeGroup->addButton(button, hours);
        heading->addWidget(button);
    }
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    auto *webButton = new QPushButton(QStringLiteral("Open in browser"));
    webButton->setToolTip(QStringLiteral(
        "Funnels, retention, session replay, heatmaps, flags, experiments and surveys open on the web dashboard"));
    for (QPushButton *button : {refreshButton, webButton}) {
        button->setObjectName(QStringLiteral("ghostButton"));
        heading->addWidget(button);
    }
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading Analytics…"));
    outer->addWidget(m_status);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    m_installNote = new QLabel;
    m_installNote->setAlignment(Qt::AlignHCenter);
    m_installNote->setWordWrap(true);
    installLayout->addWidget(m_installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install Analytics"));
    m_installButton->setObjectName(QStringLiteral("primaryButton"));
    installRow->addWidget(m_installButton);
    installRow->addStretch();
    installLayout->addLayout(installRow);
    installLayout->addStretch();
    outer->addWidget(m_installPanel, 1);

    m_emptyPanel = new QWidget;
    auto *emptyLayout = new QVBoxLayout(m_emptyPanel);
    emptyLayout->setContentsMargins(8, 36, 8, 8);
    m_emptyNote = new QLabel;
    m_emptyNote->setAlignment(Qt::AlignHCenter);
    m_emptyNote->setWordWrap(true);
    emptyLayout->addWidget(m_emptyNote);
    auto *createRow = new QHBoxLayout;
    createRow->addStretch();
    m_projectName = new QLineEdit;
    m_projectName->setPlaceholderText(QStringLiteral("Project name, e.g. Marketing site"));
    m_projectName->setAccessibleName(QStringLiteral("New project name"));
    m_projectName->setMaxLength(80);
    m_projectName->setMinimumWidth(240);
    createRow->addWidget(m_projectName);
    m_createButton = new QPushButton(QStringLiteral("Create project"));
    m_createButton->setObjectName(QStringLiteral("primaryButton"));
    createRow->addWidget(m_createButton);
    createRow->addStretch();
    emptyLayout->addLayout(createRow);
    emptyLayout->addStretch();
    outer->addWidget(m_emptyPanel, 1);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_content = scroll;
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(12);

    m_kpis = new QGridLayout;
    m_kpis->setHorizontalSpacing(24);
    m_kpis->setVerticalSpacing(2);
    const QStringList kpiNames{QStringLiteral("Visitors"), QStringLiteral("Pageviews"), QStringLiteral("Sessions"),
                               QStringLiteral("Bounce rate"), QStringLiteral("Avg. session")};
    for (int column = 0; column < kpiNames.size(); ++column) {
        m_kpis->addWidget(mutedLabel(kpiNames.at(column)), 0, column);
        auto *value = new QLabel(QStringLiteral("–"));
        value->setObjectName(QStringLiteral("analyticsKpi"));
        value->setAccessibleName(kpiNames.at(column));
        value->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 600;"));
        m_kpis->addWidget(value, 1, column);
        m_kpiValues.append(value);
        auto *change = new QLabel;
        change->setObjectName(QStringLiteral("analyticsKpiChange"));
        m_kpis->addWidget(change, 2, column);
        m_kpiChanges.append(change);
    }
    m_kpis->setColumnStretch(kpiNames.size(), 1);
    layout->addLayout(m_kpis);

    m_chart = new AnalyticsChart;
    layout->addWidget(m_chart);

    auto *tables = new QGridLayout;
    tables->setHorizontalSpacing(16);
    tables->setVerticalSpacing(6);
    m_pages = topTable({QStringLiteral("Page"), QStringLiteral("Views"), QStringLiteral("Visitors")},
                       QStringLiteral("analyticsPages"));
    m_referrers = topTable({QStringLiteral("Referrer"), QStringLiteral("Views")}, QStringLiteral("analyticsReferrers"));
    m_countries = topTable({QStringLiteral("Country"), QStringLiteral("Views")}, QStringLiteral("analyticsCountries"));
    m_devices = topTable({QStringLiteral("Device"), QStringLiteral("Views")}, QStringLiteral("analyticsDevices"));
    m_browsers = topTable({QStringLiteral("Browser"), QStringLiteral("Views")}, QStringLiteral("analyticsBrowsers"));
    m_events = topTable({QStringLiteral("Event"), QStringLiteral("Count")}, QStringLiteral("analyticsEvents"));
    const QList<std::pair<QString, QTableWidget *>> sections{
        {QStringLiteral("Top pages"), m_pages},     {QStringLiteral("Referrers"), m_referrers},
        {QStringLiteral("Countries"), m_countries}, {QStringLiteral("Devices"), m_devices},
        {QStringLiteral("Browsers"), m_browsers},   {QStringLiteral("Events"), m_events}};
    for (int index = 0; index < sections.size(); ++index) {
        const int row = index / 2 * 2;
        const int column = index % 2;
        tables->addWidget(sectionLabel(sections.at(index).first), row, column);
        tables->addWidget(sections.at(index).second, row + 1, column);
    }
    tables->setColumnStretch(0, 3);
    tables->setColumnStretch(1, 2);
    layout->addLayout(tables);

    m_truncated = mutedLabel(QStringLiteral(
        "This range holds more events than one query scans, so only the most recent ones were counted."));
    m_truncated->setVisible(false);
    layout->addWidget(m_truncated);

    layout->addWidget(sectionLabel(QStringLiteral("Add this project to a site")));
    auto *setupRow = new QHBoxLayout;
    m_writeKey = mutedLabel(QString());
    m_writeKey->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setupRow->addWidget(m_writeKey, 1);
    auto *copyButton = new QPushButton(QStringLiteral("Copy snippet"));
    copyButton->setObjectName(QStringLiteral("ghostButton"));
    copyButton->setToolTip(QStringLiteral("Copy the script tags to paste before </head> on your site"));
    setupRow->addWidget(copyButton);
    layout->addLayout(setupRow);
    layout->addStretch();
    scroll->setWidget(body);
    outer->addWidget(m_content, 1);

    connect(m_organization, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || m_loading)
            return;
        m_projects = QJsonObject();
        m_overview = QJsonObject();
        loadOrganization();
    });
    connect(m_project, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || m_loading)
            return;
        m_overview = QJsonObject();
        applyState();
        loadOverview();
    });
    connect(m_rangeGroup, &QButtonGroup::idClicked, this, [this](int hours) {
        m_rangeHours = hours;
        loadOverview();
    });
    connect(refreshButton, &QPushButton::clicked, this, [this] { refreshOrganizations(); });
    connect(webButton, &QPushButton::clicked, this, [this] { openWebDashboard(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });
    connect(m_createButton, &QPushButton::clicked, this, [this] { createProject(); });
    connect(m_projectName, &QLineEdit::returnPressed, this, [this] { createProject(); });
    connect(copyButton, &QPushButton::clicked, this, [this] { copySnippet(); });

    applyState();
}

void AnalyticsFeature::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                               ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Analytics network access is unavailable."));
        return;
    }
    const QPointer<AnalyticsFeature> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                    const QString &error) mutable {
        if (guard)
            handler(ok, payload, error);
    });
}

QString AnalyticsFeature::organization() const
{
    return m_organization->currentData().toString();
}

QString AnalyticsFeature::organizationPath(const QString &suffix) const
{
    return QStringLiteral("/api/analytics/") + organization() + suffix;
}

QJsonObject AnalyticsFeature::organizationRecord() const
{
    const QString name = organization();
    for (const QJsonValue &value : m_organizations) {
        const QJsonObject item = value.toObject();
        if (text(item, "name") == name)
            return item;
    }
    return {};
}

QJsonObject AnalyticsFeature::projectRecord() const
{
    const QString id = m_project->currentData().toString();
    for (const QJsonValue &value : m_projects.value(QStringLiteral("projects")).toArray()) {
        const QJsonObject project = value.toObject();
        if (text(project, "id") == id)
            return project;
    }
    return {};
}

void AnalyticsFeature::refresh()
{
    refreshOrganizations();
}

bool AnalyticsFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    const QString path = forkmesh::market::nativePackageLifecyclePath(
        organization(), m_packageId, QStringLiteral("export"));
    if (path.isEmpty() || !organizationRecord().value(QStringLiteral("installed")).toBool())
        return false;
    request(QByteArrayLiteral("GET"), path, {}, std::move(done));
    return true;
}

QList<forkmesh::market::NativePackageSearchHit> AnalyticsFeature::searchHits(const QString &query, int limit) const
{
    const QString needle = query.trimmed();
    if (limit <= 0 || needle.size() < 2)
        return {};
    QList<forkmesh::market::NativePackageSearchHit> hits;
    for (const QJsonValue &value : m_projects.value(QStringLiteral("projects")).toArray()) {
        const QJsonObject project = value.toObject();
        const QString name = projectName(project);
        if (hits.size() < limit && name.contains(needle, Qt::CaseInsensitive))
            hits.append({QStringLiteral("project:") + text(project, "id"), name,
                         QStringLiteral("Analytics project in %1").arg(organization())});
    }
    const QJsonArray pages = m_overview.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("breakdowns")).toObject()
                                 .value(QStringLiteral("pages")).toArray();
    for (const QJsonValue &value : pages) {
        const QJsonObject page = value.toObject();
        if (hits.size() >= limit)
            break;
        if (text(page, "value").contains(needle, Qt::CaseInsensitive))
            hits.append({QStringLiteral("page:") + text(page, "value"), text(page, "value"),
                         QStringLiteral("%1 views · %2 visitors in the last %3")
                             .arg(countText(number(page, "count")), countText(number(page, "visitors")),
                                  m_rangeGroup->button(m_rangeHours)->text())});
    }
    return hits;
}

void AnalyticsFeature::activateSearchHit(const QString &id)
{
    if (id.startsWith(QLatin1String("project:"))) {
        const int index = m_project->findData(id.mid(8));
        if (index >= 0)
            m_project->setCurrentIndex(index);
        return;
    }
    if (!id.startsWith(QLatin1String("page:")))
        return;
    const QString path = id.mid(5);
    for (int row = 0; row < m_pages->rowCount(); ++row) {
        QTableWidgetItem *item = m_pages->item(row, 0);
        if (item && item->text() == path) {
            m_pages->scrollToItem(item);
            m_pages->setFocus();
            setStatus(QStringLiteral("%1: %2 views, %3 visitors in the last %4")
                          .arg(path, m_pages->item(row, 1)->text(), m_pages->item(row, 2)->text(),
                               m_rangeGroup->button(m_rangeHours)->text()));
            return;
        }
    }
}

void AnalyticsFeature::refreshOrganizations()
{
    if (m_loading)
        return;
    m_loading = true;
    const QString previous = organization();
    setStatus(QStringLiteral("Loading your organizations…"));
    request(QByteArrayLiteral("GET"), QStringLiteral("/api/analytics"), {},
            [this, previous](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok)
            return setStatus(QStringLiteral("Analytics could not load: %1").arg(error), true);
        m_organizations = payload.value(QStringLiteral("organizations")).toArray();
        int preferred = -1;
        m_organization->blockSignals(true);
        m_organization->clear();
        for (const QJsonValue &value : std::as_const(m_organizations)) {
            const QJsonObject item = value.toObject();
            QString label = text(item, "name") + QStringLiteral(" · ") + text(item, "role");
            if (!item.value(QStringLiteral("installed")).toBool())
                label += QStringLiteral(" · not installed");
            else if (preferred < 0)
                preferred = m_organization->count();
            m_organization->addItem(label, text(item, "name"));
        }
        const int index = m_organization->findData(previous);
        m_organization->setCurrentIndex(index >= 0 ? index : std::max(0, preferred));
        m_organization->blockSignals(false);
        if (!m_organization->count()) {
            m_projects = QJsonObject();
            m_overview = QJsonObject();
            applyState();
            return setStatus(QStringLiteral("Join or create an organization first."), true);
        }
        loadOrganization();
    });
}

void AnalyticsFeature::loadOrganization()
{
    if (m_loading || organization().isEmpty())
        return;
    if (!organizationRecord().value(QStringLiteral("installed")).toBool()) {
        m_projects = QJsonObject();
        m_overview = QJsonObject();
        applyState();
        return setStatus(QStringLiteral("Analytics is not installed for %1.").arg(organization()));
    }
    m_loading = true;
    const QString previous = m_project->currentData().toString();
    setStatus(QStringLiteral("Loading projects…"));
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/projects")), {},
            [this, previous](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            m_projects = QJsonObject();
            applyState();
            return setStatus(QStringLiteral("Analytics could not load: %1").arg(error), true);
        }
        m_projects = payload;
        const QJsonArray projects = payload.value(QStringLiteral("projects")).toArray();
        int fallback = 0;
        m_project->blockSignals(true);
        m_project->clear();
        for (const QJsonValue &value : projects) {
            const QJsonObject project = value.toObject();
            if (project.value(QStringLiteral("siteDefault")).toBool())
                fallback = m_project->count();
            m_project->addItem(projectName(project), text(project, "id"));
        }
        const int index = m_project->findData(previous);
        m_project->setCurrentIndex(index >= 0 ? index : fallback);
        m_project->blockSignals(false);
        applyState();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
        if (projects.isEmpty())
            return setStatus(QStringLiteral("%1 has no analytics project yet.").arg(organization()));
        loadOverview();
    });
}

void AnalyticsFeature::loadOverview()
{
    const QString id = m_project->currentData().toString();
    if (organization().isEmpty() || id.isEmpty())
        return;
    const int serial = ++m_serial;
    setStatus(QStringLiteral("Loading the last %1…").arg(m_rangeGroup->button(m_rangeHours)->text()));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/projects/") + id + QStringLiteral("/query")),
            overviewQuery(QDateTime::currentMSecsSinceEpoch(), m_rangeHours,
                          QDateTime::currentDateTime().offsetFromUtc() / 60),
            [this, serial](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_serial)
            return;
        if (!ok)
            return setStatus(QStringLiteral("The overview could not load: %1").arg(error), true);
        m_overview = payload;
        renderOverview();
        applyState();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
}

void AnalyticsFeature::installPlugin()
{
    const QString path = forkmesh::market::nativePackageLifecyclePath(
        organization(), m_packageId, QStringLiteral("install"));
    if (path.isEmpty())
        return setStatus(QStringLiteral("Select a valid organization before installing Analytics."), true);
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), path, {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Analytics was not installed: %1").arg(error), true);
        setStatus(QStringLiteral("Analytics installed for this organization."));
        refreshOrganizations();
    });
}

void AnalyticsFeature::createProject()
{
    if (organization().isEmpty() || !m_createButton->isVisible())
        return;
    const QString name = m_projectName->text().trimmed();
    m_createButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/projects")),
            QJsonObject{{QStringLiteral("name"), name.isEmpty() ? QStringLiteral("Website") : name},
                        {QStringLiteral("timezoneOffsetMinutes"),
                         QDateTime::currentDateTime().offsetFromUtc() / 60}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_createButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("The project was not created: %1").arg(error), true);
        m_projectName->clear();
        m_project->blockSignals(true);
        m_project->addItem(projectName(payload.value(QStringLiteral("project")).toObject()),
                           text(payload.value(QStringLiteral("project")).toObject(), "id"));
        m_project->setCurrentIndex(m_project->count() - 1);
        m_project->blockSignals(false);
        loadOrganization();
    });
}

void AnalyticsFeature::copySnippet()
{
    const QString key = text(projectRecord(), "writeKey");
    if (key.isEmpty())
        return;
    QApplication::clipboard()->setText(snippetText(QString::fromLatin1(kAnalyticsOrigin), key));
    setStatus(QStringLiteral("Snippet copied. Paste it before </head> on every page you want counted."));
}

void AnalyticsFeature::openWebDashboard()
{
    QUrl url(QString::fromLatin1(kAnalyticsOrigin) + QStringLiteral("/dashboard/analytics"));
    if (!organization().isEmpty())
        url.setQuery(QStringLiteral("org=") + QString::fromUtf8(QUrl::toPercentEncoding(organization())));
    QDesktopServices::openUrl(url);
}

void AnalyticsFeature::applyState()
{
    const QJsonObject record = organizationRecord();
    const QString role = text(record, "role");
    const bool listed = !record.isEmpty();
    const bool installed = record.value(QStringLiteral("installed")).toBool();
    const bool canManage = role == QLatin1String("owner") || role == QLatin1String("admin");
    const bool projectsLoaded = m_projects.contains(QStringLiteral("projects"));
    const bool hasProjects = !m_projects.value(QStringLiteral("projects")).toArray().isEmpty();
    m_installPanel->setVisible(listed && !installed);
    m_installButton->setVisible(canManage);
    m_installNote->setText(canManage
        ? QStringLiteral("Analytics counts visitors, pageviews and events on your organization's sites, on this "
                         "ForkMesh instance only: no third-party service sees a visitor and no IP address is "
                         "stored. Install it for %1 to start.").arg(organization())
        : QStringLiteral("Ask an owner or admin of %1 to install Analytics.").arg(organization()));
    const bool manage = m_projects.value(QStringLiteral("canManage")).toBool(canManage);
    m_emptyPanel->setVisible(installed && projectsLoaded && !hasProjects);
    m_projectName->setVisible(manage);
    m_createButton->setVisible(manage);
    m_emptyNote->setText(manage
        ? QStringLiteral("Create a project for each site or app you want to measure. You will get a snippet to "
                         "paste into its pages.")
        : QStringLiteral("No analytics project yet. Ask an owner or admin of %1 to create one.").arg(organization()));
    m_project->setVisible(installed && m_project->count() > 1);
    for (QAbstractButton *button : m_rangeGroup->buttons())
        button->setVisible(installed && hasProjects);
    m_content->setVisible(installed && hasProjects && !m_overview.isEmpty());
    const QString key = text(projectRecord(), "writeKey");
    m_writeKey->setText(key.isEmpty() ? QString()
                                      : QStringLiteral("Write key %1 · send events to %2/api/analytics/_/capture")
                                            .arg(key, QString::fromLatin1(kAnalyticsOrigin)));
}

void AnalyticsFeature::fillTable(QTableWidget *table, const QJsonArray &rows, bool visitors)
{
    table->setRowCount(0);
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        const int index = table->rowCount();
        table->insertRow(index);
        auto *label = new QTableWidgetItem(text(row, "value"));
        label->setToolTip(text(row, "value"));
        table->setItem(index, 0, label);
        auto *count = new QTableWidgetItem(countText(number(row, "count")));
        count->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table->setItem(index, 1, count);
        if (visitors) {
            auto *people = new QTableWidgetItem(countText(number(row, "visitors")));
            people->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            table->setItem(index, 2, people);
        }
    }
    if (rows.isEmpty()) {
        table->insertRow(0);
        auto *empty = new QTableWidgetItem(QStringLiteral("No data in this range"));
        empty->setForeground(palette().color(QPalette::PlaceholderText));
        table->setItem(0, 0, empty);
    }
}

void AnalyticsFeature::renderOverview()
{
    const QJsonObject result = m_overview.value(QStringLiteral("result")).toObject();
    const QJsonObject totals = result.value(QStringLiteral("totals")).toObject();
    const QJsonObject previous = result.value(QStringLiteral("previous")).toObject();
    const QList<const char *> counted{"visitors", "pageviews", "sessions"};
    for (int index = 0; index < counted.size(); ++index) {
        const double current = number(totals, counted.at(index));
        const double before = number(previous, counted.at(index));
        m_kpiValues.at(index)->setText(countText(current));
        const QString change = changeText(current, before);
        m_kpiChanges.at(index)->setText(change.isEmpty() ? QString() : change + QStringLiteral(" vs previous"));
        m_kpiChanges.at(index)->setStyleSheet(
            change.startsWith(QLatin1Char('-'))                       ? QStringLiteral("color: #d1453b;")
            : change.isEmpty() || change == QLatin1String("+0%") ? QString()
                                                                  : QStringLiteral("color: #1baf7a;"));
    }
    m_kpiValues.at(3)->setText(QStringLiteral("%1%").arg(std::lround(number(totals, "bounceRate") * 100)));
    m_kpiChanges.at(3)->clear();
    m_kpiValues.at(4)->setText(durationText(qint64(number(totals, "averageSessionMs"))));
    m_kpiChanges.at(4)->clear();
    m_chart->setSeries(result.value(QStringLiteral("timeseries")).toObject());
    const QJsonObject breakdowns = result.value(QStringLiteral("breakdowns")).toObject();
    fillTable(m_pages, breakdowns.value(QStringLiteral("pages")).toArray(), true);
    fillTable(m_referrers, breakdowns.value(QStringLiteral("referrers")).toArray(), false);
    fillTable(m_countries, breakdowns.value(QStringLiteral("countries")).toArray(), false);
    fillTable(m_devices, breakdowns.value(QStringLiteral("devices")).toArray(), false);
    fillTable(m_browsers, breakdowns.value(QStringLiteral("browsers")).toArray(), false);
    fillTable(m_events, breakdowns.value(QStringLiteral("events")).toArray(), false);
    m_truncated->setVisible(m_overview.value(QStringLiteral("truncated")).toBool());
    setStatus(QStringLiteral("%1 events in the last %2 · updated %3")
                  .arg(countText(number(totals, "events")), m_rangeGroup->button(m_rangeHours)->text(),
                       QLocale().toString(QTime::currentTime(), QStringLiteral("h:mm AP"))));
}

void AnalyticsFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::analytics

namespace {
const forkmesh::market::NativePackageRegistration analyticsRegistration({
    QStringLiteral("builtin:analytics"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Analytics"),
    QStringLiteral("pulse"),
    QStringLiteral("/api/analytics"),
    QStringLiteral("forkmesh-analytics-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::analytics::AnalyticsFeature(host, parent);
    },
});
} // namespace

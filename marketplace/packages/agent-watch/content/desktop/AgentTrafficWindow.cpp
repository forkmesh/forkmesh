#include "AgentTrafficWindow.h"

#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QShowEvent>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

using forkmesh::agentvis::Body;
using forkmesh::agentvis::Connection;
using forkmesh::agentvis::Host;
using forkmesh::agentvis::Scene;
using forkmesh::agentvis::Traffic;

namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kSortRole = Qt::UserRole + 1;

class SortedItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant mine = data(column, kSortRole);
        const QVariant theirs = other.data(column, kSortRole);
        if (mine.isValid() && theirs.isValid())
            return mine.toDouble() < theirs.toDouble();
        return text(column).localeAwareCompare(other.text(column)) < 0;
    }
};

QString connectionKey(int sessionId, qint64 pid, const QString &address, quint16 port)
{
    return QString::number(sessionId) + QLatin1Char('|') + QString::number(pid)
           + QLatin1Char('|') + address + QLatin1Char('|') + QString::number(port);
}

QString portText(quint16 port)
{
    const QString service = forkmesh::agentvis::portService(port);
    return service.isEmpty() ? QString::number(port)
                             : QStringLiteral("%1 %2").arg(port).arg(service);
}

QString processText(qint64 pid, const QString &process)
{
    if (pid <= 0)
        return process.isEmpty() ? QStringLiteral("?") : process;
    return QStringLiteral("%1 (%2)").arg(process.isEmpty() ? QStringLiteral("?") : process)
        .arg(pid);
}

QString agoText(double seconds)
{
    if (seconds < 1.5)
        return QStringLiteral("now");
    if (seconds < 90)
        return QStringLiteral("%1 s ago").arg(qRound(seconds));
    return QStringLiteral("%1 min ago").arg(qRound(seconds / 60));
}

void setCell(QTreeWidgetItem *item, int column, const QString &text, double sort)
{
    item->setText(column, text);
    item->setData(column, kSortRole, sort);
}

QTreeWidget *makeList(QWidget *parent, const QString &name, const QStringList &headers)
{
    auto *list = new QTreeWidget(parent);
    list->setObjectName(name);
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setAlternatingRowColors(true);
    list->setHeaderLabels(headers);
    list->header()->setStretchLastSection(true);
    list->header()->setSectionResizeMode(QHeaderView::Interactive);
    return list;
}

} // namespace

AgentTrafficWindow::AgentTrafficWindow(Scene *scene, int sessionId, const QString &host,
                                       QWidget *parent)
    : QWidget(parent, Qt::Window), m_scene(scene), m_sessionId(sessionId), m_host(host)
{
    setObjectName(QStringLiteral("agentTrafficWindow"));
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(980, 620);

    m_heading = new QLabel(this);
    m_heading->setObjectName(QStringLiteral("agentTrafficHeading"));
    QFont bold = m_heading->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() * 1.25);
    m_heading->setFont(bold);
    m_heading->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("agentTrafficSummary"));
    m_details = new QLabel(this);
    m_details->setObjectName(QStringLiteral("agentTrafficDetails"));
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setObjectName(QStringLiteral("agentTrafficFilter"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setPlaceholderText(
        QStringLiteral("Filter by process, agent, address, port or direction"));
    connect(m_filterEdit, &QLineEdit::textChanged, this,
            [this](const QString &text) { setFilter(text); });

    m_pauseButton = new QPushButton(QStringLiteral("Pause"), this);
    m_pauseButton->setObjectName(QStringLiteral("agentTrafficPause"));
    m_pauseButton->setCheckable(true);
    m_pauseButton->setToolTip(QStringLiteral(
        "Hold both lists still. Traffic keeps being recorded and fills in when you resume."));
    connect(m_pauseButton, &QPushButton::toggled, this, [this](bool on) { setPaused(on); });

    auto *clear = new QPushButton(QStringLiteral("Clear"), this);
    clear->setObjectName(QStringLiteral("agentTrafficClear"));
    clear->setToolTip(QStringLiteral("Empty the live list; new traffic keeps arriving."));
    connect(clear, &QPushButton::clicked, this, [this] { clearTraffic(); });

    auto *bar = new QHBoxLayout;
    bar->addWidget(m_filterEdit, 1);
    bar->addWidget(m_pauseButton);
    bar->addWidget(clear);

    m_connections = makeList(this, QStringLiteral("agentTrafficConnections"),
                             {QStringLiteral("Process"), QStringLiteral("Agent"),
                              QStringLiteral("Remote"), QStringLiteral("Port"),
                              QStringLiteral("↓ Received"), QStringLiteral("↑ Sent"),
                              QStringLiteral("↓ Now"), QStringLiteral("↑ Now"),
                              QStringLiteral("Last seen")});
    m_connections->setSortingEnabled(true);
    m_connections->sortByColumn(ConnectionLast, Qt::AscendingOrder);
    m_connections->setToolTip(QStringLiteral(
        "Every connection seen, by process, address and port. Select one to show only its "
        "traffic below."));
    connect(m_connections, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QList<QTreeWidgetItem *> picked = m_connections->selectedItems();
        m_selected = picked.isEmpty() ? QString() : picked.first()->data(0, kKeyRole).toString();
        applyFilter();
    });

    m_traffic = makeList(this, QStringLiteral("agentTrafficLive"),
                         {QStringLiteral("Time"), QStringLiteral("Direction"),
                          QStringLiteral("Process"), QStringLiteral("Agent"),
                          QStringLiteral("Remote"), QStringLiteral("Port"),
                          QStringLiteral("Bytes")});
    m_traffic->setSelectionMode(QAbstractItemView::ExtendedSelection);

    auto *connectionsBox = new QWidget(this);
    auto *connectionsLayout = new QVBoxLayout(connectionsBox);
    connectionsLayout->setContentsMargins(0, 0, 0, 0);
    connectionsLayout->addWidget(new QLabel(QStringLiteral("Connections"), connectionsBox));
    connectionsLayout->addWidget(m_connections);
    auto *trafficBox = new QWidget(this);
    auto *trafficLayout = new QVBoxLayout(trafficBox);
    trafficLayout->setContentsMargins(0, 0, 0, 0);
    trafficLayout->addWidget(new QLabel(QStringLiteral("Live traffic, newest first"), trafficBox));
    trafficLayout->addWidget(m_traffic);

    auto *split = new QSplitter(Qt::Vertical, this);
    split->addWidget(connectionsBox);
    split->addWidget(trafficBox);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->addWidget(m_heading);
    layout->addWidget(m_summary);
    layout->addWidget(m_details);
    layout->addLayout(bar);
    layout->addWidget(split, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, [this] { refresh(); });

    refresh();
    for (QTreeWidget *list : {m_connections, m_traffic}) {
        for (int column = 0; column < list->columnCount(); ++column)
            list->resizeColumnToContents(column);
    }
}

QString AgentTrafficWindow::keyFor(int sessionId, const QString &host)
{
    return QString::number(sessionId) + QLatin1Char('|') + host;
}

QString AgentTrafficWindow::sessionName(int sessionId) const
{
    if (sessionId == Scene::kDesktop)
        return QStringLiteral("this desktop");
    const Body *body = m_scene ? m_scene->body(sessionId) : nullptr;
    if (!body || body->title.isEmpty())
        return QStringLiteral("#%1").arg(sessionId);
    return QStringLiteral("#%1 %2").arg(sessionId).arg(body->title);
}

QString AgentTrafficWindow::remoteText(const QString &host, const QString &address) const
{
    if (address.isEmpty() || address == host)
        return host;
    return QStringLiteral("%1 (%2)").arg(host, address);
}

QString AgentTrafficWindow::heading() const
{
    return m_heading->text();
}

QString AgentTrafficWindow::summary() const
{
    return m_summary->text();
}

void AgentTrafficWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
    m_timer->start();
}

void AgentTrafficWindow::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_timer->stop();
}

void AgentTrafficWindow::refresh()
{
    if (!m_scene)
        return;
    m_host = m_scene->hostNamed(m_host);
    const QString title = m_sessionId == Scene::kAnySession
                              ? m_host
                              : QStringLiteral("%1 → %2").arg(sessionName(m_sessionId), m_host);
    m_heading->setText(title);
    setWindowTitle(QStringLiteral("Traffic · %1").arg(title));

    QStringList details;
    if (const Host *host = m_scene->host(m_host)) {
        if (host->mirror)
            details << QStringLiteral("one of your mirror nodes");
        QStringList seen;
        for (const auto &endpoint : host->endpoints)
            seen << forkmesh::agentvis::describeEndpoint(endpoint.address, endpoint.port);
        if (!seen.isEmpty())
            details << seen.join(QStringLiteral("  ·  "));
    }
    const QStringList network = m_scene->addressNetwork(m_host);
    if (!network.isEmpty())
        details << network.join(QStringLiteral("  ·  "));
    m_details->setText(details.join(QLatin1Char('\n')));
    m_details->setVisible(!details.isEmpty());

    if (m_paused)
        return;
    refreshConnections();
    appendTraffic();
}

void AgentTrafficWindow::refreshConnections()
{
    const double now = m_scene->now();
    const QVector<Connection> connections = m_scene->connections(m_sessionId, m_host);
    qint64 up = 0;
    qint64 down = 0;
    double upRate = 0;
    double downRate = 0;
    QSet<QString> alive;
    const bool sorting = m_connections->isSortingEnabled();
    m_connections->setSortingEnabled(false);
    for (const Connection &connection : connections) {
        up += connection.up;
        down += connection.down;
        upRate += connection.upRate;
        downRate += connection.downRate;
        const QString key = connectionKey(connection.sessionId, connection.pid,
                                          connection.address, connection.port);
        alive.insert(key);
        QTreeWidgetItem *&item = m_connectionItems[key];
        if (!item) {
            item = new SortedItem(m_connections);
            item->setData(0, kKeyRole, key);
            item->setTextAlignment(ConnectionReceived, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(ConnectionSent, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(ConnectionDownRate, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(ConnectionUpRate, Qt::AlignRight | Qt::AlignVCenter);
        }
        item->setText(ConnectionProcess, processText(connection.pid, connection.process));
        item->setText(ConnectionAgent, sessionName(connection.sessionId));
        item->setText(ConnectionRemote, remoteText(connection.host, connection.address));
        setCell(item, ConnectionPort, portText(connection.port), connection.port);
        setCell(item, ConnectionReceived, forkmesh::agentvis::formatBytes(connection.down),
                double(connection.down));
        setCell(item, ConnectionSent, forkmesh::agentvis::formatBytes(connection.up),
                double(connection.up));
        setCell(item, ConnectionDownRate,
                connection.downRate > 0 ? forkmesh::agentvis::formatRate(connection.downRate)
                                        : QString(),
                connection.downRate);
        setCell(item, ConnectionUpRate,
                connection.upRate > 0 ? forkmesh::agentvis::formatRate(connection.upRate)
                                      : QString(),
                connection.upRate);
        setCell(item, ConnectionLast, agoText(now - connection.lastSeconds),
                now - connection.lastSeconds);
        item->setHidden(!matches(item, ConnectionColumns));
    }
    for (auto it = m_connectionItems.begin(); it != m_connectionItems.end();) {
        if (alive.contains(it.key())) {
            ++it;
            continue;
        }
        delete it.value();
        it = m_connectionItems.erase(it);
    }
    m_connections->setSortingEnabled(sorting);

    m_summary->setText(
        QStringLiteral("%1 connection%2  ·  now ↓ %3  ↑ %4  ·  seen ↓ %5  ↑ %6")
            .arg(connections.size())
            .arg(connections.size() == 1 ? QString() : QStringLiteral("s"))
            .arg(forkmesh::agentvis::formatRate(downRate),
                 forkmesh::agentvis::formatRate(upRate),
                 forkmesh::agentvis::formatBytes(down), forkmesh::agentvis::formatBytes(up)));
}

void AgentTrafficWindow::appendTraffic()
{
    const QVector<Traffic> records = m_scene->traffic(m_sessionId, m_host, m_after);
    m_after = m_scene->lastTraffic();
    if (records.isEmpty())
        return;
    const QDateTime wall = QDateTime::currentDateTime();
    const double now = m_scene->now();
    const QColor out = QColor::fromHslF(0.08f, 0.85f, 0.5f);
    const QColor in = QColor::fromHslF(0.52f, 0.8f, 0.42f);
    QList<QTreeWidgetItem *> rows;
    for (const Traffic &record : records) {
        const QString key =
            connectionKey(record.sessionId, record.pid, record.address, record.port);
        const QString when =
            wall.addMSecs(-qint64((now - record.seconds) * 1000.0))
                .toString(QStringLiteral("HH:mm:ss.zzz"));
        for (const bool outgoing : {true, false}) {
            const qint64 bytes = outgoing ? record.up : record.down;
            if (bytes <= 0)
                continue;
            auto *row = new QTreeWidgetItem;
            row->setData(0, kKeyRole, key);
            row->setText(TrafficTime, when);
            row->setText(TrafficDirection,
                         outgoing ? QStringLiteral("↑ out") : QStringLiteral("↓ in"));
            row->setForeground(TrafficDirection, outgoing ? out : in);
            row->setText(TrafficProcess, processText(record.pid, record.process));
            row->setText(TrafficAgent, sessionName(record.sessionId));
            row->setText(TrafficRemote, remoteText(record.host, record.address));
            row->setText(TrafficPort, portText(record.port));
            row->setText(TrafficBytes, forkmesh::agentvis::formatBytes(bytes));
            row->setTextAlignment(TrafficBytes, Qt::AlignRight | Qt::AlignVCenter);
            row->setHidden(!matches(row, TrafficColumns));
            rows.prepend(row);
        }
    }
    if (rows.size() > kMaxRows) {
        qDeleteAll(rows.begin() + kMaxRows, rows.end());
        rows.erase(rows.begin() + kMaxRows, rows.end());
    }
    m_traffic->insertTopLevelItems(0, rows);
    while (m_traffic->topLevelItemCount() > kMaxRows)
        delete m_traffic->takeTopLevelItem(m_traffic->topLevelItemCount() - 1);
}

bool AgentTrafficWindow::matches(const QTreeWidgetItem *item, int columns) const
{
    if (columns == TrafficColumns && !m_selected.isEmpty()
        && item->data(0, kKeyRole).toString() != m_selected)
        return false;
    if (m_filter.isEmpty())
        return true;
    for (int column = 0; column < columns; ++column) {
        if (item->text(column).contains(m_filter, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

void AgentTrafficWindow::applyFilter()
{
    for (int i = 0; i < m_connections->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_connections->topLevelItem(i);
        item->setHidden(!matches(item, ConnectionColumns));
    }
    for (int i = 0; i < m_traffic->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_traffic->topLevelItem(i);
        item->setHidden(!matches(item, TrafficColumns));
    }
}

void AgentTrafficWindow::setFilter(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed == m_filter)
        return;
    m_filter = trimmed;
    if (m_filterEdit->text().trimmed() != trimmed)
        m_filterEdit->setText(trimmed);
    applyFilter();
}

void AgentTrafficWindow::setPaused(bool paused)
{
    if (paused == m_paused)
        return;
    m_paused = paused;
    if (m_pauseButton->isChecked() != paused)
        m_pauseButton->setChecked(paused);
    m_pauseButton->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    if (!paused)
        refresh();
}

void AgentTrafficWindow::clearTraffic()
{
    m_traffic->clear();
    if (m_scene)
        m_after = m_scene->lastTraffic();
}

#include "NexusFeature.h"

#include "AppPaths.h"
#include "BackoffNetworkAccessManager.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::nexus {
namespace {

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *heading(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QPushButton *button(const QString &text, bool primary)
{
    auto *result = new QPushButton(text);
    result->setObjectName(primary ? QStringLiteral("primaryButton") : QStringLiteral("ghostButton"));
    result->setCursor(Qt::PointingHandCursor);
    return result;
}

QTreeWidget *table(const QString &name, const QString &accessibleName, const QStringList &headers)
{
    auto *tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setAccessibleName(accessibleName);
    tree->setColumnCount(int(headers.size()));
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(true);
    for (int column = 0; column + 1 < headers.size(); ++column)
        tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    return tree;
}

QVBoxLayout *pageLayout(QWidget *page)
{
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(8);
    return layout;
}

QString shortTime(const QDateTime &time)
{
    if (!time.isValid())
        return {};
    return QLocale().toString(time.toLocalTime(), QLocale::ShortFormat);
}

QString portsText(const QList<quint16> &ports)
{
    QStringList parts;
    for (quint16 port : ports)
        parts << QString::number(port);
    return parts.join(QStringLiteral(", "));
}

QString serviceTitle(const QString &name)
{
    for (const KnownService &service : serviceCatalog()) {
        if (service.name == name)
            return service.title;
    }
    return name;
}

QString sectionTitle(int section)
{
    static const QStringList titles{QStringLiteral("Overview"), QStringLiteral("Discover"), QStringLiteral("Scan"),
                                    QStringLiteral("Inspect"), QStringLiteral("History"), QStringLiteral("Settings")};
    return titles.value(section);
}

}

NexusFeature::NexusFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent)
    , m_network(host.network)
    , m_searchIndexChanged(host.searchIndexChanged)
    , m_scanner(std::make_unique<TcpScanner>(this))
    , m_browser(std::make_unique<MdnsBrowser>(this))
{
    setObjectName(QStringLiteral("nexusPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("NEXUS"));
    title->setObjectName(QStringLiteral("pageTitle"));
    outer->addWidget(title);
    outer->addWidget(mutedLabel(QStringLiteral("Discover devices, scan ports and inspect DNS and web servers on networks you own or are authorized to test. "
                                               "NEXUS uses ordinary TCP connections, DNS and multicast DNS, and its history stays on this computer.")));

    auto *sectionRow = new QHBoxLayout;
    sectionRow->setSpacing(6);
    m_sections = new QButtonGroup(this);
    m_sections->setExclusive(true);
    for (int section = Overview; section <= Settings; ++section) {
        auto *sectionButton = new QPushButton(sectionTitle(section));
        sectionButton->setObjectName(QStringLiteral("logRangeButton"));
        sectionButton->setCheckable(true);
        sectionButton->setChecked(section == Overview);
        sectionButton->setCursor(Qt::PointingHandCursor);
        sectionButton->setAccessibleName(QStringLiteral("Show %1").arg(sectionTitle(section)));
        m_sections->addButton(sectionButton, section);
        sectionRow->addWidget(sectionButton);
    }
    sectionRow->addStretch();
    outer->addLayout(sectionRow);

    m_stack = new QStackedWidget;
    m_stack->addWidget(buildOverview());
    m_stack->addWidget(buildDiscover());
    m_stack->addWidget(buildScan());
    m_stack->addWidget(buildInspect());
    m_stack->addWidget(buildHistory());
    m_stack->addWidget(buildSettings());
    outer->addWidget(m_stack, 1);

    connect(m_sections, &QButtonGroup::idClicked, this, [this](int id) { showSection(Section(id)); });
    refresh();
}

NexusFeature::~NexusFeature() = default;

QWidget *NexusFeature::buildOverview()
{
    auto *page = new QWidget;
    auto *layout = pageLayout(page);
    m_overviewSummary = new QLabel;
    m_overviewSummary->setObjectName(QStringLiteral("nexusSummary"));
    m_overviewSummary->setWordWrap(true);
    layout->addWidget(m_overviewSummary);
    auto *actions = new QHBoxLayout;
    m_overviewDiscover = button(QStringLiteral("Discover devices"), true);
    m_overviewScan = button(QStringLiteral("Scan this network"), false);
    actions->addWidget(m_overviewDiscover);
    actions->addWidget(m_overviewScan);
    actions->addStretch();
    layout->addLayout(actions);
    layout->addWidget(heading(QStringLiteral("Notable observations")));
    m_observations = table(QStringLiteral("nexusObservations"), QStringLiteral("Notable observations"), {QStringLiteral("Observation")});
    m_observations->setHeaderHidden(true);
    layout->addWidget(m_observations, 1);
    layout->addWidget(heading(QStringLiteral("Recent devices")));
    m_recentDevices = table(QStringLiteral("nexusRecentDevices"), QStringLiteral("Recently discovered devices"),
                            {QStringLiteral("Name"), QStringLiteral("Addresses"), QStringLiteral("Services")});
    layout->addWidget(m_recentDevices, 1);

    connect(m_overviewDiscover, &QPushButton::clicked, this, [this] {
        showSection(Discover);
        startDiscovery();
    });
    connect(m_overviewScan, &QPushButton::clicked, this, [this] {
        const QString network = localNetwork();
        showSection(Scan);
        if (!network.isEmpty())
            m_target->setText(network);
        m_target->setFocus();
    });
    connect(m_recentDevices, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item)
            scanDevice(item->data(0, Qt::UserRole).toString());
    });
    return page;
}

QWidget *NexusFeature::buildDiscover()
{
    auto *page = new QWidget;
    auto *layout = pageLayout(page);
    auto *row = new QHBoxLayout;
    m_discoverButton = button(QStringLiteral("Discover"), true);
    m_discoverButton->setToolTip(QStringLiteral("Ask devices on this network which services they offer, using multicast DNS (Bonjour)"));
    row->addWidget(m_discoverButton);
    m_scanDevice = button(QStringLiteral("Scan selected device"), false);
    m_scanDevice->setEnabled(false);
    row->addWidget(m_scanDevice);
    row->addStretch();
    layout->addLayout(row);
    m_discoverStatus = mutedLabel(QStringLiteral("Discovery asks devices on this network to announce their services. It finds printers, speakers, TVs, "
                                                 "file shares, computers and smart-home devices that use Bonjour or multicast DNS."));
    layout->addWidget(m_discoverStatus);
    m_devices = table(QStringLiteral("nexusDevices"), QStringLiteral("Discovered devices"),
                      {QStringLiteral("Name"), QStringLiteral("Addresses"), QStringLiteral("Host name"), QStringLiteral("Services"),
                       QStringLiteral("Ports"), QStringLiteral("Last seen")});
    m_devices->setSortingEnabled(true);
    m_devices->sortByColumn(0, Qt::AscendingOrder);
    layout->addWidget(m_devices, 1);

    connect(m_discoverButton, &QPushButton::clicked, this, [this] {
        if (m_browser->running())
            m_browser->cancel();
        else
            startDiscovery();
    });
    connect(m_devices, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        m_scanDevice->setEnabled(item && !item->data(0, Qt::UserRole).toString().isEmpty());
    });
    connect(m_devices, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item)
            scanDevice(item->data(0, Qt::UserRole).toString());
    });
    connect(m_scanDevice, &QPushButton::clicked, this, [this] {
        if (QTreeWidgetItem *item = m_devices->currentItem())
            scanDevice(item->data(0, Qt::UserRole).toString());
    });
    return page;
}

QWidget *NexusFeature::buildScan()
{
    auto *page = new QWidget;
    auto *layout = pageLayout(page);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    m_target = new QLineEdit;
    m_target->setObjectName(QStringLiteral("nexusTarget"));
    m_target->setPlaceholderText(QStringLiteral("Host, IP address, range (192.168.1.10-40) or CIDR (192.168.1.0/24)"));
    m_target->setAccessibleName(QStringLiteral("Scan target"));
    auto *targetRow = new QHBoxLayout;
    targetRow->addWidget(m_target, 1);
    m_detectNetwork = button(QStringLiteral("This network"), false);
    m_detectNetwork->setObjectName(QStringLiteral("nexusDetectNetwork"));
    m_detectNetwork->setToolTip(QStringLiteral("Fill in the range of the network this computer is on, worked out from its IP address"));
    targetRow->addWidget(m_detectNetwork);
    form->addRow(QStringLiteral("Target"), targetRow);
    auto *portRow = new QHBoxLayout;
    m_ports = new QLineEdit(describePorts(presetPorts(PortPreset::Quick)));
    m_ports->setObjectName(QStringLiteral("nexusPorts"));
    m_ports->setAccessibleName(QStringLiteral("Ports to scan"));
    m_ports->setPlaceholderText(QStringLiteral("22,80,443,8000-8100"));
    portRow->addWidget(m_ports, 1);
    m_presets = new QComboBox;
    m_presets->setObjectName(QStringLiteral("nexusPresets"));
    m_presets->setAccessibleName(QStringLiteral("Port presets"));
    m_presets->addItem(QStringLiteral("Presets…"), -1);
    for (PortPreset preset : portPresets())
        m_presets->addItem(presetTitle(preset), int(preset));
    portRow->addWidget(m_presets);
    form->addRow(QStringLiteral("Ports"), portRow);
    layout->addLayout(form);

    auto *actions = new QHBoxLayout;
    m_scanButton = button(QStringLiteral("Start scan"), true);
    actions->addWidget(m_scanButton);
    m_showClosed = new QCheckBox(QStringLiteral("Show closed and silent ports"));
    actions->addWidget(m_showClosed);
    actions->addStretch();
    layout->addLayout(actions);
    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setMaximumHeight(6);
    m_progress->hide();
    layout->addWidget(m_progress);
    m_scanStatus = mutedLabel(QStringLiteral("Scans make a normal TCP connection to each port. Open means something answered; Closed means the device refused; "
                                             "No response means nothing answered in time, which can be a firewall or a device that is off."));
    layout->addWidget(m_scanStatus);
    m_scanFindings = mutedLabel({});
    m_scanFindings->setObjectName(QStringLiteral("nexusScanFindings"));
    m_scanFindings->hide();
    layout->addWidget(m_scanFindings);
    m_results = table(QStringLiteral("nexusResults"), QStringLiteral("Scan results"),
                      {QStringLiteral("Address"), QStringLiteral("Port"), QStringLiteral("State"), QStringLiteral("Service"),
                       QStringLiteral("Latency"), QStringLiteral("Details")});
    m_results->setSortingEnabled(true);
    m_results->sortByColumn(1, Qt::AscendingOrder);
    layout->addWidget(m_results, 1);

    connect(m_detectNetwork, &QPushButton::clicked, this, [this] {
        const QString network = localNetwork();
        if (network.isEmpty()) {
            m_scanStatus->setText(QStringLiteral("No local network found. Connect to Wi-Fi or Ethernet, or type a range."));
            return;
        }
        m_target->setText(network);
        m_target->setFocus();
    });
    connect(m_presets, &QComboBox::activated, this, [this](int index) {
        const int preset = m_presets->itemData(index).toInt();
        if (preset >= 0)
            m_ports->setText(describePorts(presetPorts(PortPreset(preset))));
        m_presets->setCurrentIndex(0);
    });
    connect(m_scanButton, &QPushButton::clicked, this, [this] {
        if (m_scanner->running())
            m_scanner->cancel();
        else
            startScan();
    });
    connect(m_target, &QLineEdit::returnPressed, this, [this] {
        if (!m_scanner->running())
            startScan();
    });
    connect(m_showClosed, &QCheckBox::toggled, this, [this] { renderScanResults(); });
    return page;
}

QWidget *NexusFeature::buildInspect()
{
    auto *page = new QWidget;
    auto *layout = pageLayout(page);
    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);

    auto *dns = new QWidget;
    auto *dnsLayout = pageLayout(dns);
    dnsLayout->addWidget(heading(QStringLiteral("DNS lookup")));
    auto *dnsRow = new QHBoxLayout;
    m_dnsName = new QLineEdit;
    m_dnsName->setObjectName(QStringLiteral("nexusDnsName"));
    m_dnsName->setAccessibleName(QStringLiteral("Name to look up"));
    m_dnsName->setPlaceholderText(QStringLiteral("example.com, or an IP address for PTR"));
    dnsRow->addWidget(m_dnsName, 1);
    m_dnsType = new QComboBox;
    m_dnsType->setAccessibleName(QStringLiteral("Record type"));
    m_dnsType->addItems(dnsRecordTypes());
    dnsRow->addWidget(m_dnsType);
    m_dnsButton = button(QStringLiteral("Look up"), false);
    dnsRow->addWidget(m_dnsButton);
    dnsLayout->addLayout(dnsRow);
    m_dnsStatus = mutedLabel(QStringLiteral("Uses this computer's DNS resolver."));
    dnsLayout->addWidget(m_dnsStatus);
    m_dnsAnswers = table(QStringLiteral("nexusDnsAnswers"), QStringLiteral("DNS answers"),
                         {QStringLiteral("Name"), QStringLiteral("Type"), QStringLiteral("TTL"), QStringLiteral("Value")});
    dnsLayout->addWidget(m_dnsAnswers, 1);
    splitter->addWidget(dns);

    auto *web = new QWidget;
    auto *webLayout = pageLayout(web);
    webLayout->addWidget(heading(QStringLiteral("Web and TLS")));
    auto *webRow = new QHBoxLayout;
    m_url = new QLineEdit(QStringLiteral("https://"));
    m_url->setObjectName(QStringLiteral("nexusUrl"));
    m_url->setAccessibleName(QStringLiteral("Address to inspect"));
    webRow->addWidget(m_url, 1);
    m_inspectButton = button(QStringLiteral("Inspect"), false);
    webRow->addWidget(m_inspectButton);
    webLayout->addLayout(webRow);
    m_inspectStatus = mutedLabel(QStringLiteral("Follows redirects, checks the certificate the normal way and reviews security headers. "
                                                "The request goes through the desktop firewall like any other."));
    webLayout->addWidget(m_inspectStatus);
    m_inspection = table(QStringLiteral("nexusInspection"), QStringLiteral("Web inspection"), {QStringLiteral("Item"), QStringLiteral("Value")});
    m_inspection->setRootIsDecorated(true);
    webLayout->addWidget(m_inspection, 1);
    splitter->addWidget(web);
    layout->addWidget(splitter, 1);

    connect(m_dnsButton, &QPushButton::clicked, this, [this] { lookupDnsName(); });
    connect(m_dnsName, &QLineEdit::returnPressed, this, [this] { lookupDnsName(); });
    connect(m_inspectButton, &QPushButton::clicked, this, [this] { inspectWeb(); });
    connect(m_url, &QLineEdit::returnPressed, this, [this] { inspectWeb(); });
    return page;
}

QWidget *NexusFeature::buildHistory()
{
    auto *page = new QWidget;
    auto *layout = pageLayout(page);
    auto *row = new QHBoxLayout;
    m_exportJson = button(QStringLiteral("Export JSON…"), false);
    m_exportCsv = button(QStringLiteral("Export CSV…"), false);
    m_deleteScan = button(QStringLiteral("Delete"), false);
    m_clearHistory = button(QStringLiteral("Clear history…"), false);
    for (QPushButton *action : {m_exportJson, m_exportCsv, m_deleteScan})
        row->addWidget(action);
    row->addStretch();
    row->addWidget(m_clearHistory);
    layout->addLayout(row);
    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->setChildrenCollapsible(false);
    m_history = table(QStringLiteral("nexusHistory"), QStringLiteral("Saved scans"),
                      {QStringLiteral("Started"), QStringLiteral("Target"), QStringLiteral("Ports"), QStringLiteral("Open"), QStringLiteral("Status")});
    splitter->addWidget(m_history);
    m_historyDetail = table(QStringLiteral("nexusHistoryDetail"), QStringLiteral("Open ports in the selected scan"),
                            {QStringLiteral("Address"), QStringLiteral("Port"), QStringLiteral("Service"), QStringLiteral("Details")});
    splitter->addWidget(m_historyDetail);
    layout->addWidget(splitter, 1);

    connect(m_history, &QTreeWidget::currentItemChanged, this, [this] { renderHistoryDetail(); });
    connect(m_exportJson, &QPushButton::clicked, this, [this] { exportSelected(false); });
    connect(m_exportCsv, &QPushButton::clicked, this, [this] { exportSelected(true); });
    connect(m_deleteScan, &QPushButton::clicked, this, [this] { deleteSelected(); });
    connect(m_clearHistory, &QPushButton::clicked, this, [this] { clearHistory(); });
    return page;
}

QWidget *NexusFeature::buildSettings()
{
    auto *page = new QWidget;
    auto *layout = pageLayout(page);
    layout->addWidget(heading(QStringLiteral("Scan defaults")));
    auto *form = new QFormLayout;
    m_timeout = new QDoubleSpinBox;
    m_timeout->setRange(0.2, 30.0);
    m_timeout->setSingleStep(0.1);
    m_timeout->setDecimals(1);
    m_timeout->setSuffix(QStringLiteral(" s"));
    m_timeout->setAccessibleName(QStringLiteral("Connection timeout in seconds"));
    form->addRow(QStringLiteral("Connection timeout"), m_timeout);
    m_concurrency = new QSpinBox;
    m_concurrency->setRange(1, 256);
    m_concurrency->setAccessibleName(QStringLiteral("Connections at once"));
    form->addRow(QStringLiteral("Connections at once"), m_concurrency);
    layout->addLayout(form);
    layout->addWidget(heading(QStringLiteral("Authorization")));
    m_allowPublic = new QCheckBox(QStringLiteral("Allow internet targets"));
    layout->addWidget(m_allowPublic);
    layout->addWidget(mutedLabel(QStringLiteral("Off by default: NEXUS scans only this computer and private networks (10/8, 172.16/12, 192.168/16, "
                                                "100.64/10, link-local and IPv6 local addresses). With it on, up to four single internet hosts can be "
                                                "scanned after you confirm you own them or have written permission to test them.")));
    auto *authorizedForm = new QFormLayout;
    m_authorized = new QLineEdit;
    m_authorized->setObjectName(QStringLiteral("nexusAuthorizedNetworks"));
    m_authorized->setPlaceholderText(QStringLiteral("203.0.113.0/28, 198.51.100.7"));
    m_authorized->setAccessibleName(QStringLiteral("Authorized networks"));
    authorizedForm->addRow(QStringLiteral("Authorized networks"), m_authorized);
    layout->addLayout(authorizedForm);
    layout->addWidget(mutedLabel(QStringLiteral("Networks you are responsible for, such as a server range you rent. They are treated like your own "
                                                "network, so ranges inside them can be scanned without the internet limit.")));
    m_settingsStatus = mutedLabel({});
    layout->addWidget(m_settingsStatus);
    layout->addStretch();

    connect(m_timeout, &QDoubleSpinBox::valueChanged, this, [this] { settingsChanged(); });
    connect(m_concurrency, &QSpinBox::valueChanged, this, [this] { settingsChanged(); });
    connect(m_allowPublic, &QCheckBox::toggled, this, [this] { settingsChanged(); });
    connect(m_authorized, &QLineEdit::editingFinished, this, [this] { settingsChanged(); });
    return page;
}

void NexusFeature::showSection(Section section)
{
    if (QAbstractButton *sectionButton = m_sections->button(section))
        sectionButton->setChecked(true);
    m_stack->setCurrentIndex(section);
    if (section == Scan && m_target->text().trimmed().isEmpty())
        m_target->setText(localNetwork());
}

void NexusFeature::refresh()
{
    load();
    loadSettingsIntoForm();
    renderOverview();
    renderDevices();
    renderHistory();
}

void NexusFeature::load()
{
    if (m_loaded)
        return;
    m_loaded = true;
    QFile file(QDir(forkmesh::paths::pluginDataDir(QStringLiteral("nexus"))).filePath(QStringLiteral("nexus.json")));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (document.isObject())
        m_archive = archiveFromJson(document.object());
}

void NexusFeature::save()
{
    const QString directory = forkmesh::paths::pluginDataDir(QStringLiteral("nexus"));
    if (!QDir().mkpath(directory))
        return;
    QSaveFile file(QDir(directory).filePath(QStringLiteral("nexus.json")));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(archiveToJson(m_archive)).toJson(QJsonDocument::Indented));
    file.commit();
}

QString NexusFeature::localNetwork() const
{
    QList<Network> addresses;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        for (const QNetworkAddressEntry &entry : interface.addressEntries())
            addresses << Network(entry.ip(), entry.prefixLength());
    }
    return suggestedLocalNetwork(addresses);
}

const ScanSession *NexusFeature::previousScan(const ScanSession &session) const
{
    bool past = false;
    for (const ScanSession &candidate : m_archive.scans) {
        if (candidate.id == session.id) {
            past = true;
            continue;
        }
        if (past && candidate.target == session.target)
            return &candidate;
    }
    return nullptr;
}

void NexusFeature::renderOverview()
{
    const int open = m_archive.scans.isEmpty() ? 0 : openCount(m_archive.scans.constFirst());
    m_overviewSummary->setText(QStringLiteral("%1 · %2 open in the last scan · %3 saved")
                                   .arg(m_archive.devices.size() == 1 ? QStringLiteral("1 device") : QStringLiteral("%1 devices").arg(m_archive.devices.size()))
                                   .arg(open)
                                   .arg(m_archive.scans.size() == 1 ? QStringLiteral("1 scan") : QStringLiteral("%1 scans").arg(m_archive.scans.size())));
    const QString network = localNetwork();
    m_overviewScan->setText(network.isEmpty() ? QStringLiteral("Scan a network") : QStringLiteral("Scan %1").arg(network));
    m_observations->clear();
    QStringList observations;
    for (const ScanSession &session : m_archive.scans.mid(0, 3))
        observations << portChanges(session, previousScan(session));
    observations.removeDuplicates();
    if (observations.isEmpty())
        observations << QStringLiteral("No observations yet. Run a discovery or a scan to build a picture of your network.");
    for (const QString &observation : std::as_const(observations))
        new QTreeWidgetItem(m_observations, {observation});
    m_recentDevices->clear();
    for (const DiscoveredDevice &device : m_archive.devices.mid(0, 8)) {
        auto *item = new QTreeWidgetItem(m_recentDevices, {device.name, device.addresses.join(QStringLiteral(", ")), device.services.join(QStringLiteral(", "))});
        item->setData(0, Qt::UserRole, device.addresses.value(0));
    }
    if (m_archive.devices.isEmpty())
        new QTreeWidgetItem(m_recentDevices, {QStringLiteral("No devices discovered yet.")});
}

void NexusFeature::renderDevices()
{
    const QString selected = m_devices->currentItem() ? m_devices->currentItem()->text(0) : QString();
    m_devices->setSortingEnabled(false);
    m_devices->clear();
    for (const DiscoveredDevice &device : std::as_const(m_archive.devices)) {
        auto *item = new QTreeWidgetItem(m_devices, {device.name, device.addresses.join(QStringLiteral(", ")), device.hostnames.join(QStringLiteral(", ")),
                                                     device.services.join(QStringLiteral(", ")), portsText(device.ports), shortTime(device.lastSeen)});
        item->setData(0, Qt::UserRole, device.addresses.value(0));
        if (device.name == selected)
            m_devices->setCurrentItem(item);
    }
    m_devices->setSortingEnabled(true);
}

void NexusFeature::renderScanResults()
{
    m_results->setSortingEnabled(false);
    m_results->clear();
    const bool showAll = m_showClosed->isChecked();
    for (const PortResult &result : std::as_const(m_currentScan.results)) {
        if (!showAll && result.state != PortState::Open)
            continue;
        QString service = serviceTitle(result.service);
        if (result.state == PortState::Open && !result.confirmed && !service.isEmpty())
            service += QStringLiteral(" (inferred)");
        auto *item = new QTreeWidgetItem;
        item->setText(0, result.address);
        item->setData(1, Qt::DisplayRole, int(result.port));
        item->setText(2, stateTitle(result.state));
        item->setText(3, service);
        if (result.latencyMs >= 0)
            item->setData(4, Qt::DisplayRole, result.latencyMs);
        item->setText(5, result.detail);
        item->setToolTip(5, result.detail);
        m_results->addTopLevelItem(item);
    }
    m_results->setSortingEnabled(true);
}

void NexusFeature::renderHistory()
{
    const QString selected = m_history->currentItem() ? m_history->currentItem()->data(0, Qt::UserRole).toString() : QString();
    m_history->clear();
    for (const ScanSession &session : std::as_const(m_archive.scans)) {
        const QString status = session.cancelled ? QStringLiteral("Stopped") : QStringLiteral("Complete");
        auto *item = new QTreeWidgetItem(m_history, {shortTime(session.startedAt), session.target, session.ports,
                                                     QString::number(openCount(session)), status});
        item->setData(0, Qt::UserRole, session.id);
        if (session.id == selected)
            m_history->setCurrentItem(item);
    }
    if (!m_history->currentItem() && m_history->topLevelItemCount() > 0)
        m_history->setCurrentItem(m_history->topLevelItem(0));
    const bool any = !m_archive.scans.isEmpty();
    m_exportJson->setEnabled(any);
    m_exportCsv->setEnabled(any);
    m_deleteScan->setEnabled(any);
    m_clearHistory->setEnabled(any || !m_archive.devices.isEmpty());
    renderHistoryDetail();
}

void NexusFeature::renderHistoryDetail()
{
    m_historyDetail->clear();
    QTreeWidgetItem *current = m_history->currentItem();
    if (!current)
        return;
    const QString id = current->data(0, Qt::UserRole).toString();
    for (const ScanSession &session : std::as_const(m_archive.scans)) {
        if (session.id != id)
            continue;
        for (const PortResult &result : session.results) {
            if (result.state != PortState::Open)
                continue;
            auto *item = new QTreeWidgetItem;
            item->setText(0, result.address);
            item->setData(1, Qt::DisplayRole, int(result.port));
            item->setText(2, serviceTitle(result.service));
            item->setText(3, result.detail);
            m_historyDetail->addTopLevelItem(item);
        }
        if (m_historyDetail->topLevelItemCount() == 0)
            new QTreeWidgetItem(m_historyDetail, {QStringLiteral("No open ports in this scan.")});
        return;
    }
}

void NexusFeature::loadSettingsIntoForm()
{
    m_loadingSettings = true;
    m_timeout->setValue(m_archive.settings.timeoutMs / 1000.0);
    m_concurrency->setValue(m_archive.settings.concurrency);
    m_allowPublic->setChecked(m_archive.settings.allowPublicTargets);
    m_authorized->setText(m_archive.settings.authorizedNetworks);
    m_loadingSettings = false;
}

void NexusFeature::settingsChanged()
{
    if (m_loadingSettings)
        return;
    QString error;
    const QList<Network> networks = parseNetworks(m_authorized->text(), &error);
    m_settingsStatus->setText(error);
    m_archive.settings.timeoutMs = int(m_timeout->value() * 1000);
    m_archive.settings.concurrency = m_concurrency->value();
    m_archive.settings.allowPublicTargets = m_allowPublic->isChecked();
    m_archive.settings.authorizedNetworks = describeNetworks(networks);
    save();
}

void NexusFeature::startDiscovery()
{
    m_discoverButton->setText(QStringLiteral("Stop"));
    m_overviewDiscover->setEnabled(false);
    m_discoverStatus->setText(QStringLiteral("Asking devices on this network to announce their services…"));
    m_browser->start(defaultServiceTypes(), 8,
                     [this](const QList<DiscoveredDevice> &devices, int round, int rounds) {
                         m_discoverStatus->setText(QStringLiteral("Listening for announcements (%1 of %2 seconds): %3 found so far.")
                                                       .arg(round).arg(rounds).arg(devices.size()));
                     },
                     [this](const QList<DiscoveredDevice> &devices, const QString &error) {
                         m_discoverButton->setText(QStringLiteral("Discover"));
                         m_overviewDiscover->setEnabled(true);
                         if (!error.isEmpty()) {
                             m_discoverStatus->setText(error);
                             return;
                         }
                         QList<DiscoveredDevice> merged = devices;
                         for (const DiscoveredDevice &old : std::as_const(m_archive.devices)) {
                             const bool seen = std::any_of(merged.cbegin(), merged.cend(), [&old](const DiscoveredDevice &device) {
                                 return device.name == old.name;
                             });
                             if (!seen && merged.size() < 500)
                                 merged << old;
                         }
                         m_archive.devices = merged;
                         save();
                         m_discoverStatus->setText(devices.isEmpty()
                                                       ? QStringLiteral("No devices answered. Some networks block multicast DNS; you can still scan an address or range.")
                                                       : QStringLiteral("%1 answered this time. Double-click a device to scan it.").arg(devices.size()));
                         renderDevices();
                         renderOverview();
                         renderHistory();
                         if (m_searchIndexChanged)
                             m_searchIndexChanged();
                     });
}

void NexusFeature::scanDevice(const QString &address)
{
    if (address.isEmpty())
        return;
    showSection(Scan);
    m_target->setText(address);
    m_ports->setText(describePorts(presetPorts(PortPreset::Common)));
    startScan();
}

void NexusFeature::startScan()
{
    const PortParse ports = parsePorts(m_ports->text());
    if (!ports.error.isEmpty()) {
        m_scanStatus->setText(ports.error);
        return;
    }
    const ScanPolicy policy = policyFor(m_archive.settings);
    const TargetParse target = parseTarget(m_target->text(), policy.maximumHosts);
    if (!target.error.isEmpty()) {
        m_scanStatus->setText(target.error);
        return;
    }
    const QString targetText = m_target->text().trimmed();
    if (target.hostname.isEmpty()) {
        runScan(target.addresses, target.isRange, targetText, ports.ports);
        return;
    }
    m_scanButton->setEnabled(false);
    m_scanStatus->setText(QStringLiteral("Looking up %1…").arg(target.hostname));
    const QList<quint16> portList = ports.ports;
    QHostInfo::lookupHost(target.hostname, this, [this, targetText, portList](const QHostInfo &info) {
        m_scanButton->setEnabled(true);
        QList<QHostAddress> addresses = info.addresses();
        std::stable_sort(addresses.begin(), addresses.end(), [](const QHostAddress &a, const QHostAddress &b) {
            return a.protocol() == QAbstractSocket::IPv4Protocol && b.protocol() != QAbstractSocket::IPv4Protocol;
        });
        if (info.error() != QHostInfo::NoError || addresses.isEmpty()) {
            m_scanStatus->setText(QStringLiteral("%1 could not be resolved: %2").arg(targetText, info.errorString()));
            return;
        }
        runScan({addresses.constFirst()}, false, targetText, portList);
    });
}

void NexusFeature::runScan(const QList<QHostAddress> &addresses, bool isRange, const QString &target, const QList<quint16> &ports)
{
    const ScanPolicy policy = policyFor(m_archive.settings);
    Decision decision = evaluate(policy, addresses, int(ports.size()), isRange, false);
    if (decision.kind == DecisionKind::NeedsAttestation) {
        QMessageBox box(QMessageBox::Question, QStringLiteral("Confirm authorization"),
                        QStringLiteral("%1\n\nI own these targets or have written permission to test them.").arg(decision.message),
                        QMessageBox::NoButton, this);
        QPushButton *confirm = box.addButton(QStringLiteral("I am authorized"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != confirm) {
            m_scanStatus->setText(QStringLiteral("Scan cancelled."));
            return;
        }
        decision = evaluate(policy, addresses, int(ports.size()), isRange, true);
    }
    if (decision.kind != DecisionKind::Allowed) {
        m_scanStatus->setText(decision.message);
        return;
    }
    m_currentScan = ScanSession{};
    m_currentScan.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_currentScan.target = target;
    m_currentScan.ports = describePorts(ports);
    m_currentScan.startedAt = QDateTime::currentDateTimeUtc();
    m_skippedByFirewall = 0;
    QList<QHostAddress> allowed;
    for (const QHostAddress &address : addresses) {
        if (deniedByFirewall(address, 0))
            ++m_skippedByFirewall;
        else
            allowed << address;
    }
    m_currentScan.probes = int(allowed.size() * ports.size());
    m_scanFindings->hide();
    renderScanResults();
    m_scanButton->setText(QStringLiteral("Stop"));
    m_target->setEnabled(false);
    m_ports->setEnabled(false);
    m_progress->setRange(0, qMax(1, m_currentScan.probes));
    m_progress->setValue(0);
    m_progress->show();
    m_scanStatus->setText(QStringLiteral("Scanning %1 ports on %2…").arg(ports.size()).arg(allowed.size() == 1 ? allowed.constFirst().toString() : QStringLiteral("%1 hosts").arg(allowed.size())));
    ScanOptions options;
    options.timeoutMs = m_archive.settings.timeoutMs;
    options.concurrency = m_archive.settings.concurrency;
    m_scanner->start(
        allowed, ports, options,
        [this](const PortResult &result) {
            m_currentScan.results << result;
            if (result.state == PortState::Open || m_showClosed->isChecked())
                renderScanResults();
        },
        [this](int completed, int total) {
            m_progress->setValue(completed);
            m_scanStatus->setText(QStringLiteral("Scanned %1 of %2 · %3 open").arg(completed).arg(total).arg(openCount(m_currentScan)));
        },
        [this](bool cancelled) { scanFinished(cancelled); });
}

void NexusFeature::scanFinished(bool cancelled)
{
    m_currentScan.cancelled = cancelled;
    m_currentScan.finishedAt = QDateTime::currentDateTimeUtc();
    m_scanButton->setText(QStringLiteral("Start scan"));
    m_target->setEnabled(true);
    m_ports->setEnabled(true);
    m_progress->hide();
    const int open = openCount(m_currentScan);
    QString status = QStringLiteral("%1 · %2 open of %3 checked.")
                         .arg(cancelled ? QStringLiteral("Scan stopped") : QStringLiteral("Scan complete"))
                         .arg(open)
                         .arg(m_currentScan.results.size());
    if (m_skippedByFirewall > 0)
        status += QStringLiteral(" %1 skipped because your firewall blocks them.").arg(m_skippedByFirewall == 1 ? QStringLiteral("1 host was") : QStringLiteral("%1 hosts were").arg(m_skippedByFirewall));
    m_scanStatus->setText(status);
    renderScanResults();
    m_archive.scans.prepend(m_currentScan);
    while (m_archive.scans.size() > MaximumStoredScans)
        m_archive.scans.removeLast();
    const QStringList findings = portChanges(m_currentScan, previousScan(m_currentScan));
    m_scanFindings->setText(findings.join(u'\n'));
    m_scanFindings->setVisible(!findings.isEmpty());
    save();
    renderHistory();
    renderOverview();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

bool NexusFeature::deniedByFirewall(const QHostAddress &address, quint16 port) const
{
    auto *manager = qobject_cast<BackoffNetworkAccessManager *>(m_network);
    if (!manager || !manager->firewallEnabled())
        return false;
    QUrl url;
    url.setScheme(QStringLiteral("tcp"));
    url.setHost(address.toString());
    if (port != 0)
        url.setPort(port);
    return manager->firewallMatch(url) == BackoffNetworkAccessManager::FirewallDenied;
}

void NexusFeature::lookupDnsName()
{
    const QString name = m_dnsName->text().trimmed();
    if (name.isEmpty()) {
        m_dnsStatus->setText(QStringLiteral("Enter a name to look up."));
        return;
    }
    m_dnsButton->setEnabled(false);
    m_dnsStatus->setText(QStringLiteral("Looking up %1…").arg(name));
    lookupDns(this, name, m_dnsType->currentText(), [this](const DnsLookupResult &result) {
        m_dnsButton->setEnabled(true);
        m_dnsAnswers->clear();
        if (!result.error.isEmpty()) {
            m_dnsStatus->setText(QStringLiteral("%1 %2: %3").arg(result.type, result.name, result.error));
            return;
        }
        m_dnsStatus->setText(QStringLiteral("%1 %2 · %3 in %4 ms").arg(result.type, result.name,
                                                                        result.answers.size() == 1 ? QStringLiteral("1 answer") : QStringLiteral("%1 answers").arg(result.answers.size()))
                                 .arg(result.elapsedMs));
        for (const DnsAnswer &answer : result.answers) {
            auto *item = new QTreeWidgetItem(m_dnsAnswers);
            item->setText(0, answer.name);
            item->setText(1, answer.type);
            item->setData(2, Qt::DisplayRole, answer.ttl);
            item->setText(3, answer.value);
        }
    });
}

void NexusFeature::inspectWeb()
{
    QString text = m_url->text().trimmed();
    if (!text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("https://"));
    const QUrl url = QUrl::fromUserInput(text);
    m_inspectButton->setEnabled(false);
    m_inspection->clear();
    m_inspectStatus->setText(QStringLiteral("Inspecting %1…").arg(url.toDisplayString()));
    inspectHttp(m_network, this, url, [this](const HttpInspection &inspection) {
        m_inspectButton->setEnabled(true);
        m_inspection->clear();
        auto group = [this](const QString &title) {
            auto *item = new QTreeWidgetItem(m_inspection, {title});
            QFont font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
            item->setExpanded(true);
            return item;
        };
        if (!inspection.error.isEmpty())
            m_inspectStatus->setText(inspection.error);
        if (!inspection.hops.isEmpty()) {
            const HttpHop &last = inspection.hops.constLast();
            if (inspection.error.isEmpty())
                m_inspectStatus->setText(QStringLiteral("%1 %2 · %3 ms").arg(last.status).arg(last.reason).arg(last.elapsedMs));
            QTreeWidgetItem *chain = group(QStringLiteral("Requests"));
            for (const HttpHop &hop : inspection.hops)
                new QTreeWidgetItem(chain, {QStringLiteral("%1 %2").arg(hop.status).arg(hop.reason), QStringLiteral("%1 · %2 ms").arg(hop.url.toDisplayString()).arg(hop.elapsedMs)});
            chain->setExpanded(true);
            QTreeWidgetItem *headers = group(QStringLiteral("Response headers"));
            for (const auto &header : last.headers)
                new QTreeWidgetItem(headers, {QString::fromLatin1(header.first), QString::fromLatin1(header.second)});
            headers->setExpanded(true);
        }
        if (!inspection.protocol.isEmpty() || !inspection.chain.isEmpty() || !inspection.tlsErrors.isEmpty()) {
            QTreeWidgetItem *tls = group(QStringLiteral("TLS"));
            if (!inspection.protocol.isEmpty())
                new QTreeWidgetItem(tls, {QStringLiteral("Protocol"), QStringLiteral("%1 · %2").arg(inspection.protocol, inspection.cipher)});
            for (const QString &error : inspection.tlsErrors)
                new QTreeWidgetItem(tls, {QStringLiteral("Certificate problem"), error});
            int index = 0;
            for (const CertificateSummary &certificate : inspection.chain) {
                const QString role = index == 0 ? QStringLiteral("Certificate") : QStringLiteral("Issuer %1").arg(index);
                new QTreeWidgetItem(tls, {role, QStringLiteral("%1 · issued by %2 · valid %3 to %4")
                                                    .arg(certificate.subject, certificate.issuer,
                                                         QLocale().toString(certificate.notBefore.date(), QLocale::ShortFormat),
                                                         QLocale().toString(certificate.notAfter.date(), QLocale::ShortFormat))});
                ++index;
            }
            tls->setExpanded(true);
        }
        if (!inspection.findings.isEmpty()) {
            QTreeWidgetItem *findings = group(QStringLiteral("Observations"));
            for (const HeaderFinding &finding : inspection.findings) {
                const QString severity = finding.severity == QLatin1String("warning") ? QStringLiteral("Warning")
                                         : finding.severity == QLatin1String("review") ? QStringLiteral("Review")
                                                                                        : QStringLiteral("Note");
                new QTreeWidgetItem(findings, {severity, finding.text});
            }
            findings->setExpanded(true);
        }
    });
}

void NexusFeature::exportSelected(bool csv)
{
    QTreeWidgetItem *current = m_history->currentItem();
    if (!current)
        return;
    const QString id = current->data(0, Qt::UserRole).toString();
    const auto found = std::find_if(m_archive.scans.cbegin(), m_archive.scans.cend(), [&id](const ScanSession &session) { return session.id == id; });
    if (found == m_archive.scans.cend())
        return;
    QString safeTarget = found->target;
    safeTarget.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9.-]+")), QStringLiteral("_"));
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
                                  .filePath(QStringLiteral("nexus-%1-%2.%3").arg(safeTarget, found->startedAt.toString(QStringLiteral("yyyyMMdd-HHmm")), csv ? QStringLiteral("csv") : QStringLiteral("json")));
    const QString path = QFileDialog::getSaveFileName(this, csv ? QStringLiteral("Export scan as CSV") : QStringLiteral("Export scan as JSON"), suggested,
                                                      csv ? QStringLiteral("CSV files (*.csv)") : QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("Export scan"), file.errorString());
        return;
    }
    file.write(csv ? scanCsv(*found) : QJsonDocument(scanToJson(*found)).toJson(QJsonDocument::Indented));
    if (!file.commit())
        QMessageBox::warning(this, QStringLiteral("Export scan"), file.errorString());
}

void NexusFeature::deleteSelected()
{
    QTreeWidgetItem *current = m_history->currentItem();
    if (!current)
        return;
    const QString id = current->data(0, Qt::UserRole).toString();
    m_archive.scans.erase(std::remove_if(m_archive.scans.begin(), m_archive.scans.end(), [&id](const ScanSession &session) { return session.id == id; }),
                          m_archive.scans.end());
    save();
    renderHistory();
    renderOverview();
}

void NexusFeature::clearHistory()
{
    if (QMessageBox::question(this, QStringLiteral("Clear history"),
                              QStringLiteral("Remove every saved scan and discovered device from NEXUS on this computer?"))
        != QMessageBox::Yes)
        return;
    m_archive.scans.clear();
    m_archive.devices.clear();
    save();
    renderHistory();
    renderDevices();
    renderOverview();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

QList<forkmesh::market::NativePackageSearchHit> NexusFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty())
        return hits;
    for (const DiscoveredDevice &device : m_archive.devices) {
        if (hits.size() >= limit)
            break;
        const QString haystack = QStringList{device.name, device.hostnames.join(u' '), device.addresses.join(u' '), device.services.join(u' ')}.join(u' ');
        if (haystack.contains(needle, Qt::CaseInsensitive))
            hits << forkmesh::market::NativePackageSearchHit{QStringLiteral("device:") + device.name, device.name,
                                                             QStringLiteral("NEXUS device · %1").arg(device.addresses.join(QStringLiteral(", ")))};
    }
    for (const ScanSession &session : m_archive.scans) {
        if (hits.size() >= limit)
            break;
        if (session.target.contains(needle, Qt::CaseInsensitive))
            hits << forkmesh::market::NativePackageSearchHit{QStringLiteral("scan:") + session.id, session.target,
                                                             QStringLiteral("NEXUS scan · %1 open · %2").arg(openCount(session)).arg(shortTime(session.startedAt))};
    }
    return hits;
}

void NexusFeature::activateSearchHit(const QString &id)
{
    load();
    if (id.startsWith(QLatin1String("device:"))) {
        renderDevices();
        showSection(Discover);
        const QString name = id.mid(7);
        for (int i = 0; i < m_devices->topLevelItemCount(); ++i) {
            if (m_devices->topLevelItem(i)->text(0) == name) {
                m_devices->setCurrentItem(m_devices->topLevelItem(i));
                break;
            }
        }
    } else if (id.startsWith(QLatin1String("scan:"))) {
        renderHistory();
        showSection(History);
        const QString scan = id.mid(5);
        for (int i = 0; i < m_history->topLevelItemCount(); ++i) {
            if (m_history->topLevelItem(i)->data(0, Qt::UserRole).toString() == scan) {
                m_history->setCurrentItem(m_history->topLevelItem(i));
                break;
            }
        }
    }
}

bool NexusFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    load();
    const bool hasData = !m_archive.scans.isEmpty() || !m_archive.devices.isEmpty();
    done(true,
         QJsonObject{{QStringLiteral("hasData"), hasData},
                     {QStringLiteral("filename"), QStringLiteral("forkmesh-nexus.json")},
                     {QStringLiteral("export"), archiveToJson(m_archive)}},
         QString());
    return true;
}

}

namespace {
const forkmesh::market::NativePackageRegistration nexusRegistration({
    QStringLiteral("builtin:nexus"),
    {QStringLiteral("extension.native")},
    QStringLiteral("NEXUS"),
    QStringLiteral("pulse"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::nexus::NexusFeature(host, parent);
    },
});
}

#include "OpenWrtFeature.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <optional>

namespace forkmesh::openwrt {
namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kSectionRole = Qt::UserRole + 1;

enum Tab { OverviewTab, NetworkTab, WirelessTab, ClientsTab, ForwardsTab, ServicesTab, PackagesTab, SystemTab, AdvancedTab };

const QStringList &criticalServices()
{
    static const QStringList names = {QStringLiteral("network"), QStringLiteral("firewall"), QStringLiteral("dropbear"),
                                      QStringLiteral("uhttpd"), QStringLiteral("rpcd"), QStringLiteral("dnsmasq"),
                                      QStringLiteral("odhcpd"), QStringLiteral("ubus"), QStringLiteral("log")};
    return names;
}

QLabel *mutedLabel(const QString &text = {})
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *sectionTitle(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QPushButton *makeButton(const QString &text, const QString &style = QStringLiteral("ghostButton"))
{
    auto *button = new QPushButton(text);
    button->setObjectName(style);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QTreeWidget *makeTree(const QString &name, const QStringList &headers, const QString &accessible)
{
    auto *tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setAccessibleName(accessible);
    tree->setColumnCount(headers.size());
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setStretchLastSection(true);
    for (int column = 0; column + 1 < headers.size(); ++column)
        tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    return tree;
}

QLineEdit *makeField(const QString &placeholder = {})
{
    auto *field = new QLineEdit;
    field->setPlaceholderText(placeholder);
    field->setClearButtonEnabled(true);
    return field;
}

void fillChoices(QComboBox *combo, const QList<Choice> &choices)
{
    combo->clear();
    for (const Choice &choice : choices)
        combo->addItem(choice.label, choice.value);
}

void setComboValue(QComboBox *combo, const QString &value)
{
    int index = combo->findData(value);
    if (index < 0 && !value.isEmpty()) {
        combo->addItem(value, value);
        index = combo->count() - 1;
    }
    combo->setCurrentIndex(qMax(0, index));
}

QString itemKey(const QTreeWidgetItem *item)
{
    return item ? item->data(0, kKeyRole).toString() : QString();
}

bool itemMatches(const QTreeWidgetItem *item, const QStringList &needles)
{
    for (const QString &needle : needles) {
        if (needle.isEmpty())
            continue;
        bool found = false;
        for (int column = 0; column < item->columnCount() && !found; ++column)
            found = item->text(column).contains(needle, Qt::CaseInsensitive);
        if (!found)
            return false;
    }
    return true;
}

void applyFilter(QTreeWidget *tree, const QStringList &needles)
{
    if (!tree)
        return;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = tree->topLevelItem(i);
        bool childMatch = false;
        for (int c = 0; c < item->childCount(); ++c) {
            const bool match = itemMatches(item->child(c), needles);
            item->child(c)->setHidden(!match && !itemMatches(item, needles));
            childMatch = childMatch || match;
        }
        item->setHidden(!itemMatches(item, needles) && !childMatch);
    }
}

bool validAddressList(const QString &text, bool allowPrefix)
{
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return false;
    for (const QString &part : parts) {
        const QStringList pieces = part.split(QLatin1Char('/'));
        if (pieces.size() > 2 || (pieces.size() == 2 && !allowPrefix) || !validIpv4(pieces.first()))
            return false;
        if (pieces.size() == 2) {
            bool ok = false;
            const int prefix = pieces.at(1).toInt(&ok);
            if (!ok || prefix < 0 || prefix > 32)
                return false;
        }
    }
    return true;
}

bool validDnsList(const QString &text)
{
    for (const QString &part : text.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts)) {
        if (!validIpv4(part) && !part.contains(QLatin1Char(':')))
            return false;
    }
    return true;
}

QString wifiBand(const UciSection &radio)
{
    const QString band = radio.option(QStringLiteral("band"));
    if (band == QStringLiteral("2g"))
        return QStringLiteral("2.4 GHz");
    if (band == QStringLiteral("5g"))
        return QStringLiteral("5 GHz");
    if (band == QStringLiteral("6g"))
        return QStringLiteral("6 GHz");
    if (band == QStringLiteral("60g"))
        return QStringLiteral("60 GHz");
    const QString hwmode = radio.option(QStringLiteral("hwmode"));
    if (hwmode == QStringLiteral("11a"))
        return QStringLiteral("5 GHz");
    if (!hwmode.isEmpty())
        return QStringLiteral("2.4 GHz");
    return QStringLiteral("Radio");
}

QString sanitizedHostname(const QString &name, const QString &mac)
{
    QString clean = name;
    clean.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9-]")), QStringLiteral("-"));
    clean = clean.left(63);
    while (clean.startsWith(QLatin1Char('-')))
        clean.remove(0, 1);
    while (clean.endsWith(QLatin1Char('-')))
        clean.chop(1);
    if (clean.isEmpty())
        clean = QStringLiteral("device-") + QString(mac).remove(QLatin1Char(':')).right(6).toLower();
    return clean;
}

}

OpenWrtFeature::OpenWrtFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent)
    , m_host(host)
    , m_client(host.network)
    , m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("openWrtPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *headingRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Router"));
    title->setObjectName(QStringLiteral("pageTitle"));
    headingRow->addWidget(title);
    m_routerLabel = mutedLabel();
    m_routerLabel->setWordWrap(false);
    headingRow->addWidget(m_routerLabel);
    headingRow->addStretch();
    m_refreshButton = makeButton(QStringLiteral("Refresh"));
    m_signOutButton = makeButton(QStringLiteral("Sign out"));
    headingRow->addWidget(m_refreshButton);
    headingRow->addWidget(m_signOutButton);
    outer->addLayout(headingRow);

    m_status = mutedLabel();
    m_status->setObjectName(QStringLiteral("statusLine"));
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->hide();
    outer->addWidget(m_status);

    m_pages = new QStackedWidget;
    m_pages->addWidget(buildSignIn());
    m_pages->addWidget(buildManager());
    outer->addWidget(m_pages, 1);

    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_signOutButton, &QPushButton::clicked, this, [this] { signOut(); });
    m_client.setSessionRenewed([this] {
        if (pendingCount(m_changes) > 0)
            showStatus(QStringLiteral("The router session timed out, so the router dropped the changes you had not applied. ForkMesh signed in again."), true);
        loadChanges();
    });
    m_refreshButton->setVisible(false);
    m_signOutButton->setVisible(false);
}

OpenWrtFeature::~OpenWrtFeature() = default;

QWidget *OpenWrtFeature::buildSignIn()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->setSpacing(10);
    layout->addWidget(sectionTitle(QStringLiteral("Connect to your OpenWrt router")));
    layout->addWidget(mutedLabel(QStringLiteral("Manage an OpenWrt router on your network: its internet and local network, Wi-Fi, connected devices, port forwards, services, packages, firmware and every setting. "
                                                 "ForkMesh talks to the router directly over your local network, the same way its LuCI web page does.")));

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_address = makeField(QStringLiteral("192.168.1.1"));
    m_address->setAccessibleName(QStringLiteral("Router address"));
    m_user = makeField(QStringLiteral("root"));
    m_user->setAccessibleName(QStringLiteral("Router user name"));
    m_password = new QLineEdit;
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setAccessibleName(QStringLiteral("Router password"));
    form->addRow(QStringLiteral("Address"), m_address);
    form->addRow(QStringLiteral("User"), m_user);
    form->addRow(QStringLiteral("Password"), m_password);
    layout->addLayout(form);

    auto *actions = new QHBoxLayout;
    m_connectButton = makeButton(QStringLiteral("Connect"), QStringLiteral("primaryButton"));
    actions->addWidget(m_connectButton);
    actions->addStretch();
    layout->addLayout(actions);

    m_signInNote = mutedLabel();
    layout->addWidget(m_signInNote);
    m_certificateLabel = mutedLabel();
    m_certificateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_certificateLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_certificateLabel->hide();
    layout->addWidget(m_certificateLabel);
    auto *trustRow = new QHBoxLayout;
    m_trustButton = makeButton(QStringLiteral("Trust this certificate"));
    m_trustButton->hide();
    trustRow->addWidget(m_trustButton);
    trustRow->addStretch();
    layout->addLayout(trustRow);
    layout->addWidget(mutedLabel(QStringLiteral("Your password is kept in memory only while this page is open and is sent only to the router. ForkMesh never stores it and the network log records only the size of router traffic.")));
    layout->addStretch();

    const QSettings settings;
    m_address->setText(settings.value(settingsKey(QStringLiteral("address"))).toString());
    m_user->setText(settings.value(settingsKey(QStringLiteral("user")), QStringLiteral("root")).toString());
    if (m_address->text().isEmpty())
        detectGateway();

    const auto updateNote = [this] {
        const Address address = parseAddress(m_address->text());
        if (!address.valid())
            m_signInNote->setText(address.error);
        else if (!address.encrypted)
            m_signInNote->setText(QStringLiteral("Plain HTTP: the password crosses your local network unencrypted. Install luci-ssl on the router and use https:// to encrypt it."));
        else
            m_signInNote->setText(QStringLiteral("HTTPS: the first time, ForkMesh asks you to check the router's certificate fingerprint."));
    };
    updateNote();
    connect(m_address, &QLineEdit::textChanged, this, updateNote);
    connect(m_connectButton, &QPushButton::clicked, this, [this] { connectToRouter(); });
    connect(m_password, &QLineEdit::returnPressed, this, [this] { connectToRouter(); });
    connect(m_trustButton, &QPushButton::clicked, this, [this] { trustCertificate(); });
    return page;
}

QWidget *OpenWrtFeature::buildManager()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_changesBar = new QWidget;
    m_changesBar->setObjectName(QStringLiteral("inlineNotice"));
    auto *bar = new QHBoxLayout(m_changesBar);
    bar->setContentsMargins(0, 0, 0, 0);
    m_changesLabel = new QLabel;
    m_changesLabel->setWordWrap(true);
    bar->addWidget(m_changesLabel, 1);
    m_rollback = new QCheckBox(QStringLiteral("Undo automatically if the router stops answering"));
    m_rollback->setChecked(true);
    m_rollback->setToolTip(QStringLiteral("After applying, ForkMesh confirms the router still answers. If it cannot within %1 seconds, the router puts the old settings back by itself.").arg(kApplyRollbackSeconds));
    bar->addWidget(m_rollback);
    auto *review = makeButton(QStringLiteral("Review"));
    m_discardButton = makeButton(QStringLiteral("Discard"));
    m_applyButton = makeButton(QStringLiteral("Apply"), QStringLiteral("primaryButton"));
    bar->addWidget(review);
    bar->addWidget(m_discardButton);
    bar->addWidget(m_applyButton);
    m_changesBar->hide();
    layout->addWidget(m_changesBar);

    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("openWrtTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(buildOverview(), QStringLiteral("Overview"));
    m_tabs->addTab(buildNetwork(), QStringLiteral("Network"));
    m_tabs->addTab(buildWireless(), QStringLiteral("Wi-Fi"));
    m_tabs->addTab(buildClients(), QStringLiteral("Devices"));
    m_tabs->addTab(buildForwards(), QStringLiteral("Port forwards"));
    m_tabs->addTab(buildServices(), QStringLiteral("Services"));
    m_tabs->addTab(buildPackages(), QStringLiteral("Packages"));
    m_tabs->addTab(buildSystem(), QStringLiteral("System"));
    m_tabs->addTab(buildAdvanced(), QStringLiteral("All settings"));
    layout->addWidget(m_tabs, 1);

    connect(review, &QPushButton::clicked, this, [this] { reviewChanges(); });
    connect(m_discardButton, &QPushButton::clicked, this, [this] { discardChanges(); });
    connect(m_applyButton, &QPushButton::clicked, this, [this] { applyChanges(); });
    return page;
}

QWidget *OpenWrtFeature::buildOverview()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    m_overview = new QLabel;
    m_overview->setTextFormat(Qt::RichText);
    m_overview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_overview->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_overview->setWordWrap(true);
    layout->addWidget(m_overview);
    auto *actions = new QHBoxLayout;
    auto *restart = makeButton(QStringLiteral("Restart router"), QStringLiteral("dangerButton"));
    actions->addWidget(restart);
    actions->addStretch();
    layout->addLayout(actions);
    layout->addStretch();
    connect(restart, &QPushButton::clicked, this, [this] { reboot(); });
    return page;
}

QWidget *OpenWrtFeature::buildNetwork()
{
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    m_interfaceTree = makeTree(QStringLiteral("openWrtInterfaces"),
                               {QStringLiteral("Interface"), QStringLiteral("Protocol"), QStringLiteral("Status"), QStringLiteral("Addresses"), QStringLiteral("Device")},
                               QStringLiteral("Router network interfaces"));
    splitter->addWidget(m_interfaceTree);

    m_interfaceEditor = new QWidget;
    auto *editor = new QVBoxLayout(m_interfaceEditor);
    editor->setContentsMargins(12, 4, 0, 0);
    m_interfaceTitle = sectionTitle({});
    editor->addWidget(m_interfaceTitle);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_proto = new QComboBox;
    fillChoices(m_proto, protocolChoices());
    m_ipaddr = makeField(QStringLiteral("192.168.1.1"));
    m_netmask = makeField(QStringLiteral("255.255.255.0"));
    m_gateway = makeField(QStringLiteral("Optional"));
    m_dns = makeField(QStringLiteral("Optional, for example 1.1.1.1 9.9.9.9"));
    m_pppoeUser = makeField();
    m_pppoePassword = new QLineEdit;
    m_pppoePassword->setEchoMode(QLineEdit::Password);
    m_pppoePassword->setPlaceholderText(QStringLiteral("Leave empty to keep the current one"));
    m_dhcpServer = new QCheckBox(QStringLiteral("Hand out addresses on this network (DHCP server)"));
    m_dhcpStart = new QSpinBox;
    m_dhcpStart->setRange(1, 65535);
    m_dhcpLimit = new QSpinBox;
    m_dhcpLimit->setRange(1, 65535);
    m_leaseTime = makeField(QStringLiteral("12h"));
    form->addRow(QStringLiteral("Protocol"), m_proto);
    form->addRow(QStringLiteral("IP address"), m_ipaddr);
    form->addRow(QStringLiteral("Netmask"), m_netmask);
    form->addRow(QStringLiteral("Gateway"), m_gateway);
    form->addRow(QStringLiteral("DNS servers"), m_dns);
    form->addRow(QStringLiteral("PPPoE user"), m_pppoeUser);
    form->addRow(QStringLiteral("PPPoE password"), m_pppoePassword);
    form->addRow(QString(), m_dhcpServer);
    form->addRow(QStringLiteral("First address"), m_dhcpStart);
    form->addRow(QStringLiteral("Number of addresses"), m_dhcpLimit);
    form->addRow(QStringLiteral("Lease time"), m_leaseTime);
    m_staticRows = {m_ipaddr, m_netmask, m_gateway};
    m_pppoeRows = {m_pppoeUser, m_pppoePassword};
    m_dhcpRows = {m_dhcpServer, m_dhcpStart, m_dhcpLimit, m_leaseTime};
    editor->addLayout(form);
    auto *actions = new QHBoxLayout;
    auto *save = makeButton(QStringLiteral("Save"), QStringLiteral("primaryButton"));
    auto *restart = makeButton(QStringLiteral("Restart interface"));
    actions->addWidget(save);
    actions->addWidget(restart);
    actions->addStretch();
    editor->addLayout(actions);
    editor->addWidget(mutedLabel(QStringLiteral("Saved changes wait in the bar above until you apply them.")));
    editor->addStretch();
    m_interfaceEditor->hide();
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_interfaceEditor);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    const auto updateRows = [this, form] {
        const QString proto = m_proto->currentData().toString();
        for (QWidget *widget : m_staticRows)
            form->setRowVisible(widget, proto == QStringLiteral("static"));
        for (QWidget *widget : m_pppoeRows)
            form->setRowVisible(widget, proto == QStringLiteral("pppoe"));
        for (QWidget *widget : m_dhcpRows)
            form->setRowVisible(widget, proto == QStringLiteral("static"));
        form->setRowVisible(m_dns, proto != QStringLiteral("none"));
    };
    connect(m_proto, &QComboBox::currentIndexChanged, this, updateRows);
    connect(m_interfaceTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            showInterface(itemKey(item));
    });
    connect(save, &QPushButton::clicked, this, [this] { saveInterface(); });
    connect(restart, &QPushButton::clicked, this, [this] { restartInterface(); });
    updateRows();
    return splitter;
}

QWidget *OpenWrtFeature::buildWireless()
{
    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    m_wifiTree = makeTree(QStringLiteral("openWrtWifi"), {QStringLiteral("Name"), QStringLiteral("Details"), QStringLiteral("State")},
                          QStringLiteral("Router radios and Wi-Fi networks"));
    m_wifiTree->setRootIsDecorated(true);
    splitter->addWidget(m_wifiTree);

    m_wifiEditors = new QStackedWidget;
    auto *empty = mutedLabel(QStringLiteral("Select a radio or a Wi-Fi network."));
    empty->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_wifiEditors->addWidget(empty);

    auto *radio = new QWidget;
    auto *radioLayout = new QVBoxLayout(radio);
    radioLayout->setContentsMargins(12, 4, 0, 0);
    m_radioTitle = sectionTitle({});
    radioLayout->addWidget(m_radioTitle);
    auto *radioForm = new QFormLayout;
    radioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_radioEnabled = new QCheckBox(QStringLiteral("Radio on"));
    m_channel = makeField(QStringLiteral("auto"));
    m_country = makeField(QStringLiteral("Two letters, for example US or DE"));
    m_country->setMaxLength(2);
    m_txpower = makeField(QStringLiteral("Driver default"));
    radioForm->addRow(QString(), m_radioEnabled);
    radioForm->addRow(QStringLiteral("Channel"), m_channel);
    radioForm->addRow(QStringLiteral("Country"), m_country);
    radioForm->addRow(QStringLiteral("Transmit power (dBm)"), m_txpower);
    radioLayout->addLayout(radioForm);
    auto *radioActions = new QHBoxLayout;
    auto *saveRadio = makeButton(QStringLiteral("Save"), QStringLiteral("primaryButton"));
    auto *addNetwork = makeButton(QStringLiteral("Add Wi-Fi network"));
    radioActions->addWidget(saveRadio);
    radioActions->addWidget(addNetwork);
    radioActions->addStretch();
    radioLayout->addLayout(radioActions);
    radioLayout->addWidget(mutedLabel(QStringLiteral("Use the country you are in: it sets which channels and power levels are legal.")));
    radioLayout->addStretch();
    m_wifiEditors->addWidget(radio);

    auto *network = new QWidget;
    auto *networkLayout = new QVBoxLayout(network);
    networkLayout->setContentsMargins(12, 4, 0, 0);
    m_networkTitle = sectionTitle({});
    networkLayout->addWidget(m_networkTitle);
    auto *networkForm = new QFormLayout;
    networkForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_wifiEnabled = new QCheckBox(QStringLiteral("Network on"));
    m_ssid = makeField();
    m_encryption = new QComboBox;
    fillChoices(m_encryption, encryptionChoices());
    m_wifiKey = new QLineEdit;
    m_wifiKey->setEchoMode(QLineEdit::Password);
    auto *showKey = new QCheckBox(QStringLiteral("Show password"));
    m_hidden = new QCheckBox(QStringLiteral("Hide the network name"));
    m_isolate = new QCheckBox(QStringLiteral("Keep devices on this network apart (guest isolation)"));
    m_wifiNetwork = new QComboBox;
    networkForm->addRow(QString(), m_wifiEnabled);
    networkForm->addRow(QStringLiteral("Name (SSID)"), m_ssid);
    networkForm->addRow(QStringLiteral("Security"), m_encryption);
    networkForm->addRow(QStringLiteral("Password"), m_wifiKey);
    networkForm->addRow(QString(), showKey);
    networkForm->addRow(QStringLiteral("Connects to"), m_wifiNetwork);
    networkForm->addRow(QString(), m_hidden);
    networkForm->addRow(QString(), m_isolate);
    networkLayout->addLayout(networkForm);
    auto *networkActions = new QHBoxLayout;
    auto *saveNetwork = makeButton(QStringLiteral("Save"), QStringLiteral("primaryButton"));
    auto *deleteNetwork = makeButton(QStringLiteral("Delete network"), QStringLiteral("dangerButton"));
    networkActions->addWidget(saveNetwork);
    networkActions->addWidget(deleteNetwork);
    networkActions->addStretch();
    networkLayout->addLayout(networkActions);
    networkLayout->addStretch();
    m_wifiEditors->addWidget(network);

    splitter->addWidget(m_wifiEditors);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    connect(showKey, &QCheckBox::toggled, this, [this](bool on) {
        m_wifiKey->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(m_encryption, &QComboBox::currentIndexChanged, this, [this, networkForm] {
        const QString encryption = m_encryption->currentData().toString();
        networkForm->setRowVisible(m_wifiKey, encryption != QStringLiteral("none") && encryption != QStringLiteral("owe"));
    });
    connect(m_wifiTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) { showWireless(item); });
    connect(saveRadio, &QPushButton::clicked, this, [this] { this->saveRadio(); });
    connect(addNetwork, &QPushButton::clicked, this, [this] { addWifiNetwork(); });
    connect(saveNetwork, &QPushButton::clicked, this, [this] { saveWifiNetwork(); });
    connect(deleteNetwork, &QPushButton::clicked, this, [this] { deleteWifiNetwork(); });
    return splitter;
}

QWidget *OpenWrtFeature::buildClients()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    m_clientTree = makeTree(QStringLiteral("openWrtClients"),
                            {QStringLiteral("Name"), QStringLiteral("IP address"), QStringLiteral("MAC address"), QStringLiteral("Lease"), QStringLiteral("Reserved")},
                            QStringLiteral("Devices on the router's network"));
    layout->addWidget(m_clientTree, 1);
    auto *actions = new QHBoxLayout;
    auto *reserve = makeButton(QStringLiteral("Reserve this address"));
    auto *remove = makeButton(QStringLiteral("Remove reservation"));
    actions->addWidget(reserve);
    actions->addWidget(remove);
    actions->addStretch();
    layout->addLayout(actions);
    layout->addWidget(sectionTitle(QStringLiteral("Reserve an address")));
    auto *row = new QHBoxLayout;
    m_reserveName = makeField(QStringLiteral("Name"));
    m_reserveMac = makeField(QStringLiteral("MAC address"));
    m_reserveIp = makeField(QStringLiteral("IP address"));
    auto *add = makeButton(QStringLiteral("Add reservation"));
    row->addWidget(m_reserveName);
    row->addWidget(m_reserveMac);
    row->addWidget(m_reserveIp);
    row->addWidget(add);
    layout->addLayout(row);
    layout->addWidget(mutedLabel(QStringLiteral("A reserved device always gets the same address from the router.")));
    connect(reserve, &QPushButton::clicked, this, [this] { reserveClient(); });
    connect(remove, &QPushButton::clicked, this, [this] { removeReservation(); });
    connect(add, &QPushButton::clicked, this, [this] { addReservation(); });
    return page;
}

QWidget *OpenWrtFeature::buildForwards()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    auto *top = new QHBoxLayout;
    top->addWidget(mutedLabel(QStringLiteral("A port forward lets a device on the internet reach a device on your network through the router.")), 1);
    auto *create = makeButton(QStringLiteral("New port forward"), QStringLiteral("primaryButton"));
    top->addWidget(create);
    layout->addLayout(top);
    m_forwardTree = makeTree(QStringLiteral("openWrtForwards"),
                             {QStringLiteral("Name"), QStringLiteral("Protocol"), QStringLiteral("External port"), QStringLiteral("To address"), QStringLiteral("To port"), QStringLiteral("State")},
                             QStringLiteral("Router port forwards"));
    layout->addWidget(m_forwardTree, 1);
    m_forwardTitle = sectionTitle(QStringLiteral("New port forward"));
    layout->addWidget(m_forwardTitle);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_forwardName = makeField(QStringLiteral("For example Game server"));
    m_forwardProto = new QComboBox;
    fillChoices(m_forwardProto, forwardProtocolChoices());
    m_forwardExternal = makeField(QStringLiteral("For example 8080 or 6000-6010"));
    m_forwardIp = makeField(QStringLiteral("192.168.1.20"));
    m_forwardInternal = makeField(QStringLiteral("Same as the external port"));
    m_forwardEnabled = new QCheckBox(QStringLiteral("On"));
    m_forwardEnabled->setChecked(true);
    form->addRow(QStringLiteral("Name"), m_forwardName);
    form->addRow(QStringLiteral("Protocol"), m_forwardProto);
    form->addRow(QStringLiteral("External port"), m_forwardExternal);
    form->addRow(QStringLiteral("To device address"), m_forwardIp);
    form->addRow(QStringLiteral("To port"), m_forwardInternal);
    form->addRow(QString(), m_forwardEnabled);
    layout->addLayout(form);
    auto *actions = new QHBoxLayout;
    auto *save = makeButton(QStringLiteral("Save"));
    m_forwardDelete = makeButton(QStringLiteral("Delete"), QStringLiteral("dangerButton"));
    actions->addWidget(save);
    actions->addWidget(m_forwardDelete);
    actions->addStretch();
    layout->addLayout(actions);
    connect(create, &QPushButton::clicked, this, [this] {
        m_forwardTree->clearSelection();
        showForward({});
        m_forwardName->setFocus();
    });
    connect(m_forwardTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            showForward(itemKey(item));
    });
    connect(save, &QPushButton::clicked, this, [this] { saveForward(); });
    connect(m_forwardDelete, &QPushButton::clicked, this, [this] { deleteForward(); });
    showForward({});
    return page;
}

QWidget *OpenWrtFeature::buildServices()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    m_serviceTree = makeTree(QStringLiteral("openWrtServices"), {QStringLiteral("Service"), QStringLiteral("Starts at boot"), QStringLiteral("Running")},
                             QStringLiteral("Router services"));
    layout->addWidget(m_serviceTree, 1);
    auto *actions = new QHBoxLayout;
    const QList<QPair<QString, QString>> buttons = {{QStringLiteral("start"), QStringLiteral("Start")},
                                                    {QStringLiteral("stop"), QStringLiteral("Stop")},
                                                    {QStringLiteral("restart"), QStringLiteral("Restart")},
                                                    {QStringLiteral("enable"), QStringLiteral("Start at boot")},
                                                    {QStringLiteral("disable"), QStringLiteral("Don't start at boot")}};
    for (const auto &entry : buttons) {
        auto *button = makeButton(entry.second);
        const QString action = entry.first;
        connect(button, &QPushButton::clicked, this, [this, action] { serviceAction(action); });
        actions->addWidget(button);
    }
    actions->addStretch();
    layout->addLayout(actions);
    return page;
}

QWidget *OpenWrtFeature::buildPackages()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    m_packageFilter = makeField(QStringLiteral("Filter installed packages"));
    m_packageFilter->setAccessibleName(QStringLiteral("Filter installed packages"));
    layout->addWidget(m_packageFilter);
    m_packageTree = makeTree(QStringLiteral("openWrtPackages"), {QStringLiteral("Package"), QStringLiteral("Version")},
                             QStringLiteral("Packages installed on the router"));
    layout->addWidget(m_packageTree, 2);
    auto *actions = new QHBoxLayout;
    m_packageName = makeField(QStringLiteral("Package to install, for example luci-app-sqm"));
    m_packageName->setAccessibleName(QStringLiteral("Package to install"));
    auto *install = makeButton(QStringLiteral("Install"));
    auto *remove = makeButton(QStringLiteral("Remove selected"), QStringLiteral("dangerButton"));
    auto *update = makeButton(QStringLiteral("Update package lists"));
    actions->addWidget(m_packageName, 1);
    actions->addWidget(install);
    actions->addWidget(remove);
    actions->addWidget(update);
    layout->addLayout(actions);
    m_packageOutput = new QPlainTextEdit;
    m_packageOutput->setReadOnly(true);
    m_packageOutput->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_packageOutput->setPlaceholderText(QStringLiteral("Output from the router's package manager appears here."));
    layout->addWidget(m_packageOutput, 1);
    connect(m_packageFilter, &QLineEdit::textChanged, this, [this] { filterTrees(); });
    connect(install, &QPushButton::clicked, this, [this] { packageAction(QStringLiteral("install"), m_packageName->text().trimmed()); });
    connect(m_packageName, &QLineEdit::returnPressed, this, [this] { packageAction(QStringLiteral("install"), m_packageName->text().trimmed()); });
    connect(remove, &QPushButton::clicked, this, [this] { packageAction(QStringLiteral("remove"), itemKey(m_packageTree->currentItem())); });
    connect(update, &QPushButton::clicked, this, [this] { packageAction(QStringLiteral("update"), {}); });
    return page;
}

QWidget *OpenWrtFeature::buildSystem()
{
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(sectionTitle(QStringLiteral("Hostname")));
    auto *hostRow = new QHBoxLayout;
    m_hostname = makeField(QStringLiteral("OpenWrt"));
    m_hostname->setAccessibleName(QStringLiteral("Router hostname"));
    auto *saveHost = makeButton(QStringLiteral("Save"));
    hostRow->addWidget(m_hostname, 1);
    hostRow->addWidget(saveHost);
    layout->addLayout(hostRow);

    layout->addWidget(sectionTitle(QStringLiteral("Router password")));
    auto *passwordForm = new QFormLayout;
    m_newPassword = new QLineEdit;
    m_newPassword->setEchoMode(QLineEdit::Password);
    m_confirmPassword = new QLineEdit;
    m_confirmPassword->setEchoMode(QLineEdit::Password);
    passwordForm->addRow(QStringLiteral("New password"), m_newPassword);
    passwordForm->addRow(QStringLiteral("Again"), m_confirmPassword);
    layout->addLayout(passwordForm);
    auto *passwordRow = new QHBoxLayout;
    auto *changePassword = makeButton(QStringLiteral("Change password"));
    passwordRow->addWidget(changePassword);
    passwordRow->addStretch();
    layout->addLayout(passwordRow);
    layout->addWidget(mutedLabel(QStringLiteral("This is the password for the router's own account (the one you signed in with); it also protects SSH and LuCI. It takes effect at once.")));

    layout->addWidget(sectionTitle(QStringLiteral("Backup")));
    auto *backupRow = new QHBoxLayout;
    auto *backup = makeButton(QStringLiteral("Save backup…"));
    auto *restore = makeButton(QStringLiteral("Restore backup…"));
    backupRow->addWidget(backup);
    backupRow->addWidget(restore);
    backupRow->addStretch();
    layout->addLayout(backupRow);
    layout->addWidget(mutedLabel(QStringLiteral("A backup holds every setting, including Wi-Fi and account passwords: keep the file somewhere private.")));

    layout->addWidget(sectionTitle(QStringLiteral("Firmware")));
    auto *firmwareRow = new QHBoxLayout;
    auto *firmware = makeButton(QStringLiteral("Upgrade firmware…"));
    firmwareRow->addWidget(firmware);
    firmwareRow->addStretch();
    layout->addLayout(firmwareRow);
    layout->addWidget(mutedLabel(QStringLiteral("Use a sysupgrade image for exactly this router model from firmware-selector.openwrt.org. The router checks the image before anything is written.")));

    layout->addWidget(sectionTitle(QStringLiteral("Restart and reset")));
    auto *resetRow = new QHBoxLayout;
    auto *reboot = makeButton(QStringLiteral("Restart router"));
    auto *reset = makeButton(QStringLiteral("Reset to factory defaults…"), QStringLiteral("dangerButton"));
    resetRow->addWidget(reboot);
    resetRow->addWidget(reset);
    resetRow->addStretch();
    layout->addLayout(resetRow);
    layout->addStretch();

    connect(saveHost, &QPushButton::clicked, this, [this] { saveHostname(); });
    connect(changePassword, &QPushButton::clicked, this, [this] { this->changePassword(); });
    connect(backup, &QPushButton::clicked, this, [this] { saveBackup(); });
    connect(restore, &QPushButton::clicked, this, [this] { restoreBackup(); });
    connect(firmware, &QPushButton::clicked, this, [this] { upgradeFirmware(); });
    connect(reboot, &QPushButton::clicked, this, [this] { this->reboot(); });
    connect(reset, &QPushButton::clicked, this, [this] { factoryReset(); });

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

QWidget *OpenWrtFeature::buildAdvanced()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("Configuration")));
    m_advancedPicker = new QComboBox;
    m_advancedPicker->setAccessibleName(QStringLiteral("Router configuration file"));
    m_advancedPicker->setMinimumContentsLength(16);
    top->addWidget(m_advancedPicker);
    top->addWidget(mutedLabel(QStringLiteral("Every UCI setting on the router. Edits wait in the bar above until you apply them.")), 1);
    layout->addLayout(top);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    m_advancedTree = makeTree(QStringLiteral("openWrtUci"), {QStringLiteral("Section / option"), QStringLiteral("Value")},
                              QStringLiteral("Router configuration sections and options"));
    m_advancedTree->setRootIsDecorated(true);
    m_advancedTree->setUniformRowHeights(false);
    splitter->addWidget(m_advancedTree);

    auto *editor = new QWidget;
    auto *editorLayout = new QVBoxLayout(editor);
    editorLayout->setContentsMargins(12, 0, 0, 0);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_advancedSection = new QLineEdit;
    m_advancedSection->setReadOnly(true);
    m_advancedOption = makeField(QStringLiteral("option_name"));
    m_advancedValue = new QPlainTextEdit;
    m_advancedValue->setPlaceholderText(QStringLiteral("One value per line makes a list"));
    m_advancedValue->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_advancedValue->setMaximumHeight(120);
    form->addRow(QStringLiteral("Section"), m_advancedSection);
    form->addRow(QStringLiteral("Option"), m_advancedOption);
    form->addRow(QStringLiteral("Value"), m_advancedValue);
    editorLayout->addLayout(form);
    auto *optionActions = new QHBoxLayout;
    auto *setOption = makeButton(QStringLiteral("Set option"));
    auto *deleteOption = makeButton(QStringLiteral("Delete option"));
    auto *deleteSection = makeButton(QStringLiteral("Delete section"), QStringLiteral("dangerButton"));
    optionActions->addWidget(setOption);
    optionActions->addWidget(deleteOption);
    optionActions->addWidget(deleteSection);
    optionActions->addStretch();
    editorLayout->addLayout(optionActions);
    editorLayout->addWidget(sectionTitle(QStringLiteral("Add a section")));
    auto *newForm = new QFormLayout;
    m_newSectionType = makeField(QStringLiteral("Type, for example rule"));
    m_newSectionName = makeField(QStringLiteral("Optional name"));
    newForm->addRow(QStringLiteral("Type"), m_newSectionType);
    newForm->addRow(QStringLiteral("Name"), m_newSectionName);
    editorLayout->addLayout(newForm);
    auto *newRow = new QHBoxLayout;
    auto *addSection = makeButton(QStringLiteral("Add section"));
    newRow->addWidget(addSection);
    newRow->addStretch();
    editorLayout->addLayout(newRow);
    editorLayout->addStretch();
    splitter->addWidget(editor);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    connect(m_advancedPicker, &QComboBox::currentIndexChanged, this, [this] {
        const QString config = m_advancedPicker->currentText();
        if (config.isEmpty() || config == m_advancedConfig)
            return;
        m_advancedConfig = config;
        if (m_configs.contains(config))
            renderAdvanced();
        else
            loadConfig(config);
    });
    connect(m_advancedTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) { showAdvancedItem(item); });
    connect(setOption, &QPushButton::clicked, this, [this] { setAdvancedOption(); });
    connect(deleteOption, &QPushButton::clicked, this, [this] { deleteAdvancedOption(); });
    connect(deleteSection, &QPushButton::clicked, this, [this] { deleteAdvancedSection(); });
    connect(addSection, &QPushButton::clicked, this, [this] { addAdvancedSection(); });
    return page;
}

QString OpenWrtFeature::settingsKey(const QString &name) const
{
    const QString id = m_host.packageId.isEmpty() ? QStringLiteral("openwrt") : m_host.packageId;
    return QStringLiteral("market/packages/%1/data/%2").arg(id, name);
}

void OpenWrtFeature::detectGateway()
{
    const auto use = [this](const QString &gateway) {
        if (!gateway.isEmpty() && m_address->text().isEmpty() && isLocalHostName(gateway))
            m_address->setText(gateway);
    };
#if defined(Q_OS_LINUX)
    QFile routes(QStringLiteral("/proc/net/route"));
    if (routes.open(QIODevice::ReadOnly | QIODevice::Text))
        use(defaultGatewayFromProcRoute(QString::fromLatin1(routes.readAll())));
#elif defined(Q_OS_MACOS)
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [process, use] {
        process->deleteLater();
        use(defaultGatewayFromRouteGet(QString::fromLocal8Bit(process->readAllStandardOutput())));
    });
    connect(process, &QProcess::errorOccurred, process, &QObject::deleteLater);
    process->start(QStringLiteral("/sbin/route"), {QStringLiteral("-n"), QStringLiteral("get"), QStringLiteral("default")});
#else
    Q_UNUSED(use);
#endif
}

void OpenWrtFeature::showStatus(const QString &text, bool error)
{
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("statusLine"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

bool OpenWrtFeature::confirm(const QString &title, const QString &text, const QString &action, bool danger)
{
    QMessageBox box(this);
    box.setIcon(danger ? QMessageBox::Warning : QMessageBox::Question);
    box.setWindowTitle(title);
    box.setText(title);
    box.setInformativeText(text);
    QPushButton *accept = box.addButton(action, QMessageBox::AcceptRole);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();
    return box.clickedButton() == accept;
}

void OpenWrtFeature::report(const RpcResult &result, const QString &success)
{
    if (result.ok)
        showStatus(success);
    else
        showStatus(result.error, true);
}

void OpenWrtFeature::connectToRouter()
{
    const Address address = parseAddress(m_address->text());
    if (!address.valid()) {
        showStatus(address.error, true);
        return;
    }
    const QString user = m_user->text().trimmed().isEmpty() ? QStringLiteral("root") : m_user->text().trimmed();
    const QString password = m_password->text();
    m_connectButton->setEnabled(false);
    m_connectButton->setText(QStringLiteral("Connecting…"));
    m_certificateLabel->hide();
    m_trustButton->hide();
    showStatus(QStringLiteral("Connecting to %1…").arg(address.base.host()));

    const auto proceed = [this, address, user, password] {
        m_client.setBase(address.base);
        const QString certificateKey = settingsKey(QStringLiteral("certificates/") + QString(address.base.authority()).replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9.-]")), QStringLiteral("_")));
        m_client.setTrustedCertificate(QSettings().value(certificateKey).toString());
        m_client.signIn(user, password, [this, address, user, certificateKey](const RpcResult &result) {
            m_connectButton->setEnabled(true);
            m_connectButton->setText(QStringLiteral("Connect"));
            if (!result.ok) {
                if (!result.certificate.isEmpty()) {
                    m_pendingCertificate = result.certificate;
                    m_certificateLabel->setProperty("settingsKey", certificateKey);
                    m_certificateLabel->setText(QStringLiteral("Certificate fingerprint (SHA-256):\n%1").arg(result.certificate));
                    m_certificateLabel->show();
                    m_trustButton->show();
                }
                showStatus(result.error, true);
                return;
            }
            QSettings settings;
            settings.setValue(settingsKey(QStringLiteral("address")), m_address->text().trimmed());
            settings.setValue(settingsKey(QStringLiteral("user")), user);
            m_password->clear();
            m_routerLabel->setText(QStringLiteral("%1 · %2").arg(address.base.host(), user));
            m_refreshButton->setVisible(true);
            m_signOutButton->setVisible(true);
            m_pages->setCurrentIndex(1);
            showStatus(QStringLiteral("Connected to %1.").arg(address.base.host()));
            loadAll();
        });
    };

    const QString host = address.base.host();
    if (isLocalHostName(host)) {
        proceed();
        return;
    }
    QHostInfo::lookupHost(host, this, [this, host, proceed](const QHostInfo &info) {
        const QList<QHostAddress> addresses = info.addresses();
        const bool local = !addresses.isEmpty() && std::all_of(addresses.begin(), addresses.end(), [](const QHostAddress &item) {
            return isLocalHostName(item.toString());
        });
        if (!local) {
            m_connectButton->setEnabled(true);
            m_connectButton->setText(QStringLiteral("Connect"));
            showStatus(addresses.isEmpty() ? QStringLiteral("%1 could not be found on your network.").arg(host)
                                           : QStringLiteral("%1 is not on your local network. Only routers on your own network can be managed, so the router password never crosses the internet.").arg(host),
                       true);
            return;
        }
        proceed();
    });
}

void OpenWrtFeature::trustCertificate()
{
    const QString key = m_certificateLabel->property("settingsKey").toString();
    if (m_pendingCertificate.isEmpty() || key.isEmpty())
        return;
    QSettings().setValue(key, m_pendingCertificate);
    m_pendingCertificate.clear();
    connectToRouter();
}

void OpenWrtFeature::signOut()
{
    m_client.signOut();
    m_board = {};
    m_info = {};
    m_interfaces.clear();
    m_wirelessStatus = {};
    m_configs.clear();
    m_changes = {};
    m_leases.clear();
    m_services.clear();
    m_packages.clear();
    m_packageTool = -1;
    m_advancedConfigs.clear();
    m_advancedConfig.clear();
    m_routerLabel->clear();
    m_refreshButton->setVisible(false);
    m_signOutButton->setVisible(false);
    m_pages->setCurrentIndex(0);
    renderChanges();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

void OpenWrtFeature::refresh()
{
    if (m_client.signedIn())
        loadAll();
}

void OpenWrtFeature::loadAll()
{
    loadOverview();
    for (const QString &config : {QStringLiteral("network"), QStringLiteral("wireless"), QStringLiteral("dhcp"), QStringLiteral("firewall"), QStringLiteral("system")})
        loadConfig(config);
    loadChanges();
    loadLeases();
    loadServices();
    loadPackages();
    loadAdvancedConfigs();
}

void OpenWrtFeature::loadOverview()
{
    m_client.call(QStringLiteral("system"), QStringLiteral("board"), {}, [this](const RpcResult &result) {
        if (!result.ok) {
            showStatus(result.error, true);
            return;
        }
        m_board = result.data;
        renderOverview();
    });
    m_client.call(QStringLiteral("system"), QStringLiteral("info"), {}, [this](const RpcResult &result) {
        if (result.ok)
            m_info = result.data;
        renderOverview();
    });
    m_client.call(QStringLiteral("network.interface"), QStringLiteral("dump"), {}, [this](const RpcResult &result) {
        if (result.ok)
            m_interfaces = parseInterfaces(result.data);
        renderOverview();
        renderNetwork();
    });
    m_client.call(QStringLiteral("network.wireless"), QStringLiteral("status"), {}, [this](const RpcResult &result) {
        m_wirelessStatus = result.ok ? result.data : QJsonObject();
        renderWireless();
    });
}

void OpenWrtFeature::loadConfig(const QString &config)
{
    m_client.call(QStringLiteral("uci"), QStringLiteral("get"), {{QStringLiteral("config"), config}}, [this, config](const RpcResult &result) {
        if (!result.ok && result.status != UbusNotFound) {
            showStatus(QStringLiteral("Could not read the %1 settings: %2").arg(config, result.error), true);
            return;
        }
        m_configs.insert(config, result.data.value(QStringLiteral("values")).toObject());
        if (config == QStringLiteral("network") || config == QStringLiteral("dhcp")) {
            renderNetwork();
            renderWireless();
            renderClients();
        } else if (config == QStringLiteral("wireless")) {
            renderWireless();
        } else if (config == QStringLiteral("firewall")) {
            renderForwards();
        } else if (config == QStringLiteral("system")) {
            renderOverview();
        }
        if (config == m_advancedConfig)
            renderAdvanced();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
}

void OpenWrtFeature::loadChanges()
{
    m_client.call(QStringLiteral("uci"), QStringLiteral("changes"), {}, [this](const RpcResult &result) {
        m_changes = result.ok ? result.data.value(QStringLiteral("changes")).toObject() : QJsonObject();
        renderChanges();
    });
}

void OpenWrtFeature::loadLeases()
{
    m_client.call(QStringLiteral("luci-rpc"), QStringLiteral("getDHCPLeases"), {}, [this](const RpcResult &result) {
        if (result.ok) {
            const qint64 now = QDateTime::currentSecsSinceEpoch();
            m_leases.clear();
            for (const QJsonValue &value : result.data.value(QStringLiteral("dhcp_leases")).toArray()) {
                const QJsonObject raw = value.toObject();
                Lease lease;
                lease.mac = raw.value(QStringLiteral("macaddr")).toString().toLower();
                lease.ip = raw.value(QStringLiteral("ipaddr")).toString();
                lease.hostname = raw.value(QStringLiteral("hostname")).toString();
                const QJsonValue expires = raw.value(QStringLiteral("expires"));
                lease.expires = expires.isDouble() && expires.toInteger() > 0 ? now + expires.toInteger() : 0;
                if (!lease.mac.isEmpty())
                    m_leases << lease;
            }
            renderClients();
            return;
        }
        m_client.call(QStringLiteral("file"), QStringLiteral("read"), {{QStringLiteral("path"), QStringLiteral("/tmp/dhcp.leases")}}, [this](const RpcResult &file) {
            m_leases = file.ok ? parseLeases(file.data.value(QStringLiteral("data")).toString()) : QList<Lease>();
            renderClients();
        });
    });
}

void OpenWrtFeature::loadServices()
{
    m_client.call(QStringLiteral("rc"), QStringLiteral("list"), {}, [this](const RpcResult &result) {
        m_services = result.ok ? parseServices(result.data) : QList<Service>();
        renderServices();
        if (!result.ok)
            m_serviceTree->setAccessibleDescription(result.error);
    });
}

void OpenWrtFeature::loadPackages()
{
    if (m_packageTool < 0) {
        detectPackageTool(0);
        return;
    }
    const PackageTool tool = packageTools().at(m_packageTool);
    m_client.exec(tool.program, packageArguments(tool, QStringLiteral("list")), [this](const RpcResult &result) {
        if (result.ok)
            m_packages = parseInstalledPackages(result.data.value(QStringLiteral("stdout")).toString());
        else
            m_packageOutput->setPlainText(result.error);
        renderPackages();
    });
}

void OpenWrtFeature::detectPackageTool(int index)
{
    const QList<PackageTool> tools = packageTools();
    if (index >= tools.size()) {
        m_packageOutput->setPlainText(QStringLiteral("The router does not let ForkMesh run its package manager. Install luci-app-package-manager (or luci-app-opkg on older releases) on the router to allow it."));
        return;
    }
    const PackageTool tool = tools.at(index);
    m_client.exec(tool.program, packageArguments(tool, QStringLiteral("list")), [this, index](const RpcResult &result) {
        if (!result.ok || result.data.value(QStringLiteral("stdout")).toString().trimmed().isEmpty()) {
            detectPackageTool(index + 1);
            return;
        }
        m_packageTool = index;
        m_packages = parseInstalledPackages(result.data.value(QStringLiteral("stdout")).toString());
        renderPackages();
    });
}

void OpenWrtFeature::loadAdvancedConfigs()
{
    m_client.call(QStringLiteral("uci"), QStringLiteral("configs"), {}, [this](const RpcResult &result) {
        if (!result.ok)
            return;
        m_advancedConfigs.clear();
        for (const QJsonValue &value : result.data.value(QStringLiteral("configs")).toArray())
            m_advancedConfigs << value.toString();
        m_advancedConfigs.sort();
        if (m_advancedConfig.isEmpty() || !m_advancedConfigs.contains(m_advancedConfig))
            m_advancedConfig = m_advancedConfigs.contains(QStringLiteral("network")) ? QStringLiteral("network") : m_advancedConfigs.value(0);
        m_advancedPicker->blockSignals(true);
        m_advancedPicker->clear();
        m_advancedPicker->addItems(m_advancedConfigs);
        m_advancedPicker->setCurrentText(m_advancedConfig);
        m_advancedPicker->blockSignals(false);
        if (m_configs.contains(m_advancedConfig))
            renderAdvanced();
        else if (!m_advancedConfig.isEmpty())
            loadConfig(m_advancedConfig);
    });
}

void OpenWrtFeature::stage(const QString &config, const QString &section, const QJsonObject &values,
                           const QStringList &removed, const QString &what)
{
    QStringList existing;
    if (const std::optional<UciSection> current = this->section(config, section)) {
        for (const QString &option : removed) {
            if (current->options.contains(option))
                existing << option;
        }
    }
    const auto finish = [this, config, what](const RpcResult &result) {
        loadConfig(config);
        loadChanges();
        if (result.ok)
            showStatus(QStringLiteral("%1 saved. Apply to make it take effect.").arg(what));
        else
            showStatus(result.error, true);
    };
    const auto removeThen = [this, config, section, existing, finish](const RpcResult &previous) {
        if (!previous.ok || existing.isEmpty()) {
            finish(previous);
            return;
        }
        m_client.call(QStringLiteral("uci"), QStringLiteral("delete"),
                      {{QStringLiteral("config"), config}, {QStringLiteral("section"), section}, {QStringLiteral("options"), QJsonArray::fromStringList(existing)}},
                      finish);
    };
    if (values.isEmpty()) {
        RpcResult nothing;
        nothing.ok = true;
        removeThen(nothing);
        return;
    }
    m_client.call(QStringLiteral("uci"), QStringLiteral("set"),
                  {{QStringLiteral("config"), config}, {QStringLiteral("section"), section}, {QStringLiteral("values"), values}}, removeThen);
}

void OpenWrtFeature::addSection(const QString &config, const QString &type, const QString &name, const QJsonObject &values,
                                const QString &what)
{
    QJsonObject args{{QStringLiteral("config"), config}, {QStringLiteral("type"), type}, {QStringLiteral("values"), values}};
    if (!name.isEmpty())
        args.insert(QStringLiteral("name"), name);
    m_client.call(QStringLiteral("uci"), QStringLiteral("add"), args, [this, config, what](const RpcResult &result) {
        loadConfig(config);
        loadChanges();
        report(result, QStringLiteral("%1 added. Apply to make it take effect.").arg(what));
    });
}

void OpenWrtFeature::deleteSection(const QString &config, const QString &section, const QString &what)
{
    m_client.call(QStringLiteral("uci"), QStringLiteral("delete"), {{QStringLiteral("config"), config}, {QStringLiteral("section"), section}},
                  [this, config, what](const RpcResult &result) {
                      loadConfig(config);
                      loadChanges();
                      report(result, QStringLiteral("%1 deleted. Apply to make it take effect.").arg(what));
                  });
}

void OpenWrtFeature::reviewChanges()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Unsaved router changes"));
    box.setText(QStringLiteral("These changes are waiting on the router:"));
    box.setInformativeText(describeChanges(m_changes).join(QLatin1Char('\n')));
    box.exec();
}

void OpenWrtFeature::applyChanges()
{
    if (m_applying || pendingCount(m_changes) == 0)
        return;
    const bool rollback = m_rollback->isChecked();
    m_applying = true;
    m_applyButton->setEnabled(false);
    m_discardButton->setEnabled(false);
    showStatus(QStringLiteral("Applying changes…"));
    m_client.call(QStringLiteral("uci"), QStringLiteral("apply"), {{QStringLiteral("rollback"), rollback}, {QStringLiteral("timeout"), kApplyRollbackSeconds}},
                  [this, rollback](const RpcResult &result) {
                      if (!result.ok) {
                          m_applying = false;
                          m_applyButton->setEnabled(true);
                          m_discardButton->setEnabled(true);
                          showStatus(result.status == UbusNoData ? QStringLiteral("There was nothing to apply.") : result.error, result.status != UbusNoData);
                          loadChanges();
                          return;
                      }
                      if (!rollback) {
                          m_applying = false;
                          m_applyButton->setEnabled(true);
                          m_discardButton->setEnabled(true);
                          showStatus(QStringLiteral("Applied. The router is reloading the changed services."));
                          QTimer::singleShot(3000, this, [this] { loadAll(); });
                          return;
                      }
                      const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + (kApplyRollbackSeconds - 10) * 1000;
                      showStatus(QStringLiteral("Applied. Checking that the router still answers…"));
                      QTimer::singleShot(3000, this, [this, deadline] { confirmApply(deadline); });
                  });
}

void OpenWrtFeature::confirmApply(qint64 deadline)
{
    m_client.call(QStringLiteral("uci"), QStringLiteral("confirm"), {}, [this, deadline](const RpcResult &result) {
        if (!result.ok && (result.unreachable || result.accessDenied) && QDateTime::currentMSecsSinceEpoch() < deadline) {
            QTimer::singleShot(2000, this, [this, deadline] { confirmApply(deadline); });
            return;
        }
        m_applying = false;
        m_applyButton->setEnabled(true);
        m_discardButton->setEnabled(true);
        if (result.ok)
            showStatus(QStringLiteral("Applied and confirmed. The router kept the new settings."));
        else if (result.unreachable)
            showStatus(QStringLiteral("The router stopped answering after the change, so it will put the old settings back by itself within %1 seconds.").arg(kApplyRollbackSeconds), true);
        else
            showStatus(QStringLiteral("The change could not be confirmed (%1). The router will put the old settings back by itself.").arg(result.error), true);
        loadAll();
    }, 4000);
}

void OpenWrtFeature::discardChanges()
{
    const QStringList configs = m_changes.keys();
    if (configs.isEmpty())
        return;
    if (!confirm(QStringLiteral("Discard changes?"), QStringLiteral("The %1 changes you have not applied will be thrown away.").arg(pendingCount(m_changes)), QStringLiteral("Discard")))
        return;
    auto remaining = std::make_shared<int>(configs.size());
    for (const QString &config : configs) {
        m_client.call(QStringLiteral("uci"), QStringLiteral("revert"), {{QStringLiteral("config"), config}}, [this, config, remaining](const RpcResult &result) {
            if (!result.ok)
                showStatus(result.error, true);
            loadConfig(config);
            if (--*remaining == 0) {
                loadChanges();
                showStatus(QStringLiteral("Changes discarded."));
            }
        });
    }
}

void OpenWrtFeature::renderChanges()
{
    const int count = pendingCount(m_changes);
    m_changesBar->setVisible(count > 0 && m_client.signedIn());
    m_changesLabel->setText(count == 1 ? QStringLiteral("1 change is waiting. It takes effect when you apply it.")
                                       : QStringLiteral("%1 changes are waiting. They take effect when you apply them.").arg(count));
}

void OpenWrtFeature::renderOverview()
{
    const QJsonObject release = m_board.value(QStringLiteral("release")).toObject();
    const QJsonObject memory = m_info.value(QStringLiteral("memory")).toObject();
    const QJsonObject root = m_info.value(QStringLiteral("root")).toObject();
    QList<QPair<QString, QString>> rows;
    rows << qMakePair(QStringLiteral("Hostname"), hostname());
    rows << qMakePair(QStringLiteral("Model"), m_board.value(QStringLiteral("model")).toString());
    rows << qMakePair(QStringLiteral("Firmware"), release.value(QStringLiteral("description")).toString());
    rows << qMakePair(QStringLiteral("Kernel"), m_board.value(QStringLiteral("kernel")).toString());
    if (!m_info.isEmpty()) {
        rows << qMakePair(QStringLiteral("Uptime"), formatUptime(m_info.value(QStringLiteral("uptime")).toInteger()));
        rows << qMakePair(QStringLiteral("Load"), formatLoad(m_info.value(QStringLiteral("load")).toArray()));
        const qint64 total = memory.value(QStringLiteral("total")).toInteger();
        const qint64 available = memory.contains(QStringLiteral("available")) ? memory.value(QStringLiteral("available")).toInteger()
                                                                               : memory.value(QStringLiteral("free")).toInteger() + memory.value(QStringLiteral("buffered")).toInteger();
        rows << qMakePair(QStringLiteral("Memory"), QStringLiteral("%1 free of %2").arg(formatBytes(available), formatBytes(total)));
        if (!root.isEmpty())
            rows << qMakePair(QStringLiteral("Storage"), QStringLiteral("%1 free of %2").arg(formatBytes(root.value(QStringLiteral("avail")).toInteger() * 1024),
                                                                                              formatBytes(root.value(QStringLiteral("total")).toInteger() * 1024)));
    }
    for (const InterfaceStatus &status : m_interfaces) {
        if (status.name == QStringLiteral("loopback"))
            continue;
        QString text = status.up ? QStringLiteral("Up %1").arg(formatUptime(status.uptime)) : QStringLiteral("Down");
        if (!status.ipv4.isEmpty())
            text += QStringLiteral(" · ") + status.ipv4.join(QStringLiteral(", "));
        if (!status.gateway.isEmpty())
            text += QStringLiteral(" · gateway ") + status.gateway;
        rows << qMakePair(QStringLiteral("Interface %1").arg(status.name), text);
    }
    QString html = QStringLiteral("<table cellspacing=\"0\" cellpadding=\"4\">");
    for (const auto &row : rows) {
        if (row.second.isEmpty())
            continue;
        html += QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>").arg(row.first.toHtmlEscaped(), row.second.toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
    m_overview->setText(html);
    if (m_hostname && !m_hostname->hasFocus())
        m_hostname->setText(hostname());
}

void OpenWrtFeature::renderNetwork()
{
    const QString keep = m_selectedInterface;
    m_interfaceTree->blockSignals(true);
    m_interfaceTree->clear();
    QTreeWidgetItem *selected = nullptr;
    for (const UciSection &section : sections(QStringLiteral("network"), QStringLiteral("interface"))) {
        if (section.name == QStringLiteral("loopback"))
            continue;
        const auto status = std::find_if(m_interfaces.begin(), m_interfaces.end(), [&section](const InterfaceStatus &item) { return item.name == section.name; });
        const bool known = status != m_interfaces.end();
        const QString state = !known ? QStringLiteral("Unknown") : status->up ? QStringLiteral("Up %1").arg(formatUptime(status->uptime)) : QStringLiteral("Down");
        const QString addresses = known ? (status->ipv4 + status->ipv6).join(QStringLiteral(", ")) : section.option(QStringLiteral("ipaddr"));
        auto *item = new QTreeWidgetItem(m_interfaceTree, {section.name, choiceLabel(protocolChoices(), section.option(QStringLiteral("proto"))), state, addresses,
                                                           known ? status->device : section.option(QStringLiteral("device"), section.option(QStringLiteral("ifname")))});
        item->setData(0, kKeyRole, section.name);
        if (section.name == keep)
            selected = item;
    }
    m_interfaceTree->blockSignals(false);
    if (selected) {
        m_interfaceTree->setCurrentItem(selected);
        showInterface(keep);
    }
    filterTrees();
}

void OpenWrtFeature::showInterface(const QString &name)
{
    m_selectedInterface = name;
    const std::optional<UciSection> iface = section(QStringLiteral("network"), name);
    m_interfaceEditor->setVisible(iface.has_value());
    if (!iface)
        return;
    m_interfaceTitle->setText(QStringLiteral("Interface %1").arg(name));
    setComboValue(m_proto, iface->option(QStringLiteral("proto"), QStringLiteral("none")));
    m_ipaddr->setText(iface->option(QStringLiteral("ipaddr")));
    m_netmask->setText(iface->option(QStringLiteral("netmask")));
    m_gateway->setText(iface->option(QStringLiteral("gateway")));
    m_dns->setText(iface->option(QStringLiteral("dns")));
    m_pppoeUser->setText(iface->option(QStringLiteral("username")));
    m_pppoePassword->clear();
    const std::optional<UciSection> dhcp = section(QStringLiteral("dhcp"), dhcpSectionFor(name));
    m_dhcpServer->setChecked(dhcp && !dhcp->flag(QStringLiteral("ignore")));
    m_dhcpStart->setValue(dhcp ? dhcp->option(QStringLiteral("start"), QStringLiteral("100")).toInt() : 100);
    m_dhcpLimit->setValue(dhcp ? dhcp->option(QStringLiteral("limit"), QStringLiteral("150")).toInt() : 150);
    m_leaseTime->setText(dhcp ? dhcp->option(QStringLiteral("leasetime"), QStringLiteral("12h")) : QStringLiteral("12h"));
}

void OpenWrtFeature::saveInterface()
{
    const QString name = m_selectedInterface;
    const std::optional<UciSection> iface = section(QStringLiteral("network"), name);
    if (!iface)
        return;
    const QString proto = m_proto->currentData().toString();
    QJsonObject values{{QStringLiteral("proto"), proto}};
    QStringList removed;
    if (proto == QStringLiteral("static")) {
        const QString ipaddr = m_ipaddr->text().trimmed();
        const bool cidr = ipaddr.contains(QLatin1Char('/'));
        if (!validAddressList(ipaddr, true)) {
            showStatus(QStringLiteral("Enter a valid IPv4 address, for example 192.168.1.1."), true);
            return;
        }
        if (!cidr && !validNetmask(m_netmask->text())) {
            showStatus(QStringLiteral("Enter a valid netmask, for example 255.255.255.0."), true);
            return;
        }
        if (!m_gateway->text().trimmed().isEmpty() && !validIpv4(m_gateway->text())) {
            showStatus(QStringLiteral("The gateway must be an IPv4 address."), true);
            return;
        }
        const bool list = iface->options.value(QStringLiteral("ipaddr")).isArray() || ipaddr.contains(QRegularExpression(QStringLiteral("[\\s,]")));
        values.insert(QStringLiteral("ipaddr"), optionValue(ipaddr, list));
        if (cidr)
            removed << QStringLiteral("netmask");
        else
            values.insert(QStringLiteral("netmask"), m_netmask->text().trimmed());
        if (m_gateway->text().trimmed().isEmpty())
            removed << QStringLiteral("gateway");
        else
            values.insert(QStringLiteral("gateway"), m_gateway->text().trimmed());
        const QString current = iface->option(QStringLiteral("ipaddr")).section(QLatin1Char('/'), 0, 0);
        if (!current.isEmpty() && current == m_client.base().host() && ipaddr.section(QLatin1Char('/'), 0, 0) != current
            && !confirm(QStringLiteral("Change the address you are connected through?"),
                        QStringLiteral("ForkMesh reaches the router at %1. After applying, connect again at the new address. Turn off \"Undo automatically\" before applying, "
                                       "or the router will put the old address back because ForkMesh can no longer confirm the change.").arg(current),
                        QStringLiteral("Save"), true))
            return;
    } else {
        removed << QStringLiteral("ipaddr") << QStringLiteral("netmask") << QStringLiteral("gateway");
    }
    if (proto == QStringLiteral("pppoe")) {
        if (m_pppoeUser->text().trimmed().isEmpty()) {
            showStatus(QStringLiteral("PPPoE needs the user name from your internet provider."), true);
            return;
        }
        values.insert(QStringLiteral("username"), m_pppoeUser->text().trimmed());
        if (!m_pppoePassword->text().isEmpty())
            values.insert(QStringLiteral("password"), m_pppoePassword->text());
    } else {
        removed << QStringLiteral("username") << QStringLiteral("password");
    }
    const QString dns = m_dns->text().trimmed();
    if (dns.isEmpty() || proto == QStringLiteral("none"))
        removed << QStringLiteral("dns");
    else if (!validDnsList(dns)) {
        showStatus(QStringLiteral("DNS servers must be IP addresses separated by spaces."), true);
        return;
    } else
        values.insert(QStringLiteral("dns"), optionValue(dns, true));
    stage(QStringLiteral("network"), name, values, removed, QStringLiteral("Interface %1").arg(name));

    if (proto != QStringLiteral("static"))
        return;
    const QString dhcpName = dhcpSectionFor(name);
    const QJsonObject pool{{QStringLiteral("start"), QString::number(m_dhcpStart->value())},
                           {QStringLiteral("limit"), QString::number(m_dhcpLimit->value())},
                           {QStringLiteral("leasetime"), m_leaseTime->text().trimmed().isEmpty() ? QStringLiteral("12h") : m_leaseTime->text().trimmed()}};
    if (!dhcpName.isEmpty()) {
        QJsonObject dhcpValues = m_dhcpServer->isChecked() ? pool : QJsonObject{{QStringLiteral("ignore"), QStringLiteral("1")}};
        stage(QStringLiteral("dhcp"), dhcpName, dhcpValues, m_dhcpServer->isChecked() ? QStringList{QStringLiteral("ignore")} : QStringList{},
              QStringLiteral("DHCP server for %1").arg(name));
    } else if (m_dhcpServer->isChecked()) {
        QJsonObject dhcpValues = pool;
        dhcpValues.insert(QStringLiteral("interface"), name);
        addSection(QStringLiteral("dhcp"), QStringLiteral("dhcp"), validUciName(name) ? name : QString(), dhcpValues, QStringLiteral("DHCP server for %1").arg(name));
    }
}

void OpenWrtFeature::restartInterface()
{
    const QString name = m_selectedInterface;
    if (name.isEmpty())
        return;
    if (!confirm(QStringLiteral("Restart %1?").arg(name),
                 QStringLiteral("The interface goes down and comes back up. Devices using it lose their connection for a few seconds, and if ForkMesh reaches the router through it, this page may stop answering briefly."),
                 QStringLiteral("Restart")))
        return;
    const QJsonObject args{{QStringLiteral("interface"), name}};
    m_client.call(QStringLiteral("network.interface"), QStringLiteral("down"), args, [this, args, name](const RpcResult &down) {
        if (!down.ok) {
            showStatus(down.error, true);
            return;
        }
        m_client.call(QStringLiteral("network.interface"), QStringLiteral("up"), args, [this, name](const RpcResult &up) {
            report(up, QStringLiteral("Interface %1 restarted.").arg(name));
            QTimer::singleShot(3000, this, [this] { loadOverview(); });
        });
    });
}

void OpenWrtFeature::renderWireless()
{
    const QString keep = m_wifiSection;
    m_wifiTree->blockSignals(true);
    m_wifiTree->clear();
    QTreeWidgetItem *selected = nullptr;
    const QList<UciSection> ifaces = sections(QStringLiteral("wireless"), QStringLiteral("wifi-iface"));
    for (const UciSection &radio : sections(QStringLiteral("wireless"), QStringLiteral("wifi-device"))) {
        const QJsonObject status = m_wirelessStatus.value(radio.name).toObject();
        const QString state = radio.flag(QStringLiteral("disabled")) ? QStringLiteral("Off")
                            : status.value(QStringLiteral("up")).toBool() ? QStringLiteral("On")
                                                                          : QStringLiteral("Not running");
        auto *radioItem = new QTreeWidgetItem(m_wifiTree, {radio.name, QStringLiteral("%1 · channel %2").arg(wifiBand(radio), radio.option(QStringLiteral("channel"), QStringLiteral("auto"))), state});
        radioItem->setData(0, kKeyRole, QStringLiteral("radio:") + radio.name);
        radioItem->setData(0, kSectionRole, radio.name);
        if (radio.name == keep)
            selected = radioItem;
        for (const UciSection &iface : ifaces) {
            if (iface.option(QStringLiteral("device")) != radio.name)
                continue;
            const QString details = QStringLiteral("%1 · %2").arg(choiceLabel(encryptionChoices(), iface.option(QStringLiteral("encryption"), QStringLiteral("none"))),
                                                                  iface.option(QStringLiteral("network")));
            auto *item = new QTreeWidgetItem(radioItem, {iface.option(QStringLiteral("ssid"), sectionLabel(iface)), details,
                                                         iface.flag(QStringLiteral("disabled")) ? QStringLiteral("Off") : QStringLiteral("On")});
            item->setData(0, kKeyRole, QStringLiteral("iface:") + iface.name);
            item->setData(0, kSectionRole, iface.name);
            if (iface.name == keep)
                selected = item;
        }
        radioItem->setExpanded(true);
    }
    m_wifiNetwork->clear();
    for (const UciSection &section : sections(QStringLiteral("network"), QStringLiteral("interface"))) {
        if (section.name != QStringLiteral("loopback"))
            m_wifiNetwork->addItem(section.name, section.name);
    }
    m_wifiTree->blockSignals(false);
    if (selected)
        m_wifiTree->setCurrentItem(selected);
    showWireless(selected);
    filterTrees();
}

void OpenWrtFeature::showWireless(QTreeWidgetItem *item)
{
    const QString key = itemKey(item);
    m_wifiSection = item ? item->data(0, kSectionRole).toString() : QString();
    const std::optional<UciSection> current = section(QStringLiteral("wireless"), m_wifiSection);
    if (!current) {
        m_wifiEditors->setCurrentIndex(0);
        return;
    }
    if (key.startsWith(QStringLiteral("radio:"))) {
        m_wifiEditors->setCurrentIndex(1);
        m_radioTitle->setText(QStringLiteral("%1 (%2)").arg(current->name, wifiBand(*current)));
        m_radioEnabled->setChecked(!current->flag(QStringLiteral("disabled")));
        m_channel->setText(current->option(QStringLiteral("channel"), QStringLiteral("auto")));
        m_country->setText(current->option(QStringLiteral("country")));
        m_txpower->setText(current->option(QStringLiteral("txpower")));
        return;
    }
    m_wifiEditors->setCurrentIndex(2);
    const QString mode = current->option(QStringLiteral("mode"), QStringLiteral("ap"));
    m_networkTitle->setText(mode == QStringLiteral("ap") ? QStringLiteral("Wi-Fi network on %1").arg(current->option(QStringLiteral("device")))
                                                         : QStringLiteral("Wi-Fi link on %1 (mode %2)").arg(current->option(QStringLiteral("device")), mode));
    m_wifiEnabled->setChecked(!current->flag(QStringLiteral("disabled")));
    m_ssid->setText(current->option(QStringLiteral("ssid")));
    setComboValue(m_encryption, current->option(QStringLiteral("encryption"), QStringLiteral("none")));
    m_wifiKey->setText(current->option(QStringLiteral("key")));
    m_hidden->setChecked(current->flag(QStringLiteral("hidden")));
    m_isolate->setChecked(current->flag(QStringLiteral("isolate")));
    setComboValue(m_wifiNetwork, current->option(QStringLiteral("network")).section(QLatin1Char(' '), 0, 0));
}

void OpenWrtFeature::saveRadio()
{
    const std::optional<UciSection> radio = section(QStringLiteral("wireless"), m_wifiSection);
    if (!radio)
        return;
    const QString channel = m_channel->text().trimmed().toLower();
    bool numeric = false;
    const int number = channel.toInt(&numeric);
    if (!channel.isEmpty() && channel != QStringLiteral("auto") && (!numeric || number <= 0 || number > 233)) {
        showStatus(QStringLiteral("The channel is \"auto\" or a channel number."), true);
        return;
    }
    const QString country = m_country->text().trimmed().toUpper();
    if (!country.isEmpty() && !QRegularExpression(QStringLiteral("^[A-Z0-9]{2}$")).match(country).hasMatch()) {
        showStatus(QStringLiteral("The country is a two-letter code such as US or DE."), true);
        return;
    }
    const QString txpower = m_txpower->text().trimmed();
    if (!txpower.isEmpty() && (txpower.toInt() <= 0 || txpower.toInt() > 36)) {
        showStatus(QStringLiteral("Transmit power is a number of dBm between 1 and 36, or empty for the driver default."), true);
        return;
    }
    QJsonObject values{{QStringLiteral("channel"), channel.isEmpty() ? QStringLiteral("auto") : channel}};
    QStringList removed;
    if (m_radioEnabled->isChecked())
        removed << QStringLiteral("disabled");
    else
        values.insert(QStringLiteral("disabled"), QStringLiteral("1"));
    if (country.isEmpty())
        removed << QStringLiteral("country");
    else
        values.insert(QStringLiteral("country"), country);
    if (txpower.isEmpty())
        removed << QStringLiteral("txpower");
    else
        values.insert(QStringLiteral("txpower"), txpower);
    stage(QStringLiteral("wireless"), radio->name, values, removed, QStringLiteral("Radio %1").arg(radio->name));
}

void OpenWrtFeature::saveWifiNetwork()
{
    const std::optional<UciSection> iface = section(QStringLiteral("wireless"), m_wifiSection);
    if (!iface)
        return;
    const QString ssid = m_ssid->text();
    const QString encryption = m_encryption->currentData().toString();
    const QString key = m_wifiKey->text();
    QString problem = ssidProblem(ssid);
    if (problem.isEmpty())
        problem = wifiKeyProblem(encryption, key);
    if (!problem.isEmpty()) {
        showStatus(problem, true);
        return;
    }
    QJsonObject values{{QStringLiteral("ssid"), ssid}, {QStringLiteral("encryption"), encryption}};
    QStringList removed;
    if (encryption == QStringLiteral("none") || encryption == QStringLiteral("owe"))
        removed << QStringLiteral("key");
    else
        values.insert(QStringLiteral("key"), key);
    if (m_wifiNetwork->currentIndex() >= 0)
        values.insert(QStringLiteral("network"), m_wifiNetwork->currentData().toString());
    if (m_wifiEnabled->isChecked())
        removed << QStringLiteral("disabled");
    else
        values.insert(QStringLiteral("disabled"), QStringLiteral("1"));
    if (m_hidden->isChecked())
        values.insert(QStringLiteral("hidden"), QStringLiteral("1"));
    else
        removed << QStringLiteral("hidden");
    if (m_isolate->isChecked())
        values.insert(QStringLiteral("isolate"), QStringLiteral("1"));
    else
        removed << QStringLiteral("isolate");
    stage(QStringLiteral("wireless"), iface->name, values, removed, QStringLiteral("Wi-Fi network %1").arg(ssid));
}

void OpenWrtFeature::addWifiNetwork()
{
    const std::optional<UciSection> radio = section(QStringLiteral("wireless"), m_wifiSection);
    if (!radio || radio->type != QStringLiteral("wifi-device"))
        return;
    bool ok = false;
    const QString ssid = QInputDialog::getText(this, QStringLiteral("Add Wi-Fi network"), QStringLiteral("Network name (SSID) on %1:").arg(radio->name),
                                               QLineEdit::Normal, {}, &ok);
    if (!ok)
        return;
    if (const QString problem = ssidProblem(ssid); !problem.isEmpty()) {
        showStatus(problem, true);
        return;
    }
    const QString key = QInputDialog::getText(this, QStringLiteral("Add Wi-Fi network"), QStringLiteral("Password for %1 (at least 8 characters, WPA2/WPA3):").arg(ssid),
                                              QLineEdit::Password, {}, &ok);
    if (!ok)
        return;
    if (const QString problem = wifiKeyProblem(QStringLiteral("sae-mixed"), key); !problem.isEmpty()) {
        showStatus(problem, true);
        return;
    }
    const QString network = m_wifiNetwork->findData(QStringLiteral("lan")) >= 0 ? QStringLiteral("lan") : m_wifiNetwork->itemData(0).toString();
    addSection(QStringLiteral("wireless"), QStringLiteral("wifi-iface"), {},
               {{QStringLiteral("device"), radio->name}, {QStringLiteral("mode"), QStringLiteral("ap")}, {QStringLiteral("network"), network},
                {QStringLiteral("ssid"), ssid}, {QStringLiteral("encryption"), QStringLiteral("sae-mixed")}, {QStringLiteral("key"), key}},
               QStringLiteral("Wi-Fi network %1").arg(ssid));
}

void OpenWrtFeature::deleteWifiNetwork()
{
    const std::optional<UciSection> iface = section(QStringLiteral("wireless"), m_wifiSection);
    if (!iface || iface->type != QStringLiteral("wifi-iface"))
        return;
    const QString ssid = iface->option(QStringLiteral("ssid"), iface->name);
    if (!confirm(QStringLiteral("Delete %1?").arg(ssid), QStringLiteral("Devices connected to this Wi-Fi network lose their connection once you apply."), QStringLiteral("Delete"), true))
        return;
    deleteSection(QStringLiteral("wireless"), iface->name, QStringLiteral("Wi-Fi network %1").arg(ssid));
}

void OpenWrtFeature::renderClients()
{
    const QString keep = itemKey(m_clientTree->currentItem());
    const QList<UciSection> hosts = sections(QStringLiteral("dhcp"), QStringLiteral("host"));
    const auto reservationFor = [&hosts](const QString &mac) -> const UciSection * {
        for (const UciSection &host : hosts) {
            if (host.option(QStringLiteral("mac")).split(QLatin1Char(' ')).contains(mac, Qt::CaseInsensitive))
                return &host;
        }
        return nullptr;
    };
    m_clientTree->clear();
    QStringList shown;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const Lease &lease : m_leases) {
        const UciSection *host = reservationFor(lease.mac);
        auto *item = new QTreeWidgetItem(m_clientTree, {lease.hostname.isEmpty() && host ? host->option(QStringLiteral("name")) : lease.hostname, lease.ip, lease.mac,
                                                        leaseExpiry(lease.expires, now), host ? host->option(QStringLiteral("ip"), QStringLiteral("Yes")) : QString()});
        item->setData(0, kKeyRole, lease.mac);
        item->setData(0, kSectionRole, host ? host->name : QString());
        if (host)
            shown << host->name;
    }
    for (const UciSection &host : hosts) {
        if (shown.contains(host.name))
            continue;
        const QString mac = host.option(QStringLiteral("mac")).section(QLatin1Char(' '), 0, 0).toLower();
        auto *item = new QTreeWidgetItem(m_clientTree, {host.option(QStringLiteral("name")), QString(), mac, QStringLiteral("Not connected"), host.option(QStringLiteral("ip"), QStringLiteral("Yes"))});
        item->setData(0, kKeyRole, mac.isEmpty() ? host.name : mac);
        item->setData(0, kSectionRole, host.name);
    }
    selectTreeItem(m_clientTree, keep);
    filterTrees();
}

void OpenWrtFeature::reserveClient()
{
    QTreeWidgetItem *item = m_clientTree->currentItem();
    if (!item)
        return;
    if (!item->data(0, kSectionRole).toString().isEmpty()) {
        showStatus(QStringLiteral("This device already has a reserved address."));
        return;
    }
    const QString mac = item->text(2);
    const QString ip = item->text(1);
    if (!validMac(mac) || !validIpv4(ip))
        return;
    addSection(QStringLiteral("dhcp"), QStringLiteral("host"), {},
               {{QStringLiteral("name"), sanitizedHostname(item->text(0), mac)}, {QStringLiteral("mac"), mac}, {QStringLiteral("ip"), ip}, {QStringLiteral("dns"), QStringLiteral("1")}},
               QStringLiteral("Reservation of %1").arg(ip));
}

void OpenWrtFeature::removeReservation()
{
    QTreeWidgetItem *item = m_clientTree->currentItem();
    const QString host = item ? item->data(0, kSectionRole).toString() : QString();
    if (host.isEmpty())
        return;
    deleteSection(QStringLiteral("dhcp"), host, QStringLiteral("Reservation for %1").arg(item->text(0).isEmpty() ? item->text(2) : item->text(0)));
}

void OpenWrtFeature::addReservation()
{
    const QString mac = m_reserveMac->text().trimmed().toLower();
    const QString ip = m_reserveIp->text().trimmed();
    const QString name = m_reserveName->text().trimmed();
    if (!validMac(mac)) {
        showStatus(QStringLiteral("Enter the device's MAC address, for example aa:bb:cc:dd:ee:ff."), true);
        return;
    }
    if (!validIpv4(ip)) {
        showStatus(QStringLiteral("Enter the IPv4 address to reserve."), true);
        return;
    }
    if (!name.isEmpty() && !hostnameProblem(name).isEmpty()) {
        showStatus(hostnameProblem(name), true);
        return;
    }
    addSection(QStringLiteral("dhcp"), QStringLiteral("host"), {},
               {{QStringLiteral("name"), sanitizedHostname(name, mac)}, {QStringLiteral("mac"), mac}, {QStringLiteral("ip"), ip}, {QStringLiteral("dns"), QStringLiteral("1")}},
               QStringLiteral("Reservation of %1").arg(ip));
    m_reserveName->clear();
    m_reserveMac->clear();
    m_reserveIp->clear();
}

void OpenWrtFeature::renderForwards()
{
    const QString keep = m_selectedForward;
    m_forwardTree->blockSignals(true);
    m_forwardTree->clear();
    for (const UciSection &redirect : sections(QStringLiteral("firewall"), QStringLiteral("redirect"))) {
        const QString target = redirect.option(QStringLiteral("target"), QStringLiteral("DNAT"));
        if (target != QStringLiteral("DNAT"))
            continue;
        const QString proto = redirect.option(QStringLiteral("proto"), QStringLiteral("tcp udp"));
        auto *item = new QTreeWidgetItem(m_forwardTree, {redirect.option(QStringLiteral("name"), sectionLabel(redirect)), choiceLabel(forwardProtocolChoices(), proto),
                                                         redirect.option(QStringLiteral("src_dport")), redirect.option(QStringLiteral("dest_ip")),
                                                         redirect.option(QStringLiteral("dest_port"), redirect.option(QStringLiteral("src_dport"))),
                                                         redirect.flag(QStringLiteral("enabled"), true) ? QStringLiteral("On") : QStringLiteral("Off")});
        item->setData(0, kKeyRole, redirect.name);
    }
    m_forwardTree->blockSignals(false);
    if (!keep.isEmpty() && section(QStringLiteral("firewall"), keep))
        selectTreeItem(m_forwardTree, keep);
    else
        showForward({});
    filterTrees();
}

void OpenWrtFeature::showForward(const QString &name)
{
    m_selectedForward = name;
    const std::optional<UciSection> redirect = section(QStringLiteral("firewall"), name);
    m_forwardDelete->setVisible(redirect.has_value());
    m_forwardTitle->setText(redirect ? QStringLiteral("Port forward %1").arg(redirect->option(QStringLiteral("name"), sectionLabel(*redirect))) : QStringLiteral("New port forward"));
    m_forwardName->setText(redirect ? redirect->option(QStringLiteral("name")) : QString());
    setComboValue(m_forwardProto, redirect ? redirect->option(QStringLiteral("proto"), QStringLiteral("tcp udp")) : QStringLiteral("tcp udp"));
    m_forwardExternal->setText(redirect ? redirect->option(QStringLiteral("src_dport")) : QString());
    m_forwardIp->setText(redirect ? redirect->option(QStringLiteral("dest_ip")) : QString());
    m_forwardInternal->setText(redirect ? redirect->option(QStringLiteral("dest_port")) : QString());
    m_forwardEnabled->setChecked(redirect ? redirect->flag(QStringLiteral("enabled"), true) : true);
}

void OpenWrtFeature::saveForward()
{
    const QString name = m_forwardName->text().trimmed();
    const QString external = m_forwardExternal->text().trimmed();
    const QString ip = m_forwardIp->text().trimmed();
    const QString internal = m_forwardInternal->text().trimmed();
    if (name.isEmpty()) {
        showStatus(QStringLiteral("Give the port forward a name."), true);
        return;
    }
    if (!validPort(external)) {
        showStatus(QStringLiteral("The external port is a number from 1 to 65535, or a range such as 6000-6010."), true);
        return;
    }
    if (!validIpv4(ip)) {
        showStatus(QStringLiteral("Enter the IPv4 address of the device on your network."), true);
        return;
    }
    if (!internal.isEmpty() && !validPort(internal)) {
        showStatus(QStringLiteral("The port on the device is a number from 1 to 65535, or empty to use the external port."), true);
        return;
    }
    QJsonObject values{{QStringLiteral("name"), name},
                       {QStringLiteral("proto"), optionValue(m_forwardProto->currentData().toString(), true)},
                       {QStringLiteral("src_dport"), external},
                       {QStringLiteral("dest_ip"), ip}};
    QStringList removed;
    if (internal.isEmpty())
        removed << QStringLiteral("dest_port");
    else
        values.insert(QStringLiteral("dest_port"), internal);
    if (m_forwardEnabled->isChecked())
        removed << QStringLiteral("enabled");
    else
        values.insert(QStringLiteral("enabled"), QStringLiteral("0"));
    if (section(QStringLiteral("firewall"), m_selectedForward)) {
        stage(QStringLiteral("firewall"), m_selectedForward, values, removed, QStringLiteral("Port forward %1").arg(name));
        return;
    }
    values.insert(QStringLiteral("target"), QStringLiteral("DNAT"));
    values.insert(QStringLiteral("src"), QStringLiteral("wan"));
    values.insert(QStringLiteral("dest"), QStringLiteral("lan"));
    if (!m_forwardEnabled->isChecked())
        values.insert(QStringLiteral("enabled"), QStringLiteral("0"));
    addSection(QStringLiteral("firewall"), QStringLiteral("redirect"), {}, values, QStringLiteral("Port forward %1").arg(name));
}

void OpenWrtFeature::deleteForward()
{
    const std::optional<UciSection> redirect = section(QStringLiteral("firewall"), m_selectedForward);
    if (!redirect)
        return;
    const QString name = redirect->option(QStringLiteral("name"), sectionLabel(*redirect));
    if (!confirm(QStringLiteral("Delete %1?").arg(name), QStringLiteral("The port stops reaching the device once you apply."), QStringLiteral("Delete"), true))
        return;
    deleteSection(QStringLiteral("firewall"), redirect->name, QStringLiteral("Port forward %1").arg(name));
    m_selectedForward.clear();
}

void OpenWrtFeature::renderServices()
{
    const QString keep = itemKey(m_serviceTree->currentItem());
    m_serviceTree->clear();
    for (const Service &service : m_services) {
        auto *item = new QTreeWidgetItem(m_serviceTree, {service.name, service.enabled ? QStringLiteral("Yes") : QStringLiteral("No"),
                                                         service.running ? QStringLiteral("Running") : QStringLiteral("Stopped")});
        item->setData(0, kKeyRole, service.name);
    }
    selectTreeItem(m_serviceTree, keep);
    filterTrees();
}

void OpenWrtFeature::serviceAction(const QString &action)
{
    const QString name = itemKey(m_serviceTree->currentItem());
    if (name.isEmpty()) {
        showStatus(QStringLiteral("Select a service first."));
        return;
    }
    if ((action == QStringLiteral("stop") || action == QStringLiteral("disable")) && criticalServices().contains(name)
        && !confirm(QStringLiteral("%1 %2?").arg(action == QStringLiteral("stop") ? QStringLiteral("Stop") : QStringLiteral("Disable"), name),
                    QStringLiteral("%1 is needed to run the router or to reach it. Stopping it can cut your network off or lock ForkMesh and LuCI out until the router restarts.").arg(name),
                    QStringLiteral("Continue"), true))
        return;
    m_client.call(QStringLiteral("rc"), QStringLiteral("init"), {{QStringLiteral("name"), name}, {QStringLiteral("action"), action}}, [this, name, action](const RpcResult &result) {
        report(result, QStringLiteral("%1: %2 done.").arg(name, action));
        loadServices();
    }, 60000);
}

void OpenWrtFeature::renderPackages()
{
    const QString keep = itemKey(m_packageTree->currentItem());
    m_packageTree->clear();
    QList<QTreeWidgetItem *> items;
    for (const Package &package : m_packages) {
        auto *item = new QTreeWidgetItem({package.name, package.version});
        item->setData(0, kKeyRole, package.name);
        items << item;
    }
    m_packageTree->addTopLevelItems(items);
    selectTreeItem(m_packageTree, keep);
    filterTrees();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

void OpenWrtFeature::packageAction(const QString &action, const QString &name)
{
    if (m_packageTool < 0) {
        showStatus(QStringLiteral("The router's package manager is not available to ForkMesh."), true);
        return;
    }
    if (action != QStringLiteral("update") && !validPackageName(name)) {
        showStatus(name.isEmpty() ? QStringLiteral("Choose a package first.") : QStringLiteral("\"%1\" is not a package name.").arg(name), true);
        return;
    }
    if (action == QStringLiteral("remove")
        && !confirm(QStringLiteral("Remove %1?").arg(name), QStringLiteral("Removing a package the router depends on can break it until you reinstall the package or reset the router."),
                    QStringLiteral("Remove"), true))
        return;
    const PackageTool tool = packageTools().at(m_packageTool);
    m_packageOutput->setPlainText(QStringLiteral("Running %1 %2…").arg(tool.program, packageArguments(tool, action, name).join(QLatin1Char(' '))));
    m_client.exec(tool.program, packageArguments(tool, action, name), [this, action, name](const RpcResult &result) {
        const QString out = result.data.value(QStringLiteral("stdout")).toString();
        const QString err = result.data.value(QStringLiteral("stderr")).toString();
        QString text = (out + (err.isEmpty() ? QString() : QStringLiteral("\n") + err)).trimmed();
        if (!result.ok && !result.error.isEmpty() && !text.contains(result.error))
            text = (text + QStringLiteral("\n") + result.error).trimmed();
        m_packageOutput->setPlainText(text.isEmpty() ? QStringLiteral("Done.") : text);
        const QString subject = name.isEmpty() ? QStringLiteral("Package lists") : name;
        report(result, action == QStringLiteral("update") ? QStringLiteral("Package lists updated.")
                       : action == QStringLiteral("install") ? QStringLiteral("%1 installed.").arg(subject)
                                                             : QStringLiteral("%1 removed.").arg(subject));
        if (action == QStringLiteral("install") && result.ok)
            m_packageName->clear();
        if (action != QStringLiteral("update"))
            loadPackages();
    }, 300000);
}

void OpenWrtFeature::saveHostname()
{
    const QString name = m_hostname->text().trimmed();
    if (const QString problem = hostnameProblem(name); !problem.isEmpty()) {
        showStatus(problem, true);
        return;
    }
    const QList<UciSection> systems = sections(QStringLiteral("system"), QStringLiteral("system"));
    if (systems.isEmpty()) {
        showStatus(QStringLiteral("The router has no system settings section to change."), true);
        return;
    }
    stage(QStringLiteral("system"), systems.first().name, {{QStringLiteral("hostname"), name}}, {}, QStringLiteral("Hostname"));
}

void OpenWrtFeature::changePassword()
{
    const QString password = m_newPassword->text();
    if (const QString problem = passwordProblem(password, m_confirmPassword->text()); !problem.isEmpty()) {
        showStatus(problem, true);
        return;
    }
    const QString user = m_client.user().isEmpty() ? QStringLiteral("root") : m_client.user();
    if (!confirm(QStringLiteral("Change the password for %1?").arg(user), QStringLiteral("The new password takes effect at once for LuCI, SSH and ForkMesh."), QStringLiteral("Change password")))
        return;
    m_client.call(QStringLiteral("luci"), QStringLiteral("setPassword"), {{QStringLiteral("username"), user}, {QStringLiteral("password"), password}},
                  [this, password](const RpcResult &result) {
                      if (result.ok) {
                          m_client.rememberPassword(password);
                          m_newPassword->clear();
                          m_confirmPassword->clear();
                      }
                      report(result, QStringLiteral("Router password changed."));
                  });
}

void OpenWrtFeature::saveBackup()
{
    const QString name = QStringLiteral("backup-%1-%2.tar.gz").arg(hostname().isEmpty() ? QStringLiteral("OpenWrt") : hostname(),
                                                                   QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save router backup"), name, QStringLiteral("Router backups (*.tar.gz)"));
    if (path.isEmpty())
        return;
    showStatus(QStringLiteral("Making a backup…"));
    m_client.downloadBackup([this, path](const QByteArray &data, const QString &error) {
        if (!error.isEmpty()) {
            showStatus(error, true);
            return;
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            showStatus(QStringLiteral("Could not write %1: %2").arg(path, file.errorString()), true);
            return;
        }
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        showStatus(QStringLiteral("Backup saved to %1 (%2).").arg(path, formatBytes(data.size())));
    });
}

void OpenWrtFeature::restoreBackup()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Restore router backup"), {}, QStringLiteral("Router backups (*.tar.gz);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) {
        showStatus(QStringLiteral("Could not read %1, or it is too large to be a router backup.").arg(path), true);
        return;
    }
    const QByteArray data = file.readAll();
    if (data.size() < 2 || uchar(data.at(0)) != 0x1f || uchar(data.at(1)) != 0x8b) {
        showStatus(QStringLiteral("%1 is not a router backup (.tar.gz).").arg(QFileInfo(path).fileName()), true);
        return;
    }
    if (!confirm(QStringLiteral("Restore this backup?"), QStringLiteral("Every setting on the router is replaced by the backup's, and the router restarts. Unsaved changes are lost."),
                 QStringLiteral("Restore and restart"), true))
        return;
    showStatus(QStringLiteral("Uploading the backup…"));
    m_client.upload(QStringLiteral("/tmp/backup.tar.gz"), data, [this](const RpcResult &uploaded) {
        if (!uploaded.ok) {
            showStatus(uploaded.error, true);
            return;
        }
        m_client.exec(QStringLiteral("/sbin/sysupgrade"), {QStringLiteral("--restore-backup"), QStringLiteral("/tmp/backup.tar.gz")}, [this](const RpcResult &restored) {
            if (!restored.ok) {
                showStatus(QStringLiteral("The router could not restore the backup: %1").arg(restored.error), true);
                return;
            }
            m_client.call(QStringLiteral("system"), QStringLiteral("reboot"), {}, [this](const RpcResult &) {
                signOut();
                showStatus(QStringLiteral("Backup restored. The router is restarting; connect again in a minute or two."));
            });
        });
    });
}

void OpenWrtFeature::upgradeFirmware()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a firmware image"), {},
                                                      QStringLiteral("Firmware images (*.bin *.img *.itb *.gz);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 128 * 1024 * 1024) {
        showStatus(QStringLiteral("Could not read %1, or it is too large for a router image.").arg(path), true);
        return;
    }
    const QByteArray data = file.readAll();
    showStatus(QStringLiteral("Uploading %1 (%2)…").arg(QFileInfo(path).fileName(), formatBytes(data.size())));
    m_client.upload(QStringLiteral("/tmp/firmware.bin"), data, [this](const RpcResult &uploaded) {
        if (!uploaded.ok) {
            showStatus(uploaded.error, true);
            return;
        }
        showStatus(QStringLiteral("Checking the image…"));
        m_client.call(QStringLiteral("system"), QStringLiteral("validate_firmware_image"), {{QStringLiteral("path"), QStringLiteral("/tmp/firmware.bin")}},
                      [this](const RpcResult &validation) {
                          if (!validation.ok) {
                              showStatus(QStringLiteral("The router could not check the image: %1").arg(validation.error), true);
                              return;
                          }
                          flashFirmware(validation.data);
                      }, 60000);
    });
}

void OpenWrtFeature::flashFirmware(const QJsonObject &validation)
{
    const auto cleanup = [this] {
        m_client.call(QStringLiteral("file"), QStringLiteral("remove"), {{QStringLiteral("path"), QStringLiteral("/tmp/firmware.bin")}}, [](const RpcResult &) {});
    };
    if (const QString problem = firmwareProblem(validation); !problem.isEmpty()) {
        cleanup();
        showStatus(problem, true);
        return;
    }
    const bool canKeep = validation.value(QStringLiteral("allow_backup")).toBool(true);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Install this firmware?"));
    box.setText(QStringLiteral("Install this firmware?"));
    box.setInformativeText(QStringLiteral("The router checked the image and accepts it. Installing takes a few minutes and the router restarts. "
                                          "Do not unplug it or close its power while it works."));
    auto *keep = new QCheckBox(QStringLiteral("Keep current settings"));
    keep->setChecked(canKeep);
    keep->setEnabled(canKeep);
    box.setCheckBox(keep);
    QPushButton *flash = box.addButton(QStringLiteral("Install firmware"), QMessageBox::AcceptRole);
    QPushButton *cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != flash) {
        cleanup();
        showStatus(QStringLiteral("Firmware upgrade cancelled."));
        return;
    }
    QStringList arguments;
    if (!keep->isChecked())
        arguments << QStringLiteral("-n");
    arguments << QStringLiteral("/tmp/firmware.bin");
    m_client.exec(QStringLiteral("/sbin/sysupgrade"), arguments, [this](const RpcResult &result) {
        if (!result.ok && !result.unreachable && result.status != UbusTimeout) {
            showStatus(QStringLiteral("The router refused to install the image: %1").arg(result.error), true);
            return;
        }
        signOut();
        showStatus(QStringLiteral("The router is installing the firmware and will restart. Wait a few minutes before connecting again, and do not unplug it."));
    }, 15000);
}

void OpenWrtFeature::factoryReset()
{
    const QString phrase = confirmationPhrase(hostname());
    bool ok = false;
    const QString typed = QInputDialog::getText(this, QStringLiteral("Reset to factory defaults"),
                                                QStringLiteral("This erases every setting and installed package, then restarts the router at 192.168.1.1 with no password.\n\nType %1 to confirm:").arg(phrase),
                                                QLineEdit::Normal, {}, &ok);
    if (!ok)
        return;
    if (typed.trimmed() != phrase) {
        showStatus(QStringLiteral("Nothing was reset: the confirmation did not match."), true);
        return;
    }
    m_client.exec(QStringLiteral("/sbin/firstboot"), {QStringLiteral("-r"), QStringLiteral("-y")}, [this](const RpcResult &result) {
        if (!result.ok && !result.unreachable) {
            showStatus(QStringLiteral("The router could not reset: %1").arg(result.error), true);
            return;
        }
        signOut();
        showStatus(QStringLiteral("The router is resetting and restarting. Afterwards it answers at 192.168.1.1 with no password set."));
    }, 15000);
}

void OpenWrtFeature::reboot()
{
    if (!confirm(QStringLiteral("Restart the router?"), QStringLiteral("Your network and internet go down for a minute or two while it restarts. Changes you have not applied are lost."),
                 QStringLiteral("Restart"), true))
        return;
    m_client.call(QStringLiteral("system"), QStringLiteral("reboot"), {}, [this](const RpcResult &result) {
        if (!result.ok && !result.unreachable) {
            showStatus(result.error, true);
            return;
        }
        signOut();
        showStatus(QStringLiteral("The router is restarting. Connect again in a minute or two."));
    });
}

void OpenWrtFeature::renderAdvanced()
{
    QString keepSection;
    QString keepOption;
    if (QTreeWidgetItem *current = m_advancedTree->currentItem()) {
        keepSection = current->data(0, kSectionRole).toString();
        keepOption = current->parent() ? itemKey(current) : QString();
    }
    m_advancedTree->blockSignals(true);
    m_advancedTree->clear();
    QTreeWidgetItem *selected = nullptr;
    for (const UciSection &section : sections(m_advancedConfig)) {
        auto *item = new QTreeWidgetItem(m_advancedTree, {sectionLabel(section), section.type});
        item->setData(0, kKeyRole, QString());
        item->setData(0, kSectionRole, section.name);
        item->setToolTip(0, QStringLiteral("%1.%2 (%3)").arg(m_advancedConfig, section.name, section.type));
        if (section.name == keepSection && keepOption.isEmpty())
            selected = item;
        QStringList keys = section.options.keys();
        keys.sort();
        for (const QString &key : keys) {
            const QString value = isSecretOption(key) ? QStringLiteral("••••••") : optionText(section.options.value(key));
            auto *child = new QTreeWidgetItem(item, {key, value});
            child->setData(0, kKeyRole, key);
            child->setData(0, kSectionRole, section.name);
            child->setToolTip(1, value);
            if (section.name == keepSection && key == keepOption)
                selected = child;
        }
    }
    m_advancedTree->blockSignals(false);
    if (selected) {
        m_advancedTree->setCurrentItem(selected);
        if (selected->parent())
            selected->parent()->setExpanded(true);
    }
    showAdvancedItem(selected);
    filterTrees();
}

void OpenWrtFeature::showAdvancedItem(QTreeWidgetItem *item)
{
    const QString sectionName = item ? item->data(0, kSectionRole).toString() : QString();
    const QString option = item && item->parent() ? itemKey(item) : QString();
    m_advancedSection->setText(sectionName.isEmpty() ? QString() : QStringLiteral("%1.%2").arg(m_advancedConfig, sectionName));
    m_advancedSection->setProperty("section", sectionName);
    m_advancedOption->setText(option);
    const std::optional<UciSection> current = section(m_advancedConfig, sectionName);
    if (!current || option.isEmpty()) {
        m_advancedValue->clear();
        return;
    }
    const QJsonValue value = current->options.value(option);
    if (value.isArray()) {
        QStringList lines;
        for (const QJsonValue &entry : value.toArray())
            lines << optionText(entry);
        m_advancedValue->setPlainText(lines.join(QLatin1Char('\n')));
    } else {
        m_advancedValue->setPlainText(optionText(value));
    }
}

void OpenWrtFeature::setAdvancedOption()
{
    const QString sectionName = m_advancedSection->property("section").toString();
    const QString option = m_advancedOption->text().trimmed();
    const std::optional<UciSection> current = section(m_advancedConfig, sectionName);
    if (!current) {
        showStatus(QStringLiteral("Select a section first."), true);
        return;
    }
    if (!validUciOptionName(option)) {
        showStatus(QStringLiteral("An option name uses letters, digits and underscores only."), true);
        return;
    }
    const QStringList lines = m_advancedValue->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        stage(m_advancedConfig, sectionName, {}, {option}, QStringLiteral("%1.%2.%3").arg(m_advancedConfig, sectionName, option));
        return;
    }
    const bool list = lines.size() > 1 || current->options.value(option).isArray();
    const QJsonValue value = list ? QJsonValue(QJsonArray::fromStringList(lines)) : QJsonValue(lines.first());
    if (list && current->options.contains(option) && !current->options.value(option).isArray()) {
        m_client.call(QStringLiteral("uci"), QStringLiteral("delete"), {{QStringLiteral("config"), m_advancedConfig}, {QStringLiteral("section"), sectionName}, {QStringLiteral("option"), option}},
                      [this, sectionName, option, value](const RpcResult &) {
                          stage(m_advancedConfig, sectionName, {{option, value}}, {}, QStringLiteral("%1.%2.%3").arg(m_advancedConfig, sectionName, option));
                      });
        return;
    }
    stage(m_advancedConfig, sectionName, {{option, value}}, {}, QStringLiteral("%1.%2.%3").arg(m_advancedConfig, sectionName, option));
}

void OpenWrtFeature::deleteAdvancedOption()
{
    const QString sectionName = m_advancedSection->property("section").toString();
    const QString option = m_advancedOption->text().trimmed();
    const std::optional<UciSection> current = section(m_advancedConfig, sectionName);
    if (!current || !current->options.contains(option))
        return;
    stage(m_advancedConfig, sectionName, {}, {option}, QStringLiteral("%1.%2.%3").arg(m_advancedConfig, sectionName, option));
}

void OpenWrtFeature::deleteAdvancedSection()
{
    const QString sectionName = m_advancedSection->property("section").toString();
    const std::optional<UciSection> current = section(m_advancedConfig, sectionName);
    if (!current)
        return;
    if (!confirm(QStringLiteral("Delete %1?").arg(sectionLabel(*current)), QStringLiteral("The whole %1 section and its %2 options are removed from %3 once you apply.")
                                                                                .arg(current->type).arg(current->options.size()).arg(m_advancedConfig),
                 QStringLiteral("Delete"), true))
        return;
    deleteSection(m_advancedConfig, sectionName, QStringLiteral("%1.%2").arg(m_advancedConfig, sectionName));
}

void OpenWrtFeature::addAdvancedSection()
{
    const QString type = m_newSectionType->text().trimmed();
    const QString name = m_newSectionName->text().trimmed();
    if (m_advancedConfig.isEmpty() || !validUciName(type) || (!name.isEmpty() && !validUciName(name))) {
        showStatus(QStringLiteral("A section type and name use letters, digits and underscores only."), true);
        return;
    }
    if (!name.isEmpty() && section(m_advancedConfig, name)) {
        showStatus(QStringLiteral("%1 already has a section called %2.").arg(m_advancedConfig, name), true);
        return;
    }
    addSection(m_advancedConfig, type, name, {}, QStringLiteral("Section %1").arg(name.isEmpty() ? type : name));
    m_newSectionType->clear();
    m_newSectionName->clear();
}

QList<UciSection> OpenWrtFeature::sections(const QString &config, const QString &type) const
{
    return uciSections(m_configs.value(config), type);
}

std::optional<UciSection> OpenWrtFeature::section(const QString &config, const QString &name) const
{
    if (name.isEmpty())
        return std::nullopt;
    for (const UciSection &item : sections(config)) {
        if (item.name == name)
            return item;
    }
    return std::nullopt;
}

QString OpenWrtFeature::dhcpSectionFor(const QString &interfaceName) const
{
    for (const UciSection &item : sections(QStringLiteral("dhcp"), QStringLiteral("dhcp"))) {
        if (item.option(QStringLiteral("interface")) == interfaceName)
            return item.name;
    }
    return {};
}

QString OpenWrtFeature::hostname() const
{
    const QList<UciSection> systems = sections(QStringLiteral("system"), QStringLiteral("system"));
    if (!systems.isEmpty() && !systems.first().option(QStringLiteral("hostname")).isEmpty())
        return systems.first().option(QStringLiteral("hostname"));
    return m_board.value(QStringLiteral("hostname")).toString();
}

void OpenWrtFeature::selectTreeItem(QTreeWidget *tree, const QString &key)
{
    if (key.isEmpty())
        return;
    QTreeWidgetItemIterator it(tree);
    while (*it) {
        if (itemKey(*it) == key) {
            tree->setCurrentItem(*it);
            tree->scrollToItem(*it);
            return;
        }
        ++it;
    }
}

void OpenWrtFeature::filterTrees()
{
    const QStringList global{m_query};
    for (QTreeWidget *tree : {m_interfaceTree, m_wifiTree, m_clientTree, m_forwardTree, m_serviceTree, m_advancedTree})
        applyFilter(tree, global);
    applyFilter(m_packageTree, {m_query, m_packageFilter ? m_packageFilter->text().trimmed() : QString()});
}

void OpenWrtFeature::setHostSearchQuery(const QString &query)
{
    m_query = query.trimmed();
    filterTrees();
}

QList<forkmesh::market::NativePackageSearchHit> OpenWrtFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || limit <= 0 || !m_client.signedIn())
        return hits;
    const auto add = [&hits, &needle, limit](const QString &id, const QString &title, const QString &detail) {
        if (hits.size() >= limit || (!title.contains(needle, Qt::CaseInsensitive) && !detail.contains(needle, Qt::CaseInsensitive)))
            return;
        hits << forkmesh::market::NativePackageSearchHit{id, title, detail};
    };
    for (const UciSection &iface : sections(QStringLiteral("network"), QStringLiteral("interface")))
        add(QStringLiteral("network:") + iface.name, QStringLiteral("Interface %1").arg(iface.name), QStringLiteral("Router network · %1").arg(iface.option(QStringLiteral("proto"))));
    for (const UciSection &iface : sections(QStringLiteral("wireless"), QStringLiteral("wifi-iface")))
        add(QStringLiteral("wifi:") + iface.name, iface.option(QStringLiteral("ssid"), iface.name), QStringLiteral("Router Wi-Fi network"));
    for (const Lease &lease : m_leases)
        add(QStringLiteral("client:") + lease.mac, lease.hostname.isEmpty() ? lease.ip : lease.hostname, QStringLiteral("Router device · %1 · %2").arg(lease.ip, lease.mac));
    for (const UciSection &redirect : sections(QStringLiteral("firewall"), QStringLiteral("redirect")))
        add(QStringLiteral("forward:") + redirect.name, redirect.option(QStringLiteral("name"), redirect.name),
            QStringLiteral("Router port forward · %1 → %2").arg(redirect.option(QStringLiteral("src_dport")), redirect.option(QStringLiteral("dest_ip"))));
    for (const Service &service : m_services)
        add(QStringLiteral("service:") + service.name, service.name, QStringLiteral("Router service · %1").arg(service.running ? QStringLiteral("running") : QStringLiteral("stopped")));
    for (const Package &package : m_packages)
        add(QStringLiteral("package:") + package.name, package.name, QStringLiteral("Router package · %1").arg(package.version));
    return hits;
}

void OpenWrtFeature::activateSearchHit(const QString &id)
{
    const QString kind = id.section(QLatin1Char(':'), 0, 0);
    const QString key = id.section(QLatin1Char(':'), 1);
    m_pages->setCurrentIndex(m_client.signedIn() ? 1 : 0);
    if (kind == QStringLiteral("network")) {
        m_tabs->setCurrentIndex(NetworkTab);
        selectTreeItem(m_interfaceTree, key);
    } else if (kind == QStringLiteral("wifi")) {
        m_tabs->setCurrentIndex(WirelessTab);
        selectTreeItem(m_wifiTree, QStringLiteral("iface:") + key);
    } else if (kind == QStringLiteral("client")) {
        m_tabs->setCurrentIndex(ClientsTab);
        selectTreeItem(m_clientTree, key);
    } else if (kind == QStringLiteral("forward")) {
        m_tabs->setCurrentIndex(ForwardsTab);
        selectTreeItem(m_forwardTree, key);
    } else if (kind == QStringLiteral("service")) {
        m_tabs->setCurrentIndex(ServicesTab);
        selectTreeItem(m_serviceTree, key);
    } else if (kind == QStringLiteral("package")) {
        m_tabs->setCurrentIndex(PackagesTab);
        m_packageFilter->clear();
        selectTreeItem(m_packageTree, key);
    }
}

}

namespace {
const forkmesh::market::NativePackageRegistration openWrtRegistration({
    QStringLiteral("builtin:openwrt"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Router"),
    QStringLiteral("broadcast"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::openwrt::OpenWrtFeature(host, parent);
    },
});
}

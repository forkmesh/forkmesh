#pragma once

#include "NativePackageExtension.h"
#include "OpenWrtClient.h"
#include "OpenWrtModel.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace forkmesh::openwrt {

class OpenWrtFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit OpenWrtFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~OpenWrtFeature() override;

    void refresh() override;
    void prepareSearch() override {}
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void packageChanged() override;

private:
    QWidget *buildSignIn();
    QWidget *buildManager();
    QWidget *buildOverview();
    QWidget *buildNetwork();
    QWidget *buildWireless();
    QWidget *buildClients();
    QWidget *buildForwards();
    QWidget *buildStorage();
    QWidget *buildServices();
    QWidget *buildPackages();
    QWidget *buildSystem();
    QWidget *buildAdvanced();

    void detectGateway();
    void connectToRouter();
    void trustCertificate();
    void signOut();
    QString loginStoreName() const;
    void updateLoginStore();
    void signInWithSavedLogin();
    void saveLoginToStore(const QString &password, const QString &done);
    void showStatus(const QString &text, bool error = false);
    bool confirm(const QString &title, const QString &text, const QString &action, bool danger = false);
    void report(const RpcResult &result, const QString &success);

    void loadAll();
    void loadOverview();
    void loadConfig(const QString &config);
    void loadChanges();
    void loadLeases();
    void loadServices();
    void loadStorage();
    void loadPackages();
    void loadAdvancedConfigs();

    void stage(const QString &config, const QString &section, const QJsonObject &values,
               const QStringList &removed, const QString &what, const Client::Done &after = {});
    void addSection(const QString &config, const QString &type, const QString &name, const QJsonObject &values,
                    const QString &what, const Client::Done &after = {});
    void deleteSection(const QString &config, const QString &section, const QString &what);
    void reviewChanges();
    void applyChanges();
    void confirmApply(qint64 deadline);
    void discardChanges();

    void renderOverview();
    void renderNetwork();
    void renderWireless();
    void renderClients();
    void renderForwards();
    void renderServices();
    void renderStorage();
    void renderPackages();
    void renderAdvanced();
    void renderChanges();

    void showInterface(const QString &name);
    void saveInterface();
    void restartInterface();
    void showWireless(QTreeWidgetItem *item);
    void saveRadio();
    void saveWifiNetwork();
    void addWifiNetwork();
    void deleteWifiNetwork();
    void reserveClient();
    void removeReservation();
    void addReservation();
    void showForward(const QString &section);
    void saveForward();
    void deleteForward();
    void serviceAction(const QString &action);
    const StorageDisk *selectedDisk() const;
    void mountAtBoot();
    void stopMountAtBoot();
    void unmountDisk();
    void mountAll();
    void setAutoMount(bool on);
    void shareDisk();
    void stopSharing();
    void installStorageSupport();
    void installStoragePackages(QStringList remaining, QStringList failed);
    void installNetworkSharing();
    void installPackageList(QStringList remaining, QStringList failed, QString firstFailure,
                            const std::function<void(const QStringList &, const QString &)> &done);
    void installPackages(const QStringList &names, const std::function<void(const QString &)> &note,
                         const std::function<void(const RpcResult &)> &done);
    void waitForPackages(const QStringList &names, qint64 deadline, const RpcResult &timedOut,
                         const std::function<void(const RpcResult &)> &done);
    void browseShare();
    void createFstab(const std::function<void()> &then);
    void createShareConfig();
    void finishShareInstall(const QString &status, bool error);
    void checkStorage(bool keepLog = false);
    void appendStorageLog(const QString &text);
    void packageAction(const QString &action, const QString &name);
    void detectPackageTool(int index);
    void saveHostname();
    void changePassword();
    void saveBackup();
    void restoreBackup();
    void upgradeFirmware();
    void flashFirmware(const QJsonObject &validation);
    void factoryReset();
    void reboot();
    void showAdvancedItem(QTreeWidgetItem *item);
    void setAdvancedOption();
    void deleteAdvancedOption();
    void deleteAdvancedSection();
    void addAdvancedSection();

    QList<UciSection> sections(const QString &config, const QString &type = {}) const;
    std::optional<UciSection> section(const QString &config, const QString &name) const;
    QString dhcpSectionFor(const QString &interfaceName) const;
    QString hostname() const;
    QString settingsKey(const QString &name) const;
    void filterTrees();
    void selectTreeItem(QTreeWidget *tree, const QString &key);

    forkmesh::market::NativePackageHost m_host;
    Client m_client;
    std::function<void()> m_searchIndexChanged;
    QString m_query;

    QJsonObject m_board;
    QJsonObject m_info;
    QList<InterfaceStatus> m_interfaces;
    QJsonObject m_wirelessStatus;
    QHash<QString, QJsonObject> m_configs;
    QJsonObject m_changes;
    QList<Lease> m_leases;
    QList<Service> m_services;
    QList<BlockDevice> m_blocks;
    QList<MountPoint> m_mounts;
    QList<StorageDisk> m_disks;
    QString m_storageProblem;
    QSet<QString> m_missingConfigs;
    bool m_storageBusy = false;
    QList<Package> m_packages;
    int m_packageTool = -1;
    QStringList m_advancedConfigs;
    QString m_advancedConfig;
    QString m_selectedInterface;
    QString m_selectedForward;
    bool m_applying = false;

    QStackedWidget *m_pages = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_routerLabel = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_signOutButton = nullptr;

    QLineEdit *m_address = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_savedLoginButton = nullptr;
    QCheckBox *m_saveLogin = nullptr;
    QLabel *m_passwordNote = nullptr;
    QString m_loginUrl;
    bool m_passwordFromStore = false;
    QLabel *m_signInNote = nullptr;
    QLabel *m_certificateLabel = nullptr;
    QPushButton *m_trustButton = nullptr;
    QString m_pendingCertificate;

    QWidget *m_changesBar = nullptr;
    QLabel *m_changesLabel = nullptr;
    QCheckBox *m_rollback = nullptr;
    QPushButton *m_applyButton = nullptr;
    QPushButton *m_discardButton = nullptr;
    QTabWidget *m_tabs = nullptr;

    QLabel *m_overview = nullptr;

    QTreeWidget *m_interfaceTree = nullptr;
    QWidget *m_interfaceEditor = nullptr;
    QLabel *m_interfaceTitle = nullptr;
    QComboBox *m_proto = nullptr;
    QLineEdit *m_ipaddr = nullptr;
    QLineEdit *m_netmask = nullptr;
    QLineEdit *m_gateway = nullptr;
    QLineEdit *m_dns = nullptr;
    QLineEdit *m_pppoeUser = nullptr;
    QLineEdit *m_pppoePassword = nullptr;
    QCheckBox *m_dhcpServer = nullptr;
    QSpinBox *m_dhcpStart = nullptr;
    QSpinBox *m_dhcpLimit = nullptr;
    QLineEdit *m_leaseTime = nullptr;
    QList<QWidget *> m_staticRows;
    QList<QWidget *> m_pppoeRows;
    QList<QWidget *> m_dhcpRows;

    QTreeWidget *m_wifiTree = nullptr;
    QStackedWidget *m_wifiEditors = nullptr;
    QString m_wifiSection;
    QLabel *m_radioTitle = nullptr;
    QCheckBox *m_radioEnabled = nullptr;
    QLineEdit *m_channel = nullptr;
    QLineEdit *m_country = nullptr;
    QLineEdit *m_txpower = nullptr;
    QLabel *m_networkTitle = nullptr;
    QCheckBox *m_wifiEnabled = nullptr;
    QLineEdit *m_ssid = nullptr;
    QComboBox *m_encryption = nullptr;
    QLineEdit *m_wifiKey = nullptr;
    QCheckBox *m_hidden = nullptr;
    QCheckBox *m_isolate = nullptr;
    QComboBox *m_wifiNetwork = nullptr;

    QTreeWidget *m_clientTree = nullptr;
    QLineEdit *m_reserveName = nullptr;
    QLineEdit *m_reserveMac = nullptr;
    QLineEdit *m_reserveIp = nullptr;

    QTreeWidget *m_forwardTree = nullptr;
    QLabel *m_forwardTitle = nullptr;
    QLineEdit *m_forwardName = nullptr;
    QComboBox *m_forwardProto = nullptr;
    QLineEdit *m_forwardExternal = nullptr;
    QLineEdit *m_forwardIp = nullptr;
    QLineEdit *m_forwardInternal = nullptr;
    QCheckBox *m_forwardEnabled = nullptr;
    QPushButton *m_forwardDelete = nullptr;

    QTreeWidget *m_serviceTree = nullptr;

    QLabel *m_storageNote = nullptr;
    QCheckBox *m_autoMount = nullptr;
    QTreeWidget *m_storageTree = nullptr;
    QPlainTextEdit *m_storageLog = nullptr;
    QTreeWidget *m_shareTree = nullptr;
    QLabel *m_shareNote = nullptr;
    QPushButton *m_shareInstall = nullptr;
    QPushButton *m_shareBrowse = nullptr;

    QTreeWidget *m_packageTree = nullptr;
    QLineEdit *m_packageFilter = nullptr;
    QLineEdit *m_packageName = nullptr;
    QPlainTextEdit *m_packageOutput = nullptr;

    QLineEdit *m_hostname = nullptr;
    QLineEdit *m_newPassword = nullptr;
    QLineEdit *m_confirmPassword = nullptr;

    QComboBox *m_advancedPicker = nullptr;
    QTreeWidget *m_advancedTree = nullptr;
    QLineEdit *m_advancedSection = nullptr;
    QLineEdit *m_advancedOption = nullptr;
    QPlainTextEdit *m_advancedValue = nullptr;
    QLineEdit *m_newSectionType = nullptr;
    QLineEdit *m_newSectionName = nullptr;
};

}

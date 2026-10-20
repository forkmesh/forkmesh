#pragma once

#include "NativePackageExtension.h"
#include "NexusCore.h"
#include "NexusNetwork.h"

#include <QList>
#include <QString>

#include <functional>
#include <memory>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;
class QWidget;

namespace forkmesh::nexus {

class NexusFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit NexusFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~NexusFeature() override;

    void refresh() override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;

private:
    enum Section { Overview, Discover, Scan, Inspect, History, Settings };

    QWidget *buildOverview();
    QWidget *buildDiscover();
    QWidget *buildScan();
    QWidget *buildInspect();
    QWidget *buildHistory();
    QWidget *buildSettings();

    void showSection(Section section);
    void load();
    void save();
    void renderOverview();
    void renderDevices();
    void renderScanResults();
    void renderHistory();
    void renderHistoryDetail();
    void loadSettingsIntoForm();
    void settingsChanged();

    void startDiscovery();
    void startScan();
    void runScan(const QList<QHostAddress> &addresses, bool isRange, const QString &target, const QList<quint16> &ports);
    void scanFinished(bool cancelled);
    void lookupDnsName();
    void inspectWeb();
    void exportSelected(bool csv);
    void deleteSelected();
    void clearHistory();
    void scanDevice(const QString &address);
    bool deniedByFirewall(const QHostAddress &address, quint16 port) const;

    QString localNetwork() const;
    const ScanSession *previousScan(const ScanSession &session) const;

    NexusArchive m_archive;
    bool m_loaded = false;
    QNetworkAccessManager *m_network = nullptr;
    std::function<void()> m_searchIndexChanged;
    std::unique_ptr<TcpScanner> m_scanner;
    std::unique_ptr<MdnsBrowser> m_browser;
    ScanSession m_currentScan;
    int m_skippedByFirewall = 0;

    QButtonGroup *m_sections = nullptr;
    QStackedWidget *m_stack = nullptr;

    QLabel *m_overviewSummary = nullptr;
    QTreeWidget *m_observations = nullptr;
    QTreeWidget *m_recentDevices = nullptr;
    QPushButton *m_overviewDiscover = nullptr;
    QPushButton *m_overviewScan = nullptr;

    QPushButton *m_discoverButton = nullptr;
    QLabel *m_discoverStatus = nullptr;
    QTreeWidget *m_devices = nullptr;
    QPushButton *m_scanDevice = nullptr;

    QLineEdit *m_target = nullptr;
    QLineEdit *m_ports = nullptr;
    QComboBox *m_presets = nullptr;
    QPushButton *m_scanButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_scanStatus = nullptr;
    QCheckBox *m_showClosed = nullptr;
    QTreeWidget *m_results = nullptr;
    QLabel *m_scanFindings = nullptr;

    QLineEdit *m_dnsName = nullptr;
    QComboBox *m_dnsType = nullptr;
    QPushButton *m_dnsButton = nullptr;
    QLabel *m_dnsStatus = nullptr;
    QTreeWidget *m_dnsAnswers = nullptr;
    QLineEdit *m_url = nullptr;
    QPushButton *m_inspectButton = nullptr;
    QLabel *m_inspectStatus = nullptr;
    QTreeWidget *m_inspection = nullptr;

    QTreeWidget *m_history = nullptr;
    QTreeWidget *m_historyDetail = nullptr;
    QPushButton *m_exportJson = nullptr;
    QPushButton *m_exportCsv = nullptr;
    QPushButton *m_deleteScan = nullptr;
    QPushButton *m_clearHistory = nullptr;

    QDoubleSpinBox *m_timeout = nullptr;
    QSpinBox *m_concurrency = nullptr;
    QCheckBox *m_allowPublic = nullptr;
    QLineEdit *m_authorized = nullptr;
    QLabel *m_settingsStatus = nullptr;
    bool m_loadingSettings = false;
};

}

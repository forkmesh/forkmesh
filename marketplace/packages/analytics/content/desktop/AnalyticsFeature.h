#pragma once

#include "NativePackageExtension.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include <functional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDialog;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;

namespace forkmesh::analytics {

class AnalyticsChart;

class AnalyticsFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    explicit AnalyticsFeature(const forkmesh::market::NativePackageHost &host,
                              QWidget *parent = nullptr);
    void refresh() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

    static QString countText(double value);
    static QString durationText(qint64 milliseconds);
    static QString changeText(double current, double previous);
    static QJsonObject overviewQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes);
    static QJsonObject campaignQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes, const QString &groupBy,
                                     const QString &host);
    static QString snippetText(const QString &origin, const QString &writeKey);
    static QString projectName(const QJsonObject &project);
    static QJsonObject goalSeries(const QJsonObject &goal);
    static QJsonObject conversionsQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes, const QJsonArray &goals);
    static QJsonObject funnelQuery(qint64 nowMs, int rangeHours, int tzOffsetMinutes, const QJsonObject &stored);
    static QString rateText(double rate);
    static QString variantSplit(const QJsonObject &flag);
    static QString agoText(qint64 nowMs, qint64 atMs);
    static QString recordingTip(const QJsonObject &recording);

private:
    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, ReplyHandler handler);
    QString organization() const;
    QString organizationPath(const QString &suffix = QString()) const;
    QJsonObject organizationRecord() const;
    QJsonObject projectRecord() const;
    void refreshOrganizations();
    void loadOrganization();
    void loadOverview();
    void loadCampaigns(int serial);
    void fillCampaigns(const QString &groupBy, const QJsonObject &result);
    void installPlugin();
    void createProject();
    void copySnippet();
    void openWebDashboard();
    void applyState();
    void renderOverview();
    void fillTable(QTableWidget *table, const QJsonArray &rows, bool visitors);
    void loadRecordings(int serial);
    void renderRecordings();
    void openRecording(int row);
    void enableRecordings();
    void setStatus(const QString &text, bool error = false);
    QWidget *buildConversionsPage();
    QWidget *buildExperimentsPage();
    void showSection(int index);
    void reloadSection();
    void clearManaged();
    QString projectPath(const QString &suffix) const;
    bool canManage() const;
    void loadObjects(const QString &kind, std::function<void(const QJsonArray &)> done);
    void loadConversions();
    void renderGoals();
    void renderGoalBreakdown();
    void renderFunnels();
    void loadFunnel();
    void editGoal(const QJsonObject &goal);
    void deleteGoal();
    void applyPlaybook();
    void loadExperiments();
    void renderExperiments();
    void loadExperimentResult();
    void newExperiment();
    void setExperimentRunning(bool running);
    QJsonObject selectedGoal() const;
    QJsonObject selectedExperiment() const;
    QJsonObject flagFor(const QString &key) const;

    Request m_request;
    std::function<void()> m_searchIndexChanged;
    QString m_packageId;
    QComboBox *m_organization = nullptr;
    QComboBox *m_project = nullptr;
    QButtonGroup *m_rangeGroup = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_installPanel = nullptr;
    QLabel *m_installNote = nullptr;
    QPushButton *m_installButton = nullptr;
    QWidget *m_emptyPanel = nullptr;
    QLabel *m_emptyNote = nullptr;
    QLineEdit *m_projectName = nullptr;
    QPushButton *m_createButton = nullptr;
    QWidget *m_content = nullptr;
    QGridLayout *m_kpis = nullptr;
    QList<QLabel *> m_kpiValues;
    QList<QLabel *> m_kpiChanges;
    AnalyticsChart *m_chart = nullptr;
    QTableWidget *m_pages = nullptr;
    QTableWidget *m_referrers = nullptr;
    QTableWidget *m_countries = nullptr;
    QTableWidget *m_devices = nullptr;
    QTableWidget *m_browsers = nullptr;
    QTableWidget *m_events = nullptr;
    QTableWidget *m_campaigns = nullptr;
    QTableWidget *m_landing = nullptr;
    QTableWidget *m_terms = nullptr;
    QTableWidget *m_recordings = nullptr;
    QLabel *m_recordingsNote = nullptr;
    QPushButton *m_recordingsEnable = nullptr;
    QJsonArray m_recordingList;
    QPointer<QDialog> m_replayDialog;
    QLabel *m_writeKey = nullptr;
    QLabel *m_truncated = nullptr;
    QButtonGroup *m_sectionGroup = nullptr;
    QStackedWidget *m_sections = nullptr;
    int m_section = 0;
    QLabel *m_conversionsNote = nullptr;
    QPushButton *m_newGoal = nullptr;
    QPushButton *m_playbook = nullptr;
    QPushButton *m_editGoal = nullptr;
    QPushButton *m_deleteGoal = nullptr;
    QTableWidget *m_goals = nullptr;
    QLabel *m_breakdownTitle = nullptr;
    QTableWidget *m_goalChannels = nullptr;
    QTableWidget *m_goalLanding = nullptr;
    QComboBox *m_funnel = nullptr;
    QTableWidget *m_funnelSteps = nullptr;
    QLabel *m_experimentsNote = nullptr;
    QPushButton *m_newExperiment = nullptr;
    QPushButton *m_toggleExperiment = nullptr;
    QTableWidget *m_experimentTable = nullptr;
    QLabel *m_experimentDetail = nullptr;
    QTableWidget *m_variants = nullptr;
    QJsonArray m_goalList;
    QJsonArray m_insights;
    QJsonArray m_flags;
    QJsonArray m_experiments;
    QJsonObject m_conversions;
    int m_conversionSerial = 0;
    int m_experimentSerial = 0;
    QJsonArray m_organizations;
    QJsonObject m_projects;
    QJsonObject m_overview;
    int m_rangeHours = 24 * 7;
    int m_serial = 0;
    bool m_loading = false;
};

} // namespace forkmesh::analytics

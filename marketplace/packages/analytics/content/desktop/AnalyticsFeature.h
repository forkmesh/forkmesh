#pragma once

#include "NativePackageExtension.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class QButtonGroup;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
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
    static QString snippetText(const QString &origin, const QString &writeKey);
    static QString projectName(const QJsonObject &project);

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
    void installPlugin();
    void createProject();
    void copySnippet();
    void openWebDashboard();
    void applyState();
    void renderOverview();
    void fillTable(QTableWidget *table, const QJsonArray &rows, bool visitors);
    void setStatus(const QString &text, bool error = false);

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
    QLabel *m_writeKey = nullptr;
    QLabel *m_truncated = nullptr;
    QJsonArray m_organizations;
    QJsonObject m_projects;
    QJsonObject m_overview;
    int m_rangeHours = 24 * 7;
    int m_serial = 0;
    bool m_loading = false;
};

} // namespace forkmesh::analytics

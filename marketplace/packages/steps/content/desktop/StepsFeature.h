#pragma once

#include "NativePackageExtension.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QComboBox;

namespace forkmesh::steps {

class StepsChart;

class StepsFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    explicit StepsFeature(const forkmesh::market::NativePackageHost &host,
                          QWidget *parent = nullptr);
    void refresh() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

    static QString stepsText(qint64 steps);
    static QString distanceText(double meters, bool imperial);
    static bool exportHasData(const QJsonObject &payload);

private:
    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, ReplyHandler handler);
    QString apiPath(const QString &suffix = QString()) const;
    void applyInstallState();
    void installPlugin();
    void load();
    void render();
    void saveManual();
    void clearManual();
    void saveGoal();
    void exportMine();
    void uninstallPlugin();
    bool saveExportBundle(const QJsonObject &payload);
    void setStatus(const QString &text, bool error = false);
    bool imperial() const;

    Request m_request;
    std::function<void()> m_searchIndexChanged;
    QLabel *m_status = nullptr;
    QWidget *m_installPanel = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_uninstallButton = nullptr;
    QWidget *m_content = nullptr;
    QLabel *m_todaySteps = nullptr;
    QLabel *m_todayDetail = nullptr;
    QProgressBar *m_goalBar = nullptr;
    QLabel *m_weekLabel = nullptr;
    QLabel *m_averageLabel = nullptr;
    QLabel *m_streakLabel = nullptr;
    QLabel *m_bestLabel = nullptr;
    QLabel *m_sourceLabel = nullptr;
    StepsChart *m_chart = nullptr;
    QDateEdit *m_manualDay = nullptr;
    QSpinBox *m_manualSteps = nullptr;
    QDoubleSpinBox *m_manualDistance = nullptr;
    QLabel *m_manualUnit = nullptr;
    QSpinBox *m_goal = nullptr;
    QComboBox *m_units = nullptr;
    QJsonObject m_overview;
    bool m_checked = false;
    bool m_installed = false;
    bool m_loading = false;
};

} // namespace forkmesh::steps

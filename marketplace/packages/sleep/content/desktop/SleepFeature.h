#pragma once

#include "NativePackageExtension.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QTime>

#include <functional>

class QDateEdit;
class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTimeEdit;

namespace forkmesh::sleep {

class SleepChart;

class SleepFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    explicit SleepFeature(const forkmesh::market::NativePackageHost &host,
                          QWidget *parent = nullptr);
    void refresh() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

    static QString durationText(qint64 minutes);
    static QString clockText(int minutesOfDay);
    static bool exportHasData(const QJsonObject &payload);
    static QPair<qint64, qint64> nightSpan(const QDate &night, const QTime &bed,
                                           const QTime &wake);

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

    Request m_request;
    std::function<void()> m_searchIndexChanged;
    QLabel *m_status = nullptr;
    QWidget *m_installPanel = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_uninstallButton = nullptr;
    QWidget *m_content = nullptr;
    QLabel *m_lastNight = nullptr;
    QLabel *m_lastDetail = nullptr;
    QProgressBar *m_goalBar = nullptr;
    QLabel *m_weekLabel = nullptr;
    QLabel *m_averageLabel = nullptr;
    QLabel *m_bedtimeLabel = nullptr;
    QLabel *m_wakeLabel = nullptr;
    QLabel *m_sourceLabel = nullptr;
    SleepChart *m_chart = nullptr;
    QDateEdit *m_manualNight = nullptr;
    QTimeEdit *m_manualBed = nullptr;
    QTimeEdit *m_manualWake = nullptr;
    QSpinBox *m_manualAwake = nullptr;
    QDoubleSpinBox *m_goal = nullptr;
    QTimeEdit *m_bedtime = nullptr;
    QTimeEdit *m_wakeTime = nullptr;
    QJsonObject m_overview;
    bool m_checked = false;
    bool m_installed = false;
    bool m_loading = false;
};

} // namespace forkmesh::sleep

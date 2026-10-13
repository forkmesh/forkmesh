#pragma once

#include "NativePackageExtension.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <functional>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace forkmesh::sugarbreak {

class SugarBreakDayStrip;

class SugarBreakFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    explicit SugarBreakFeature(const forkmesh::market::NativePackageHost &host,
                               QWidget *parent = nullptr);
    void refresh() override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

    static QString lengthLabel(int days);
    static QString dayStatusLabel(const QString &status);
    static QString withDay(const QString &path, const QString &day);
    static QString errorText(const QJsonObject &payload, const QString &error);
    static bool exportHasData(const QJsonObject &payload);

private:
    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, ReplyHandler handler);
    QString apiPath(const QString &suffix = QString()) const;
    QString today() const;
    void applyInstallState();
    void installPlugin();
    void load();
    void render();
    void renderLengths();
    void renderEvents(const QJsonArray &events);
    void renderHistory();
    void startBreak();
    void logEvent(const QString &kind);
    void undoEvent(const QString &id);
    void endBreak();
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
    QWidget *m_startPanel = nullptr;
    QLabel *m_lastResult = nullptr;
    QComboBox *m_length = nullptr;
    QPushButton *m_startButton = nullptr;
    QWidget *m_activePanel = nullptr;
    QLabel *m_dayLabel = nullptr;
    QLabel *m_dayDetail = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_progressLabel = nullptr;
    QPushButton *m_skipButton = nullptr;
    QPushButton *m_slipButton = nullptr;
    QLabel *m_todayLabel = nullptr;
    QTableWidget *m_events = nullptr;
    QLabel *m_eventsEmpty = nullptr;
    QLabel *m_skipsLabel = nullptr;
    QLabel *m_freeDaysLabel = nullptr;
    QLabel *m_streakLabel = nullptr;
    QLabel *m_bestLabel = nullptr;
    QLabel *m_leftLabel = nullptr;
    QLabel *m_slipDaysLabel = nullptr;
    SugarBreakDayStrip *m_strip = nullptr;
    QPushButton *m_endButton = nullptr;
    QTableWidget *m_history = nullptr;
    QLabel *m_historyEmpty = nullptr;
    QJsonObject m_overview;
    QString m_loadedDay;
    QString m_notice;
    bool m_checked = false;
    bool m_installed = false;
    bool m_loading = false;
    bool m_reloadPending = false;
    bool m_busy = false;
};

} // namespace forkmesh::sugarbreak

#pragma once

#include "NativePackageExtension.h"
#include "TimersFeature.h"

#include <QPointer>

class QHBoxLayout;
class QNetworkRequest;
class QPushButton;
class QTimer;

namespace forkmesh::timers {

class TimersClock : public forkmesh::market::NativePackageWidget
{
public:
    TimersClock(const forkmesh::market::NativePackageHost &host, QWidget *parent);

    void refresh() override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query,
                                                               int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override {}
    QString chromeSlot() const override;
    void addChromeMenuActions(QMenu *menu) override;
    void activate() override;
    bool acceptsHostCommand(const QString &command) const override;
    bool hostCommand(const QString &command, const QJsonObject &args) override;

    bool addTimer(const QString &request, TimerKind kind);
    bool addCountdownUntil(const QString &label, qint64 endsAtMs);
    void setChromeSlot(const QString &slot);
    void dropAt(const QPoint &globalPos);
    QStringList chipTexts() const;
    bool command(const QString &labelPrefix, const QString &command);
    void advance(qint64 milliseconds);
    bool pingRequest(const QString &timerId, const QString &kind, qint64 endsAtMs,
                     const QString &label, QNetworkRequest *request, QByteArray *body);

private:
    qint64 nowMs() const;
    void save();
    void showClockMenu();
    void showChipMenu(const QString &id, const QPoint &globalPos);
    bool sendPing(const Timer &timer, const QString &title, const QString &body);
    void flash(const QString &text);

    forkmesh::market::NativePackageHost m_host;
    std::function<void()> m_searchIndexChanged;
    TimersFeature m_feature;
    QHBoxLayout *m_chipRow = nullptr;
    QPushButton *m_clockButton = nullptr;
    QTimer *m_tick = nullptr;
    qint64 m_clockOffsetMs = 0;
    bool m_announcementsReady = false;
};

}

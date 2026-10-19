#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QSize>
#include <QString>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;

namespace forkmesh::analytics {

struct MotionPoint
{
    qint64 at = 0;
    QPointF point;
};

struct MotionClick
{
    qint64 at = 0;
    QPointF point;
    double scrollY = 0;
    QString type;
};

struct MotionScroll
{
    qint64 at = 0;
    double y = 0;
    double height = 0;
};

struct MotionViewport
{
    qint64 at = 0;
    QSize size;
};

struct MotionPage
{
    qint64 at = 0;
    QString href;
    double height = 0;
};

struct MotionState
{
    bool hasCursor = false;
    QPointF cursor;
    QList<MotionPoint> trail;
    QList<MotionClick> clicks;
    QSize viewport;
    double scrollY = 0;
    double pageHeight = 0;
    QString href;
    int pageIndex = -1;
};

class MotionTrack
{
public:
    void append(const QJsonArray &events);
    void finish();
    bool isEmpty() const;
    qint64 start() const { return m_start; }
    qint64 end() const { return m_end; }
    qint64 nextActivity(qint64 at) const;
    MotionState stateAt(qint64 at) const;
    const QList<MotionPage> &pages() const { return m_pages; }
    const QList<MotionClick> &clicks() const { return m_clicks; }
    const QList<MotionPoint> &moves() const { return m_moves; }
    const QList<MotionScroll> &scrolls() const { return m_scrolls; }

    static QString countryFlag(const QString &code);
    static QString pathOf(const QString &url);

private:
    QJsonArray m_raw;
    QList<MotionPoint> m_moves;
    QList<MotionClick> m_clicks;
    QList<MotionScroll> m_scrolls;
    QList<MotionViewport> m_viewports;
    QList<MotionPage> m_pages;
    QList<qint64> m_activity;
    qint64 m_start = 0;
    qint64 m_end = 0;
};

class ReplayStage;
class ReplayTimeline;

class AnalyticsReplayDialog final : public QDialog
{
public:
    using ReplyHandler = std::function<void(bool, const QJsonObject &, const QString &)>;
    using Fetch = std::function<void(const QString &, ReplyHandler)>;

    AnalyticsReplayDialog(const QJsonObject &recording, Fetch fetch, QWidget *parent = nullptr);

    static QString clockText(qint64 milliseconds);

private:
    void loadFrom(int afterSeq);
    void ready();
    void seek(qint64 at);
    void tick();
    void setPlaying(bool playing);
    void paintAt();

    QJsonObject m_recording;
    Fetch m_fetch;
    MotionTrack m_track;
    ReplayStage *m_stage = nullptr;
    ReplayTimeline *m_timeline = nullptr;
    QLabel *m_note = nullptr;
    QLabel *m_time = nullptr;
    QPushButton *m_play = nullptr;
    QComboBox *m_speed = nullptr;
    QCheckBox *m_skip = nullptr;
    QListWidget *m_pages = nullptr;
    QTimer *m_timer = nullptr;
    qint64 m_at = 0;
    qint64 m_lastTick = 0;
    int m_parts = 0;
    bool m_playing = false;
};

} // namespace forkmesh::analytics

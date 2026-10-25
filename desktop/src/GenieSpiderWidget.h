#pragma once

#include "GenieLandmarks.h"
#include "GenieSpiderPainter.h"
#include "GenieVitals.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QImage>
#include <QList>
#include <QPair>
#include <QPixmap>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QWidget>

#include <functional>

class QLabel;
class QScreen;
class QProgressBar;
class QPushButton;
class QTimer;
class QVBoxLayout;

class GenieSpiderWidget : public QWidget
{
    Q_OBJECT

public:
    explicit GenieSpiderWidget(QWidget *host);

    static constexpr int kSide = 78;
    static constexpr int kPad = 8;

    void setVitals(const GenieVitals &vitals);
    void setArena(const QRect &area, const QList<QRect> &avoid);
    void setScene(std::function<GenieScene()> scene);
    void rescanSoon();
    void calm();
    bool playing() const { return m_play.kind != GenieInteraction::None; }
    void setWanders(bool wanders);
    bool wanders() const { return m_wanders; }
    void setMood(GenieMood mood);
    void play(GenieReaction reaction);
    void dance(int which = -1);
    bool dancing() const { return m_pose.reaction == GenieReaction::Dance; }
    void setParked(bool parked);
    bool parked() const { return m_parked; }
    void walkTo(double x);
    double homeFraction() const;
    void setHomeFraction(double fraction);
    QRect bodyRect() const;
    void setEscapes(bool escapes) { m_escapes = escapes; }
    void setRoams(bool roams);
    bool roams() const { return m_roams; }
    void throwFrom(const QPointF &foot, const QPointF &velocity);
    QPixmap snapshot(qreal ratio) const;
    void setSubmerged(bool submerged);
    bool submerged() const { return m_submerged; }
    QImage frameImage();
    void lookAt(const QPointF &at);
    void hop(double vx, double vy);
    bool walking() const { return m_targetX >= 0; }
    bool airborne() const { return m_vx != 0 || m_vy != 0; }

signals:
    void clicked();
    void menuRequested(const QPoint &globalPos);
    void moved();
    void placed();
    void escaped(const QPoint &grab);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void tick();
    void pace(int ms);
    void render();
    void wander();
    void scheduleWander();
    void fly(double dt, double t);
    bool flightStep(double t);
    void settle();
    double floorAt(double x, double above = -1e9) const;
    void place();
    double clampX(double x) const;
    void head(double x);
    bool grounded() const;
    GenieStage stage() const;
    void refreshScene(double t, double maxAge);
    double restingX(double wanted);
    void keepClear(double t);
    bool startPlay(double t);
    void act(double t);
    bool script(double a, double t);
    void stopPlay();
    void focus(const QPointF &at, double t);
    QPointF aim(const QPointF &at) const;
    QPointF fingertip(const QPointF &aim) const;
    void showSilk(const QPointF &from, const QPointF &to, double reach, double slack);
    void hideSilk();
    bool conjure(double a, double t);
    void showMagic(double a, double t);
    void hideMagic();
    void calmMagic();

    GenieVitals m_vitals;
    GenieSpiderPose m_pose;
    QRect m_area;
    QList<QRect> m_avoid;
    QTimer *m_frame = nullptr;
    QTimer *m_wander = nullptr;
    QTimer *m_longPress = nullptr;
    QElapsedTimer m_clock;
    double m_lastTick = 0;
    double m_reactionStart = 0;
    double m_x = -1;
    double m_y = -1;
    double m_vx = 0;
    double m_vy = 0;
    double m_flight = 0;
    double m_targetX = -1;
    double m_fraction = 0.82;
    double m_lastMove = 0;
    double m_landAt = -10;
    bool m_wanders = true;
    bool m_parked = false;
    bool m_held = false;
    bool m_dragged = false;
    bool m_thrown = false;
    bool m_placing = false;
    bool m_escapes = false;
    bool m_roams = false;
    QList<QPair<double, QPointF>> m_trail;
    QPoint m_grab;
    QPoint m_pressGlobal;
    QImage m_image;
    QImage m_halo;
    bool m_dirty = true;
    bool m_hovered = false;
    double m_greetAt = -10;
    double m_haloAt = -10;
    int m_lastDance = -1;
    bool m_submerged = false;
    QPointF m_lookAt;
    double m_lookUntil = -10;
    std::function<GenieScene()> m_sceneSource;
    GenieScene m_scene;
    double m_sceneAt = -10;
    double m_sceneDue = 0;
    bool m_sceneStale = true;
    GenieInteractionBag m_bag{QRandomGenerator::global()->generate()};
    GeniePlan m_play;
    bool m_playActing = false;
    bool m_playWalk = false;
    double m_playStart = 0;
    double m_playMark = 0;
    double m_playFrom = -1;
    int m_playStep = -1;
    int m_playIndex = 0;
    quint32 m_playRoll = 0;
    double m_perk = 0;
    QPointF m_focus;
    double m_focusUntil = -10;
    QPointer<QWidget> m_silk;
    QPointer<QWidget> m_magic;
    QPointF m_poofFrom;
    double m_lift = 0;
    double m_grow = 1;
};

class GenieTipBubble : public QFrame
{
    Q_OBJECT

public:
    explicit GenieTipBubble(QWidget *host);

    void showTip(const QString &text, const QRect &anchor, int ms, const QString &prompt = QString(),
                 const QString &task = QString());
    void say(const QString &line, const QRect &anchor, int ms);
    void setReplies(bool on);

signals:
    void clicked();
    void addToAgent(const QString &prompt);
    void reply(const QString &tip);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void present(const QString &text, const QRect &anchor, int ms, const QString &prompt, const QString &task,
                 bool line);

    QLabel *m_text = nullptr;
    QLabel *m_task = nullptr;
    QPushButton *m_add = nullptr;
    QPushButton *m_reply = nullptr;
    QTimer *m_hide = nullptr;
    QString m_prompt;
    bool m_replies = true;
    bool m_line = false;
    int m_tailX = 0;
};

class GenieDesktopStage : public QWidget
{
    Q_OBJECT

public:
    GenieDesktopStage();

    static constexpr const char *kMime = "application/x-forkmesh-genie";

    void open(QScreen *screen);
    void setCatching(bool catching);
    bool takeCatch(QPointF *at, QPointF *velocity);

signals:
    void arenaChanged();

protected:
    void childEvent(QChildEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void note(const QPointF &at);
    void updateMask();

    QElapsedTimer m_clock;
    QList<QPair<double, QPointF>> m_trail;
    QPointF m_at;
    QPointF m_velocity;
    bool m_catching = false;
    bool m_caught = false;
};

class GenieCoachPanel : public QFrame
{
    Q_OBJECT

public:
    explicit GenieCoachPanel(QWidget *host);

    void setCoaching(const GenieVitals &vitals, const GenieCoaching &coaching, bool measuring);
    void place(const QRect &anchor);

signals:
    void navigate(const QString &page);
    void stepTicked(const QString &id, bool done);
    void refreshRequested();
    void closeRequested();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *m_mood = nullptr;
    QLabel *m_hearts = nullptr;
    QWidget *m_codeRow = nullptr;
    QWidget *m_marketingRow = nullptr;
    QProgressBar *m_code = nullptr;
    QProgressBar *m_marketing = nullptr;
    QLabel *m_seenTitle = nullptr;
    QWidget *m_seen = nullptr;
    QVBoxLayout *m_seenLayout = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_stage = nullptr;
    QLabel *m_headline = nullptr;
    QLabel *m_advice = nullptr;
    QPushButton *m_action = nullptr;
    QLabel *m_playbookTitle = nullptr;
    QWidget *m_steps = nullptr;
    QVBoxLayout *m_stepsLayout = nullptr;
    QLabel *m_measured = nullptr;
    QString m_actionPage;
};

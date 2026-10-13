#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QTimer;

namespace forkmesh::ui {

class StartupSplash : public QWidget
{
public:
    StartupSplash(bool dark, const QString &version, const QString &commit);

    void attachTo(QWidget *host);

    void beginStep(const QString &label);
    void addDetail(const QString &text);
    void completeCurrentStep();
    void failCurrentStep(const QString &reason);
    void noteHostPainted(const QString &label);
    void finish(const QString &label);
    void dismiss();

    bool isFinishing() const { return m_finishAtMs >= 0; }
    int stepCount() const { return m_completedSteps; }
    QStringList transcript() const;

    // `force` bypasses the pre-event-loop paint budget below. Callers that
    // are ending the splash (or handing it to a host window) must use it.
    void pump(bool force = false);
    void pumpEvents();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class RowState { Running, Done, Failed, Detail };

    struct Row {
        QString text;
        RowState state = RowState::Running;
        qint64 startedMs = 0;
        qint64 endedMs = -1; // still running while negative
    };

    struct Palette {
        QColor shadow, card, headerTop, headerBottom, border, separator;
        QColor text, muted, faint, track, accent, ok, warn, bad;
    };

    void syncFonts();
    void ensureLogo();
    void ensureShadow();
    QRectF cardRect() const;
    QRect damageRect() const;
    void followHost();
    void paintShadow(QPainter &p, const QRectF &card) const;
    void paintHeader(QPainter &p, const QRectF &card, qint64 now) const;
    void paintMeshOrbit(QPainter &p, const QPointF &centre, qint64 now) const;
    void paintProgress(QPainter &p, const QRectF &card, qint64 now);
    void paintRows(QPainter &p, const QRectF &list, qint64 now) const;
    void paintRowGlyph(QPainter &p, const QRectF &box, const Row &row,
                       qint64 now) const;
    void paintFooter(QPainter &p, const QRectF &card) const;

    void finishInternal(const QString &label, bool calibrate);
    void noteActivity();

    qreal rowHeight(const Row &row) const;
    qreal contentHeight() const;
    qreal progressTarget(qint64 now) const;
    QString elapsedText(qint64 ms) const;

    Palette m_palette;
    bool m_dark = true;
    QPointer<QWidget> m_host;
    QString m_version;
    QString m_commit;
    QPixmap m_logo;
    qreal m_logoDpr = 0.0;
    QPixmap m_shadow;
    qreal m_shadowDpr = 0.0;
    QVector<Row> m_rows;
    int m_runningRow = -1;
    int m_completedSteps = 0;
    bool m_hostPainting = false;
    bool m_hostPainted = false;
    qint64 m_lastActivityMs = 0;
    // Before the event loop starts, pump() is the only thing that paints, and
    // every startup step calls it synchronously. A launch with ~40 steps was
    // spending ~450 ms of the pre-event-loop window inside repaint(). These two
    // fields hold the last repaint's cost and finish time so pump() can cap the
    // splash at a fixed fraction of launch time (see kPaintBudgetRatio).
    qint64 m_lastPaintEndedMs = -1;
    qint64 m_lastPaintCostMs = 0;
    int m_expectedSteps = 45;
    qint64 m_expectedMs = 9000;
    QElapsedTimer m_clock;
    qint64 m_finishAtMs = -1;   // when finish() was called, else -1
    qint64 m_lastPaintMs = 0;   // for time-based progress easing
    qreal m_opacity = 1.0;
    qreal m_shownProgress = 0.0;
    QTimer *m_animation = nullptr;
    QTimer *m_deadline = nullptr;
    QString m_fontFamily;
    QFont m_titleFont, m_subtitleFont, m_rowFont, m_detailFont, m_metaFont;
};


StartupSplash *showStartupSplash(const QString &version, const QString &commit);
StartupSplash *activeStartupSplash();

void attachStartupSplashTo(QWidget *host);

void startupStep(const QString &label);
void startupDetail(const QString &text);
void startupStepFailed(const QString &reason);
void startupWindowPainted(const QString &label);
void finishStartupSplash(const QString &label = QString());
void dismissStartupSplash();

} // namespace forkmesh::ui

#pragma once

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

class QPainter;
class QVariantAnimation;

class PromptFlightOverlay final : public QWidget
{
public:
    enum class Kind { Sparkle, Prompt };

    static constexpr int kSparkleMs = 1100;
    static constexpr int kPromptMs = 1200;

    static PromptFlightOverlay *launchSparkle(QWidget *host, const QWidget *from,
                                              const QWidget *to);
    static PromptFlightOverlay *launchPrompt(QWidget *host, const QWidget *from,
                                             const QWidget *to,
                                             const QString &text);

    Kind kind() const { return m_kind; }
    QPointF startPoint() const { return m_start; }
    QPointF endPoint() const { return m_end; }
    QString text() const { return m_text; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    PromptFlightOverlay(QWidget *host, Kind kind, const QPointF &start,
                        const QPointF &end, const QString &text, int durationMs);

    QPointF sparklePoint(qreal s) const;
    QPointF promptCenter(qreal t) const;
    void paintSparkle(QPainter &p) const;
    void paintPrompt(QPainter &p) const;

    Kind m_kind;
    QPointF m_start;
    QPointF m_end;
    QPointF m_control;
    QString m_text;
    QRectF m_bubble;
    qreal m_progress = 0;
    QVariantAnimation *m_animation = nullptr;
};

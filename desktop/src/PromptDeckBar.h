#pragma once

#include <QColor>
#include <QStringList>
#include <QWidget>

class QVariantAnimation;

class PromptDeckBar : public QWidget
{
    Q_OBJECT
public:
    explicit PromptDeckBar(QWidget *parent = nullptr);

    static QString cleanPrompt(const QString &text);

    void addPrompt(const QString &text);
    void clearPrompts();
    void setColors(const QColor &surface, const QColor &tint, const QColor &border,
                   const QColor &text, const QColor &muted, const QColor &accent);

    int count() const { return m_prompts.size(); }
    int shownIndex() const { return m_index; }
    QString shownPrompt() const;
    QString positionText() const;
    bool isExpanded() const { return m_expanded; }
    int heightForWidth(int width) const override;
    bool hasHeightForWidth() const override { return true; }
    QSize sizeHint() const override;

    void showOlder();
    void showNewer();
    void showLatest();
    void setExpanded(bool expanded);

signals:
    void heightChanged();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void leaveEvent(QEvent *e) override;

private:
    enum class Hit { None, Older, Newer, Card };

    int behind() const;
    int cardHeight(int width) const;
    QRect cardRect() const;
    QRect olderRect() const;
    QRect newerRect() const;
    QRect textRect(const QRect &card) const;
    Hit hitAt(const QPoint &pos) const;
    void step(int delta);
    void slideIn(int from);
    void refreshAccessible();

    QStringList m_prompts;
    int m_index = 0;
    bool m_expanded = false;
    qreal m_drag = 0;
    qreal m_slide = 0;
    QPoint m_pressAt;
    bool m_pressed = false;
    bool m_dragging = false;
    qreal m_wheel = 0;
    qint64 m_wheelStepAt = 0;
    Hit m_hover = Hit::None;
    QVariantAnimation *m_anim = nullptr;
    QColor m_surface, m_tint, m_border, m_text, m_muted, m_accent;
};

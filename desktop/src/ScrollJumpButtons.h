#pragma once

#include <QObject>

class QAbstractScrollArea;
class QEvent;
class QString;
class JumpArrowButton;

class ScrollJumpButtons : public QObject
{
    Q_OBJECT
public:
    explicit ScrollJumpButtons(QAbstractScrollArea *area);

    void setButtonStyle(const QString &css);
    void setTheme(bool dark);

signals:
    void topClicked();    // ▲ pressed: jump to the start
    void bottomClicked(); // ▼ pressed: jump to the end

protected:
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    void reposition();
    void scheduleVisibilityUpdate();
    void updateVisibility();
    void applyArrowColor(bool dark);

    QAbstractScrollArea *m_area = nullptr;
    JumpArrowButton *m_top = nullptr;
    JumpArrowButton *m_bottom = nullptr;
    bool m_updatePending = false;
    int m_seenExtent = 0;
};

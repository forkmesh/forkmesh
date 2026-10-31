#pragma once

#include "AgentTaskList.h"

#include <QColor>
#include <QFrame>

class QLabel;
class QToolButton;

class AgentTaskListBar : public QFrame
{
    Q_OBJECT
public:
    explicit AgentTaskListBar(QWidget *parent = nullptr);

    static constexpr int kMaxRows = 8;

    void setTasks(const forkmesh::agenttasks::Tracker &tracker);
    void setColors(const QColor &surface, const QColor &border, const QColor &text,
                   const QColor &muted, const QColor &accent, const QColor &done);
    void setCollapsed(bool collapsed);

    bool isCollapsed() const { return m_collapsed; }
    QString headerText() const;
    QString rowsHtml() const;
    int heightForWidth(int width) const override;
    bool hasHeightForWidth() const override { return true; }

signals:
    void heightChanged();

private:
    void refresh();
    void restyle();

    QToolButton *m_header = nullptr;
    QLabel *m_rows = nullptr;
    QList<forkmesh::agenttasks::TaskGroup> m_groups;
    forkmesh::agenttasks::Counts m_counts;
    QString m_current;
    bool m_collapsed = false;
    bool m_userCollapsed = false;
    QColor m_surface, m_border, m_text, m_muted, m_accent, m_done;
};

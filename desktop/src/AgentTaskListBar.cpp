#include "AgentTaskListBar.h"

#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

using forkmesh::agenttasks::Task;
using forkmesh::agenttasks::Tracker;

AgentTaskListBar::AgentTaskListBar(QWidget *parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("transcriptTaskList"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 6, 10, 8);
    layout->setSpacing(4);
    m_header = new QToolButton(this);
    m_header->setObjectName(QStringLiteral("transcriptTaskListHeader"));
    m_header->setAutoRaise(true);
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(m_header, &QToolButton::clicked, this, [this] {
        m_userCollapsed = !m_collapsed;
        setCollapsed(!m_collapsed);
    });
    m_rows = new QLabel(this);
    m_rows->setObjectName(QStringLiteral("transcriptTaskListRows"));
    m_rows->setTextFormat(Qt::RichText);
    m_rows->setWordWrap(true);
    m_rows->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_header);
    layout->addWidget(m_rows);
    hide();
    restyle();
}

void AgentTaskListBar::setTasks(const Tracker &tracker)
{
    const bool wasDone = m_counts.total > 0 && m_counts.completed == m_counts.total;
    m_groups = tracker.groups();
    m_counts = tracker.counts();
    const Task *current = tracker.current();
    m_current = current ? (current->activeForm.isEmpty() ? current->subject : current->activeForm)
                        : QString();
    const bool done = tracker.allCompleted();
    if (done != wasDone && !m_userCollapsed)
        m_collapsed = done;
    if (m_groups.isEmpty())
        m_userCollapsed = false;
    refresh();
}

void AgentTaskListBar::setColors(const QColor &surface, const QColor &border,
                                 const QColor &text, const QColor &muted,
                                 const QColor &accent, const QColor &done)
{
    m_surface = surface;
    m_border = border;
    m_text = text;
    m_muted = muted;
    m_accent = accent;
    m_done = done;
    restyle();
    refresh();
}

void AgentTaskListBar::setCollapsed(bool collapsed)
{
    m_collapsed = collapsed;
    refresh();
}

QString AgentTaskListBar::headerText() const
{
    QString text = QStringLiteral("%1 %2")
                       .arg(m_collapsed ? QStringLiteral("▸") : QStringLiteral("▾"),
                            forkmesh::agenttasks::summaryText(m_counts));
    if (m_collapsed && !m_current.isEmpty())
        text += QStringLiteral(" · ") + m_current;
    return text;
}

QString AgentTaskListBar::rowsHtml() const
{
    QStringList rows;
    int shown = 0;
    for (const forkmesh::agenttasks::TaskGroup &group : m_groups) {
        if (shown == kMaxRows)
            break;
        const QString indent = group.subagent() ? QStringLiteral("&nbsp;&nbsp;&nbsp;") : QString();
        if (group.subagent())
            rows << QStringLiteral("<span style='color:%1'>⎿ %2</span>")
                        .arg(m_muted.name(), group.title.toHtmlEscaped());
        for (const Task &task : group.tasks) {
            if (shown == kMaxRows)
                break;
            ++shown;
            if (task.completed())
                rows << QStringLiteral("%1<span style='color:%2'>✔ <s>%3</s></span>")
                            .arg(indent, m_done.name(), task.subject.toHtmlEscaped());
            else if (task.inProgress())
                rows << QStringLiteral("%1<span style='color:%2'>◼</span> <b>%3</b>")
                            .arg(indent, m_accent.name(),
                                 (task.activeForm.isEmpty() ? task.subject : task.activeForm)
                                     .toHtmlEscaped());
            else
                rows << QStringLiteral("%1<span style='color:%2'>◻</span> %3")
                            .arg(indent, m_muted.name(), task.subject.toHtmlEscaped());
        }
    }
    if (m_counts.total > shown)
        rows << QStringLiteral("<span style='color:%1'>… +%2 more</span>")
                    .arg(m_muted.name())
                    .arg(m_counts.total - shown);
    return QStringLiteral("<div style='color:%1'>%2</div>")
        .arg(m_text.name(), rows.join(QStringLiteral("<br>")));
}

int AgentTaskListBar::heightForWidth(int width) const
{
    if (m_groups.isEmpty())
        return 0;
    return layout()->totalHeightForWidth(width);
}

void AgentTaskListBar::refresh()
{
    m_header->setText(headerText());
    m_rows->setText(rowsHtml());
    m_rows->setVisible(!m_collapsed);
    setVisible(!m_groups.isEmpty());
    emit heightChanged();
}

void AgentTaskListBar::restyle()
{
    const QColor surface = m_surface.isValid() ? m_surface : palette().window().color();
    const QColor border = m_border.isValid() ? m_border : palette().mid().color();
    const QColor text = m_text.isValid() ? m_text : palette().text().color();
    setStyleSheet(QStringLiteral(
                      "#transcriptTaskList{background:%1;border:1px solid %2;border-radius:8px;}"
                      "#transcriptTaskListHeader{color:%3;background:transparent;border:none;"
                      "font-weight:600;text-align:left;padding:0;}"
                      "#transcriptTaskListRows{background:transparent;border:none;}")
                      .arg(surface.name(), border.name(), text.name()));
}

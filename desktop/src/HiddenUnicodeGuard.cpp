#include "HiddenUnicodeGuard.h"

#include "HiddenUnicode.h"

#include <QApplication>
#include <QEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QValidator>

#include <algorithm>
#include <utility>

namespace forkmesh::hiddenunicode {
namespace {

constexpr const char *kGuardedProperty = "forkmeshHiddenUnicodeGuard";
constexpr int kContext = 2;

class LineValidator final : public QValidator
{
public:
    explicit LineValidator(QLineEdit *edit)
        : QValidator(edit)
        , m_edit(edit)
    {
    }

    State validate(QString &input, int &pos) const override
    {
        if (m_edit && m_edit->echoMode() != QLineEdit::Normal)
            return Acceptable;
        const auto ranges = droppedRanges(input);
        for (auto it = ranges.crbegin(); it != ranges.crend(); ++it) {
            input.remove(it->first, it->second);
            if (pos > it->first)
                pos -= std::min(pos - it->first, it->second);
        }
        return Acceptable;
    }

private:
    QPointer<QLineEdit> m_edit;
};

class EditorWatcher final : public QObject
{
public:
    EditorWatcher(QWidget *editor, QTextDocument *document)
        : QObject(editor)
        , m_editor(editor)
        , m_document(document)
    {
        connect(document, &QTextDocument::contentsChange, this, &EditorWatcher::changed);
    }

private:
    bool editable() const
    {
        if (auto *plain = qobject_cast<QPlainTextEdit *>(m_editor.data()))
            return !plain->isReadOnly();
        if (auto *rich = qobject_cast<QTextEdit *>(m_editor.data()))
            return !rich->isReadOnly();
        return false;
    }

    bool userIsTyping() const
    {
        if (!m_editor)
            return false;
        return m_editor->hasFocus() || m_editor->window()->focusWidget() == m_editor
            || m_editor->underMouse();
    }

    void changed(int position, int, int added)
    {
        if (added <= 0 || m_cleaning || !m_document || !editable() || !userIsTyping())
            return;
        if (m_document->isRedoAvailable())
            return;
        QTextCursor span(m_document);
        span.setPosition(std::clamp(position, 0, m_document->characterCount() - 1));
        span.setPosition(std::clamp(position + added, 0, m_document->characterCount() - 1),
                         QTextCursor::KeepAnchor);
        m_pending.append(span);
        if (m_pending.size() == 1)
            QTimer::singleShot(0, this, &EditorWatcher::flush);
    }

    void flush()
    {
        const QList<QTextCursor> pending = std::exchange(m_pending, {});
        if (!m_document)
            return;
        m_cleaning = true;
        for (const QTextCursor &span : pending)
            cleanSpan(span.selectionStart(), span.selectionEnd());
        m_cleaning = false;
    }

    void cleanSpan(int start, int end)
    {
        const int last = m_document->characterCount() - 1;
        const int from = std::max(0, start - kContext);
        const int to = std::min(last, end + kContext);
        if (to <= from)
            return;
        QString segment;
        segment.reserve(to - from);
        for (int i = from; i < to; ++i)
            segment.append(m_document->characterAt(i));
        QList<std::pair<int, int>> inside;
        for (const auto &range : droppedRanges(segment)) {
            const int rangeStart = std::max(from + range.first, start);
            const int rangeEnd = std::min(from + range.first + range.second, end);
            if (rangeEnd > rangeStart)
                inside.append({rangeStart, rangeEnd});
        }
        if (inside.isEmpty())
            return;
        QTextCursor cursor(m_document);
        cursor.joinPreviousEditBlock();
        for (auto it = inside.crbegin(); it != inside.crend(); ++it) {
            cursor.setPosition(it->first);
            cursor.setPosition(it->second, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        }
        cursor.endEditBlock();
    }

    QPointer<QWidget> m_editor;
    QPointer<QTextDocument> m_document;
    QList<QTextCursor> m_pending;
    bool m_cleaning = false;
};

class InputGuard final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Polish && watched->isWidgetType())
            guardWidget(static_cast<QWidget *>(watched));
        return false;
    }
};

}

void guardWidget(QWidget *widget)
{
    if (!widget || widget->property(kGuardedProperty).toBool())
        return;
    if (auto *line = qobject_cast<QLineEdit *>(widget)) {
        widget->setProperty(kGuardedProperty, true);
        if (!line->validator() && line->inputMask().isEmpty())
            line->setValidator(new LineValidator(line));
        return;
    }
    QTextDocument *document = nullptr;
    if (auto *plain = qobject_cast<QPlainTextEdit *>(widget))
        document = plain->document();
    else if (auto *rich = qobject_cast<QTextEdit *>(widget))
        document = rich->document();
    if (!document)
        return;
    widget->setProperty(kGuardedProperty, true);
    new EditorWatcher(widget, document);
}

void installInputGuard(QApplication *app)
{
    static bool installed = false;
    if (installed || !app)
        return;
    installed = true;
    app->installEventFilter(new InputGuard(app));
    const auto widgets = QApplication::allWidgets();
    for (QWidget *widget : widgets)
        guardWidget(widget);
}

}

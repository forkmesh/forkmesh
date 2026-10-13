#pragma once

#include <QList>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QSyntaxHighlighter>
#include <QTextCursor>

class QMenu;
class QPlainTextEdit;

// Adds the familiar spell-check squiggle to prose in the prompt composer.
// The production constructor discovers an installed system dictionary. The
// word-set constructor keeps the highlighter independently testable without
// depending on dictionaries installed on the build machine.
class PromptSpellCheckHighlighter final : public QSyntaxHighlighter
{
public:
    explicit PromptSpellCheckHighlighter(QTextDocument *document);
    PromptSpellCheckHighlighter(QTextDocument *document,
                                const QSet<QString> &acceptedWords);

    bool spellCheckingAvailable() const;
    bool isMisspelled(const QString &word) const;
    QStringList suggestions(const QString &word) const;

    // Right-click correction: the editor's context menu (mouse or menu key)
    // leads with suggestions for the squiggled word it was opened on.
    void attachTo(QPlainTextEdit *editor);
    // The squiggled word touching `cursor`, selected; a null cursor otherwise.
    QTextCursor misspelledWordAt(const QTextCursor &cursor) const;
    // The editor's standard context menu with corrections prepended when
    // `cursor` sits on a misspelled word. The caller owns the menu.
    QMenu *createContextMenu(QPlainTextEdit *editor, const QTextCursor &cursor,
                             const QPoint &position) const;

protected:
    void highlightBlock(const QString &text) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QList<QPair<int, int>> misspelledRanges(const QString &text) const;

    QSet<QString> m_acceptedWords;
    bool m_useSystemDictionary = false;
    QPointer<QPlainTextEdit> m_editor;
};

#include "PromptSpellCheckHighlighter.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

QList<QTextLayout::FormatRange> formatsFor(const QTextDocument &document)
{
    const QTextBlock block = document.firstBlock();
    return block.layout() ? block.layout()->formats()
                          : QList<QTextLayout::FormatRange>{};
}

} // namespace

int main(int argc, char **argv)
{
    QApplication application(argc, argv);

    QTextDocument document;
    PromptSpellCheckHighlighter highlighter(
        &document,
        {QStringLiteral("please"), QStringLiteral("fix"),
         QStringLiteral("the"), QStringLiteral("prompt")});
    document.setPlainText(QStringLiteral("Please fx the prompt"));
    highlighter.rehighlight();

    const QList<QTextLayout::FormatRange> formats = formatsFor(document);
    check(formats.size() == 1, "only the misspelled prompt word is formatted");
    check(!formats.isEmpty() && formats.first().start == 7 &&
              formats.first().length == 2,
          "the underline covers the misspelled word exactly");
    check(!formats.isEmpty() &&
              formats.first().format.underlineStyle() ==
                  QTextCharFormat::SpellCheckUnderline,
          "misspellings use the platform spell-check squiggle style");

    document.setPlainText(QStringLiteral(
        "Fix API forkMesh @maintaner https://example.invalid/typo `teh`"));
    highlighter.rehighlight();
    check(formatsFor(document).isEmpty(),
          "acronyms, identifiers, mentions, URLs, and inline code are ignored");

    QTextDocument unavailableDocument;
    PromptSpellCheckHighlighter unavailable(&unavailableDocument, {});
    unavailableDocument.setPlainText(QStringLiteral("mispeling"));
    unavailable.rehighlight();
    check(!unavailable.spellCheckingAvailable() &&
              formatsFor(unavailableDocument).isEmpty(),
          "a missing dictionary leaves prompt text unformatted");

    QTextDocument systemDocument;
    PromptSpellCheckHighlighter systemHighlighter(&systemDocument);
    if (systemHighlighter.spellCheckingAvailable()) {
        check(systemHighlighter.isMisspelled(QStringLiteral("mispeling")),
              "the discovered system dictionary rejects a misspelling");
        check(!systemHighlighter.isMisspelled(QStringLiteral("prompt")),
              "the discovered system dictionary accepts ordinary prompt prose");
        check(systemHighlighter.suggestions(QStringLiteral("mispeling"))
                  .contains(QStringLiteral("misspelling")),
              "the discovered system dictionary suggests the correction");
    }

    QPlainTextEdit editor;
    auto *editorHighlighter = new PromptSpellCheckHighlighter(
        editor.document(),
        {QStringLiteral("please"), QStringLiteral("fix"), QStringLiteral("the"),
         QStringLiteral("prompt"), QStringLiteral("words")});
    editorHighlighter->attachTo(&editor);
    editor.setPlainText(QStringLiteral("Please fx the prompt"));
    editorHighlighter->rehighlight();

    QTextCursor onTypo(editor.document());
    onTypo.setPosition(8);
    const QTextCursor typo = editorHighlighter->misspelledWordAt(onTypo);
    check(!typo.isNull() && typo.selectedText() == QStringLiteral("fx"),
          "the cursor on a squiggled word selects that word");
    QTextCursor onCorrectWord(editor.document());
    onCorrectWord.setPosition(2);
    check(editorHighlighter->misspelledWordAt(onCorrectWord).isNull(),
          "a correctly spelled word offers nothing to correct");
    check(editorHighlighter->suggestions(QStringLiteral("fx")).value(0) ==
              QStringLiteral("fix"),
          "the nearest dictionary word is suggested first");
    check(editorHighlighter->suggestions(QStringLiteral("Promt")).value(0) ==
              QStringLiteral("Prompt"),
          "suggestions keep the capital of the word they replace");

    QMenu *menu = editorHighlighter->createContextMenu(&editor, onTypo, {});
    const QList<QAction *> actions = menu->actions();
    check(!actions.isEmpty() && actions.first()->text() == QStringLiteral("fix"),
          "the context menu leads with the correction");
    check(actions.size() > 2 && actions.at(1)->isSeparator(),
          "corrections are separated from the standard edit actions");
    if (!actions.isEmpty())
        actions.first()->trigger();
    check(editor.toPlainText() == QStringLiteral("Please fix the prompt"),
          "choosing a correction replaces the misspelled word");
    delete menu;
    editor.undo();
    check(editor.toPlainText() == QStringLiteral("Please fx the prompt"),
          "a correction is a single undo step");

    QMenu *plainMenu =
        editorHighlighter->createContextMenu(&editor, onCorrectWord, {});
    check(!plainMenu->actions().isEmpty() &&
              plainMenu->actions().first()->text() !=
                  QStringLiteral("fix"),
          "a correctly spelled word keeps the standard menu");
    delete plainMenu;

    editor.setPlainText(QStringLiteral("zzqx"));
    QTextCursor onGibberish(editor.document());
    QMenu *emptyMenu =
        editorHighlighter->createContextMenu(&editor, onGibberish, {});
    check(!emptyMenu->actions().isEmpty() &&
              !emptyMenu->actions().first()->isEnabled(),
          "a word with no near match says so instead of guessing");
    delete emptyMenu;

    editor.setPlainText(QStringLiteral("Please fx the prompt"));
    editor.resize(300, 120);
    editor.show();
    QTextCursor caret(editor.document());
    caret.setPosition(8);
    editor.setTextCursor(caret);
    QContextMenuEvent keyboardMenu(QContextMenuEvent::Keyboard, QPoint(5, 5),
                                   editor.mapToGlobal(QPoint(5, 5)));
    QApplication::sendEvent(&editor, &keyboardMenu);
    QMenu *shown = nullptr;
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *candidate = qobject_cast<QMenu *>(widget);
            candidate && candidate->isVisible())
            shown = candidate;
    }
    check(shown && !shown->actions().isEmpty() &&
              shown->actions().first()->text() == QStringLiteral("fix"),
          "opening the editor's context menu offers the correction");
    if (shown)
        shown->close();

    QTextCursor mouseTarget(editor.document());
    mouseTarget.setPosition(2);
    editor.setTextCursor(mouseTarget);
    QTextCursor pointerOnTypo(editor.document());
    pointerOnTypo.setPosition(8);
    const QPoint onFx = editor.cursorRect(pointerOnTypo).center();
    QContextMenuEvent mouseMenu(QContextMenuEvent::Mouse, onFx,
                                editor.viewport()->mapToGlobal(onFx));
    QApplication::sendEvent(editor.viewport(), &mouseMenu);
    shown = nullptr;
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *candidate = qobject_cast<QMenu *>(widget);
            candidate && candidate->isVisible())
            shown = candidate;
    }
    check(shown && !shown->actions().isEmpty() &&
              shown->actions().first()->text() == QStringLiteral("fix"),
          "right-clicking a squiggle corrects the word under the pointer, "
          "not the caret");
    if (shown)
        shown->close();

    return failures == 0 ? 0 : 1;
}

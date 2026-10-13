package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.testing.assertEq

class CodeEditingTest {
    private fun enter(text: String, offset: Int, indent: String = "  ", colon: Boolean = false): CodeEdit =
        CodeIndent.formatEdit(
            CodeEdit(text, offset),
            CodeEdit(text.replaceRange(offset, offset, "\n"), offset + 1),
            indentUnit = indent,
            colonOpensBlock = colon,
        )

    fun testCaretReportsOneBasedLineAndColumnWithTheLineCount() {
        val text = "const a = 1;\nfoo();\n"
        assertEq(CodeCaret(1, 1, 3), codeCaretOf(text, 0))
        assertEq(CodeCaret(2, 4, 3), codeCaretOf(text, 16))
        assertEq(CodeCaret(3, 1, 3), codeCaretOf(text, text.length))
    }

    fun testAnInvalidSelectionSitsAtTheStart() {
        assertEq(CodeCaret(1, 1, 2), codeCaretOf("a\nb", null))
    }

    fun testIndentDefaultsToTwoSpaces() {
        assertEq("  ", detectIndentUnit("console.log(1);\n"))
    }

    fun testIndentFollowsTheNarrowestSpaceIndent() {
        assertEq("    ", detectIndentUnit("def a():\n    if x:\n        pass\n /* x */\n"))
    }

    fun testIndentKeepsTabsInATabIndentedFile() {
        assertEq("\t", detectIndentUnit("func a() {\n\treturn\n}\n"))
    }

    fun testNewlineCarriesTheCurrentIndentation() {
        assertEq(CodeEdit("  foo();\n  ", 11), enter("  foo();", 8))
    }

    fun testNewlineIndentsOneLevelAfterAnOpeningBracket() {
        val value = enter("function a() {", 14)
        assertEq("function a() {\n  ", value.text)
        assertEq(17, value.selectionStart)
    }

    fun testNewlineMovesAClosingBracketOntoItsOwnLine() {
        val value = enter("  if (x) {}", 10)
        assertEq("  if (x) {\n    \n  }", value.text)
        assertEq(15, value.selectionStart)
    }

    fun testColonOpensABlockOnlyWhenAsked() {
        assertEq("if x:\n", enter("if x:", 5, indent = "    ").text)
        assertEq("if x:\n    ", enter("if x:", 5, indent = "    ", colon = true).text)
    }

    fun testEditsOtherThanASingleNewlineAreLeftAlone() {
        val pasted = CodeEdit("  a\nb", 5)
        assertEq(pasted, CodeIndent.formatEdit(CodeEdit("  a", 3), pasted, "  "))
    }

    fun testInsertReplacesTheSelectionAndLandsAfterIt() {
        assertEq(CodeEdit("foo()", 5), CodeIndent.insert(CodeEdit("foo", 3), "()"))
        assertEq(CodeEdit("    foo()", 4), CodeIndent.insert(CodeEdit("  foo()", 0, 2), "    "))
    }

    fun testGutterTapFindsTheLineStart() {
        assertEq(0, CodeIndent.lineStartOffset("a\nbb\nccc", 0))
        assertEq(5, CodeIndent.lineStartOffset("a\nbb\nccc", 2))
        assertEq(5, CodeIndent.lineStartOffset("a\nbb\nccc", 9))
    }

    fun testStatusBarText() {
        assertEq("Ln 2, Col 4 · 3 lines", editorStatusText(CodeCaret(2, 4, 3), focused = true))
        assertEq("1 line", editorStatusText(CodeCaret(1, 1, 1), focused = false))
        assertEq("Tabs", indentLabel("\t"))
        assertEq("Spaces: 4", indentLabel("    "))
        assertEq(30, CodeKeys.symbols.size)
    }
}

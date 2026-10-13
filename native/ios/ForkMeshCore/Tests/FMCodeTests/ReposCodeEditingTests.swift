// The code editor's logic: codeCaretOf,
// detectIndentUnit and CodeIndentFormatter.

import XCTest
@testable import FMCode

final class ReposCodeEditingTests: XCTestCase {
    /// What typing Return at `offset` of `text` produces after the formatter.
    private func enter(_ text: String, _ offset: Int, indent: String = "  ", colon: Bool = false) -> CodeEditValue {
        let units = Array(text.utf16)
        let typed = String(decoding: Array(units[0..<offset]) + [0x0A] + Array(units[offset...]), as: UTF16.self)
        return CodeEditing.formatNewline(
            old: CodeEditValue(text: text, caret: offset), new: CodeEditValue(text: typed, caret: offset + 1),
            indentUnit: indent, colonOpensBlock: colon)
    }

    func testCaretReportsOneBasedLineAndColumnWithTheLineCount() {
        let text = "const a = 1;\nfoo();\n"
        XCTAssertEqual(CodeEditing.caret(in: text, utf16Offset: 0), CodeCaret(line: 1, column: 1, lineCount: 3))
        XCTAssertEqual(CodeEditing.caret(in: text, utf16Offset: 16), CodeCaret(line: 2, column: 4, lineCount: 3))
        XCTAssertEqual(CodeEditing.caret(in: text, utf16Offset: text.utf16.count), CodeCaret(line: 3, column: 1, lineCount: 3))
    }

    func testAnInvalidSelectionSitsAtTheStart() {
        XCTAssertEqual(CodeEditing.caret(in: "a\nb", utf16Offset: nil), CodeCaret(line: 1, column: 1, lineCount: 2))
        XCTAssertEqual(CodeEditing.caret(in: "", utf16Offset: 5), CodeCaret(line: 1, column: 1, lineCount: 1))
    }

    func testIndentDefaultsToTwoSpaces() {
        XCTAssertEqual(CodeEditing.detectIndentUnit("console.log(1);\n"), "  ")
    }

    func testIndentFollowsTheNarrowestSpaceIndent() {
        XCTAssertEqual(CodeEditing.detectIndentUnit("def a():\n    if x:\n        pass\n /* x */\n"), "    ")
        XCTAssertEqual(CodeEditing.detectIndentUnit("a\n" + String(repeating: " ", count: 12) + "b\n"), String(repeating: " ", count: 8))
    }

    func testIndentKeepsTabsInATabIndentedFile() {
        XCTAssertEqual(CodeEditing.detectIndentUnit("func a() {\n\treturn\n}\n"), "\t")
    }

    func testCarriesTheCurrentIndentationOntoTheNewLine() {
        let value = enter("  foo();", 8)
        XCTAssertEqual(value.text, "  foo();\n  ")
        XCTAssertEqual(value.selectionStart, 11)
        XCTAssertEqual(value.selectionEnd, 11)
    }

    func testIndentsOneLevelAfterAnOpeningBracket() {
        let value = enter("function a() {", 14)
        XCTAssertEqual(value.text, "function a() {\n  ")
        XCTAssertEqual(value.selectionStart, 17)
    }

    func testMovesAClosingBracketAfterTheCaretOntoItsOwnLine() {
        let value = enter("  if (x) {}", 10)
        XCTAssertEqual(value.text, "  if (x) {\n    \n  }")
        XCTAssertEqual(value.selectionStart, 15)
    }

    func testOpensABlockOnATrailingColonOnlyWhenAsked() {
        XCTAssertEqual(enter("if x:", 5, indent: "    ").text, "if x:\n")
        XCTAssertEqual(enter("if x:", 5, indent: "    ", colon: true).text, "if x:\n    ")
    }

    func testLeavesEditsOtherThanASingleNewlineAlone() {
        let old = CodeEditValue(text: "  a", caret: 3)
        let pasted = CodeEditValue(text: "  a\nb", caret: 5)
        XCTAssertEqual(CodeEditing.formatNewline(old: old, new: pasted, indentUnit: "  "), pasted)
        // Return over a selection replaces it and still indents.
        let selected = CodeEditing.formatNewline(
            old: CodeEditValue(text: "  abc", selectionStart: 3, selectionEnd: 5),
            new: CodeEditValue(text: "  a\n", caret: 4), indentUnit: "  ")
        XCTAssertEqual(selected.text, "  a\n  ")
        XCTAssertEqual(selected.selectionStart, 6)
    }

    func testKeyBarSymbols() {
        XCTAssertEqual(CodeEditing.keyBarSymbols.count, 30)
        XCTAssertEqual(CodeEditing.keyBarSymbols.first, "{")
        XCTAssertEqual(CodeEditing.keyBarSymbols.last, "@")
        XCTAssertTrue(CodeEditing.keyBarSymbols.contains("\\"))
    }
}

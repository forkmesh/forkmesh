// The pure half of the code editor: the caret, indent unit detection, the
// indent formatter and the key bar's symbol row. The editor view itself is
// Features/Code/CodeEditorView.swift. Offsets are UTF-16 code units, as in
// UITextView's NSRange.

import FMBase
import Foundation

/// `CodeCaret`: 1-based line and column of the caret, and the line count.
public struct CodeCaret: Equatable, Hashable, Sendable {
    public var line: Int
    public var column: Int
    public var lineCount: Int

    public init(line: Int, column: Int, lineCount: Int) {
        self.line = line
        self.column = column
        self.lineCount = lineCount
    }

    public static let start = CodeCaret(line: 1, column: 1, lineCount: 1)
}

/// A buffer and its selection (`TextEditingValue`), in UTF-16 offsets. A
/// negative `selectionStart` is an invalid selection.
public struct CodeEditValue: Equatable, Hashable, Sendable {
    public var text: String
    public var selectionStart: Int
    public var selectionEnd: Int

    public init(text: String, selectionStart: Int, selectionEnd: Int) {
        self.text = text
        self.selectionStart = selectionStart
        self.selectionEnd = selectionEnd
    }

    /// A collapsed caret at `offset`.
    public init(text: String, caret offset: Int) {
        self.init(text: text, selectionStart: offset, selectionEnd: offset)
    }

    public var isValid: Bool { selectionStart >= 0 && selectionEnd >= 0 }
    public var isCollapsed: Bool { selectionStart == selectionEnd }
}

public enum CodeEditing {
    /// `codeCaretOf`: nil (an invalid selection) sits at the start.
    public static func caret(in text: String, utf16Offset: Int?) -> CodeCaret {
        let units = Array(text.utf16)
        let caret = utf16Offset.map { min(max($0, 0), units.count) } ?? 0
        var lines = 1
        var caretLine = 1
        var lineStart = 0
        for i in 0..<units.count {
            if i == caret { caretLine = lines }
            if units[i] == 0x0A {
                lines += 1
                if i < caret { lineStart = i + 1 }
            }
        }
        if caret == units.count { caretLine = lines }
        return CodeCaret(line: caretLine, column: caret - lineStart + 1, lineCount: lines)
    }

    /// `detectIndentUnit`: a tab-indented file keeps tabs, otherwise the
    /// narrowest run of two or more leading spaces (two when nothing is
    /// indented yet), clamped to 2...8.
    public static func detectIndentUnit(_ text: String) -> String {
        var tabLines = 0
        var spaceLines = 0
        var narrowest: Int?
        for line in text.components(separatedBy: "\n") {
            if line.dartTrimmed.isEmpty { continue }
            if line.hasPrefix("\t") {
                tabLines += 1
                continue
            }
            let spaces = line.utf16.prefix { $0 == 0x20 }.count
            // A single space is usually alignment (` * ` in doc comments).
            if spaces < 2 { continue }
            spaceLines += 1
            narrowest = narrowest.map { min($0, spaces) } ?? spaces
        }
        if tabLines > spaceLines { return "\t" }
        return String(repeating: " ", count: min(max(narrowest ?? 2, 2), 8))
    }

    private static let pairs: [UInt16: UInt16] = [0x7B: 0x7D, 0x5B: 0x5D, 0x28: 0x29]

    /// `CodeIndentFormatter.formatEditUpdate`: when the edit from `old` to
    /// `new` typed a single newline over the selection, carries the line's
    /// indentation onto the new line, one level deeper after an opening
    /// bracket (or a `:` when `colonOpensBlock`, for Python and YAML), and
    /// drops a closing bracket right after the caret onto its own line. Any
    /// other edit is returned unchanged.
    public static func formatNewline(
        old: CodeEditValue, new: CodeEditValue, indentUnit: String, colonOpensBlock: Bool = false
    ) -> CodeEditValue {
        if !old.isValid || !new.isCollapsed { return new }
        let oldUnits = Array(old.text.utf16)
        let newUnits = Array(new.text.utf16)
        let start = min(old.selectionStart, old.selectionEnd)
        let end = max(old.selectionStart, old.selectionEnd)
        guard end <= oldUnits.count else { return new }
        let expected = Array(oldUnits[0..<start]) + [0x0A] + Array(oldUnits[end...])
        if new.selectionStart != start + 1 || newUnits.count != oldUnits.count - (end - start) + 1
            || newUnits[start] != 0x0A || newUnits != expected {
            return new
        }

        var lineStart = 0
        if start > 0 {
            var i = start - 1
            while i >= 0 {
                if oldUnits[i] == 0x0A {
                    lineStart = i + 1
                    break
                }
                i -= 1
            }
        }
        let line = Array(oldUnits[lineStart..<start])
        var indentEnd = 0
        while indentEnd < line.count && (line[indentEnd] == 0x20 || line[indentEnd] == 0x09) { indentEnd += 1 }
        let indent = Array(line[0..<indentEnd])
        let trimmed = trimRightUnits(line)
        let last: UInt16? = trimmed.last
        let opens = (last.map { pairs[$0] != nil } ?? false) || (colonOpensBlock && last == 0x3A)

        var insert: [UInt16] = [0x0A] + indent
        if opens { insert += Array(indentUnit.utf16) }
        let caret = start + insert.count
        if let last, let close = pairs[last], end < oldUnits.count, oldUnits[end] == close {
            insert += [0x0A] + indent
        }
        if insert == [0x0A] { return new }
        let text = Array(oldUnits[0..<start]) + insert + Array(oldUnits[end...])
        return CodeEditValue(text: String(decoding: text, as: UTF16.self), caret: caret)
    }

    /// Trailing whitespace trimmed over code units.
    private static func trimRightUnits(_ units: [UInt16]) -> [UInt16] {
        var end = units.count
        while end > 0 {
            let u = units[end - 1]
            let isSpace = u == 0x20 || (u >= 0x09 && u <= 0x0D) || u == 0x85 || u == 0xA0 || u == 0x1680
                || (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 || u == 0x202F || u == 0x205F
                || u == 0x3000 || u == 0xFEFF
            if !isSpace { break }
            end -= 1
        }
        return Array(units[0..<end])
    }

    /// `CodeKeyBar.symbols`: the characters phone keyboards bury.
    public static let keyBarSymbols: [String] = [
        "{", "}", "(", ")", "[", "]", ";", ":", "=", "<", ">", "\"", "'", "`", "/", "\\", ".", ",", "_", "-", "+",
        "*", "&", "|", "!", "?", "#", "$", "%", "@",
    ]
}

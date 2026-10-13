package com.forkmesh.nativeapp.code

data class CodeCaret(val line: Int, val column: Int, val lineCount: Int)

fun codeCaretOf(text: String, selectionEnd: Int?): CodeCaret {
    val caret = selectionEnd?.coerceIn(0, text.length) ?: 0
    var lines = 1
    var caretLine = 1
    var lineStart = 0
    for (i in text.indices) {
        if (i == caret) caretLine = lines
        if (text[i] == '\n') {
            lines++
            if (i < caret) lineStart = i + 1
        }
    }
    if (caret == text.length) caretLine = lines
    return CodeCaret(line = caretLine, column = caret - lineStart + 1, lineCount = lines)
}

fun detectIndentUnit(text: String): String {
    var tabLines = 0
    var spaceLines = 0
    var narrowest: Int? = null
    for (line in text.split('\n')) {
        if (line.isBlank()) continue
        if (line.startsWith("\t")) {
            tabLines++
            continue
        }
        var spaces = 0
        while (spaces < line.length && line[spaces] == ' ') spaces++
        if (spaces < 2) continue
        spaceLines++
        narrowest = if (narrowest == null) spaces else minOf(narrowest, spaces)
    }
    if (tabLines > spaceLines) return "\t"
    return " ".repeat((narrowest ?: 2).coerceIn(2, 8))
}

data class CodeEdit(val text: String, val selectionStart: Int, val selectionEnd: Int = selectionStart)

object CodeIndent {
    private val pairs = mapOf('{' to '}', '[' to ']', '(' to ')')

    fun colonOpensBlock(lang: CodeLang): Boolean = lang == CodeLang.Python || lang == CodeLang.Yaml

    fun formatEdit(old: CodeEdit, new: CodeEdit, indentUnit: String, colonOpensBlock: Boolean = false): CodeEdit {
        val start = old.selectionStart
        val end = old.selectionEnd
        if (start < 0 || end < start || end > old.text.length) return new
        if (new.selectionStart != new.selectionEnd) return new
        val before = old.text
        val text = new.text
        if (new.selectionStart != start + 1 ||
            text.length != before.length - (end - start) + 1 ||
            text[start] != '\n' ||
            text != before.replaceRange(start, end, "\n")
        ) {
            return new
        }
        val lineStart = if (start == 0) 0 else before.lastIndexOf('\n', start - 1) + 1
        val line = before.substring(lineStart, start)
        var indentEnd = 0
        while (indentEnd < line.length && (line[indentEnd] == ' ' || line[indentEnd] == '\t')) indentEnd++
        val indent = line.substring(0, indentEnd)
        val trimmed = line.trimEnd()
        val last = trimmed.lastOrNull()
        val opens = (last != null && pairs.containsKey(last)) || (colonOpensBlock && last == ':')
        var insert = "\n$indent"
        if (opens) insert += indentUnit
        val caret = start + insert.length
        if (last != null && pairs.containsKey(last) && end < before.length && before[end] == pairs[last]) {
            insert += "\n$indent"
        }
        if (insert == "\n") return new
        return CodeEdit(before.replaceRange(start, end, insert), caret)
    }

    fun insert(value: CodeEdit, inserted: String): CodeEdit {
        val start = value.selectionStart.coerceIn(0, value.text.length)
        val end = value.selectionEnd.coerceIn(start, value.text.length)
        val text = value.text.replaceRange(start, end, inserted)
        return CodeEdit(text, start + inserted.length)
    }

    fun lineStartOffset(text: String, line: Int): Int {
        var offset = 0
        for (i in 0 until line) {
            val next = text.indexOf('\n', offset)
            if (next < 0) break
            offset = next + 1
        }
        return offset
    }
}

object CodeKeys {
    val symbols: List<String> = listOf(
        "{", "}", "(", ")", "[", "]", ";", ":", "=", "<", ">", "\"", "'", "`", "/", "\\",
        ".", ",", "_", "-", "+", "*", "&", "|", "!", "?", "#", "$", "%", "@",
    )
}

fun editorStatusText(caret: CodeCaret, focused: Boolean): String {
    val lines = "${caret.lineCount} ${if (caret.lineCount == 1) "line" else "lines"}"
    return if (focused) "Ln ${caret.line}, Col ${caret.column} · $lines" else lines
}

fun indentLabel(indent: String): String = if (indent == "\t") "Tabs" else "Spaces: ${indent.length}"

package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.code.CodeTokenKind.Base
import com.forkmesh.nativeapp.code.CodeTokenKind.Comment
import com.forkmesh.nativeapp.code.CodeTokenKind.Function
import com.forkmesh.nativeapp.code.CodeTokenKind.Heading
import com.forkmesh.nativeapp.code.CodeTokenKind.Keyword
import com.forkmesh.nativeapp.code.CodeTokenKind.Number
import com.forkmesh.nativeapp.code.CodeTokenKind.String
import com.forkmesh.nativeapp.code.CodeTokenKind.Type
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class CodeHighlighterTest {
    private fun tokens(lang: CodeLang, line: kotlin.String, inBlock: Boolean = false) =
        CodeHighlighter(lang).highlight(line, inBlock).map { it.text to it.kind }

    fun testLanguagesByPath() {
        assertEq(CodeLang.Dart, codeLangForPath("lib/main.dart"))
        assertEq(CodeLang.TypeScript, codeLangForPath("src/App.TSX"))
        assertEq(CodeLang.Shell, codeLangForPath("docker/Dockerfile"))
        assertEq(CodeLang.Shell, codeLangForPath("Makefile"))
        assertEq(CodeLang.Shell, codeLangForPath("rules.mk"))
        assertEq(CodeLang.C, codeLangForPath("a/b.mm"))
        assertEq(CodeLang.Toml, codeLangForPath("setup.cfg"))
        assertEq(CodeLang.Json, codeLangForPath("pubspec.lock"))
        assertEq(CodeLang.Html, codeLangForPath("icon.svg"))
        assertEq(CodeLang.Plain, codeLangForPath("LICENSE"))
        assertEq("C / C++", CodeLang.C.label)
        assertEq("Plain text", CodeLang.Plain.label)
    }

    fun testKotlinLine() {
        assertEq(
            listOf(
                "fun" to Keyword, " " to Base, "main" to Function, "(" to Base, "args" to Base, ": " to Base,
                "List" to Type, "<String" to Type, ">) = " to Base, "println" to Function, "(" to Base,
                "\"hi\"" to String, ", " to Base, "42" to Number, ", " to Base, "null" to Number, ") " to Base,
                "// done" to Comment,
            ),
            tokens(CodeLang.Kotlin, "fun main(args: List<String>) = println(\"hi\", 42, null) // done"),
        )
    }

    fun testCapitalisedCallsReadAsTypes() {
        assertEq(listOf("Widget" to Type, "(" to Base), tokens(CodeLang.Dart, "Widget("))
        assertEq(listOf("Foo" to Type, " (" to Base), tokens(CodeLang.Dart, "Foo ("))
        assertEq(listOf("@override" to Function), tokens(CodeLang.Dart, "@override"))
        assertEq(listOf("0xFF" to Number, " " to Base, "1_000.5e3" to Number), tokens(CodeLang.Dart, "0xFF 1_000.5e3"))
    }

    fun testHashCommentsOnlyWhereTheLanguageHasThem() {
        assertEq(listOf("x" to Base, " = " to Base, "1" to Number, " " to Base, "# note" to Comment), tokens(CodeLang.Python, "x = 1 # note"))
        assertEq(listOf("def" to Keyword, " " to Base, "f" to Function, "():" to Base), tokens(CodeLang.Python, "def f():"))
        assertEq(listOf("#" to Base, "include" to Base), tokens(CodeLang.C, "#include"))
    }

    fun testStringsTolerateAnUnterminatedQuote() {
        assertEq(listOf("'it\\'s" to String), tokens(CodeLang.JavaScript, "'it\\'s"))
        assertEq(listOf("`tpl`" to String), tokens(CodeLang.JavaScript, "`tpl`"))
    }

    fun testBlockCommentsAcrossLines() {
        val lines = listOf("int a; /* start", "still comment", "end */ int b;", "// /* not a block", "x")
        val highlighter = CodeHighlighter(CodeLang.C)
        assertEq(listOf(false, true, true, false, false), highlighter.blockCommentStarts(lines).toList())
        assertEq(listOf("still comment" to Comment), tokens(CodeLang.C, "still comment", inBlock = true))
        assertEq(
            listOf("end */" to Comment, " " to Base, "int" to Type, " " to Base, "b" to Base, ";" to Base),
            tokens(CodeLang.C, "end */ int b;", inBlock = true),
        )
        assertEq(listOf(false, false), CodeHighlighter(CodeLang.Python).blockCommentStarts(listOf("/*", "x")).toList())
        assertEq(listOf("/* a */" to Comment, " " to Base, "b" to Base), tokens(CodeLang.Go, "/* a */ b"))
    }

    fun testHtmlCommentsAndTags() {
        assertEq(listOf("<!-- hi -->" to Comment, " " to Base, "<div" to Type, ">" to Base), tokens(CodeLang.Html, "<!-- hi --> <div>"))
        assertEq(listOf("a" to Base, "//" to Base, "b" to Base), tokens(CodeLang.Html, "a//b"))
    }

    fun testMarkdownLines() {
        assertEq(listOf("## Title" to Heading), tokens(CodeLang.Markdown, "## Title"))
        assertEq(listOf("> quote" to Comment), tokens(CodeLang.Markdown, "> quote"))
        assertEq(listOf("```kotlin" to String), tokens(CodeLang.Markdown, "```kotlin"))
        assertEq(listOf("  - " to Keyword, "item" to Base), tokens(CodeLang.Markdown, "  - item"))
        assertEq(listOf("12. " to Keyword, "step" to Base), tokens(CodeLang.Markdown, "12. step"))
        assertEq(listOf("plain *text*" to Base), tokens(CodeLang.Markdown, "plain *text*"))
    }

    fun testEmptyAndPlainLines() {
        assertEq(listOf("" to Base), tokens(CodeLang.Kotlin, ""))
        assertEq(listOf("   " to Base), tokens(CodeLang.Kotlin, "   "))
        assertEq(listOf("if" to Base, " " to Base, "x" to Base), tokens(CodeLang.Yaml, "if x"))
        val rebuilt = CodeHighlighter(CodeLang.Rust).highlight("pub fn f<'a>(x: &'a str) -> Option<u8> { None }").joinToString("") { it.text }
        expect(rebuilt == "pub fn f<'a>(x: &'a str) -> Option<u8> { None }", rebuilt)
    }
}

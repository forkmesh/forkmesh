package com.forkmesh.nativeapp.code

enum class CodeLang(val label: String) {
    Dart("Dart"),
    TypeScript("TypeScript"),
    JavaScript("JavaScript"),
    Python("Python"),
    Go("Go"),
    Rust("Rust"),
    C("C / C++"),
    Java("Java"),
    Kotlin("Kotlin"),
    Swift("Swift"),
    Ruby("Ruby"),
    Shell("Shell"),
    Yaml("YAML"),
    Toml("TOML"),
    Json("JSON"),
    Html("HTML"),
    Css("CSS"),
    Markdown("Markdown"),
    Plain("Plain text"),
}

enum class CodeTokenKind { Base, Keyword, Heading, Type, String, Comment, Number, Function }

data class CodeToken(val text: String, val kind: CodeTokenKind)

fun codeLangForPath(path: String): CodeLang {
    val lower = path.lowercase()
    val name = lower.split('/').last()
    if (name == "dockerfile" || name == "makefile" || name.endsWith(".mk")) return CodeLang.Shell
    val dot = name.lastIndexOf('.')
    val ext = if (dot < 0) "" else name.substring(dot + 1)
    return when (ext) {
        "dart" -> CodeLang.Dart
        "ts", "tsx" -> CodeLang.TypeScript
        "js", "jsx", "mjs", "cjs" -> CodeLang.JavaScript
        "py", "pyi" -> CodeLang.Python
        "go" -> CodeLang.Go
        "rs" -> CodeLang.Rust
        "c", "cc", "cpp", "cxx", "h", "hpp", "m", "mm" -> CodeLang.C
        "java" -> CodeLang.Java
        "kt", "kts" -> CodeLang.Kotlin
        "swift" -> CodeLang.Swift
        "rb", "rake", "gemspec" -> CodeLang.Ruby
        "sh", "bash", "zsh", "fish" -> CodeLang.Shell
        "yaml", "yml" -> CodeLang.Yaml
        "toml", "ini", "cfg", "conf" -> CodeLang.Toml
        "json", "jsonc", "lock" -> CodeLang.Json
        "html", "htm", "xml", "svg", "vue", "svelte" -> CodeLang.Html
        "css", "scss", "less" -> CodeLang.Css
        "md", "mdx", "markdown" -> CodeLang.Markdown
        else -> CodeLang.Plain
    }
}

class CodeHighlighter(val lang: CodeLang) {
    private val pattern: Regex = buildPattern(lang)
    private val keywords: Set<String> = keywordsFor(lang)
    private val types: Set<String> = BUILTIN_TYPES

    private val hashComments: Boolean
        get() = when (lang) {
            CodeLang.Python, CodeLang.Ruby, CodeLang.Shell, CodeLang.Yaml, CodeLang.Toml -> true
            else -> false
        }

    private val blockComments: Boolean
        get() = when (lang) {
            CodeLang.Python, CodeLang.Ruby, CodeLang.Shell, CodeLang.Yaml, CodeLang.Toml,
            CodeLang.Markdown, CodeLang.Plain, CodeLang.Json,
            -> false
            else -> true
        }

    fun blockCommentStarts(lines: List<String>): BooleanArray {
        val result = BooleanArray(lines.size)
        if (!blockComments) return result
        var inBlock = false
        for (i in lines.indices) {
            result[i] = inBlock
            val line = lines[i]
            var cursor = 0
            while (cursor < line.length) {
                if (inBlock) {
                    val close = line.indexOf("*/", cursor)
                    if (close < 0) break
                    inBlock = false
                    cursor = close + 2
                } else {
                    val open = line.indexOf("/*", cursor)
                    if (open < 0) break
                    val lineComment = line.indexOf("//", cursor)
                    if (lineComment in 0 until open) break
                    inBlock = true
                    cursor = open + 2
                }
            }
        }
        return result
    }

    fun highlight(line: String, startsInBlockComment: Boolean = false): List<CodeToken> {
        val spans = ArrayList<CodeToken>()
        var cursor = 0
        if (startsInBlockComment) {
            val close = line.indexOf("*/")
            if (close < 0) return listOf(CodeToken(line, CodeTokenKind.Comment))
            spans += CodeToken(line.substring(0, close + 2), CodeTokenKind.Comment)
            cursor = close + 2
        }
        if (lang == CodeLang.Markdown) return markdown(line, spans, cursor)

        for (match in pattern.findAll(line, cursor)) {
            val start = match.range.first
            val end = match.range.last + 1
            if (start > cursor) spans += CodeToken(line.substring(cursor, start), CodeTokenKind.Base)
            val token = match.value
            val first = if (token.isEmpty()) "" else token.substring(0, 1)
            val kind = when {
                token.startsWith("//") || token.startsWith("/*") || token.startsWith("<!--") ||
                    (hashComments && first == "#") -> CodeTokenKind.Comment
                first == "\"" || first == "'" || first == "`" -> CodeTokenKind.String
                first == "@" -> CodeTokenKind.Function
                first == "<" -> CodeTokenKind.Type
                token.isNotEmpty() && token[0] in '0'..'9' -> CodeTokenKind.Number
                token in keywords -> CodeTokenKind.Keyword
                token in LITERALS -> CodeTokenKind.Number
                token in types -> CodeTokenKind.Type
                end < line.length && CALL_AHEAD.containsMatchIn(line.substring(end)) ->
                    if (token[0] in 'A'..'Z') CodeTokenKind.Type else CodeTokenKind.Function
                else -> CodeTokenKind.Base
            }
            spans += CodeToken(token, kind)
            cursor = end
        }
        if (cursor < line.length) spans += CodeToken(line.substring(cursor), CodeTokenKind.Base)
        if (spans.isEmpty()) return listOf(CodeToken(line, CodeTokenKind.Base))
        return spans
    }

    private fun markdown(line: String, spans: MutableList<CodeToken>, cursor: Int): List<CodeToken> {
        val rest = line.substring(cursor)
        val trimmed = rest.trimStart()
        val listMarker = LIST_MARKER.find(rest)
        when {
            trimmed.startsWith("#") -> spans += CodeToken(rest, CodeTokenKind.Heading)
            trimmed.startsWith(">") -> spans += CodeToken(rest, CodeTokenKind.Comment)
            trimmed.startsWith("```") -> spans += CodeToken(rest, CodeTokenKind.String)
            listMarker != null -> {
                val end = listMarker.range.last + 1
                spans += CodeToken(rest.substring(0, end), CodeTokenKind.Keyword)
                spans += CodeToken(rest.substring(end), CodeTokenKind.Base)
            }
            else -> spans += CodeToken(rest, CodeTokenKind.Base)
        }
        return spans
    }

    companion object {
        private val CALL_AHEAD = Regex("^\\s*\\(")
        private val LIST_MARKER = Regex("^(\\s*)([-*+]|\\d+\\.)\\s")

        private val LITERALS = setOf(
            "true", "false", "null", "nil", "None", "True", "False", "undefined", "NaN", "self", "this", "super",
        )

        private fun buildPattern(lang: CodeLang): Regex {
            val hash = when (lang) {
                CodeLang.Python, CodeLang.Ruby, CodeLang.Shell, CodeLang.Yaml, CodeLang.Toml -> true
                else -> false
            }
            val slash = when (lang) {
                CodeLang.Python, CodeLang.Ruby, CodeLang.Shell, CodeLang.Yaml, CodeLang.Toml,
                CodeLang.Markdown, CodeLang.Html, CodeLang.Plain,
                -> false
                else -> true
            }
            val html = lang == CodeLang.Html
            val parts = ArrayList<String>()
            if (html) parts += "<!--.*?(?:-->|$)"
            parts += "\"(?:[^\"\\\\]|\\\\.)*\"?"
            parts += "'(?:[^'\\\\]|\\\\.)*'?"
            parts += "`[^`]*`?"
            if (slash) parts += "//.*"
            if (hash) parts += "#.*"
            if (slash) parts += "/\\*.*?(?:\\*/|$)"
            parts += "@[A-Za-z_][\\w.]*"
            parts += "</?[A-Za-z][\\w.:-]*"
            parts += "\\b0[xX][0-9a-fA-F_]+\\b"
            parts += "\\b\\d[\\d_]*(?:\\.\\d+)?(?:[eE][+-]?\\d+)?\\b"
            parts += "[A-Za-z_$][\\w$]*(?=\\s*\\()"
            parts += "[A-Za-z_$][\\w$]*"
            return Regex(parts.joinToString("|"))
        }

        private val C_LIKE = setOf(
            "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case", "default", "try",
            "catch", "finally", "throw", "throws", "new", "class", "extends", "implements", "import", "export", "from",
            "as", "in", "of", "is", "async", "await", "yield", "static", "const", "let", "var", "function", "void",
            "enum", "interface", "type", "namespace", "with", "mixin", "typedef", "final", "late", "required", "get",
            "set", "abstract", "override", "operator", "extension", "part", "library", "show", "hide", "external",
            "factory", "covariant", "sealed", "base", "public", "private", "protected", "package", "struct", "union",
            "func", "fn", "pub", "mut", "impl", "trait", "use", "mod", "crate", "where", "match", "loop", "move", "ref",
            "dyn", "unsafe", "defer", "select", "range", "go", "chan", "map", "fallthrough", "goto", "instanceof",
            "typeof", "delete", "fun", "val", "when", "object", "companion", "data", "open", "internal", "lateinit",
            "init", "suspend", "guard", "protocol", "inout", "sizeof", "unsigned", "signed", "volatile", "register",
            "inline", "virtual", "template", "typename", "using", "constexpr", "nullptr", "declare", "readonly", "keyof",
            "satisfies", "debugger", "then", "on",
        )

        private val PYTHON = setOf(
            "def", "lambda", "pass", "raise", "except", "elif", "not", "and", "or", "global", "nonlocal", "del",
            "assert", "with", "yield", "from", "import", "as", "class", "return", "if", "else", "for", "while", "try",
            "finally", "in", "is", "async", "await", "break", "continue", "match", "case",
        )

        private val RUBY = setOf(
            "def", "end", "do", "module", "class", "if", "elsif", "else", "unless", "while", "until", "for", "in",
            "return", "yield", "begin", "rescue", "ensure", "raise", "require", "require_relative", "include", "extend",
            "attr_reader", "attr_writer", "attr_accessor", "puts", "then", "case", "when", "and", "or", "not", "lambda",
            "proc", "private", "public",
        )

        private val SHELL = setOf(
            "if", "then", "fi", "else", "elif", "for", "in", "do", "done", "while", "until", "case", "esac", "function",
            "export", "local", "return", "echo", "exit", "source", "set", "unset", "readonly", "shift", "cd", "FROM",
            "RUN", "CMD", "COPY", "ADD", "ENV", "ARG", "WORKDIR", "EXPOSE", "ENTRYPOINT", "VOLUME", "USER", "LABEL",
        )

        private val CSS = setOf("import", "media", "keyframes", "font-face", "supports", "important", "from", "to")

        private fun keywordsFor(lang: CodeLang): Set<String> = when (lang) {
            CodeLang.Python -> PYTHON
            CodeLang.Ruby -> RUBY
            CodeLang.Shell -> SHELL
            CodeLang.Css -> CSS
            CodeLang.Yaml, CodeLang.Toml, CodeLang.Json, CodeLang.Markdown, CodeLang.Plain -> emptySet()
            else -> C_LIKE
        }

        private val BUILTIN_TYPES = setOf(
            "String", "int", "double", "bool", "num", "List", "Map", "Set", "Future", "Stream", "Widget", "State",
            "BuildContext", "Object", "dynamic", "Never", "Iterable", "Duration", "DateTime", "Uint8List", "number",
            "string", "boolean", "any", "unknown", "never", "Promise", "Array", "Record", "Partial", "Readonly", "Date",
            "Error", "float", "str", "bytes", "dict", "list", "tuple", "float32", "float64", "int32", "int64", "uint",
            "uint8", "byte", "rune", "error", "i32", "i64", "u8", "u32", "u64", "usize", "isize", "f32", "f64", "char",
            "Vec", "Option", "Result", "Box", "Rc", "Arc", "HashMap", "size_t", "long", "short", "Int", "Double",
            "Float", "Boolean", "Unit", "Any", "Character",
        )
    }
}

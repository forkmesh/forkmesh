package com.forkmesh.nativeapp.codeinsight

import java.util.regex.Pattern

internal class Stripped(val noComments: String, val code: String)

internal class Measured(
    val info: InsightFile,
    val significant: IntArray = IntArray(0),
    val windows: LongArray = LongArray(0),
    val skip: Boolean = false,
)

internal object Patterns {
    val controlStart: Pattern = QRegex.compile(
        "^(if|else|for|foreach|while|do|switch|try|catch|finally|synchronized|using|lock|unsafe|" +
            "checked|unchecked|fixed|with|when|loop|match|select|defer|go|return|case|guard|" +
            "repeat|until|unless|elif|then|throw|new|await|yield|get|set|willSet|didSet)\\b",
    )
    val typeHeader: Pattern = QRegex.compile(
        "\\b(class|struct|interface|enum|namespace|union|trait|impl|protocol|extension|record|" +
            "object|module|package|actor)\\b",
    )
    val functionKeyword: Pattern = QRegex.compile("\\b(function|func|fn|fun|def|sub)\\b")
    val computedProperty: Pattern = QRegex.compile("\\bvar\\s+([A-Za-z_]\\w*)\\s*:[^=]*$")
    val callName: Pattern = QRegex.compile(
        "([A-Za-z_$~][\\w$]*(?:(?:::|\\.)~?[A-Za-z_$][\\w$]*)*|operator\\s*[^\\s(]+)\\s*(?:<[^()]*>)?\\s*\\(",
    )
    val arrowName: Pattern = QRegex.compile(
        "([A-Za-z_$][\\w$]*)\\s*[:=]\\s*(?:async\\s*)?(?:\\([^)]*\\)|[A-Za-z_$][\\w$]*)\\s*(?::[^=]*)?=>",
    )
    val tailAfterParen: Pattern = QRegex.compile("^[\\s\\w:&*<>,\\[\\].?!\\-|@]*$")
    val ternaryStrict: Pattern = QRegex.compile("\\s\\?\\s")
    val rustArms: Pattern = QRegex.compile("=>")
    val pythonDecisions: Pattern = QRegex.compile("\\b(if|elif|for|while|except|and|or|case)\\b")
    val keywordDecisions: Pattern = QRegex.compile(
        "\\b(if|elsif|elseif|unless|while|until|for|when|rescue|and|or|cond|case)\\b|&&|\\|\\|",
    )
    val pythonDef: Pattern = QRegex.compile("^(?:async\\s+)?def\\s+([A-Za-z_]\\w*)\\s*\\(")
    val pythonClass: Pattern = QRegex.compile("^class\\s+([A-Za-z_]\\w*)")
    val pythonBlock: Pattern = QRegex.compile(
        "^(if|elif|else|for|while|try|except|finally|with|match|case|async\\s+for|async\\s+with)\\b.*:$",
    )
    val keywordDef: Pattern = QRegex.compile("^\\s*(?:local\\s+)?(?:def|defp|defmacro|function)\\s+([A-Za-z_][\\w.:?!]*)")
    val keywordOpen: Pattern = QRegex.compile("\\b(if|unless|while|until|for|case|begin|do|function|def|defp|class|module)\\b")
    val keywordEnd: Pattern = QRegex.compile("\\bend\\b")
    val typeDeclaration: Pattern = QRegex.compile(
        "\\b(class|struct|interface|enum|protocol|record|object|actor|trait|typealias)\\s+([A-Z][A-Za-z0-9_]*)",
    )
    val packageDeclaration: Pattern = QRegex.compile("^\\s*(?:package|namespace)\\s+([A-Za-z_][\\w.]*)", multiline = true)
    val pureVirtual: Pattern = QRegex.compile("\\)\\s*(?:const\\s*)?=\\s*0\\s*;")
    val cInclude: Pattern = QRegex.compile("^\\s*#\\s*(?:include|import)\\s*([\"<])([^\">\\n]+)[\">]", multiline = true)
    val objcModule: Pattern = QRegex.compile("^\\s*@import\\s+([\\w.]+)", multiline = true)
    val jsImport: Pattern = QRegex.compile(
        "\\bimport\\s+(?:type\\s+)?(?:[\\w$*{}\\s,]+?\\s+from\\s+)?[\"']([^\"'\\n]+)[\"']",
    )
    val jsExport: Pattern = QRegex.compile(
        "\\bexport\\s+(?:type\\s+)?(?:\\*|\\{[^}]*\\}|\\*\\s+as\\s+\\w+)\\s+from\\s+[\"']([^\"'\\n]+)[\"']",
    )
    val jsRequire: Pattern = QRegex.compile("\\b(?:require|import)\\s*\\(\\s*[\"']([^\"'\\n]+)[\"']\\s*\\)")
    val pyImport: Pattern = QRegex.compile(
        "^[ \\t]*import[ \\t]+([\\w.]+(?:[ \\t]+as[ \\t]+\\w+)?(?:[ \\t]*,[ \\t]*[\\w.]+(?:[ \\t]+as[ \\t]+\\w+)?)*)",
        multiline = true,
    )
    val pyFrom: Pattern = QRegex.compile(
        "^[ \\t]*from[ \\t]+(\\.*[\\w.]*)[ \\t]+import[ \\t]+(\\([^)]*\\)|[^\\n]+)",
        multiline = true,
    )
    val goBlock: Pattern = QRegex.compile("\\bimport\\s*\\(([^)]*)\\)")
    val goSingle: Pattern = QRegex.compile("\\bimport\\s+(?:[\\w.]+\\s+)?\"([^\"\\n]+)\"")
    val quoted: Pattern = QRegex.compile("\"([^\"\\n]+)\"")
    val rustMod: Pattern = QRegex.compile("^\\s*(?:pub(?:\\([^)]*\\))?\\s+)?mod\\s+([A-Za-z_]\\w*)\\s*;", multiline = true)
    val rustUse: Pattern = QRegex.compile("\\buse\\s+((?:crate|super|self)(?:::[A-Za-z_]\\w*)+)")
    val jvmImport: Pattern = QRegex.compile("^\\s*import\\s+(?:static\\s+)?([A-Za-z_][\\w.]*)(\\.\\*)?", multiline = true)
    val csUsing: Pattern = QRegex.compile("^\\s*(?:global\\s+)?using\\s+(?:static\\s+)?([A-Za-z_][\\w.]*)\\s*;", multiline = true)
    val swiftImport: Pattern = QRegex.compile("^\\s*(?:@\\w+\\s+)*import\\s+(?:\\w+\\s+)?([A-Za-z_][\\w.]*)", multiline = true)
    val rubyRequire: Pattern = QRegex.compile("\\brequire(_relative)?\\s*\\(?\\s*[\"']([^\"'\\n]+)[\"']")
    val phpInclude: Pattern = QRegex.compile(
        "\\b(?:require|include)(?:_once)?\\s*\\(?\\s*(?:__DIR__\\s*\\.\\s*)?[\"']([^\"'\\n]+)[\"']",
    )
    val phpUse: Pattern = QRegex.compile("^\\s*use\\s+([A-Za-z_][\\w\\\\]*)", multiline = true)
    val dartImport: Pattern = QRegex.compile("^\\s*(?:import|export|part)\\s+[\"']([^\"'\\n]+)[\"']", multiline = true)
    val luaRequire: Pattern = QRegex.compile("\\brequire\\s*\\(?\\s*[\"']([^\"'\\n]+)[\"']")
    val cssImport: Pattern = QRegex.compile("@(?:import|use|forward)\\s+(?:url\\()?\\s*[\"']([^\"'\\n]+)[\"']")
    val qmlImport: Pattern = QRegex.compile("^\\s*import\\s+([A-Za-z_][\\w.]*|\"[^\"]+\")", multiline = true)
    val shellSource: Pattern = QRegex.compile(
        "(?:^|[;&|]\\s*|\\s)(?:source|\\.)\\s+[\"']?([\\w./\${}-]+\\.(?:sh|bash))",
        multiline = true,
    )
    val elixirAlias: Pattern = QRegex.compile("^\\s*(?:alias|import|use|require)\\s+([A-Z][\\w.]*)", multiline = true)
}

internal enum class HeaderKind { Block, Function, Type, Container }

internal class HeaderInfo(
    var kind: HeaderKind = HeaderKind.Block,
    var name: String = "",
    var parameters: Int = 0,
    var abstract: Boolean = false,
    var countedType: Boolean = false,
)

internal object Measure {
    const val WINDOW = 6
    private val GOLDEN = 0x9e3779b97f4a7c15UL.toLong()
    private val FNV_OFFSET = 0xcbf29ce484222325UL.toLong()
    private const val FNV_PRIME = 0x100000001b3L

    private val countedTypes = setOf("class", "struct", "interface", "enum", "union", "trait", "protocol", "record", "object", "actor")

    private val nonNameWords = setOf(
        "if", "for", "while", "switch", "catch", "return", "new", "throw", "await", "sizeof", "typeof",
        "delete", "case", "function", "func", "fn", "fun", "def", "async", "decltype", "alignof",
        "static_assert", "foreach", "using", "lock", "when", "match", "elif", "assert", "super", "this", "sub",
    )

    private val continuationEnds = listOf("(", ",", "=", ":", "->", "=>", "<", "&", "|", "+", ".", "[")
    private val continuationStarts = listOf(
        ",", ":", ".", "->", ")", "where", "throws", "const", "override", "noexcept", "final", "?", "&&", "||", "async", "->",
    )

    private val scriptLike = setOf("JavaScript", "TypeScript", "Vue", "Svelte")
    private val wrappers = setOf("(anonymous)", "define", "describe", "suite", "context", "$", "jQuery", "ready", "addEventListener")
    private val strictTernary = setOf("TypeScript", "Swift", "Kotlin", "C#", "Dart", "Vue", "Svelte", "Scala")
    private val looseTernary = setOf("C", "C++", "Objective-C", "Java", "JavaScript", "PHP", "Groovy", "Perl", "QML")
    private val decisionWords = setOf(
        "if", "for", "foreach", "while", "case", "catch", "guard", "when", "except", "elif", "elsif", "elseif", "unless", "until", "rescue",
    )
    private val typedLanguages = setOf("Swift", "C#", "Kotlin", "Java", "Scala", "Dart")

    private val boilerplatePrefixes = listOf(
        "import ", "#include", "#import", "using ", "from ", "package ", "require", "use ", "@import",
        "export {", "export *", "} from", "#pragma", "#define", "#endif", "#if", "#else", "namespace ", "module ",
    )

    private val generatedMarkers = listOf(
        "@generated", "do not edit", "auto-generated", "autogenerated", "generated by the protocol buffer", "code generated by",
    ).map { it.toByteArray(Charsets.US_ASCII) }

    fun stripSource(text: String, spec: LanguageSpec): Stripped {
        val n = text.length
        val nc = text.toCharArray()
        val cd = text.toCharArray()
        val python = spec.syntax == Syntax.Python
        val name = spec.name
        val backticks = name == "JavaScript" || name == "TypeScript" || name == "Vue" || name == "Svelte" || name == "Go" || name == "Shell"
        val rust = name == "Rust"
        val cFamily = name == "C" || name == "C++" || name == "Objective-C"
        val shell = name == "Shell" || name == "Perl"
        val elixir = spec.syntax == Syntax.Keyword && name == "Elixir"
        fun blank(buffer: CharArray, at: Int) {
            if (buffer[at] != '\n') buffer[at] = ' '
        }
        var i = 0
        while (i < n) {
            val c = text[i]
            val next = if (i + 1 < n) text[i + 1] else '\u0000'
            val lineComment = (spec.comments == Comments.CLike && c == '/' && next == '/') ||
                (spec.comments == Comments.Lua && c == '-' && next == '-') ||
                (
                    spec.comments == Comments.Hash && c == '#' &&
                        !(
                            shell && i > 0 &&
                                (text[i - 1] == '$' || text[i - 1] == '{' || !(QText.isSpace(text[i - 1]) || text[i - 1] == ';'))
                            )
                    )
            if (spec.comments == Comments.Lua && lineComment && i + 3 < n && text[i + 2] == '[' && text[i + 3] == '[') {
                val end = text.indexOf("]]", i + 4)
                val stop = if (end < 0) n else end + 2
                for (k in i until stop) {
                    blank(nc, k)
                    blank(cd, k)
                }
                i = stop
                continue
            }
            if (lineComment) {
                while (i < n && text[i] != '\n') {
                    blank(nc, i)
                    blank(cd, i)
                    i++
                }
                continue
            }
            if ((spec.comments == Comments.CLike || spec.comments == Comments.Css) && c == '/' && next == '*') {
                val end = text.indexOf("*/", i + 2)
                val stop = if (end < 0) n else end + 2
                for (k in i until stop) {
                    blank(nc, k)
                    blank(cd, k)
                }
                i = stop
                continue
            }
            val quote = c == '"' || c == '\'' || (backticks && c == '`')
            if (quote) {
                if (c == '\'') {
                    if (rust && i + 2 < n && (Character.isLetter(text[i + 1]) || text[i + 1] == '_') && text[i + 2] != '\'') {
                        i++
                        continue
                    }
                    if (cFamily && i > 0 && Character.isDigit(text[i - 1])) {
                        i++
                        continue
                    }
                    if (elixir) {
                        i++
                        continue
                    }
                }
                val triple = python && i + 2 < n && text[i + 1] == c && text[i + 2] == c
                val multiline = triple || c == '`'
                var k = i + if (triple) 3 else 1
                while (k < n) {
                    val ch = text[k]
                    if (ch == '\\') {
                        k += 2
                        continue
                    }
                    if (!multiline && ch == '\n') break
                    if (ch == c) {
                        if (!triple) break
                        if (k + 2 < n && text[k + 1] == c && text[k + 2] == c) break
                    }
                    k++
                }
                val contentStart = i + if (triple) 3 else 1
                val contentEnd = minOf(k, n)
                for (m in contentStart until contentEnd) blank(cd, m)
                i = minOf(n, contentEnd + if (k < n && text[k] == c) (if (triple) 3 else 1) else 0)
                continue
            }
            i++
        }
        if (cFamily) {
            var lineStart = 0
            while (lineStart < n) {
                var lineEnd = indexOf(cd, '\n', lineStart)
                if (lineEnd < 0) lineEnd = n
                var first = lineStart
                while (first < lineEnd && QText.isSpace(cd[first])) first++
                if (first < lineEnd && cd[first] == '#') {
                    var end = lineEnd
                    while (true) {
                        var last = end - 1
                        while (last > lineStart && QText.isSpace(cd[last])) last--
                        for (m in lineStart until end) blank(cd, m)
                        if (last >= lineStart && cd[last] == '\\' && end < n) {
                            lineStart = end + 1
                            end = indexOf(cd, '\n', lineStart)
                            if (end < 0) end = n
                            continue
                        }
                        break
                    }
                    lineEnd = end
                }
                lineStart = lineEnd + 1
            }
        }
        return Stripped(String(nc), String(cd))
    }

    private fun indexOf(buffer: CharArray, c: Char, from: Int): Int {
        for (i in from until buffer.size) if (buffer[i] == c) return i
        return -1
    }

    fun lastSegment(qualified: String): String {
        val cut = maxOf(qualified.lastIndexOf("::"), qualified.lastIndexOf('.'))
        if (cut < 0) return qualified
        return qualified.substring(cut + if (qualified[cut] == ':') 2 else 1)
    }

    fun matchingParen(text: String, open: Int): Int {
        var depth = 0
        for (i in open until text.length) {
            val c = text[i]
            if (c == '(') {
                depth++
            } else if (c == ')') {
                depth--
                if (depth == 0) return i
            }
        }
        return -1
    }

    fun countParameters(inside: String, python: Boolean): Int {
        val trimmed = QText.trimmed(inside)
        if (trimmed.isEmpty() || trimmed == "void") return 0
        var depth = 0
        val parts = ArrayList<String>()
        val current = StringBuilder()
        for (c in trimmed) {
            if (c == '(' || c == '<' || c == '[' || c == '{') {
                depth++
            } else if (c == ')' || c == '>' || c == ']' || c == '}') {
                depth = maxOf(0, depth - 1)
            }
            if (c == ',' && depth == 0) {
                parts.add(QText.trimmed(current.toString()))
                current.setLength(0)
                continue
            }
            current.append(c)
        }
        parts.add(QText.trimmed(current.toString()))
        var count = 0
        for ((i, part) in parts.withIndex()) {
            if (part.isEmpty()) continue
            if (python && i == 0 && (part == "self" || part == "cls")) continue
            if (python && (part == "*" || part == "/")) continue
            count++
        }
        return count
    }

    private fun parenBalance(text: String): Int {
        var balance = 0
        for (c in text) balance += if (c == '(') 1 else if (c == ')') -1 else 0
        return balance
    }

    fun cleanHeader(raw: String): String {
        var kept = ArrayList<String>()
        for (line in raw.split('\n')) {
            val t = QText.trimmed(line)
            if (t.isEmpty()) continue
            if (t.startsWith('#') || (t.startsWith('@') && !t.contains(' ') && !t.contains('('))) continue
            if (t.startsWith('[') && t.endsWith(']')) continue
            kept.add(t)
        }
        if (kept.size > 1) {
            var first = kept.size - 1
            var balance = parenBalance(kept[first])
            while (first > 0) {
                val above = kept[first - 1]
                val current = kept[first]
                var join = balance < 0
                if (!join) join = continuationEnds.any { above.endsWith(it) }
                if (!join) join = continuationStarts.any { current.startsWith(it) }
                if (!join) break
                first--
                balance += parenBalance(kept[first])
            }
            kept = ArrayList(kept.subList(first, kept.size))
        }
        var header = kept.joinToString(" ")
        while (header.startsWith('@')) {
            val space = header.indexOf(' ')
            val paren = header.indexOf('(')
            if (paren >= 0 && (space < 0 || paren < space)) {
                val close = matchingParen(header, paren)
                if (close < 0) break
                header = QText.trimmed(header.substring(close + 1))
            } else if (space > 0) {
                header = QText.trimmed(header.substring(space + 1))
            } else {
                break
            }
        }
        return header
    }

    private fun identifierPrefix(text: String): String {
        for (i in text.indices) {
            val c = text[i]
            if (!(c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c == '_')) return text.substring(0, i)
        }
        return text
    }

    fun classifyHeader(rawHeader: String, language: String): HeaderInfo {
        val info = HeaderInfo()
        val header = cleanHeader(rawHeader)
        if (header.isEmpty()) return info
        if (Patterns.controlStart.matcher(header).lookingAt()) return info
        val firstParen = header.indexOf('(')
        val beforeParen = if (firstParen < 0) header else header.substring(0, firstParen)
        val keywordFunction = Patterns.functionKeyword.found(header)
        val type = Patterns.typeHeader.firstMatch(beforeParen)
        if (type != null && !keywordFunction) {
            val keyword = type.group(1)
            val counted = keyword in countedTypes
            info.kind = if (counted) HeaderKind.Type else HeaderKind.Container
            info.countedType = counted
            info.abstract = keyword == "interface" || keyword == "protocol" || keyword == "trait" || header.contains("abstract ")
            info.name = identifierPrefix(QText.trimmed(beforeParen.substring(type.end(1))))
            return info
        }
        var function = keywordFunction
        if (!function && header.endsWith("=>")) function = true
        if (!function && (language == "Swift" || language == "Kotlin")) {
            val computed = Patterns.computedProperty.firstMatch(header)
            if (computed != null) {
                info.kind = HeaderKind.Function
                info.name = computed.group(1)
                return info
            }
        }
        if (!function && firstParen > 0) {
            val lastClose = header.lastIndexOf(')')
            if (lastClose > firstParen) {
                val tail = header.substring(lastClose + 1)
                var depth = 0
                var assignment = false
                val operatorAt = header.indexOf("operator")
                for (k in 0 until firstParen) {
                    val c = header[k]
                    if (c == '<' || c == '[') {
                        depth++
                    } else if (c == '>' || c == ']') {
                        depth = maxOf(0, depth - 1)
                    } else if (c == '=' && depth == 0) {
                        val prev = if (k > 0) header[k - 1] else '\u0000'
                        val nxt = if (k + 1 < header.length) header[k + 1] else '\u0000'
                        val operatorBefore = operatorAt >= 0 && operatorAt + 8 <= k
                        if (nxt != '=' && prev != '=' && prev != '!' && prev != '<' && prev != '>' && !operatorBefore) assignment = true
                    }
                }
                val tailOk = Patterns.tailAfterParen.found(tail) || QText.trimmed(tail).startsWith(':')
                if (!assignment && tailOk) {
                    val call = Patterns.callName.firstMatch(header.substring(0, firstParen + 1))
                    if (call != null && lastSegment(call.group(1)) !in nonNameWords) function = true
                }
            }
        }
        if (!function) return info
        info.kind = HeaderKind.Function
        val it = Patterns.callName.matcher(header)
        var paramsOpen = -1
        while (it.find()) {
            val candidate = it.group(1)
            if (lastSegment(candidate) in nonNameWords || candidate in nonNameWords) continue
            info.name = candidate
            paramsOpen = it.end(0) - 1
            break
        }
        if (info.name.isEmpty()) {
            val arrow = Patterns.arrowName.firstMatch(header)
            if (arrow != null) info.name = arrow.group(1)
        }
        if (paramsOpen < 0) paramsOpen = header.indexOf('(')
        if (paramsOpen >= 0) {
            val close = matchingParen(header, paramsOpen)
            if (close > paramsOpen) info.parameters = countParameters(header.substring(paramsOpen + 1, close), false)
        }
        if (info.name.isEmpty()) info.name = "(anonymous)"
        if (language in scriptLike && lastSegment(info.name) in wrappers) info.kind = HeaderKind.Container
        return info
    }

    private fun isWord(c: Char): Boolean = c in 'a'..'z' || c in 'A'..'Z' || c in '0'..'9' || c == '_'

    private fun scanDecisions(code: String, positions: MutableList<Int>) {
        val n = code.length
        var i = 0
        while (i < n) {
            val c = code[i]
            if ((c == '&' || c == '|') && i + 1 < n && code[i + 1] == c) {
                positions.add(i)
                i += 2
                continue
            }
            if (isWord(c)) {
                var end = i + 1
                while (end < n && isWord(code[end])) end++
                val length = end - i
                if (length in 2..7 && code[i] in 'a'..'w' && code.substring(i, end) in decisionWords) positions.add(i)
                i = end
                continue
            }
            i++
        }
    }

    private fun capitalizedWords(code: String, declared: Set<String>, limit: Int): Set<String> {
        val referenced = HashSet<String>()
        val n = code.length
        var i = 0
        while (i < n && referenced.size < limit) {
            if (!isWord(code[i])) {
                i++
                continue
            }
            var end = i + 1
            while (end < n && isWord(code[end])) end++
            if (code[i] in 'A'..'Z' && end - i >= 3) {
                val name = code.substring(i, end)
                if (name !in declared) referenced.add(name)
            }
            i = end
        }
        return referenced
    }

    private fun decisionPositions(code: String, language: String): IntArray {
        val positions = ArrayList<Int>()
        scanDecisions(code, positions)
        if (language in strictTernary) {
            val t = Patterns.ternaryStrict.matcher(code)
            while (t.find()) positions.add(t.start())
        } else if (language in looseTernary) {
            for (i in code.indices) {
                if (code[i] != '?') continue
                val next = if (i + 1 < code.length) code[i + 1] else '\u0000'
                val prev = if (i > 0) code[i - 1] else '\u0000'
                if (next == '?' || next == '.' || next == ':' || prev == '?' || next == '>' || next == ')' || next == ',') continue
                positions.add(i)
            }
        } else if (language == "Rust") {
            val t = Patterns.rustArms.matcher(code)
            while (t.find()) positions.add(t.start())
        }
        val result = positions.toIntArray()
        result.sort()
        return result
    }

    private class LineStarts(text: String) {
        val starts: IntArray

        init {
            var count = 1
            for (c in text) if (c == '\n') count++
            starts = IntArray(count)
            var at = 1
            for (i in text.indices) if (text[i] == '\n') starts[at++] = i + 1
        }

        fun lineAt(position: Int): Int {
            var lo = 0
            var hi = starts.size
            while (lo < hi) {
                val mid = (lo + hi) ushr 1
                if (starts[mid] <= position) lo = mid + 1 else hi = mid
            }
            return lo
        }
    }

    private fun finishTotals(info: InsightFile, functions: List<InsightFunction>, fileLevel: Int) {
        var total = fileLevel
        for (fn in functions) {
            total += fn.complexity
            info.maxComplexity = maxOf(info.maxComplexity, fn.complexity)
            info.maxNesting = maxOf(info.maxNesting, fn.nesting)
        }
        info.complexity = total
        info.functions = functions
    }

    private fun analyzeBraces(info: InsightFile, stripped: Stripped) {
        val code = stripped.code
        val lines = LineStarts(code)
        val functions = ArrayList<InsightFunction>()
        val stackKind = ArrayList<HeaderKind>()
        val stackFunction = ArrayList<Int>()
        val bodyStart = ArrayList<Int>()
        val bodyEnd = ArrayList<Int>()
        var headerStart = 0
        var functionDepth = -1
        var currentFunction = -1
        var parenDepth = 0
        for (i in code.indices) {
            val c = code[i]
            if (c == '(') {
                parenDepth++
            } else if (c == ')') {
                parenDepth = maxOf(0, parenDepth - 1)
            } else if (c == ';' && parenDepth == 0) {
                headerStart = i + 1
            } else if (c == '{') {
                if (currentFunction >= 0) {
                    stackKind.add(HeaderKind.Block)
                    stackFunction.add(currentFunction)
                    val nesting = stackKind.size - functionDepth - 1
                    val fn = functions[currentFunction]
                    fn.nesting = maxOf(fn.nesting, nesting)
                } else {
                    val header = code.substring(headerStart, i)
                    val kind = classifyHeader(header, info.language)
                    if (kind.kind == HeaderKind.Function) {
                        val fn = InsightFunction(name = kind.name)
                        var first = headerStart
                        while (first < i && QText.isSpace(code[first])) first++
                        val cleaned = cleanHeader(header)
                        if (cleaned.isNotEmpty()) {
                            val at = code.indexOf(QText.left(cleaned, 12), first)
                            if (at in 0 until i) first = at
                        }
                        fn.line = lines.lineAt(first)
                        fn.parameters = kind.parameters
                        functions.add(fn)
                        bodyStart.add(i + 1)
                        bodyEnd.add(i + 1)
                        currentFunction = functions.size - 1
                        stackKind.add(HeaderKind.Function)
                        stackFunction.add(currentFunction)
                        functionDepth = stackKind.size - 1
                    } else {
                        if (kind.kind == HeaderKind.Type) {
                            if (kind.countedType) info.types++
                            if (kind.abstract) info.abstractTypes++
                        }
                        stackKind.add(kind.kind)
                        stackFunction.add(-1)
                    }
                }
                headerStart = i + 1
                parenDepth = 0
            } else if (c == '}') {
                if (stackKind.isNotEmpty()) {
                    val frameKind = stackKind.removeAt(stackKind.size - 1)
                    val frameFunction = stackFunction.removeAt(stackFunction.size - 1)
                    if (frameKind == HeaderKind.Function && stackKind.size == functionDepth) {
                        val fn = functions[frameFunction]
                        bodyEnd[frameFunction] = i
                        fn.length = maxOf(1, lines.lineAt(i) - fn.line + 1)
                        currentFunction = -1
                        functionDepth = -1
                    }
                }
                headerStart = i + 1
                parenDepth = 0
            }
        }
        if (currentFunction >= 0) {
            val fn = functions[currentFunction]
            bodyEnd[currentFunction] = code.length
            fn.length = maxOf(1, lines.lineAt(code.length - 1) - fn.line + 1)
        }
        var fileLevel = 0
        for (position in decisionPositions(code, info.language)) {
            var lo = 0
            var hi = bodyStart.size - 1
            var found = -1
            while (lo <= hi) {
                val mid = (lo + hi) / 2
                if (bodyStart[mid] <= position) {
                    found = mid
                    lo = mid + 1
                } else {
                    hi = mid - 1
                }
            }
            if (found >= 0 && position < bodyEnd[found]) functions[found].complexity++ else fileLevel++
        }
        finishTotals(info, functions, fileLevel)
        if (info.types > 0 && info.abstractTypes == 0 && (info.language == "C++" || info.language == "C") &&
            Patterns.pureVirtual.found(code)
        ) info.abstractTypes = 1
    }

    private fun indentOf(line: String): Int {
        var indent = 0
        for (c in line) {
            if (c == ' ') indent++ else if (c == '\t') indent += 4 else break
        }
        return indent
    }

    private fun analyzePython(info: InsightFile, stripped: Stripped) {
        val codeLines = stripped.code.split('\n')
        val textLines = stripped.noComments.split('\n')
        val functions = ArrayList<InsightFunction>()
        var current = -1
        var fnIndent = 0
        var lastCodeLine = 0
        val blocks = ArrayList<Int>()
        var fileLevel = 0
        fun close(endLine: Int) {
            if (current < 0) return
            val fn = functions[current]
            fn.length = maxOf(1, endLine - fn.line + 1)
            current = -1
            blocks.clear()
        }
        var continuation = 0
        for (i in codeLines.indices) {
            val line = codeLines[i]
            val trimmed = QText.trimmed(line)
            if (trimmed.isEmpty()) continue
            val continued = continuation > 0
            for (c in trimmed) {
                if (c == '(' || c == '[' || c == '{') {
                    continuation++
                } else if (c == ')' || c == ']' || c == '}') {
                    continuation = maxOf(0, continuation - 1)
                }
            }
            val lineNo = i + 1
            val indent = indentOf(line)
            if (!continued && current >= 0 && indent <= fnIndent) close(lastCodeLine)
            if (!continued) {
                val def = Patterns.pythonDef.firstMatch(trimmed)
                if (def != null && current < 0) {
                    val fn = InsightFunction(name = def.group(1), line = lineNo)
                    val source = if (i < textLines.size) textLines.subList(i, minOf(textLines.size, i + 30)).joinToString("\n") else ""
                    val open = source.indexOf('(')
                    if (open >= 0) {
                        val closeAt = matchingParen(source, open)
                        if (closeAt > open) fn.parameters = countParameters(source.substring(open + 1, closeAt), true)
                    }
                    functions.add(fn)
                    current = functions.size - 1
                    fnIndent = indent
                    lastCodeLine = lineNo
                    continue
                }
                if (Patterns.pythonClass.found(trimmed)) {
                    info.types++
                    if (trimmed.contains("ABC") || trimmed.contains("Protocol") || trimmed.contains("ABCMeta")) info.abstractTypes++
                }
            }
            val decisions = Patterns.pythonDecisions.countIn(trimmed)
            if (current >= 0) {
                val fn = functions[current]
                fn.complexity += decisions
                if (!continued) {
                    while (blocks.isNotEmpty() && blocks.last() >= indent) blocks.removeAt(blocks.size - 1)
                    fn.nesting = maxOf(fn.nesting, blocks.size)
                    if (Patterns.pythonBlock.found(trimmed)) blocks.add(indent)
                }
            } else {
                fileLevel += decisions
            }
            lastCodeLine = lineNo
        }
        close(lastCodeLine)
        finishTotals(info, functions, fileLevel)
    }

    private fun analyzeKeyword(info: InsightFile, stripped: Stripped) {
        val codeLines = stripped.code.split('\n')
        val functions = ArrayList<InsightFunction>()
        var current = -1
        var baseDepth = 0
        var depth = 0
        var fileLevel = 0
        var lastCodeLine = 0
        for (i in codeLines.indices) {
            val trimmed = QText.trimmed(codeLines[i])
            if (trimmed.isEmpty()) continue
            val def = Patterns.keywordDef.firstMatch(trimmed)
            if (def != null) {
                if (current >= 0) functions[current].length = maxOf(1, lastCodeLine - functions[current].line + 1)
                val fn = InsightFunction(name = def.group(1), line = i + 1)
                val open = trimmed.indexOf('(')
                if (open >= 0) {
                    val closeAt = matchingParen(trimmed, open)
                    if (closeAt > open) fn.parameters = countParameters(trimmed.substring(open + 1, closeAt), false)
                }
                functions.add(fn)
                current = functions.size - 1
                baseDepth = depth
            }
            if (trimmed.startsWith("class ") || trimmed.startsWith("module ") || trimmed.startsWith("defmodule ")) info.types++
            val opens = Patterns.keywordOpen.countIn(trimmed) - if (trimmed.contains(" if ") && !trimmed.startsWith("if")) 1 else 0
            val ends = Patterns.keywordEnd.countIn(trimmed)
            depth = maxOf(0, depth + opens - ends)
            val decisions = Patterns.keywordDecisions.countIn(trimmed)
            if (current >= 0) {
                val fn = functions[current]
                fn.complexity += decisions
                fn.nesting = maxOf(fn.nesting, maxOf(0, depth - baseDepth - 1))
            } else {
                fileLevel += decisions
            }
            lastCodeLine = i + 1
        }
        if (current >= 0) functions[current].length = maxOf(1, lastCodeLine - functions[current].line + 1)
        finishTotals(info, functions, fileLevel)
    }

    fun importsFromStripped(language: String, text: String): List<String> {
        val specs = ArrayList<String>()
        fun collect(pattern: Pattern, source: String, group: Int) {
            val m = pattern.matcher(source)
            while (m.find()) {
                val value = QText.trimmed(m.groupOrEmpty(group))
                if (value.isNotEmpty()) specs.add(value)
            }
        }
        when (language) {
            "C", "C++", "Objective-C" -> {
                val m = Patterns.cInclude.matcher(text)
                while (m.find()) {
                    val path = QText.trimmed(m.group(2))
                    specs.add(if (m.group(1) == "<") "<$path>" else path)
                }
                if (language == "Objective-C") {
                    val mm = Patterns.objcModule.matcher(text)
                    while (mm.find()) specs.add("<" + mm.group(1) + ">")
                }
            }
            "JavaScript", "TypeScript", "Vue", "Svelte" -> {
                collect(Patterns.jsImport, text, 1)
                collect(Patterns.jsExport, text, 1)
                collect(Patterns.jsRequire, text, 1)
            }
            "Python" -> {
                val m = Patterns.pyImport.matcher(text)
                while (m.find()) {
                    for (part in m.group(1).split(',')) {
                        val module = QText.firstToken(QText.trimmed(part))
                        if (module.isNotEmpty()) specs.add(module)
                    }
                }
                val from = Patterns.pyFrom.matcher(text)
                while (from.find()) {
                    val module = from.group(1)
                    specs.add(module)
                    val names = from.group(2).replace("(", "").replace(")", "").replace("\\", "")
                    for (raw in names.split(',')) {
                        val name = QText.firstToken(QText.trimmed(raw))
                        if (name.isEmpty() || name == "*") continue
                        val joined = if (module.endsWith('.')) module + name else "$module.$name"
                        specs.add("?$joined")
                    }
                }
            }
            "Go" -> {
                val m = Patterns.goBlock.matcher(text)
                while (m.find()) collect(Patterns.quoted, m.group(1), 1)
                collect(Patterns.goSingle, text, 1)
            }
            "Rust" -> {
                val m = Patterns.rustMod.matcher(text)
                while (m.find()) specs.add("mod " + m.group(1))
                collect(Patterns.rustUse, text, 1)
            }
            "Java", "Kotlin", "Scala", "Groovy" -> {
                val m = Patterns.jvmImport.matcher(text)
                while (m.find()) specs.add(m.group(1) + if (m.groupOrEmpty(2).isEmpty()) "" else ".*")
            }
            "C#" -> collect(Patterns.csUsing, text, 1)
            "Swift" -> collect(Patterns.swiftImport, text, 1)
            "Ruby" -> {
                val m = Patterns.rubyRequire.matcher(text)
                while (m.find()) specs.add(if (m.groupOrEmpty(1).isEmpty()) m.group(2) else "./" + m.group(2))
            }
            "Elixir" -> collect(Patterns.elixirAlias, text, 1)
            "PHP" -> {
                collect(Patterns.phpInclude, text, 1)
                collect(Patterns.phpUse, text, 1)
            }
            "Dart" -> collect(Patterns.dartImport, text, 1)
            "Lua" -> collect(Patterns.luaRequire, text, 1)
            "CSS", "SCSS", "Less" -> collect(Patterns.cssImport, text, 1)
            "QML" -> {
                val m = Patterns.qmlImport.matcher(text)
                while (m.find()) {
                    var value = m.group(1)
                    if (value.startsWith('"')) value = "./" + value.substring(1, value.length - 1)
                    specs.add(value)
                }
            }
            "Shell" -> collect(Patterns.shellSource, text, 1)
        }
        return LinkedHashSet(specs).toList()
    }

    private fun boilerplateLine(line: String): Boolean = boilerplatePrefixes.any { line.startsWith(it) }

    private fun lineHash(text: String): Long {
        var h = FNV_OFFSET
        for (c in text) h = (h xor c.code.toLong()) * FNV_PRIME
        h = (h xor (h ushr 30)) * 0xbf58476d1ce4e5b9UL.toLong()
        h = (h xor (h ushr 27)) * 0x94d049bb133111ebUL.toLong()
        return h xor (h ushr 31)
    }

    private fun computeWindows(noComments: String): Pair<IntArray, LongArray> {
        val lines = noComments.split('\n')
        val significant = ArrayList<Int>()
        val hashes = ArrayList<Long>()
        val buffer = StringBuilder()
        for (i in lines.indices) {
            val normalized = QText.simplified(lines[i], buffer)
            if (normalized.length < 4 || boilerplateLine(normalized)) continue
            if (normalized.none { QText.isLetterOrNumber(it) }) continue
            significant.add(i + 1)
            hashes.add(lineHash(normalized) * GOLDEN)
        }
        val count = maxOf(0, hashes.size - WINDOW + 1)
        val windows = LongArray(count)
        for (k in 0 until count) {
            var h = FNV_OFFSET
            for (j in 0 until WINDOW) {
                h = h xor (hashes[k + j] + GOLDEN + (h shl 6) + (h ushr 2))
                h *= FNV_PRIME
            }
            windows[k] = h
        }
        return significant.toIntArray() to windows
    }

    private fun collectSymbols(info: InsightFile, stripped: Stripped) {
        if (info.language !in typedLanguages) return
        val declared = HashSet<String>()
        val declares = ArrayList<String>()
        val it = Patterns.typeDeclaration.matcher(stripped.code)
        while (it.find()) {
            val name = it.group(2)
            if (declared.add(name)) declares.add(name)
        }
        val pkg = Patterns.packageDeclaration.firstMatch(stripped.code)
        if (pkg != null) declares.add("package:" + pkg.group(1))
        info.declares = declares
        info.references = capitalizedWords(stripped.code, declared, 800).sorted()
    }

    fun generatedText(bytes: ByteArray): Boolean {
        val n = minOf(bytes.size, 2048)
        val head = ByteArray(n) { val b = bytes[it]; if (b in 'A'.code.toByte()..'Z'.code.toByte()) (b + 32).toByte() else b }
        return generatedMarkers.any { contains(head, it) }
    }

    private fun contains(haystack: ByteArray, needle: ByteArray): Boolean {
        outer@ for (i in 0..haystack.size - needle.size) {
            for (j in needle.indices) if (haystack[i + j] != needle[j]) continue@outer
            return true
        }
        return false
    }

    fun binaryText(bytes: ByteArray): Boolean {
        val n = minOf(bytes.size, 8000)
        for (i in 0 until n) if (bytes[i] == 0.toByte()) return true
        return false
    }

    fun minifiedText(bytes: ByteArray): Boolean {
        if (bytes.size <= 20000) return false
        var newlines = 0
        for (b in bytes) if (b == '\n'.code.toByte()) newlines++
        return newlines < bytes.size / 400
    }

    fun measure(path: String, bytes: ByteArray, withWindows: Boolean): Measured {
        val spec = Languages.specFor(path)
        val info = InsightFile(path = path, language = spec.name, bytes = bytes.size.toLong(), test = Languages.isTestPath(path))
        val text = String(bytes, Charsets.UTF_8)
        val stripped = stripSource(text, spec)
        val nc = stripped.noComments
        val code = stripped.code
        var lineStart = 0
        var lineCount = 0
        while (lineStart <= text.length) {
            var lineEnd = text.indexOf('\n', lineStart)
            val last = lineEnd < 0
            if (last) lineEnd = text.length
            if (last && lineStart == lineEnd) break
            lineCount++
            if (QText.isBlank(text, lineStart, lineEnd)) {
                info.blankLines++
            } else if (QText.isBlank(nc, lineStart, lineEnd)) {
                info.commentLines++
            } else if (spec.syntax == Syntax.Python && quotesOnly(code, lineStart, lineEnd)) {
                info.commentLines++
            } else {
                info.codeLines++
            }
            if (last) break
            lineStart = lineEnd + 1
        }
        info.lines = lineCount
        when (spec.syntax) {
            Syntax.Braces -> analyzeBraces(info, stripped)
            Syntax.Python -> analyzePython(info, stripped)
            Syntax.Keyword -> analyzeKeyword(info, stripped)
            Syntax.Css -> info.complexity = 0
        }
        info.imports = importsFromStripped(spec.name, nc)
        collectSymbols(info, stripped)
        if (withWindows && spec.syntax != Syntax.Css) {
            val (significant, windows) = computeWindows(nc)
            return Measured(info, significant, windows)
        }
        return Measured(info)
    }

    private fun quotesOnly(code: String, start: Int, end: Int): Boolean {
        var any = false
        for (i in start until end) {
            val c = code[i]
            if (QText.isSpace(c)) continue
            if (c != '"' && c != '\'') return false
            any = true
        }
        return any
    }
}

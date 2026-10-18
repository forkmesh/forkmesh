package com.forkmesh.nativeapp.codeinsight

internal enum class Syntax { Braces, Python, Keyword, Css }

internal enum class Comments { CLike, Hash, Lua, Css }

internal data class LanguageSpec(
    val name: String = "",
    val syntax: Syntax = Syntax.Braces,
    val comments: Comments = Comments.CLike,
)

internal object Languages {
    val none = LanguageSpec()

    val table: Map<String, LanguageSpec> = HashMap<String, LanguageSpec>().apply {
        fun add(extensions: List<String>, name: String, syntax: Syntax, comments: Comments) {
            val spec = LanguageSpec(name, syntax, comments)
            for (extension in extensions) put(extension, spec)
        }
        add(listOf("c", "h"), "C", Syntax.Braces, Comments.CLike)
        add(listOf("cc", "cpp", "cxx", "c++", "hpp", "hh", "hxx", "ipp", "inl", "tpp"), "C++", Syntax.Braces, Comments.CLike)
        add(listOf("m", "mm"), "Objective-C", Syntax.Braces, Comments.CLike)
        add(listOf("js", "mjs", "cjs", "jsx"), "JavaScript", Syntax.Braces, Comments.CLike)
        add(listOf("ts", "tsx", "mts", "cts"), "TypeScript", Syntax.Braces, Comments.CLike)
        add(listOf("vue"), "Vue", Syntax.Braces, Comments.CLike)
        add(listOf("svelte"), "Svelte", Syntax.Braces, Comments.CLike)
        add(listOf("py", "pyw", "pyi"), "Python", Syntax.Python, Comments.Hash)
        add(listOf("go"), "Go", Syntax.Braces, Comments.CLike)
        add(listOf("rs"), "Rust", Syntax.Braces, Comments.CLike)
        add(listOf("java"), "Java", Syntax.Braces, Comments.CLike)
        add(listOf("kt", "kts"), "Kotlin", Syntax.Braces, Comments.CLike)
        add(listOf("swift"), "Swift", Syntax.Braces, Comments.CLike)
        add(listOf("cs"), "C#", Syntax.Braces, Comments.CLike)
        add(listOf("php"), "PHP", Syntax.Braces, Comments.CLike)
        add(listOf("dart"), "Dart", Syntax.Braces, Comments.CLike)
        add(listOf("scala", "sc"), "Scala", Syntax.Braces, Comments.CLike)
        add(listOf("groovy", "gradle"), "Groovy", Syntax.Braces, Comments.CLike)
        add(listOf("qml"), "QML", Syntax.Braces, Comments.CLike)
        add(listOf("sh", "bash", "zsh"), "Shell", Syntax.Braces, Comments.Hash)
        add(listOf("pl", "pm"), "Perl", Syntax.Braces, Comments.Hash)
        add(listOf("rb", "rake"), "Ruby", Syntax.Keyword, Comments.Hash)
        add(listOf("ex", "exs"), "Elixir", Syntax.Keyword, Comments.Hash)
        add(listOf("lua"), "Lua", Syntax.Keyword, Comments.Lua)
        add(listOf("css"), "CSS", Syntax.Css, Comments.Css)
        add(listOf("scss", "sass"), "SCSS", Syntax.Css, Comments.CLike)
        add(listOf("less"), "Less", Syntax.Css, Comments.CLike)
    }

    val byName: Map<String, LanguageSpec> = table.values.associateBy { it.name }

    private val vendoredSegments = setOf(
        "node_modules", "vendor", "vendors", "third_party", "thirdparty", "third-party",
        "bower_components", "pods", "carthage", "deriveddata", ".git", "__pycache__", ".venv", "venv",
        "site-packages", ".tox", ".dart_tool", "godeps", ".next", ".nuxt", ".gradle", ".idea", ".build",
        "_build", ".cache", "jspm_packages", ".yarn", ".pnpm-store",
    )

    private val buildSegments = setOf("build", "dist", "out", "target", "coverage", "bin", "obj")

    private val testSegments = setOf(
        "test", "tests", "__tests__", "spec", "specs", "testing", "androidtest", "uitests", "e2e",
        "__mocks__", "fixtures", "testdata", "browser_tests",
    )

    private val vendoredSuffixes = listOf(
        ".min.js", ".min.css", ".bundle.js", ".pb.go", "_pb2.py", "_pb2_grpc.py", ".g.dart",
        ".freezed.dart", ".designer.cs", ".pb.h", ".pb.cc", ".pb.swift", ".chunk.js", "-min.js",
    )

    private val typedTest = QRegex.compile("[a-z0-9]Tests?\\.(swift|kt|java|cs|m|mm|scala|dart)$")

    fun extensionOf(path: String): String {
        val slash = path.lastIndexOf('/')
        val dot = path.lastIndexOf('.')
        if (dot <= slash + 1) return ""
        return path.substring(dot + 1).lowercase()
    }

    fun specFor(path: String): LanguageSpec = table[extensionOf(path)] ?: none

    fun isTestPath(path: String): Boolean {
        val segments = path.split('/')
        for (i in 0 until segments.size - 1) {
            val segment = segments[i].lowercase()
            if (segment in testSegments || segment.endsWith("tests")) return true
        }
        val name = segments.last()
        val lower = name.lowercase()
        if (lower.startsWith("test_") || lower.contains("_test.") || lower.contains(".test.") ||
            lower.contains(".spec.") || lower.contains("_spec.") || lower.startsWith("conftest.")
        ) return true
        return typedTest.found(name)
    }

    fun isVendoredPath(path: String): Boolean {
        val segments = path.split('/')
        for (i in 0 until segments.size - 1) if (segments[i].lowercase() in vendoredSegments) return true
        val name = segments.last().lowercase()
        for (suffix in vendoredSuffixes) if (name.endsWith(suffix)) return true
        return name.contains(".generated.") || name == "package-lock.json"
    }

    fun isSkippedFolderPath(path: String): Boolean {
        val segments = path.split('/')
        for (i in 0 until segments.size - 1) {
            val segment = segments[i].lowercase()
            if (segment.startsWith('.') || segment in buildSegments || segment in vendoredSegments) return true
        }
        return false
    }
}

internal object Paths {
    fun dirOf(path: String): String {
        val slash = path.lastIndexOf('/')
        return if (slash < 0) "" else path.substring(0, slash)
    }

    fun fileNameOf(path: String): String {
        val slash = path.lastIndexOf('/')
        return if (slash < 0) path else path.substring(slash + 1)
    }

    fun stemOf(path: String): String {
        val name = fileNameOf(path)
        val dot = name.lastIndexOf('.')
        return if (dot <= 0) name else name.substring(0, dot)
    }

    fun withoutExtension(path: String): String {
        val slash = path.lastIndexOf('/')
        val dot = path.lastIndexOf('.')
        return if (dot <= slash + 1) path else path.substring(0, dot)
    }

    fun join(dir: String, rest: String): String {
        if (dir.isEmpty()) return rest
        if (rest.isEmpty()) return dir
        return "$dir/$rest"
    }

    fun normalized(path: String): String {
        if (path.isEmpty()) return ""
        var clean = QText.cleanPath(path)
        while (clean.startsWith("./")) clean = clean.substring(2)
        if (clean == "." || clean.startsWith("../") || clean == ".." || clean.startsWith('/')) return ""
        return clean
    }

    fun commonPrefix(a: String, b: String): Int {
        val n = minOf(a.length, b.length)
        var i = 0
        while (i < n && a[i] == b[i]) i++
        return i
    }
}

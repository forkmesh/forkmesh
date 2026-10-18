import Foundation

enum Syntax: Sendable { case braces, python, keyword, css }
enum Comments: Sendable { case cLike, hash, lua, css }

struct LanguageSpec: Sendable {
    var name = ""
    var syntax = Syntax.braces
    var comments = Comments.cLike
}

enum Languages {
    static let ordered: [(extensions: [String], spec: LanguageSpec)] = [
        (["c", "h"], LanguageSpec(name: "C", syntax: .braces, comments: .cLike)),
        (["cc", "cpp", "cxx", "c++", "hpp", "hh", "hxx", "ipp", "inl", "tpp"], LanguageSpec(name: "C++", syntax: .braces, comments: .cLike)),
        (["m", "mm"], LanguageSpec(name: "Objective-C", syntax: .braces, comments: .cLike)),
        (["js", "mjs", "cjs", "jsx"], LanguageSpec(name: "JavaScript", syntax: .braces, comments: .cLike)),
        (["ts", "tsx", "mts", "cts"], LanguageSpec(name: "TypeScript", syntax: .braces, comments: .cLike)),
        (["vue"], LanguageSpec(name: "Vue", syntax: .braces, comments: .cLike)),
        (["svelte"], LanguageSpec(name: "Svelte", syntax: .braces, comments: .cLike)),
        (["py", "pyw", "pyi"], LanguageSpec(name: "Python", syntax: .python, comments: .hash)),
        (["go"], LanguageSpec(name: "Go", syntax: .braces, comments: .cLike)),
        (["rs"], LanguageSpec(name: "Rust", syntax: .braces, comments: .cLike)),
        (["java"], LanguageSpec(name: "Java", syntax: .braces, comments: .cLike)),
        (["kt", "kts"], LanguageSpec(name: "Kotlin", syntax: .braces, comments: .cLike)),
        (["swift"], LanguageSpec(name: "Swift", syntax: .braces, comments: .cLike)),
        (["cs"], LanguageSpec(name: "C#", syntax: .braces, comments: .cLike)),
        (["php"], LanguageSpec(name: "PHP", syntax: .braces, comments: .cLike)),
        (["dart"], LanguageSpec(name: "Dart", syntax: .braces, comments: .cLike)),
        (["scala", "sc"], LanguageSpec(name: "Scala", syntax: .braces, comments: .cLike)),
        (["groovy", "gradle"], LanguageSpec(name: "Groovy", syntax: .braces, comments: .cLike)),
        (["qml"], LanguageSpec(name: "QML", syntax: .braces, comments: .cLike)),
        (["sh", "bash", "zsh"], LanguageSpec(name: "Shell", syntax: .braces, comments: .hash)),
        (["pl", "pm"], LanguageSpec(name: "Perl", syntax: .braces, comments: .hash)),
        (["rb", "rake"], LanguageSpec(name: "Ruby", syntax: .keyword, comments: .hash)),
        (["ex", "exs"], LanguageSpec(name: "Elixir", syntax: .keyword, comments: .hash)),
        (["lua"], LanguageSpec(name: "Lua", syntax: .keyword, comments: .lua)),
        (["css"], LanguageSpec(name: "CSS", syntax: .css, comments: .css)),
        (["scss", "sass"], LanguageSpec(name: "SCSS", syntax: .css, comments: .cLike)),
        (["less"], LanguageSpec(name: "Less", syntax: .css, comments: .cLike)),
    ]

    static let table: [String: LanguageSpec] = {
        var table: [String: LanguageSpec] = [:]
        for entry in ordered {
            for ext in entry.extensions { table[ext] = entry.spec }
        }
        return table
    }()

    static let byName: [String: LanguageSpec] = {
        var names: [String: LanguageSpec] = [:]
        for entry in ordered { names[entry.spec.name] = entry.spec }
        return names
    }()

    static func spec(for path: String) -> LanguageSpec {
        table[PathText.extensionOf(path)] ?? LanguageSpec()
    }

    static let testSegments: Set<String> = [
        "test", "tests", "__tests__", "spec", "specs", "testing", "androidtest", "uitests", "e2e",
        "__mocks__", "fixtures", "testdata", "browser_tests",
    ]

    static let vendoredSegments: Set<String> = [
        "node_modules", "vendor", "vendors", "third_party", "thirdparty", "third-party", "bower_components",
        "pods", "carthage", "deriveddata", ".git", "__pycache__", ".venv", "venv", "site-packages", ".tox",
        ".dart_tool", "godeps", ".next", ".nuxt", ".gradle", ".idea", ".build", "_build", ".cache",
        "jspm_packages", ".yarn", ".pnpm-store",
    ]

    static let buildSegments: Set<String> = ["build", "dist", "out", "target", "coverage", "bin", "obj"]

    static let vendoredSuffixes = [
        ".min.js", ".min.css", ".bundle.js", ".pb.go", "_pb2.py", "_pb2_grpc.py", ".g.dart", ".freezed.dart",
        ".designer.cs", ".pb.h", ".pb.cc", ".pb.swift", ".chunk.js", "-min.js",
    ]

    static let typedTest = InsightRegex("[a-z0-9]Tests?\\.(swift|kt|java|cs|m|mm|scala|dart)$")

    static func isTest(_ path: String) -> Bool {
        let segments = path.components(separatedBy: "/")
        for segment in segments.dropLast() {
            let lower = segment.lowercased()
            if testSegments.contains(lower) || lower.hasSuffix("tests") { return true }
        }
        let name = segments.last ?? ""
        let lower = name.lowercased()
        if lower.hasPrefix("test_") || lower.contains("_test.") || lower.contains(".test.") || lower.contains(".spec.")
            || lower.contains("_spec.") || lower.hasPrefix("conftest.") {
            return true
        }
        return typedTest.matches(U.units(name))
    }

    static func isVendored(_ path: String) -> Bool {
        let segments = path.components(separatedBy: "/")
        for segment in segments.dropLast() where vendoredSegments.contains(segment.lowercased()) {
            return true
        }
        let name = (segments.last ?? "").lowercased()
        for suffix in vendoredSuffixes where name.hasSuffix(suffix) { return true }
        return name.contains(".generated.") || name == "package-lock.json"
    }
}

import Foundation

struct Resolver {
    let s: InsightSnapshot
    var byName: [String: [Int]] = [:]
    var byNoExt: [String: [Int]] = [:]
    var dirFiles: [String: [Int]] = [:]
    var packages: [String: [Int]] = [:]
    var namespaces = Set<String>()
    var types: [String: [String: [Int]]] = [:]
    let goRoots: [(module: String, dir: String)]

    static let jsExtensions = ["", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".mts", ".cts", ".vue", ".svelte", ".d.ts",
                               "/index.ts", "/index.tsx", "/index.js", "/index.jsx", "/index.mjs", "/index.vue"]

    init(_ snapshot: InsightSnapshot) {
        s = snapshot
        goRoots = snapshot.packageRoots.keys.filter { $0.hasPrefix("go:") }.sorted(by: U.less)
            .map { (String($0.dropFirst(3)), snapshot.packageRoots[$0] ?? "") }
        for (i, file) in s.files.enumerated() {
            byName[PathText.fileNameOf(file.path), default: []].append(i)
            byNoExt[PathText.withoutExtension(file.path), default: []].append(i)
            dirFiles[PathText.dirOf(file.path), default: []].append(i)
            for declared in file.declares {
                if declared.hasPrefix("package:") {
                    let name = String(declared.dropFirst(8))
                    packages[name, default: []].append(i)
                    namespaces.insert(name)
                    var prefix = name
                    while prefix.contains(".") {
                        prefix = PathText.section(prefix, ".", 0, -2)
                        namespaces.insert(prefix)
                    }
                } else {
                    types[Resolver.family(file.language), default: [:]][declared, default: []].append(i)
                }
            }
        }
    }

    static func family(_ language: String) -> String {
        if language == "Java" || language == "Kotlin" || language == "Scala" || language == "Groovy" { return "jvm" }
        return language
    }

    func exact(_ path: String) -> Int {
        let clean = PathText.normalized(path)
        return clean.isEmpty ? -1 : (s.byPath[clean] ?? -1)
    }

    func probe(_ base: String, _ extensions: [String]) -> Int {
        for ext in extensions {
            let hit = exact(base + ext)
            if hit >= 0 { return hit }
        }
        return -1
    }

    func best(_ candidates: [Int], _ importer: String) -> Int {
        var chosen = -1
        var score = -1
        for candidate in candidates {
            let common = PathText.commonPrefix(s.files[candidate].path, importer)
            if common > score {
                score = common
                chosen = candidate
            }
        }
        return chosen
    }

    func suffix(_ spec: String, _ importer: String, _ extensions: [String]) -> Int {
        let clean = PathText.cleanPath(spec)
        if clean.isEmpty || clean.hasPrefix("..") { return -1 }
        for ext in extensions {
            let wanted = clean + ext
            let named = byName[PathText.fileNameOf(wanted)] ?? []
            var matches: [Int] = []
            for candidate in named {
                let path = s.files[candidate].path
                if path == wanted || path.hasSuffix("/" + wanted) { matches.append(candidate) }
            }
            if !matches.isEmpty { return best(matches, importer) }
        }
        return -1
    }

    func resolve(_ importer: Int, _ rawSpec: String) -> [Int] {
        let file = s.files[importer]
        let language = file.language
        let dir = PathText.dirOf(file.path)
        let spec = rawSpec.hasPrefix("?") ? String(rawSpec.dropFirst()) : rawSpec
        func one(_ index: Int) -> [Int] { index >= 0 ? [index] : [] }
        switch language {
        case "C", "C++", "Objective-C":
            if spec.hasPrefix("<") {
                let inner = String(spec.dropFirst().dropLast())
                if !inner.contains(".") && !inner.contains("/") { return [] }
                return one(suffix(inner, file.path, [""]))
            }
            var hit = exact(PathText.join(dir, spec))
            if hit < 0 { hit = exact(spec) }
            if hit < 0 { hit = suffix(spec, file.path, [""]) }
            return one(hit)
        case "JavaScript", "TypeScript", "Vue", "Svelte":
            var bases: [String] = []
            if spec.hasPrefix("./") || spec.hasPrefix("../") || spec == "." || spec == ".." {
                bases = [PathText.join(dir, spec)]
            } else if spec.hasPrefix("@/") || spec.hasPrefix("~/") {
                bases = ["src/" + spec.dropFirst(2), String(spec.dropFirst(2))]
            } else if spec.hasPrefix("/") {
                bases = [String(spec.dropFirst())]
            } else {
                bases = [spec, "src/" + spec]
            }
            for base in bases {
                var hit = probe(base, Resolver.jsExtensions)
                if hit < 0 {
                    for js in [".js", ".mjs", ".jsx", ".cjs"] where base.hasSuffix(js) {
                        hit = probe(String(base.dropLast(js.count)), [".ts", ".tsx", ".mts", ".cts"])
                        break
                    }
                }
                if hit >= 0 { return one(hit) }
            }
            if !spec.hasPrefix(".") && spec.contains("/") && !spec.hasPrefix("@") {
                return one(suffix(spec, file.path, [".ts", ".tsx", ".js", ".jsx", "/index.ts", "/index.js"]))
            }
            return []
        case "Python":
            return one(resolvePython(file, spec))
        case "Go":
            for root in goRoots {
                let module = root.module
                if spec != module && !spec.hasPrefix(module + "/") { continue }
                let restUnits = Array(spec.utf16)
                let skip = module.utf16.count + 1
                let rest = skip < restUnits.count ? U.string(Array(restUnits[skip...])) : ""
                let target = PathText.normalized(PathText.join(root.dir, rest))
                var hits: [Int] = []
                for candidate in dirFiles[target] ?? [] {
                    let other = s.files[candidate]
                    if other.language == "Go" && !other.test { hits.append(candidate) }
                    if hits.count >= 16 { break }
                }
                return hits
            }
            return []
        case "Rust":
            return one(resolveRust(file, spec))
        case "Java", "Kotlin", "Scala", "Groovy":
            if spec.hasSuffix(".*") { return [] }
            let jvm = [".java", ".kt", ".scala", ".groovy"]
            var current = spec
            var attempt = 0
            while attempt < 2 && current.contains(".") {
                let hit = suffix(current.replacingOccurrences(of: ".", with: "/"), file.path, jvm)
                if hit >= 0 { return one(hit) }
                let package = PathText.section(current, ".", 0, -2)
                let type = PathText.section(current, ".", -1)
                var declared: [Int] = []
                for candidate in packages[package] ?? [] {
                    let other = s.files[candidate]
                    if PathText.stemOf(other.path) == type || other.declares.contains(type) { declared.append(candidate) }
                }
                if !declared.isEmpty { return one(best(declared, file.path)) }
                current = package
                attempt += 1
            }
            return []
        case "PHP":
            if spec.contains("\\") {
                var parts = spec.components(separatedBy: "\\").filter { !$0.isEmpty }
                while parts.count >= 2 {
                    let hit = suffix(parts.joined(separator: "/"), file.path, [".php"])
                    if hit >= 0 { return one(hit) }
                    parts.removeFirst()
                }
                return []
            }
            var hit = exact(PathText.join(dir, spec))
            if hit < 0 { hit = exact(spec) }
            return one(hit)
        case "Ruby":
            if spec.hasPrefix("./") {
                return one(probe(PathText.join(dir, String(spec.dropFirst(2))), ["", ".rb"]))
            }
            var hit = probe("lib/" + spec, [".rb"])
            if hit < 0 && spec.contains("/") { hit = suffix(spec, file.path, [".rb"]) }
            return one(hit)
        case "Dart":
            if spec.hasPrefix("dart:") { return [] }
            if spec.hasPrefix("package:") {
                let rest = String(spec.dropFirst(8))
                let name = PathText.section(rest, "/", 0, 0)
                guard let root = s.packageRoots["dart:" + name] else { return [] }
                return one(exact(PathText.join(PathText.join(root, "lib"), PathText.section(rest, "/", 1))))
            }
            return one(exact(PathText.join(dir, spec)))
        case "Lua":
            let path = spec.replacingOccurrences(of: ".", with: "/")
            var hit = probe(path, [".lua", "/init.lua"])
            if hit < 0 { hit = suffix(path, file.path, [".lua", "/init.lua"]) }
            return one(hit)
        case "CSS", "SCSS", "Less":
            if spec.contains("://") { return [] }
            let base = PathText.join(dir, spec)
            let partial = PathText.join(PathText.dirOf(base), "_" + PathText.fileNameOf(base))
            var hit = probe(base, ["", ".scss", ".sass", ".css", ".less"])
            if hit < 0 { hit = probe(partial, ["", ".scss", ".sass"]) }
            return one(hit)
        case "QML":
            if !spec.hasPrefix("./") { return [] }
            var hits: [Int] = []
            for candidate in dirFiles[PathText.normalized(PathText.join(dir, spec))] ?? []
            where s.files[candidate].language == "QML" && hits.count < 12 {
                hits.append(candidate)
            }
            return hits
        case "Shell":
            if spec.contains("$") {
                return one(suffix(PathText.section(spec, "/", -2), file.path, [""]))
            }
            var hit = exact(PathText.join(dir, spec))
            if hit < 0 { hit = exact(spec) }
            return one(hit)
        default:
            return []
        }
    }

    func resolvePython(_ file: InsightFile, _ spec: String) -> Int {
        let extensions = [".py", "/__init__.py", ".pyi"]
        let dots = spec.prefix { $0 == "." }.count
        let module = String(spec.dropFirst(dots))
        let path = module.replacingOccurrences(of: ".", with: "/")
        if dots > 0 {
            var base = PathText.dirOf(file.path)
            if dots > 1 {
                for _ in 1..<dots { base = PathText.dirOf(base) }
            }
            if path.isEmpty { return exact(PathText.join(base, "__init__.py")) }
            return probe(PathText.join(base, path), extensions)
        }
        if path.isEmpty { return -1 }
        var hit = probe(path, extensions)
        if hit < 0 { hit = probe(PathText.join(PathText.dirOf(file.path), path), extensions) }
        if hit < 0 {
            var matches: [Int] = []
            for tail in [path, path + "/__init__"] {
                let name = PathText.fileNameOf(tail)
                for candidate in byName[name + ".py"] ?? [] {
                    let noExt = PathText.withoutExtension(s.files[candidate].path)
                    if noExt == tail || noExt.hasSuffix("/" + tail) { matches.append(candidate) }
                }
            }
            if !matches.isEmpty { hit = best(matches, file.path) }
        }
        return hit
    }

    func resolveRustPath(_ base: String, _ segments: [String]) -> Int {
        var k = segments.count
        while k >= 1 {
            let joined = PathText.join(base, segments[0..<k].joined(separator: "/"))
            let hit = probe(joined, [".rs", "/mod.rs"])
            if hit >= 0 { return hit }
            k -= 1
        }
        return -1
    }

    func resolveRust(_ file: InsightFile, _ spec: String) -> Int {
        let dir = PathText.dirOf(file.path)
        let stem = PathText.stemOf(file.path)
        let rootLike = stem == "mod" || stem == "lib" || stem == "main"
        let moduleDir = rootLike ? dir : PathText.join(dir, stem)
        if spec.hasPrefix("mod ") {
            return probe(PathText.join(moduleDir, String(spec.dropFirst(4))), [".rs", "/mod.rs"])
        }
        var segments = spec.components(separatedBy: "::")
        let head = segments.removeFirst()
        if head == "crate" {
            var crate = dir
            while true {
                if exact(PathText.join(crate, "lib.rs")) >= 0 || exact(PathText.join(crate, "main.rs")) >= 0 { break }
                if crate.isEmpty { return -1 }
                crate = PathText.dirOf(crate)
            }
            return resolveRustPath(crate, segments)
        }
        if head == "super" { return resolveRustPath(rootLike ? PathText.dirOf(dir) : dir, segments) }
        return resolveRustPath(moduleDir, segments)
    }
}

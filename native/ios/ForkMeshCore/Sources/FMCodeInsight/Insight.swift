import Dispatch
import Foundation

public enum Insight {
    public static func languageForPath(_ path: String) -> String {
        Languages.spec(for: path).name
    }

    public static func isTestPath(_ path: String) -> Bool {
        Languages.isTest(path)
    }

    public static func isVendoredPath(_ path: String) -> Bool {
        Languages.isVendored(path)
    }

    public static func measureSource(_ path: String, _ bytes: [UInt8]) -> InsightFile {
        Measure.measure(path, bytes, windows: false).info
    }

    public static func measureSource(_ path: String, _ text: String) -> InsightFile {
        measureSource(path, Array(text.utf8))
    }

    public static func extractImports(_ language: String, _ text: String) -> [String] {
        guard let spec = Languages.byName[language], !language.isEmpty else { return [] }
        let stripped = Strip.source(U.units(text), spec)
        return Imports.fromStripped(language, stripped.noComments).filter { !$0.hasPrefix("?") }
    }

    public static func healthBand(_ health: Double) -> HealthBand {
        if health >= 8.0 { return .healthy }
        if health >= 4.0 { return .problematic }
        return .unhealthy
    }

    public static func computeHealth(_ file: InsightFile) -> InsightHealth {
        var score = 10.0
        var reasons: [String] = []
        var brainCount = 0
        var brain = 0.0
        var complex = 0.0
        var longFunctions = 0.0
        var arguments = 0.0
        var brainNames: [String] = []
        var complexNames: [String] = []
        var longNames: [String] = []
        var argumentNames: [String] = []
        var deepest: InsightFunction?
        for fn in file.functions {
            let isBrain = fn.length >= 70 && fn.complexity >= 15
            if isBrain {
                brainCount += 1
                brain += 1.5
                brainNames.append("\(fn.name) (complexity \(fn.complexity), \(fn.length) lines)")
            } else if fn.complexity > 10 {
                complex += 0.35
                complexNames.append("\(fn.name) (\(fn.complexity))")
            }
            if !isBrain && fn.length > 70 {
                longFunctions += 0.25
                longNames.append("\(fn.name) (\(fn.length) lines)")
            }
            if fn.parameters > 5 {
                arguments += 0.2
                argumentNames.append("\(fn.name) (\(fn.parameters))")
            }
            if deepest == nil || fn.nesting > deepest!.nesting { deepest = fn }
        }
        func listed(_ names: [String]) -> String {
            var text = names.prefix(3).joined(separator: ", ")
            if names.count > 3 { text += " and \(names.count - 3) more" }
            return text
        }
        if brainCount > 0 {
            score -= min(3.5, brain)
            reasons.append("Brain method\(brainCount == 1 ? "" : "s"): \(listed(brainNames))")
        }
        if !complexNames.isEmpty {
            score -= min(2.5, complex)
            reasons.append("Complex function\(complexNames.count == 1 ? "" : "s"): \(listed(complexNames))")
        }
        if file.maxComplexity > 50 {
            score -= 1.0
            reasons.append("Extremely complex function (complexity \(file.maxComplexity))")
        } else if file.maxComplexity > 25 {
            score -= 0.5
        }
        if let deepest, deepest.nesting >= 4 {
            score -= deepest.nesting >= 6 ? 1.25 : 0.5
            reasons.append("Deeply nested logic: depth \(deepest.nesting) in \(deepest.name)")
        }
        if !longNames.isEmpty {
            score -= min(1.5, longFunctions)
            reasons.append("Large function\(longNames.count == 1 ? "" : "s"): \(listed(longNames))")
        }
        if !argumentNames.isEmpty {
            score -= min(1.0, arguments)
            reasons.append("Many arguments: \(listed(argumentNames))")
        }
        if file.codeLines > 800 {
            score -= min(2.0, Double(file.codeLines - 800) / 1000.0 + 0.5)
            reasons.append("Large file: \(file.codeLines) lines of code")
        }
        if file.functions.count > 75 {
            score -= 0.75
            reasons.append("Low cohesion: \(file.functions.count) functions in one file")
        }
        if file.codeLines > 0 {
            let share = Double(file.duplicatedLines) / Double(file.codeLines)
            if share > 0.1 && file.duplicatedLines >= 12 {
                score -= min(1.5, share * 5.0)
                reasons.append("Code duplication: \(file.duplicatedLines) lines (\(Int((share * 100).rounded())) %)")
            }
        }
        if file.codeLines > 100 && Double(file.complexity) / Double(file.codeLines) > 0.25 {
            score -= 0.5
            reasons.append("Complex conditionals: \(file.complexity) decisions in \(file.codeLines) lines")
        }
        if file.dependsOn.count > 25 {
            score -= 0.5
            reasons.append("Many dependencies: imports \(file.dependsOn.count) files")
        }
        if file.cycle >= 0 {
            score -= 0.5
            reasons.append("Part of a circular import")
        }
        score = max(1.0, min((score * 10.0).rounded() / 10.0, 10.0))
        return InsightHealth(health: score, issues: reasons)
    }

    public static func trendPicks(commitCount: Int, samples: Int = 16) -> [Int] {
        if commitCount <= 0 { return [] }
        let want = max(2, samples)
        var picks: [Int] = []
        if commitCount <= want {
            picks = Array(0..<commitCount)
        } else {
            for k in 0..<want {
                let index = Int((Double(k) * Double(commitCount - 1) / Double(want - 1)).rounded())
                if picks.last != index { picks.append(index) }
            }
        }
        return picks.reversed()
    }

    public static func trend(_ path: String, versions: [InsightVersion]) -> [InsightTrendPoint] {
        versions.map { version in
            let info = measureSource(path, version.bytes)
            return InsightTrendPoint(time: version.time, commit: version.commit, author: version.author,
                                     codeLines: info.codeLines, complexity: info.complexity,
                                     maxComplexity: info.maxComplexity, health: computeHealth(info).health)
        }
    }

    static func readPackageRoots(_ snapshot: inout InsightSnapshot, _ all: [InsightEntry], _ source: InsightSource) throws {
        var manifests: [InsightEntry] = []
        for entry in all {
            let name = PathText.fileNameOf(entry.path)
            if (name == "go.mod" || name == "pubspec.yaml") && !isVendoredPath(entry.path) { manifests.append(entry) }
            if manifests.count > 200 { break }
        }
        for entry in manifests {
            let bytes = (try? source.read(entry.path, limit: 1 << 20)) ?? nil
            let text = U.decode(bytes ?? [])
            let dir = PathText.dirOf(entry.path)
            if entry.path.hasSuffix("go.mod") {
                if let m = P.goModule.firstMatch(in: text) {
                    snapshot.packageRoots["go:" + m.string(1, in: text).replacingOccurrences(of: "\"", with: "")] = dir
                }
            } else if let m = P.dartName.firstMatch(in: text) {
                snapshot.packageRoots["dart:" + m.string(1, in: text)] = dir
            }
        }
    }

    final class ResultBox: @unchecked Sendable {
        var results: [Measured?]

        init(count: Int) {
            results = [Measured?](repeating: nil, count: count)
        }
    }

    public static func analyze(source: InsightSource,
                               options: InsightOptions = InsightOptions(),
                               now: Int64? = nil,
                               progress: (String, Double) -> Void = { _, _ in },
                               isCancelled: @escaping () -> Bool = { false }) throws -> InsightSnapshot {
        let started = DispatchTime.now().uptimeNanoseconds
        func elapsed() -> Int64 { Int64((DispatchTime.now().uptimeNanoseconds - started) / 1_000_000) }
        let clock = now ?? Int64(Date().timeIntervalSince1970)
        var snapshot = InsightSnapshot()
        snapshot.source = source.description
        snapshot.name = source.name
        snapshot.analyzedAt = clock
        func report(_ text: String, _ fraction: Double) { progress(text, clamp01(fraction)) }
        func checkCancel() throws {
            if isCancelled() { throw CancellationError() }
        }
        if !source.exists {
            snapshot.error = "The folder \(source.description) does not exist on this computer."
            return snapshot
        }
        report("Listing files in \(source.name)…", 0.01)
        snapshot.git = source.git
        if source.git {
            snapshot.head = source.head
            snapshot.branch = source.branch
        }
        let all = try source.entries()
        try checkCancel()
        var sources: [InsightEntry] = []
        var skipped = 0
        for entry in all where !languageForPath(entry.path).isEmpty {
            if isVendoredPath(entry.path) || entry.size > options.maxFileBytes {
                skipped += 1
                continue
            }
            sources.append(entry)
        }
        sources = sources.enumerated().sorted { x, y in
            if x.element.path != y.element.path { return U.less(x.element.path, y.element.path) }
            return x.offset < y.offset
        }.map(\.element)
        let sourceTotal = sources.count
        if sources.count > options.maxFiles {
            sources = Array(sources.prefix(max(0, options.maxFiles)))
            snapshot.warning = "Analyzed the first \(U.grouped(options.maxFiles)) of \(U.grouped(sourceTotal)) source files"
        }
        try readPackageRoots(&snapshot, all, source)
        if sources.isEmpty {
            snapshot.error = "No source code found in \(source.name)."
            snapshot.elapsedMs = elapsed()
            return snapshot
        }
        var significant: [[Int]] = []
        var windows: [[UInt64]] = []
        let chunkSize = 1200
        let limit = options.maxFileBytes
        var start = 0
        while start < sources.count {
            try checkCancel()
            let chunk = Array(sources[start..<min(start + chunkSize, sources.count)])
            report("Reading \(U.grouped(min(start + chunkSize, sources.count))) of \(U.grouped(sources.count)) files…",
                   0.04 + 0.6 * Double(start) / Double(sources.count))
            let box = ResultBox(count: chunk.count)
            box.results.withUnsafeMutableBufferPointer { results in
                let shared = results
                DispatchQueue.concurrentPerform(iterations: chunk.count) { k in
                    let entry = chunk[k]
                    let bytes = ((try? source.read(entry.path, limit: limit)) ?? nil) ?? []
                    var measured: Measured
                    if Measure.shouldSkip(bytes) {
                        measured = Measured()
                        measured.skip = true
                        measured.info.path = entry.path
                    } else {
                        measured = Measure.measure(entry.path, bytes, windows: true)
                    }
                    shared[k] = measured
                }
            }
            for case let measured? in box.results {
                if measured.skip {
                    skipped += 1
                    continue
                }
                snapshot.byPath[measured.info.path] = snapshot.files.count
                snapshot.files.append(measured.info)
                significant.append(measured.significant)
                windows.append(measured.windows)
            }
            start += chunkSize
        }
        snapshot.skippedFiles = skipped
        var cpp = 0
        var c = 0
        var objc = 0
        for file in snapshot.files {
            if file.language == "C++" {
                cpp += 1
            } else if file.language == "C" && !file.path.hasSuffix(".h") {
                c += 1
            } else if file.language == "Objective-C" {
                objc += 1
            }
        }
        let headerLanguage = objc > cpp && objc > c ? "Objective-C" : (cpp >= c && cpp > 0 ? "C++" : "")
        if !headerLanguage.isEmpty {
            for i in snapshot.files.indices where snapshot.files[i].path.hasSuffix(".h") && snapshot.files[i].language == "C" {
                snapshot.files[i].language = headerLanguage
            }
        }
        try checkCancel()
        report("Connecting imports…", 0.66)
        resolveDependencies(&snapshot)
        markCycles(&snapshot)
        report("Looking for duplicated code…", 0.7)
        applyWindows(&snapshot, significant, windows)
        significant = []
        windows = []
        if source.git && options.history {
            try checkCancel()
            report("Reading git history…", 0.74)
            let commits = try source.history(options, cancel: isCancelled)
            try checkCancel()
            report("Finding files that change together…", 0.9)
            applyHistory(&snapshot, commits ?? [], options, now: now ?? Int64(Date().timeIntervalSince1970))
        }
        report("Scoring code health…", 0.95)
        for i in snapshot.files.indices {
            let result = computeHealth(snapshot.files[i])
            snapshot.files[i].health = result.health
            snapshot.files[i].healthIssues = result.issues
        }
        computeHotspots(&snapshot)
        snapshot.elapsedMs = elapsed()
        report("Done", 1.0)
        return snapshot
    }
}

import Foundation

extension Insight {
    static func asciiTrim(_ bytes: ArraySlice<UInt8>) -> ArraySlice<UInt8> {
        var lo = bytes.startIndex
        var hi = bytes.endIndex
        func space(_ b: UInt8) -> Bool { b == 32 || (b >= 9 && b <= 13) }
        while lo < hi && space(bytes[lo]) { lo += 1 }
        while hi > lo && space(bytes[hi - 1]) { hi -= 1 }
        return bytes[lo..<hi]
    }

    static func splitBytes(_ bytes: ArraySlice<UInt8>, _ separator: UInt8) -> [ArraySlice<UInt8>] {
        var parts: [ArraySlice<UInt8>] = []
        var start = bytes.startIndex
        var i = bytes.startIndex
        while i < bytes.endIndex {
            if bytes[i] == separator {
                parts.append(bytes[start..<i])
                start = i + 1
            }
            i += 1
        }
        parts.append(bytes[start..<bytes.endIndex])
        return parts
    }

    static func utf8(_ bytes: ArraySlice<UInt8>) -> String {
        String(decoding: bytes, as: UTF8.self)
    }

    static func qtTrimmed(_ text: String) -> String {
        let units = U.units(text)
        let range = U.trim(units, 0..<units.count)
        return range.count == units.count ? text : U.string(units, range)
    }

    static func number<T: FixedWidthInteger>(_ bytes: ArraySlice<UInt8>, as type: T.Type) -> T {
        T(utf8(asciiTrim(bytes))) ?? 0
    }

    public static func parseGitLog(_ text: String) -> [InsightCommit] {
        parseGitLog(Array(text.utf8))
    }

    public static func parseGitLog(_ log: [UInt8]) -> [InsightCommit] {
        var commits: [InsightCommit] = []
        for record in splitBytes(log[...], 0x1E) {
            if asciiTrim(record).isEmpty { continue }
            let headerEnd = record.firstIndex(of: 10)
            let header = headerEnd.map { record[record.startIndex..<$0] } ?? record
            let fields = splitBytes(header, 0x1F)
            if fields.count < 3 { continue }
            let author = qtTrimmed(utf8(fields[1]))
            let time = number(fields[2], as: Int64.self)
            let subject = fields.count > 3 ? utf8(fields[3]) : ""
            var files: [InsightCommitFile] = []
            if let headerEnd {
                for line in splitBytes(record[(headerEnd + 1)...], 10) {
                    let parts = splitBytes(line, 9)
                    if parts.count < 3 { continue }
                    let pathBytes = line[parts[2].startIndex..<line.endIndex]
                    let path = qtTrimmed(utf8(pathBytes))
                    let added: Int? = parts[0].elementsEqual([45]) ? nil : Int(number(parts[0], as: Int32.self))
                    let deleted: Int? = parts[1].elementsEqual([45]) ? nil : Int(number(parts[1], as: Int32.self))
                    files.append(InsightCommitFile(path: path, added: added, deleted: deleted))
                }
            }
            commits.append(InsightCommit(hash: utf8(fields[0]), author: author, time: time, subject: subject, files: files))
        }
        return commits
    }

    public static func applyHistory(_ snapshot: inout InsightSnapshot, _ commits: [InsightCommit], _ options: InsightOptions = InsightOptions(), now: Int64) {
        struct Accumulator {
            var commits = 0
            var recent = 0
            var fixes = 0
            var added = 0
            var deleted = 0
            var last: Int64 = 0
            var first: Int64 = 0
            var authorLines: [String: Int] = [:]
            var authorCommits: [String: Int] = [:]
        }
        var perFile: [Int: Accumulator] = [:]
        var authorLast: [String: Int64] = [:]
        var authorTotals: [String: Int] = [:]
        var pairs: [UInt64: Int] = [:]
        let recentCutoff = now - 90 * 86400
        let formerCutoff = now - 180 * 86400
        var count = 0
        var earliest: Int64 = 0
        var latest: Int64 = 0
        for commit in commits {
            let author = commit.author
            let time = commit.time
            count += 1
            earliest = earliest == 0 ? time : min(earliest, time)
            latest = max(latest, time)
            authorLast[author] = max(authorLast[author] ?? 0, time)
            authorTotals[author, default: 0] += 1
            let fix = P.fixPattern.matches(U.units(commit.subject))
            var touched: [Int] = []
            for entry in commit.files {
                guard let index = snapshot.byPath[entry.path] else { continue }
                let added = entry.added ?? 0
                let deleted = entry.deleted ?? 0
                var acc = perFile[index] ?? Accumulator()
                acc.commits += 1
                if time >= recentCutoff { acc.recent += 1 }
                if fix { acc.fixes += 1 }
                acc.added = Int(Int32(truncatingIfNeeded: acc.added + added))
                acc.deleted = Int(Int32(truncatingIfNeeded: acc.deleted + deleted))
                acc.last = max(acc.last, time)
                acc.first = acc.first == 0 ? time : min(acc.first, time)
                acc.authorLines[author, default: 0] += added
                acc.authorCommits[author, default: 0] += 1
                perFile[index] = acc
                if !touched.contains(index) { touched.append(index) }
            }
            if touched.count >= 2 && touched.count <= options.maxCommitFiles {
                touched.sort()
                for a in 0..<touched.count {
                    for b in (a + 1)..<touched.count {
                        pairs[UInt64(UInt32(touched[a])) << 32 | UInt64(UInt32(touched[b])), default: 0] += 1
                    }
                }
            }
        }
        var former = Set<String>()
        for (author, last) in authorLast where last < formerCutoff { former.insert(author) }
        for (index, acc) in perFile {
            var file = snapshot.files[index]
            file.commits = acc.commits
            file.recentCommits = acc.recent
            file.fixCommits = acc.fixes
            file.linesAdded = acc.added
            file.linesDeleted = acc.deleted
            file.lastChanged = acc.last
            file.firstChanged = acc.first
            file.authors = acc.authorCommits.count
            var totalLines = 0
            var formerLines = 0
            var main = ""
            var mainLines = -1
            var mainCommits = -1
            for (name, commitsBy) in acc.authorCommits {
                let lines = acc.authorLines[name] ?? 0
                totalLines += lines
                if former.contains(name) { formerLines += lines }
                if lines > mainLines || (lines == mainLines && commitsBy > mainCommits)
                    || (lines == mainLines && commitsBy == mainCommits && U.less(name, main)) {
                    main = name
                    mainLines = lines
                    mainCommits = commitsBy
                }
            }
            file.mainAuthor = main
            if totalLines > 0 {
                file.mainAuthorShare = Double(mainLines) / Double(totalLines)
                file.formerAuthorShare = Double(formerLines) / Double(totalLines)
            } else if acc.commits > 0 {
                file.mainAuthorShare = Double(mainCommits) / Double(acc.commits)
                var formerCommits = 0
                for (name, commitsBy) in acc.authorCommits where former.contains(name) { formerCommits += commitsBy }
                file.formerAuthorShare = Double(formerCommits) / Double(acc.commits)
            }
            let authors = acc.authorCommits.map { InsightAuthorCount(name: $0.key, commits: $0.value) }.sorted { x, y in
                if x.commits != y.commits { return x.commits > y.commits }
                return U.less(x.name, y.name)
            }
            file.topAuthors = Array(authors.prefix(5))
            snapshot.files[index] = file
        }
        var couplings: [InsightCoupling] = []
        for key in pairs.keys.sorted() {
            let shared = pairs[key] ?? 0
            if shared < options.minSharedCommits { continue }
            let a = Int(key >> 32)
            let b = Int(key & 0xffff_ffff)
            let average = Double(snapshot.files[a].commits + snapshot.files[b].commits) / 2.0
            if average <= 0 { continue }
            let degree = clamp01(Double(shared) / average)
            if degree < options.minCouplingDegree { continue }
            couplings.append(InsightCoupling(a: a, b: b, shared: shared, degree: degree))
        }
        couplings = couplings.enumerated().sorted { x, y in
            if x.element.degree != y.element.degree { return x.element.degree > y.element.degree }
            if x.element.shared != y.element.shared { return x.element.shared > y.element.shared }
            if x.element.a != y.element.a { return x.element.a < y.element.a }
            return x.offset < y.offset
        }.map(\.element)
        if couplings.count > 20000 { couplings = Array(couplings.prefix(20000)) }
        snapshot.couplings = couplings
        let everyone = authorTotals.sorted { x, y in
            if x.value != y.value { return x.value > y.value }
            return U.less(x.key, y.key)
        }
        snapshot.authors = everyone.map(\.key)
        snapshot.formerAuthors = everyone.map(\.key).filter { former.contains($0) }
        snapshot.commitsScanned = count
        snapshot.historyStart = earliest
        snapshot.historyEnd = latest
        if count > 0 {
            let span = max(Int64(1), (now - earliest + 86399) / 86400)
            snapshot.historyDays = Int(min(span, Int64(options.historyDays)))
        } else {
            snapshot.historyDays = 0
        }
    }

    public static func applyHistory(_ snapshot: inout InsightSnapshot, gitLog: String, _ options: InsightOptions = InsightOptions(), now: Int64) {
        applyHistory(&snapshot, parseGitLog(gitLog), options, now: now)
    }
}

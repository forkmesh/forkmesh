import Foundation

public struct InsightFunction: Sendable, Equatable {
    public var name: String
    public var line: Int
    public var length: Int
    public var complexity: Int
    public var nesting: Int
    public var parameters: Int

    public init(name: String = "", line: Int = 0, length: Int = 0, complexity: Int = 1, nesting: Int = 0, parameters: Int = 0) {
        self.name = name
        self.line = line
        self.length = length
        self.complexity = complexity
        self.nesting = nesting
        self.parameters = parameters
    }
}

public struct InsightAuthorCount: Sendable, Equatable {
    public var name: String
    public var commits: Int

    public init(name: String, commits: Int) {
        self.name = name
        self.commits = commits
    }
}

public struct InsightFile: Sendable, Equatable {
    public var path = ""
    public var language = ""
    public var bytes: Int64 = 0
    public var lines = 0
    public var codeLines = 0
    public var commentLines = 0
    public var blankLines = 0
    public var types = 0
    public var abstractTypes = 0
    public var complexity = 0
    public var maxComplexity = 0
    public var maxNesting = 0
    public var duplicatedLines = 0
    public var test = false
    public var functions: [InsightFunction] = []
    public var imports: [String] = []
    public var externalImports: [String] = []
    public var declares: [String] = []
    public var references: [String] = []
    public var dependsOn: [Int] = []
    public var dependedOnBy: [Int] = []
    public var commits = 0
    public var recentCommits = 0
    public var authors = 0
    public var fixCommits = 0
    public var linesAdded = 0
    public var linesDeleted = 0
    public var lastChanged: Int64 = 0
    public var firstChanged: Int64 = 0
    public var mainAuthor = ""
    public var mainAuthorShare: Double = 0
    public var formerAuthorShare: Double = 0
    public var topAuthors: [InsightAuthorCount] = []
    public var health: Double = 10
    public var healthIssues: [String] = []
    public var hotspot: Double = 0
    public var cycle = -1

    public init(path: String = "") {
        self.path = path
    }
}

public struct InsightCoupling: Sendable, Equatable {
    public var a: Int
    public var b: Int
    public var shared: Int
    public var degree: Double

    public init(a: Int = -1, b: Int = -1, shared: Int = 0, degree: Double = 0) {
        self.a = a
        self.b = b
        self.shared = shared
        self.degree = degree
    }
}

public enum HealthBand: String, Sendable, Equatable, CaseIterable {
    case healthy, problematic, unhealthy

    public var name: String {
        switch self {
        case .healthy: return "Healthy"
        case .problematic: return "Problematic"
        case .unhealthy: return "Unhealthy"
        }
    }

    public var label: String { name }
}

public struct InsightHealth: Sendable, Equatable {
    public var health: Double
    public var issues: [String]

    public init(health: Double, issues: [String]) {
        self.health = health
        self.issues = issues
    }
}

public struct InsightSnapshot: Sendable, Equatable {
    public var source = ""
    public var name = ""
    public var head = ""
    public var branch = ""
    public var analyzedAt: Int64 = 0
    public var elapsedMs: Int64 = 0
    public var git = false
    public var historyDays = 0
    public var commitsScanned = 0
    public var historyStart: Int64 = 0
    public var historyEnd: Int64 = 0
    public var files: [InsightFile] = []
    public var byPath: [String: Int] = [:]
    public var packageRoots: [String: String] = [:]
    public var couplings: [InsightCoupling] = []
    public var cycles: [[Int]] = []
    public var authors: [String] = []
    public var formerAuthors: [String] = []
    public var skippedFiles = 0
    public var warning = ""
    public var error = ""

    public init() {}

    public init(files: [InsightFile]) {
        for file in files {
            byPath[file.path] = self.files.count
            self.files.append(file)
        }
    }

    public func indexOf(_ path: String) -> Int { byPath[path] ?? -1 }

    public var totalCodeLines: Int {
        Int(min(files.reduce(Int64(0)) { $0 + Int64($1.codeLines) }, Int64(Int32.max)))
    }

    public var totalComplexity: Int {
        Int(min(files.reduce(Int64(0)) { $0 + Int64($1.complexity) }, Int64(Int32.max)))
    }

    public func couplingsFor(_ file: Int) -> [InsightCoupling] {
        var result: [InsightCoupling] = []
        for coupling in couplings {
            if coupling.a == file {
                result.append(coupling)
            } else if coupling.b == file {
                result.append(InsightCoupling(a: coupling.b, b: coupling.a, shared: coupling.shared, degree: coupling.degree))
            }
        }
        return result.enumerated().sorted { x, y in
            if x.element.degree != y.element.degree { return x.element.degree > y.element.degree }
            if x.element.shared != y.element.shared { return x.element.shared > y.element.shared }
            return x.offset < y.offset
        }.map(\.element)
    }
}

public struct InsightOptions: Sendable, Equatable {
    public var history = true
    public var historyDays = 365
    public var maxFiles = 30000
    public var maxCommits = 8000
    public var maxCommitFiles = 40
    public var minSharedCommits = 3
    public var minCouplingDegree = 0.25
    public var maxFileBytes: Int64 = 2 * 1024 * 1024

    public init() {}
}

public struct InsightCommitFile: Sendable, Equatable {
    public var path: String
    public var added: Int?
    public var deleted: Int?

    public init(path: String, added: Int?, deleted: Int?) {
        self.path = path
        self.added = added
        self.deleted = deleted
    }
}

public struct InsightCommit: Sendable, Equatable {
    public var hash: String
    public var author: String
    public var time: Int64
    public var subject: String
    public var files: [InsightCommitFile]

    public init(hash: String, author: String, time: Int64, subject: String, files: [InsightCommitFile]) {
        self.hash = hash
        self.author = author
        self.time = time
        self.subject = subject
        self.files = files
    }
}

public struct InsightFileEdge: Sendable, Equatable {
    public var from: Int
    public var to: Int

    public init(from: Int, to: Int) {
        self.from = from
        self.to = to
    }
}

public struct InsightModuleNode: Sendable, Equatable {
    public var path = ""
    public var name = ""
    public var files: [Int] = []
    public var codeLines = 0
    public var complexity = 0
    public var functions = 0
    public var types = 0
    public var abstractTypes = 0
    public var afferent = 0
    public var efferent = 0
    public var internalEdges = 0
    public var instability: Double = 0
    public var abstractness: Double = 0
    public var distance: Double = 0
    public var cohesion: Double = 0
    public var health: Double = 10
    public var commits = 0
    public var cycle = -1
    public var layer = 0

    public init() {}
}

public struct InsightModuleEdge: Sendable, Equatable {
    public var from = -1
    public var to = -1
    public var weight = 0
    public var inCycle = false
    public var fileEdges: [InsightFileEdge] = []

    public init() {}
}

public struct InsightModuleGraph: Sendable, Equatable {
    public var depth = 1
    public var nodes: [InsightModuleNode] = []
    public var edges: [InsightModuleEdge] = []
    public var cycles: [[Int]] = []
    public var moduleOfFile: [Int] = []

    public init() {}

    public func edgeIndex(_ from: Int, _ to: Int) -> Int {
        edges.firstIndex { $0.from == from && $0.to == to } ?? -1
    }
}

public struct InsightTrendPoint: Sendable, Equatable {
    public var time: Int64
    public var commit: String
    public var author: String
    public var codeLines: Int
    public var complexity: Int
    public var maxComplexity: Int
    public var health: Double

    public init(time: Int64 = 0, commit: String = "", author: String = "", codeLines: Int = 0, complexity: Int = 0, maxComplexity: Int = 0, health: Double = 10) {
        self.time = time
        self.commit = commit
        self.author = author
        self.codeLines = codeLines
        self.complexity = complexity
        self.maxComplexity = maxComplexity
        self.health = health
    }
}

public struct InsightVersion: Sendable, Equatable {
    public var time: Int64
    public var commit: String
    public var author: String
    public var bytes: [UInt8]

    public init(time: Int64, commit: String, author: String, bytes: [UInt8]) {
        self.time = time
        self.commit = commit
        self.author = author
        self.bytes = bytes
    }
}

public struct InsightEntry: Sendable, Equatable {
    public var path: String
    public var size: Int64

    public init(path: String, size: Int64 = -1) {
        self.path = path
        self.size = size
    }
}

public protocol InsightSource {
    var name: String { get }
    var description: String { get }
    var head: String { get }
    var branch: String { get }
    var git: Bool { get }
    var exists: Bool { get }
    func entries() throws -> [InsightEntry]
    func read(_ path: String, limit: Int64) throws -> [UInt8]?
    func history(_ options: InsightOptions, cancel: @escaping () -> Bool) throws -> [InsightCommit]?
}

public extension InsightSource {
    var exists: Bool { true }
    var head: String { "" }
    var branch: String { "" }
}

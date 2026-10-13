import FMApi
import FMBase
import Foundation

@MainActor
public protocol ReviewLedgerReading: AnyObject {
    func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree
    func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob
    func compare(_ owner: String, _ name: String, base: String, head: String) async throws -> RepoComparison
}

extension ApiService: ReviewLedgerReading {}

public struct ReviewLedgerError: Error, Equatable, CustomStringConvertible {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
}

@MainActor
public final class ReviewLedger {
    public static let ref = "forkmesh/pulls"
    public static let maxEvents = 400
    public static let maxPatchBytes = 4 * 1024 * 1024

    let reader: ReviewLedgerReading
    let owner: String
    let name: String

    public init(reader: ReviewLedgerReading, owner: String, name: String) {
        self.reader = reader
        self.owner = owner
        self.name = name
    }

    public static func eventFileNames(_ entries: [RepoTreeEntry]) -> [RepoTreeEntry] {
        entries.filter { !$0.isDirectory && $0.name.hasSuffix(".md") && $0.name != "pull.md" }
            .sorted { $0.name < $1.name }
            .suffix(maxEvents)
            .map { $0 }
    }

    public func load(_ number: Int) async throws -> PullReviewRecord {
        let folder = ".forkmesh/pulls/\(number)"
        let listing: RepoTree
        do {
            listing = try await reader.tree(owner, name, path: folder, ref: ReviewLedger.ref)
        } catch {
            throw ReviewLedgerError("Pull request #\(number) could not be read from the pull ledger.")
        }
        guard listing.entries.contains(where: { $0.name == "pull.md" }) else {
            throw ReviewLedgerError("Pull request #\(number) was not found.")
        }
        let header = try await reader.blob(owner, name, "\(folder)/pull.md", ref: ReviewLedger.ref)
        var events: [ReviewEvent] = []
        for file in ReviewLedger.eventFileNames(listing.entries) {
            if let blob = try? await reader.blob(owner, name, file.path, ref: ReviewLedger.ref) {
                events.append(ReviewEvent(markdown: blob.content))
            }
        }
        var storedPatch = ""
        if listing.entries.contains(where: { $0.name == "changes.patch" }),
           let blob = try? await reader.blob(owner, name, "\(folder)/changes.patch", ref: ReviewLedger.ref) {
            storedPatch = blob.content
        }
        var record = PullReviewRecord(number: number, markdown: header.content, events: events, patch: storedPatch)
        if record.patch.isEmpty { record.patch = await comparePatch(record) }
        if record.patch.utf8.count > ReviewLedger.maxPatchBytes { record.patch = "" }
        return record
    }

    func comparePatch(_ record: PullReviewRecord) async -> String {
        let pairs: [(String, String)] = [
            (record.creationBaseOid, record.creationHeadOid), (record.base, record.head),
        ].filter { !$0.0.isEmpty && !$0.1.isEmpty }
        for (base, head) in pairs {
            if let comparison = try? await reader.compare(owner, name, base: base, head: head), !comparison.patch.isEmpty {
                return comparison.patch
            }
        }
        return ""
    }
}

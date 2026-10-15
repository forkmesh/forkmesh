// The logic half of quick edit: edit one file
// of a repository this device has NOT cloned, and propose the change as a pull
// request. A ForkMesh pull request carries its own patch rather than pointing
// at a branch, so contributing needs neither push access nor a fork — only a
// correct unified diff, whose blob ids are a pure function of the bytes the
// API already serves.

import FMApi
import FMBase
import FMGit
import Foundation
import Observation

/// Why a file cannot be edited here. Each maps to something the person can
/// actually do about it.
public enum QuickEditBlocker: String, Sendable {
    /// Binary content. Nothing a text editor can honestly offer.
    case binary
    /// Bytes that are not valid UTF-8.
    case notUtf8
    /// Past `QuickEdit.maxBytes`.
    case tooLarge

    public var title: String {
        switch self {
        case .binary: return "Binary file"
        case .notUtf8: return "Not a text file"
        case .tooLarge: return "File too large"
        }
    }

    public var message: String {
        switch self {
        case .binary:
            return "This file is not text, so it cannot be edited on this device. Clone the repository to change it."
        case .notUtf8:
            return "This file is not valid UTF-8. Editing it here would rewrite the bytes "
                + "that could not be read, so it has to be changed from a clone."
        case .tooLarge:
            return "Files over \(QuickEdit.maxBytes / 1024) KB are edited from a clone, not in the quick editor."
        }
    }
}

public enum QuickEdit {
    /// Largest file the quick editor will open. Past this the clone path is the
    /// honest answer.
    public static let maxBytes = 512 * 1024

    /// Why `bytes` cannot be edited here, or nil when they can. What the file
    /// *is* comes before what is wrong with it.
    public static func blocker(bytes: [UInt8], maxBytes: Int = QuickEdit.maxBytes) -> QuickEditBlocker? {
        if bytes.count > maxBytes { return .tooLarge }
        if bytes.contains(0) { return .binary }
        if (try? Utf8.decode(bytes)) == nil { return .notUtf8 }
        return nil
    }

    /// The pull request's head label, following the desktop's cross-node
    /// convention of `<node>:<branch>`. No branch exists behind it: the
    /// receiving node merges from the carried patch. The timestamp keeps two
    /// edits of the same file apart.
    public static func head(account: String, path: String, now: Date = Date()) -> String {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        let parts = calendar.dateComponents([.month, .day, .hour, .minute], from: now)
        func two(_ value: Int?) -> String {
            let text = String(value ?? 0)
            return text.count < 2 ? "0" + text : text
        }
        let stamp = "\(two(parts.month))\(two(parts.day))-\(two(parts.hour))\(two(parts.minute))"
        let last = path.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? ""
        var slug = ""
        var pendingDash = false
        for scalar in last.lowercased().unicodeScalars {
            let isAlnum = (0x61...0x7a).contains(scalar.value) || (0x30...0x39).contains(scalar.value)
            if isAlnum {
                if pendingDash && !slug.isEmpty { slug += "-" }
                pendingDash = false
                slug.unicodeScalars.append(scalar)
            } else {
                pendingDash = true
            }
        }
        let branch = "mobile-edit/\(slug.isEmpty ? "file" : slug)-\(stamp)"
        return account.isEmpty ? branch : "\(account):\(branch)"
    }

    /// A relay refusal in the words of someone who just tried to edit a file.
    public static func submitFailure(_ error: String) -> String {
        if error.contains("inbox_full") {
            return "This repository is not accepting pull requests right now — its inbox is full until the owner drains it."
        }
        if error.contains("author_quota") {
            return "You have too many pull requests pending on this repository. Wait for the owner to work through them."
        }
        if error.contains("pull_too_large") {
            return "This change is too large to submit from mobile."
        }
        return "Could not submit the change: \(error)"
    }
}

/// State and actions of the quick editor screen.
@MainActor
@Observable
public final class QuickEditModel {
    public typealias SubmitPull = @MainActor (
        _ owner: String, _ name: String, _ title: String, _ base: String, _ head: String,
        _ patch: String, _ description: String, _ authorName: String
    ) async throws -> Void

    public let repo: Repository
    public let path: String
    /// The branch being browsed; becomes the pull request's base.
    public let ref: String

    /// The editor text.
    public var text = ""
    public private(set) var original = ""
    public private(set) var loading = true
    public private(set) var submitting = false
    public private(set) var blocker: QuickEditBlocker?
    public private(set) var error: String?
    @ObservationIgnored private var bytes: [UInt8]?

    @ObservationIgnored let readBytes: @MainActor (String, String, String, String, Int) async throws -> Data
    @ObservationIgnored let submit: SubmitPull?

    public init(
        repo: Repository, path: String, ref: String = "",
        readBytes: @escaping @MainActor (_ owner: String, _ name: String, _ path: String, _ ref: String, _ maxBytes: Int) async throws -> Data,
        submit: SubmitPull?
    ) {
        self.repo = repo
        self.path = path
        self.ref = ref
        self.readBytes = readBytes
        self.submit = submit
    }

    /// The app wiring over ApiService and InboxService.
    public convenience init(repo: Repository, path: String, ref: String = "", api: ApiService, inbox: InboxService?) {
        self.init(
            repo: repo, path: path, ref: ref,
            readBytes: { owner, name, path, ref, max in try await api.rawBytes(owner, name, path, ref: ref, maxBytes: max) },
            submit: inbox.map { inbox in
                { owner, name, title, base, head, patch, description, author in
                    try await inbox.submitNewPull(
                        owner, name, title: title, base: base, head: head, patch: patch,
                        description: description, authorName: author)
                }
            })
    }

    public var base: String { ref.isEmpty ? repo.defaultBranch : ref }
    public var fileName: String { path.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? path }
    public var dirty: Bool { bytes != nil && text != original }
    public var canPropose: Bool { dirty && !submitting }
    public var suggestedTitle: String { "Update \(fileName)" }

    public func load() async {
        do {
            let data = try await readBytes(repo.owner, repo.name, path, ref, QuickEdit.maxBytes)
            let raw = [UInt8](data)
            if let blocker = QuickEdit.blocker(bytes: raw) {
                loading = false
                self.blocker = blocker
                return
            }
            let decoded = (try? Utf8.decode(raw)) ?? ""
            loading = false
            bytes = raw
            original = decoded
            text = decoded
        } catch {
            loading = false
            let described = dartErrorText(error)
            // rawBytes refuses anything past the ceiling; name it precisely.
            blocker = described.contains("too large") ? .tooLarge : nil
            self.error = blocker == nil ? described : nil
        }
    }

    public enum ProposeResult: Equatable {
        /// Nothing changed from what was served (message to show).
        case unchanged(String)
        /// Submitted; show the message and close the editor.
        case submitted(String)
        /// Refused or failed (message to show in the danger colour).
        case failed(String)
    }

    /// The patch the current text would propose; empty when nothing changed.
    public func buildPatch() -> PatchBuildResultBox? {
        guard let bytes else { return nil }
        let result = LocalPatch.forFile(
            path: path, beforeBytes: bytes, afterText: text,
            oldId: GitBlobId.of(bytes), newId: GitBlobId.of(Array(text.utf8)))
        return PatchBuildResultBox(patch: result.patch, isEmpty: result.isEmpty)
    }

    /// Submit the change as a pull request titled `title`.
    public func propose(title: String, description: String, account: String, now: Date = Date()) async -> ProposeResult {
        guard dirty, !submitting, let patch = buildPatch() else { return .unchanged("Nothing changed in \(path).") }
        if patch.isEmpty {
            // Reachable when an edit is made and then undone by hand.
            return .unchanged("Nothing changed in \(path).")
        }
        guard let submit else { return .failed(QuickEdit.submitFailure("inbox unavailable")) }
        submitting = true
        do {
            try await submit(
                repo.owner, repo.name, title, base, QuickEdit.head(account: account, path: path, now: now),
                patch.patch, description, account)
            return .submitted("Submitted to the inbox - pending the repo owner applying it.")
        } catch {
            submitting = false
            return .failed(QuickEdit.submitFailure(dartErrorText(error)))
        }
    }
}

/// The two facts of a patch the quick editor needs (keeps FMDiff out of views).
public struct PatchBuildResultBox: Equatable, Sendable {
    public let patch: String
    public let isEmpty: Bool
}

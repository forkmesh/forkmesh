import FMApi
import FMBase
import Foundation
import Observation

public enum PullCompose {
    public static let thisRepository = "This repository"
    public static let compareLabel = "Compare"
    public static let comparing = "Comparing…"
    public static let pickBranches = "Pick the branch with your changes and the branch to merge it into."
    public static let sameBranch = "Choose two different branches."
    public static let noChanges = "There is nothing to compare: the head branch has no changes on top of the base."
    public static let compareFailed =
        "The mirror could not compare these branches yet. A branch pushed a moment ago can take a minute to appear."
    public static let pastePatch = "Paste a patch instead"
    public static let useBranches = "Compare branches instead"
    public static let titleRequired = "Add a title."
    public static let remoteNote =
        "Sent straight to the other instance and reviewed there. Both copies must share history."

    public static func targetLabel(_ fork: RemoteFork?) -> String { fork?.displayName ?? thisRepository }

    public static func ableToMerge(_ comparison: RepoComparison) -> String {
        let count = comparison.commitCount
        return count > 0 ? "\(count) \(count == 1 ? "commit" : "commits") to merge." : "Ready to send."
    }

    public static func sentLocal() -> String { CollabCopy.pendingNote }

    public static func sentRemote(_ fork: RemoteFork, number: Int) -> String {
        number > 0
            ? "Sent to \(fork.displayName) as pull request #\(number)."
            : "Sent to \(fork.displayName). It is reviewed there."
    }

    public static func remoteError(_ error: Error, _ fork: RemoteFork) -> String {
        let code = (error as? ApiException)?.message ?? dartErrorText(error)
        switch code {
        case "remote_unreachable":
            return "Nothing was delivered: \(fork.host) could not be reached, or it runs a ForkMesh version that does not take pull requests from other instances yet."
        case "bad_signature":
            return "\(fork.host) could not verify the signature, so nothing was opened there."
        case "not_found":
            return "\(fork.owner)/\(fork.name) was not found on \(fork.host)."
        case "rate_limited", "inbox_full", "quota_exceeded":
            return "\(fork.host) is not taking more pull requests right now (\(code)). Try again later."
        default:
            return "Could not send the pull request to \(fork.displayName) (\(code))."
        }
    }
}

@MainActor
@Observable
public final class NewPullModel {
    public let repo: Repository
    public var title = ""
    public var body = ""
    public var base: String {
        didSet { if base != oldValue { comparison = nil } }
    }
    public var head = "" {
        didSet { if head != oldValue { comparison = nil } }
    }
    public var patch = ""
    public var pasting = false
    public var target: RemoteFork?
    public private(set) var branches: [String] = []
    public private(set) var forks: [RemoteFork] = []
    public private(set) var comparison: RepoComparison?
    public private(set) var comparing = false
    public private(set) var compareError = ""
    public private(set) var sending = false
    public private(set) var errorText = ""

    @ObservationIgnored private let api: CollabAPI?

    public init(repo: Repository, api: CollabAPI? = nil, head: String = "") {
        self.repo = repo
        self.api = api
        self.base = repo.defaultBranch
        self.head = head
        self.pasting = api == nil
    }

    public var targets: [RemoteFork?] { [nil] + forks.map { Optional($0) } }

    public var headChoices: [String] { branches.filter { $0 != base } }

    public var files: [PatchFile] { parsePatchFiles(effectivePatch) }

    public var effectivePatch: String { pasting ? patch : comparison?.patch ?? "" }

    public var effectiveCommits: String { pasting ? "" : comparison?.commits ?? "" }

    public var ready: Bool { NewPullComposer.ready(title: title, patch: effectivePatch) && (!pasting || target == nil) }

    public func load() async {
        guard let api else { return }
        async let names = try? api.branches(repo.owner, repo.name)
        async let saved = try? api.remoteForks(repo.owner, repo.name)
        let (loaded, remote) = await (names, saved)
        branches = (loaded ?? []).map(\.name)
        forks = remote ?? []
        if base.isEmpty, let first = (loaded ?? []).first(where: \.isDefault)?.name ?? branches.first { base = first }
        if !head.isEmpty && comparison == nil { await compare() }
    }

    public func compare() async {
        guard let api else { return }
        let base = self.base.dartTrimmed
        let head = self.head.dartTrimmed
        compareError = ""
        if base.isEmpty || head.isEmpty {
            compareError = PullCompose.pickBranches
            return
        }
        if base == head {
            compareError = PullCompose.sameBranch
            return
        }
        comparing = true
        defer { comparing = false }
        do {
            let result = try await api.compare(repo.owner, repo.name, base: base, head: head)
            if base != self.base.dartTrimmed || head != self.head.dartTrimmed { return }
            if result.patch.dartTrimmed.isEmpty {
                comparison = nil
                compareError = PullCompose.noChanges
            } else {
                comparison = result
                if title.dartTrimmed.isEmpty { title = PullCompose.defaultTitle(head) }
            }
        } catch {
            comparison = nil
            compareError = PullCompose.compareFailed
        }
    }

    public func submit(inbox: CollabInbox?, host: String) async -> CollabNotice? {
        if !NewPullComposer.ready(title: title, patch: effectivePatch) {
            errorText = title.dartTrimmed.isEmpty ? PullCompose.titleRequired : PullCompose.pickBranches
            return nil
        }
        guard let inbox else { return CollabNotice(CollabCopy.runFailure(CollabInboxUnavailable()), isError: true) }
        sending = true
        errorText = ""
        defer { sending = false }
        let title = self.title.dartTrimmed
        let base = self.base.dartTrimmed
        let head = self.head.dartTrimmed
        let description = body.replacingOccurrences(of: #"[\r\n]+$"#, with: "", options: .regularExpression)
        if let fork = target {
            if pasting || host.isEmpty {
                errorText = PullCompose.pickBranches
                return nil
            }
            let signedHead = RemoteFork.signedHead(host: host, owner: repo.displayOwner, name: repo.name, branch: head)
            do {
                let number = try await inbox.openRemotePull(
                    fork, title: title, base: base, head: signedHead, patch: effectivePatch, commits: effectiveCommits,
                    description: description, authorName: "")
                return CollabNotice(PullCompose.sentRemote(fork, number: number))
            } catch {
                errorText = PullCompose.remoteError(error, fork)
                return nil
            }
        }
        do {
            _ = try await inbox.openNewPull(
                repo.owner, repo.name, title: title, base: base, head: head, patch: effectivePatch,
                commits: effectiveCommits, description: description, authorName: "")
            return CollabNotice(PullCompose.sentLocal())
        } catch {
            errorText = CollabCopy.runFailure(error)
            return nil
        }
    }
}

extension PullCompose {
    public static func defaultTitle(_ head: String) -> String {
        let leaf = head.split(separator: "/").last.map(String.init) ?? head
        let words = leaf.replacingOccurrences(of: "-", with: " ").replacingOccurrences(of: "_", with: " ").dartTrimmed
        guard let first = words.first else { return "" }
        return first.uppercased() + words.dropFirst()
    }
}

import FMGit
import Foundation

enum ShallowPushProof {
    static let schedule: [GitDeepen] = [.relative(50), .relative(250), .relative(1000), .unshallow]

    static func prove(
        _ repo: GitRepository,
        branch: String,
        remote: String,
        schedule: [GitDeepen] = ShallowPushProof.schedule,
        fetch: (GitFetchRefspec, GitDeepen?) async throws -> Void,
        token: GitCancellationToken
    ) async throws {
        let tracking = try GitRefName.parse("refs/remotes/\(remote)/\(branch)")
        let refspec = try GitFetchRefspec.parse("+refs/heads/\(branch):\(tracking.value)")
        let local = try GitBranchName(branch).ref()
        let store = RefStore(repo.gitDirectory)
        try await bounded(branch) { try await fetch(refspec, nil) }
        var steps = schedule[...]
        while true {
            try token.throwIfCancelled()
            guard let remoteTip = try store.readDirect(tracking) else { return }
            guard let localTip = try store.readDirect(local) else {
                throw GitException(.invalidRef, "local branch \(branch) does not exist")
            }
            let ancestry = try await bounded(branch) { try repo.ancestry(remoteTip, localTip, token) }
            switch ancestry {
            case .ancestor:
                return
            case .notAncestor:
                throw diverged(branch)
            case .unknown:
                guard let step = steps.popFirst() else { throw historyLimit(branch) }
                try await bounded(branch) { try await fetch(refspec, step) }
            }
        }
    }

    static func diverged(_ branch: String) -> GitWorkspaceException {
        GitWorkspaceException(
            "\(branch) on ForkMesh has commits that your \(branch) does not include, so pushing would overwrite them. "
                + "Push your work to a new branch and open a pull request, or fetch and redo your change on top of \(branch).",
            code: .nonFastForward,
            detail: "push_diverged")
    }

    static func historyLimit(_ branch: String) -> GitWorkspaceException {
        GitWorkspaceException(
            "This device holds only recent history of \(branch), and checking that your push only adds commits "
                + "needs more history than it may download. Push your work to a new branch and open a pull request, "
                + "or push from a full clone on a computer.",
            code: .resourceLimitExceeded,
            detail: "shallow_history_limit")
    }

    private static func bounded<T>(_ branch: String, _ body: () async throws -> T) async throws -> T {
        do {
            return try await body()
        } catch let error as GitException where error.code == .resourceLimitExceeded {
            throw historyLimit(branch)
        }
    }
}

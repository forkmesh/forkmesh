// The owner-key gates of the repository detail screen: the Actions tab's read
// proof (forkmesh-actions-read-v1) and desktop command requests
// (forkmesh-desktop-command-v1). Both sign with the device identity and only
// work for the repository owner's own session.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import Foundation
import Observation

/// `_ActionReadProof` (and the signed fields of a desktop command).
public struct RepoOwnerProof: Equatable, Sendable {
    public var ownerAccount: String
    public var ts: String
    public var sig: String

    public init(ownerAccount: String, ts: String, sig: String) {
        self.ownerAccount = ownerAccount
        self.ts = ts
        self.sig = sig
    }
}

public enum RepoOwnerSigning {
    /// `forkmesh-actions-read-v1\n<owner>\n<name>\n<ts>`.
    public static func actionsReadCanonical(owner: String, name: String, ts: String) -> String {
        "forkmesh-actions-read-v1\n\(owner)\n\(name)\n\(ts)"
    }

    /// `forkmesh-desktop-command-v1\n<owner>\n<name>\n<command>\n<target>\n<ts>`.
    public static func desktopCommandCanonical(owner: String, name: String, command: String, target: String, ts: String) -> String {
        DesktopCommandContract.canonical(owner: owner, name: name, command: command, target: target, ts: ts)
    }

    /// The signed-in account, lower-cased, or "".
    static func account(_ session: AuthSession?) -> String {
        session?.nodeName.dartTrimmed.lowercased() ?? ""
    }

    /// `_readProof`: throws the gate's sentence when this device may not read
    /// the owner's workflow logs.
    public static func actionsReadProof(
        repo: Repository, session: AuthSession?, identity: DeviceIdentity?, nowMillis: Int64
    ) throws -> RepoOwnerProof {
        let account = account(session)
        guard let session, let identity, !account.isEmpty else {
            throw RepoException(
                "Actions are owner-key gated. Sign in with the repo owner account and local owner key to view workflow logs.")
        }
        if account != repo.owner.dartTrimmed.lowercased() {
            throw RepoException(
                "Actions are owner-key gated. This device is signed in as \(account), not \(repo.owner).")
        }
        if session.pubkey.dartTrimmed != identity.publicKeyB64url {
            throw RepoException(
                "Actions are owner-key gated. Pair this mobile device with the owner key before viewing workflow logs.")
        }
        let ts = String(nowMillis)
        let sig = identity.sign(Array(Utf8.encode(actionsReadCanonical(owner: repo.owner, name: repo.name, ts: ts))))
        return RepoOwnerProof(ownerAccount: account, ts: ts, sig: sig)
    }

    /// The owner checks and signature of `_requestDesktopCommand`.
    public static func desktopCommandProof(
        repo: Repository, command: String, target: String, session: AuthSession?, identity: DeviceIdentity?,
        nowMillis: Int64
    ) throws -> RepoOwnerProof {
        try desktopCommandProof(
            owner: desktopCommandOwner(repo, account: account(session)), name: repo.name, command: command,
            target: target, session: session, identity: identity, nowMillis: nowMillis)
    }

    public static func desktopCommandOwner(_ repo: Repository, account: String) -> String {
        account == repo.owner.dartTrimmed.lowercased() ? repo.owner : repo.displayOwner
    }

    public static func desktopCommandProof(
        owner: String, name: String, command: String, target: String, session: AuthSession?, identity: DeviceIdentity?,
        nowMillis: Int64
    ) throws -> RepoOwnerProof {
        let account = account(session)
        guard let session, let identity, !account.isEmpty else {
            throw RepoException("Sign in with the repo owner account to request desktop control.")
        }
        if session.pubkey.dartTrimmed != identity.publicKeyB64url {
            throw RepoException("Pair this mobile device with the owner key before requesting desktop control.")
        }
        let ts = String(nowMillis)
        let canonical = desktopCommandCanonical(owner: owner, name: name, command: command, target: target, ts: ts)
        let sig = identity.sign(Array(Utf8.encode(canonical)))
        return RepoOwnerProof(ownerAccount: account, ts: ts, sig: sig)
    }
}

/// Who is asking: the session and the device key, read when needed (the
/// session changes on sign-in).
@MainActor
public struct RepoSigner {
    public let session: @MainActor () -> AuthSession?
    public let identity: DeviceIdentity?
    public let clock: Clock

    public init(session: @escaping @MainActor () -> AuthSession?, identity: DeviceIdentity?, clock: Clock = SystemClock()) {
        self.session = session
        self.identity = identity
        self.clock = clock
    }

    /// `_AgentsTabState._ownerAccount`: the signed-in account, else the owner.
    public func ownerAccount(for repo: Repository) -> String {
        let node = RepoOwnerSigning.account(session())
        return node.isEmpty ? repo.owner : node
    }
}

/// `_requestDesktopCommand`: queues a signed request the paired desktop app
/// asks its owner to approve. Answers the inline notice text.
@MainActor
public enum RepoDesktopCommands {
    nonisolated public static let defaultFailure = "Desktop request was not queued."

    public static func request(
        client: RepoClient, signer: RepoSigner, repo: Repository, command: String, target: String,
        payload: JSONObject, successMessage: String, failureMessage: String = defaultFailure
    ) async -> String {
        await DesktopCommandRequests.request(
            client: client, signer: signer,
            owner: RepoOwnerSigning.desktopCommandOwner(repo, account: RepoOwnerSigning.account(signer.session())),
            name: repo.name, command: command, target: target,
            payload: payload, successMessage: successMessage, failureMessage: failureMessage)
    }

    /// `_ActionRunDetailScreen._requestRerun`.
    public static func rerunPayload(_ run: ActionRun) -> JSONObject {
        var payload: JSONObject = ["runId": .int(Int64(run.id))]
        if !run.workflowPath.isEmpty { payload["workflowPath"] = .string(run.workflowPath) }
        return payload
    }

    public static let rerunSuccess = "Desktop request queued. Approve it in the ForkMesh desktop app to rerun."

    /// `_ReleasesTab._requestRelease`.
    public static func releasePayload(_ release: RepoRelease) -> JSONObject {
        var payload: JSONObject = ["tag": .string(release.tag)]
        if !release.channel.isEmpty { payload["channel"] = .string(release.channel) }
        return payload
    }
}

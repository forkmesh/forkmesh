import FMAccount
import FMApi
import FMBase
import FMCrypto
import Foundation
import Observation

public enum DesktopCommandContract {
    public static let signingDomain = "forkmesh-desktop-command-v1"

    public static let pullCommands = [
        "pull.merge", "pull.merge_delete", "pull.fix_agent", "pull.close", "pull.reopen", "pull.delete",
    ]
    public static let runCommands = ["action.rerun"]
    public static let releaseCommands = ["release.draft", "release.notes", "release.create", "release.publish"]
    public static var commands: [String] { pullCommands + runCommands + releaseCommands }

    private static let pullTarget = try! NSRegularExpression(pattern: "\\Apull:[1-9][0-9]{0,8}\\z")
    private static let runTarget = try! NSRegularExpression(pattern: "\\Arun:[A-Za-z0-9][A-Za-z0-9._-]{0,127}\\z")
    private static let releaseTarget = try! NSRegularExpression(pattern: "\\Arelease:[A-Za-z0-9][A-Za-z0-9._+-]{0,127}\\z")

    public static func canonical(owner: String, name: String, command: String, target: String, ts: String) -> String {
        [signingDomain, owner, name, command, target, ts].joined(separator: "\n")
    }

    public static func pullTarget(_ number: Int) -> String { "pull:\(number)" }
    public static func runTarget(_ runId: Int) -> String { "run:\(runId)" }
    public static func releaseTarget(_ tag: String) -> String { "release:\(tag)" }

    public static func isValid(command: String, target: String) -> Bool {
        let regex: NSRegularExpression
        if pullCommands.contains(command) {
            regex = pullTarget
        } else if runCommands.contains(command) {
            regex = runTarget
        } else if releaseCommands.contains(command) {
            regex = releaseTarget
        } else {
            return false
        }
        return regex.firstMatch(in: target, range: NSRange(target.startIndex..., in: target)) != nil
    }

    public static let unsupportedMessage = "This desktop request is not supported."
}

@MainActor
public protocol DesktopCommandRouteProbing: AnyObject {
    func desktopCommandRouteAnswered(_ owner: String, _ name: String) async -> Bool?
}

extension ApiService: DesktopCommandRouteProbing {}

@MainActor
@Observable
public final class DesktopCommandAvailability {
    public static let shared = DesktopCommandAvailability()
    public static let recheckMs: Int64 = 15 * 60 * 1000

    public private(set) var available = false
    @ObservationIgnored private var checkedAtMs: Int64 = 0
    @ObservationIgnored private var inFlight = false
    @ObservationIgnored private let clock: Clock

    public init(clock: Clock = SystemClock()) {
        self.clock = clock
    }

    @discardableResult
    public func refresh(owner: String, name: String, probe: DesktopCommandRouteProbing) async -> Bool {
        if available || inFlight { return available }
        let now = clock.nowMillis
        if checkedAtMs > 0 && now - checkedAtMs < DesktopCommandAvailability.recheckMs { return available }
        inFlight = true
        let answer = await probe.desktopCommandRouteAnswered(owner, name)
        inFlight = false
        guard let answer else { return available }
        checkedAtMs = clock.nowMillis
        available = answer
        return available
    }

    public func reset() {
        available = false
        checkedAtMs = 0
    }
}

@MainActor
public enum DesktopCommandRequests {
    public static func request(
        client: RepoClient, signer: RepoSigner, owner: String, name: String, command: String, target: String,
        payload: JSONObject, successMessage: String, failureMessage: String = RepoDesktopCommands.defaultFailure
    ) async -> String {
        guard DesktopCommandContract.isValid(command: command, target: target) else {
            return DesktopCommandContract.unsupportedMessage
        }
        do {
            let proof = try RepoOwnerSigning.desktopCommandProof(
                owner: owner, name: name, command: command, target: target, session: signer.session(),
                identity: signer.identity, nowMillis: signer.clock.nowMillis)
            let result = try await client.desktopCommand(
                owner, name, ownerAccount: proof.ownerAccount, command: command, target: target, ts: proof.ts,
                sig: proof.sig, payload: payload)
            return result.ok ? successMessage : failureMessage
        } catch {
            return dartErrorText(error)
        }
    }

    public static func rerun(client: RepoClient, signer: RepoSigner, repo: RepoRef, runId: Int, workflowPath: String) async -> String {
        var payload: JSONObject = ["runId": .int(Int64(runId))]
        if !workflowPath.isEmpty { payload["workflowPath"] = .string(workflowPath) }
        return await request(
            client: client, signer: signer, owner: repo.owner, name: repo.name, command: "action.rerun",
            target: DesktopCommandContract.runTarget(runId), payload: payload,
            successMessage: RepoDesktopCommands.rerunSuccess)
    }

    public static let rerunLabel = "Request rerun on desktop"
}

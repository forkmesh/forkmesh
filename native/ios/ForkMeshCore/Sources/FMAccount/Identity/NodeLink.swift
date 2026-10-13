import FMBase
import Foundation
import Observation

public struct NodeLinkGrant: Equatable, Sendable {
    public let node: String
    public let ts: String
    public let sig: String
    public let host: String

    public var timestampMillis: Int64 { Int64(ts) ?? 0 }

    public func ageMillis(nowMillis: Int64) -> Int64 { nowMillis - timestampMillis }

    public func isFresh(nowMillis: Int64) -> Bool { abs(ageMillis(nowMillis: nowMillis)) <= NodeLink.maxSkewMillis }

    public func body(user: String) -> JSONObject {
        ["nodeName": .string(node), "user": .string(user.lowercased()), "ts": .string(ts), "sig": .string(sig)]
    }
}

public enum NodeLinkParse: Equatable, Sendable {
    case grant(NodeLinkGrant)
    case installerCode(String)
    case invalid(String)
}

public enum NodeLinkOutcome: Equatable, Sendable {
    case linked(node: String, nodes: [String])
    case alreadyLinked(node: String)
    case failed(String)
}

public enum NodeLink {
    public static let maxSkewMillis: Int64 = 5 * 60 * 1_000
    public static let path = "/api/accounts/link-grant"

    public static func isNodeName(_ name: String) -> Bool {
        guard !name.isEmpty, name.count <= 63 else { return false }
        guard let first = name.unicodeScalars.first, ("a"..."z").contains(first) || ("0"..."9").contains(first) else { return false }
        return name.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" || $0 == "_" }
    }

    public static func parse(_ scanned: String) -> NodeLinkParse {
        let text = scanned.trimmingCharacters(in: .whitespacesAndNewlines)
        if text.isEmpty { return .invalid("Scan the code the desktop shows, or paste its link.") }
        if text.count == 6, text.allSatisfy({ $0.isASCII && $0.isNumber }) { return .installerCode(text) }
        guard text.count <= 2_048, let components = URLComponents(string: text) else {
            return .invalid("That is not a ForkMesh link.")
        }
        let items = components.queryItems ?? []
        func value(_ name: String) -> String { items.first { $0.name == name }?.value ?? "" }
        let node = value("link_node").lowercased()
        let ts = value("link_ts")
        let sig = value("link_sig")
        guard !node.isEmpty || !ts.isEmpty || !sig.isEmpty else { return .invalid("That link does not link a node.") }
        guard isNodeName(node) else { return .invalid("The link names no valid node.") }
        guard !ts.isEmpty, ts.count <= 20, ts.allSatisfy({ $0.isASCII && $0.isNumber }) else {
            return .invalid("The link has no valid time.")
        }
        guard !sig.isEmpty, sig.count <= 200 else { return .invalid("The link is not signed.") }
        let scheme = components.scheme?.lowercased() ?? ""
        guard scheme == "https" || scheme == "http" || scheme == "forkmesh" else { return .invalid("That is not a ForkMesh link.") }
        return .grant(NodeLinkGrant(node: node, ts: ts, sig: sig, host: components.host?.lowercased() ?? ""))
    }

    public static func hostMatches(_ grant: NodeLinkGrant, serverOrigin: String) -> Bool {
        guard !grant.host.isEmpty else { return true }
        let host = URLComponents(string: serverOrigin)?.host?.lowercased() ?? ""
        if host.isEmpty { return true }
        return host == grant.host || NodeLink.sameInstance(host, grant.host)
    }

    static func sameInstance(_ a: String, _ b: String) -> Bool {
        AppEnv.canonicalProjectId(a) == AppEnv.canonicalProjectId(b)
    }

    public static func outcome(_ map: JSONObject) -> NodeLinkOutcome {
        let node = map.firstStr("nodeId", "node")
        if map.bool("alreadyLinked") { return .alreadyLinked(node: node) }
        if map.bool("linked") { return .linked(node: node, nodes: map.strings("nodes")) }
        return .failed("The relay did not link the node.")
    }
}

@MainActor
@Observable
public final class NodeLinkModel {
    public private(set) var busy = false
    public private(set) var outcome: NodeLinkOutcome?
    public private(set) var pending: NodeLinkGrant?
    public private(set) var error = ""

    @ObservationIgnored private let client: AccountEndpointCalling
    @ObservationIgnored private let user: @MainActor () -> String
    @ObservationIgnored private let clock: Clock

    public init(client: AccountEndpointCalling, user: @escaping @MainActor () -> String, clock: Clock = SystemClock()) {
        self.client = client
        self.user = user
        self.clock = clock
    }

    public func review(_ scanned: String) {
        outcome = nil
        pending = nil
        switch NodeLink.parse(scanned) {
        case .grant(let grant):
            if !grant.isFresh(nowMillis: clock.nowMillis) {
                error = "That link has expired. Ask the desktop for a new one."
            } else if !NodeLink.hostMatches(grant, serverOrigin: client.serverOrigin) {
                error = "That link is for \(grant.host), not the server this app uses."
            } else {
                error = ""
                pending = grant
            }
        case .installerCode:
            error = "That is an installer code. Enter it on the desktop that installed the node."
        case .invalid(let message):
            error = message
        }
    }

    public func cancel() {
        pending = nil
    }

    @discardableResult
    public func confirm() async -> NodeLinkOutcome? {
        guard let grant = pending else { return nil }
        let account = user()
        guard NodeLink.isNodeName(account.lowercased()) else {
            error = "Sign in to a user account first."
            return nil
        }
        busy = true
        defer { busy = false }
        do {
            let result = NodeLink.outcome(try await client.accountCall("POST", NodeLink.path, grant.body(user: account)))
            outcome = result
            pending = nil
            if case .failed(let message) = result { error = message } else { error = "" }
            return result
        } catch {
            self.error = AccountErrorText.text(error, fallback: "Could not link the node.")
            return nil
        }
    }
}

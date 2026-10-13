import Foundation
import FMBase

public struct NetworkStats: Equatable, Hashable, Sendable {
    public var nodesOnline: Int
    public var hostsOnline: Int
    public var repos: Int

    public init(nodesOnline: Int = 0, hostsOnline: Int = 0, repos: Int = 0) {
        self.nodesOnline = nodesOnline
        self.hostsOnline = hostsOnline
        self.repos = repos
    }

    /// Both stats shapes: `onlineNodes` as a list wins over the counters.
    public init(json j: JSONObject) {
        let nodesOnline: Int
        if let online = j["onlineNodes"]?.arrayValue {
            nodesOnline = online.count
        } else {
            nodesOnline = ModelJSON.int(ModelJSON.coalesce(j["nodesOnline"], j["clients"]) ?? .int(0))
        }
        self.init(
            nodesOnline: nodesOnline,
            hostsOnline: ModelJSON.int(ModelJSON.coalesce(j["hostsOnline"], j["hosts"]) ?? .int(0)),
            repos: ModelJSON.int(ModelJSON.coalesce(j["repos"], j["repositories"]) ?? .int(0))
        )
    }
}

public struct NetworkLeaderboards: Equatable, Hashable, Sendable {
    public var fundsMainnodes: [FundsReceivedEntry]
    public var fundsContributors: [FundsReceivedEntry]
    public var fundsProjects: [FundsReceivedEntry]

    public init(
        fundsMainnodes: [FundsReceivedEntry] = [], fundsContributors: [FundsReceivedEntry] = [],
        fundsProjects: [FundsReceivedEntry] = []
    ) {
        self.fundsMainnodes = fundsMainnodes
        self.fundsContributors = fundsContributors
        self.fundsProjects = fundsProjects
    }

    public init(json: JSONObject) {
        func board(_ key: String) -> [FundsReceivedEntry] { json.maps(key).map(FundsReceivedEntry.init(json:)) }
        self.init(
            fundsMainnodes: board("fundsMainnodes"),
            fundsContributors: board("fundsContributors"),
            fundsProjects: board("fundsProjects")
        )
    }

    public var isEmpty: Bool { fundsMainnodes.isEmpty && fundsContributors.isEmpty && fundsProjects.isEmpty }
}

public struct FundsReceivedEntry: Equatable, Hashable, Sendable {
    public var name: String
    public var lamports: Int
    public var sol: Double

    public init(name: String = "", lamports: Int = 0, sol: Double = 0) {
        self.name = name
        self.lamports = lamports
        self.sol = sol
    }

    public init(json: JSONObject) {
        self.init(
            name: ModelJSON.firstString(json, ["name", "node", "account", "project", "repo"]),
            lamports: ModelJSON.int(json["lamports"]),
            sol: ModelJSON.double(json["sol"])
        )
    }

    public var amountLabel: String {
        if sol > 0 { return "\(ModelJSON.trimDouble(sol)) SOL (\(lamports) lamports)" }
        return "\(lamports) lamports"
    }
}

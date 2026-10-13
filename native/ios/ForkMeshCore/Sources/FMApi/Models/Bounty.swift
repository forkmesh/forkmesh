import Foundation
import FMBase

public struct BountyWallet: Equatable, Hashable, Sendable {
    public var address: String
    public var balanceLamports: Int
    public var balanceSol: Double
    public var payUri: String

    public init(address: String = "", balanceLamports: Int = 0, balanceSol: Double = 0, payUri: String = "") {
        self.address = address
        self.balanceLamports = balanceLamports
        self.balanceSol = balanceSol
        self.payUri = payUri
    }

    /// Anything but an object is the empty wallet.
    public init(json data: JSONValue?) {
        let source = data?.objectValue ?? JSONObject()
        self.init(
            address: ModelJSON.firstString(source, ["address", "walletAddress"]),
            balanceLamports: ModelJSON.int(source["balanceLamports"]),
            balanceSol: ModelJSON.double(source["balanceSol"]),
            payUri: ModelJSON.firstString(source, ["uri", "payUri"])
        )
    }

    public var hasWallet: Bool { !address.dartTrimmed.isEmpty }

    public var balanceLabel: String {
        if balanceSol > 0 { return "\(ModelJSON.trimDouble(balanceSol)) SOL (\(balanceLamports) lamports)" }
        return "\(balanceLamports) lamports"
    }

    public var shortAddress: String { shortenAddress(address) }

    public var explorerAddressUrl: String { solanaExplorerAddressUrl(address) }
}

/// "Wallet...111111" for anything longer than 16 UTF-16 units, trimmed.
func shortenAddress(_ address: String) -> String {
    let clean = address.dartTrimmed
    let length = DartCore.length(clean)
    if length <= 16 { return clean }
    let units = Array(clean.utf16)
    return String(decoding: units[0..<6], as: UTF16.self) + "..."
        + String(decoding: units[(length - 6)...], as: UTF16.self)
}

public struct IssueBounty: Equatable, Hashable, Sendable {
    public var address: String
    public var status: String
    public var amountUsd: Double
    public var requiredLamports: Int
    public var receivedLamports: Int
    public var amountSol: Double
    public var payUri: String
    public var payee: String
    public var payoutSig: String
    public var confirmed: Bool

    public init(
        address: String = "", status: String = "open", amountUsd: Double = 0, requiredLamports: Int = 0,
        receivedLamports: Int = 0, amountSol: Double = 0, payUri: String = "", payee: String = "",
        payoutSig: String = "", confirmed: Bool = false
    ) {
        self.address = address
        self.status = status
        self.amountUsd = amountUsd
        self.requiredLamports = requiredLamports
        self.receivedLamports = receivedLamports
        self.amountSol = amountSol
        self.payUri = payUri
        self.payee = payee
        self.payoutSig = payoutSig
        self.confirmed = confirmed
    }

    /// A bounty record, or one wrapped in `bounty` / `funding`; anything but an
    /// object is an open bounty with no funding.
    public init(json data: JSONValue?) {
        let source = data?.objectValue ?? JSONObject()
        let nested = source["bounty"]?.objectValue ?? source["funding"]?.objectValue ?? source
        let status = ModelJSON.firstString(nested, ["status", "bountyStatus"])
        self.init(
            address: ModelJSON.firstString(nested, ["address", "bountyAddress"]),
            status: status.isEmpty ? "open" : status,
            amountUsd: ModelJSON.double(ModelJSON.coalesce(nested["amountUsd"], nested["bountyUsd"])),
            requiredLamports: ModelJSON.int(
                ModelJSON.coalesce(nested["requiredLamports"], nested["bountyRequiredLamports"])),
            receivedLamports: ModelJSON.int(
                ModelJSON.coalesce(nested["receivedLamports"], nested["bountyReceivedLamports"])),
            amountSol: ModelJSON.double(ModelJSON.coalesce(nested["amountSol"], nested["bountyAmountSol"])),
            payUri: ModelJSON.firstString(nested, ["uri", "payUri", "bountyPayUri"]),
            payee: ModelJSON.firstString(nested, ["payee", "bountyPayee"]),
            payoutSig: ModelJSON.firstString(nested, ["payoutSig", "bountyPayoutSig"]),
            confirmed: nested.bool("confirmed")
        )
    }

    public var hasFunding: Bool {
        !address.isEmpty || amountUsd > 0 || requiredLamports > 0 || receivedLamports > 0 || !payoutSig.isEmpty
    }

    public var statusLabel: String {
        let clean = status.dartTrimmed
        if !clean.isEmpty { return clean }
        if !payoutSig.isEmpty { return "paid" }
        if requiredLamports > 0 && receivedLamports >= requiredLamports { return "funded" }
        return "open"
    }

    public var progressLabel: String {
        if requiredLamports > 0 { return "\(receivedLamports) / \(requiredLamports) lamports" }
        if receivedLamports > 0 { return "\(receivedLamports) lamports received" }
        return ""
    }
}

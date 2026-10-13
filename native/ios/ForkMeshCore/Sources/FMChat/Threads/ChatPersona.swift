import FMBase
import FMCrypto

public func guestChatName(_ nodeKey: String) -> String {
    if nodeKey.isEmpty { return "Guest" }
    let digest = Sha256.hash(Array(nodeKey.utf8))
    var value: UInt64 = 0
    for i in 0..<4 { value = (value << 8) | UInt64(digest[i]) }
    return "Guest \(1000 + value % 9000)"
}

public struct ChatPersona: Equatable, Sendable {
    public let name: String
    public let accountKind: String
    public let ownerUser: String

    public init(name: String, accountKind: String, ownerUser: String = "") {
        self.name = name
        self.accountKind = accountKind
        self.ownerUser = ownerUser
    }

    public var isGuest: Bool { accountKind == "guest" }

    public func stamp(_ frame: inout JSONObject) {
        frame["accountKind"] = .string(accountKind)
        if !ownerUser.isEmpty { frame["ownerUser"] = .string(ownerUser) }
    }

    public static func resolve(nodeKey: String, account: String, displayName: String = "") -> ChatPersona {
        let user = account.dartTrimmed
        if user.isEmpty { return ChatPersona(name: guestChatName(nodeKey), accountKind: "guest") }
        let display = displayName.dartTrimmed
        return ChatPersona(name: display.isEmpty ? user : display, accountKind: "user", ownerUser: user)
    }
}

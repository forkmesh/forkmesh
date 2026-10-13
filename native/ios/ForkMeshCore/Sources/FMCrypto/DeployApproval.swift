import FMBase
import Foundation

public enum DeployApproval {
    public static let prefix = "forkmesh-deploy-approval-v1"

    public static func message(
        kind: String, owner: String, repo: String, target: String, base: String, head: String,
        requestId: String, ts: Int64
    ) -> String {
        [
            prefix, kind, owner.trimmingCharacters(in: .whitespacesAndNewlines).lowercased(), repo, target, base, head,
            requestId, String(ts),
        ].joined(separator: "\n")
    }

    public static func sign(
        _ identity: DeviceIdentity, kind: String, owner: String, repo: String, target: String,
        base: String = "", head: String = "", requestId: String, ts: Int64
    ) -> JSONObject? {
        let publicKey = identity.publicKeyBytes
        guard publicKey.count == Ed25519.publicKeyBytes, ts > 0 else { return nil }
        let text = message(
            kind: kind, owner: owner, repo: repo, target: target, base: base, head: head, requestId: requestId, ts: ts)
        let signature = identity.sign(Array(text.utf8))
        guard !signature.isEmpty else { return nil }
        return [
            "publicKey": .string(B64.encodeUrlNoPadding(publicKey)),
            "ts": .int(ts),
            "signature": .string(signature),
        ]
    }
}

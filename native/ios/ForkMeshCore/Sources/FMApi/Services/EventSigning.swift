import Foundation

public enum EventSigning {
    public static func repoKey(_ owner: String, _ name: String) -> String {
        let o = owner.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let n = name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !o.isEmpty, !n.isEmpty else { return "" }
        for part in [o, n] where part.contains("/") || part.contains("\n") || part.contains("\0") {
            return ""
        }
        return o + "/" + n
    }

    public static func prefix(_ kind: String, owner: String, name: String) -> String {
        let key = repoKey(owner, name)
        return key.isEmpty ? "forkmesh-\(kind)-v1\n" : "forkmesh-\(kind)-v2\n\(key)\n"
    }
}

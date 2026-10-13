import Foundation

public struct ProjectInvitation: Equatable, Sendable {
    public var projectName: String
    public var inviter: String
    public var origin: String

    public init(projectName: String, inviter: String, origin: String) {
        self.projectName = projectName
        self.inviter = inviter
        self.origin = origin
    }
}

public enum ProjectInvite {
    public struct Recipients: Equatable, Sendable {
        public let addresses: [String]
        public let invalid: String
    }

    public static func host(_ invitation: ProjectInvitation) -> String {
        let origin = trimmedOrigin(invitation)
        let host: String
        if let range = origin.range(of: "://") {
            host = String(origin[range.upperBound...])
        } else {
            host = origin
        }
        return host.isEmpty ? "ForkMesh" : host
    }

    public static func invitationLink(_ invitation: ProjectInvitation) -> String {
        let origin = trimmedOrigin(invitation)
        guard origin.hasPrefix("https://") || origin.hasPrefix("http://") else { return "" }
        let inviter = invitation.inviter.trimmingCharacters(in: .whitespacesAndNewlines)
        if inviter.isEmpty { return "\(origin)/signup" }
        return "\(origin)/r/\(percentEncode(inviter))"
    }

    public static func directMessageText(_ invitation: ProjectInvitation) -> String {
        let host = host(invitation)
        return "I’d like you to join \(projectLabel(invitation)) on \(host). To open it in ForkMesh, "
            + "choose Add project and use \(host) as its instance."
    }

    public static func emailSubject(_ invitation: ProjectInvitation) -> String {
        let inviter = invitation.inviter.trimmingCharacters(in: .whitespacesAndNewlines)
        if inviter.isEmpty { return "You’re invited to \(projectLabel(invitation)) on ForkMesh" }
        return "\(inviter) invited you to \(projectLabel(invitation)) on ForkMesh"
    }

    public static func emailBody(_ invitation: ProjectInvitation) -> String {
        let inviter = invitation.inviter.trimmingCharacters(in: .whitespacesAndNewlines)
        let host = host(invitation)
        let link = invitationLink(invitation)
        var body = "Hi,\n\n"
        body += "I’d like you to join \(projectLabel(invitation)) on ForkMesh. It lives on the instance \(host).\n\n"
        if !link.isEmpty { body += "Accept the invitation and create your account here:\n\(link)\n\n" }
        body += "Then, in the ForkMesh app, choose Add project and use \(host) as its instance. "
            + "Reply with your account name and I’ll add you.\n"
        if !inviter.isEmpty { body += "\n\(inviter)\n" }
        return body
    }

    public static func emailRecipients(_ input: String) -> Recipients {
        var out: [String] = []
        let parts = input.components(separatedBy: CharacterSet(charactersIn: ",;").union(.whitespacesAndNewlines))
        for part in parts where !part.isEmpty {
            guard isAddress(part) else { return Recipients(addresses: [], invalid: part) }
            if !out.contains(where: { $0.caseInsensitiveCompare(part) == .orderedSame }) { out.append(part) }
        }
        return Recipients(addresses: out, invalid: "")
    }

    public static func looksLikeEmail(_ text: String) -> Bool {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.contains("@"), !trimmed.hasPrefix("@") else { return false }
        let parsed = emailRecipients(trimmed)
        return parsed.invalid.isEmpty && !parsed.addresses.isEmpty
    }

    public static func mailtoUrl(_ recipients: [String], _ invitation: ProjectInvitation) -> String {
        let to = recipients.map { percentEncode($0.trimmingCharacters(in: .whitespaces), keep: "@+") }
            .joined(separator: ",")
        return "mailto:\(to)?subject=\(mailtoField(emailSubject(invitation)))&body=\(mailtoField(emailBody(invitation)))"
    }

    private static func trimmedOrigin(_ invitation: ProjectInvitation) -> String {
        var origin = invitation.origin.trimmingCharacters(in: .whitespacesAndNewlines)
        while origin.hasSuffix("/") { origin.removeLast() }
        return origin
    }

    private static func projectLabel(_ invitation: ProjectInvitation) -> String {
        let name = invitation.projectName.trimmingCharacters(in: .whitespacesAndNewlines)
        return name.isEmpty ? "my project" : "“\(name)”"
    }

    private static func isAddress(_ part: String) -> Bool {
        let pieces = part.split(separator: "@", omittingEmptySubsequences: false)
        guard pieces.count == 2, !pieces[0].isEmpty else { return false }
        let forbidden = CharacterSet(charactersIn: "<>\"").union(.whitespacesAndNewlines)
        guard part.rangeOfCharacter(from: forbidden) == nil else { return false }
        let domain = pieces[1].split(separator: ".", omittingEmptySubsequences: false)
        return domain.count >= 2 && domain.allSatisfy { !$0.isEmpty }
    }

    private static func mailtoField(_ value: String) -> String {
        let normalized = value.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\n", with: "\r\n")
        return percentEncode(normalized)
    }

    private static func percentEncode(_ value: String, keep: String = "") -> String {
        var out = ""
        for byte in value.utf8 {
            let scalar = Unicode.Scalar(byte)
            let char = Character(scalar)
            let unreserved = (byte >= 0x41 && byte <= 0x5A) || (byte >= 0x61 && byte <= 0x7A)
                || (byte >= 0x30 && byte <= 0x39) || "-._~".contains(char)
            if byte < 0x80 && (unreserved || keep.contains(char)) {
                out.append(char)
            } else {
                out += String(format: "%%%02X", byte)
            }
        }
        return out
    }
}

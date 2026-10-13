import FMBase
import Foundation

public struct CrmNoteDraft: Equatable, Sendable, Identifiable {
    public var contactId: String
    public var name: String
    public var to: String
    public var subject: String
    public var body: String
    public var thread: String
    public var mastodon: Bool

    public var id: String { contactId }

    public static let mark = "DRAFT, not sent."

    public static func markRange(in notes: String) -> Range<String.Index>? {
        var start = notes.startIndex
        while let found = notes.range(of: mark, range: start..<notes.endIndex) {
            if found.lowerBound == notes.startIndex || notes[notes.index(before: found.lowerBound)] == "\n" { return found }
            start = found.upperBound
        }
        return nil
    }

    public static func drafts(_ contacts: [CrmContact], activity: [CrmActivity] = []) -> [CrmNoteDraft] {
        contacts.compactMap { draft($0, activity: activity) }
    }

    public static func draft(_ contact: CrmContact, activity: [CrmActivity] = []) -> CrmNoteDraft? {
        let notes = contact.notes
        guard let found = markRange(in: notes) else { return nil }
        let tail = notes[found.lowerBound...]
        let paragraph = tail.range(of: "\n\n").map { String(tail[..<$0.lowerBound]) } ?? String(tail)
        var lines = paragraph.components(separatedBy: "\n")
        let head = lines.removeFirst()
        let mastodon = head.range(of: "mastodon", options: .caseInsensitive) != nil
        var subject = ""
        if let first = lines.first, first.hasPrefix("Subject:") {
            subject = String(first.dropFirst("Subject:".count)).trimmingCharacters(in: .whitespaces)
            lines.removeFirst()
        }
        let body = lines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
        if body.isEmpty { return nil }
        var thread = ""
        var to = contact.email
        if mastodon {
            to = contact.mastodon
            thread = activity.first { $0.contactId == contact.id && $0.provider == "mastodon" && $0.providerUrl.hasPrefix("https://") }?
                .providerUrl ?? ""
            if thread.isEmpty, let match = notes.range(of: #"https://[A-Za-z0-9.-]+/@[A-Za-z0-9_.]+/[0-9]{6,}"#, options: .regularExpression) {
                thread = String(notes[match])
            }
        }
        return CrmNoteDraft(
            contactId: contact.id, name: contact.name.isEmpty ? to : contact.name, to: to, subject: subject, body: body,
            thread: thread, mastodon: mastodon)
    }

    public static func markedSent(_ notes: String, day: String, by: String = "") -> String? {
        guard let found = markRange(in: notes) else { return nil }
        var copy = notes
        copy.replaceSubrange(found, with: "DRAFT, sent \(day)" + (by.isEmpty ? "" : " by \(by)") + ".")
        return copy
    }

    public var canSend: Bool { mastodon ? !thread.isEmpty : to.contains("@") }

    public var kindTitle: String { mastodon ? "Mastodon reply" : "Email" }

    public func emailDraft(_ contact: CrmContact?) -> CrmEmailDraft {
        var draft = CrmEmailDraft(contact: contact, to: to)
        draft.subject = subject
        draft.body = body
        draft.noteDraft = true
        return draft
    }
}

public struct CrmZohoCode: Equatable, Sendable {
    public var clientId = ""
    public var clientSecret = ""
    public var code = ""
    public var region = "com"

    public init() {}

    public static let regions = ["com", "eu", "in", "com.au", "jp", "ca"]
    public static let consoleURL = URL(string: "https://api-console.zoho.com/")!
    public static let scopes = "ZohoMail.messages.READ,ZohoMail.folders.READ,ZohoMail.accounts.READ"

    public var isComplete: Bool {
        ![clientId, clientSecret, code].contains { $0.trimmingCharacters(in: .whitespaces).isEmpty }
    }

    public var body: JSONObject {
        [
            "clientId": .string(clientId.trimmingCharacters(in: .whitespaces)),
            "clientSecret": .string(clientSecret.trimmingCharacters(in: .whitespaces)),
            "code": .string(code.trimmingCharacters(in: .whitespaces)), "region": .string(region),
        ]
    }
}

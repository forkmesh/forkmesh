// The discussion and fediverse logic: category labels, new discussion
// validation, the discussion row, detail chips and copy, timeline tiles,
// federated replies copy and each reply's author, provenance and backlink
// checks. After native/android core/.../collab/DiscussionThread.kt.

import FMApi
import FMBase
import Foundation

/// `_discussionCategoryLabels`, in menu order.
public let discussionCategoryLabels: [(key: String, label: String)] = [
    ("general", "General"),
    ("ideas", "Ideas"),
    ("help", "Help"),
    ("show-and-tell", "Show & tell"),
]

public enum DiscussionCard {
    public static func subtitle(_ discussion: RepoDiscussion) -> String {
        "#\(discussion.number) · \(discussion.category)" + (discussion.author.isEmpty ? "" : " · \(discussion.author)")
    }
}

public enum DiscussionDetail {
    public static let noDescription = "No description provided."
    public static let timelineEmpty = "No discussion replies have been published yet."
    public static let replyTitle = "Signed discussion reply"
    public static let replyCopy = "Replies are signed and sent to the discussion inbox, pending the repo owner applying them."
    public static let replyHint = "Write a signed reply"
    public static let replyLabel = "Submit reply"
    public static let guestAction = "reply to discussions"
    /// `_DiscussionTimelineTile`'s title.
    public static let eventTitle = "Reply"

    public static func title(_ number: Int) -> String { "Discussion #\(number)" }

    public static func chips(_ discussion: RepoDiscussion, repoFullName: String) -> [CollabChip] {
        var chips = [CollabChip("forum_outlined", "discussion"), CollabChip("category_outlined", discussion.category)]
        if !discussion.author.isEmpty { chips.append(CollabChip("person_outline", discussion.author)) }
        chips.append(CollabChip("folder_outlined", repoFullName))
        return chips
    }

    public static func body(_ discussion: RepoDiscussion) -> String {
        discussion.body.isEmpty ? noDescription : discussion.body
    }
}

/// `showNewDiscussionDialog`.
public enum NewDiscussionComposer {
    public static let title = "New discussion"
    public static let guestAction = "start a discussion"
    public static let defaultCategory = "general"

    /// The dialog's `ok` check: nothing is sent without a title and a body.
    public static func ready(title: String, body: String) -> Bool {
        !title.dartTrimmed.isEmpty && !body.dartTrimmed.isEmpty
    }
}

/// `_FederatedRepliesCard` and `_FederatedReplyTile`.
public enum FederatedThread {
    public static let title = "Fediverse thread"
    public static let intro =
        "Remote ActivityPub replies are shown with their original instance and backlink. They are separate from ForkMesh’s signed native event log."
    public static let unavailableText = "Remote replies are currently unavailable."
    public static let empty = "No remote ActivityPub replies yet."
    public static let notNative = "Remote ActivityPub · not a signed native event"
    public static let edited = "(edited)"
    public static let original = "Original"

    public static func author(_ reply: FederatedReply) -> String {
        if !reply.authorName.isEmpty { return reply.authorName }
        if !reply.author.isEmpty { return reply.author }
        return "Remote participant"
    }

    public static func provenance(_ reply: FederatedReply) -> String {
        [reply.sourceSoftware, reply.sourceInstance].filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public static func unavailable(_ reply: FederatedReply) -> Bool { reply.tombstone || reply.moderated }

    /// `reply.depth * 14.0` of left padding.
    public static func indent(_ reply: FederatedReply) -> Double { Double(reply.depth) * 14 }

    /// A backlink with an https scheme, a host and no user info: only such
    /// links open in the browser. Parsed leniently, so a space in the path
    /// does not hide the button.
    public static func canOpenBacklink(_ backlink: String) -> Bool {
        let units = Array(backlink.utf16)
        // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
        guard let first = units.first, isAlpha(first) else { return false }
        var i = 1
        while i < units.count, isAlpha(units[i]) || isDigit(units[i]) || units[i] == 0x2B || units[i] == 0x2D || units[i] == 0x2E {
            i += 1
        }
        guard i < units.count, units[i] == 0x3A else { return false }
        let scheme = String(decoding: units[0..<i], as: UTF16.self).lowercased()
        if scheme != "https" { return false }
        i += 1
        guard i + 1 < units.count, units[i] == 0x2F, units[i + 1] == 0x2F else { return false }
        i += 2
        var end = i
        while end < units.count, units[end] != 0x2F, units[end] != 0x3F, units[end] != 0x23 { end += 1 }
        let authority = Array(units[i..<end])
        if let at = authority.lastIndex(of: 0x40), at > 0 { return false }
        let hostPort = authority.lastIndex(of: 0x40).map { Array(authority[($0 + 1)...]) } ?? authority
        let host: [UInt16]
        if hostPort.first == 0x5B {
            guard let close = hostPort.firstIndex(of: 0x5D) else { return false }
            host = Array(hostPort[1..<close])
        } else if let colon = hostPort.firstIndex(of: 0x3A) {
            host = Array(hostPort[..<colon])
        } else {
            host = hostPort
        }
        return !host.isEmpty
    }

    /// The URL to hand to the browser, when `canOpenBacklink`.
    public static func backlinkURL(_ backlink: String) -> URL? {
        guard canOpenBacklink(backlink) else { return nil }
        if let url = URL(string: backlink) { return url }
        let allowed = CharacterSet.urlFragmentAllowed.union(.urlPathAllowed).union(CharacterSet(charactersIn: "?#%"))
        return backlink.addingPercentEncoding(withAllowedCharacters: allowed).flatMap { URL(string: $0) }
    }

    private static func isAlpha(_ unit: UInt16) -> Bool { (unit >= 0x41 && unit <= 0x5A) || (unit >= 0x61 && unit <= 0x7A) }
    private static func isDigit(_ unit: UInt16) -> Bool { unit >= 0x30 && unit <= 0x39 }
}

import FMBase
import Foundation

public struct CrmLeadMarks: Equatable, Sendable {
    public var rank: Double = 0
    public var rankField = ""
    public var score: Double = 0
    public var scoreField = ""
    public var value: Int64 = 0

    public init(_ contact: CrmContact) {
        value = max(0, contact.value)
        for (key, raw) in contact.fields.sorted(by: { $0.key < $1.key }) {
            let field = key.lowercased()
            guard let number = Double(raw.trimmingCharacters(in: .whitespaces)), number.isFinite, number > 0 else { continue }
            if field == "rank" || field.hasSuffix("_rank"), rank <= 0 || number < rank {
                rank = number
                rankField = key
            }
            if field == "score" || field.hasSuffix("_score"), number > score {
                score = number
                scoreField = key
            }
        }
    }
}

public struct CrmNextContact: Equatable, Sendable {
    public enum Mode: String, Sendable, CaseIterable, Identifiable {
        case reachOut
        case followUp

        public var id: String { rawValue }
        public var title: String { self == .reachOut ? "Reach out" : "Follow up" }
        public var note: String { self == .reachOut ? "Best new lead to contact" : "Someone already in conversation" }
    }

    public var contact: CrmContact?
    public var reason = ""
    public var due = 0

    public static let replyDays: Int64 = 60
    public static let followUpDays: Int64 = 14
    public static let wonStages: Set<String> = [
        "won", "closed-won", "customer", "renewal", "committed", "signed", "paid", "active"]
    public static let endedStages: Set<String> = [
        "passed", "rejected", "closed", "inactive", "hired", "lost", "closed-lost", "churned", "declined",
        "unsubscribed", "do-not-contact"]
    public static let unsentStatuses: Set<String> = ["draft", "sending", "failed", "reject"]
    private static let dayMs: Int64 = 86_400_000

    public static func reachable(_ contact: CrmContact) -> Bool {
        hasEmail(contact) || CrmText.profileURL("mastodon", contact.mastodon) != nil
            || CrmText.profileURL("twitter", contact.twitter) != nil
    }

    static func hasEmail(_ contact: CrmContact) -> Bool {
        contact.email.range(of: #"^[^\s@]+@[^\s@]+\.[^\s@]+$"#, options: .regularExpression) != nil
    }

    static func ago(_ at: Int64, now: Int64) -> String {
        let days = (now - at) / dayMs
        return days <= 0 ? "today" : days == 1 ? "yesterday" : "\(days) days ago"
    }

    static func number(_ value: Double) -> String {
        value == value.rounded() && abs(value) < 1e15 ? String(Int64(value)) : String(value)
    }

    public static func pick(
        _ mode: Mode, contacts: [CrmContact], activity: [CrmActivity], skipped: Set<String> = [], now: Int64
    ) -> CrmNextContact {
        var linked: [String: [CrmActivity]] = [:]
        for message in activity where !message.contactId.isEmpty { linked[message.contactId, default: []].append(message) }
        var best = CrmNextContact()
        var bestKey: [Double] = []
        let people = CrmPeople.of(contacts)
        var duePeople: Set<String> = []
        for contact in contacts where reachable(contact) {
            let members = people[contact.id] ?? [contact]
            if members.contains(where: { skipped.contains($0.id) }) { continue }
            let last = members.map(\.lastContactedAt).max() ?? 0
            var heardAt: Int64 = 0
            var wroteAt: Int64 = 0
            var heardSubject = ""
            for message in members.flatMap({ linked[$0.id] ?? [] }) {
                if message.incoming {
                    if message.at > heardAt {
                        heardAt = message.at
                        heardSubject = message.subject
                    }
                } else if !unsentStatuses.contains(message.status.lowercased()) {
                    wroteAt = max(wroteAt, message.at)
                }
            }
            let step = contact.nextStep.trimmingCharacters(in: .whitespacesAndNewlines)
            let waiting = heardAt >= now - replyDays * dayMs && heardAt > wroteAt && heardAt >= last
            let closed = members.contains { wonStages.contains($0.stage.lowercased()) || endedStages.contains($0.stage.lowercased()) }
            var reason: String
            var key: [Double]
            switch mode {
            case .reachOut:
                if waiting || closed || last > 0 { continue }
                let marks = CrmLeadMarks(contact)
                let email = hasEmail(contact)
                var qualifies: [String] = []
                if !step.isEmpty { qualifies.append("next step on the record: \(step)") }
                if marks.value > 0 { qualifies.append("deal value \(CrmText.money(marks.value))") }
                if marks.rank > 0 { qualifies.append("ranked \(number(marks.rank)) (\(marks.rankField))") }
                if marks.score > 0 { qualifies.append("score \(number(marks.score)) (\(marks.scoreField))") }
                if email { qualifies.append("an email address on file") }
                reason = (contact.createdAt > 0 ? "Added \(ago(contact.createdAt, now: now)) and never contacted. " : "Never contacted. ")
                    + (qualifies.isEmpty
                        ? "Nothing on the record sets a new lead apart, so the longest on the list goes first."
                        : "Best qualified new lead: \(qualifies.joined(separator: "; ")).")
                key = [
                    step.isEmpty ? 1 : 0, -Double(marks.value), marks.rank > 0 ? marks.rank : .infinity, -marks.score,
                    email ? 0 : 1, Double(contact.createdAt),
                ]
            case .followUp:
                if waiting {
                    reason = "They wrote to us \(ago(heardAt, now: now))"
                        + (heardSubject.isEmpty ? "" : " (\u{201C}\(heardSubject)\u{201D})")
                        + " and have had no answer yet, so the next move is ours."
                    key = [0, step.isEmpty ? 1 : 0, -Double(heardAt)]
                } else if !closed && last > 0 && now - last >= followUpDays * dayMs {
                    reason = "Last contacted \(ago(last, now: now)) and nothing has come back since; "
                        + "a follow-up is due after \(followUpDays) quiet days."
                    key = [1, step.isEmpty ? 1 : 0, Double(last)]
                } else {
                    continue
                }
                if !step.isEmpty { reason += " Next step on the record: \(step)." }
            }
            if members.count > 1 {
                let others = members.count - 1
                reason += " Same person as \(others) other record\(others == 1 ? "" : "s") (\(CrmPeople.keysLabel(members))), so they come up once."
            }
            duePeople.insert(members[0].id)
            best.due = duePeople.count
            let better = best.contact == nil || key.lexicographicallyPrecedes(bestKey)
                || (key == bestKey && contact.id < (best.contact?.id ?? ""))
            if better {
                best.contact = contact
                best.reason = reason
                bestKey = key
            }
        }
        return best
    }
}

public enum CrmPeople {
    public static func of(_ contacts: [CrmContact]) -> [String: [CrmContact]] {
        var parent: [String: String] = [:]
        var holder: [String: String] = [:]
        func root(_ start: String) -> String {
            var id = start
            while let up = parent[id], up != id {
                parent[id] = parent[up] ?? up
                id = parent[id] ?? up
            }
            return id
        }
        for contact in contacts where parent[contact.id] == nil {
            parent[contact.id] = contact.id
            for key in contact.personKeys {
                if let held = holder[key] { parent[root(contact.id)] = root(held) } else { holder[key] = contact.id }
            }
        }
        var groups: [String: [CrmContact]] = [:]
        var placed: Set<String> = []
        for contact in contacts where placed.insert(contact.id).inserted { groups[root(contact.id), default: []].append(contact) }
        var people: [String: [CrmContact]] = [:]
        for members in groups.values {
            let sorted = members.sorted { $0.id < $1.id }
            for contact in sorted { people[contact.id] = sorted }
        }
        return people
    }

    public static func keysLabel(_ members: [CrmContact]) -> String {
        Set(members.flatMap(\.personKeys)).sorted()
            .map { $0.hasPrefix("x:") ? "@\($0.dropFirst(2)) on X" : String($0.dropFirst($0.hasPrefix("email:") ? 6 : 0)) }
            .joined(separator: ", ")
    }
}

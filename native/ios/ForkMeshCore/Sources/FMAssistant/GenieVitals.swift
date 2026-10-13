import FMApi
import FMBase
import FMControl
import FMPackages
import Foundation

public struct GenieVitalSigns: Equatable, Sendable {
    public var commits: Int
    public var agentRuns: Int
    public var merged: Int
    public var emailsSent: Int
    public var contactsTouched: Int
    public var marketingVisits: Int
    public var replies: Int
    public var bounced: Int
    public var measuredAt: Date

    public init(
        commits: Int = 0, agentRuns: Int = 0, merged: Int = 0, emailsSent: Int = 0, contactsTouched: Int = 0,
        marketingVisits: Int = 0, replies: Int = 0, bounced: Int = 0, measuredAt: Date = Date()
    ) {
        self.commits = commits
        self.agentRuns = agentRuns
        self.merged = merged
        self.emailsSent = emailsSent
        self.contactsTouched = contactsTouched
        self.marketingVisits = marketingVisits
        self.replies = replies
        self.bounced = bounced
        self.measuredAt = measuredAt
    }

    public nonisolated static let storeKey: String = "genie/vitals"

    public static func load(_ store: KeyValueStore?) -> GenieVitalSigns? {
        guard let values = store?.stringList(forKey: storeKey), values.count == 9 else { return nil }
        let n: [Double] = values.map { Double($0) ?? 0 }
        return GenieVitalSigns(
            commits: Int(n[0]), agentRuns: Int(n[1]), merged: Int(n[2]), emailsSent: Int(n[3]), contactsTouched: Int(n[4]),
            marketingVisits: Int(n[5]), replies: Int(n[6]), bounced: Int(n[7]), measuredAt: Date(timeIntervalSince1970: n[8]))
    }

    public func save(_ store: KeyValueStore?) {
        let values: [Int] = [commits, agentRuns, merged, emailsSent, contactsTouched, marketingVisits, replies, bounced]
        store?.set(values.map(String.init) + [String(measuredAt.timeIntervalSince1970)], forKey: GenieVitalSigns.storeKey)
    }
}

public enum GenieVitalLevel: String, Equatable, Sendable, CaseIterable {
    case thriving, happy, peckish, hungry, sick

    public var title: String {
        switch self {
        case .thriving: return "Thriving"
        case .happy: return "Happy"
        case .peckish: return "Peckish"
        case .hungry: return "Hungry"
        case .sick: return "Poorly"
        }
    }

    public static func of(_ health: Double) -> GenieVitalLevel {
        switch health {
        case 0.75...: return .thriving
        case 0.5..<0.75: return .happy
        case 0.3..<0.5: return .peckish
        case 0.12..<0.3: return .hungry
        default: return .sick
        }
    }
}

public struct GenieVitals: Equatable, Sendable {
    public nonisolated static let window: TimeInterval = 7 * 24 * 3600
    public nonisolated static let codeGoal: Double = 14
    public nonisolated static let marketingGoal: Double = 16
    public nonisolated static let silentCap: Double = 0.45
    public nonisolated static let fewRepliesCap: Double = 0.7
    public nonisolated static let fewReplies: Int = 3
    public nonisolated static let staleAfter: TimeInterval = 2 * 24 * 3600
    public nonisolated static let maxHearts: Int = 5

    public var signs: GenieVitalSigns
    public var code: Double
    public var marketing: Double
    public var health: Double

    public init(_ signs: GenieVitalSigns, now: Date = Date()) {
        self.signs = signs
        let fade: Double = GenieVitals.freshness(signs.measuredAt, now: now)
        code = GenieVitals.fill(GenieVitals.codePoints(signs), goal: GenieVitals.codeGoal) * fade
        marketing = min(GenieVitals.fill(GenieVitals.marketingPoints(signs), goal: GenieVitals.marketingGoal), GenieVitals.marketingCap(signs)) * fade
        health = GenieVitals.combine(code: code, marketing: marketing)
    }

    public var level: GenieVitalLevel { GenieVitalLevel.of(health) }

    public var hearts: Int { Int((health * Double(GenieVitals.maxHearts)).rounded()) }

    public static func codePoints(_ signs: GenieVitalSigns) -> Double {
        Double(signs.commits) + Double(signs.agentRuns) * 2 + Double(signs.merged) * 3
    }

    public static func marketingPoints(_ signs: GenieVitalSigns) -> Double {
        max(Double(signs.emailsSent) * 2 + Double(signs.contactsTouched) * 0.5 + Double(signs.replies) * 4 - Double(signs.bounced), 0)
    }

    public static func marketingCap(_ signs: GenieVitalSigns) -> Double {
        if signs.replies <= 0 { return silentCap }
        return signs.replies < fewReplies ? fewRepliesCap : 1
    }

    public static func unanswered(_ signs: GenieVitalSigns) -> Bool {
        signs.replies <= 0 && signs.emailsSent + signs.contactsTouched > 0
    }

    public static func fill(_ points: Double, goal: Double) -> Double {
        guard points > 0, goal > 0 else { return 0 }
        return min(1 - exp(-points / goal * 1.6), 1)
    }

    public static func combine(code: Double, marketing: Double) -> Double {
        let mean: Double = (code + marketing) / 2
        let weakest: Double = min(code, marketing)
        return min(max(mean * 0.55 + weakest * 0.45, 0), 1)
    }

    public static func freshness(_ measured: Date, now: Date) -> Double {
        let age: TimeInterval = now.timeIntervalSince(measured)
        guard age > staleAfter else { return 1 }
        return max(0.35, 1 - (age - staleAfter) / window)
    }

    public static func word(_ value: Double) -> String {
        switch value {
        case 0.75...: return "humming"
        case 0.5..<0.75: return "healthy"
        case 0.3..<0.5: return "slowing down"
        case 0.12..<0.3: return "hungry"
        default: return "starving"
        }
    }

    public var weakest: String { code <= marketing ? "code" : "marketing" }

    public var summary: String {
        var parts: [String] = []
        let s: GenieVitalSigns = signs
        let codeFacts: [String] = [
            GenieVitals.count(s.commits, "commit"), GenieVitals.count(s.agentRuns, "agent run"), GenieVitals.count(s.merged, "merge"),
        ].filter { !$0.isEmpty }
        let marketingFacts: [String] = [
            GenieVitals.count(s.emailsSent, "person emailed", plural: "people emailed"),
            s.contactsTouched > s.emailsSent ? GenieVitals.count(s.contactsTouched, "contact reached", plural: "contacts reached") : "",
            GenieVitals.count(s.replies, "reply", plural: "replies"),
            GenieVitals.count(s.bounced, "bounce", plural: "bounces"),
        ].filter { !$0.isEmpty }
        parts.append("I'm feeling \(level.title.lowercased()).")
        parts.append(
            "Code is \(GenieVitals.word(code))" + (codeFacts.isEmpty ? " with nothing new this week." : ": \(GenieVitals.joined(codeFacts)) this week."))
        parts.append(
            "Marketing is \(GenieVitals.word(marketing))"
                + (marketingFacts.isEmpty ? " with no outreach this week." : ": \(GenieVitals.joined(marketingFacts)) this week."))
        let silent: Bool = GenieVitals.unanswered(s)
        let nothingSent: Bool = silent && s.emailsSent <= 0
        if silent {
            parts.append(
                nothingSent ? "No email went out, so there was nothing for anyone to reply to." : "Nobody has replied, so the marketing is not working yet.")
        }
        if health < 0.5 {
            parts.append(
                weakest == "code" ? "Ship a commit or start an agent to feed me."
                    : nothingSent ? "A reply is what feeds me: send the note you already have."
                    : silent ? "A reply is what feeds me: sharpen the message and follow up." : "Reach out to a contact to feed me.")
        }
        return parts.joined(separator: " ")
    }

    static func count(_ n: Int, _ one: String, plural: String? = nil) -> String {
        guard n > 0 else { return "" }
        return n == 1 ? "1 \(one)" : "\(n) \(plural ?? one + "s")"
    }

    static func joined(_ items: [String]) -> String {
        guard items.count > 1 else { return items.first ?? "" }
        return items.dropLast().joined(separator: ", ") + " and " + (items.last ?? "")
    }

    public static func isMarketing(_ kind: Route.Kind) -> Bool {
        marketingKinds.contains(kind)
    }

    public nonisolated static let marketingKinds: Set<Route.Kind> = [.crm, .contact, .crmOrg, .marketing, .analytics, .competitors, .ads]
}

public struct GenieCrmCounts: Equatable, Sendable {
    public var emailed: Int = 0
    public var reached: Int = 0
    public var replies: Int = 0
    public var bounced: Int = 0

    public nonisolated static let delivered: Set<String> = ["", "external", "accepted", "delivery", "delivered", "open", "click"]
    public nonisolated static let refused: Set<String> = ["failed", "reject", "bounce", "soft bounce", "suspension", "unsubscribe", "spam"]

    public init(emailed: Int = 0, reached: Int = 0, replies: Int = 0, bounced: Int = 0) {
        self.emailed = emailed
        self.reached = reached
        self.replies = replies
        self.bounced = bounced
    }

    public init(_ data: CrmOrganizationData, since: Int64) {
        reached = data.contacts.filter { $0.lastContactedAt >= since }.count
        var emailedTo: Set<String> = []
        var answered: Set<String> = []
        for email in data.activity where email.at >= since {
            if email.direction == "inbound" {
                answered.insert(GenieCrmCounts.who(email, email.fromEmail))
                continue
            }
            let status: String = email.status.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            if GenieCrmCounts.refused.contains(status) {
                bounced += 1
            } else if GenieCrmCounts.delivered.contains(status) {
                emailedTo.insert(GenieCrmCounts.who(email, email.to))
            }
        }
        var byAddress: [String: String] = [:]
        for contact in data.contacts {
            let others: [String] = (contact.fields["other_emails"] ?? "").split(separator: ",").map(String.init)
            for address in ([contact.email] + others).compactMap(GenieCrmCounts.address) where byAddress[address] == nil {
                byAddress[address] = contact.id
            }
        }
        for mail in data.mail where mail.receivedAt >= since {
            if let address = GenieCrmCounts.address(mail.from), let id = byAddress[address] { answered.insert("c:" + id) }
        }
        emailed = emailedTo.count
        replies = answered.count
    }

    static func who(_ email: CrmActivity, _ address: String) -> String {
        if !email.contactId.isEmpty { return "c:" + email.contactId }
        let mail: String = address.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        return mail.isEmpty ? "i:" + email.id : "m:" + mail
    }

    nonisolated static let addressPattern = try! NSRegularExpression(pattern: "[A-Za-z0-9._%+'-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}")

    static func address(_ text: String) -> String? {
        let range = NSRange(text.startIndex..., in: text)
        guard let hit = addressPattern.firstMatch(in: text, range: range), let found = Range(hit.range, in: text) else { return nil }
        return text[found].lowercased()
    }
}

public struct GenieAgentCounts: Equatable, Sendable {
    public var runs: Int = 0
    public var merged: Int = 0

    public init(runs: Int = 0, merged: Int = 0) {
        self.runs = runs
        self.merged = merged
    }

    public init(rows:[UnifiedAgentRow]?, relay: [AgentLaunchSession]?, since: Int64) {
        var seen: [String: Bool] = [:]
        if let rows {
            for row in rows where !row.outside && Int64(row.createdAtMs) >= since { seen[row.id] = row.merged }
        } else {
            for session in relay ?? [] where Int64(session.createdAtMs) >= since { seen[session.sessionId] = session.isMergedBranch }
        }
        runs = seen.count
        merged = seen.values.filter { $0 }.count
    }
}

extension ForkMeshToolbox {
    public func vitalSigns(marketingVisits: Int = 0) async -> GenieVitalSigns {
        let current: Date = now()
        let since: Int64 = Int64((current.timeIntervalSince1970 - GenieVitals.window) * 1000)
        var signs: GenieVitalSigns = GenieVitalSigns(marketingVisits: marketingVisits, measuredAt: current)
        if let repos = try? await myRepositories().prefix(ForkMeshToolbox.maxRepoScan) {
            for repo in repos {
                guard let rows = try? await sources.commits(repo.owner, repo.name) else { continue }
                signs.commits += rows.filter { ForkMeshToolbox.commitDate($0) >= since }.count
            }
        }
        let rows: [UnifiedAgentRow]? = await agentRows()
        let relay: [AgentLaunchSession]? = rows == nil ? try? await sources.agents() : nil
        let agents = GenieAgentCounts(rows: rows, relay: relay, since: since)
        signs.agentRuns = agents.runs
        signs.merged = agents.merged
        if let organization = try? await crmOrganization(JSONObject()), let data = try? await crmData(organization) {
            let crm = GenieCrmCounts(data, since: since)
            signs.emailsSent = crm.emailed
            signs.contactsTouched = crm.reached
            signs.replies = crm.replies
            signs.bounced = crm.bounced
        }
        return signs
    }
}

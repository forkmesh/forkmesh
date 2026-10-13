import FMBase
import FMCrypto
import FMLocalAI
import FMPackages
import Foundation

public enum GenieOutreachChannel: String, Equatable, Sendable {
    case email
    case mastodon
}

public struct GenieMastodonHandle: Equatable, Sendable {
    public var user: String
    public var host: String

    public init?(_ raw: String) {
        var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if let url = URL(string: text), let scheme = url.scheme?.lowercased(), scheme == "https" || scheme == "http" {
            guard let host = url.host, !host.isEmpty else { return nil }
            let path = url.path.split(separator: "/").first.map(String.init) ?? ""
            guard path.hasPrefix("@"), path.count > 1 else { return nil }
            text = path + "@" + host
        }
        if text.hasPrefix("@") { text.removeFirst() }
        let parts = text.split(separator: "@", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 2, !parts[0].isEmpty, parts[1].contains("."),
              !text.contains(where: { $0.isWhitespace || $0 == "/" })
        else { return nil }
        user = parts[0]
        host = parts[1].lowercased()
    }

    public var mention: String { "@\(user)@\(host)" }

    public var lookupURL: URL? {
        var components = URLComponents()
        components.scheme = "https"
        components.host = host
        components.path = "/api/v1/accounts/lookup"
        components.queryItems = [URLQueryItem(name: "acct", value: user)]
        return components.url
    }
}

public struct GenieOutreachPick: Equatable, Sendable {
    public var organization: String
    public var contact: CrmContact
    public var flowName: String
    public var channel: GenieOutreachChannel
    public var sender: String
    public var investor: Bool
    public var zohoRegion: String

    public init(
        organization: String, contact: CrmContact, flowName: String, channel: GenieOutreachChannel, sender: String,
        investor: Bool = false, zohoRegion: String = "com"
    ) {
        self.organization = organization
        self.contact = contact
        self.flowName = flowName
        self.channel = channel
        self.sender = sender
        self.investor = investor
        self.zohoRegion = zohoRegion
    }

    public var needsDeck: Bool { investor && channel == .email }

    public var firstName: String {
        let name = contact.name.trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty else { return "" }
        return name.split(separator: " ").first.map(String.init) ?? name
    }

    public var handle: GenieMastodonHandle? { GenieMastodonHandle(contact.mastodon) }

    public var images: [URL] { GenieOutreach.images(contact) }

    public var caption: String {
        channel == .email ? "Email \(contact.displayName)?" : "Toot to \(contact.displayName)?"
    }
}

public struct GenieOutreachDraft: Equatable, Sendable {
    public var channel: GenieOutreachChannel
    public var to: String
    public var subject: String
    public var body: String
    public var byModel: Bool
    public var writer: String
    public var deck: String
    public var zohoRegion: String

    public init(
        channel: GenieOutreachChannel, to: String, subject: String = "", body: String, byModel: Bool, writer: String = "",
        deck: String = "", zohoRegion: String = "com"
    ) {
        self.channel = channel
        self.to = to
        self.subject = subject
        self.body = body
        self.byModel = byModel
        self.writer = writer
        self.deck = deck
        self.zohoRegion = zohoRegion
    }

    public var mailtoURL: URL? {
        guard channel == .email else { return nil }
        return CrmSettings.mailtoURL(to: to, subject: subject, body: body)
    }

    public var zohoURL: URL? {
        guard channel == .email else { return nil }
        return CrmSettings.zohoComposeURL(region: zohoRegion, to: to, subject: subject, body: body)
    }
}

public struct GenieOutreachWriter: Sendable {
    public var name: String
    public var timeout: Double
    public var run: @MainActor @Sendable ([LocalChatMessage]) async throws -> String

    public init(name: String, timeout: Double, run: @escaping @MainActor @Sendable ([LocalChatMessage]) async throws -> String) {
        self.name = name
        self.timeout = timeout
        self.run = run
    }

    @MainActor
    public static func local(_ model: GenieModel?, timeout: Double = GenieOutreach.timeout) -> GenieOutreachWriter? {
        guard let model, model.engine.availability.isAvailable else { return nil }
        let engine = model.engine
        var parameters = model.parameters
        parameters.maxTokens = 420
        parameters.temperature = GenieOutreach.temperature
        let asked = parameters
        let prepare = model.prepare
        return GenieOutreachWriter(name: model.name, timeout: timeout) { messages in
            try await prepare()
            return try await engine.complete(messages: messages, tools: [], parameters: asked).text
        }
    }
}

public enum GenieOutreach {
    public nonisolated static let closedStages: Set<String> = [
        "passed", "rejected", "closed", "inactive", "lost", "closed-lost", "declined", "churned", "not-interested",
        "disqualified",
    ]
    public nonisolated static let tootLimit = 500
    public nonisolated static let emailWords = 140
    public nonisolated static let timeout: Double = 25
    public nonisolated static let cloudTimeout: Double = 90
    public nonisolated static let temperature: Double = 0.4
    public nonisolated static let investorWords = 180
    public nonisolated static let investorFlowIds: Set<String> = ["investors", "investment"]
    public nonisolated static let pitch =
        "ForkMesh (https://forkmesh.com) lets developers collaborate on, mirror and preserve source-code repositories across independent hosts, without relying on any single hosting provider. Git, issues, pull requests, chat and coding agents come together in one app on desktop, web, iPhone and Android."
    public nonisolated static let investorFacts: [String] = [
        "ForkMesh (https://forkmesh.com) turns Claude Code and Codex into attributable contributors on a local-first Git platform teams control. Tagline: Code with agents. Own the forge.",
        "Problem: coding is moving from autocomplete to delegated agent work, but tasks, transcripts, branches and reviews are scattered, accountability for agent changes is shallow, and code, identity and collaboration stay rented from a central platform.",
        "Product: an issue or prompt starts a Claude Code or Codex session in its own worktree and branch; sessions run in parallel and can be streamed, steered, stopped and resumed; the patch and session metadata (model, cost, tool use) become a signed pull request that a human reviews and merges. Desktop app and web.",
        "Proof (Git history and live network, 13 September 2026): ForkMesh builds ForkMesh. 7,416 agent branches merged into main, 51,457 commits in 90 days, 24 human and agent contributors, 63 community pull requests, 54 repositories on the live network.",
        "Open source and peer-to-peer: repositories mirror across independent nodes; the protocol is MIT-licensed; any community incentives are optional, non-custodial and never guaranteed.",
        "Customers: 20 to 200 person software teams using several coding agents on private code; buyer is the CTO, VP Engineering or security lead. Land one agent workflow, then expand.",
        "Business model: Community (free, open source), Agent team (subscription plus usage), Enterprise (annual contract: policy, RBAC, audit, self-hosted and federated). $25K blended ACV hypothesis.",
        "Why now: 180M+ developers on GitHub, 4.7M paid Copilot subscribers growing 75% a year; teams use several agents and need provenance, policy and trusted merges.",
        "Competition: hosted AI forges (Copilot, GitLab Duo) keep the code and control plane; coding agents (Claude, Codex, Cursor) are not a shared system of record; Radicle is peer-to-peer Git without agent orchestration. ForkMesh owns the intersection.",
        "Team: Jett Vale, CEO and founder, built and exited a prior company; Md. Kaif, CTO and co-founder, distributed systems and Git.",
        "The round: raising $1.0M for an 18-month plan to reach 20 paid organizations, $500K ARR and Series A readiness by Q4 2027. Funds: 55% product and engineering, 20% infrastructure and security, 15% go-to-market, 10% legal, ops and contingency.",
        "Contact: founders@forkmesh.com.",
    ]

    public static func stageKey(_ stage: String) -> String {
        stage.trimmingCharacters(in: .whitespaces).lowercased()
            .replacingOccurrences(of: " ", with: "-").replacingOccurrences(of: "_", with: "-")
    }

    public static func closed(_ contact: CrmContact, flows: [CrmFlow]) -> Bool {
        let stage = stageKey(contact.stage)
        if closedStages.contains(stage) { return true }
        guard let flow = flows.first(where: { $0.id == contact.flow }), flow.stages.count > 1, let last = flow.stages.last else { return false }
        return stageKey(last) == stage
    }

    public static func emailAddress(_ contact: CrmContact) -> String? {
        let email = contact.email.trimmingCharacters(in: .whitespacesAndNewlines)
        let parts = email.split(separator: "@", omittingEmptySubsequences: false)
        guard parts.count == 2, !parts[0].isEmpty, parts[1].contains("."), !email.contains(where: \.isWhitespace) else { return nil }
        return email
    }

    public static func channel(_ contact: CrmContact) -> GenieOutreachChannel? {
        if emailAddress(contact) != nil { return .email }
        if GenieMastodonHandle(contact.mastodon) != nil { return .mastodon }
        return nil
    }

    public static func reached(_ contact: CrmContact, in data: CrmOrganizationData) -> Bool {
        if contact.lastContactedAt > 0 { return true }
        return data.activity.contains { $0.contactId == contact.id && !$0.incoming && $0.status != "draft" }
    }

    public static func investor(_ contact: CrmContact, flows: [CrmFlow]) -> Bool {
        if investorFlowIds.contains(contact.flow) { return true }
        guard let flow = flows.first(where: { $0.id == contact.flow }) else { return false }
        if flow.icon == "investors" { return true }
        let name = flow.name.lowercased()
        return ["investor", "angel", "vc", "fund", "capital"].contains { name.contains($0) }
    }

    public static func candidates(_ data: CrmOrganizationData, skip: Set<String> = []) -> [CrmContact] {
        data.contacts.filter { contact in
            !skip.contains(contact.id) && channel(contact) != nil && !reached(contact, in: data)
                && !closed(contact, flows: data.flows)
        }
    }

    public static func pick(
        _ organizations: [CrmOrganizationData], skip: Set<String> = [], sender: String = "",
        random: () -> Double = { Double.random(in: 0..<1) }
    ) -> GenieOutreachPick? {
        let pool = organizations.flatMap { data in candidates(data, skip: skip).map { (data, $0) } }
        guard !pool.isEmpty else { return nil }
        let index = min(pool.count - 1, max(0, Int(random() * Double(pool.count))))
        let (data, contact) = pool[index]
        return offer(contact, in: data, sender: sender)
    }

    public static func best(_ organizations: [CrmOrganizationData], skip: Set<String> = [], sender: String = "") -> GenieOutreachPick? {
        let pool = organizations.flatMap { data in candidates(data, skip: skip).map { (data, $0) } }
        guard let (data, contact) = pool.min(by: { ranksAhead($0.1, of: $1.1) }) else { return nil }
        return offer(contact, in: data, sender: sender)
    }

    public static func ranksAhead(_ a: CrmContact, of b: CrmContact) -> Bool {
        if a.value != b.value { return a.value > b.value }
        let aNext = !a.nextStep.trimmingCharacters(in: .whitespaces).isEmpty
        let bNext = !b.nextStep.trimmingCharacters(in: .whitespaces).isEmpty
        if aNext != bNext { return aNext }
        let aEmail = channel(a) == .email
        let bEmail = channel(b) == .email
        if aEmail != bEmail { return aEmail }
        let aAdded = a.createdAt > 0 ? a.createdAt : Int64.max
        let bAdded = b.createdAt > 0 ? b.createdAt : Int64.max
        if aAdded != bAdded { return aAdded < bAdded }
        return a.id < b.id
    }

    static func offer(_ contact: CrmContact, in data: CrmOrganizationData, sender: String) -> GenieOutreachPick? {
        guard let channel = channel(contact) else { return nil }
        let from = data.settings.fromName.trimmingCharacters(in: .whitespaces)
        return GenieOutreachPick(
            organization: data.organization, contact: contact, flowName: data.flow(contact.flow).name, channel: channel,
            sender: from.isEmpty ? sender : from, investor: investor(contact, flows: data.flows),
            zohoRegion: data.settings.zoho.region)
    }

    public static func images(_ contact: CrmContact) -> [URL] {
        var urls: [URL] = []
        for key in ["photo", "avatar", "image", "picture", "photoUrl", "avatarUrl"] {
            let match = contact.fields.first { $0.key.lowercased() == key.lowercased() }?.value ?? ""
            if let url = URL(string: match.trimmingCharacters(in: .whitespaces)), url.scheme?.lowercased() == "https" {
                urls.append(url)
            }
        }
        let github = githubUser(contact.github)
        if !github.isEmpty, let url = URL(string: "https://github.com/\(github).png?size=160") { urls.append(url) }
        if let email = emailAddress(contact) {
            let hash = Sha256.hex(Array(email.lowercased().utf8))
            if let url = URL(string: "https://gravatar.com/avatar/\(hash)?s=160&d=404") { urls.append(url) }
        }
        return urls
    }

    static func githubUser(_ raw: String) -> String {
        var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if let range = text.range(of: "github.com/", options: .caseInsensitive) { text = String(text[range.upperBound...]) }
        if text.hasPrefix("@") { text.removeFirst() }
        let user = text.split(separator: "/").first.map(String.init) ?? ""
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-"))
        guard !user.isEmpty, user.unicodeScalars.allSatisfy(allowed.contains) else { return "" }
        return user
    }

    public static func describe(_ pick: GenieOutreachPick) -> String {
        let contact = pick.contact
        var lines = ["Name: \(contact.displayName)"]
        if !contact.title.isEmpty { lines.append("Role: \(contact.title)") }
        if !contact.company.isEmpty { lines.append("Company: \(contact.company)") }
        for (label, value) in [
            ("Website", contact.website), ("Location", contact.location), ("GitHub", contact.github),
            ("Mastodon", contact.mastodon), ("Next step", contact.nextStep),
        ] where !value.isEmpty {
            lines.append("\(label): \(value)")
        }
        if !contact.tags.isEmpty { lines.append("Tags: " + contact.tags.joined(separator: ", ")) }
        let notes = contact.notes.trimmingCharacters(in: .whitespacesAndNewlines)
        if !notes.isEmpty { lines.append("Notes: " + (notes.count > 400 ? String(notes.prefix(399)) + "…" : notes)) }
        if !pick.flowName.isEmpty { lines.append("Pipeline: \(pick.flowName), stage \(contact.stage)") }
        return lines.joined(separator: "\n")
    }

    public static func messages(_ pick: GenieOutreachPick, deck: String = "") -> [LocalChatMessage] {
        let angle = CrmPrompt.angle(sheet: "", flowName: pick.flowName)
        let signer = pick.sender.isEmpty ? "the ForkMesh team" : pick.sender
        let pitching = pick.investor && pick.channel == .email
        var system = "You write first messages from the ForkMesh team to people who have never heard from us. " + pitch
        if pitching { system += "\n\nAbout ForkMesh, from its investor deck:\n" + investorFacts.map { "- " + $0 }.joined(separator: "\n") + "\n\n" }
        system += " Write as \(signer). Be warm, plain and specific to who they are and their work."
        system += " Work only from the facts given here: never invent traction, numbers, dates, names, quotes, links or shared history, and do not claim to know their work beyond what their record says. Leave out anything you are not given."
        system += " No placeholders in square brackets and no markdown."
        var user = "Who they are:\n" + describe(pick) + "\n\n"
        switch pick.channel {
        case .email:
            if pitching {
                user += "Write a short first email (at most \(investorWords) words) to this investor about ForkMesh."
                if !angle.isEmpty { user += " " + angle }
                user += " Make one clear ask: a 20-minute call."
            } else {
                user += "Write a short first email (at most \(emailWords) words) about how ForkMesh fits them."
                if !angle.isEmpty { user += " " + angle }
            }
            if !deck.isEmpty {
                user += "\n\nTheir personal deck link: \(deck)\nIt is unique to them and every open and page read is recorded against their record, so put it in the email exactly as written, on its own line, once. Do not shorten, rewrite or add anything to it.\n\n"
            }
            user += " Reply with exactly: a first line starting \"Subject:\", a blank line, then the body, signed by \(signer)."
        case .mastodon:
            let mention = pick.handle?.mention ?? pick.contact.mastodon
            user += "Write one Mastodon post (under 450 characters) to them about ForkMesh."
            if !angle.isEmpty { user += " " + angle }
            user += " Start with their handle \(mention), use at most two hashtags, and reply with only the post."
        }
        return [.system(system), .user(user)]
    }

    public static func parse(_ text: String, pick: GenieOutreachPick, deck: String = "", writer: String = "") -> GenieOutreachDraft? {
        var body = text.replacingOccurrences(of: "**", with: "").trimmingCharacters(in: .whitespacesAndNewlines)
        if body.hasPrefix("```") {
            body = body.split(separator: "\n", omittingEmptySubsequences: false).dropFirst()
                .filter { !$0.hasPrefix("```") }.joined(separator: "\n")
                .trimmingCharacters(in: .whitespacesAndNewlines)
        }
        let placeholder = body.contains("[") && body.contains("]")
        guard !body.isEmpty, !placeholder else { return nil }
        switch pick.channel {
        case .email:
            guard let to = emailAddress(pick.contact) else { return nil }
            var lines = body.split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
            guard let index = lines.firstIndex(where: { $0.lowercased().trimmingCharacters(in: .whitespaces).hasPrefix("subject:") }),
                  let colon = lines[index].firstIndex(of: ":")
            else { return nil }
            let line = lines[index]
            let subject = String(line[line.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
            lines.removeSubrange(0...index)
            let rest = lines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
            guard !subject.isEmpty, rest.count >= 40 else { return nil }
            return GenieOutreachDraft(
                channel: .email, to: to, subject: subject, body: withDeck(rest, deck: deck), byModel: true, writer: writer, deck: deck,
                zohoRegion: pick.zohoRegion)
        case .mastodon:
            guard let handle = pick.handle else { return nil }
            var post = body.trimmingCharacters(in: CharacterSet(charactersIn: "\"\u{201C}\u{201D}").union(.whitespacesAndNewlines))
            if !post.lowercased().contains("@" + handle.user.lowercased()) { post = handle.mention + " " + post }
            guard post.count >= 40 else { return nil }
            return GenieOutreachDraft(channel: .mastodon, to: handle.mention, body: clipToot(post), byModel: true, writer: writer)
        }
    }

    public static func withDeck(_ body: String, deck: String) -> String {
        guard !deck.isEmpty, !body.contains(deck) else { return body }
        var paragraphs = body.components(separatedBy: "\n\n")
        let line = "Our deck, made for you: \(deck)"
        if paragraphs.count > 1 { paragraphs.insert(line, at: paragraphs.count - 1) } else { paragraphs.append(line) }
        return paragraphs.joined(separator: "\n\n")
    }

    public static func unsupported(_ text: String, sources: String) -> [String] {
        var found: [String] = []
        for pattern in ["https?://[^\\s<>\"')\\]]+", "[0-9]+(?:[.,][0-9]+)*"] {
            guard let regex = try? NSRegularExpression(pattern: pattern) else { continue }
            let range = NSRange(text.startIndex..., in: text)
            for match in regex.matches(in: text, range: range) {
                guard let span = Range(match.range, in: text) else { continue }
                let token = String(text[span]).trimmingCharacters(in: CharacterSet(charactersIn: ".,;:!?"))
                if !token.isEmpty, !sources.contains(token), !found.contains(token) { found.append(token) }
            }
        }
        return found
    }

    public static func clipToot(_ text: String) -> String {
        guard text.count > tootLimit else { return text }
        let cut = String(text.prefix(tootLimit - 1))
        let words = cut.split(separator: " ", omittingEmptySubsequences: false).dropLast().joined(separator: " ")
        return (words.isEmpty ? cut : words) + "…"
    }

    public static func fallback(_ pick: GenieOutreachPick, deck: String = "") -> GenieOutreachDraft {
        let contact = pick.contact
        let hello = pick.firstName.isEmpty ? "Hi there" : "Hi \(pick.firstName)"
        var about = "your work"
        if !contact.company.isEmpty { about = "your work at \(contact.company)" } else if !contact.title.isEmpty { about = "your work as \(contact.title)" }
        let signer = pick.sender.isEmpty ? "The ForkMesh team" : pick.sender
        let to = emailAddress(contact) ?? contact.email
        switch pick.channel {
        case .email where pick.investor && !deck.isEmpty:
            let body = """
                \(hello),

                ForkMesh (https://forkmesh.com) turns Claude Code and Codex into attributable contributors on a local-first Git platform that teams control. We are raising our round now, and I would value your view.

                Our deck, made for you:
                \(deck)

                Would you be open to a 20-minute call?

                Best,
                \(signer)
                """
            return GenieOutreachDraft(
                channel: .email, to: to, subject: "ForkMesh deck for \(contact.company.isEmpty ? contact.displayName : contact.company)",
                body: body, byModel: false, deck: deck, zohoRegion: pick.zohoRegion)
        case .email:
            let body = """
                \(hello),

                I came across \(about) and thought ForkMesh might be useful to you. It lets developers collaborate on, mirror and preserve their repositories across independent hosts, so no single provider can take their code away, with issues, pull requests, chat and coding agents in one app.

                Would you be open to a quick look or a short call? You can try it at https://forkmesh.com.

                Best,
                \(signer)
                """
            return GenieOutreachDraft(
                channel: .email, to: to, subject: "ForkMesh for \(contact.company.isEmpty ? contact.displayName : contact.company)",
                body: withDeck(body, deck: deck), byModel: false, deck: deck, zohoRegion: pick.zohoRegion)
        case .mastodon:
            let mention = pick.handle?.mention ?? contact.mastodon
            let post = "\(mention) \(hello)! I think ForkMesh could fit \(about): repositories mirrored and preserved across independent hosts, with issues, pull requests and chat in one app. Have a look at https://forkmesh.com #git #opensource"
            return GenieOutreachDraft(channel: .mastodon, to: mention, body: clipToot(post), byModel: false)
        }
    }

    @MainActor
    public static func write(_ pick: GenieOutreachPick, model: GenieModel?, timeout: Double = GenieOutreach.timeout) async -> GenieOutreachDraft {
        await write(pick, writers: [GenieOutreachWriter.local(model, timeout: timeout)].compactMap { $0 })
    }

    @MainActor
    public static func write(_ pick: GenieOutreachPick, writers: [GenieOutreachWriter], deck: String = "") async -> GenieOutreachDraft {
        let messages = messages(pick, deck: deck)
        let sources = messages.map(\.content).joined(separator: "\n")
        for writer in writers {
            guard let text = await attempt(writer, messages),
                  let draft = parse(text, pick: pick, deck: deck, writer: writer.name),
                  unsupported(draft.subject + "\n" + draft.body, sources: sources).isEmpty
            else { continue }
            return draft
        }
        return fallback(pick, deck: deck)
    }

    @MainActor
    static func attempt(_ writer: GenieOutreachWriter, _ messages: [LocalChatMessage]) async -> String? {
        let run = writer.run
        let timeout = writer.timeout
        return await withTaskGroup(of: String?.self) { group in
            group.addTask { try? await run(messages) }
            group.addTask {
                try? await Task.sleep(nanoseconds: UInt64(max(timeout, 0.1) * 1_000_000_000))
                return nil
            }
            let first = await group.next() ?? nil
            group.cancelAll()
            return first
        }
    }
}

public final class GenieOutreachLog {
    public nonisolated static let offKey = "genie/outreach_off"
    public nonisolated static let shownKey = "genie/outreach_shown"
    public nonisolated static let draftedKey = "genie/outreach_drafted"
    public nonisolated static let lastKey = "genie/outreach_last"
    public nonisolated static let every: TimeInterval = 2 * 60 * 60
    public nonisolated static let firstAfter: TimeInterval = 3 * 60
    public nonisolated static let restAfterShown: TimeInterval = 7 * 24 * 60 * 60
    public nonisolated static let most = 300

    private let store: KeyValueStore?
    public private(set) var off: Bool
    public private(set) var last: Date
    private var shown: [String: Date]
    private var drafted: [String]

    public init(store: KeyValueStore?) {
        self.store = store
        off = store?.bool(forKey: GenieOutreachLog.offKey) ?? false
        last = Date(timeIntervalSince1970: store?.double(forKey: GenieOutreachLog.lastKey) ?? 0)
        var seen: [String: Date] = [:]
        for entry in store?.stringList(forKey: GenieOutreachLog.shownKey) ?? [] {
            let parts = entry.split(separator: " ")
            if parts.count == 2, let at = Double(parts[1]) { seen[String(parts[0])] = Date(timeIntervalSince1970: at) }
        }
        shown = seen
        drafted = store?.stringList(forKey: GenieOutreachLog.draftedKey) ?? []
    }

    public func setOff(_ value: Bool) {
        off = value
        store?.set(value, forKey: GenieOutreachLog.offKey)
    }

    public func due(now: Date, activeSince: Date) -> Bool {
        !off && now.timeIntervalSince(activeSince) >= GenieOutreachLog.firstAfter
            && now.timeIntervalSince(last) >= GenieOutreachLog.every
    }

    public func skip(now: Date) -> Set<String> {
        let recent = shown.filter { now.timeIntervalSince($0.value) < GenieOutreachLog.restAfterShown }.keys
        return Set(recent).union(drafted)
    }

    public func showed(_ contactId: String, at now: Date) {
        last = now
        shown[contactId] = now
        let kept = shown.sorted { $0.value > $1.value }.prefix(GenieOutreachLog.most)
        shown = Dictionary(uniqueKeysWithValues: kept.map { ($0.key, $0.value) })
        store?.set(now.timeIntervalSince1970, forKey: GenieOutreachLog.lastKey)
        store?.set(kept.map { "\($0.key) \(Int64($0.value.timeIntervalSince1970))" }, forKey: GenieOutreachLog.shownKey)
    }

    public var draftedContacts: Set<String> { Set(drafted) }

    public func drafted(_ contactId: String) {
        drafted = Array((drafted.filter { $0 != contactId } + [contactId]).suffix(GenieOutreachLog.most))
        store?.set(drafted, forKey: GenieOutreachLog.draftedKey)
    }

    public func checked(at now: Date) {
        last = now
        store?.set(now.timeIntervalSince1970, forKey: GenieOutreachLog.lastKey)
    }
}

extension ForkMeshToolbox {
    public func outreachPick(skip: Set<String>, random: () -> Double = { Double.random(in: 0..<1) }) async -> GenieOutreachPick? {
        guard let loaded = await outreachOrganizations() else { return nil }
        return GenieOutreach.pick(loaded, skip: skip, sender: sources.account()?.username ?? "", random: random)
    }

    public func outreachBest(skip: Set<String>) async -> GenieOutreachPick? {
        guard let loaded = await outreachOrganizations() else { return nil }
        return GenieOutreach.best(loaded, skip: skip, sender: sources.account()?.username ?? "")
    }

    private func outreachOrganizations() async -> [CrmOrganizationData]? {
        guard let client = sources.crm() else { return nil }
        guard let organizations = try? await client.crmOrganizations().filter(\.installed).map(\.name) else { return nil }
        var loaded: [CrmOrganizationData] = []
        for organization in organizations.prefix(5) {
            if let data = try? await client.crm(organization) { loaded.append(data) }
        }
        return loaded
    }

    public func outreachDeck(_ pick: GenieOutreachPick) async throws -> String {
        guard let client = sources.crm() else { throw AssistantError("The CRM is not available in this build.") }
        do {
            let link = try await client.createCrmDeckLink(pick.organization, contactId: pick.contact.id)
            guard link.active, link.url.hasPrefix("https://") else { throw AssistantError("The CRM did not return a deck link.") }
            return link.url
        } catch let error as PackagesError {
            if error.code == "deck_needs_investor_pipeline" {
                throw AssistantError("Move \(pick.contact.displayName) to an investor pipeline first so they can get a deck link.")
            }
            throw AssistantError(error.message)
        }
    }
}

import FMApi
import FMPackages
import SwiftUI

enum CrmDay {
    static func key(_ ms: Int64) -> String {
        guard ms > 0 else { return "Earlier" }
        let date = Date(timeIntervalSince1970: TimeInterval(ms) / 1000)
        let calendar = Calendar.current
        if calendar.isDateInToday(date) { return "Today" }
        if calendar.isDateInYesterday(date) { return "Yesterday" }
        return date.formatted(.dateTime.weekday(.wide).month().day())
    }

    static func time(_ ms: Int64) -> String {
        guard ms > 0 else { return "" }
        return Date(timeIntervalSince1970: TimeInterval(ms) / 1000).formatted(date: .omitted, time: .shortened)
    }
}

enum CrmFeedItem: Identifiable {
    case email(CrmActivity)
    case mail(CrmMailMessage)

    var id: String {
        switch self {
        case .email(let email): return "e" + email.id
        case .mail(let message): return "m" + message.id
        }
    }

    var at: Int64 {
        switch self {
        case .email(let email): return email.at
        case .mail(let message): return message.receivedAt
        }
    }
}

enum CrmFeedFilter: String, CaseIterable, Identifiable {
    case all = "All"
    case sent = "Sent"
    case received = "Received"
    case failed = "Failed"
    case mail = "Mailbox"

    var id: String { rawValue }
}

struct CrmActivityPane: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let present: (CrmSheet) -> Void
    @State private var filter: CrmFeedFilter = .all
    @Environment(\.openURL) private var openURL

    private var items: [CrmFeedItem] {
        let query = model.query
        var out: [CrmFeedItem] = []
        if filter != .mail {
            out += data.activity.filter { email in
                let contact = data.contact(id: email.contactId)
                let matches = PackagesText.matches(query, email.subject, email.to, email.sender, contact?.displayName ?? "")
                switch filter {
                case .sent: return matches && !email.incoming
                case .received: return matches && email.incoming
                case .failed: return matches && email.failed
                default: return matches
                }
            }.map(CrmFeedItem.email)
        }
        if filter == .all || filter == .mail || filter == .received {
            out += data.mail.filter { $0.matches(query) }.map(CrmFeedItem.mail)
        }
        return out.sorted { $0.at > $1.at }
    }

    var body: some View {
        let feed = items
        let days = Dictionary(grouping: feed) { CrmDay.key($0.at) }
        let order = feed.map { CrmDay.key($0.at) }.reduce(into: [String]()) { if !$0.contains($1) { $0.append($1) } }
        List {
            Section {
                Picker("Show", selection: $filter) {
                    ForEach(CrmFeedFilter.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .listRowBackground(Color.clear)
                .listRowInsets(EdgeInsets())
            }
            if feed.isEmpty {
                CrmEmptyRow(text: "No activity yet.", icon: "clock").listRowBackground(Color.clear)
            }
            ForEach(order, id: \.self) { day in
                Section(day) {
                    ForEach(days[day] ?? []) { item in row(item) }
                }
            }
        }
        .listStyle(.insetGrouped)
        .fmRefreshable { await model.load() }
    }

    @ViewBuilder private func row(_ item: CrmFeedItem) -> some View {
        switch item {
        case .email(let email):
            Button {
                present(.email(email))
            } label: {
                CrmActivityRow(activity: email, contact: data.contact(id: email.contactId))
            }
            .buttonStyle(.plain)
        case .mail(let message):
            CrmMailRow(message: message, contact: data.contact(email: CrmMailRow.address(message.from)))
                .contentShape(Rectangle())
                .onTapGesture {
                    if let url = URL(string: message.providerUrl), url.scheme == "https" { openURL(url) }
                }
        }
    }
}

struct CrmMailRow: View {
    let message: CrmMailMessage
    let contact: CrmContact?

    static func address(_ from: String) -> String {
        if let open = from.lastIndex(of: "<"), let close = from.lastIndex(of: ">"), open < close {
            return String(from[from.index(after: open)..<close])
        }
        return from
    }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            CrmAvatar(name: contact?.name ?? message.who, email: contact?.email ?? CrmMailRow.address(message.from), size: 30)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 4) {
                    Text(contact?.displayName ?? message.who).font(.subheadline.weight(.semibold)).lineLimit(1)
                    if message.hasAttachment { Image(systemName: "paperclip").font(.caption2).foregroundStyle(.secondary) }
                    Spacer(minLength: 0)
                    Text(CrmDay.time(message.receivedAt)).font(.caption2).foregroundStyle(.secondary)
                }
                Text(message.subject.isEmpty ? "(no subject)" : message.subject).font(.subheadline).lineLimit(1)
                if !message.summary.isEmpty {
                    Text(message.summary).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                }
            }
        }
        .accessibilityElement(children: .combine)
    }
}

struct CrmMailPane: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    @State private var box = "Inbox"
    @State private var syncing = false
    @Environment(\.openURL) private var openURL

    var body: some View {
        let boxes = data.settings.mailBoxes
        let current = boxes.contains(box) ? box : (boxes.first ?? "Inbox")
        let messages = data.mail.filter { $0.box.lowercased() == current.lowercased() && $0.matches(model.query) }
        List {
            Section {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(boxes, id: \.self) { name in
                            let count = data.mail.filter { $0.box.lowercased() == name.lowercased() }.count
                            Button {
                                FmHaptics.selection()
                                box = name
                            } label: {
                                CrmChip(text: "\(name) \(count)", tint: .accentColor, filled: name == current).font(.subheadline)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
                zohoStatus
            }
            .listRowBackground(Color.clear)
            Section {
                if messages.isEmpty {
                    CrmEmptyRow(text: data.mail.isEmpty ? "No mail synced yet." : "Nothing in \(current).", icon: "tray")
                }
                ForEach(messages) { message in
                    CrmMailRow(message: message, contact: data.contact(email: CrmMailRow.address(message.from)))
                        .contentShape(Rectangle())
                        .onTapGesture {
                            if let url = URL(string: message.providerUrl), url.scheme == "https" { openURL(url) }
                        }
                        .swipeActions(edge: .trailing) {
                            Button(role: .destructive) {
                                Task { if await model.deleteMail(message) { FmHaptics.commit(.delete) } }
                            } label: {
                                Label("Delete", systemImage: "trash")
                            }
                            if let archive = boxes.last, archive != message.box {
                                Button {
                                    Task { if await model.moveMail(message, to: archive) { FmHaptics.selection() } }
                                } label: {
                                    Label(archive, systemImage: "archivebox")
                                }
                                .tint(.indigo)
                            }
                        }
                        .contextMenu {
                            ForEach(boxes.filter { $0 != message.box }, id: \.self) { target in
                                Button("Move to \(target)", systemImage: "tray.and.arrow.down") {
                                    Task { await model.moveMail(message, to: target) }
                                }
                            }
                            if let url = URL(string: message.providerUrl), url.scheme == "https" {
                                Button("Open in mailbox", systemImage: "arrow.up.forward.app") { openURL(url) }
                            }
                            Button("Delete", systemImage: "trash", role: .destructive) {
                                Task { await model.deleteMail(message) }
                            }
                        }
                }
            }
        }
        .listStyle(.insetGrouped)
        .fmRefreshable { await sync() }
    }

    private var zohoStatus: some View {
        HStack {
            Image(systemName: data.settings.zoho.connected ? "checkmark.seal.fill" : "exclamationmark.circle")
                .foregroundStyle(data.settings.zoho.connected ? .green : .orange)
            VStack(alignment: .leading, spacing: 1) {
                Text(data.settings.zoho.connected ? "Zoho Mail connected" : "Zoho Mail not connected").font(.subheadline.weight(.medium))
                if data.settings.zoho.lastSyncAt > 0 {
                    Text("Synced \(PackagesFormat.relative(data.settings.zoho.lastSyncAt))").font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer()
            Button {
                Task { await sync() }
            } label: {
                if syncing {
                    ProgressView()
                } else {
                    Text(data.settings.zoho.connected ? "Sync" : "Connect").font(.subheadline.weight(.semibold))
                }
            }
            .buttonStyle(.bordered)
            .disabled(syncing)
        }
        .padding(10)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    private func sync() async {
        syncing = true
        defer { syncing = false }
        if let url = await model.syncMail() { openURL(url) }
    }
}

struct CrmOutreachPane: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let present: (CrmSheet) -> Void
    @State private var removing: String?

    private var outreach: CrmOutreach { data.outreach }

    var body: some View {
        List {
            if !outreach.allowed {
                Section {
                    ContentUnavailableView(
                        "Outreach is for the outreach team", systemImage: "paperplane",
                        description: Text("Owners and admins add members to the team. Ask one of them to add you."))
                }
            } else {
                Section {
                    HStack(spacing: 16) {
                        Gauge(value: Double(outreach.sentToday), in: 0...Double(max(1, outreach.dailyLimit))) {
                            Text("Today")
                        } currentValueLabel: {
                            Text("\(outreach.remaining)")
                        }
                        .gaugeStyle(.accessoryCircularCapacity)
                        .tint(outreach.remaining > 5 ? .green : .orange)
                        VStack(alignment: .leading, spacing: 3) {
                            Text("\(outreach.remaining) sends left today").font(.headline)
                            Text(outreach.fromName.isEmpty ? outreach.fromEmail : "\(outreach.fromName) · \(outreach.fromEmail)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                            if !outreach.configured {
                                Text("The server has no mail sender configured.").font(.caption).foregroundStyle(.orange)
                                    .fmMessageTools("The server has no mail sender configured.")
                            }
                        }
                    }
                    Button {
                        present(.compose(CrmEmailDraft(), outreach: true))
                    } label: {
                        Label("New outreach email", systemImage: "square.and.pencil")
                    }
                    .disabled(outreach.remaining == 0)
                }
                if !outreach.templates.isEmpty {
                    Section("Templates") {
                        ForEach(outreach.templates) { template in
                            Button {
                                var draft = CrmEmailDraft()
                                draft.template = template.key
                                draft.subject = template.subject
                                draft.body = template.body
                                present(.compose(draft, outreach: true))
                            } label: {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(template.label).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                                    Text(template.subject).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                                }
                            }
                        }
                    }
                }
                Section("Recent sends") {
                    if outreach.recent.isEmpty { Text("Nothing sent yet.").foregroundStyle(.secondary) }
                    ForEach(outreach.recent) { send in
                        HStack(spacing: 10) {
                            Image(systemName: send.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                                .foregroundStyle(send.ok ? .green : .red)
                            VStack(alignment: .leading, spacing: 1) {
                                Text(send.subject).font(.subheadline).lineLimit(1)
                                Text([send.to, send.sender, PackagesFormat.relative(send.ts)].filter { !$0.isEmpty }.joined(separator: " · "))
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                                if !send.error.isEmpty { Text(send.error).font(.caption2).foregroundStyle(.red).fmMessageTools(send.error) }
                            }
                        }
                    }
                }
                if outreach.canManage {
                    teamSection
                }
            }
        }
        .listStyle(.insetGrouped)
        .fmRefreshable { await model.load() }
    }

    private var teamSection: some View {
        let onTeam = Set(outreach.members.map { $0.name.lowercased() })
        let candidates = data.members.filter { !onTeam.contains($0.lowercased()) }
        return Section {
            ForEach(outreach.members) { member in
                HStack {
                    CrmAvatar(name: member.name, email: "", size: 26)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(member.name).font(.subheadline)
                        if !member.addedBy.isEmpty {
                            Text("Added by \(member.addedBy)").font(.caption2).foregroundStyle(.secondary)
                        }
                    }
                }
                .swipeActions {
                    Button(role: .destructive) {
                        Task { await model.setOutreachMember(member.name, allowed: false) }
                    } label: {
                        Label("Remove", systemImage: "person.badge.minus")
                    }
                }
            }
            if !candidates.isEmpty {
                Menu {
                    ForEach(candidates, id: \.self) { name in
                        Button(name) { Task { await model.setOutreachMember(name, allowed: true) } }
                    }
                } label: {
                    Label("Add a member", systemImage: "person.badge.plus")
                }
            }
        } header: {
            Text("Outreach team")
        } footer: {
            Text("Owners and admins can always send outreach. Members send once they are on the team.")
        }
    }
}

struct CrmTeamPane: View {
    let data: CrmOrganizationData
    @State private var days = 7
    @State private var member = ""

    private var range: Range<Int> {
        let count = data.team.days.count
        return max(0, count - days)..<count
    }

    private var people: [CrmTeamMember] {
        data.team.members.filter { member.isEmpty || $0.name == member }
    }

    private func total(_ part: (emails: Int, outreach: Int, contacts: Int)) -> Int {
        part.emails + part.outreach + part.contacts
    }

    private func dayTotal(_ person: CrmTeamMember, _ index: Int) -> Int {
        let day = person.day(index)
        return day.emails + day.outreach + day.contacts
    }

    private func sum(_ person: CrmTeamMember) -> (emails: Int, outreach: Int, contacts: Int) {
        range.reduce((0, 0, 0)) { total, index in
            let day = person.day(index)
            return (total.0 + day.emails, total.1 + day.outreach, total.2 + day.contacts)
        }
    }

    var body: some View {
        let totals = people.reduce((0, 0, 0)) { total, person in
            let part = sum(person)
            return (total.0 + part.emails, total.1 + part.outreach, total.2 + part.contacts)
        }
        let ranked = people.map { ($0, sum($0)) }.sorted { total($0.1) > total($1.1) }
        List {
            Section {
                Picker("Period", selection: $days) {
                    Text("7 days").tag(7)
                    Text("30 days").tag(30)
                }
                .pickerStyle(.segmented)
                if data.team.members.count > 1 {
                    Picker("Member", selection: $member) {
                        Text("Everyone").tag("")
                        ForEach(data.team.members) { Text($0.name).tag($0.name) }
                    }
                }
                HStack(spacing: 8) {
                    CrmStatTile(value: "\(totals.0)", label: "Emails", icon: "envelope.fill", tint: .blue)
                    CrmStatTile(value: "\(totals.1)", label: "Outreach", icon: "paperplane.fill", tint: .orange)
                    CrmStatTile(value: "\(totals.2)", label: "Contacts", icon: "person.badge.plus", tint: .green)
                    CrmStatTile(value: "\(totals.0 + totals.1 + totals.2)", label: "Touches", icon: "hand.tap.fill", tint: .purple)
                }
            }
            Section("Daily") {
                CrmStackedChart(data: data, people: people, range: range)
                    .frame(height: 140)
                    .padding(.vertical, 6)
            }
            Section("Leaderboard") {
                if ranked.isEmpty { Text("No activity yet.").foregroundStyle(.secondary) }
                ForEach(Array(ranked.enumerated()), id: \.offset) { index, entry in
                    let person = entry.0
                    let part = entry.1
                    HStack(spacing: 10) {
                        Text("\(index + 1)").font(.caption.weight(.bold).monospacedDigit()).foregroundStyle(.secondary).frame(width: 18)
                        CrmAvatar(name: person.name, email: "", size: 28)
                        VStack(alignment: .leading, spacing: 3) {
                            Text(person.name).font(.subheadline.weight(.semibold))
                            Text("\(part.emails) emails · \(part.outreach) outreach · \(part.contacts) contacts")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        CrmMiniBars(values: range.map { dayTotal(person, $0) }, tint: .accentColor)
                            .frame(width: 80)
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
    }
}

struct CrmStackedChart: View {
    let data: CrmOrganizationData
    let people: [CrmTeamMember]
    let range: Range<Int>

    var body: some View {
        let totals = range.map { index in
            people.reduce((0, 0, 0)) { sum, person in
                let day = person.day(index)
                return (sum.0 + day.emails, sum.1 + day.outreach, sum.2 + day.contacts)
            }
        }
        let top = max(1, totals.map { $0.0 + $0.1 + $0.2 }.max() ?? 1)
        VStack(spacing: 6) {
            GeometryReader { proxy in
                HStack(alignment: .bottom, spacing: range.count > 10 ? 2 : 6) {
                    ForEach(Array(totals.enumerated()), id: \.offset) { _, day in
                        let unit = proxy.size.height / CGFloat(top)
                        VStack(spacing: 0) {
                            Rectangle().fill(Color.green).frame(height: unit * CGFloat(day.2))
                            Rectangle().fill(Color.orange).frame(height: unit * CGFloat(day.1))
                            Rectangle().fill(Color.blue).frame(height: unit * CGFloat(day.0))
                        }
                        .frame(maxWidth: .infinity)
                        .clipShape(RoundedRectangle(cornerRadius: 2))
                        .frame(height: proxy.size.height, alignment: .bottom)
                    }
                }
            }
            HStack(spacing: 12) {
                legend("Emails", .blue)
                legend("Outreach", .orange)
                legend("Contacts", .green)
                Spacer()
                if let first = data.team.days.indices.contains(range.lowerBound) ? data.team.days[range.lowerBound] : nil {
                    Text("since \(first)").font(.caption2).foregroundStyle(.secondary)
                }
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Daily activity chart")
    }

    private func legend(_ title: String, _ color: Color) -> some View {
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 7, height: 7)
            Text(title).font(.caption2).foregroundStyle(.secondary)
        }
    }
}

struct CrmDeckRow: View {
    let link: CrmDeckLink
    let contact: CrmContact?

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: link.active ? "rectangle.on.rectangle.angled.fill" : "rectangle.on.rectangle.slash")
                .font(.caption)
                .foregroundStyle(link.active ? .orange : .secondary)
                .frame(width: 26, height: 26)
                .background((link.active ? Color.orange : Color.secondary).opacity(0.12), in: Circle())
            VStack(alignment: .leading, spacing: 3) {
                HStack {
                    Text(contact?.displayName ?? "Investor deck").font(.subheadline.weight(.medium)).lineLimit(1)
                    if !link.active { CrmChip(text: "Revoked", tint: .secondary) }
                }
                Text(link.views == 0 ? "Not opened yet" : "\(link.views) view\(link.views == 1 ? "" : "s") · \(CrmText.duration(link.seconds)) · \(PackagesFormat.relative(link.lastViewedAt))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if link.pageCount > 0 {
                    ProgressView(value: link.pageShare)
                        .tint(.orange)
                        .accessibilityLabel("\(link.pagesViewed) of \(link.pageCount) pages viewed")
                }
            }
        }
    }
}

struct CrmDecksPane: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let present: (CrmSheet) -> Void

    var body: some View {
        let links = data.decks.filter { link in
            model.query.isEmpty || (data.contact(id: link.contactId)?.matches(model.query) ?? false)
        }
        List {
            Section {
                if links.isEmpty {
                    CrmEmptyRow(text: "No deck links yet. Make one from a contact.", icon: "rectangle.on.rectangle.angled")
                }
                ForEach(links) { link in
                    Button {
                        present(.deck(link))
                    } label: {
                        CrmDeckRow(link: link, contact: data.contact(id: link.contactId))
                    }
                    .buttonStyle(.plain)
                    .contextMenu {
                        Button("Copy link", systemImage: "link") { CrmClipboard.copy(link.url) }
                        if link.active {
                            Button("Revoke", systemImage: "xmark.circle", role: .destructive) {
                                Task { await model.revoke(link) }
                            }
                        }
                    }
                }
            } header: {
                Text("\(data.decks.filter(\.active).count) active links · \(data.decks.reduce(0) { $0 + $1.views }) views")
            }
        }
        .listStyle(.insetGrouped)
        .fmRefreshable { await model.load() }
    }
}

struct CrmDeckSheet: View {
    @Bindable var model: CrmModel
    let link: CrmDeckLink
    @State private var detail: CrmDeckDetail?
    @State private var fetched = false
    @State private var confirmRevoke = false
    @Environment(\.dismiss) private var dismiss

    private var current: CrmDeckLink { detail?.link ?? model.data?.decks.first { $0.token == link.token } ?? link }

    var body: some View {
        List {
            Section {
                CrmDeckRow(link: current, contact: model.contact(current.contactId))
                if let url = URL(string: current.url) {
                    ShareLink(item: url) { Label("Share link", systemImage: "square.and.arrow.up") }
                    Button("Copy link", systemImage: "link") { CrmClipboard.copy(current.url) }
                }
                LabeledContent("Created", value: "\(PackagesFormat.date(current.createdAt)) by \(current.createdBy)")
            }
            if let detail {
                Section("Pages") {
                    ForEach(detail.pages) { page in
                        HStack(spacing: 8) {
                            Text("\(page.page)").font(.caption.monospacedDigit()).foregroundStyle(.secondary).frame(width: 18)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(page.title).font(.caption).lineLimit(1)
                                GeometryReader { proxy in
                                    Capsule()
                                        .fill(page.viewers > 0 ? Color.orange : Color.secondary.opacity(0.2))
                                        .frame(width: max(4, proxy.size.width * CGFloat(page.seconds) / CGFloat(max(1, detail.longestPage))))
                                }
                                .frame(height: 5)
                            }
                            Text(CrmText.duration(page.seconds)).font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                        }
                    }
                }
                if !detail.sessions.isEmpty {
                    Section("Sessions") {
                        ForEach(detail.sessions) { session in
                            VStack(alignment: .leading, spacing: 2) {
                                Text(PackagesFormat.relative(session.startedAt)).font(.subheadline)
                                Text([CrmText.duration(session.seconds), "\(session.pages) pages", session.country, session.agent]
                                    .filter { !$0.isEmpty }.joined(separator: " · "))
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            } else if !fetched {
                Section { ProgressView().frame(maxWidth: .infinity) }
            }
            if current.active {
                Section {
                    Button("Revoke link", systemImage: "xmark.circle", role: .destructive) { confirmRevoke = true }
                }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("Deck link")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
        }
        .task {
            detail = await model.deckDetail(link)
            fetched = true
        }
        .confirmationDialog("Revoke this deck link?", isPresented: $confirmRevoke, titleVisibility: .visible) {
            Button("Revoke", role: .destructive) {
                Task {
                    if await model.revoke(current) {
                        FmHaptics.commit()
                        detail?.link.active = false
                    }
                }
            }
        } message: {
            Text("The link stops opening the deck. Its view history stays.")
        }
    }
}

struct CrmSharedPane: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let present: (CrmSheet) -> Void

    var body: some View {
        List {
            Section {
                ForEach(data.sharedViews) { view in
                    Button {
                        present(.shared(view))
                    } label: {
                        HStack(spacing: 10) {
                            Image(systemName: view.isPipeline ? "point.3.connected.trianglepath.dotted" : "list.bullet.rectangle")
                                .foregroundStyle(Color.accentColor)
                                .frame(width: 26)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(view.name).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                                Text("\(view.organization) · shared by \(view.owner)").font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            CrmChip(text: view.canEdit ? "Can edit" : "View only", tint: view.canEdit ? .green : .secondary)
                        }
                    }
                }
            } footer: {
                Text("Lists people in other organizations shared with you. Copy one to bring its contacts into \(model.organization).")
            }
        }
        .listStyle(.insetGrouped)
    }
}

struct CrmSharedViewSheet: View {
    @Bindable var model: CrmModel
    let view: CrmView
    @State private var bundle: (view: CrmView, contacts: [CrmContact])?
    @State private var loading = true
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            if let bundle {
                Section {
                    ForEach(bundle.contacts) { contact in
                        VStack(alignment: .leading, spacing: 4) {
                            CrmContactRow(contact: contact, flow: nil, showStage: !contact.stage.isEmpty && bundle.view.isPipeline, columns: bundle.view.summaryColumns)
                            if bundle.view.canEdit, !bundle.view.stages.isEmpty {
                                Picker("Stage", selection: Binding(
                                    get: { contact.stage },
                                    set: { stage in Task { await setStage(contact, stage) } })
                                ) {
                                    ForEach(bundle.view.stages, id: \.self) { Text(CrmStageLabel.title($0)).tag($0) }
                                }
                                .font(.caption)
                            }
                        }
                    }
                    if bundle.contacts.isEmpty { Text("This list is empty.").foregroundStyle(.secondary) }
                } header: {
                    Text("\(bundle.contacts.count) contacts from \(bundle.view.organization)")
                }
            } else if loading {
                ProgressView().frame(maxWidth: .infinity)
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle(view.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Copy to \(model.organization)") {
                    Task {
                        if await model.copySharedView(view) != nil {
                            FmHaptics.commit()
                            dismiss()
                        }
                    }
                }
                .disabled(model.saving)
            }
        }
        .task {
            bundle = await model.sharedView(view)
            loading = false
        }
    }

    private func setStage(_ contact: CrmContact, _ stage: String) async {
        guard let current = bundle, stage != contact.stage else { return }
        if let saved = await model.updateSharedContact(current.view, contact: contact, changes: ["stage": .string(stage)]) {
            var contacts = current.contacts
            if let index = contacts.firstIndex(where: { $0.id == saved.id }) { contacts[index] = saved }
            bundle = (current.view, contacts)
            FmHaptics.selection()
        }
    }
}

import FMBase
import FMPackages
import SwiftUI
import UIKit

struct MarketingFormSheet: View {
    let model: MarketingModel
    let present: (MarketingSheet?) -> Void
    @State private var draft: MarketingDraft
    @State private var problem = ""
    @State private var saving = false
    @State private var confirmDelete = false
    @Environment(\.dismiss) private var dismiss

    init(model: MarketingModel, draft: MarketingDraft, present: @escaping (MarketingSheet?) -> Void) {
        self.model = model
        self.present = present
        _draft = State(initialValue: draft)
    }

    var body: some View {
        Form {
            if draft.kind == .xPost {
                Section {
                    MarketingFieldRow(field: draft.fields[0], value: binding(draft.fields[0].key), overview: model.overview,
                                      zone: model.timeZone)
                } footer: {
                    Text("\(draft["text"].count) characters · posts as @\(model.overview?.x.username ?? "")")
                }
            } else {
                Section {
                    ForEach(draft.fields) { field in
                        MarketingFieldRow(field: field, value: binding(field.key), overview: model.overview, zone: model.timeZone)
                    }
                } footer: {
                    if let footer { Text(footer) }
                }
            }
            if !problem.isEmpty {
                Section { Label(problem, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote).fmMessageTools(problem) }
            }
            if draft.kind.deletable && !draft.isNew {
                Section {
                    Button("Delete", role: .destructive) { confirmDelete = true }
                }
            }
        }
        .navigationTitle(draft.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button(confirmTitle) { Task { await save() } }
                    .disabled(saving)
            }
        }
        .confirmationDialog("Delete this \(draft.kind.title.lowercased())?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task {
                    if await model.delete(draft.kind, id: draft.id) {
                        FmHaptics.commit(.delete)
                        dismiss()
                    }
                }
            }
        }
        .interactiveDismissDisabled(saving)
    }

    private var confirmTitle: String {
        switch draft.kind {
        case .xPost: return "Post"
        case .xConnection: return "Connect"
        case .metric: return "Log"
        default: return "Save"
        }
    }

    private var footer: String? {
        switch draft.kind {
        case .adResults: return "Copy these from the platform's ads manager. Saving stamps when they were read."
        case .metric: return "Every figure is saved as a new reading; leave the ones you did not check as they are."
        case .funnelWeek: return "One row per week; logging the same week again updates it."
        case .channel: return "Access tokens are write-only: the server keeps them sealed and never sends them back."
        case .xConnection: return "Create the keys in the X developer portal with read and write access. They stay sealed on the server."
        case .redditThread: return "Marking a thread replied needs the rules check, the disclosure and your reply link."
        default: return nil
        }
    }

    private func binding(_ key: String) -> Binding<String> {
        Binding(get: { draft[key] }, set: { draft[key] = $0 })
    }

    private func save() async {
        saving = true
        defer { saving = false }
        problem = ""
        if let error = await model.save(draft) {
            problem = error
            FmHaptics.failure()
        } else {
            FmHaptics.commit()
            dismiss()
        }
    }
}

struct MarketingFieldRow: View {
    let field: MarketingField
    @Binding var value: String
    let overview: MarketingOverview?
    let zone: TimeZone

    var body: some View {
        switch field.kind {
        case .text, .url, .money, .count, .decimal, .list:
            LabeledContent(field.label) {
                TextField(field.hint.isEmpty ? field.label : field.hint, text: $value)
                    .multilineTextAlignment(.trailing)
                    .keyboardType(keyboard)
                    .textInputAutocapitalization(field.kind == .url || field.kind == .list ? .never : .sentences)
                    .autocorrectionDisabled(field.kind != .text)
            }
        case .secret:
            LabeledContent(field.label) {
                SecureField(field.hint.isEmpty ? field.label : field.hint, text: $value)
                    .multilineTextAlignment(.trailing)
            }
        case .multiline, .lines, .tasks:
            VStack(alignment: .leading, spacing: 4) {
                Text(field.label).font(.caption).foregroundStyle(.secondary)
                TextField(field.hint.isEmpty ? field.label : field.hint, text: $value, axis: .vertical)
                    .lineLimit(field.kind == .multiline ? 3...12 : 2...10)
                    .textInputAutocapitalization(field.kind == .lines ? .never : .sentences)
            }
        case .choice(let options):
            Picker(field.label, selection: $value) {
                ForEach(options, id: \.self) { option in
                    Text(option.isEmpty ? "None" : MarketingText.title(option)).tag(option)
                }
            }
        case .channel(let options):
            Picker(field.label, selection: $value) {
                ForEach(options, id: \.self) { option in
                    Text(option.isEmpty ? "Find from the link" : overview?.channelName(option) ?? option).tag(option)
                }
            }
        case .channels(let options):
            NavigationLink {
                MarketingChannelChecklist(title: field.label, options: options, overview: overview, value: $value)
            } label: {
                LabeledContent(field.label, value: value.isEmpty ? "None" : value)
            }
        case .toggle:
            Toggle(field.label, isOn: Binding(get: { value == "true" }, set: { value = $0 ? "true" : "" }))
        case .date, .dateTime:
            MarketingDateRow(label: field.label, time: field.kind == .dateTime, value: $value, zone: zone)
        }
    }

    private var keyboard: UIKeyboardType {
        switch field.kind {
        case .url: return .URL
        case .money, .decimal: return .decimalPad
        case .count: return .numberPad
        default: return .default
        }
    }
}

struct MarketingDateRow: View {
    let label: String
    let time: Bool
    @Binding var value: String
    let zone: TimeZone

    var body: some View {
        if let ms = MarketingDates.parse(value, zone: zone) {
            HStack {
                DatePicker(
                    label,
                    selection: Binding(
                        get: { Date(timeIntervalSince1970: Double(ms) / 1000) },
                        set: { value = MarketingDates.text(Int64($0.timeIntervalSince1970 * 1000), time: time, zone: zone) }),
                    displayedComponents: time ? [.date, .hourAndMinute] : [.date])
                Button {
                    value = ""
                } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Clear \(label.lowercased())")
            }
        } else {
            Button {
                value = MarketingDates.text(Int64(Date().timeIntervalSince1970 * 1000), time: time, zone: zone)
            } label: {
                LabeledContent(label, value: value.isEmpty ? "Add" : value)
            }
        }
    }
}

struct MarketingChannelChecklist: View {
    let title: String
    let options: [String]
    let overview: MarketingOverview?
    @Binding var value: String

    var body: some View {
        let selected = MarketingForms.splitList(value)
        List(options, id: \.self) { option in
            Button {
                var next = selected
                if let index = next.firstIndex(of: option) { next.remove(at: index) } else { next.append(option) }
                value = next.joined(separator: ", ")
            } label: {
                HStack {
                    Text(overview?.channelName(option) ?? option)
                    Spacer()
                    if selected.contains(option) { Image(systemName: "checkmark").foregroundStyle(Color.accentColor) }
                }
            }
            .foregroundStyle(.primary)
        }
        .navigationTitle(title)
    }
}

struct MarketingCampaignSheet: View {
    let model: MarketingModel
    let campaignId: String
    let present: (MarketingSheet?) -> Void
    @State private var newTask = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        if let overview = model.overview, let campaign = overview.campaign(campaignId) {
            List {
                Section {
                    LabeledContent("Status", value: MarketingText.title(campaign.status))
                    ForEach(campaign.facts(overview), id: \.self) { Text($0) }
                    ForEach(campaign.dates) { LabeledContent($0.label, value: PackagesFormat.date($0.ms)) }
                    if !campaign.json.str("audience").isEmpty { LabeledContent("Audience", value: campaign.json.str("audience")) }
                    if !campaign.json.str("message").isEmpty { Text(campaign.json.str("message")).font(.callout) }
                    let uncovered = campaign.json.obj("stats").strings("uncoveredChannels")
                    if !uncovered.isEmpty {
                        Text("No content yet on " + uncovered.map(overview.channelName).joined(separator: ", "))
                            .font(.caption)
                            .foregroundStyle(.orange)
                            .fmMessageTools("No content yet on " + uncovered.map(overview.channelName).joined(separator: ", "))
                    }
                }
                if !campaign.link.isEmpty {
                    Section("Tagged link") {
                        Text(campaign.link).font(.caption.monospaced()).textSelection(.enabled)
                        Button("Copy link", systemImage: "doc.on.doc") {
                            UIPasteboard.general.string = campaign.link
                            model.notice = "Link copied."
                        }
                        if let url = URL(string: campaign.link) { ShareLink(item: url) }
                    }
                }
                Section("Tasks") {
                    ForEach(Array(campaign.tasks.enumerated()), id: \.offset) { index, task in
                        Button {
                            Task { await model.toggleTask(campaign, index: index, done: !task.done) }
                        } label: {
                            HStack {
                                Image(systemName: task.done ? "checkmark.circle.fill" : "circle")
                                    .foregroundStyle(task.done ? Color.green : Color.secondary)
                                Text(task.title).strikethrough(task.done)
                                Spacer()
                                if !task.owner.isEmpty { Text(task.owner).font(.caption).foregroundStyle(.secondary) }
                            }
                        }
                        .foregroundStyle(.primary)
                        .accessibilityValue(task.done ? "Done" : "Open")
                    }
                    HStack {
                        TextField("Add a task", text: $newTask)
                            .onSubmit { add(campaign) }
                        if model.isWorking(campaign.id) { ProgressView() }
                    }
                }
                let content = overview.content.filter {
                    !$0.campaign.isEmpty && ($0.campaign.lowercased() == campaign.title.lowercased()
                        || $0.campaign.lowercased() == campaign.json.str("slug"))
                }
                Section("Content") {
                    ForEach(content) { item in
                        Button {
                            present(.form(MarketingDraft(kind: .content, id: item.id, overview: overview, json: item.json, zone: model.timeZone)))
                        } label: {
                            LabeledContent(item.title, value: MarketingText.title(item.status))
                        }
                        .foregroundStyle(.primary)
                    }
                    Button("Plan content for this campaign", systemImage: "plus.circle") {
                        present(.form(MarketingForms.contentDraft(overview, campaign: campaign.title,
                                                                  channel: campaign.json.strings("channels").first ?? "", zone: model.timeZone)))
                    }
                }
                Section {
                    if overview.assistantAvailable {
                        Button("Ask the assistant about this campaign", systemImage: "sparkles") { present(.assistant(campaign.id)) }
                    }
                    MarketingStatusMenu(model: model, kind: .campaign, id: campaign.id, current: campaign.status)
                }
                if !campaign.json.str("brief").isEmpty {
                    Section("Brief") { Text(campaign.json.str("brief")).font(.callout).textSelection(.enabled) }
                }
            }
            .navigationTitle(campaign.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } }
                ToolbarItem(placement: .primaryAction) {
                    Button("Edit") {
                        present(.form(MarketingDraft(kind: .campaign, id: campaign.id, overview: overview, json: campaign.json,
                                                     zone: model.timeZone)))
                    }
                }
            }
        } else {
            ContentUnavailableView("Campaign removed", systemImage: "flag.slash")
        }
    }

    private func add(_ campaign: MarketingRecord) {
        let title = newTask
        newTask = ""
        Task { await model.addTask(campaign, title: title) }
    }
}

struct MarketingChannelSheet: View {
    let model: MarketingModel
    let channel: String
    let present: (MarketingSheet?) -> Void
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    var body: some View {
        if let overview = model.overview {
            let stats = overview.stats(channel)
            let setting = overview.setting(channel)
            List {
                if let stats {
                    Section {
                        MarketingChannelRow(overview: overview, channel: stats, refreshing: model.isWorking("refresh:" + channel))
                        ForEach(stats.checks) { check in
                            Label(check.label, systemImage: check.ok ? "checkmark.circle.fill" : "circle")
                                .foregroundStyle(check.ok ? Color.green : Color.orange)
                                .font(.subheadline)
                        }
                    }
                }
                Section {
                    if overview.refreshable.contains(channel) {
                        Button("Refresh now", systemImage: "arrow.clockwise") { Task { await model.refreshChannel(channel) } }
                            .disabled(model.isWorking("refresh:" + channel))
                    }
                    Button("Log numbers by hand", systemImage: "square.and.pencil") {
                        present(.form(MarketingForms.metricDraft(overview, channel: channel, zone: model.timeZone)))
                    }
                    if let stats, stats.canCapture, let url = stats.captureURL(organization: model.organization) {
                        Button("Update with the browser extension", systemImage: "puzzlepiece.extension") { openURL(url) }
                    }
                    if let link = setting?.profileUrl, let url = URL(string: link), link.hasPrefix("https://") {
                        Link(destination: url) { Label("Open the channel page", systemImage: "safari") }
                    }
                    if overview.canManageSettings {
                        Button("Channel settings", systemImage: "gearshape") {
                            present(.form(MarketingForms.channelDraft(overview, channel: channel, zone: model.timeZone)))
                        }
                    }
                    if let error = setting?.lastError, !error.isEmpty {
                        Text("Last refresh failed: " + PackagesError.sentence(error)).font(.caption).foregroundStyle(.orange)
                            .fmMessageTools("Last refresh failed: " + PackagesError.sentence(error))
                    }
                }
                if let playbook = overview.playbook(channel) {
                    Section("Playbook") {
                        Text(playbook.goal).font(.subheadline)
                        if !playbook.kpis.isEmpty { LabeledContent("Watch", value: playbook.kpis.joined(separator: ", ")) }
                        ForEach(playbook.tips, id: \.self) { Label($0, systemImage: "lightbulb").font(.callout) }
                    }
                }
                let content = overview.content.filter { $0.channel == channel }
                if !content.isEmpty {
                    Section("Content") {
                        ForEach(content.prefix(20)) { item in
                            LabeledContent(item.title, value: MarketingText.title(item.status))
                        }
                    }
                }
                Section("Posted links") {
                    ForEach(overview.posts(channel: channel)) { post in
                        Button {
                            present(.form(MarketingDraft(kind: .post, id: post.id, overview: overview, json: post.json, zone: model.timeZone)))
                        } label: {
                            MarketingRecordRow(record: post, overview: overview)
                        }
                        .foregroundStyle(.primary)
                    }
                    Button("Track a posted link", systemImage: "plus.circle") {
                        present(.form(MarketingDraft(kind: .post, overview: overview, json: ["channel": .string(channel)], zone: model.timeZone)))
                    }
                }
            }
            .navigationTitle(overview.channelName(channel))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } } }
        }
    }
}

struct MarketingAssistantSheet: View {
    let model: MarketingModel
    let campaignId: String
    let present: (MarketingSheet?) -> Void
    @State private var question = ""
    @State private var campaign: String
    @Environment(\.dismiss) private var dismiss

    init(model: MarketingModel, campaignId: String, present: @escaping (MarketingSheet?) -> Void) {
        self.model = model
        self.campaignId = campaignId
        self.present = present
        _campaign = State(initialValue: campaignId)
    }

    var body: some View {
        Form {
            Section {
                Picker("About", selection: $campaign) {
                    Text("All marketing").tag("")
                    ForEach(model.overview?.campaigns ?? []) { Text($0.title).tag($0.id) }
                }
                TextField("Ask a question (optional)", text: $question, axis: .vertical)
                    .lineLimit(1...4)
                Button {
                    Task { await model.ask(campaignId: campaign, question: question) }
                } label: {
                    HStack {
                        Label("Suggest next steps", systemImage: "sparkles")
                        if model.asking { Spacer(); ProgressView() }
                    }
                }
                .disabled(model.asking)
            } footer: {
                Text("The assistant reads this organization's campaigns, channels and keywords and suggests up to eight steps.")
            }
            PackagesErrorBanner(text: model.errorText)
            if let advice = model.advice {
                Section("Summary") { Text(advice.summary).textSelection(.enabled) }
                Section("Suggestions") {
                    ForEach(advice.suggestions) { idea in
                        VStack(alignment: .leading, spacing: 4) {
                            Text(idea.title).font(.subheadline.weight(.medium))
                            if !idea.detail.isEmpty { Text(idea.detail).font(.caption).foregroundStyle(.secondary) }
                            if !idea.copy.isEmpty { Text(idea.copy).font(.caption).padding(6).background(.quaternary, in: RoundedRectangle(cornerRadius: 6)) }
                            HStack {
                                Text([MarketingText.title(idea.kind), idea.channel.isEmpty ? "" : model.overview?.channelName(idea.channel) ?? "",
                                      idea.inDays > 0 ? "in \(idea.inDays) days" : "today"].filter { !$0.isEmpty }.joined(separator: " · "))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                Spacer()
                                if !idea.actionTitle.isEmpty {
                                    Button(idea.actionTitle) {
                                        Task {
                                            if let draft = await model.apply(idea) { present(.form(draft)) }
                                        }
                                    }
                                    .font(.caption.weight(.semibold))
                                    .buttonStyle(.borderless)
                                }
                            }
                        }
                    }
                }
            }
        }
        .navigationTitle("Marketing assistant")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } } }
        .onDisappear { model.clearAdvice() }
    }
}

struct MarketingReportSheet: View {
    let model: MarketingModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        ScrollView {
            if let report = model.report {
                Text((try? AttributedString(markdown: report.markdown, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
                    ?? AttributedString(report.markdown))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding()
            } else {
                ProgressView().padding()
            }
        }
        .navigationTitle("Weekly report")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } }
            if let report = model.report {
                ToolbarItem(placement: .primaryAction) { ShareLink(item: report.markdown) }
            }
        }
        .task { await model.loadReport() }
    }
}

struct MarketingLinkSheet: View {
    let model: MarketingModel
    @State private var draft: MarketingDraft?
    @State private var link: MarketingLink?
    @State private var problem = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Form {
            if let current = draft {
                Section {
                    ForEach(current.fields) { field in
                        MarketingFieldRow(field: field, value: Binding(get: { draft?[field.key] ?? "" }, set: { draft?[field.key] = $0 }),
                                          overview: model.overview, zone: model.timeZone)
                    }
                    Button("Build link", systemImage: "link") {
                        Task {
                            let (built, error) = await model.buildLink(current)
                            link = built
                            problem = error ?? ""
                        }
                    }
                } footer: {
                    Text("Tags follow the organization's allowlist so visits are counted under the right campaign.")
                }
            }
            if !problem.isEmpty { Section { Text(problem).foregroundStyle(.red).font(.footnote).fmMessageTools(problem) } }
            if let link {
                Section("Tagged link") {
                    Text(link.url).font(.caption.monospaced()).textSelection(.enabled)
                    Button("Copy link", systemImage: "doc.on.doc") {
                        UIPasteboard.general.string = link.url
                        model.notice = "Link copied."
                    }
                    ForEach(link.checks, id: \.self) { (check: String) in
                        Label(check, systemImage: "exclamationmark.triangle").foregroundStyle(.orange).font(.caption).fmMessageTools(check)
                    }
                }
            }
        }
        .navigationTitle("Tagged link")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } } }
        .onAppear { if draft == nil { draft = model.newDraft(.link) } }
    }
}

struct MarketingKeywordImportSheet: View {
    let model: MarketingModel
    @State private var text = ""
    @State private var source = "Search Console"
    @State private var date = Date()
    @State private var limit = 100
    @State private var problem = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        let rows = MarketingKeywordImport.rows(text)
        Form {
            Section {
                TextField("Top queries,Clicks,Impressions,CTR,Position", text: $text, axis: .vertical)
                    .lineLimit(6...16)
                    .font(.caption.monospaced())
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            } footer: {
                Text(rows.isEmpty ? "Paste Search Console's Queries export, Bing's keyword report, or one keyword per line."
                    : "\(MarketingText.plural(rows.count, "keyword", "keywords")) found" + (rows.count > limit ? "; the top \(limit) by clicks will be imported." : "."))
            }
            Section {
                DatePicker("Reading date", selection: $date, displayedComponents: .date)
                LabeledContent("Source") { TextField("Source", text: $source).multilineTextAlignment(.trailing) }
                Stepper("Keep the top \(limit)", value: $limit, in: 10...500, step: 10)
            }
            if !problem.isEmpty { Section { Text(problem).foregroundStyle(.red).font(.footnote).fmMessageTools(problem) } }
        }
        .navigationTitle("Import keywords")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Import") {
                    Task {
                        let day = MarketingDates.parse(MarketingDates.text(Int64(date.timeIntervalSince1970 * 1000), time: false, zone: model.timeZone),
                                                       zone: TimeZone(identifier: "UTC")!) ?? 0
                        if let error = await model.importKeywords(text, source: source, limit: limit, observedAt: day + 12 * 3_600_000) {
                            problem = error
                        } else {
                            FmHaptics.commit()
                            dismiss()
                        }
                    }
                }
                .disabled(rows.isEmpty || model.isWorking("keywords"))
            }
        }
    }
}

struct MarketingAddChannelsSheet: View {
    let model: MarketingModel
    @State private var selected: Set<String> = []
    @State private var query = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        let channels = (model.overview?.addableChannels ?? []).filter { PackagesText.matches(query, $0.name, $0.category) }
        let categories = Array(Set(channels.map(\.category))).sorted()
        List {
            ForEach(categories, id: \.self) { category in
                Section(category.isEmpty ? "Other" : category) {
                    ForEach(channels.filter { $0.category == category }) { channel in
                        Button {
                            if selected.contains(channel.id) { selected.remove(channel.id) } else { selected.insert(channel.id) }
                        } label: {
                            HStack {
                                Text(channel.name)
                                Spacer()
                                if selected.contains(channel.id) { Image(systemName: "checkmark").foregroundStyle(Color.accentColor) }
                            }
                        }
                        .foregroundStyle(.primary)
                    }
                }
            }
        }
        .searchable(text: $query)
        .navigationTitle("Add channels")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Add \(selected.count)") {
                    Task {
                        await model.addChannels(Array(selected).sorted())
                        dismiss()
                    }
                }
                .disabled(selected.isEmpty || model.isWorking("channels"))
            }
        }
    }
}

struct MarketingSettingsSheet: View {
    let model: MarketingModel
    let present: (MarketingSheet?) -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            if let overview = model.overview {
                Section {
                    if overview.x.connected {
                        LabeledContent("Connected", value: "@" + overview.x.username)
                        Button("Post to X", systemImage: "paperplane") { present(.form(MarketingForms.xPostDraft(overview))) }
                        Button("Disconnect X", role: .destructive) { Task { await model.disconnectX() } }
                    } else if let draft = model.newDraft(.xConnection) {
                        Button("Connect an X account", systemImage: "link.badge.plus") { present(.form(draft)) }
                    }
                } header: {
                    Text("X")
                } footer: {
                    Text("A connected account lets Marketing post X content straight from the plan.")
                }
                Section("Measurement") {
                    ForEach([MarketingKind.attribution, .targets, .goals, .redditWatch], id: \.self) { kind in
                        if let draft = model.newDraft(kind) {
                            Button(kind.title) { present(.form(draft)) }
                        }
                    }
                }
                Section("Channels") {
                    Button("Add channels", systemImage: "plus.circle") { present(.addChannels) }
                    ForEach(overview.channelSettings) { setting in
                        Button {
                            present(.form(MarketingForms.channelDraft(overview, channel: setting.id, zone: model.timeZone)))
                        } label: {
                            LabeledContent(setting.displayName.isEmpty ? overview.channelName(setting.id) : setting.displayName,
                                           value: setting.enabled ? (setting.profileUrl.isEmpty ? "No link" : "On") : "Off")
                        }
                        .foregroundStyle(.primary)
                    }
                }
            }
        }
        .navigationTitle("Marketing settings")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } } }
    }
}

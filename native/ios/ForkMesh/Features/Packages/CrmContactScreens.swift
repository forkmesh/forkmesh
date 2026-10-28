import FMApi
import FMBase
import FMPackages
import SwiftUI

struct CrmContactDestination: View {
    let routeId: String

    var body: some View {
        PackagesGate(feature: "CRM") { client in
            if let ref = CrmContactRef(routeId: routeId) {
                CrmContactScreen(model: CrmSession.model(client: client), client: client, ref: ref)
            } else {
                ContentUnavailableView("Contact not found", systemImage: "person.crop.circle.badge.xmark")
            }
        }
    }
}

struct CrmContactScreen: View {
    @Bindable var model: CrmModel
    let client: PackagesClient
    let ref: CrmContactRef
    @State private var loaded: CrmContact?
    @State private var errorText = ""
    @State private var sheet: CrmSheet?
    @State private var confirmDelete = false
    @State private var notice: FmNotice?
    @State private var visible = false
    @State private var tab = CrmContactTab.overview
    @State private var invoices: ContactInvoicesState = .loading
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    private var editable: Bool { model.organization == ref.organization && model.data != nil }

    private var contact: CrmContact? {
        model.organization == ref.organization ? (model.contact(ref.contactId) ?? loaded) : loaded
    }

    var body: some View {
        List {
            PackagesErrorBanner(text: errorText) { await load() }
            if let contact {
                let data = model.data
                let flow = data?.flow(contact.flow)
                Section { CrmContactHeader(contact: contact, flow: flow) }
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets())
                Section { quickActions(contact, flow: flow) }
                    .listRowBackground(Color.clear)
                    .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0))
                if notice != nil {
                    Section { FmNoticeLine($notice) }
                }
                Section {
                    Picker("Show", selection: $tab) {
                        ForEach(CrmContactTab.allCases) { Text($0.title).tag($0) }
                    }
                    .pickerStyle(.segmented)
                }
                .listRowBackground(Color.clear)
                .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0))
                switch tab {
                case .overview:
                    detailsSection(contact)
                    CrmContactInvoicesSection(client: client, ref: ref, contact: contact, state: $invoices)
                    if editable, let data, let flow {
                        pipelineSection(contact, data: data, flow: flow)
                        if !data.listViews.isEmpty { listsSection(contact, data: data) }
                    }
                case .activity:
                    if let data { timelineSection(contact, data: data) }
                    if editable, let data {
                        let duplicates = data.duplicates(of: contact)
                        if !duplicates.isEmpty {
                            Section("Possible duplicates") {
                                ForEach(duplicates) { other in
                                    Button {
                                        sheet = .merge(other)
                                    } label: {
                                        HStack {
                                            CrmContactRow(contact: other, flow: data.flow(other.flow))
                                            Image(systemName: "arrow.triangle.merge").foregroundStyle(Color.accentColor)
                                        }
                                    }
                                    .buttonStyle(.plain)
                                }
                            }
                        }
                    }
                case .notes:
                    Section("Notes") {
                        if contact.notes.isEmpty {
                            Button("Add a note", systemImage: "square.and.pencil") { sheet = .edit(contact) }
                                .disabled(!editable)
                        } else {
                            Text(contact.notes).textSelection(.enabled)
                        }
                    }
                    if !contact.fields.isEmpty {
                        Section("More") {
                            ForEach(contact.fields.keys.sorted(), id: \.self) { key in
                                LabeledContent(CrmColumnCatalog.label(key)) {
                                    Text(contact.fields[key] ?? "").textSelection(.enabled).multilineTextAlignment(.trailing)
                                }
                            }
                        }
                    }
                    Section {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Added \(PackagesFormat.date(contact.createdAt)) by \(contact.createdBy.isEmpty ? "someone" : contact.createdBy)")
                            Text("Updated \(PackagesFormat.relative(contact.updatedAt)) by \(contact.updatedBy.isEmpty ? contact.createdBy : contact.updatedBy)")
                        }
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    }
                }
            } else {
                ProgressView().frame(maxWidth: .infinity)
            }
        }
        .listSectionSpacing(.compact)
        .environment(\.defaultMinListRowHeight, CrmLayout.dense ? 36 : 44)
        .navigationTitle(contact?.displayName ?? "Contact")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { toolbar }
        .confirmationDialog(
            "Delete \(contact?.displayName ?? "this contact")?", isPresented: $confirmDelete, titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                Task { await delete() }
            }
        } message: {
            Text("The contact and its pipeline history are removed for everyone in \(ref.organization).")
        }
        .task { await load() }
        .fmRefreshable { await load() }
        .sheet(item: $sheet, onDismiss: leaveIfGone) { item in
            CrmSheetHost(model: model, sheet: item, present: { sheet = $0 })
        }
        .onAppear { visible = true }
        .onDisappear { visible = false }
        .onChange(of: model.notice) { _, text in
            guard visible, !text.isEmpty else { return }
            notice = FmNotice(text)
            model.notice = ""
        }
    }

    private func leaveIfGone() {
        guard editable, model.contact(ref.contactId) == nil else { return }
        dismiss()
    }

    @ToolbarContentBuilder private var toolbar: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            Button("Edit") {
                if let contact { sheet = .edit(contact) }
            }
            .disabled(contact == nil || !editable)
        }
        ToolbarItem(placement: .secondaryAction) {
            Button("Draft outreach with an agent", systemImage: "sparkles") {
                if let contact { AppNavigator.shared?.router.requestCompose(ComposeTarget(text: prompt(contact))) }
            }
            .disabled(contact == nil)
        }
        ToolbarItem(placement: .secondaryAction) {
            Button("Copy prompt", systemImage: "doc.on.doc") {
                if let contact {
                    CrmClipboard.copy(prompt(contact))
                    notice = FmNotice("Copied a prompt about \(contact.displayName).")
                }
            }
            .disabled(contact == nil)
        }
        ToolbarItem(placement: .secondaryAction) {
            Button("Copy link", systemImage: "link") {
                if let contact, let url = CrmPrompt.contactLink(organization: model.organization, contactId: contact.id) {
                    CrmClipboard.copy(url.absoluteString)
                    notice = FmNotice("Copied a link to \(contact.displayName). It opens for members of \(model.organization).")
                }
            }
            .disabled(contact == nil)
        }
        ToolbarItem(placement: .secondaryAction) {
            Button("Merge into…", systemImage: "arrow.triangle.merge") {
                if let contact { sheet = .merge(contact) }
            }
            .disabled(contact == nil || !editable)
        }
        ToolbarItem(placement: .secondaryAction) {
            Button("Delete contact", systemImage: "trash", role: .destructive) { confirmDelete = true }
                .disabled(contact == nil || !editable || model.saving)
        }
    }

    private func prompt(_ contact: CrmContact) -> String {
        let flowName = model.data?.flow(contact.flow).name ?? CrmStageLabel.title(contact.flow)
        return CrmPrompt.outreach(contact, sheet: model.activeTitle, flowName: flowName)
    }

    private func quickActions(_ contact: CrmContact, flow: CrmFlow?) -> some View {
        HStack(spacing: 8) {
            CrmPillButton(title: "Email", icon: "envelope.fill", tint: .blue) {
                sheet = .compose(CrmEmailDraft(contact: contact), outreach: false)
            }
            .disabled(contact.email.isEmpty || !editable)
            CrmPillButton(title: "Call", icon: "phone.fill", tint: .green) {
                if let url = URL(string: "tel:" + contact.phone.filter { !$0.isWhitespace }) { openURL(url) }
            }
            .disabled(contact.phone.isEmpty)
            CrmPillButton(title: "Contacted", icon: "checkmark.message.fill", tint: .teal) {
                Task {
                    if await model.markContacted([contact.id]) {
                        FmHaptics.commit()
                        notice = FmNotice("Marked as contacted now.")
                    }
                }
            }
            .disabled(!editable)
            CrmPillButton(title: "Deck", icon: "rectangle.on.rectangle.angled", tint: .orange) {
                Task {
                    if let link = await model.deckLink(for: contact) { sheet = .deck(link) }
                }
            }
            .disabled(!editable)
        }
        .padding(.horizontal, 2)
    }

    private func pipelineSection(_ contact: CrmContact, data: CrmOrganizationData, flow: CrmFlow) -> some View {
        Section {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(stages(for: contact, flow: flow), id: \.self) { stage in
                        let active = stage == contact.stage
                        Button {
                            Task { await move(contact, to: stage) }
                        } label: {
                            CrmChip(text: CrmStageLabel.title(stage), icon: active ? "checkmark" : nil, tint: CrmPalette.stage(stage, in: flow), filled: active)
                                .font(.caption)
                        }
                        .buttonStyle(.plain)
                        .disabled(model.saving)
                    }
                }
                .padding(.vertical, 2)
            }
            Picker("Pipeline", selection: Binding(
                get: { contact.flow },
                set: { target in Task { await moveFlow(contact, to: target) } })
            ) {
                ForEach(data.flows) { Label($0.name, systemImage: CrmFlowIcon.symbol($0.icon)).tag($0.id) }
                if !data.flows.contains(where: { $0.id == contact.flow }) {
                    Text(CrmStageLabel.title(contact.flow)).tag(contact.flow)
                }
            }
            .disabled(model.saving)
            LabeledContent("Last contacted") {
                Text(contact.lastContactedAt > 0 ? PackagesFormat.relative(contact.lastContactedAt) : "Never")
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("Pipeline")
        }
    }

    private func listsSection(_ contact: CrmContact, data: CrmOrganizationData) -> some View {
        Section("Lists") {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(data.listViews) { view in
                        let member = contact.views.contains(view.id)
                        Button {
                            Task {
                                let ok = await model.bulk(member ? .removeList(view.id) : .addList(view.id), ids: [contact.id])
                                if ok { FmHaptics.selection() }
                            }
                        } label: {
                            CrmChip(text: view.name, icon: member ? "checkmark" : "plus", tint: .accentColor, filled: member)
                                .font(.caption)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.vertical, 2)
            }
        }
    }

    private func detailsSection(_ contact: CrmContact) -> some View {
        Section("Details") {
            detail("Email", contact.email, icon: "envelope", url: contact.email.isEmpty ? nil : URL(string: "mailto:" + contact.email))
            detail("Phone", contact.phone, icon: "phone", url: URL(string: "tel:" + contact.phone.filter { !$0.isWhitespace }))
            detail("Website", contact.website, icon: "globe", url: CrmText.profileURL("website", contact.website))
            detail("Location", contact.location, icon: "mappin.and.ellipse")
            detail("LinkedIn", contact.linkedin, icon: "person.crop.square", url: CrmText.profileURL("linkedin", contact.linkedin))
            detail("GitHub", contact.github, icon: "chevron.left.forwardslash.chevron.right", url: CrmText.profileURL("github", contact.github))
            detail("Mastodon", contact.mastodon, icon: "bubble.left.and.bubble.right", url: CrmText.profileURL("mastodon", contact.mastodon))
            detail("Bluesky", contact.bluesky, icon: "cloud", url: CrmText.profileURL("bluesky", contact.bluesky))
            detail("X", contact.twitter, icon: "at", url: CrmText.profileURL("twitter", contact.twitter))
            detail("Next step", contact.nextStep, icon: "arrow.turn.down.right")
            detail("Owner", contact.owner, icon: "person.crop.circle")
            detail("Source", contact.source, icon: "arrow.down.backward.circle")
            if !contact.tags.isEmpty {
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 4) {
                        ForEach(contact.tags, id: \.self) { CrmChip(text: "#" + $0, tint: .purple) }
                    }
                }
            }
        }
    }

    private func timelineSection(_ contact: CrmContact, data: CrmOrganizationData) -> some View {
        let emails = data.activity(for: contact.id)
        let decks = data.decks(for: contact.id)
        return Section {
            ForEach(decks) { link in
                Button {
                    sheet = .deck(link)
                } label: {
                    CrmDeckRow(link: link, contact: nil)
                }
                .buttonStyle(.plain)
            }
            ForEach(emails) { email in
                Button {
                    sheet = .email(email)
                } label: {
                    CrmActivityRow(activity: email, contact: nil)
                }
                .buttonStyle(.plain)
            }
            if emails.isEmpty && decks.isEmpty {
                Text("No emails or deck views yet.").font(.subheadline).foregroundStyle(.secondary)
            }
        } header: {
            HStack {
                Text("Timeline")
                Spacer()
                if editable && !contact.email.isEmpty {
                    Menu {
                        Button("Logged email I sent", systemImage: "paperplane") {
                            Task { await log(contact, direction: "outbound") }
                        }
                        Button("Logged email I received", systemImage: "tray.and.arrow.down") {
                            Task { await log(contact, direction: "inbound") }
                        }
                    } label: {
                        Label("Log", systemImage: "plus.circle").labelStyle(.titleAndIcon).font(.caption.weight(.semibold))
                    }
                }
            }
        }
    }

    private func log(_ contact: CrmContact, direction: String) async {
        let subject = direction == "inbound" ? "Email from \(contact.displayName)" : "Email to \(contact.displayName)"
        if await model.logEmail(to: contact.email, subject: subject, contactId: contact.id, direction: direction) != nil {
            FmHaptics.commit()
        }
    }

    private func stages(for contact: CrmContact, flow: CrmFlow) -> [String] {
        flow.stages.contains(contact.stage) ? flow.stages : flow.stages + [contact.stage]
    }

    private func move(_ contact: CrmContact, to stage: String) async {
        guard stage != contact.stage else { return }
        if await model.move(contact, toStage: stage) {
            FmHaptics.selection()
            loaded = model.contact(contact.id) ?? loaded
            errorText = ""
        } else {
            errorText = model.errorText
        }
    }

    private func moveFlow(_ contact: CrmContact, to flow: String) async {
        guard flow != contact.flow else { return }
        if await model.move(contact, toFlow: flow) {
            FmHaptics.selection()
            loaded = model.contact(contact.id) ?? loaded
        } else {
            errorText = model.errorText
        }
    }

    private func delete() async {
        guard let contact, editable else { return }
        if await model.delete(contact) {
            FmHaptics.commit(.delete)
            dismiss()
        } else {
            errorText = model.errorText
        }
    }

    private func load() async {
        if model.organization != ref.organization { model.organization = ref.organization }
        await model.load()
        do {
            loaded = try await client.crmContact(ref)
            errorText = ""
        } catch {
            errorText = PackagesError.describe(error)
        }
        invoices = await CrmContactInvoicesSection.fetch(client: client, ref: ref)
    }

    @ViewBuilder private func detail(_ title: String, _ value: String, icon: String, url: URL? = nil) -> some View {
        if !value.isEmpty {
            HStack(spacing: 10) {
                Image(systemName: icon).font(.footnote).foregroundStyle(.secondary).frame(width: 20)
                Text(title).font(.footnote).foregroundStyle(.secondary).frame(width: 78, alignment: .leading)
                if let url, url.scheme != nil {
                    Link(value, destination: url).font(.subheadline).lineLimit(1).truncationMode(.middle)
                } else {
                    Text(value).font(.subheadline).lineLimit(2).textSelection(.enabled)
                }
                Spacer(minLength: 0)
            }
            .contextMenu {
                Button("Copy", systemImage: "doc.on.doc") { CrmClipboard.copy(value) }
            }
        }
    }
}

enum CrmContactTab: String, CaseIterable, Identifiable {
    case overview
    case activity
    case notes

    var id: String { rawValue }

    var title: String {
        switch self {
        case .overview: return "Overview"
        case .activity: return "Activity"
        case .notes: return "Notes"
        }
    }
}

struct CrmContactHeader: View {
    let contact: CrmContact
    let flow: CrmFlow?

    var body: some View {
        let dense = CrmLayout.dense
        VStack(spacing: 8) {
            HStack(alignment: .center, spacing: 12) {
                CrmAvatar(name: contact.name, email: contact.email, size: dense ? 46 : 56)
                VStack(alignment: .leading, spacing: 3) {
                    Text(contact.displayName).font((dense ? Font.headline : Font.title3).bold()).lineLimit(2)
                    if !contact.subtitle.isEmpty {
                        Text(contact.subtitle).font(.footnote).foregroundStyle(.secondary).lineLimit(1)
                    }
                    HStack(spacing: 5) {
                        CrmStageBadge(stage: contact.stage, flow: flow)
                        if let flow { CrmChip(text: flow.name, icon: CrmFlowIcon.symbol(flow.icon), tint: .secondary) }
                        if contact.value > 0 { CrmChip(text: CrmText.money(contact.value), icon: "dollarsign", tint: .green) }
                    }
                }
                Spacer(minLength: 0)
            }
            if let flow, flow.stages.contains(contact.stage) {
                CrmStageDots(flow: flow, stage: contact.stage)
            }
        }
        .padding(12)
        .background(
            LinearGradient(
                colors: [CrmPalette.stage(contact.stage, in: flow).opacity(0.16), Color(uiColor: .secondarySystemGroupedBackground)],
                startPoint: .topLeading, endPoint: .bottomTrailing),
            in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    }
}

struct CrmActivityRow: View {
    let activity: CrmActivity
    let contact: CrmContact?

    private var icon: String {
        if activity.failed { return "exclamationmark.triangle.fill" }
        if activity.incoming { return "tray.and.arrow.down.fill" }
        if activity.engaged { return "eye.fill" }
        return "paperplane.fill"
    }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon)
                .font(.caption)
                .foregroundStyle(CrmPalette.status(activity))
                .frame(width: 26, height: 26)
                .background(CrmPalette.status(activity).opacity(0.12), in: Circle())
            VStack(alignment: .leading, spacing: 2) {
                Text(activity.subject.isEmpty ? "(no subject)" : activity.subject).font(.subheadline.weight(.medium)).lineLimit(1)
                Text([contact?.displayName ?? activity.to, activity.sender, PackagesFormat.relative(activity.at)]
                    .filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if !activity.incoming, !activity.engagementLine.isEmpty {
                    Text(activity.engagementLine)
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(.tint)
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 0)
            CrmChip(text: activity.status, tint: CrmPalette.status(activity))
        }
        .accessibilityElement(children: .combine)
    }
}

struct CrmContactEditor: View {
    @Bindable var model: CrmModel
    let original: CrmContact?
    var addTo: String?
    @State private var edit: CrmContactEdit
    @State private var newField = ""
    @State private var newValue = ""
    @State private var submitting = false
    @State private var moreOpen: Bool
    @State private var requestId = CrmModel.newRequestId()
    @Environment(\.dismiss) private var dismiss

    init(model: CrmModel, original: CrmContact?, addTo: String? = nil) {
        self.model = model
        self.original = original
        self.addTo = addTo
        var start = original.map(CrmContactEdit.init(contact:)) ?? CrmContactEdit()
        if original == nil {
            let flow = model.activeFlow ?? model.flow
            start.flow = flow.id
            start.stage = model.filter.stage.isEmpty ? (flow.stages.first ?? "lead") : model.filter.stage
        }
        _edit = State(initialValue: start)
        _moreOpen = State(initialValue: false)
    }

    private var fieldKeys: [String] {
        let known = model.data?.customFieldKeys() ?? []
        return known + edit.fields.keys.sorted().filter { !known.contains($0) }
    }

    private var moreFilled: Int {
        [edit.owner, edit.source, edit.tags, edit.website, edit.location, edit.linkedin, edit.github, edit.mastodon,
         edit.bluesky, edit.twitter].filter { !$0.isEmpty }.count + edit.fields.values.filter { !$0.isEmpty }.count
    }

    var body: some View {
        ScrollViewReader { proxy in
            Form {
                problem.id("crm-editor-top")
                Section {
                    TextField("Name", text: $edit.name).textContentType(.name)
                    TextField("Email", text: $edit.email).textContentType(.emailAddress).keyboardType(.emailAddress)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    TextField("Company", text: $edit.company).textContentType(.organizationName)
                    TextField("Title", text: $edit.title).textContentType(.jobTitle)
                    TextField("Phone", text: $edit.phone).textContentType(.telephoneNumber).keyboardType(.phonePad)
                } header: {
                    Text("Contact")
                } footer: {
                    if !edit.isValid { Text("A name or an email is enough to save.") }
                }
                Section("Pipeline") {
                    if let data = model.data {
                        Picker("Pipeline", selection: $edit.flow) {
                            ForEach(data.flows) { Text($0.name).tag($0.id) }
                        }
                        .onChange(of: edit.flow) { _, flow in
                            let stages = data.flow(flow).stages
                            if !stages.contains(edit.stage) { edit.stage = stages.first ?? "lead" }
                        }
                        Picker("Stage", selection: $edit.stage) {
                            ForEach(data.flow(edit.flow).stages, id: \.self) { Text(CrmStageLabel.title($0)).tag($0) }
                        }
                    }
                    LabeledContent("Deal value") {
                        TextField("0", value: $edit.value, format: IntegerFormatStyle<Int64>())
                            .keyboardType(.numberPad)
                            .multilineTextAlignment(.trailing)
                    }
                    TextField("Next step", text: $edit.nextStep)
                }
                Section {
                    DisclosureGroup(isExpanded: $moreOpen) {
                        moreFields
                    } label: {
                        HStack {
                            Text("More details")
                            Spacer()
                            if moreFilled > 0 {
                                Text("\(moreFilled) filled").font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
                Section("Notes") {
                    TextField("Notes", text: $edit.notes, axis: .vertical).lineLimit(2...10)
                }
            }
            .onChange(of: model.errorText) { _, text in
                if !text.isEmpty { withAnimation { proxy.scrollTo("crm-editor-top", anchor: .top) } }
            }
        }
        .navigationTitle(original == nil ? "New contact" : "Edit contact")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { model.clearError() }
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") {
                    model.clearError()
                    dismiss()
                }
            }
            ToolbarItem(placement: .confirmationAction) {
                Button {
                    save()
                } label: {
                    if submitting { ProgressView() } else { Text("Save").bold() }
                }
                .disabled(!edit.isValid || submitting || model.saving)
                .accessibilityIdentifier("crm-contact-save")
            }
        }
        .interactiveDismissDisabled(submitting)
    }

    @ViewBuilder private var problem: some View {
        if let found = model.existing, original == nil {
            Section {
                HStack(spacing: 10) {
                    CrmAvatar(name: found.name, email: found.email, size: 36)
                    VStack(alignment: .leading, spacing: 1) {
                        Text("\(found.displayName) is already in the CRM").font(.subheadline.weight(.semibold))
                        Text(found.email.isEmpty ? "Same email address" : found.email).font(.caption).foregroundStyle(.secondary)
                    }
                }
                NavigationLink {
                    CrmContactDestination(routeId: found.routeId)
                } label: {
                    Label("Open \(found.displayName)", systemImage: "person.crop.circle")
                }
                Button {
                    addDetails(to: found)
                } label: {
                    Label(addTo == nil ? "Add these details to them" : "Add these details and the list", systemImage: "square.and.arrow.down.on.square")
                }
                .disabled(submitting)
            } footer: {
                Text("Adding fills only what their record is missing; nothing they already have is overwritten.")
            }
        } else if !model.errorText.isEmpty {
            Section {
                Label(model.errorText, systemImage: "exclamationmark.triangle.fill")
                    .font(.subheadline)
                    .foregroundStyle(.red)
            }
        }
    }

    @ViewBuilder private var moreFields: some View {
        HStack {
            TextField("Owner", text: $edit.owner).textInputAutocapitalization(.never)
            if let members = model.data?.members, !members.isEmpty {
                Menu {
                    ForEach(members, id: \.self) { member in Button(member) { edit.owner = member } }
                } label: {
                    Image(systemName: "person.crop.circle.badge.plus")
                }
            }
        }
        TextField("Source", text: $edit.source)
        TextField("Tags, comma separated", text: $edit.tags).textInputAutocapitalization(.never)
        CrmLastContactedRow(value: $edit.lastContactedAt)
        TextField("Website", text: $edit.website).keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
        TextField("Location", text: $edit.location)
        TextField("LinkedIn", text: $edit.linkedin).textInputAutocapitalization(.never).autocorrectionDisabled()
        TextField("GitHub", text: $edit.github).textInputAutocapitalization(.never).autocorrectionDisabled()
        TextField("Mastodon (@name@server)", text: $edit.mastodon).textInputAutocapitalization(.never).autocorrectionDisabled()
        TextField("Bluesky", text: $edit.bluesky).textInputAutocapitalization(.never).autocorrectionDisabled()
        TextField("X / Twitter", text: $edit.twitter).textInputAutocapitalization(.never).autocorrectionDisabled()
        ForEach(fieldKeys, id: \.self) { key in
            TextField(CrmColumnCatalog.label(key), text: Binding(
                get: { edit.fields[key] ?? "" },
                set: { edit.fields[key] = $0 }))
        }
        HStack {
            TextField("New field", text: $newField).textInputAutocapitalization(.never)
            TextField("Value", text: $newValue)
            Button {
                let key = CrmText.slug(newField).replacingOccurrences(of: "-", with: "_")
                guard !key.isEmpty, !CrmColumnCatalog.isBuiltIn(key) else { return }
                edit.fields[key] = newValue
                newField = ""
                newValue = ""
            } label: {
                Image(systemName: "plus.circle.fill")
            }
            .disabled(newField.trimmingCharacters(in: .whitespaces).isEmpty)
        }
    }

    private func save() {
        guard !submitting else { return }
        submitting = true
        Task {
            defer { submitting = false }
            if await model.save(edit, original: original, addTo: addTo, requestId: original == nil ? requestId : "") != nil {
                FmHaptics.commit()
                dismiss()
            } else {
                FmHaptics.failure()
            }
        }
    }

    private func addDetails(to found: CrmContact) {
        guard !submitting else { return }
        submitting = true
        Task {
            defer { submitting = false }
            if await model.addDetails(edit, to: found, addTo: addTo) != nil {
                FmHaptics.commit()
                model.notice = "Added the details to \(found.displayName)."
                dismiss()
            } else {
                FmHaptics.failure()
            }
        }
    }
}

struct CrmMergeSheet: View {
    @Bindable var model: CrmModel
    let contact: CrmContact
    @State private var query = ""
    @State private var target: CrmContact?
    @Environment(\.dismiss) private var dismiss

    private var candidates: [CrmContact] {
        guard let data = model.data else { return [] }
        let duplicates = data.duplicates(of: contact)
        let others = data.contacts.filter { $0.id != contact.id && !duplicates.contains($0) && $0.matches(query) }
        return query.isEmpty ? duplicates + others.prefix(40) : duplicates.filter { $0.matches(query) } + others
    }

    var body: some View {
        List {
            Section {
                CrmContactRow(contact: contact, flow: model.data?.flow(contact.flow))
            } header: {
                Text("Merge this contact")
            } footer: {
                Text("Its details fill the blanks of the contact you keep, its email history and deck links move over, and then it is deleted.")
            }
            Section("Into") {
                ForEach(candidates) { other in
                    Button {
                        target = other
                    } label: {
                        CrmContactRow(contact: other, flow: model.data?.flow(other.flow))
                    }
                    .buttonStyle(.plain)
                }
                if candidates.isEmpty { Text("No other contacts match.").foregroundStyle(.secondary) }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .searchable(text: $query, prompt: "Find the contact to keep")
        .navigationTitle("Merge")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
        }
        .confirmationDialog(
            "Merge \(contact.displayName) into \(target?.displayName ?? "")?",
            isPresented: Binding(get: { target != nil }, set: { if !$0 { target = nil } }), titleVisibility: .visible
        ) {
            Button("Merge", role: .destructive) {
                guard let kept = target else { return }
                Task {
                    if await model.merge(contact, into: kept) != nil {
                        FmHaptics.commit()
                        dismiss()
                    }
                }
            }
        }
    }
}

struct CrmOwnerSheet: View {
    @Bindable var model: CrmModel
    let ids: [String]
    @State private var owner = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Form {
            Section {
                TextField("Owner", text: $owner).textInputAutocapitalization(.never)
            } footer: {
                Text("Sets the owner of \(ids.count) contact\(ids.count == 1 ? "" : "s"). Leave it empty to clear it.")
            }
            Section("Members") {
                ForEach(model.data?.members ?? [], id: \.self) { member in
                    Button {
                        owner = member
                    } label: {
                        HStack {
                            CrmAvatar(name: member, email: "", size: 26)
                            Text(member).foregroundStyle(.primary)
                            Spacer()
                            if owner == member { Image(systemName: "checkmark").foregroundStyle(Color.accentColor) }
                        }
                    }
                }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("Set owner")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Apply") {
                    Task {
                        if await model.bulk(.owner(owner.trimmingCharacters(in: .whitespaces)), ids: ids) {
                            FmHaptics.commit()
                            dismiss()
                        }
                    }
                }
                .disabled(model.saving)
            }
        }
    }
}

struct CrmEmailSheet: View {
    @Bindable var model: CrmModel
    let email: CrmActivity
    @State private var detail: CrmActivity?
    @State private var fetched = false
    @State private var confirmDelete = false
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    private var shown: CrmActivity { detail ?? email }

    var body: some View {
        List {
            Section {
                VStack(alignment: .leading, spacing: 6) {
                    Text(shown.subject.isEmpty ? "(no subject)" : shown.subject).font(.headline)
                    HStack(spacing: 6) {
                        CrmChip(text: shown.status, tint: CrmPalette.status(shown))
                        CrmChip(text: shown.incoming ? "Received" : "Sent", icon: shown.incoming ? "tray.and.arrow.down" : "paperplane", tint: .secondary)
                        if !shown.provider.isEmpty { CrmChip(text: shown.provider, tint: .secondary) }
                    }
                }
                LabeledContent("To", value: shown.to)
                if !shown.fromEmail.isEmpty { LabeledContent("From", value: shown.fromName.isEmpty ? shown.fromEmail : "\(shown.fromName) <\(shown.fromEmail)>") }
                if !shown.sender.isEmpty { LabeledContent("By", value: shown.sender) }
                LabeledContent("When", value: PackagesFormat.relative(shown.at))
                if !shown.error.isEmpty { Text(shown.error).font(.footnote).foregroundStyle(.red).fmMessageTools(shown.error) }
            }
            if !shown.incoming, !shown.engagementLine.isEmpty {
                Section {
                    LabeledContent("Opens", value: "\(shown.opens)")
                    LabeledContent("Clicks", value: "\(shown.clicks)")
                    LabeledContent("Read for", value: shown.readLabel.isEmpty ? (shown.silentClient ? "\(shown.mailClient) doesn't report it" : "Not reported") : shown.readLabel)
                    LabeledContent(shown.slideCount > 0 ? "Reached" : "Scrolled", value: shown.slideCount > 0 && shown.slideReached > 0 ? "Slide \(shown.slideReached) of \(shown.slideCount)" + (shown.slideTitle.isEmpty ? "" : " · \(shown.slideTitle)") : (shown.scrollLabel.isEmpty ? "Not reported" : shown.scrollLabel))
                    if !shown.mailClient.isEmpty {
                        LabeledContent("Mail app", value: shown.mailClient)
                    }
                } header: {
                    Text("Reading")
                } footer: {
                    Text("Read time and scroll come from mail apps that run CSS animations, such as Apple Mail without Mail Privacy Protection. Gmail and protected inboxes report opens and clicks only.")
                }
            }
            if !shown.plainBody.isEmpty {
                Section("Message") { Text(shown.plainBody).font(.subheadline).textSelection(.enabled) }
            } else if !fetched {
                Section { ProgressView().frame(maxWidth: .infinity) }
            }
            Section {
                if let url = URL(string: shown.providerUrl), url.scheme == "https" {
                    Button("Open in mailbox", systemImage: "arrow.up.forward.app") { openURL(url) }
                }
                Menu {
                    ForEach(CrmActivity.statuses, id: \.self) { status in
                        Button(CrmStageLabel.title(status)) {
                            Task { if let updated = await model.setEmailStatus(shown, status: status) { detail = updated } }
                        }
                    }
                } label: {
                    Label("Set status", systemImage: "flag")
                }
                Button("Delete record", systemImage: "trash", role: .destructive) { confirmDelete = true }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("Email")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
        }
        .task {
            detail = await model.emailDetail(email.id)
            fetched = true
        }
        .confirmationDialog("Delete this email record?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task {
                    if await model.deleteEmail(email) {
                        FmHaptics.commit(.delete)
                        dismiss()
                    }
                }
            }
        } message: {
            Text("Only the CRM's record goes; the message stays in the mailbox.")
        }
    }
}

struct CrmLastContactedRow: View {
    @Binding var value: Int64

    var body: some View {
        if value > 0 {
            HStack {
                DatePicker(
                    "Last contacted",
                    selection: Binding(
                        get: { Date(timeIntervalSince1970: Double(value) / 1000) },
                        set: { value = Int64($0.timeIntervalSince1970 * 1000) }),
                    in: ...Date(), displayedComponents: [.date, .hourAndMinute])
                Button {
                    value = 0
                } label: {
                    Image(systemName: "xmark.circle.fill").foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Clear last contacted")
            }
        } else {
            Button {
                value = Int64(Date().timeIntervalSince1970 * 1000)
            } label: {
                LabeledContent("Last contacted", value: "Never · set")
            }
        }
    }
}

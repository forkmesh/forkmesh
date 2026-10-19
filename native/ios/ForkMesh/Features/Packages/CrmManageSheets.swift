import FMBase
import FMPackages
import SwiftUI

struct CrmNewListSheet: View {
    @Bindable var model: CrmModel
    @State private var name = ""
    @State private var flow = ""
    @State private var isPrivate = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Form {
            Section {
                TextField("List name", text: $name)
                Picker("Shows", selection: $flow) {
                    Text("Contacts I add to it").tag("")
                    ForEach(model.data?.flows ?? []) { Label("Pipeline: \($0.name)", systemImage: CrmFlowIcon.symbol($0.icon)).tag($0.id) }
                }
                Toggle("Only me", isOn: $isPrivate)
            } footer: {
                Text(flow.isEmpty
                    ? "A plain list holds the contacts you add to it."
                    : "A pipeline list shows the pipeline's contacts, split by stage.")
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("New list")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Create") {
                    Task {
                        if await model.createList(name: name, flow: flow, visibility: isPrivate ? "private" : "organization") != nil {
                            FmHaptics.commit()
                            dismiss()
                        }
                    }
                }
                .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty || model.saving)
            }
        }
    }
}

struct CrmPipelineEditor: View {
    @Bindable var model: CrmModel
    @State private var draft: CrmFlowDraft
    @State private var newStage = ""
    @State private var confirmDelete = false
    @Environment(\.dismiss) private var dismiss

    init(model: CrmModel, draft: CrmFlowDraft) {
        self.model = model
        _draft = State(initialValue: draft)
    }

    var body: some View {
        Form {
            Section("Pipeline") {
                TextField("Name", text: $draft.name)
                Picker("Kind", selection: $draft.icon) {
                    ForEach(CrmFlowIcon.all, id: \.self) { icon in
                        Label(CrmFlowIcon.title(icon), systemImage: CrmFlowIcon.symbol(icon)).tag(icon)
                    }
                }
            }
            Section {
                ForEach(Array(draft.stages.enumerated()), id: \.offset) { index, _ in
                    HStack {
                        Circle()
                            .fill(CrmPalette.stage(CrmText.slug(draft.stages[index]), in: draft.flow))
                            .frame(width: 9, height: 9)
                        TextField("Stage", text: Binding(
                            get: { draft.stages.indices.contains(index) ? draft.stages[index] : "" },
                            set: { if draft.stages.indices.contains(index) { draft.stages[index] = $0 } }))
                    }
                }
                .onMove { draft.stages.move(fromOffsets: $0, toOffset: $1) }
                .onDelete { draft.stages.remove(atOffsets: $0) }
                HStack {
                    TextField("Add a stage", text: $newStage).onSubmit(addStage)
                    Button(action: addStage) { Image(systemName: "plus.circle.fill") }
                        .disabled(newStage.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            } header: {
                HStack {
                    Text("Stages")
                    Spacer()
                    EditButton().font(.caption)
                }
            } footer: {
                Text("Contacts at a stage you rename move with it; contacts at a stage you remove go to the first stage.")
            }
            if !draft.isNew, (model.data?.flows.count ?? 0) > 1 {
                Section {
                    Button("Delete pipeline", systemImage: "trash", role: .destructive) { confirmDelete = true }
                }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle(draft.isNew ? "New pipeline" : draft.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    Task {
                        if await model.saveFlow(draft) != nil {
                            FmHaptics.commit()
                            dismiss()
                        }
                    }
                }
                .disabled(draft.flow == nil || model.saving || !model.canManage)
            }
        }
        .confirmationDialog("Delete \(draft.name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task {
                    if await model.deleteFlow(draft.id) {
                        FmHaptics.commit(.delete)
                        dismiss()
                    }
                }
            }
        } message: {
            Text("Its lists are deleted too. Its contacts stay, under No pipeline, until you move them.")
        }
    }

    private func addStage() {
        let stage = newStage.trimmingCharacters(in: .whitespaces)
        guard !stage.isEmpty else { return }
        draft.stages.append(stage)
        newStage = ""
    }
}

struct CrmPipelinesSheet: View {
    @Bindable var model: CrmModel
    @State private var editing: CrmFlowDraft?
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        List {
            Section {
                ForEach(model.data?.flows ?? []) { flow in
                    Button {
                        editing = CrmFlowDraft(flow: flow)
                    } label: {
                        HStack(spacing: 12) {
                            Image(systemName: CrmFlowIcon.symbol(flow.icon))
                                .foregroundStyle(Color.accentColor)
                                .frame(width: 30, height: 30)
                                .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text(flow.name).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                                    Spacer()
                                    Text("\(model.data?.contacts(in: flow).count ?? 0)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                                }
                                CrmStageDots(flow: flow, stage: flow.stages.last ?? "")
                                Text(flow.stages.map(CrmStageLabel.title).joined(separator: " → "))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(2)
                            }
                        }
                    }
                    .disabled(!model.canManage)
                }
                .onMove { from, to in
                    guard var order = model.data?.flows.map(\.id) else { return }
                    order.move(fromOffsets: from, toOffset: to)
                    Task { await model.reorderFlows(order) }
                }
            } footer: {
                Text(model.canManage ? "Up to 12 pipelines. Tap one to rename it or edit its stages." : "Only owners and admins change pipelines.")
            }
            if model.canManage, (model.data?.flows.count ?? 0) < 12 {
                Button("New pipeline", systemImage: "plus") { editing = CrmFlowDraft() }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("Pipelines")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { if model.canManage { EditButton() } }
            ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
        }
        .navigationDestination(item: $editing) { draft in
            CrmPipelineEditor(model: model, draft: draft)
        }
    }
}

struct CrmTabSettingsSheet: View {
    @Bindable var model: CrmModel
    let item: CrmTabItem
    @State private var name: String
    @State private var savedName: String
    @State private var shareText: String
    @State private var access: String
    @State private var isPrivate: Bool
    @State private var columns: [CrmColumn]
    @State private var history: [CrmListActivityEntry] = []
    @State private var more = false
    @State private var confirmDelete = false
    @State private var pendingColumn: CrmColumn?
    @Environment(\.dismiss) private var dismiss

    init(model: CrmModel, item: CrmTabItem) {
        self.model = model
        self.item = item
        _name = State(initialValue: item.title)
        _savedName = State(initialValue: item.title)
        _shareText = State(initialValue: item.view?.sharedWith.joined(separator: ", ") ?? "")
        _access = State(initialValue: item.view?.shareAccess ?? "view")
        _isPrivate = State(initialValue: item.view?.isPrivate ?? false)
        _columns = State(initialValue: CrmColumnCatalog.editorColumns(
            item.view?.columns ?? [], removed: model.data?.settings.removedColumns ?? []))
    }

    private var view: CrmView? { model.data?.views.first { $0.id == item.view?.id } ?? item.view }

    private var canShare: Bool { view?.canManage ?? model.canManage }

    var body: some View {
        Form {
            Section("Name") {
                HStack {
                    TextField("Name", text: $name)
                    if name != savedName {
                        Button("Rename") {
                            Task {
                                if await model.renameTab(item, to: name) {
                                    savedName = name
                                    FmHaptics.commit()
                                }
                            }
                        }
                        .buttonStyle(.borderless)
                    }
                }
            }
            if let view {
                listSections(view)
            } else if item.flow != nil {
                Section {
                    shareFields
                    Button("Share this pipeline", systemImage: "person.2.badge.plus") { Task { await share() } }
                } header: {
                    Text("Sharing")
                } footer: {
                    Text("Sharing turns the pipeline into a list you can give to teammates or other ForkMesh users.")
                }
                if let flow = item.flow, model.canManage {
                    Section {
                        NavigationLink("Edit stages") { CrmPipelineEditor(model: model, draft: CrmFlowDraft(flow: flow)) }
                    }
                }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle(item.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
        }
        .task {
            if let view {
                let page = await model.listActivity(view)
                history = page.entries
                more = page.more
            }
        }
        .confirmationDialog("Delete \(item.title)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete list", role: .destructive) {
                guard let view else { return }
                Task {
                    if await model.deleteList(view) {
                        FmHaptics.commit(.delete)
                        dismiss()
                    }
                }
            }
        } message: {
            Text("The contacts stay in the CRM; only the list goes.")
        }
        .confirmationDialog(
            "Delete the \(pendingColumn?.label ?? "") column?",
            isPresented: Binding(get: { pendingColumn != nil }, set: { if !$0 { pendingColumn = nil } }), titleVisibility: .visible
        ) {
            Button("Delete column", role: .destructive) {
                guard let column = pendingColumn else { return }
                pendingColumn = nil
                Task {
                    if await model.deleteColumn(column.field) { columns.removeAll { $0.field == column.field } }
                }
            }
        } message: {
            Text("Its values are erased from every contact in the organization.")
        }
    }

    @ViewBuilder private func listSections(_ view: CrmView) -> some View {
        Section {
            if let flow = model.data?.flows.first(where: { $0.id == view.flow }) {
                LabeledContent("Pipeline", value: flow.name)
            }
            Picker("Shows", selection: Binding(
                get: { view.flow },
                set: { flow in Task { await model.updateList(view, body: ["flow": .string(flow)]) } })
            ) {
                Text("Contacts added to it").tag("")
                ForEach(model.data?.flows ?? []) { Text("Pipeline: \($0.name)").tag($0.id) }
            }
            .disabled(!view.canManage)
            HStack {
                Button("Move left", systemImage: "arrow.left") { Task { await model.moveList(view, by: -1) } }
                Spacer()
                Button("Move right", systemImage: "arrow.right") { Task { await model.moveList(view, by: 1) } }
            }
            .buttonStyle(.borderless)
            if let url = CrmPrompt.listLink(organization: model.organization, viewId: view.id) {
                ShareLink(item: url) { Label("Share link to this list", systemImage: "link") }
            }
        } header: {
            Text("List")
        } footer: {
            Text(listFooter(view))
        }
        Section {
            if canShare {
                Toggle("Only me and the people below", isOn: $isPrivate)
                shareFields
                Button("Save sharing", systemImage: "person.2.badge.gearshape") { Task { await share() } }
            } else {
                Text("Shared with \(view.sharedWith.isEmpty ? "the organization" : view.sharedWith.joined(separator: ", ")).")
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("Sharing")
        }
        Section {
            ForEach($columns) { $column in
                HStack {
                    Toggle(isOn: Binding(get: { !column.hidden }, set: { column.hidden = !$0 })) {
                        VStack(alignment: .leading, spacing: 1) {
                            Text(column.label)
                            if column.custom { Text("Custom").font(.caption2).foregroundStyle(.secondary) }
                        }
                    }
                }
                .swipeActions {
                    if column.custom || CrmColumnCatalog.deletable.contains(column.field) {
                        Button(role: .destructive) {
                            pendingColumn = column
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
                    }
                }
            }
            .onMove { columns.move(fromOffsets: $0, toOffset: $1) }
            Button("Save columns", systemImage: "checkmark") {
                Task { if await model.setColumns(view, columns: columns) { FmHaptics.commit() } }
            }
        } header: {
            HStack {
                Text("Columns")
                Spacer()
                EditButton().font(.caption)
            }
        } footer: {
            Text("Visible columns show on each row and in the CSV export. Drag to reorder.")
        }
        if let removed = model.data?.settings.removedColumns, !removed.isEmpty {
            Section {
                ForEach(removed, id: \.self) { field in
                    HStack {
                        Text(CrmColumnCatalog.label(field))
                        Spacer()
                        if model.canManage {
                            Button("Restore") {
                                Task {
                                    if await model.restoreColumn(field),
                                        let entry = CrmColumnCatalog.builtIn.first(where: { $0.field == field })
                                    {
                                        columns.append(CrmColumn(field: entry.field, label: entry.label, custom: false, hidden: false))
                                    }
                                }
                            }
                            .buttonStyle(.borderless)
                        }
                    }
                }
            } header: {
                Text("Deleted columns")
            } footer: {
                Text("Restoring brings the column back for every list; values erased when it was deleted stay empty.")
            }
        }
        if let others = model.data?.views.filter({ $0.id != view.id }), !others.isEmpty {
            Section("Merge") {
                Menu {
                    ForEach(others) { other in
                        Button(other.name) {
                            Task {
                                if await model.mergeList(view, into: other) {
                                    FmHaptics.commit()
                                    dismiss()
                                }
                            }
                        }
                    }
                } label: {
                    Label("Merge into another list", systemImage: "arrow.triangle.merge")
                }
            }
        }
        Section("History") {
            if history.isEmpty { Text("No changes recorded yet.").foregroundStyle(.secondary) }
            ForEach(history) { entry in
                VStack(alignment: .leading, spacing: 2) {
                    Text(entry.text.isEmpty ? CrmStageLabel.title(entry.action.replacingOccurrences(of: ".", with: "-")) : entry.text)
                        .font(.subheadline)
                    Text("\(entry.actor) · \(PackagesFormat.relative(entry.at))").font(.caption).foregroundStyle(.secondary)
                }
            }
            if more, let last = history.last {
                Button("Show older") {
                    Task {
                        let page = await model.listActivity(view, before: last.at)
                        history += page.entries
                        more = page.more
                    }
                }
            }
        }
        if view.canManage {
            Section {
                Button("Delete list", systemImage: "trash", role: .destructive) { confirmDelete = true }
            }
        }
    }

    private func listFooter(_ view: CrmView) -> String {
        let owner = view.owner.isEmpty ? "unknown" : view.owner
        let source = view.fileName.isEmpty ? "" : " · imported from " + view.fileName
        return "Owner: " + owner + source
    }

    @ViewBuilder private var shareFields: some View {
        TextField("ForkMesh usernames, comma separated", text: $shareText)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
        if let members = model.data?.members, !members.isEmpty {
            Menu {
                ForEach(members, id: \.self) { member in
                    Button(member) { appendShare(member) }
                }
            } label: {
                Label("Add a teammate", systemImage: "person.badge.plus")
            }
        }
        Picker("They can", selection: $access) {
            Text("View").tag("view")
            Text("Edit").tag("edit")
        }
        .pickerStyle(.segmented)
    }

    private func appendShare(_ name: String) {
        var names = shareText.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        if !names.contains(name) { names.append(name) }
        shareText = names.joined(separator: ", ")
    }

    private func share() async {
        let names = shareText.split(separator: ",").map { String($0) }
        if await model.share(item, with: names, access: access, visibility: isPrivate ? "private" : "organization") {
            FmHaptics.commit()
            model.notice = "Sharing saved."
        }
    }
}

struct CrmSettingsSheet: View {
    @Bindable var model: CrmModel
    @State private var edit: CrmSettingsEdit
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    init(model: CrmModel) {
        self.model = model
        _edit = State(initialValue: CrmSettingsEdit(settings: model.data?.settings ?? CrmSettings(json: [:])))
    }

    var body: some View {
        Form {
            Section {
                TextField("From name", text: $edit.fromName)
                TextField("From email", text: $edit.fromEmail)
                    .keyboardType(.emailAddress)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            } header: {
                Text("Sender")
            } footer: {
                Text(model.data?.settings.mailtrapConfigured == true
                    ? "Emails sent from the CRM go out from this address."
                    : "This server has no mail sender configured, so the CRM opens your mailbox instead.")
            }
            Section("Mailbox") {
                Picker("Email links open", selection: $edit.mailProvider) {
                    Text("Zoho Mail").tag("zoho")
                    Text("Gmail").tag("gmail")
                }
                TextField("Mail boxes, comma separated", text: $edit.mailBoxes)
                zoho
            }
            Section {
                TextField("Provider name", text: $edit.providerName)
                TextField("Message link template (https://…{id})", text: $edit.providerUrlTemplate)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            } header: {
                Text("Sent-mail links")
            } footer: {
                Text("Links each logged email back to the message in your provider.")
            }
            if let settings = model.data?.settings, settings.updatedAt > 0 {
                Section {
                    Text("Last changed \(PackagesFormat.relative(settings.updatedAt)) by \(settings.updatedBy)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .disabled(!model.canManage)
        .navigationTitle("CRM settings")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    Task {
                        if await model.saveSettings(edit) {
                            FmHaptics.commit()
                            dismiss()
                        }
                    }
                }
                .disabled(!model.canManage || model.saving)
            }
        }
    }

    @ViewBuilder private var zoho: some View {
        let status = model.data?.settings.zoho
        if status?.connected == true {
            LabeledContent("Zoho Mail", value: "Connected (\(status?.region ?? "com"))")
            LabeledContent("Sending", value: status?.canSend == true ? "Allowed" : "Read only")
            if status?.canSend != true && status?.oneClick == true && model.canManage {
                Button("Reconnect Zoho Mail", systemImage: "arrow.triangle.2.circlepath") {
                    Task { if let url = await model.connectZoho() { openURL(url) } }
                }
            }
        } else if status?.oneClick == true {
            Button("Connect Zoho Mail", systemImage: "link.badge.plus") {
                Task { if let url = await model.connectZoho() { openURL(url) } }
            }
        } else {
            LabeledContent("Zoho Mail", value: "Not connected")
        }
        if status?.connected != true && model.canManage {
            NavigationLink("Connect with a Self Client code") { CrmZohoCodeSheet(model: model) }
        }
    }
}

struct CrmZohoCodeSheet: View {
    @Bindable var model: CrmModel
    @State private var code: CrmZohoCode
    @Environment(\.dismiss) private var dismiss

    init(model: CrmModel) {
        self.model = model
        _code = State(initialValue: CrmZohoCode(status: model.data?.settings.zoho))
    }

    var body: some View {
        Form {
            Section {
                Link("Open the Zoho API Console", destination: CrmZohoCode.consoleURL)
                Text("Add Client → Self Client → Create. Copy its client ID and secret. On Generate Code enter the scope below, pick 10 minutes and copy the code.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(CrmZohoCode.scopes).font(.caption.monospaced()).textSelection(.enabled)
            } header: {
                Text("This organization only")
            }
            Section {
                TextField("Client ID", text: $code.clientId).textInputAutocapitalization(.never).autocorrectionDisabled()
                SecureField(code.usesSavedSecret ? "Saved secret (leave blank to keep)" : "Client secret", text: $code.clientSecret)
                TextField("Code", text: $code.code).textInputAutocapitalization(.never).autocorrectionDisabled()
                Picker("Data center", selection: $code.region) {
                    ForEach(CrmZohoCode.regions, id: \.self) { Text($0).tag($0) }
                }
            } footer: {
                Text("ForkMesh redeems the code within its 10 minutes, tries every Zoho data center and reads the mail account. The secret and refresh token are stored encrypted.")
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("Zoho Self Client")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Connect") {
                    Task {
                        if await model.exchangeZoho(code) {
                            FmHaptics.commit()
                            dismiss()
                        }
                    }
                }
                .disabled(!code.isComplete || model.saving)
            }
        }
    }
}

struct CrmImportSheet: View {
    @Bindable var model: CrmModel
    let csv: String
    let fileName: String
    @State private var preview: CrmImportPreview?
    @State private var sheetName: String
    @State private var target = ""
    @State private var progress: (Int, Int)?
    @State private var result: CrmImportResult?
    @Environment(\.dismiss) private var dismiss

    init(model: CrmModel, csv: String, fileName: String) {
        self.model = model
        self.csv = csv
        self.fileName = fileName
        let base = (fileName as NSString).deletingPathExtension
        _sheetName = State(initialValue: base.isEmpty ? "Imported contacts" : base)
    }

    var body: some View {
        Form {
            if let result {
                Section {
                    Label(result.sentence, systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    ForEach(result.errors.prefix(20), id: \.self) { Text($0).font(.caption).foregroundStyle(.secondary) }
                }
            } else {
                Section {
                    LabeledContent("File", value: fileName)
                    LabeledContent("Rows", value: "\(CrmCSV.split(csv).rows)")
                    Picker("Into", selection: $target) {
                        Text("A new list").tag("")
                        ForEach(model.data?.views ?? []) { Text($0.name).tag($0.id) }
                    }
                    if target.isEmpty { TextField("List name", text: $sheetName) }
                }
                if let preview {
                    Section {
                        ForEach(Array(preview.columns.enumerated()), id: \.offset) { index, column in
                            VStack(alignment: .leading, spacing: 4) {
                                Picker(column.header.isEmpty ? "Column \(index + 1)" : column.header, selection: Binding(
                                    get: { self.preview?.columns[index].field ?? "" },
                                    set: { self.preview?.columns[index].field = $0 })
                                ) {
                                    ForEach(CrmColumnCatalog.importTargets, id: \.field) { Text($0.label).tag($0.field) }
                                }
                                if !column.samples.isEmpty {
                                    Text(column.samples.joined(separator: " · ")).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                                }
                            }
                        }
                    } header: {
                        Text("Columns")
                    } footer: {
                        Text("Email is how the CRM spots people it already has; rows without one are always added as new contacts.")
                    }
                } else if model.errorText.isEmpty {
                    Section { ProgressView("Reading the file…").frame(maxWidth: .infinity) }
                }
                if let progress {
                    Section {
                        ProgressView(value: Double(progress.0), total: Double(max(1, progress.1))) {
                            Text("Importing part \(min(progress.0 + 1, progress.1)) of \(progress.1)")
                        }
                    }
                }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("Import CSV")
        .navigationBarTitleDisplayMode(.inline)
        .interactiveDismissDisabled(progress != nil && result == nil)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button(result == nil ? "Cancel" : "Close") { dismiss() }.disabled(progress != nil && result == nil)
            }
            ToolbarItem(placement: .confirmationAction) {
                if result == nil {
                    Button("Import") { Task { await run() } }
                        .disabled(preview == nil || model.saving)
                }
            }
        }
        .task {
            model.clearError()
            preview = await model.previewImport(csv)
        }
    }

    private func run() async {
        guard let preview else { return }
        progress = (0, 1)
        let done = await model.importCSV(
            csv, fileName: fileName, sheetName: sheetName, viewId: target, mapping: preview.columns.map(\.field)
        ) { part, total in
            progress = (part, total)
        }
        if let done {
            FmHaptics.commit()
            result = done
        } else {
            progress = nil
            FmHaptics.failure()
        }
    }
}

struct CrmExportSheet: View {
    let url: URL
    let rows: Int
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 18) {
            Image(systemName: "tablecells.badge.ellipsis")
                .font(.system(size: 44))
                .foregroundStyle(Color.accentColor)
            Text("\(rows) contacts exported").font(.title3.bold())
            Text(url.lastPathComponent).font(.footnote).foregroundStyle(.secondary)
            ShareLink(item: url) {
                Label("Share CSV", systemImage: "square.and.arrow.up")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
        }
        .padding(24)
        .navigationTitle("Export")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
        }
        .presentationDetents([.medium])
    }
}

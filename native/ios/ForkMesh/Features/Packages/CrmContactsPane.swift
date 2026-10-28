import FMApi
import FMPackages
import SwiftUI

struct CrmFilterMenu: View {
    @Bindable var model: CrmModel

    var body: some View {
        Menu("Sort", systemImage: "arrow.up.arrow.down") {
            Picker("Sort", selection: $model.filter.sort) {
                ForEach(CrmSort.allCases) { Text($0.title).tag($0) }
            }
        }
        Menu("Filter", systemImage: "line.3.horizontal.decrease.circle") {
            Picker("Owner", selection: $model.filter.owner) {
                Text("Anyone").tag("")
                ForEach(model.data?.owners ?? [], id: \.self) { Text($0).tag($0) }
            }
            .pickerStyle(.menu)
            Picker("Tag", selection: $model.filter.tag) {
                Text("Any tag").tag("")
                ForEach(model.data?.tags ?? [], id: \.self) { Text($0).tag($0) }
            }
            .pickerStyle(.menu)
            if let flow = model.activeFlow {
                Picker("Stage", selection: $model.filter.stage) {
                    Text("Every stage").tag("")
                    ForEach(flow.stages, id: \.self) { Text(CrmStageLabel.title($0)).tag($0) }
                }
                .pickerStyle(.menu)
            }
            if model.filter.isActive {
                Button("Clear filters", systemImage: "xmark.circle") { clear() }
            }
        }
    }

    private func clear() {
        model.filter.owner = ""
        model.filter.tag = ""
        model.filter.stage = ""
    }
}

struct CrmFilterChips: View {
    @Bindable var model: CrmModel

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 6) {
                if !model.filter.stage.isEmpty {
                    chip("Stage: " + CrmStageLabel.title(model.filter.stage)) { model.filter.stage = "" }
                }
                if !model.filter.owner.isEmpty {
                    chip("Owner: " + model.filter.owner) { model.filter.owner = "" }
                }
                if !model.filter.tag.isEmpty {
                    chip("#" + model.filter.tag) { model.filter.tag = "" }
                }
            }
        }
    }

    private func chip(_ text: String, clear: @escaping () -> Void) -> some View {
        Button {
            FmHaptics.selection()
            clear()
        } label: {
            CrmChip(text: text, icon: "xmark", tint: .accentColor)
        }
        .buttonStyle(.plain)
    }
}

struct CrmStageStrip: View {
    @Bindable var model: CrmModel
    let flow: CrmFlow
    let contacts: [CrmContact]

    var body: some View {
        let stats = flow.stats(contacts)
        let total = max(1, contacts.count)
        VStack(alignment: .leading, spacing: 8) {
            GeometryReader { proxy in
                HStack(spacing: 2) {
                    ForEach(stats) { stat in
                        Rectangle()
                            .fill(CrmPalette.stage(stat.stage, in: flow))
                            .frame(width: max(stat.count > 0 ? 4 : 0, proxy.size.width * CGFloat(stat.count) / CGFloat(total)))
                    }
                    Spacer(minLength: 0)
                }
            }
            .frame(height: 6)
            .clipShape(Capsule())
            .background(Color.secondary.opacity(0.12), in: Capsule())
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(stats) { stat in pill(stat) }
                }
            }
        }
        .padding(.vertical, 4)
    }

    private func pill(_ stat: CrmStageStat) -> some View {
        let active = model.filter.stage == stat.stage
        let tint = CrmPalette.stage(stat.stage, in: flow)
        return Button {
            FmHaptics.selection()
            model.filter.stage = active ? "" : stat.stage
        } label: {
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    Circle().fill(tint).frame(width: 7, height: 7)
                    Text(CrmStageLabel.title(stat.stage)).font(.caption.weight(.medium)).lineLimit(1)
                }
                HStack(spacing: 4) {
                    Text("\(stat.count)").font(.subheadline.weight(.bold).monospacedDigit())
                    if stat.value > 0 {
                        Text(CrmText.money(stat.value)).font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                    }
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(active ? tint.opacity(0.22) : Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: 10, style: .continuous).strokeBorder(active ? tint : Color.clear, lineWidth: 1.5)
            )
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(CrmStageLabel.title(stat.stage)), \(stat.count) contacts")
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

struct CrmContactRow: View {
    let contact: CrmContact
    var flow: CrmFlow?
    var showStage = true
    var columns: [CrmColumn] = []
    var dense: Bool?

    var body: some View {
        let compact = dense ?? CrmLayout.dense
        HStack(alignment: .center, spacing: compact ? 9 : 12) {
            CrmAvatar(name: contact.name, email: contact.email, size: compact ? 32 : 42)
            VStack(alignment: .leading, spacing: compact ? 1 : 3) {
                HStack(spacing: 6) {
                    Text(contact.displayName).font((compact ? Font.subheadline : Font.body).weight(.semibold)).lineLimit(1)
                    Spacer(minLength: 4)
                    if showStage { CrmStageBadge(stage: contact.stage, flow: flow) }
                }
                HStack(spacing: 6) {
                    Text(contact.subtitle.isEmpty ? contact.email : contact.subtitle)
                        .font(.caption).foregroundStyle(.secondary).lineLimit(1)
                    Spacer(minLength: 4)
                    if contact.value > 0 {
                        Text(CrmText.money(contact.value))
                            .font(.caption.weight(.semibold).monospacedDigit())
                            .foregroundStyle(.green)
                    }
                }
                if compact {
                    denseFooter
                } else if !contact.nextStep.isEmpty {
                    Label(contact.nextStep, systemImage: "arrow.turn.down.right")
                        .font(.caption)
                        .foregroundStyle(Color.primary.opacity(0.8))
                        .lineLimit(1)
                }
                ForEach(columns.prefix(compact ? 1 : 2)) { column in
                    let value = contact.value(of: column.field)
                    if !value.isEmpty {
                        Text("\(column.label): \(value)").font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                    }
                }
            }
        }
        .padding(.vertical, compact ? 0 : 3)
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder private var denseFooter: some View {
        let tags = contact.tags.prefix(2)
        if !contact.nextStep.isEmpty || !tags.isEmpty || contact.lastContactedAt > 0 {
            HStack(spacing: 4) {
                if !contact.nextStep.isEmpty {
                    Label(contact.nextStep, systemImage: "arrow.turn.down.right").labelStyle(.titleAndIcon).lineLimit(1)
                } else {
                    ForEach(Array(tags), id: \.self) { CrmChip(text: "#" + $0, tint: .purple) }
                }
                Spacer(minLength: 0)
                if contact.lastContactedAt > 0 {
                    Text(PackagesFormat.relative(contact.lastContactedAt)).foregroundStyle(.tertiary).lineLimit(1)
                }
            }
            .font(.caption2)
            .foregroundStyle(Color.primary.opacity(0.75))
        }
    }
}

struct CrmContactActions: View {
    let model: CrmModel
    let data: CrmOrganizationData
    let contact: CrmContact
    let present: (CrmSheet) -> Void
    let onDelete: (CrmContact) -> Void
    @Environment(\.openURL) private var openURL

    private var flow: CrmFlow { data.flow(contact.flow) }

    var body: some View {
        if !contact.email.isEmpty {
            Button("Email", systemImage: "envelope") { present(.compose(CrmEmailDraft(contact: contact), outreach: false)) }
        }
        if !contact.phone.isEmpty, let url = URL(string: "tel:" + contact.phone.filter { !$0.isWhitespace }) {
            Button("Call", systemImage: "phone") { openURL(url) }
        }
        Menu("Move to stage", systemImage: "arrow.right.circle") {
            ForEach(flow.stages.filter { $0 != contact.stage }, id: \.self) { stage in
                Button(CrmStageLabel.title(stage)) { run(.stage(stage)) }
            }
        }
        Menu("Move to pipeline", systemImage: "point.3.connected.trianglepath.dotted") {
            ForEach(data.flows.filter { $0.id != contact.flow }) { target in
                Button(target.name, systemImage: CrmFlowIcon.symbol(target.icon)) { run(.flow(target.id)) }
            }
        }
        if !data.listViews.isEmpty {
            Menu("Lists", systemImage: "list.bullet") {
                ForEach(data.listViews) { view in
                    if contact.views.contains(view.id) {
                        Button("Remove from \(view.name)", systemImage: "minus.circle") { run(.removeList(view.id)) }
                    } else {
                        Button("Add to \(view.name)", systemImage: "plus.circle") { run(.addList(view.id)) }
                    }
                }
            }
        }
        Button("Mark contacted now", systemImage: "checkmark.message") {
            Task { if await model.markContacted([contact.id]) { FmHaptics.commit() } }
        }
        Button("Edit", systemImage: "pencil") { present(.edit(contact)) }
        Button("Deck link", systemImage: "rectangle.on.rectangle.angled") {
            Task {
                if let link = await model.deckLink(for: contact) {
                    CrmClipboard.copy(link.url)
                    model.notice = "Deck link copied."
                }
            }
        }
        Button("Copy prompt", systemImage: "sparkles") {
            CrmClipboard.copy(CrmPrompt.outreach(contact, sheet: model.activeTitle, flowName: flow.name))
            model.notice = "Copied a prompt about \(contact.displayName)."
        }
        Button("Merge into…", systemImage: "arrow.triangle.merge") { present(.merge(contact)) }
        Divider()
        Button("Delete", systemImage: "trash", role: .destructive) { onDelete(contact) }
    }

    private func run(_ action: CrmBulkAction) {
        Task {
            if await model.bulk(action, ids: [contact.id]) { FmHaptics.selection() } else { FmHaptics.failure() }
        }
    }
}

struct CrmContactList: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    @Binding var selecting: Bool
    @Binding var selection: Set<String>
    let present: (CrmSheet) -> Void
    @State private var pendingDelete: [String] = []

    var body: some View {
        let flow = model.activeFlow
        let rows = model.visibleContacts
        let columns = model.activeView?.summaryColumns ?? []
        List(selection: selecting ? $selection : nil) {
            if let flow, !selecting {
                Section { CrmStageStrip(model: model, flow: flow, contacts: model.tabContacts) }
                    .listRowInsets(EdgeInsets(top: 2, leading: 12, bottom: 2, trailing: 12))
            }
            if model.filter.isActive {
                CrmFilterChips(model: model).listRowBackground(Color.clear)
            }
            if rows.isEmpty {
                CrmEmptyRow(text: model.query.isEmpty && !model.filter.isActive ? "No contacts here yet." : "No contacts match.", icon: "person.crop.circle.badge.questionmark")
                    .listRowBackground(Color.clear)
            } else if let flow, model.filter.sort == .stage, model.filter.stage.isEmpty {
                ForEach(flow.stats(rows)) { stat in
                    let group = rows.filter { stat.stage == "other" ? !flow.stages.contains($0.stage) : $0.stage == stat.stage }
                    if !group.isEmpty {
                        Section {
                            ForEach(group) { contact in row(contact, flow: flow, columns: columns, showStage: stat.stage == "other") }
                        } header: {
                            stageHeader(stat, flow: flow)
                        }
                    }
                }
            } else {
                Section {
                    ForEach(rows) { contact in row(contact, flow: flow ?? data.flow(contact.flow), columns: columns, showStage: true) }
                } header: {
                    Text("\(rows.count) contacts")
                }
            }
        }
        .listStyle(.insetGrouped)
        .listSectionSpacing(.compact)
        .environment(\.defaultMinListRowHeight, CrmLayout.dense ? 40 : 52)
        .environment(\.editMode, .constant(selecting ? .active : .inactive))
        .safeAreaInset(edge: .bottom) {
            if selecting {
                CrmBulkBar(
                    model: model, data: data, ids: Array(selection), all: rows.map(\.id), selection: $selection,
                    present: present, onDelete: { pendingDelete = $0 })
            }
        }
        .fmRefreshable { await model.load() }
        .confirmationDialog(
            deleteTitle,
            isPresented: Binding(get: { !pendingDelete.isEmpty }, set: { if !$0 { pendingDelete = [] } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                let ids = pendingDelete
                pendingDelete = []
                Task {
                    if await model.delete(ids: ids) > 0 { FmHaptics.commit(.delete) }
                    selection.subtract(ids)
                }
            }
        } message: {
            Text("Contacts and their pipeline history are removed for everyone in \(model.organization).")
        }
    }

    private var deleteTitle: String {
        guard pendingDelete.count == 1 else { return "Delete \(pendingDelete.count) contacts?" }
        let name = model.contact(pendingDelete[0])?.displayName ?? "this contact"
        return "Delete " + name + "?"
    }

    private func stageHeader(_ stat: CrmStageStat, flow: CrmFlow) -> some View {
        HStack(spacing: 6) {
            Circle().fill(CrmPalette.stage(stat.stage, in: flow)).frame(width: 8, height: 8)
            Text(CrmStageLabel.title(stat.stage))
            Text("\(stat.count)").monospacedDigit()
            Spacer()
            if stat.value > 0 { Text(CrmText.money(stat.value)).monospacedDigit() }
        }
    }

    private func row(_ contact: CrmContact, flow: CrmFlow?, columns: [CrmColumn], showStage: Bool) -> some View {
        NavigationLink(value: Route.contact(id: contact.routeId)) {
            CrmContactRow(contact: contact, flow: flow, showStage: showStage, columns: columns)
        }
        .contextMenu {
            CrmContactActions(model: model, data: data, contact: contact, present: present, onDelete: { pendingDelete = [$0.id] })
        }
        .swipeActions(edge: .leading, allowsFullSwipe: true) {
            if let next = flow?.next(after: contact.stage) {
                Button {
                    Task { if await model.move(contact, toStage: next) { FmHaptics.selection() } }
                } label: {
                    Label(CrmStageLabel.title(next), systemImage: "arrow.right")
                }
                .tint(CrmPalette.stage(next, in: flow))
            }
            Button {
                Task { if await model.markContacted([contact.id]) { FmHaptics.commit() } }
            } label: {
                Label("Contacted", systemImage: "checkmark.message")
            }
            .tint(.green)
        }
        .swipeActions(edge: .trailing) {
            Button(role: .destructive) {
                pendingDelete = [contact.id]
            } label: {
                Label("Delete", systemImage: "trash")
            }
            if !contact.email.isEmpty {
                Button {
                    present(.compose(CrmEmailDraft(contact: contact), outreach: false))
                } label: {
                    Label("Email", systemImage: "envelope")
                }
                .tint(.blue)
            }
        }
    }
}

struct CrmBulkBar: View {
    let model: CrmModel
    let data: CrmOrganizationData
    let ids: [String]
    let all: [String]
    @Binding var selection: Set<String>
    let present: (CrmSheet) -> Void
    let onDelete: ([String]) -> Void

    private var flow: CrmFlow? { model.activeFlow ?? ids.first.flatMap { model.contact($0) }.map { data.flow($0.flow) } }

    var body: some View {
        VStack(spacing: 8) {
            HStack {
                Text(ids.isEmpty ? "Select contacts" : "\(ids.count) selected").font(.subheadline.weight(.semibold))
                Spacer()
                Button(selection.count == all.count ? "Select none" : "Select all") {
                    FmHaptics.selection()
                    selection = selection.count == all.count ? [] : Set(all)
                }
                .font(.subheadline)
            }
            HStack(spacing: 8) {
                Menu {
                    ForEach(flow?.stages ?? [], id: \.self) { stage in
                        Button(CrmStageLabel.title(stage)) { run { await model.bulk(.stage(stage), ids: ids) } }
                    }
                } label: { tile("Stage", "arrow.right.circle") }
                Menu {
                    ForEach(data.views.filter(\.isPipeline)) { view in
                        Button(view.name, systemImage: "list.bullet.indent") { run { await model.moveToPipelineList(ids, view: view) } }
                    }
                    Divider()
                    ForEach(data.flows) { target in
                        Button(target.name, systemImage: CrmFlowIcon.symbol(target.icon)) { run { await model.bulk(.flow(target.id), ids: ids) } }
                    }
                } label: { tile("Pipeline", "point.3.connected.trianglepath.dotted") }
                Menu {
                    ForEach(data.listViews) { view in
                        Menu(view.name) {
                            Button("Add to list", systemImage: "plus.circle") { run { await model.bulk(.addList(view.id), ids: ids) } }
                            Button("Move to this list only", systemImage: "arrow.right.square") { run { await model.setOnlyList(ids, viewId: view.id) } }
                            Button("Remove from list", systemImage: "minus.circle") { run { await model.bulk(.removeList(view.id), ids: ids) } }
                        }
                    }
                } label: { tile("Lists", "list.bullet") }
                Menu {
                    Button("Mark contacted now", systemImage: "checkmark.message") { run { await model.markContacted(ids) } }
                    Button("Set owner", systemImage: "person.crop.circle") { present(.bulkOwner(ids)) }
                    if ids.count == 1, let contact = model.contact(ids[0]) {
                        Button("Merge into…", systemImage: "arrow.triangle.merge") { present(.merge(contact)) }
                    }
                    Divider()
                    Button("Delete", systemImage: "trash", role: .destructive) { onDelete(ids) }
                } label: { tile("More", "ellipsis.circle") }
            }
            .disabled(ids.isEmpty || model.saving)
        }
        .padding(12)
        .shellGlass(RoundedRectangle(cornerRadius: 18, style: .continuous))
        .padding(.horizontal, 12)
        .padding(.bottom, 6)
    }

    private func tile(_ title: String, _ icon: String) -> some View {
        VStack(spacing: 3) {
            Image(systemName: icon).font(.body.weight(.semibold))
            Text(title).font(.caption2.weight(.medium))
        }
        .frame(maxWidth: .infinity, minHeight: 46)
        .background(Color.accentColor.opacity(0.1), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    private func run(_ work: @escaping () async -> Bool) {
        Task {
            if await work() {
                FmHaptics.commit()
                selection.removeAll()
            } else {
                FmHaptics.failure()
            }
        }
    }
}

struct CrmBoard: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let flow: CrmFlow
    let present: (CrmSheet) -> Void
    @State private var targeted = ""
    @State private var pendingDelete: CrmContact?

    var body: some View {
        let rows = model.visibleContacts
        ScrollView(.vertical) {
            ScrollView(.horizontal, showsIndicators: false) {
                LazyHStack(alignment: .top, spacing: 10) {
                    ForEach(flow.stats(rows)) { stat in
                        column(stat, contacts: rows.filter { stat.stage == "other" ? !flow.stages.contains($0.stage) : $0.stage == stat.stage })
                    }
                }
                .padding(.horizontal)
                .padding(.vertical, 8)
            }
        }
        .background(fm.wallpaper ? Color.clear : Color(uiColor: .systemGroupedBackground))
        .fmRefreshable { await model.load() }
        .confirmationDialog(
            "Delete \(pendingDelete?.displayName ?? "this contact")?",
            isPresented: Binding(get: { pendingDelete != nil }, set: { if !$0 { pendingDelete = nil } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                guard let contact = pendingDelete else { return }
                pendingDelete = nil
                Task { if await model.delete(contact) { FmHaptics.commit(.delete) } }
            }
        }
    }

    private func column(_ stat: CrmStageStat, contacts: [CrmContact]) -> some View {
        let tint = CrmPalette.stage(stat.stage, in: flow)
        return VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Circle().fill(tint).frame(width: 9, height: 9)
                Text(CrmStageLabel.title(stat.stage)).font(.subheadline.weight(.semibold)).lineLimit(1)
                Text("\(stat.count)").font(.caption.weight(.bold).monospacedDigit()).foregroundStyle(.secondary)
                Spacer()
                if stat.value > 0 {
                    Text(CrmText.money(stat.value)).font(.caption.monospacedDigit()).foregroundStyle(.green)
                }
            }
            .padding(.horizontal, 4)
            if contacts.isEmpty {
                Text("Drop contacts here")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .frame(maxWidth: .infinity, minHeight: 60)
                    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(style: StrokeStyle(lineWidth: 1, dash: [4])).foregroundStyle(.tertiary))
            }
            ForEach(contacts) { contact in card(contact) }
        }
        .padding(8)
        .frame(width: 260, alignment: .top)
        .background(tint.opacity(targeted == stat.stage ? 0.2 : 0.07), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .dropDestination(for: String.self) { ids, _ in
            guard stat.stage != "other" else { return false }
            let moving = ids.filter { model.contact($0).map { $0.stage != stat.stage } ?? false }
            guard !moving.isEmpty else { return false }
            Task { if await model.bulk(.stage(stat.stage), ids: moving) { FmHaptics.commit() } }
            return true
        } isTargeted: { inside in
            targeted = inside ? stat.stage : (targeted == stat.stage ? "" : targeted)
        }
    }

    private func card(_ contact: CrmContact) -> some View {
        NavigationLink(value: Route.contact(id: contact.routeId)) {
            VStack(alignment: .leading, spacing: 5) {
                HStack(spacing: 8) {
                    CrmAvatar(name: contact.name, email: contact.email, size: 28)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(contact.displayName).font(.subheadline.weight(.semibold)).lineLimit(1)
                        if !contact.subtitle.isEmpty {
                            Text(contact.subtitle).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                        }
                    }
                    Spacer(minLength: 0)
                    if contact.value > 0 {
                        Text(CrmText.money(contact.value)).font(.caption.weight(.semibold).monospacedDigit()).foregroundStyle(.green)
                    }
                }
                if !contact.nextStep.isEmpty {
                    Label(contact.nextStep, systemImage: "arrow.turn.down.right").font(.caption2).lineLimit(2)
                }
                if !contact.tags.isEmpty || contact.lastContactedAt > 0 {
                    HStack(spacing: 4) {
                        ForEach(Array(contact.tags.prefix(2)), id: \.self) { CrmChip(text: "#" + $0, tint: .purple) }
                        Spacer(minLength: 0)
                        if contact.lastContactedAt > 0 {
                            Text(PackagesFormat.relative(contact.lastContactedAt)).font(.caption2).foregroundStyle(.tertiary)
                        }
                    }
                }
            }
            .foregroundStyle(.primary)
            .crmCard()
        }
        .buttonStyle(.plain)
        .draggable(contact.id) {
            CrmChip(text: contact.displayName, icon: "person.fill", tint: .accentColor, filled: true)
        }
        .contextMenu {
            CrmContactActions(model: model, data: data, contact: contact, present: present, onDelete: { pendingDelete = $0 })
        }
    }
}

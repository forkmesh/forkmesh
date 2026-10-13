import FMApi
import FMInsights
import SwiftUI
import UIKit

struct AdminUsersScreen: View {
    @Bindable var model: AdminScreenModel
    @State private var query = ""
    @State private var showQuarantined = false
    @State private var pendingAction: (action: AdminUserAction, name: String)?
    @State private var authFailed = false

    var body: some View {
        List {
            if !model.lastAction.isEmpty {
                InsightsBanner(text: model.lastAction, systemImage: "checkmark.circle", tint: .green)
            }
            if let error = model.actionError {
                InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle", tint: .red)
            }
            Section {
                Picker("List", selection: $showQuarantined) {
                    Text("Active").tag(false)
                    Text("Quarantined").tag(true)
                }
                .pickerStyle(.segmented)
            }
            switch model.users {
            case .idle, .loading:
                InsightsLoadingRows(rows: 8)
            case .failed(let error):
                InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle")
            case .loaded(let users):
                let rows = users.filtered(query, showQuarantined: showQuarantined)
                Section("\(rows.count) account\(rows.count == 1 ? "" : "s")") {
                    ForEach(rows.prefix(500)) { user in
                        AdminUserRow(user: user, busy: model.busy.contains(user.name))
                            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                                if !user.isAdmin { actions(for: user) }
                            }
                            .contextMenu { if !user.isAdmin { actions(for: user) } }
                    }
                }
            }
            if !showQuarantined, let pending = model.pending.value, !pending.isEmpty {
                Section("Awaiting email confirmation") {
                    ForEach(pending.prefix(100)) { item in
                        LabeledContent(item.name, value: InsightsTime.relative(item.joinedAt))
                    }
                }
            }
        }
        .searchable(text: $query, prompt: "Name or email")
        .navigationTitle("Users")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await model.reload() }
        .confirmationDialog(
            pendingAction.map { "\($0.action.title) \($0.name)?" } ?? "",
            isPresented: Binding(get: { pendingAction != nil }, set: { if !$0 { pendingAction = nil } }),
            titleVisibility: .visible
        ) {
            if let pending = pendingAction {
                Button(pending.action.title, role: pending.action == .unquarantine ? nil : .destructive) {
                    let target = pending
                    pendingAction = nil
                    Task { await run(target.action, target.name) }
                }
            }
        } message: {
            if let pending = pendingAction { Text(message(pending.action)) }
        }
        .alert("Face ID did not confirm it was you", isPresented: $authFailed) {
            Button("Copy") { UIPasteboard.general.string = "Face ID did not confirm it was you" }
            Button("Send to agent") { FmMessageHandoff.send("Face ID did not confirm it was you") }
            Button("OK", role: .cancel) {}
        }
    }

    @ViewBuilder private func actions(for user: AdminUser) -> some View {
        if user.quarantined {
            Button("Restore") { pendingAction = (.unquarantine, user.name) }.tint(.green)
        } else {
            Button("Quarantine") { pendingAction = (.quarantine, user.name) }.tint(.orange)
            Button("Delete", role: .destructive) { pendingAction = (.delete, user.name) }
        }
    }

    private func message(_ action: AdminUserAction) -> String {
        switch action {
        case .quarantine: return "Their sessions and desktops are signed out and the account leaves every list. You can restore it later."
        case .unquarantine: return "The account returns to the directory. Its owner signs in again and re-enables any desktop."
        case .delete: return "The account and everything it owns are erased. This cannot be undone."
        }
    }

    private func run(_ action: AdminUserAction, _ name: String) async {
        if action.needsDeviceAuth {
            let confirmed = await InsightsFeature.confirmOwner("\(action.title) the account \(name)")
            if !confirmed {
                authFailed = true
                return
            }
        }
        _ = await model.perform(action, target: name)
    }
}

struct AdminUserRow: View {
    let user: AdminUser
    let busy: Bool

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 6) {
                    Text(user.name).font(.body.weight(.semibold))
                    ForEach(user.badges, id: \.self) { badge in
                        Text(badge).font(.caption2.weight(.semibold))
                            .padding(.horizontal, 5).padding(.vertical, 1)
                            .background(Capsule().fill(Color.secondary.opacity(0.15)))
                    }
                }
                if !user.email.isEmpty {
                    Text(user.email).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Text(joinedText)
                    .font(.caption2).foregroundStyle(.secondary)
            }
            Spacer()
            if busy { ProgressView() }
        }
        .accessibilityElement(children: .combine)
    }

    private var joinedText: String {
        let joined: String = "Joined \(InsightsTime.relative(user.createdAt))"
        guard user.nodes > 0 else { return joined }
        let noun: String = user.nodes == 1 ? "node" : "nodes"
        return "\(joined) · \(user.nodes) \(noun)"
    }
}

struct AdminDatabaseScreen: View {
    @Bindable var model: AdminScreenModel
    @State private var query = ""

    var body: some View {
        List {
            switch model.inventory {
            case .idle, .loading:
                InsightsLoadingRows(rows: 8)
            case .failed(let error):
                InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle")
            case .loaded(let inventory):
                let tables = inventory.filtered(query)
                Section {
                    ForEach(tables) { table in
                        NavigationLink {
                            AdminTableRowsScreen(client: model.client, table: table)
                                .shellPageClearance()
                        } label: {
                            AdminTableLine(table: table)
                        }
                        .disabled(table.restricted)
                        .listRowInsets(EdgeInsets(top: 3, leading: 14, bottom: 3, trailing: 10))
                    }
                } header: {
                    Text("\(inventory.tables.count) tables · \(StatsFormat.compactNumber(Double(inventory.totalRows))) rows · read-only")
                        .font(.caption2)
                        .textCase(nil)
                }
            }
        }
        .listStyle(.plain)
        .environment(\.defaultMinListRowHeight, 30)
        .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always), prompt: "Table")
        .navigationTitle("Database")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await model.reload() }
    }
}

struct AdminTableLine: View {
    let table: DatabaseTable

    var body: some View {
        HStack(spacing: 6) {
            Text(table.name)
                .font(.footnote.monospaced())
                .lineLimit(1)
                .truncationMode(.middle)
            if table.restricted {
                Image(systemName: "lock").font(.caption2).foregroundStyle(.secondary)
                    .accessibilityLabel("Restricted to its own page")
            } else if !table.gaps.isEmpty {
                Image(systemName: "exclamationmark.triangle.fill").font(.caption2).foregroundStyle(.orange)
                    .accessibilityLabel(table.gaps.joined(separator: "; "))
            }
            Spacer(minLength: 4)
            Text(StatsFormat.compactNumber(Double(table.rows)))
                .font(.caption2.monospacedDigit())
                .foregroundStyle(.secondary)
        }
    }
}

struct AdminTableRowsScreen: View {
    let client: AdminClient
    let table: DatabaseTable
    @State private var columns: [String] = []
    @State private var rows: [DatabaseRow] = []
    @State private var cursor = ""
    @State private var done = false
    @State private var loading = false
    @State private var error: InsightsError?
    @ScaledMetric(relativeTo: .caption2) private var charWidth: CGFloat = 6.8

    var body: some View {
        let widths = DatabaseGrid.widths(columns: columns, rows: rows)
        let rowidWidth = DatabaseGrid.rowidWidth(rows)
        VStack(spacing: 0) {
            if let error {
                InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle")
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
            }
            ScrollView([.horizontal, .vertical]) {
                LazyVStack(alignment: .leading, spacing: 0, pinnedViews: .sectionHeaders) {
                    Section {
                        ForEach(rows) { row in
                            NavigationLink {
                                AdminRowDetail(row: row, table: table.name)
                                    .shellPageClearance()
                            } label: {
                                gridLine(rowid: row.rowid, values: row.cells.map(\.value), widths: widths,
                                         rowidWidth: rowidWidth, header: false)
                            }
                            .buttonStyle(.plain)
                            .onAppear { if row.id == rows.last?.id && !done { Task { await load() } } }
                            Divider()
                        }
                    } header: {
                        gridLine(rowid: "#", values: columns, widths: widths, rowidWidth: rowidWidth, header: true)
                            .background(.bar)
                    }
                }
                .padding(.bottom, 96)
            }
            .overlay {
                if done && rows.isEmpty {
                    Text("This table is empty.").font(.footnote).foregroundStyle(.secondary)
                } else if rows.isEmpty && loading {
                    ProgressView()
                }
            }
        }
        .navigationTitle(table.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Text(done ? "\(rows.count) rows" : "\(rows.count)+ rows")
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
        .task {
            if columns.isEmpty { columns = table.columns.map(\.name) }
            if rows.isEmpty { await load() }
        }
    }

    private func gridLine(rowid: String, values: [String], widths: [Int], rowidWidth: Int, header: Bool) -> some View {
        HStack(spacing: 0) {
            gridCell(rowid, chars: rowidWidth, header: header, dim: true)
            ForEach(Array(zip(values.indices, values)), id: \.0) { index, value in
                gridCell(value, chars: widths.indices.contains(index) ? widths[index] : DatabaseGrid.minChars,
                         header: header, dim: !header && value == "NULL")
            }
        }
        .contentShape(Rectangle())
    }

    private func gridCell(_ value: String, chars: Int, header: Bool, dim: Bool) -> some View {
        Text(DatabaseGrid.line(value))
            .font(header ? Font.caption2.monospaced().weight(.semibold) : Font.caption2.monospaced())
            .foregroundStyle(dim ? Color.secondary : Color.primary)
            .lineLimit(1)
            .truncationMode(.tail)
            .frame(width: CGFloat(chars) * charWidth + 10, alignment: .leading)
            .padding(.vertical, 5)
            .padding(.leading, 6)
    }

    private func load() async {
        if loading || done { return }
        loading = true
        defer { loading = false }
        do {
            let page = try await client.rows(table: table.name, cursor: cursor, limit: 50)
            if !page.columns.isEmpty { columns = page.columns }
            let known = Set(rows.map(\.id))
            rows.append(contentsOf: page.rows.filter { !known.contains($0.id) })
            cursor = page.nextCursor
            done = page.nextCursor.isEmpty || rows.count >= 1000
            error = nil
        } catch {
            self.error = InsightsError.from(error)
        }
    }
}

struct AdminRowDetail: View {
    let row: DatabaseRow
    let table: String

    var body: some View {
        List {
            Section {
                ForEach(row.cells) { cell in
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        Text(cell.column)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                            .frame(width: 104, alignment: .leading)
                        Text(cell.value)
                            .font(.caption.monospaced())
                            .foregroundStyle(cell.value == "NULL" ? Color.secondary : Color.primary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .textSelection(.enabled)
                    }
                    .listRowInsets(EdgeInsets(top: 4, leading: 14, bottom: 4, trailing: 10))
                }
            }
            if !row.data.isEmpty {
                Section("data (\(row.dataMode))") {
                    if row.walletKeys {
                        InsightsBanner(text: "Wallet keys in this record are redacted.", systemImage: "key")
                    }
                    Text(row.data).font(.caption2.monospaced()).textSelection(.enabled)
                }
            }
        }
        .listStyle(.plain)
        .environment(\.defaultMinListRowHeight, 26)
        .navigationTitle(row.rowid.isEmpty ? table : "\(table) #\(row.rowid)")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct AdminUsageScreen: View {
    @Bindable var model: AdminScreenModel

    var body: some View {
        List {
            Section("Scheduled jobs") {
                switch model.cron {
                case .idle, .loading:
                    InsightsLoadingRows(rows: 3)
                case .failed(let error):
                    InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle")
                case .loaded(let cron):
                    if !cron.unavailable.isEmpty {
                        InsightsBanner(text: "The job report is unavailable (\(cron.unavailable)).", systemImage: "exclamationmark.triangle")
                    }
                    HStack {
                        Image(systemName: cron.health.symbol).foregroundStyle(cron.health.color)
                        Text(cron.lastTickAt > 0 ? "Last run \(InsightsTime.relative(cron.lastTickAt))" : "No runs recorded")
                    }
                    LabeledContent("Runs in the window", value: "\(cron.ticks)")
                    LabeledContent("Runs with failures", value: "\(cron.failedTicks)")
                    ForEach(cron.recent.prefix(15)) { tick in
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Text(InsightsTime.date(tick.ts).formatted(date: .omitted, time: .standard)).font(.caption.monospacedDigit())
                                Spacer()
                                Text("\(tick.ms) ms").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                            }
                            if !tick.failed.isEmpty {
                                Text("Failed: \(tick.failed.joined(separator: ", "))").font(.caption2).foregroundStyle(.red)
                                    .fmMessageTools("Failed: \(tick.failed.joined(separator: ", "))")
                            }
                        }
                    }
                }
            }
            Section("Database storage") {
                switch model.storage {
                case .idle, .loading:
                    InsightsLoadingRows(rows: 2)
                case .failed(let error):
                    InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle")
                case .loaded(let storage):
                    if storage.entries.isEmpty { Text("The server reported no storage figures.").foregroundStyle(.secondary) }
                    ForEach(storage.entries) { entry in LabeledContent(entry.label, value: entry.value) }
                }
            }
            Section("Relays") {
                switch model.relays {
                case .idle, .loading:
                    InsightsLoadingRows(rows: 2)
                case .failed(let error):
                    InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle")
                case .loaded(let relays):
                    if relays.isEmpty { Text("No relays have registered.").foregroundStyle(.secondary) }
                    ForEach(relays) { relay in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(relay.label.isEmpty ? relay.baseUrl : relay.label)
                            Text("\(relay.status) · \(relay.baseUrl)").font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
        }
        .navigationTitle("Usage & jobs")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await model.reload() }
    }
}

struct AdminRepoPurgeScreen: View {
    @Bindable var model: AdminScreenModel
    @State private var owner = ""
    @State private var name = ""
    @State private var typed = ""
    @State private var authFailed = false

    private var valid: Bool { AdminClient.validSegment(owner) && AdminClient.validSegment(name) }
    private var confirmation: String { AdminClient.purgeConfirmation(owner: owner, name: name) }

    var body: some View {
        Form {
            Section {
                Text("Purging erases a repository from this server: its catalog row, every issue, pull request and mirror record, and a tombstone stops its owner's node from publishing it again.")
                    .font(.footnote)
            }
            Section("Repository") {
                TextField("Owner", text: $owner).textInputAutocapitalization(.never).autocorrectionDisabled()
                TextField("Name", text: $name).textInputAutocapitalization(.never).autocorrectionDisabled()
            }
            if valid {
                Section("Type \(confirmation) to confirm") {
                    TextField(confirmation, text: $typed).textInputAutocapitalization(.never).autocorrectionDisabled()
                    Button("Purge \(confirmation)", role: .destructive) { Task { await purge() } }
                        .disabled(typed != confirmation || model.busy.contains(confirmation))
                }
            }
            if !model.lastAction.isEmpty {
                InsightsBanner(text: model.lastAction, systemImage: "checkmark.circle", tint: .green)
            }
            if let error = model.actionError {
                InsightsBanner(text: error.message, systemImage: "exclamationmark.triangle", tint: .red)
            }
        }
        .navigationTitle("Repository purge")
        .navigationBarTitleDisplayMode(.inline)
        .alert("Face ID did not confirm it was you", isPresented: $authFailed) {
            Button("Copy") { UIPasteboard.general.string = "Face ID did not confirm it was you" }
            Button("Send to agent") { FmMessageHandoff.send("Face ID did not confirm it was you") }
            Button("OK", role: .cancel) {}
        }
    }

    private func purge() async {
        guard await InsightsFeature.confirmOwner("Purge \(confirmation)") else {
            authFailed = true
            return
        }
        if await model.purge(owner: owner, name: name) {
            typed = ""
        }
    }
}

import FMApi
import FMPackages
import SwiftUI

struct CompetitorsDestination: View {
    var body: some View {
        PackagesGate(feature: "Competitor Watch") { client in
            CompetitorsScreen(client: client)
        }
    }
}

struct CompetitorsScreen: View {
    private let client: PackagesClient
    @State private var model: OrgPackageModel<CompetitorBoard>
    @State private var query = ""
    @State private var editing: CompetitorEditTarget?
    @State private var deleting: Competitor?

    init(client: PackagesClient) {
        self.client = client
        _model = State(
            initialValue: OrgPackageModel<CompetitorBoard>(
                organizations: { try await client.competitorOrganizations() },
                load: { try await client.competitors($0) }))
    }

    var body: some View {
        List {
            Section {
                OrganizationPicker(organizations: model.organizations, selection: $model.organization) { await model.load() }
                PackagesErrorBanner(text: model.errorText) { await model.refresh() }
            }
            if let board = model.payload {
                if board.competitors.isEmpty {
                    ContentUnavailableView("No competitors yet", systemImage: "binoculars")
                }
                ForEach(board.grouped(query: query), id: \.status) { group in
                    Section(CrmStageLabel.title(group.status)) {
                        ForEach(group.items) { competitor in
                            Button {
                                editing = CompetitorEditTarget(competitor: competitor)
                            } label: {
                                CompetitorRow(competitor: competitor)
                            }
                            .buttonStyle(.plain)
                            .swipeActions(edge: .leading) {
                                Button("Reviewed") { Task { await markReviewed(competitor) } }.tint(.green)
                            }
                            .swipeActions(edge: .trailing) {
                                Button("Delete", role: .destructive) { deleting = competitor }
                            }
                        }
                    }
                }
            } else if !model.loading && model.installedOrganizations.isEmpty {
                PackageNotInstalledView(package: "Competitor Watch", organizations: model.organizations)
            }
        }
        .navigationTitle("Competitors")
        .searchable(text: $query)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    editing = CompetitorEditTarget(competitor: Competitor())
                } label: {
                    Label("Add competitor", systemImage: "plus")
                }
                .disabled(model.payload == nil)
            }
        }
        .overlay {
            if model.loading && model.payload == nil { ProgressView() }
        }
        .fmRefreshable { await model.load() }
        .task { await model.refresh() }
        .sheet(item: $editing) { target in
            NavigationStack {
                CompetitorEditor(competitor: target.competitor) { saved in
                    try await save(saved)
                }
            }
        }
        .confirmationDialog(
            "Delete \(deleting?.name ?? "")?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
            titleVisibility: .visible
        ) {
            Button("Delete", role: .destructive) {
                guard let target = deleting else { return }
                Task { await delete(target) }
            }
        } message: {
            Text("This removes it for everyone in \(model.organization).")
        }
    }

    private func save(_ competitor: Competitor) async throws {
        let saved = try await client.saveCompetitor(model.organization, competitor)
        model.update { board in
            board.competitors.removeAll { $0.id == saved.id }
            board.competitors.insert(saved, at: 0)
        }
    }

    private func markReviewed(_ competitor: Competitor) async {
        do {
            let saved = try await client.markCompetitorReviewed(
                model.organization, competitor, nowMs: Int64(Date().timeIntervalSince1970 * 1000))
            model.update { board in
                if let index = board.competitors.firstIndex(where: { $0.id == saved.id }) { board.competitors[index] = saved }
            }
        } catch {
            model.errorText = PackagesError.describe(error)
        }
    }

    private func delete(_ competitor: Competitor) async {
        do {
            try await client.deleteCompetitor(model.organization, id: competitor.id)
            model.update { $0.competitors.removeAll { $0.id == competitor.id } }
        } catch {
            model.errorText = PackagesError.describe(error)
        }
    }
}

struct CompetitorEditTarget: Identifiable {
    let competitor: Competitor
    var id: String { competitor.id.isEmpty ? "new" : competitor.id }
}

struct CompetitorRow: View {
    let competitor: Competitor

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(competitor.name).font(.body.weight(.medium))
                Spacer()
                if competitor.needsReview(nowMs: Int64(Date().timeIntervalSince1970 * 1000)) {
                    Label("Review due", systemImage: "clock.badge.exclamationmark").font(.caption).foregroundStyle(.orange)
                }
            }
            if !competitor.positioning.isEmpty {
                Text(competitor.positioning).font(.caption).foregroundStyle(.secondary).lineLimit(2)
            }
        }
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
    }
}

struct CompetitorEditor: View {
    @State private var competitor: Competitor
    @State private var saving = false
    @State private var errorText = ""
    let onSave: (Competitor) async throws -> Void
    @Environment(\.dismiss) private var dismiss

    init(competitor: Competitor, onSave: @escaping (Competitor) async throws -> Void) {
        _competitor = State(initialValue: competitor)
        self.onSave = onSave
    }

    var body: some View {
        Form {
            Section {
                TextField("Name", text: $competitor.name)
                TextField("Website (https://)", text: $competitor.website)
                    .keyboardType(.URL).textInputAutocapitalization(.never)
                if !competitor.websiteIsValid { Text("Use an https:// address.").font(.caption).foregroundStyle(.red).fmMessageTools("Use an https:// address.") }
                TextField("Category", text: $competitor.category)
                Picker("Status", selection: $competitor.status) {
                    ForEach(Competitor.statuses, id: \.self) { Text(CrmStageLabel.title($0)).tag($0) }
                }
            }
            Section("Positioning") { TextField("How they position themselves", text: $competitor.positioning, axis: .vertical) }
            Section("Strengths") { TextField("Strengths", text: $competitor.strengths, axis: .vertical).lineLimit(2...8) }
            Section("Weaknesses") { TextField("Weaknesses", text: $competitor.weaknesses, axis: .vertical).lineLimit(2...8) }
            Section("Notes") { TextField("Notes", text: $competitor.notes, axis: .vertical).lineLimit(2...10) }
            PackagesErrorBanner(text: errorText)
        }
        .navigationTitle(competitor.id.isEmpty ? "New competitor" : competitor.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    Task {
                        saving = true
                        defer { saving = false }
                        do {
                            try await onSave(competitor)
                            dismiss()
                        } catch {
                            errorText = PackagesError.describe(error)
                        }
                    }
                }
                .disabled(saving || competitor.name.trimmingCharacters(in: .whitespaces).isEmpty || !competitor.websiteIsValid)
            }
        }
    }
}

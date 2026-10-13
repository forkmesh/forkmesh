import FMApi
import FMBase
import FMCode
import SwiftUI

struct RepoSecurityScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoSecurityModel?

    init(repo: RepoRef) {
        if let client = CIServices.client {
            _model = State(initialValue: RepoSecurityModel(client: client, repo: repo))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                SecurityList(model: model)
                    .task { await model.load() }
                    .fmRefreshable { await model.load() }
            } else {
                FmEmptyState(icon: "shield_outlined", title: "Security is unavailable")
            }
        }
        .navigationTitle("Security")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct SecurityList: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoSecurityModel

    private func tint(_ severity: SecuritySeverity) -> Color {
        switch severity {
        case .critical, .high: return fm.danger
        case .medium: return fm.warning
        case .low, .info: return fm.textSecondary
        }
    }

    var body: some View {
        List {
            if let error = model.error {
                Text(error).foregroundStyle(fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(error)
            }
            if let scan = model.latest {
                Section("Latest scan") {
                    HStack {
                        Text(scan.headline).font(.headline)
                        Spacer()
                        if let worst = scan.worst {
                            FmStatusBadge(label: worst.label, color: tint(worst))
                        }
                    }
                    if !scan.scannedAt.isEmpty {
                        FmKeyValueRow(label: "Scanned", value: scan.scannedAt)
                    }
                    if !scan.shortCommit.isEmpty {
                        FmKeyValueRow(label: "Commit", value: scan.shortCommit)
                    }
                    if !scan.scanner.isEmpty {
                        FmKeyValueRow(label: "Scanner", value: scan.scanner)
                    }
                    if !scan.reviewStatus.isEmpty {
                        FmKeyValueRow(label: "Review", value: scan.reviewStatus)
                    }
                }
            } else if !model.loading {
                Text("No security scan has been published for this repository yet.").foregroundStyle(fm.textSecondary)
            }
            if let status = model.scanStatus {
                Section("Next scan") {
                    Text(status.label).foregroundStyle(fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(status.label)
                }
            }
            if let scan = model.latest, scan.hasDetail {
                findingsSection
            }
            if model.history.count > 1 {
                Section("History") {
                    ForEach(model.history) { (scan: SecurityScanSummary) in
                        HStack {
                            Text(scan.receivedAt.isEmpty ? scan.scannedAt : scan.receivedAt).font(.caption)
                            Spacer()
                            Text(scan.headline).font(.caption).foregroundStyle(fm.textSecondary)
                        }
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .overlay {
            if model.loading && model.latest == nil {
                ProgressView()
            }
        }
    }

    private var findingsSection: some View {
        Section {
            Picker("Severity", selection: Binding(get: { model.severityFilter }, set: { model.severityFilter = $0 })) {
                Text("All").tag(SecuritySeverity?.none)
                ForEach(SecuritySeverity.allCases, id: \.self) { (severity: SecuritySeverity) in
                    Text(severity.label).tag(SecuritySeverity?.some(severity))
                }
            }
            ForEach(model.findings) { (finding: SecurityFinding) in
                NavigationLink {
                    SecurityFindingScreen(model: model, findingId: finding.id)
                        .shellPageClearance()
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        HStack {
                            Text(finding.summary).lineLimit(2)
                            Spacer()
                            FmStatusBadge(label: finding.severity.label, color: tint(finding.severity))
                        }
                        Text(finding.location).font(.caption).monospaced().foregroundStyle(fm.textSecondary)
                        if finding.reviewStatus != "unreviewed" {
                            Text(finding.reviewStatus.capitalized).font(.caption2).foregroundStyle(fm.textTertiary)
                        }
                    }
                }
            }
        } header: {
            Text("Findings")
        }
    }
}

struct SecurityFindingScreen: View {
    @Environment(\.fmPalette) private var fm
    let model: RepoSecurityModel
    let findingId: String
    @State private var note: String = ""

    private var finding: SecurityFinding? { model.latest?.findings.first { $0.id == findingId } }

    var body: some View {
        List {
            if let finding = finding {
                Section {
                    Text(finding.summary).font(.headline)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools([finding.summary, finding.location].filter { !$0.isEmpty }.joined(separator: "\n"))
                    FmKeyValueRow(label: "Severity", value: finding.severity.label)
                    FmKeyValueRow(label: "Category", value: finding.category)
                    FmKeyValueRow(label: "Rule", value: finding.ruleId)
                    FmKeyValueRow(label: "Location", value: finding.location)
                    if !finding.advisoryIds.isEmpty {
                        FmKeyValueRow(label: "Advisories", value: finding.advisoryIds.joined(separator: ", "))
                    }
                }
                Section("Recommendation") {
                    Text(finding.recommendation)
                }
                Section("Review") {
                    TextField("Note (optional)", text: $note, axis: .vertical)
                    ForEach(RepoSecurityModel.triageStatuses, id: \.self) { (status: String) in
                        Button {
                            Task { await model.triage(finding, status: status, note: note) }
                        } label: {
                            HStack {
                                Text(status.capitalized)
                                Spacer()
                                if finding.reviewStatus == status {
                                    Image(systemName: "checkmark")
                                }
                            }
                        }
                        .disabled(model.busy)
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Finding")
        .navigationBarTitleDisplayMode(.inline)
    }
}

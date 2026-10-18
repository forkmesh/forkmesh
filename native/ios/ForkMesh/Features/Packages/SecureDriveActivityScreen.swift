import FMPackages
import SwiftUI
import UIKit

enum SecureDriveActivityFilter: String, CaseIterable, Identifiable {
    case all, files, opened, devices, problems

    var id: String { rawValue }

    var title: String {
        switch self {
        case .all: return "Everything"
        case .files: return "File changes"
        case .opened: return "Files opened"
        case .devices: return "Devices and invitations"
        case .problems: return "Problems"
        }
    }

    func matches(_ action: String) -> Bool {
        switch self {
        case .all: return true
        case .files: return action.hasPrefix("file_") && action != "file_opened"
        case .opened: return action == "file_opened"
        case .devices: return action.hasPrefix("device_") || action.hasPrefix("invite_") || action == "join_denied" || action.hasPrefix("drive_")
        case .problems: return action == "integrity_alert"
        }
    }
}

struct SecureDriveActivityView: View {
    let model: SecureDriveModel
    @State private var query = ""
    @State private var filter: SecureDriveActivityFilter = .all
    @State private var exported: URL?
    @State private var detail: SecureDriveRecordRow?

    private var shown: [SecureDriveRecordRow] {
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        return model.snapshot.records.filter { row in
            filter.matches(row.action)
                && (trimmed.isEmpty || row.text.localizedCaseInsensitiveContains(trimmed) || row.deviceName.localizedCaseInsensitiveContains(trimmed))
        }
    }

    var body: some View {
        List {
            Section {
                Picker("Show", selection: $filter) {
                    ForEach(SecureDriveActivityFilter.allCases) { (choice: SecureDriveActivityFilter) in
                        Text(choice.title).tag(choice)
                    }
                }
                Button {
                    Task { exported = await model.exportActivity() }
                } label: {
                    Label("Export as CSV\u{2026}", systemImage: "square.and.arrow.up")
                }
                .disabled(model.busy || model.snapshot.removed)
            } footer: {
                Text("Every entry is signed by the device that made it and checked on this iPhone.")
            }
            Section {
                if shown.isEmpty {
                    Text(model.snapshot.records.isEmpty ? "No activity yet." : "No activity matches.").foregroundStyle(.secondary)
                }
                ForEach(shown) { (row: SecureDriveRecordRow) in
                    Button {
                        detail = row
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(row.text)
                                .foregroundStyle(row.action == "integrity_alert" ? Color.red : Color.primary)
                            Text("\(row.deviceName) \u{00B7} \(Date(timeIntervalSince1970: Double(row.time) / 1000).formatted(date: .abbreviated, time: .standard))")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .searchable(text: $query, prompt: "Filter activity")
        .refreshable { await model.refresh(force: true) }
        .sheet(item: exportBinding) { (file: SecureDriveJoinFile) in
            NavigationStack {
                VStack(spacing: 16) {
                    Image(systemName: "tablecells").font(.largeTitle).foregroundStyle(.secondary)
                    Text(file.url.lastPathComponent).font(.headline)
                    Text("The exported activity log is not encrypted. Share it only with people who may see who opened and changed what. The copy on this iPhone is deleted when you lock.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                    ShareLink(item: file.url) {
                        Label("Share Activity Log", systemImage: "square.and.arrow.up")
                    }
                    .buttonStyle(.borderedProminent)
                    Spacer()
                }
                .padding(24)
                .navigationTitle("Export activity")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { exported = nil }
                    }
                }
            }
            .presentationDetents([.medium])
        }
        .alert("Activity record", isPresented: detailShown) {
            Button("Copy Hash") {
                if let row = detail { UIPasteboard.general.string = row.id }
            }
            Button("OK", role: .cancel) {}
        } message: {
            if let row = detail {
                Text("Record \(row.seq + 1) from \(row.deviceName), hash \(row.id)")
            }
        }
    }

    private var exportBinding: Binding<SecureDriveJoinFile?> {
        Binding(get: { exported.map(SecureDriveJoinFile.init) }, set: { if $0 == nil { exported = nil } })
    }

    private var detailShown: Binding<Bool> {
        Binding(get: { detail != nil }, set: { if !$0 { detail = nil } })
    }
}

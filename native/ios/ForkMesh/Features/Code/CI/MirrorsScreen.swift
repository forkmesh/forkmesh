import FMApi
import FMBase
import FMCode
import SwiftUI

struct RepoMirrorsScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoMirrorsModel?

    init(repo: RepoRef) {
        if let client = CIServices.client {
            _model = State(initialValue: RepoMirrorsModel(client: client, repo: repo))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                MirrorsList(model: model)
                    .task { await model.load() }
                    .fmRefreshable { await model.load() }
            } else {
                FmEmptyState(icon: "sync_alt", title: "Mirrors are unavailable")
            }
        }
        .navigationTitle("Mirrors")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct MirrorsList: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: RepoMirrorsModel

    private func light(_ node: RepoMirrorNode) -> Color {
        switch node.light {
        case "current": return fm.success
        case "behind", "unverified": return fm.warning
        default: return fm.textTertiary
        }
    }

    var body: some View {
        List {
            if !model.syncLabel.isEmpty {
                Section("Sync") {
                    Text(model.syncLabel).font(.headline)
                    Text(model.syncSummary).font(.caption).foregroundStyle(fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools([model.syncLabel, model.syncSummary].filter { !$0.isEmpty }.joined(separator: "\n"))
                }
            }
            if let error = model.error {
                Text(error).foregroundStyle(fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(error)
            }
            Section {
                Toggle("Online only", isOn: $model.onlineOnly)
                if model.shown.isEmpty && !model.loading {
                    Text("No mirrors are serving this repository.").foregroundStyle(fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools("No mirrors are serving this repository.")
                }
                ForEach(model.shown) { (node: RepoMirrorNode) in
                    HStack(alignment: .top, spacing: 10) {
                        Circle().fill(light(node)).frame(width: 10, height: 10).padding(.top, 5)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(node.machineName.isEmpty ? node.node : "\(node.node) · \(node.machineName)")
                            if !node.subtitle.isEmpty {
                                Text(node.subtitle).font(.caption).foregroundStyle(fm.textSecondary)
                            }
                            if !node.directReason.isEmpty && !node.directVerified {
                                Text(node.directReason).font(.caption2).foregroundStyle(fm.textTertiary)
                            }
                        }
                    }
                    .accessibilityElement(children: .combine)
                }
            } header: {
                Text("Nodes")
            }
        }
        .listStyle(.insetGrouped)
    }
}

import FMControl
import FMDesktopRemote
import SwiftUI

struct DesktopUpdateSection: View {
    @Bindable var store: DesktopUpdateStore
    @Binding var pending: PendingConfirmation?
    @State private var preparing = false
    @State private var prepareError = ""

    var body: some View {
        Section {
            if !store.version.isEmpty {
                LabeledContent("Version", value: store.version)
            }
            if store.unsupported {
                Label(
                    "This desktop's ForkMesh is too old to update from your phone. Update it once on the desktop.",
                    systemImage: "arrow.down.circle"
                )
                .font(.footnote)
                .foregroundStyle(.orange)
                .fmMessageTools("This desktop's ForkMesh is too old to update from your phone. Update it once on the desktop.")
            } else {
                Button {
                    Task { await prepare() }
                } label: {
                    HStack {
                        Label("Rebuild and restart", systemImage: "arrow.triangle.2.circlepath")
                        Spacer()
                        if preparing { ProgressView() }
                    }
                }
                .disabled(store.busy || preparing || !store.loaded)
            }
            DesktopUpdateProgress(store: store)
            if !prepareError.isEmpty {
                Label(prepareError, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(prepareError)
            }
        } header: {
            Text("ForkMesh on this desktop")
        } footer: {
            Text("Rebuilds ForkMesh from the desktop's source checkout and restarts it, like the desktop's Restart button. The build log streams here while it runs.")
        }
    }

    private func prepare() async {
        preparing = true
        defer { preparing = false }
        do {
            pending = PendingConfirmation(
                title: "Rebuild and restart", confirmation: try await store.prepare(), destructive: false)
            prepareError = ""
        } catch {
            prepareError = RemoteErrorText.describe(error)
        }
    }
}

struct DesktopUpdateProgress: View {
    @Bindable var store: DesktopUpdateStore

    var body: some View {
        switch store.stage {
        case .running:
            VStack(alignment: .leading, spacing: 6) {
                ProgressView(value: Double(store.state.percent), total: 100)
                Text(store.state.headline.isEmpty ? "Starting the rebuild…" : store.state.headline)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            .accessibilityElement(children: .combine)
        case .restarting:
            HStack(spacing: 10) {
                ProgressView()
                Text("Restarting. Waiting for the desktop to come back…")
                    .font(.footnote)
            }
        case .back:
            Label(backLine, systemImage: "checkmark.circle.fill")
                .foregroundStyle(.green)
                .font(.footnote)
                .fmMessageTools(backLine)
        case .failed:
            Label(
                store.state.error.isEmpty ? store.error : store.state.error,
                systemImage: "xmark.octagon.fill"
            )
            .foregroundStyle(.red)
            .font(.footnote)
            .fmMessageTools(store.state.error.isEmpty ? store.error : store.state.error)
        case .idle:
            if !store.state.status.isEmpty && !store.lines.isEmpty {
                Text(store.state.status)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fmMessageTools(store.state.status)
            }
        }
        if !store.lines.isEmpty {
            DesktopUpdateLog(lines: store.lines, revision: store.revision)
        }
    }

    private var backLine: String {
        if !store.previousVersion.isEmpty && store.previousVersion != store.version {
            return "Restarted on \(store.version) (was \(store.previousVersion))."
        }
        return store.version.isEmpty ? "The desktop restarted." : "Restarted on \(store.version)."
    }
}

struct DesktopUpdateLog: View {
    let lines: [String]
    let revision: Int

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView([.vertical, .horizontal]) {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(lines.enumerated()), id: \.offset) { index, line in
                        Text(line)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(color(line))
                            .lineLimit(1)
                            .fixedSize()
                            .id(index)
                    }
                }
                .padding(8)
                .textSelection(.enabled)
            }
            .frame(height: 220)
            .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 8))
            .onAppear { proxy.scrollTo(lines.count - 1, anchor: .bottomLeading) }
            .onChange(of: revision) { proxy.scrollTo(lines.count - 1, anchor: .bottomLeading) }
        }
        .accessibilityLabel("Build log")
    }

    private func color(_ line: String) -> Color {
        if line.hasPrefix("!!") { return .red }
        if line.hasPrefix("==>") { return .accentColor }
        if line.hasPrefix("$ ") { return .secondary }
        return .primary
    }
}

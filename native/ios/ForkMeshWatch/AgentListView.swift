// The root list: one row per active or recent agent session, running first
// and then by recency (docs/native/PARITY.md, 4.3). Each row shows the model
// face, the title, the repository, a status dot and a context gauge. Pull
// down (or scroll the Crown past the top) to ask the phone for a fresh read.

import FMWatchWire
import SwiftUI

struct AgentListView: View {
    @Environment(WatchStore.self) private var store
    @State private var showSettings = false

    var body: some View {
        List {
            if !store.snapshot.signedIn {
                Text(store.snapshot.note.isEmpty ? "Sign in to ForkMesh on your iPhone." : store.snapshot.note)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            } else if !store.hasSnapshot {
                HStack {
                    ProgressView()
                    Text("Asking your iPhone…").font(.footnote)
                }
            } else if store.frames.isEmpty {
                Text(store.snapshot.note.isEmpty ? "No agent work yet." : store.snapshot.note)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            ForEach(store.frames) { (frame: AgentFrame) in
                NavigationLink(value: frame.key) {
                    AgentRow(frame: frame)
                }
            }
            if store.hasSnapshot && !store.reachable {
                Label("iPhone out of reach", systemImage: "iphone.slash")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Agents")
        .refreshable {
            await store.send(.refresh)
        }
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    showSettings = true
                } label: {
                    Image(systemName: "gearshape")
                }
                .accessibilityLabel("Settings")
            }
        }
        .sheet(isPresented: $showSettings) {
            WatchSettingsView()
        }
        .onAppear {
            store.setWatched(nil)
        }
    }
}

struct AgentRow: View {
    let frame: AgentFrame

    var body: some View {
        HStack(spacing: 8) {
            ModelFace(frame: frame)
            VStack(alignment: .leading, spacing: 2) {
                Text(frame.title)
                    .font(.system(.footnote, weight: .semibold))
                    .lineLimit(2)
                Text(frame.repo)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                HStack(spacing: 4) {
                    StatusDot(status: frame.status)
                    Text(frame.status.label).font(.caption2)
                    if frame.turns > 0 {
                        Text("· \(frame.turns)t").font(.caption2).foregroundStyle(.secondary)
                    }
                    if frame.tokens > 0 {
                        Text("· \(compact(frame.tokens))").font(.caption2).foregroundStyle(.secondary)
                    }
                }
            }
            Spacer(minLength: 0)
            if let fraction = frame.contextFraction {
                Gauge(value: fraction) {
                    Text("Context")
                }
                .gaugeStyle(.accessoryCircularCapacity)
                .tint(fraction > 0.85 ? .orange : .accentColor)
                .scaleEffect(0.55)
                .frame(width: 26, height: 26)
            }
        }
        .accessibilityElement(children: .combine)
    }

    private func compact(_ n: Int) -> String {
        if n >= 1_000_000 { return String(format: "%.1fM", Double(n) / 1_000_000) }
        if n >= 1_000 { return "\(n / 1_000)k" }
        return "\(n)"
    }
}

/// The model's face: its short name on a disc in the colour of what it is
/// doing now.
struct ModelFace: View {
    let frame: AgentFrame

    var body: some View {
        ZStack {
            Circle().fill(Color(hue: frame.hue, saturation: 0.55, brightness: frame.status.isActive ? 0.85 : 0.45))
            Text(String(frame.face.prefix(2)))
                .font(.system(size: 11, weight: .bold, design: .rounded))
                .foregroundStyle(.black.opacity(0.8))
        }
        .frame(width: 26, height: 26)
        .accessibilityLabel(frame.face)
    }
}

struct StatusDot: View {
    let status: WireStatus

    var body: some View {
        Circle()
            .fill(StatusDot.color(status))
            .frame(width: 6, height: 6)
    }

    static func color(_ status: WireStatus) -> Color {
        switch status {
        case .running: return .green
        case .waiting: return .yellow
        case .queued: return .blue
        case .done, .merged: return .gray
        case .failed: return .red
        case .stopped: return .orange
        case .unknown: return .gray
        }
    }
}

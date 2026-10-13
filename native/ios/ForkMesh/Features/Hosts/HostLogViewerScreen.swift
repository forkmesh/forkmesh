// WS7: a host's live log: the lines the desktop's own log feed receives from the host (journal,
// node log, web front ends), streamed with the desktop's backpressure window. Follows the end
// unless paused; search filters what is shown; Copy takes what is shown.

import FMHosts
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct HostLogViewerScreen: View {
    @State var stream: HostLogStream
    @State private var query = ""
    @State private var follow = true
    @State private var copied = false

    var body: some View {
        let lines = stream.buffer.filtered(query)
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    ForEach(lines) { line in
                        LogLineView(line: line)
                            .id(line.id)
                    }
                    Color.clear.frame(height: 1).id(Self.bottom)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
            }
            .onChange(of: stream.revision) { _, _ in
                if follow && !stream.buffer.paused {
                    proxy.scrollTo(Self.bottom, anchor: .bottom)
                }
            }
            .onAppear { proxy.scrollTo(Self.bottom, anchor: .bottom) }
        }
        .safeAreaInset(edge: .top, spacing: 0) { statusBar }
        .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .always), prompt: "Search the log")
        .navigationTitle(stream.host)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                Button {
                    stream.setPaused(!stream.buffer.paused)
                } label: {
                    Label(stream.buffer.paused ? "Resume" : "Pause",
                          systemImage: stream.buffer.paused ? "play.fill" : "pause.fill")
                }
                Menu {
                    Button {
                        copy(stream.buffer.copyText(query))
                    } label: {
                        Label(query.isEmpty ? "Copy log" : "Copy matching lines", systemImage: "doc.on.doc")
                    }
                    Toggle(isOn: $follow) {
                        Label("Follow the end", systemImage: "arrow.down.to.line")
                    }
                    Button(role: .destructive) {
                        stream.clear()
                    } label: {
                        Label("Clear", systemImage: "trash")
                    }
                } label: {
                    Label("More", systemImage: "ellipsis.circle")
                }
            }
        }
        .task { await stream.start() }
        .onDisappear {
            let current = stream
            Task { await current.stop() }
        }
    }

    private static let bottom = "log-bottom"

    private var statusBar: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(stream.live ? (stream.buffer.paused ? Color.orange : Color.green) : Color.red)
                .frame(width: 8, height: 8)
            Text(statusText)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .fmMessageTools(stream.error)
            Spacer()
            if copied {
                Text("Copied").font(.caption.weight(.semibold)).foregroundStyle(.green)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .shellGlass(Capsule())
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
    }

    private var statusText: String {
        if !stream.error.isEmpty { return stream.error }
        var parts: [String] = []
        parts.append(stream.live ? (stream.buffer.paused ? "Paused" : "Live") : "Connecting…")
        parts.append("\(stream.buffer.lines.count) lines")
        if stream.buffer.paused && !stream.buffer.held.isEmpty {
            parts.append("\(stream.buffer.held.count) waiting")
        }
        if stream.buffer.dropped > 0 { parts.append("\(stream.buffer.dropped) skipped") }
        return parts.joined(separator: " · ")
    }

    private func copy(_ text: String) {
        #if canImport(UIKit)
        UIPasteboard.general.string = text
        #endif
        copied = true
        Task {
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            copied = false
        }
    }
}

private struct LogLineView: View {
    let line: HostLogLine

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 6) {
            if !line.source.isEmpty {
                Text(line.source)
                    .font(.system(size: 10, weight: .semibold, design: .monospaced))
                    .foregroundStyle(color)
                    .frame(width: 44, alignment: .leading)
            }
            Text(line.text)
                .font(.system(size: 11, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var color: Color {
        switch line.source {
        case "node": return .green
        case "mirror": return .blue
        case "web": return .orange
        case "ssh": return .purple
        case "host": return .yellow
        default: return .secondary
        }
    }
}

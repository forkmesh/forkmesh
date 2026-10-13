// One agent, live (docs/native/PARITY.md, 4.3): its body on the Watch field,
// coloured by what it is doing, with the recent marks round it and the last
// reply on the dot display underneath; a full-screen field as the next
// vertical page; and the controls the phone says it can carry out.
//
// Pause and Approve are not offered: neither exists on the relay or the
// desktop yet (PARITY 2.2 and 4.1).

import FMAgentWatch
import FMWatchWire
import SwiftUI

struct AgentLiveView: View {
    let key: String
    @Environment(WatchStore.self) private var store

    var body: some View {
        Group {
            if let frame = store.frame(key) {
                TabView {
                    LivePage(frame: frame)
                    FieldPage(frame: frame)
                    ControlsPage(frame: frame)
                }
                .tabViewStyle(.verticalPage)
                .navigationTitle(frame.face)
                .toolbar {
                    ToolbarItem(placement: .topBarTrailing) {
                        StatusBadge(frame: frame)
                    }
                    ToolbarItemGroup(placement: .bottomBar) {
                        QuickControls(frame: frame)
                    }
                }
            } else {
                Text("This agent is no longer in the list.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .onAppear { store.setWatched(key) }
        .alert(
            store.notice ?? "",
            isPresented: Binding(get: { store.notice != nil }, set: { if !$0 { store.notice = nil } })
        ) {
            Button("OK", role: .cancel) { store.notice = nil }
        }
    }
}

/// Status and elapsed time, top right.
private struct StatusBadge: View {
    let frame: AgentFrame

    var body: some View {
        HStack(spacing: 3) {
            StatusDot(status: frame.status)
            if frame.status == .running, frame.startedAtMs > 0 {
                Text(Date(timeIntervalSince1970: Double(frame.startedAtMs) / 1000), style: .timer)
                    .font(.caption2.monospacedDigit())
            } else {
                Text(frame.status.label).font(.caption2)
            }
        }
    }
}

private struct LivePage: View {
    let frame: AgentFrame
    @Environment(WatchStore.self) private var store

    var body: some View {
        ScrollView {
            VStack(spacing: 6) {
                AgentFieldView(frame: frame, arrivedAt: store.beatArrivedAt[frame.key], showLabels: false)
                    .frame(height: 104)
                Text(frame.title)
                    .font(.system(.footnote, weight: .semibold))
                    .multilineTextAlignment(.center)
                    .lineLimit(2)
                if let beat = frame.beats.last {
                    Text("\(beat.motion.label): \(beat.text)")
                        .font(.caption2)
                        .foregroundStyle(Color(hue: beat.motion.hue, saturation: 0.5, brightness: 0.95))
                        .lineLimit(2)
                }
                DotDisplay(text: frame.lastReply)
                if frame.controls.contains(.diff) {
                    ChangesLink(frame: frame)
                }
            }
        }
    }
}

/// The last reply, on a strip of lit dots.
private struct DotDisplay: View {
    let text: String

    var body: some View {
        if !text.isEmpty {
            Text(text)
                .font(.system(size: 12, design: .monospaced))
                .foregroundStyle(Color(red: 0.55, green: 0.95, blue: 0.7))
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(6)
                .background(RoundedRectangle(cornerRadius: 6).fill(Color(white: 0.08)))
        }
    }
}

private struct FieldPage: View {
    let frame: AgentFrame
    @Environment(WatchStore.self) private var store

    var body: some View {
        AgentFieldView(frame: frame, arrivedAt: store.beatArrivedAt[frame.key], showLabels: true)
            .ignoresSafeArea()
    }
}

/// Stop, Continue and Prompt in the bottom toolbar, when they apply.
private struct QuickControls: View {
    let frame: AgentFrame
    @Environment(WatchStore.self) private var store

    var body: some View {
        if frame.controls.contains(.stop) {
            Button {
                store.perform(.stop(key: frame.key))
            } label: {
                Image(systemName: "stop.fill")
            }
            .tint(.red)
            .accessibilityLabel("Stop")
            .disabled(store.busy.contains(frame.key))
        }
        Spacer()
        if frame.controls.contains(.prompt) {
            PromptLink(frame: frame, compact: true)
        } else if frame.controls.contains(.continue) {
            Button {
                store.perform(.continue(key: frame.key))
            } label: {
                Image(systemName: "play.fill")
            }
            .accessibilityLabel("Continue")
            .disabled(store.busy.contains(frame.key))
        }
    }
}

/// Opens dictation, Scribble or the keyboard and sends what was said.
private struct PromptLink: View {
    let frame: AgentFrame
    var compact = false
    @Environment(WatchStore.self) private var store

    var body: some View {
        TextFieldLink(prompt: Text("What next?")) {
            if compact {
                Image(systemName: "mic.fill")
            } else {
                Label("Prompt", systemImage: "mic.fill")
            }
        } onSubmit: { (text: String) in
            if let problem = WatchCommand.promptProblem(text) {
                store.notice = problem
                return
            }
            store.perform(.prompt(key: frame.key, text: text))
        }
        .accessibilityLabel("Prompt")
        .disabled(store.busy.contains(frame.key))
    }
}

private struct ChangesLink: View {
    let frame: AgentFrame

    var body: some View {
        NavigationLink {
            AgentDiffView(key: frame.key)
        } label: {
            Label("Review changes", systemImage: "plus.forwardslash.minus")
        }
    }
}

private struct ControlsPage: View {
    let frame: AgentFrame
    @Environment(WatchStore.self) private var store

    var body: some View {
        List {
            if frame.controls.contains(.diff) {
                ChangesLink(frame: frame)
            }
            if frame.controls.contains(.prompt) {
                PromptLink(frame: frame)
            }
            if frame.controls.contains(.continue) {
                Button {
                    store.perform(.continue(key: frame.key))
                } label: {
                    Label("Continue", systemImage: "play.fill")
                }
            }
            if frame.controls.contains(.retry) {
                Button {
                    store.perform(.retry(key: frame.key))
                } label: {
                    Label("Run again", systemImage: "arrow.clockwise")
                }
            }
            if frame.controls.contains(.stop) {
                Button(role: .destructive) {
                    store.perform(.stop(key: frame.key))
                } label: {
                    Label("Stop", systemImage: "stop.fill")
                }
            }
            if frame.controls.subtracting(.diff).isEmpty {
                Text("Nothing can be changed from here. Stop and Continue need the desktop on your iPhone's network.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            Toggle("Mute this agent", isOn: Binding(
                get: { store.feedback.isMuted(frame.key) },
                set: { store.feedback.setMuted(frame.key, $0) }))
            Section("Run") {
                LabeledContent("Repository", value: frame.repo)
                if !frame.model.isEmpty { LabeledContent("Model", value: frame.model) }
                if frame.turns > 0 { LabeledContent("Turns", value: "\(frame.turns)") }
                if frame.tokens > 0 { LabeledContent("Tokens", value: "\(frame.tokens)") }
            }
        }
        .disabled(store.busy.contains(frame.key))
    }
}

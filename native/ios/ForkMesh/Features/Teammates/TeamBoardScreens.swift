import FMApi
import FMCode
import FMTeammates
import SwiftUI

struct TeamBriefingScreen: View {
    @Bindable var store: TeammatesStore

    @State private var briefing: TeamBriefing?
    @State private var error = ""
    @State private var pending: PendingTeamConfirmation?
    @State private var banner = ""

    var body: some View {
        List {
            if let briefing {
                Section {
                    Text(briefing.headline).font(.headline)
                    if !briefing.repo.isEmpty {
                        Text("\(briefing.repo) · \(briefing.date)").font(.caption).foregroundStyle(.secondary)
                    }
                    Button {
                        Task { await prepare() }
                    } label: {
                        Label("Generate briefing now", systemImage: "sparkles")
                    }
                }
                ForEach(briefing.entries) { entry in
                    Section {
                        HStack(spacing: 10) {
                            TeammateAvatar(name: entry.name, avatar: entry.avatar, size: 30)
                            Text(entry.name).font(.subheadline.weight(.semibold))
                            Spacer()
                            TeammateStatusBadge(status: entry.status)
                        }
                        if entry.output.isEmpty {
                            Text(entry.hasUpdate ? "Working on it…" : "No update yet today.")
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        } else {
                            Text(entry.output).font(.callout).textSelection(.enabled)
                        }
                    }
                }
            }
            if !banner.isEmpty {
                Section { Label(banner, systemImage: "checkmark.circle").foregroundStyle(.green).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(banner) }
            }
            if !error.isEmpty {
                Section { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            }
        }
        .overlay {
            if briefing == nil && error.isEmpty { ProgressView() }
        }
        .navigationTitle("Briefing")
        .fmRefreshable { await load() }
        .task { await load() }
        .onChange(of: store.revision) { _, _ in Task { await load() } }
        .sheet(item: $pending) { item in
            NavigationStack {
                TeamConfirmSheet(pending: item) { confirmation in
                    try await store.run(confirmation)
                } onDone: { message in
                    banner = message
                }
            }
        }
    }

    private func load() async {
        do {
            briefing = try await store.briefing()
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }

    private func prepare() async {
        do {
            let action = TeammateAction.briefing(id: nil)
            pending = PendingTeamConfirmation(title: action.title, confirmation: try await store.prepare(action))
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }
}

struct TeamStatsScreen: View {
    @Bindable var store: TeammatesStore

    @State private var stats: TeamStats?
    @State private var error = ""
    @State private var grading: Int?

    var body: some View {
        List {
            if let stats {
                Section {
                    LabeledContent("Tasks", value: "\(stats.totalTasks)")
                    LabeledContent("Spend", value: String(format: "$%.2f", stats.totalCost))
                }
                Section("Special Agents") {
                    ForEach(stats.teammates) { row in
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                TeammateAvatar(name: row.name, avatar: row.avatar, size: 26)
                                Text(row.name).font(.subheadline.weight(.semibold))
                                Spacer()
                                Text(String(format: "$%.2f", row.costUsd)).font(.caption).foregroundStyle(.secondary)
                            }
                            HStack(spacing: 12) {
                                Text("\(row.runs) runs")
                                Text("\(Int((row.successRate * 100).rounded()))% done")
                                if row.graded > 0 {
                                    Label(String(format: "%.1f", row.averageGrade), systemImage: "star.fill")
                                        .foregroundStyle(.yellow)
                                }
                            }
                            .font(.caption)
                            ProgressView(value: row.successRate).tint(.green)
                        }
                    }
                }
                Section("Recent tasks") {
                    ForEach(stats.tasks) { task in
                        VStack(alignment: .leading, spacing: 4) {
                            Text(task.title.isEmpty ? "Session #\(task.session)" : task.title)
                                .font(.subheadline)
                                .lineLimit(2)
                            HStack {
                                Text(task.status).font(.caption).foregroundStyle(.secondary)
                                Spacer()
                                if task.gradable {
                                    GradeStars(grade: task.grade, busy: grading == task.session) { grade in
                                        Task { await setGrade(task.session, grade) }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if !error.isEmpty {
                Section { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            }
        }
        .overlay {
            if stats == nil && error.isEmpty { ProgressView() }
        }
        .navigationTitle("Special agent stats")
        .fmRefreshable { await load() }
        .task { await load() }
    }

    private func load() async {
        do {
            stats = try await store.stats()
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }

    private func setGrade(_ session: Int, _ grade: Int) async {
        grading = session
        defer { grading = nil }
        do {
            try await store.perform(.grade(session: session, grade: grade))
            await load()
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }
}

struct GradeStars: View {
    let grade: Int
    var busy = false
    let set: (Int) -> Void

    var body: some View {
        HStack(spacing: 2) {
            if busy { ProgressView().controlSize(.mini) }
            ForEach(1...5, id: \.self) { value in
                Button {
                    set(value == grade ? 0 : value)
                } label: {
                    Image(systemName: value <= grade ? "star.fill" : "star")
                        .foregroundStyle(value <= grade ? Color.yellow : Color.secondary)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Grade \(value) of 5")
            }
        }
        .disabled(busy)
    }
}

struct TeamRoomScreen: View {
    let store: TeammatesStore

    @State private var messages: [TeamRoomMessage] = []
    @State private var loaded = false
    @State private var error = ""

    var body: some View {
        List {
            if messages.isEmpty && loaded {
                Text("Nothing in #team yet. Special agents post here when they finish, if their relay room is #team.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            ForEach(messages) { message in
                VStack(alignment: .leading, spacing: 3) {
                    HStack {
                        Text(message.sender.isEmpty ? "Special agent" : message.sender)
                            .font(.caption.weight(.semibold))
                        Spacer()
                        if message.timestampMs > 0 {
                            Text(Date(timeIntervalSince1970: Double(message.timestampMs) / 1000), style: .relative)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                    LinkedText(message.text, repo: ReferenceLinks.chatRepo, commits: false).font(.callout)
                }
                .fmMessageActions(text: message.text)
                .listRowBackground(message.isSelf ? Color.accentColor.opacity(0.08) : nil)
            }
            if !error.isEmpty {
                Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error)
            }
        }
        .navigationTitle("#team")
        .fmRefreshable { await load() }
        .task { await load() }
    }

    private func load() async {
        do {
            messages = try await store.room()
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
        loaded = true
    }
}

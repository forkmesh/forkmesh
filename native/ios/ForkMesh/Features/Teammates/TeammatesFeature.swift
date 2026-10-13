import FMBase
import FMCode
import FMControl
import FMTeammates
import SwiftUI

@MainActor
enum TeammatesFeature {
    static func register() {
        guard let hub = HostsFeature.hub else { return }
        if Services.get(TeammatesStore.self) == nil { Services.put(TeammatesStore(channel: hub)) }
        if Services.get(ShortcutsStore.self) == nil {
            let store = ShortcutsStore(channel: hub)
            store.mirror = PhoneShortcutMirror()
            Services.put(store)
        }
    }

    static var teammates: TeammatesStore? {
        register()
        return Services.get(TeammatesStore.self)
    }

    static var shortcuts: ShortcutsStore? {
        register()
        return Services.get(ShortcutsStore.self)
    }

    static var hub: DesktopControlHub? { HostsFeature.hub }
}

@MainActor
final class PhoneShortcutMirror: ShortcutMirror {
    func mirroredRepos() -> [String] {
        (LocalRepoServices.store?.records ?? []).compactMap(\.publishedAs).filter { !$0.isEmpty }
    }

    func worktree(repo: String) -> URL? {
        guard let store = LocalRepoServices.store, let record = store.record(meshIdentity: repo) else { return nil }
        return record.rebasedOn(store.root).worktree
    }
}

struct TeamUnavailableView: View {
    var title = "Special agents are not available"

    var body: some View {
        ContentUnavailableView {
            Label(title, systemImage: "person.3")
        } description: {
            Text("Sign in, then pair your desktop on the same network.")
        } actions: {
            FmMessageTools("\(title). Sign in, then pair your desktop on the same network.")
        }
    }
}

struct TeamOutdatedRow: View {
    var body: some View {
        Label(
            "This desktop's ForkMesh does not answer this yet. Update it from the desktop's Settings.",
            systemImage: "arrow.down.circle"
        )
        .font(.footnote)
        .foregroundStyle(.orange)
        .frame(maxWidth: .infinity, alignment: .leading)
        .fmMessageTools("This desktop's ForkMesh does not answer this yet. Update it from the desktop's Settings.")
    }
}

struct PendingTeamConfirmation: Identifiable {
    let title: String
    let confirmation: TeamConfirmation
    var destructive = false

    var id: String { confirmation.id }
}

struct TeamConfirmSheet: View {
    @Environment(\.dismiss) private var dismiss
    let pending: PendingTeamConfirmation
    let perform: (TeamConfirmation) async throws -> String
    var onDone: (String) -> Void = { _ in }

    @State private var running = false
    @State private var error = ""
    @State private var openedAt = Date()

    var body: some View {
        Form {
            Section {
                Text(pending.confirmation.summary)
                    .fixedSize(horizontal: false, vertical: true)
            } header: {
                Text(pending.title)
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            Section {
                Button(role: pending.destructive ? .destructive : nil) {
                    Task { await run() }
                } label: {
                    HStack {
                        Text(pending.title)
                        Spacer()
                        if running { ProgressView() }
                    }
                }
                .disabled(running)
            } footer: {
                Text("The desktop carries this out and writes it to its log.")
            }
        }
        .navigationTitle("Confirm")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
        }
    }

    private func run() async {
        if Date().timeIntervalSince(openedAt) * 1000 > Double(pending.confirmation.expiresInMs) {
            error = TeamErrorText.describe(code: "confirmation_expired")
            return
        }
        running = true
        defer { running = false }
        do {
            let message = try await perform(pending.confirmation)
            onDone(message)
            dismiss()
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }
}

enum TeammateFaces {
    static let portraits: Set<String> = [
        "blogger", "ceo", "cfo", "code-steward", "coo", "cpa", "crawler", "fixer", "hearth", "herald", "marketing",
        "mobile-developer", "quality", "reaper", "reddit", "sage", "sales", "sentinel", "tempo", "ux", "yc-news",
    ]
    static let models: Set<String> = ["opus", "sonnet", "haiku", "fable", "sol", "luna", "terra"]

    static func asset(_ avatar: String) -> String? {
        let key = avatar.trimmingCharacters(in: .whitespaces).lowercased()
        if portraits.contains(key) { return "teammate-face-\(key)" }
        if models.contains(key) { return "agent-face-\(key)" }
        return nil
    }
}

struct TeammateAvatar: View {
    let name: String
    let avatar: String
    var size: CGFloat = 40

    var body: some View {
        ZStack {
            if let face = TeammateFaces.asset(avatar) {
                Image(face).resizable().scaledToFill()
            } else {
                Circle().fill(tint.gradient)
                Text(initials)
                    .font(.system(size: size * 0.38, weight: .semibold))
                    .foregroundStyle(.white)
            }
        }
        .frame(width: size, height: size)
        .clipShape(Circle())
        .accessibilityHidden(true)
    }

    private var initials: String {
        let letters = name.split(separator: " ").prefix(2).compactMap(\.first).map { String($0).uppercased() }
        return letters.isEmpty ? "?" : letters.joined()
    }

    private var tint: Color {
        let palette: [Color] = [.blue, .purple, .orange, .teal, .pink, .indigo, .green, .red]
        let seed = (avatar.isEmpty ? name : avatar).unicodeScalars.reduce(0) { $0 &+ Int($1.value) }
        return palette[abs(seed) % palette.count]
    }
}

struct TeammateStatusBadge: View {
    let status: TeammateStatus

    var body: some View {
        Text(status.rawValue)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 7)
            .padding(.vertical, 3)
            .foregroundStyle(tint)
            .background(tint.opacity(0.14), in: Capsule())
    }

    private var tint: Color {
        switch status {
        case .working: return .green
        case .queued, .waiting: return .orange
        case .scheduled: return .blue
        case .paused: return .gray
        case .idle: return .secondary
        }
    }
}

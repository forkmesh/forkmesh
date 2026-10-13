import FMApi
import FMControl
import FMTeammates
import SwiftUI

struct TeammatesDestination: View {
    var body: some View {
        if let store = TeammatesFeature.teammates, let hub = TeammatesFeature.hub {
            TeammatesScreen(store: store, hub: hub)
        } else {
            TeamUnavailableView()
        }
    }
}

struct TeammatesScreen: View {
    @Bindable var store: TeammatesStore
    @Bindable var hub: DesktopControlHub

    @State private var filter: TeammateStatus?
    @State private var pending: PendingTeamConfirmation?
    @State private var banner = ""
    @State private var error = ""
    @State private var showEditor = false

    private let columns = [GridItem(.adaptive(minimum: 64, maximum: 120), spacing: 8)]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                header
                if let snapshot = store.snapshot {
                    filterBar(snapshot)
                    let rows = snapshot.filtered(filter)
                    if rows.isEmpty {
                        Text(snapshot.available ? "No special agents here yet." : "Pick a repository with a working tree.")
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .center)
                            .padding(.top, 30)
                    }
                    LazyVGrid(columns: columns, spacing: 8) {
                        ForEach(rows) { row in
                            NavigationLink(value: Route.teammate(id: row.id)) {
                                TeammateTile(row: row) { action, destructive in
                                    Task { await prepare(action, destructive: destructive) }
                                }
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    if !snapshot.errors.isEmpty {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(snapshot.errors, id: \.self) { line in
                                Label(line, systemImage: "exclamationmark.triangle")
                                    .font(.caption)
                                    .foregroundStyle(.orange)
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .fmMessageTools(line)
                            }
                        }
                    }
                } else if store.loading {
                    ProgressView().frame(maxWidth: .infinity)
                }
            }
            .padding()
        }
        .navigationTitle("Special Agents")
        .toolbar { toolbar }
        .fmRefreshable { await store.refresh() }
        .task {
            await store.refresh()
            await store.loadOptions()
        }
        .sheet(item: $pending) { item in
            NavigationStack {
                TeamConfirmSheet(pending: item) { confirmation in
                    try await store.run(confirmation)
                } onDone: { message in
                    banner = message
                }
            }
        }
        .sheet(isPresented: $showEditor) {
            NavigationStack {
                TeammateEditorScreen(store: store, id: nil)
            }
        }
    }

    @ViewBuilder
    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            DesktopReachLine(reach: hub.reach)
            if store.unsupported { TeamOutdatedRow() }
            if let snapshot = store.snapshot {
                HStack {
                    Menu {
                        ForEach(snapshot.repos, id: \.self) { repo in
                            Button(repo) { Task { await store.selectRepo(repo) } }
                        }
                    } label: {
                        Label(snapshot.repo.isEmpty ? "Choose a repository" : snapshot.repo, systemImage: "folder")
                            .font(.subheadline.weight(.semibold))
                    }
                    Spacer()
                }
                Text(snapshot.summary.line).font(.footnote).foregroundStyle(.secondary)
            }
            if !banner.isEmpty {
                Label(banner, systemImage: "checkmark.circle").font(.footnote).foregroundStyle(.green).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(banner)
            }
            let message = error.isEmpty ? store.error : error
            if !message.isEmpty && !store.unsupported {
                Label(message, systemImage: "exclamationmark.triangle").font(.footnote).foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(message)
            }
        }
    }

    private func filterBar(_ snapshot: TeammatesSnapshot) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                chip("All \(snapshot.summary.total)", selected: filter == nil) { filter = nil }
                chip("Working \(snapshot.summary.working)", selected: filter == .working) { filter = .working }
                chip("Scheduled \(snapshot.summary.scheduled)", selected: filter == .scheduled) { filter = .scheduled }
                chip("Idle", selected: filter == .idle) { filter = .idle }
            }
        }
    }

    private func chip(_ title: String, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title)
                .font(.footnote.weight(.semibold))
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(selected ? Color.accentColor.opacity(0.18) : Color.secondary.opacity(0.1), in: Capsule())
                .foregroundStyle(selected ? Color.accentColor : Color.primary)
        }
        .buttonStyle(.plain)
    }

    @ToolbarContentBuilder
    private var toolbar: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) {
            Menu {
                Button {
                    Task { await prepare(.runTeam) }
                } label: {
                    Label("Run the special agents now", systemImage: "play.fill")
                }
                Button {
                    Task { await prepare(.briefing(id: nil)) }
                } label: {
                    Label("Generate briefing", systemImage: "newspaper")
                }
                Button(role: .destructive) {
                    Task { await prepare(.disableSchedules, destructive: true) }
                } label: {
                    Label("Pause all schedules", systemImage: "pause.circle")
                }
                .disabled((store.snapshot?.automaticSchedules ?? 0) == 0)
                Divider()
                Button {
                    showEditor = true
                } label: {
                    Label("Add special agent", systemImage: "person.badge.plus")
                }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
        }
        ToolbarItem(placement: .secondaryAction) {
            NavigationLink {
                TeamBriefingScreen(store: store)
                    .shellPageClearance()
            } label: {
                Label("Briefing", systemImage: "newspaper")
            }
        }
        ToolbarItem(placement: .secondaryAction) {
            NavigationLink {
                TeamStatsScreen(store: store)
                    .shellPageClearance()
            } label: {
                Label("Stats", systemImage: "chart.bar")
            }
        }
        ToolbarItem(placement: .secondaryAction) {
            NavigationLink {
                TeamRoomScreen(store: store)
                    .shellPageClearance()
            } label: {
                Label("#team", systemImage: "bubble.left.and.bubble.right")
            }
        }
    }

    private func prepare(_ action: TeammateAction, destructive: Bool = false) async {
        do {
            pending = PendingTeamConfirmation(
                title: action.title, confirmation: try await store.prepare(action), destructive: destructive)
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }
}

struct TeammateTile: View {
    let row: TeammateRow
    let perform: (TeammateAction, Bool) -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        ZStack {
            TimelineView(.animation(minimumInterval: 1.0 / 30, paused: reduceMotion)) { context in
                let t = context.date.timeIntervalSinceReferenceDate
                ZStack {
                    face(at: t)
                    RadialGradient(colors: [.clear, .black.opacity(0.55)], center: .center, startRadius: 18, endRadius: 60)
                }
                .overlay { border(at: t) }
            }
            VStack(spacing: 0) {
                hud
                Spacer(minLength: 0)
                nameLabel
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .background(Color.secondary.opacity(0.12))
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .contentShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(accessibilityText)
        .accessibilityAction(named: "Run now") { perform(.run(id: row.id), false) }
        .accessibilityAction(named: "Generate briefing") { perform(.briefing(id: row.id), false) }
    }

    private func face(at t: TimeInterval) -> some View {
        GeometryReader { proxy in
            let side = proxy.size.width
            let pose = TilePose(motion: motion, time: t, phase: phase)
            Group {
                if let asset = TeammateFaces.asset(row.avatar) {
                    Image(asset).resizable().scaledToFill()
                } else {
                    ZStack {
                        Rectangle().fill(initialsTint.gradient)
                        Text(row.initials)
                            .font(.system(size: side * 0.34, weight: .semibold))
                            .foregroundStyle(.white)
                    }
                }
            }
            .frame(width: side, height: side)
            .scaleEffect(pose.scale)
            .rotationEffect(.degrees(pose.roll))
            .offset(x: pose.x * side, y: pose.y * side)
            .opacity(row.status == .paused ? 0.45 : 1)
            .saturation(row.status == .paused ? 0.3 : 1)
        }
    }

    private var hud: some View {
        HStack(spacing: 0) {
            if row.status.isWorking {
                hudGlyph("apple.terminal.fill", tint: playTint)
                    .allowsHitTesting(false)
            } else {
                Button {
                    perform(.run(id: row.id), false)
                } label: {
                    hudGlyph("play.fill", tint: playTint)
                }
                .buttonStyle(.plain)
            }
            Spacer(minLength: 0)
            if !row.mood.isEmpty {
                Text(row.mood.emoji)
                    .font(.system(size: 11))
                    .frame(width: 17, height: 17)
                    .background(.black.opacity(0.45), in: Circle())
                    .overlay {
                        if row.mood.attention { Circle().stroke(Color.orange, lineWidth: 1.2) }
                    }
            }
            Spacer(minLength: 0)
            Menu {
                Section(menuHeader) {
                    if row.status.isWorking {
                        Button(role: .destructive) {
                            perform(.stop(id: row.id), true)
                        } label: {
                            Label("Stop", systemImage: "stop.fill")
                        }
                    } else {
                        Button {
                            perform(.run(id: row.id), false)
                        } label: {
                            Label("Run now", systemImage: "play.fill")
                        }
                    }
                    Button {
                        perform(.briefing(id: row.id), false)
                    } label: {
                        Label("Generate briefing", systemImage: "newspaper")
                    }
                }
            } label: {
                hudGlyph("ellipsis", tint: .black.opacity(0.45))
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 1)
    }

    private func hudGlyph(_ symbol: String, tint: Color) -> some View {
        Image(systemName: symbol)
            .font(.system(size: 8, weight: .bold))
            .foregroundStyle(.white)
            .frame(width: 17, height: 17)
            .background(tint, in: Circle())
            .frame(width: 24, height: 24)
            .contentShape(Rectangle())
    }

    private var nameLabel: some View {
        Text(row.name)
            .font(.system(size: 9, weight: .semibold))
            .foregroundStyle(.white)
            .lineLimit(1)
            .minimumScaleFactor(0.75)
            .padding(.horizontal, 3)
            .padding(.vertical, 2)
            .frame(maxWidth: .infinity)
            .background(
                LinearGradient(colors: [.clear, .black.opacity(0.7)], startPoint: .top, endPoint: .bottom))
    }

    @ViewBuilder
    private func border(at t: TimeInterval) -> some View {
        let shape = RoundedRectangle(cornerRadius: 10, style: .continuous)
        switch row.status {
        case .working, .queued, .waiting:
            shape.strokeBorder(
                row.status == .working ? Color.green : Color.orange,
                style: StrokeStyle(lineWidth: 2, dash: [5, 4], dashPhase: -CGFloat((t * 20).truncatingRemainder(dividingBy: 9))))
        case .scheduled:
            shape.strokeBorder(Color.blue, lineWidth: 1.5)
        case .paused:
            shape.strokeBorder(Color.gray, style: StrokeStyle(lineWidth: 1.5, dash: [3, 3]))
        case .idle:
            shape.strokeBorder(Color.white.opacity(0.08), lineWidth: 0.5)
        }
    }

    private var motion: String {
        row.status == .paused ? "doze" : (row.mood.isEmpty ? "float" : row.mood.motion)
    }

    private var phase: Double {
        Double(row.id.unicodeScalars.reduce(0) { ($0 &* 31 &+ Int($1.value)) & 0xffff } % 1000) / 1000
    }

    private var playTint: Color {
        switch row.status {
        case .working: return .green
        case .queued, .waiting: return .orange
        case .paused: return .gray
        case .scheduled, .idle: return .blue
        }
    }

    private var initialsTint: Color {
        let palette: [Color] = [.blue, .purple, .orange, .teal, .pink, .indigo, .green, .red]
        let seed = (row.avatar.isEmpty ? row.name : row.avatar).unicodeScalars.reduce(0) { $0 &+ Int($1.value) }
        return palette[abs(seed) % palette.count]
    }

    private var menuHeader: String {
        [row.name, row.specialty.isEmpty ? row.modelLine : row.specialty, row.mood.line, row.lastActive]
            .filter { !$0.isEmpty }
            .joined(separator: "\n")
    }

    private var accessibilityText: String {
        [row.name, row.specialty, row.status.rawValue, row.mood.label, row.lastActive]
            .filter { !$0.isEmpty }
            .joined(separator: ", ")
    }
}

private struct TilePose {
    var scale: CGFloat
    var x: CGFloat
    var y: CGFloat
    var roll: Double

    init(motion: String, time: TimeInterval, phase: Double) {
        let t = time + phase * 60
        func wave(_ period: Double, _ shift: Double = 0) -> Double { sin(2 * .pi * (t / period + shift)) }
        let calm = motion == "still" ? 0.3 : 1.0
        var scale = 1.1 + 0.025 * calm * wave(4.2)
        var x = 0.012 * calm * wave(7.3, phase)
        var y = 0.01 * calm * wave(5.9, phase * 2)
        var roll = 0.8 * calm * wave(6.7, phase)
        switch motion {
        case "wave":
            let burst = max(0, wave(3))
            roll += 7 * burst * wave(0.9)
            scale += 0.05 * burst
        case "jump":
            y -= 0.06 * max(0, wave(1.2))
        case "bounce":
            y -= 0.03 * abs(wave(1.0))
        case "nod":
            y += 0.025 * max(0, wave(1.4))
        case "pulse":
            scale += 0.06 * (0.5 + 0.5 * wave(1.1))
        case "wiggle":
            roll += 4 * wave(0.6)
        case "shake":
            x += 0.025 * wave(0.18) * max(0, wave(2.5))
        case "spin":
            roll += 10 * wave(1.6)
        case "doze":
            y += 0.025 * (0.5 + 0.5 * wave(4))
            roll += 3 * wave(8)
        default:
            break
        }
        self.scale = CGFloat(scale)
        self.x = CGFloat(x)
        self.y = CGFloat(y)
        self.roll = roll
    }
}

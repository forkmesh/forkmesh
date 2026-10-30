import FMApi
import FMBase
import FMPackages
import SwiftUI

struct PokerScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: PokerModel?
    private let table: String?

    init(table: String? = nil) {
        self.table = table
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: PokerHub.shared.model(api: api))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model {
                PokerLobbyView(model: model, initialTable: table)
            } else {
                PokerMessage(
                    symbol: "suit.spade.fill", title: "Poker is unavailable",
                    message: "Sign in to ForkMesh to play poker with other people.")
            }
        }
        .navigationTitle(PokerModel.title)
        .navigationBarTitleDisplayMode(.inline)
        .background(fm.bgBase)
    }
}

enum PokerLobbySheet: Identifiable {
    case create
    case join
    case sit(PokerTableSummary)

    var id: String {
        switch self {
        case .create: return "create"
        case .join: return "join"
        case .sit(let summary): return "sit-\(summary.id)"
        }
    }
}

struct PokerLobbyView: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Bindable var model: PokerModel
    let initialTable: String?
    @State private var tableShown = false
    @State private var pendingTable = false
    @State private var openedInitial = false
    @State private var sheet: PokerLobbySheet?
    @AppStorage(PokerSound.mutedKey) private var muted = false

    var body: some View {
        content
            .toolbar {
                ToolbarItemGroup(placement: .primaryAction) {
                    Button {
                        muted.toggle()
                        PokerHaptics.tap()
                    } label: {
                        Image(systemName: muted ? "speaker.slash" : "speaker.wave.2")
                    }
                    .accessibilityLabel(muted ? "Turn sounds on" : "Mute sounds")
                    .accessibilityIdentifier("poker-mute")
                    Menu {
                        Button("New table", systemImage: "plus") { sheet = .create }
                        Button("Join with a code", systemImage: "number") { sheet = .join }
                        Button("Refresh", systemImage: "arrow.clockwise") { Task { await model.loadLobby() } }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityLabel("Poker options")
                }
            }
            .task {
                PokerHub.shared.hold(model)
                PokerHub.shared.visible = true
                PokerSound.shared.warmUp()
                await model.loadLobby()
                if let initialTable, !openedInitial {
                    openedInitial = true
                    open(initialTable)
                }
            }
            .onAppear { PokerHub.shared.visible = true }
            .onDisappear {
                if !tableShown { PokerHub.shared.visible = false }
            }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                if phase == .active, PokerHub.shared.visible { model.catchUp() }
            }
            .sheet(item: $sheet, onDismiss: {
                guard pendingTable else { return }
                pendingTable = false
                tableShown = true
            }) { (which: PokerLobbySheet) in
                switch which {
                case .create:
                    PokerCreateSheet(model: model) {
                        pendingTable = true
                        sheet = nil
                    }
                case .join:
                    PokerJoinSheet(model: model) {
                        pendingTable = true
                        sheet = nil
                    }
                case .sit(let summary):
                    PokerQuickSitSheet(model: model, summary: summary) {
                        pendingTable = true
                        sheet = nil
                    }
                }
            }
            .fullScreenCover(isPresented: $tableShown, onDismiss: { model.closeTable() }) {
                PokerTableScreen(model: model) { tableShown = false }
            }
            .overlay(alignment: .top) {
                if !tableShown && sheet == nil { PokerNoticeBanner(model: model) }
            }
    }

    @ViewBuilder private var content: some View {
        if model.notInstalled {
            VStack(spacing: FmSpace.x4) {
                PokerMessage(
                    symbol: "suit.spade.fill", title: "Install Poker",
                    message: "Poker is a free package. Install it from the Market to get 10,000 play chips and sit down at a table.")
                FmFilledButton("Open in the Market", onPressed: {
                    Services.get(AppRouter.self)?.open(.package(id: PokerModel.packageId))
                })
            }
            .padding(FmSpace.x5)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.lobby == nil {
            if model.lobbyError.isEmpty {
                ProgressView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                VStack(spacing: FmSpace.x4) {
                    PokerMessage(symbol: "wifi.exclamationmark", title: "Poker couldn't load", message: model.lobbyError)
                    FmFilledButton("Try again", onPressed: { Task { await model.loadLobby() } })
                }
                .padding(FmSpace.x5)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        } else {
            lobby
        }
    }

    private var lobby: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x4) {
                PokerWalletCard(model: model)
                PokerStatsRow(wallet: model.wallet)
                HStack(spacing: FmSpace.x3) {
                    FmFilledButton("New table", fillWidth: true, onPressed: { sheet = .create })
                        .accessibilityIdentifier("poker-new-table")
                    FmOutlinedButton("Join with a code", fillWidth: true, onPressed: { sheet = .join })
                        .accessibilityIdentifier("poker-join-code")
                }
                if let lobby = model.lobby {
                    tableSection("Your tables", lobby.myTables, empty: "Tables you sit at or own show here.")
                    if !lobby.invitedTables.isEmpty {
                        tableSection("Invitations", lobby.invitedTables, empty: "")
                    }
                    tableSection("Open tables", lobby.openTables, empty: "No open tables right now. Start one and fill it with bots.")
                }
                Text("Chips are play money. They can't be bought, sold, cashed out or sent to anyone.")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .multilineTextAlignment(.center)
                    .padding(.top, FmSpace.x2)
            }
            .padding(FmSpace.x4)
        }
        .refreshable { await model.loadLobby() }
    }

    @ViewBuilder
    private func tableSection(_ title: String, _ tables: [PokerTableSummary], empty: String) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: title, count: tables.isEmpty ? nil : tables.count)
            if tables.isEmpty {
                Text(empty)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .padding(.vertical, FmSpace.x2)
            } else {
                ForEach(tables) { (summary: PokerTableSummary) in
                    PokerTableRow(
                        summary: summary, actor: model.actor, seated: summary.mine,
                        onSit: { sheet = .sit(summary) }, onOpen: { open(summary.id) })
                }
            }
        }
    }

    private func open(_ id: String) {
        PokerHaptics.tap()
        tableShown = true
        Task { await model.openTable(id) }
    }
}

struct PokerMessage: View {
    @Environment(\.fmPalette) private var fm
    let symbol: String
    let title: String
    let message: String

    var body: some View {
        VStack(spacing: FmSpace.x3) {
            Image(systemName: symbol)
                .font(.system(size: 40, weight: .semibold))
                .foregroundStyle(fm.textTertiary)
            Text(title)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
            Text(message)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
        }
    }
}

struct PokerNoticeBanner: View {
    @Bindable var model: PokerModel

    var body: some View {
        Group {
            if !model.notice.isEmpty {
                Text(model.notice)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                    .background(Capsule().fill(Color.black.opacity(0.78)))
                    .overlay(Capsule().strokeBorder(Color.white.opacity(0.12), lineWidth: 0.5))
                    .padding(.top, 8)
                    .padding(.horizontal, 24)
                    .transition(.move(edge: .top).combined(with: .opacity))
                    .onTapGesture { model.notice = "" }
                    .accessibilityAddTraits(.isStaticText)
            }
        }
        .animation(.easeOut(duration: 0.25), value: model.notice)
        .task(id: model.notice) {
            guard !model.notice.isEmpty else { return }
            let shown = model.notice
            try? await Task.sleep(nanoseconds: 3_200_000_000)
            if model.notice == shown { model.notice = "" }
        }
    }
}

struct PokerWalletCard: View {
    @Bindable var model: PokerModel

    var body: some View {
        let wallet = model.wallet
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("WALLET")
                        .font(.system(size: 12, weight: .bold))
                        .tracking(1.2)
                        .foregroundStyle(Color.white.opacity(0.7))
                    Text(PokerText.chips(wallet.chips))
                        .font(.system(size: 36, weight: .heavy, design: .rounded))
                        .monospacedDigit()
                        .foregroundStyle(.white)
                        .contentTransition(.numericText())
                        .accessibilityLabel("\(PokerText.chips(wallet.chips)) chips")
                    if wallet.onTables > 0 {
                        Text("\(PokerText.chips(wallet.onTables)) on tables")
                            .font(.system(size: 13, weight: .medium))
                            .foregroundStyle(Color.white.opacity(0.75))
                    }
                }
                Spacer(minLength: 8)
                PokerChipPile(amount: max(wallet.chips, 1), size: 30)
                    .accessibilityHidden(true)
            }
            claim(wallet)
        }
        .padding(18)
        .background(
            RoundedRectangle(cornerRadius: 20, style: .continuous)
                .fill(RadialGradient(
                    colors: [PokerColors.feltCenter, PokerColors.feltEdge], center: .topLeading, startRadius: 10, endRadius: 360))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 20, style: .continuous)
                .strokeBorder(PokerColors.gold.opacity(0.55), lineWidth: 1)
                .padding(5)
        )
        .shadow(color: Color.black.opacity(0.25), radius: 10, y: 6)
        .animation(.spring(response: 0.4, dampingFraction: 0.8), value: wallet.chips)
    }

    private var claimWindow: ClosedRange<Date>? {
        let now = Date()
        let until = model.claimDate
        guard until > now else { return nil }
        return now...until
    }

    @ViewBuilder
    private func claim(_ wallet: PokerWallet) -> some View {
        if wallet.canClaim {
            Button {
                Task { await model.claim() }
            } label: {
                Label("Collect \(PokerText.chips(wallet.claimAmount)) free chips", systemImage: "gift.fill")
                    .font(.system(size: 15, weight: .bold))
                    .frame(maxWidth: .infinity, minHeight: 44)
                    .foregroundStyle(Color(red: 0.16, green: 0.11, blue: 0.0))
                    .background(Capsule().fill(PokerColors.gold))
            }
            .buttonStyle(.plain)
            .disabled(model.busy)
            .accessibilityIdentifier("poker-claim")
        } else if let window = claimWindow {
            HStack(spacing: 6) {
                Image(systemName: "clock")
                Text("Next \(PokerText.chips(wallet.claimAmount)) free chips in")
                Text(timerInterval: window, countsDown: true)
                    .monospacedDigit()
                    .fontWeight(.bold)
            }
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(Color.white.opacity(0.85))
            .padding(.horizontal, 12)
            .frame(maxWidth: .infinity, minHeight: 40)
            .background(Capsule().fill(Color.black.opacity(0.25)))
            .accessibilityElement(children: .combine)
        }
    }
}

struct PokerStatsRow: View {
    @Environment(\.fmPalette) private var fm
    let wallet: PokerWallet

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                tile("Hands", PokerText.chips(wallet.handsPlayed))
                tile("Won", PokerText.chips(wallet.handsWon))
                tile("Win rate", PokerText.winRate(wallet))
                tile("Biggest pot", PokerText.short(wallet.biggestPot))
            }
            if !wallet.bestHand.isEmpty {
                Label("Best hand: \(wallet.bestHand)", systemImage: "crown")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
        }
    }

    private func tile(_ label: String, _ value: String) -> some View {
        VStack(spacing: 2) {
            Text(value)
                .font(.system(size: 17, weight: .bold, design: .rounded))
                .monospacedDigit()
                .foregroundStyle(fm.textPrimary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            Text(label)
                .fmText(.labelSmall, color: fm.textTertiary)
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 10)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(fm.bgRaised))
        .accessibilityElement(children: .combine)
    }
}

struct PokerTableRow: View {
    @Environment(\.fmPalette) private var fm
    let summary: PokerTableSummary
    let actor: String
    let seated: Bool
    let onSit: () -> Void
    let onOpen: () -> Void

    var body: some View {
        Button(action: onOpen) {
            HStack(spacing: FmSpace.x3) {
                ZStack {
                    Circle().fill(RadialGradient(colors: [PokerColors.feltCenter, PokerColors.feltEdge], center: .center, startRadius: 2, endRadius: 22))
                    Circle().strokeBorder(PokerColors.walnut, lineWidth: 3)
                    Image(systemName: summary.isPrivate ? "lock.fill" : "suit.spade.fill")
                        .font(.system(size: 15, weight: .bold))
                        .foregroundStyle(.white.opacity(0.9))
                }
                .frame(width: 42, height: 42)
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Text(summary.name.isEmpty ? "Table \(summary.code)" : summary.name)
                            .fmText(.titleSmall)
                            .lineLimit(1)
                        if summary.invited && !summary.mine { badge("Invited", fm.accent) }
                        if summary.owner == actor && !actor.isEmpty { badge("Owner", fm.textSecondary) }
                    }
                    Text("Blinds \(summary.blindsLine) · \(summary.seatsLine)\(summary.bots > 0 ? " · \(summary.bots) bot\(summary.bots == 1 ? "" : "s")" : "")")
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .lineLimit(1)
                    Text("Buy-in \(PokerText.chips(summary.minBuyIn)) – \(PokerText.chips(summary.maxBuyIn)) · \(summary.turnSeconds) s clock")
                        .fmText(.labelSmall, color: fm.textTertiary)
                        .lineLimit(1)
                }
                Spacer(minLength: 4)
                if summary.isFull && !seated {
                    Text("Full").fmText(.labelSmall, color: fm.textTertiary)
                } else if !seated {
                    Button(action: onSit) {
                        Text("Sit")
                            .font(.system(size: 13, weight: .bold))
                            .foregroundStyle(Color(red: 0.16, green: 0.11, blue: 0))
                            .padding(.horizontal, 14)
                            .frame(minHeight: 30)
                            .background(Capsule().fill(PokerColors.gold))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Sit at \(summary.name)")
                }
                Image(systemName: "chevron.right")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
            }
            .padding(FmSpace.x3)
            .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(fm.bgRaised))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("poker-table-\(summary.code)")
    }

    private func badge(_ text: String, _ color: Color) -> some View {
        Text(text)
            .font(.system(size: 10, weight: .bold))
            .foregroundStyle(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Capsule().fill(color.opacity(0.12)))
    }
}

struct PokerChipSlider: View {
    @Environment(\.fmPalette) private var fm
    @Binding var value: Int
    let range: ClosedRange<Int>
    let step: Int
    var label: String = "Buy-in"

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(label).fmText(.bodyMedium)
                Spacer()
                Text(PokerText.chips(value))
                    .font(.system(size: 17, weight: .bold, design: .rounded))
                    .monospacedDigit()
            }
            if range.upperBound > range.lowerBound {
                Slider(
                    value: Binding<Double>(
                        get: { Double(value) },
                        set: { (raw: Double) in
                            let stepped = Int((raw / Double(step)).rounded()) * step
                            value = min(range.upperBound, max(range.lowerBound, stepped))
                        }),
                    in: Double(range.lowerBound)...Double(range.upperBound)
                ) {
                    Text(label)
                } minimumValueLabel: {
                    Text(PokerText.short(range.lowerBound)).font(.caption2)
                } maximumValueLabel: {
                    Text(PokerText.short(range.upperBound)).font(.caption2)
                }
                .tint(PokerColors.gold)
            }
        }
        .accessibilityElement(children: .contain)
    }
}

struct PokerCreateSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: PokerModel
    let onCreated: () -> Void
    @State private var draft = PokerTableDraft()
    @State private var sitDown = true

    private var stake: PokerStake? { model.stakes.first { $0.id == draft.stakes } }
    private var range: ClosedRange<Int>? {
        guard let stake else { return nil }
        return PokerBuyIn.range(min: stake.minBuyIn, max: stake.maxBuyIn, wallet: model.wallet.chips)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Table name", text: $draft.name)
                        .textInputAutocapitalization(.words)
                        .accessibilityIdentifier("poker-create-name")
                    Picker("Stakes", selection: $draft.stakes) {
                        ForEach(model.stakes) { (stake: PokerStake) in
                            Text("\(stake.label) · \(stake.blindsLine)").tag(stake.id)
                        }
                    }
                    Stepper("Seats: \(draft.seats)", value: $draft.seats, in: 2...9)
                    Stepper("Bots: \(draft.bots)", value: $draft.bots, in: 0...max(0, draft.seats - 1))
                } footer: {
                    if let stake { Text("Buy-in \(stake.buyInLine). Bots buy in from the house, never from a wallet.") }
                }
                Section("Turn clock") {
                    Picker("Turn clock", selection: $draft.turnSeconds) {
                        ForEach(PokerTableDraft.turnChoices, id: \.self) { (seconds: Int) in
                            Text("\(seconds) s").tag(seconds)
                        }
                    }
                    .pickerStyle(.segmented)
                    Toggle("Private table", isOn: $draft.isPrivate)
                }
                Section {
                    Toggle("Sit down now", isOn: $sitDown)
                    if sitDown {
                        if let range {
                            PokerChipSlider(value: $draft.buyIn, range: range, step: PokerBuyIn.step(range))
                        } else {
                            Text("Your wallet is below this table's minimum buy-in. You can still create it and watch.")
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }
                    }
                } footer: {
                    Text("Private tables are reached by invitation, link or the six-letter code.")
                }
            }
            .navigationTitle("New table")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") { create() }
                        .disabled(model.busy)
                        .accessibilityIdentifier("poker-create")
                }
            }
            .onAppear { fitBuyIn() }
            .onChange(of: draft.stakes) { (_: String, _: String) in fitBuyIn() }
            .onChange(of: draft.seats) { (_: Int, seats: Int) in draft.bots = min(draft.bots, seats - 1) }
        }
        .overlay(alignment: .top) { PokerNoticeBanner(model: model) }
    }

    private func fitBuyIn() {
        guard let stake, let range else {
            draft.buyIn = 0
            return
        }
        draft.buyIn = PokerBuyIn.suggested(big: stake.big, range: range)
    }

    private func create() {
        var ready = draft
        if !sitDown || range == nil { ready.buyIn = 0 }
        Task {
            if await model.createTable(ready) {
                PokerHaptics.tap()
                onCreated()
            }
        }
    }
}

struct PokerJoinSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: PokerModel
    let onJoined: () -> Void
    @State private var code = ""
    @FocusState private var focused: Bool

    var body: some View {
        NavigationStack {
            VStack(spacing: FmSpace.x4) {
                Text("Enter the six-letter code the table's owner shared.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                TextField("QXRTMB", text: $code)
                    .font(.system(size: 30, weight: .bold, design: .monospaced))
                    .multilineTextAlignment(.center)
                    .textInputAutocapitalization(.characters)
                    .autocorrectionDisabled()
                    .focused($focused)
                    .padding(.vertical, 12)
                    .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color.secondary.opacity(0.12)))
                    .onChange(of: code) { (_: String, next: String) in
                        let cleaned = String(next.uppercased().filter { $0.isASCII && $0.isLetter }.prefix(6))
                        if cleaned != next { code = cleaned }
                    }
                    .onSubmit { join() }
                    .accessibilityIdentifier("poker-code-field")
                FmFilledButton("Join table", fillWidth: true, onPressed: PokerIds.normalizedCode(code) == nil || model.busy ? nil : { join() })
                Spacer()
            }
            .padding(FmSpace.x5)
            .navigationTitle("Join with a code")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            }
            .onAppear { focused = true }
        }
        .presentationDetents([.medium])
        .overlay(alignment: .top) { PokerNoticeBanner(model: model) }
    }

    private func join() {
        let typed = code
        Task {
            guard let id = await model.lookUp(code: typed) else { return }
            onJoined()
            await model.openTable(id)
        }
    }
}

struct PokerQuickSitSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: PokerModel
    let summary: PokerTableSummary
    let onSeated: () -> Void
    @State private var buyIn = 0

    private var range: ClosedRange<Int>? {
        PokerBuyIn.range(min: summary.minBuyIn, max: summary.maxBuyIn, wallet: model.wallet.chips)
    }

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 16) {
                Text("Blinds \(summary.blindsLine) · \(summary.seatsLine) · \(summary.turnSeconds) s clock")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                if let range {
                    PokerChipSlider(value: $buyIn, range: range, step: PokerBuyIn.step(range))
                    FmFilledButton("Sit down with \(PokerText.chips(buyIn))", fillWidth: true, onPressed: model.busy ? nil : { sit() })
                        .accessibilityIdentifier("poker-quick-sit")
                } else {
                    Text("Your wallet has \(PokerText.chips(model.wallet.chips)) chips, below this table's minimum buy-in of \(PokerText.chips(summary.minBuyIn)).")
                        .font(.subheadline)
                }
                Spacer(minLength: 0)
            }
            .padding(20)
            .navigationTitle(summary.name.isEmpty ? "Table \(summary.code)" : summary.name)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
            .onAppear {
                if let range { buyIn = PokerBuyIn.suggested(big: summary.big, range: range) }
            }
        }
        .presentationDetents([.medium])
        .overlay(alignment: .top) { PokerNoticeBanner(model: model) }
    }

    private func sit() {
        let amount = buyIn
        let id = summary.id
        Task {
            if await model.quickSit(id, buyIn: amount) {
                PokerHaptics.tap()
                onSeated()
            } else if model.table?.id == id {
                onSeated()
            } else {
                model.closeTable()
            }
        }
    }
}

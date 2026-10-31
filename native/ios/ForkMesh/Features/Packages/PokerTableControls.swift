import FMApi
import FMBase
import FMPackages
import SwiftUI

extension View {
    @ViewBuilder func pokerGlass<S: Shape>(_ shape: S) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            self.glassEffect(Glass.regular.tint(Color.black.opacity(0.35)), in: shape)
        } else {
            self.background(.ultraThinMaterial, in: shape)
                .overlay(shape.stroke(Color.white.opacity(0.12), lineWidth: 0.5))
        }
        #else
        self.background(.ultraThinMaterial, in: shape)
            .overlay(shape.stroke(Color.white.opacity(0.12), lineWidth: 0.5))
        #endif
    }
}

struct PokerActionArea: View {
    @Bindable var model: PokerModel
    let table: PokerTable
    let landscape: Bool
    let onRebuy: () -> Void

    var body: some View {
        VStack(spacing: 8) {
            if table.seated, let hand = table.myHand, !hand.name.isEmpty, table.me?.folded != true {
                Text(hand.name)
                    .font(.system(size: 12, weight: .bold))
                    .foregroundStyle(PokerColors.gold)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 3)
                    .background(Capsule().fill(Color.black.opacity(0.55)))
                    .accessibilityLabel("Your hand: \(hand.name)")
            }
            Group {
                if table.myTurn, let legal = table.legal {
                    turn(legal)
                } else {
                    idle
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .pokerGlass(RoundedRectangle(cornerRadius: 22, style: .continuous))
            .environment(\.colorScheme, .dark)
        }
        .padding(.horizontal, 10)
        .padding(.bottom, 4)
        .animation(.spring(response: 0.35, dampingFraction: 0.85), value: table.myTurn)
    }

    @ViewBuilder
    private func turn(_ legal: PokerLegal) -> some View {
        VStack(spacing: 8) {
            if PokerActions.canRaise(legal), let range = legal.raise {
                if landscape {
                    HStack(spacing: 8) {
                        presets
                        slider(range)
                    }
                } else {
                    presets
                    slider(range)
                }
            }
            HStack(spacing: 8) {
                PokerActionButton(title: "Fold", value: "", color: Color(red: 0.62, green: 0.16, blue: 0.14), enabled: legal.fold && !model.busy) {
                    Task { await model.fold() }
                }
                .accessibilityIdentifier("poker-fold")
                PokerActionButton(
                    title: legal.call > 0 ? "Call" : "Check", value: legal.call > 0 ? PokerText.chips(legal.call) : "",
                    color: Color(red: 0.12, green: 0.40, blue: 0.72), enabled: !model.busy
                ) {
                    Task { await model.checkOrCall() }
                }
                .accessibilityIdentifier("poker-call")
                if let range = legal.raise {
                    let amount = model.raiseTo > 0 ? PokerActions.clamp(model.raiseTo, legal) : range.min
                    PokerActionButton(
                        title: table.currentBet > 0 ? "Raise to" : "Bet", value: PokerText.chips(amount),
                        color: Color(red: 0.13, green: 0.50, blue: 0.27), enabled: !model.busy
                    ) {
                        Task { await model.raise() }
                    }
                    .accessibilityIdentifier("poker-raise")
                }
                if legal.allIn > 0 {
                    PokerActionButton(title: "All-in", value: PokerText.chips(legal.allIn), color: PokerColors.goldDeep, enabled: !model.busy) {
                        Task { await model.allIn() }
                    }
                    .accessibilityIdentifier("poker-allin")
                }
            }
        }
    }

    private var presets: some View {
        HStack(spacing: 6) {
            ForEach(PokerActions.presets(table)) { (preset: PokerPreset) in
                Button {
                    model.raiseTo = preset.amount
                    PokerHaptics.tap()
                } label: {
                    Text(preset.label)
                        .font(.system(size: 12, weight: .bold))
                        .foregroundStyle(model.raiseTo == preset.amount ? Color.black : Color.white)
                        .padding(.horizontal, 10)
                        .frame(minHeight: 30)
                        .background(Capsule().fill(model.raiseTo == preset.amount ? PokerColors.gold : Color.white.opacity(0.14)))
                }
                .buttonStyle(.plain)
                .accessibilityLabel("\(preset.label): \(PokerText.chips(preset.amount))")
            }
        }
    }

    private func slider(_ range: PokerRaiseRange) -> some View {
        let step = Double(PokerActions.step(table))
        return Slider(
            value: Binding<Double>(
                get: { Double(model.raiseTo > 0 ? model.raiseTo : range.min) },
                set: { (raw: Double) in
                    let stepped = Int((raw / step).rounded() * step)
                    model.raiseTo = raw >= Double(range.max) - step / 2 ? range.max : min(range.max, max(range.min, stepped))
                }),
            in: Double(range.min)...Double(max(range.max, range.min + 1))
        )
        .tint(PokerColors.gold)
        .accessibilityLabel("Raise amount")
        .accessibilityValue(PokerText.chips(model.raiseTo))
    }

    @ViewBuilder private var idle: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                Text(statusLine)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(.white)
                    .lineLimit(2)
                Text(walletLine)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(Color.white.opacity(0.65))
                    .lineLimit(1)
            }
            Spacer(minLength: 6)
            if let me = table.me {
                if table.canRebuy && (me.stack == 0 || !table.inHand) {
                    pill("Rebuy", systemImage: "plus.circle.fill", action: onRebuy)
                }
                if me.away {
                    pill("I'm back", systemImage: "play.fill") { Task { _ = await model.setAway(false) } }
                } else {
                    pill("Sit out", systemImage: "pause.fill") { Task { _ = await model.setAway(true) } }
                }
            }
        }
        .frame(maxWidth: .infinity)
    }

    private var statusLine: String {
        guard table.seated else {
            return table.openSeats.isEmpty ? "Watching. The table is full." : "Watching. Tap an open seat to sit down."
        }
        if let me = table.me {
            if me.away { return "You're sitting out." }
            if me.busted { return "You're out of chips. Rebuy to keep playing." }
        }
        if table.waiting { return "Waiting for another player." }
        if table.showdown { return "Next hand in a moment." }
        if table.toAct >= 0, table.toAct < table.seats.count {
            let seat = table.seats[table.toAct]
            if !seat.empty { return "\(seat.displayName) is thinking…" }
        }
        return PokerText.phase(table.phase)
    }

    private var walletLine: String {
        var parts = ["Wallet \(PokerText.chips(table.wallet))"]
        if let me = table.me { parts.append("Stack \(PokerText.chips(me.stack))") }
        return parts.joined(separator: " · ")
    }

    private func pill(_ title: String, systemImage: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(size: 13, weight: .bold))
                .foregroundStyle(.white)
                .padding(.horizontal, 12)
                .frame(minHeight: 36)
                .background(Capsule().fill(Color.white.opacity(0.14)))
        }
        .buttonStyle(.plain)
        .disabled(model.busy)
    }
}

struct PokerActionButton: View {
    let title: String
    let value: String
    let color: Color
    let enabled: Bool
    let action: () -> Void

    var body: some View {
        Button {
            PokerHaptics.tap()
            action()
        } label: {
            VStack(spacing: 0) {
                Text(title)
                    .font(.system(size: 14, weight: .heavy))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                if !value.isEmpty {
                    Text(value)
                        .font(.system(size: 12, weight: .bold, design: .rounded))
                        .monospacedDigit()
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                        .opacity(0.9)
                }
            }
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity, minHeight: 48)
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(LinearGradient(colors: [color.opacity(0.95), color.opacity(0.75)], startPoint: .top, endPoint: .bottom))
            )
            .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Color.white.opacity(0.18), lineWidth: 0.8))
        }
        .buttonStyle(.plain)
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
        .accessibilityLabel(value.isEmpty ? title : "\(title) \(value)")
    }
}

struct PokerTableSheetView: View {
    @Bindable var model: PokerModel
    let sheet: PokerTableSheet
    let onDone: () -> Void

    var body: some View {
        Group {
            switch sheet {
            case .sit(let seat): PokerSitSheet(model: model, seat: seat, onDone: onDone)
            case .rebuy: PokerRebuySheet(model: model, onDone: onDone)
            case .chat: PokerChatSheet(model: model)
            case .log: PokerLogSheet(model: model)
            case .invite: PokerInviteSheet(model: model, onDone: onDone)
            }
        }
        .overlay(alignment: .top) { PokerNoticeBanner(model: model) }
    }
}

struct PokerSitSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: PokerModel
    let seat: Int
    let onDone: () -> Void
    @State private var buyIn = 0

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 16) {
                if let table = model.table, let range = PokerBuyIn.range(table) {
                    Text("Blinds \(table.blindsLine). Buy in for \(PokerText.chips(table.minBuyIn)) to \(PokerText.chips(table.maxBuyIn)) chips from your wallet of \(PokerText.chips(table.wallet)).")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    PokerChipSlider(value: $buyIn, range: range, step: PokerBuyIn.step(range))
                    FmFilledButton("Sit down with \(PokerText.chips(buyIn))", fillWidth: true, onPressed: model.busy ? nil : { sit() })
                        .accessibilityIdentifier("poker-sit-confirm")
                } else if let table = model.table {
                    Text("Your wallet has \(PokerText.chips(table.wallet)) chips, below this table's minimum buy-in of \(PokerText.chips(table.minBuyIn)). Collect free chips in the lobby or pick lower stakes.")
                        .font(.subheadline)
                }
                Spacer(minLength: 0)
            }
            .padding(20)
            .navigationTitle("Seat \(seat + 1)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
            .onAppear {
                if let table = model.table, let range = PokerBuyIn.range(table) {
                    buyIn = PokerBuyIn.suggested(big: table.big, range: range)
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func sit() {
        let amount = buyIn
        Task {
            if await model.sit(seat: seat, buyIn: amount) {
                PokerHaptics.tap()
                onDone()
            }
        }
    }
}

struct PokerRebuySheet: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: PokerModel
    let onDone: () -> Void
    @State private var amount = 0

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 16) {
                if let table = model.table, let range = PokerBuyIn.rebuyRange(table) {
                    Text("Top your stack up to at most \(PokerText.chips(table.maxBuyIn)) from your wallet of \(PokerText.chips(table.wallet)). Rebuys happen between hands.")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    PokerChipSlider(value: $amount, range: range, step: PokerBuyIn.step(range), label: "Add")
                    FmFilledButton("Add \(PokerText.chips(amount)) chips", fillWidth: true, onPressed: model.busy ? nil : { rebuy() })
                } else {
                    Text("Your stack is already at the table's maximum, or your wallet is empty.")
                        .font(.subheadline)
                }
                Spacer(minLength: 0)
            }
            .padding(20)
            .navigationTitle("Rebuy")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
            .onAppear {
                if let table = model.table, let range = PokerBuyIn.rebuyRange(table) { amount = range.upperBound }
            }
        }
        .presentationDetents([.medium])
    }

    private func rebuy() {
        let value = amount
        Task {
            if await model.rebuy(value) { onDone() }
        }
    }
}

struct PokerChatSheet: View {
    @Bindable var model: PokerModel
    @State private var draft = ""
    @FocusState private var focused: Bool

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                ScrollViewReader { reader in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 10) {
                            let lines = model.table?.chat ?? []
                            if lines.isEmpty {
                                Text("Say hello to the table.")
                                    .foregroundStyle(.secondary)
                                    .frame(maxWidth: .infinity)
                                    .padding(.top, 40)
                            }
                            ForEach(lines) { (line: PokerChatLine) in
                                VStack(alignment: line.name == model.actor ? .trailing : .leading, spacing: 2) {
                                    Text(line.name)
                                        .font(.caption.weight(.semibold))
                                        .foregroundStyle(.secondary)
                                    Text(line.text)
                                        .padding(.horizontal, 12)
                                        .padding(.vertical, 8)
                                        .background(
                                            RoundedRectangle(cornerRadius: 16, style: .continuous)
                                                .fill(line.name == model.actor ? PokerColors.feltCenter : Color.secondary.opacity(0.18)))
                                        .foregroundStyle(line.name == model.actor ? Color.white : Color.primary)
                                }
                                .frame(maxWidth: .infinity, alignment: line.name == model.actor ? .trailing : .leading)
                                .id(line.id)
                            }
                        }
                        .padding(16)
                    }
                    .onAppear { scrollToEnd(reader) }
                    .onChange(of: model.table?.chat.count ?? 0) { (_: Int, _: Int) in
                        withAnimation { scrollToEnd(reader) }
                    }
                }
                HStack(spacing: 8) {
                    TextField("Message", text: $draft, axis: .vertical)
                        .lineLimit(1...4)
                        .focused($focused)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 9)
                        .background(Capsule().fill(Color.secondary.opacity(0.15)))
                        .onSubmit { send() }
                        .accessibilityIdentifier("poker-chat-field")
                    Button(action: send) {
                        Image(systemName: "arrow.up.circle.fill")
                            .font(.system(size: 30))
                    }
                    .disabled(draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || model.busy)
                    .accessibilityLabel("Send")
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
            }
            .navigationTitle("Table chat")
            .navigationBarTitleDisplayMode(.inline)
        }
        .presentationDetents([.medium, .large])
    }

    private func scrollToEnd(_ reader: ScrollViewProxy) {
        if let last = model.table?.chat.last { reader.scrollTo(last.id, anchor: .bottom) }
    }

    private func send() {
        let text = draft
        guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        Task {
            if await model.sendChat(text) { draft = "" }
        }
    }
}

enum PokerLogTab: String, CaseIterable, Identifiable {
    case hand = "This hand"
    case last = "Last hand"
    case history = "History"

    var id: String { rawValue }
}

struct PokerLogSheet: View {
    @Bindable var model: PokerModel
    @State private var tab: PokerLogTab = .hand

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Picker("Show", selection: $tab) {
                        ForEach(PokerLogTab.allCases) { (item: PokerLogTab) in Text(item.rawValue).tag(item) }
                    }
                    .pickerStyle(.segmented)
                    .listRowBackground(Color.clear)
                }
                if let table = model.table {
                    switch tab {
                    case .hand: hand(table)
                    case .last: last(table)
                    case .history: history(table)
                    }
                }
            }
            .navigationTitle("Hand log")
            .navigationBarTitleDisplayMode(.inline)
        }
        .presentationDetents([.medium, .large])
    }

    @ViewBuilder
    private func hand(_ table: PokerTable) -> some View {
        Section("Hand #\(table.handNumber)") {
            if table.log.isEmpty {
                Text("Nothing has happened yet.").foregroundStyle(.secondary)
            }
            ForEach(table.log.reversed()) { (line: PokerLogLine) in
                HStack(alignment: .firstTextBaseline) {
                    Text(line.text.isEmpty ? PokerText.actionTag(line.kind, amount: line.amount) : line.text)
                    Spacer()
                    Text(model.localDate(line.at), style: .time)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        if !table.commitment.isEmpty {
            Section {
                Text(table.commitment)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
            } header: {
                Text("Deck commitment")
            } footer: {
                Text("Published before the first card: one SHA-256 over the 52 shuffled positions, each sealed with its own salt. When the hand ends the salts of the shown cards are revealed, never folded cards.")
            }
        }
    }

    @ViewBuilder
    private func last(_ table: PokerTable) -> some View {
        if let hand = table.lastHand, hand.number > 0 {
            Section("Hand #\(hand.number)") {
                HStack(spacing: 4) {
                    ForEach(PokerCard.parse(hand.board), id: \.code) { (card: PokerCard) in
                        PokerCardView(card: card, width: 34)
                    }
                }
                .padding(.vertical, 4)
                ForEach(Array(hand.winners.enumerated()), id: \.offset) { (entry: (offset: Int, element: PokerWinner)) in
                    let winner = entry.element
                    VStack(alignment: .leading, spacing: 4) {
                        Text("\(seatName(table, winner.seat, winner.name)) won \(PokerText.chips(winner.amount))")
                            .font(.headline)
                        if !winner.handName.isEmpty { Text(winner.handName).foregroundStyle(.secondary) }
                        if !winner.cards.isEmpty {
                            HStack(spacing: 4) {
                                ForEach(PokerCard.parse(winner.cards), id: \.code) { (card: PokerCard) in
                                    PokerCardView(card: card, width: 28)
                                }
                            }
                        }
                    }
                }
                ForEach(Array(hand.shown.enumerated()), id: \.offset) { (entry: (offset: Int, element: PokerShown)) in
                    let shown = entry.element
                    HStack {
                        Text(seatName(table, shown.seat, shown.name))
                        Spacer()
                        Text(shown.handName).foregroundStyle(.secondary)
                        ForEach(PokerCard.parse(shown.cards), id: \.code) { (card: PokerCard) in
                            PokerCardView(card: card, width: 22)
                        }
                    }
                }
            }
            Section {
                LabeledContent("Commitment") {
                    Text(hand.commitment).font(.system(.caption2, design: .monospaced)).textSelection(.enabled)
                }
                if let reveal = hand.reveal {
                    LabeledContent("Sealed positions") {
                        Text("\(reveal.hashes.count)").monospacedDigit()
                    }
                    if reveal.cards.isEmpty {
                        Text("No cards were shown in this hand.").foregroundStyle(.secondary)
                    }
                    ForEach(reveal.cards) { (shown: PokerRevealedCard) in
                        HStack(spacing: 10) {
                            if let card = shown.parsed {
                                PokerCardView(card: card, width: 22)
                            }
                            VStack(alignment: .leading, spacing: 1) {
                                Text("Position \(shown.index + 1)").font(.subheadline)
                                Text("Salt \(shown.salt)")
                                    .font(.system(.caption2, design: .monospaced))
                                    .foregroundStyle(.secondary)
                                    .textSelection(.enabled)
                            }
                        }
                        .accessibilityElement(children: .combine)
                    }
                }
                switch hand.verified {
                case .some(true):
                    Label("Every shown card sat where the sealed deck said.", systemImage: "checkmark.seal.fill")
                        .foregroundStyle(.green)
                case .some(false):
                    Label("The reveal does not match the sealed deck.", systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.red)
                case .none:
                    Text("Nothing to check yet.").foregroundStyle(.secondary)
                }
            } header: {
                Text("Provably fair")
            } footer: {
                Text("Before the deal the table sealed each of the 52 shuffled positions with its own salt and published one SHA-256 over all of them. After the hand it reveals the salts of the cards that were shown, never folded cards, so you can check each one sat where the deck said.")
            }
        } else {
            Section {
                Text("No hand has finished at this table yet.").foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private func history(_ table: PokerTable) -> some View {
        Section("Recent hands") {
            if table.history.isEmpty {
                Text("No hands yet.").foregroundStyle(.secondary)
            }
            ForEach(table.history.reversed()) { (entry: PokerHistoryEntry) in
                VStack(alignment: .leading, spacing: 2) {
                    HStack {
                        Text("#\(entry.number)").font(.headline).monospacedDigit()
                        Text(entry.winners.joined(separator: ", "))
                        Spacer()
                        Text(PokerText.chips(entry.amount)).monospacedDigit().fontWeight(.semibold)
                    }
                    if !entry.handName.isEmpty {
                        Text(entry.handName).font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    private func seatName(_ table: PokerTable, _ seat: Int, _ name: String) -> String {
        guard table.seats.indices.contains(seat), !table.seats[seat].empty, table.seats[seat].name == name else { return name }
        return table.seats[seat].displayName
    }
}

struct PokerInviteSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: PokerModel
    let onDone: () -> Void
    @State private var names = ""

    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 16) {
                Text("Type ForkMesh account names, separated by commas. Each one gets a ping with a link to this table.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                TextField("alice, bob", text: $names)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .padding(12)
                    .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color.secondary.opacity(0.15)))
                    .accessibilityIdentifier("poker-invite-field")
                if let code = model.table?.code {
                    Text("Or share the code \(code).").font(.footnote).foregroundStyle(.secondary)
                }
                FmFilledButton(
                    "Send invitations", fillWidth: true, busy: model.busy,
                    onPressed: PokerModel.inviteNames(names).isEmpty || model.busy ? nil : { invite() })
                Spacer(minLength: 0)
            }
            .padding(20)
            .navigationTitle("Invite players")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } } }
        }
        .presentationDetents([.medium])
    }

    private func invite() {
        let text = names
        Task {
            if await model.invite(text) { onDone() }
        }
    }
}

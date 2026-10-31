import FMApi
import FMBase
import FMPackages
import SwiftUI
import UIKit

enum PokerTableSheet: Identifiable, Equatable {
    case sit(Int)
    case rebuy
    case chat
    case log
    case invite

    var id: String {
        switch self {
        case .sit(let seat): return "sit-\(seat)"
        case .rebuy: return "rebuy"
        case .chat: return "chat"
        case .log: return "log"
        case .invite: return "invite"
        }
    }
}

struct PokerTableScreen: View {
    @Environment(\.scenePhase) private var scenePhase
    @Bindable var model: PokerModel
    let onClose: () -> Void
    @State private var sheet: PokerTableSheet?
    @State private var confirmLeave = false
    @State private var confirmClose = false
    @State private var chatSeen: Int64 = 0
    @AppStorage(PokerSound.mutedKey) private var muted = false

    var body: some View {
        GeometryReader { geometry in
            let landscape = geometry.size.width > geometry.size.height
            let top: CGFloat = 52
            let bottom: CGFloat = bottomHeight(landscape: landscape)
            let stage = CGSize(width: geometry.size.width, height: max(200, geometry.size.height - top - bottom))
            ZStack(alignment: .top) {
                VStack(spacing: 0) {
                    Color.clear.frame(height: top)
                    if let table = model.table {
                        PokerStage(model: model, table: table, size: stage, landscape: landscape) { (seat: Int) in
                            sheet = .sit(seat)
                        }
                        .frame(width: stage.width, height: stage.height)
                    } else {
                        placeholder.frame(width: stage.width, height: stage.height)
                    }
                    Spacer(minLength: 0)
                }
                topBar
                VStack {
                    Spacer()
                    if let table = model.table {
                        PokerActionArea(model: model, table: table, landscape: landscape) { sheet = .rebuy }
                            .frame(maxWidth: landscape ? 720 : .infinity)
                    }
                }
            }
        }
        .background(
            RadialGradient(colors: [PokerColors.roomCenter, PokerColors.roomEdge], center: .center, startRadius: 40, endRadius: 700)
                .ignoresSafeArea()
        )
        .overlay(alignment: .top) { PokerNoticeBanner(model: model).padding(.top, 52) }
        .preferredColorScheme(.dark)
        .persistentSystemOverlays(.hidden)
        .sheet(item: $sheet) { (which: PokerTableSheet) in
            PokerTableSheetView(model: model, sheet: which) { sheet = nil }
        }
        .confirmationDialog("Leave this table?", isPresented: $confirmLeave, titleVisibility: .visible) {
            Button("Leave table", role: .destructive) {
                Task {
                    if await model.leave() { onClose() }
                }
            }
        } message: {
            Text("Your stack goes back to your wallet. Leaving during a hand folds it, and your chips are cashed out when it ends.")
        }
        .confirmationDialog("Close this table for everyone?", isPresented: $confirmClose, titleVisibility: .visible) {
            Button("Close table", role: .destructive) {
                Task {
                    if await model.closeForEveryone() { onClose() }
                }
            }
        } message: {
            Text("Every stack goes back to its owner's wallet.")
        }
        .onAppear {
            FmOrientationLock.allowLandscape(true)
            UIApplication.shared.isIdleTimerDisabled = true
            PokerHub.shared.hold(model)
            PokerHub.shared.visible = true
            PokerSound.shared.warmUp()
            model.appeared()
            chatSeen = model.table?.chat.last?.at ?? 0
        }
        .onDisappear {
            FmOrientationLock.allowLandscape(false)
            UIApplication.shared.isIdleTimerDisabled = false
            model.disappeared()
            PokerSound.shared.stop()
        }
        .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
            switch phase {
            case .active: model.catchUp()
            case .background: model.disappeared()
            default: break
            }
        }
        .onChange(of: model.table?.id) { (_: String?, _: String?) in
            chatSeen = model.table?.chat.last?.at ?? 0
        }
        .onChange(of: sheet) { (_: PokerTableSheet?, next: PokerTableSheet?) in
            if next == .chat { chatSeen = model.table?.chat.last?.at ?? chatSeen }
        }
    }

    private func bottomHeight(landscape: Bool) -> CGFloat {
        guard let table = model.table else { return 60 }
        if table.myTurn {
            let raises = table.legal.map(PokerActions.canRaise) ?? false
            return landscape ? (raises ? 104 : 64) : (raises ? 150 : 88)
        }
        return landscape ? 56 : 78
    }

    @ViewBuilder private var placeholder: some View {
        if model.tableError.isEmpty {
            ProgressView()
                .tint(.white)
                .controlSize(.large)
        } else {
            VStack(spacing: 14) {
                Image(systemName: "suit.spade")
                    .font(.system(size: 40, weight: .semibold))
                    .foregroundStyle(Color.white.opacity(0.5))
                Text(model.tableError)
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(.white)
                    .multilineTextAlignment(.center)
                Button("Back to the lobby", action: onClose)
                    .buttonStyle(.borderedProminent)
                    .tint(PokerColors.goldDeep)
            }
            .padding(24)
        }
    }

    private var unreadChat: Bool {
        guard let last = model.table?.chat.last else { return false }
        return last.at > chatSeen && last.name != model.actor
    }

    private var topBar: some View {
        HStack(spacing: 8) {
            barButton("chevron.down", label: "Back to the lobby", action: onClose)
                .accessibilityIdentifier("poker-table-close")
            VStack(alignment: .leading, spacing: 1) {
                Text(model.table?.name ?? "Poker")
                    .font(.system(size: 16, weight: .bold))
                    .foregroundStyle(.white)
                    .lineLimit(1)
                if let table = model.table {
                    Text("Blinds \(table.blindsLine) · Hand #\(table.handNumber) · \(table.code)")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(Color.white.opacity(0.65))
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 4)
            barButton(muted ? "speaker.slash.fill" : "speaker.wave.2.fill", label: muted ? "Turn sounds on" : "Mute sounds") {
                muted.toggle()
                PokerHaptics.tap()
            }
            ZStack(alignment: .topTrailing) {
                barButton("bubble.left.and.bubble.right.fill", label: "Table chat") { sheet = .chat }
                if unreadChat {
                    Circle().fill(PokerColors.clockRed).frame(width: 10, height: 10).offset(x: -2, y: 2)
                }
            }
            barButton("list.bullet.rectangle", label: "Hand log") { sheet = .log }
            menu
        }
        .padding(.horizontal, 12)
        .frame(height: 52)
    }

    private var menu: some View {
        Menu {
            if let table = model.table {
                if table.seated {
                    if table.me?.away == true {
                        Button("I'm back", systemImage: "play.fill") { Task { _ = await model.setAway(false) } }
                    } else {
                        Button("Sit out", systemImage: "pause.fill") { Task { _ = await model.setAway(true) } }
                    }
                    if table.canRebuy {
                        Button("Rebuy", systemImage: "plus.circle") { sheet = .rebuy }
                    }
                }
                if table.ownerIsMe {
                    Section("Owner") {
                        if !table.openSeats.isEmpty {
                            Button("Add a bot", systemImage: "cpu") { Task { _ = await model.addBot() } }
                        }
                        let bots = table.seats.filter { !$0.empty && $0.bot }
                        if !bots.isEmpty {
                            Menu("Remove a bot") {
                                ForEach(bots) { (seat: PokerSeat) in
                                    Button(seat.displayName) { Task { _ = await model.removeBot(seat: seat.index) } }
                                }
                            }
                        }
                        Button("Invite players", systemImage: "person.badge.plus") { sheet = .invite }
                    }
                }
                if let url = URL(string: "https://app.forkmesh.com/dashboard/poker?table=\(table.id)") {
                    ShareLink(item: url, subject: Text("Poker: \(table.name)"), message: Text("Join my poker table on ForkMesh. Code \(table.code)")) {
                        Label("Share table", systemImage: "square.and.arrow.up")
                    }
                }
                Button("Copy code \(table.code)", systemImage: "doc.on.doc") {
                    UIPasteboard.general.string = table.code
                    model.notice = "Code copied."
                }
                if table.seated {
                    Button("Leave table", systemImage: "rectangle.portrait.and.arrow.right", role: .destructive) { confirmLeave = true }
                }
                if table.ownerIsMe {
                    Button("Close table", systemImage: "xmark.octagon", role: .destructive) { confirmClose = true }
                }
            }
        } label: {
            barIcon("ellipsis")
        }
        .accessibilityLabel("Table options")
        .accessibilityIdentifier("poker-table-menu")
    }

    private func barButton(_ symbol: String, label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) { barIcon(symbol) }
            .buttonStyle(.plain)
            .accessibilityLabel(label)
    }

    private func barIcon(_ symbol: String) -> some View {
        Image(systemName: symbol)
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: 38, height: 38)
            .background(Circle().fill(.ultraThinMaterial))
            .overlay(Circle().strokeBorder(Color.white.opacity(0.14), lineWidth: 0.5))
            .environment(\.colorScheme, .dark)
    }
}

struct PokerStage: View {
    let model: PokerModel
    let table: PokerTable
    let size: CGSize
    let landscape: Bool
    let onSeat: (Int) -> Void

    var body: some View {
        let geometry = PokerStageGeometry(size: size, landscape: landscape, seatCount: table.seatCount, mySeat: table.mySeat)
        let spring: Animation? = model.animates ? .spring(response: 0.5, dampingFraction: 0.82) : nil
        ZStack {
            PokerFelt(size: geometry.table, rail: geometry.rail)
                .position(geometry.center)
            PokerDealerFigure(line: table.dealerLine, width: geometry.dealerWidth)
                .position(geometry.dealerSpot)
            pot(geometry)
            board(geometry)
            winnerBanner(geometry)
            ForEach(table.seats) { (seat: PokerSeat) in
                bet(seat, geometry)
            }
            ForEach(table.winners) { (seat: PokerSeat) in
                payout(seat, geometry)
            }
            if table.dealer >= 0, table.dealer < table.seats.count, !table.waiting {
                PokerDealerButton(size: geometry.puck)
                    .position(geometry.puckPoint(table.dealer))
                    .animation(.spring(response: 0.7, dampingFraction: 0.8), value: table.dealer)
            }
            ForEach(table.seats) { (seat: PokerSeat) in
                holeCards(seat, geometry)
            }
            ForEach(table.seats) { (seat: PokerSeat) in
                PokerSeatView(
                    model: model, table: table, seat: seat, diameter: geometry.avatar,
                    mine: seat.index == table.mySeat && table.seated, onSit: { onSeat(seat.index) })
                    .position(geometry.seatPoint(seat.index))
            }
        }
        .frame(width: size.width, height: size.height)
        .animation(spring, value: model.eventBatch)
        .animation(.easeInOut(duration: 0.3), value: table.dealerLine)
    }

    @ViewBuilder
    private func pot(_ geometry: PokerStageGeometry) -> some View {
        let amount = table.potInMiddle
        if amount > 0 && !(table.showdown && !table.winners.isEmpty) {
            VStack(spacing: 3) {
                PokerChipPile(amount: amount, size: geometry.chip * 1.15)
                Text("Pot \(PokerText.chips(table.potTotal))")
                    .font(.system(size: 13, weight: .heavy, design: .rounded))
                    .monospacedDigit()
                    .foregroundStyle(.white)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(Color.black.opacity(0.45)))
                if table.pots.count > 1 {
                    Text(table.pots.dropFirst().map { "Side \(PokerText.short($0.amount))" }.joined(separator: " · "))
                        .font(.system(size: 10, weight: .semibold))
                        .foregroundStyle(Color.white.opacity(0.8))
                }
            }
            .position(geometry.potSpot)
            .transition(.opacity)
            .accessibilityElement(children: .combine)
        }
    }

    private func board(_ geometry: PokerStageGeometry) -> some View {
        let winning = table.winningCards
        let cards = table.boardCards
        return HStack(spacing: geometry.boardCard * 0.12) {
            ForEach(0..<5, id: \.self) { (index: Int) in
                ZStack {
                    RoundedRectangle(cornerRadius: geometry.boardCard * 0.12, style: .continuous)
                        .strokeBorder(Color.white.opacity(0.12), lineWidth: 1)
                        .frame(width: geometry.boardCard, height: geometry.boardCard * 1.4)
                    if index < cards.count {
                        let card = cards[index]
                        PokerCardView(
                            card: card, width: geometry.boardCard, lifted: winning.contains(card.code),
                            dimmed: !winning.isEmpty && !winning.contains(card.code), dealt: model.animates)
                            .id("board-\(table.handNumber)-\(index)")
                            .transition(dealTransition(from: geometry.dealerSpot, to: geometry.boardPoint(index), delay: Double(index % 3) * 0.08))
                    }
                }
            }
        }
        .position(geometry.boardSpot)
    }

    @ViewBuilder
    private func winnerBanner(_ geometry: PokerStageGeometry) -> some View {
        if table.showdown, let line = winnerLine {
            Text(line)
                .font(.system(size: landscape ? 15 : 14, weight: .heavy))
                .foregroundStyle(Color(red: 0.16, green: 0.11, blue: 0))
                .multilineTextAlignment(.center)
                .lineLimit(2)
                .padding(.horizontal, 14)
                .padding(.vertical, 6)
                .background(Capsule().fill(LinearGradient(colors: [PokerColors.gold, PokerColors.goldDeep], startPoint: .top, endPoint: .bottom)))
                .shadow(color: PokerColors.gold.opacity(0.6), radius: 10)
                .frame(maxWidth: geometry.table.width * 0.7)
                .position(geometry.bannerSpot)
                .transition(.scale(scale: 0.6).combined(with: .opacity))
        }
    }

    private var winnerLine: String? {
        if let hand = table.lastHand, hand.number == table.handNumber, !hand.winners.isEmpty {
            return hand.winners.map { (winner: PokerWinner) -> String in
                let name = table.seats.indices.contains(winner.seat) ? table.seats[winner.seat].displayName : winner.name
                let what = winner.handName.isEmpty ? "" : " with \(winner.handName)"
                return "\(name) wins \(PokerText.chips(winner.amount))\(what)"
            }.joined(separator: "\n")
        }
        let winners = table.winners
        guard !winners.isEmpty else { return nil }
        return winners.map { (seat: PokerSeat) -> String in
            let what = seat.handName.isEmpty ? "" : " with \(seat.handName)"
            return "\(seat.displayName) wins \(PokerText.chips(seat.won))\(what)"
        }.joined(separator: "\n")
    }

    @ViewBuilder
    private func bet(_ seat: PokerSeat, _ geometry: PokerStageGeometry) -> some View {
        if !seat.empty && seat.bet > 0 {
            let at = geometry.betPoint(seat.index)
            PokerBetStack(amount: seat.bet, size: geometry.chip)
                .id("bet-\(table.handNumber)-\(seat.index)")
                .position(at)
                .transition(.asymmetric(
                    insertion: .offset(x: geometry.seatPoint(seat.index).x - at.x, y: geometry.seatPoint(seat.index).y - at.y)
                        .combined(with: .opacity),
                    removal: .offset(x: geometry.potSpot.x - at.x, y: geometry.potSpot.y - at.y).combined(with: .opacity)))
        }
    }

    @ViewBuilder
    private func payout(_ seat: PokerSeat, _ geometry: PokerStageGeometry) -> some View {
        if table.showdown && seat.won > 0 {
            let at = geometry.betPoint(seat.index)
            PokerBetStack(amount: seat.won, size: geometry.chip)
                .id("won-\(table.handNumber)-\(seat.index)")
                .position(at)
                .transition(.asymmetric(
                    insertion: .offset(x: geometry.potSpot.x - at.x, y: geometry.potSpot.y - at.y),
                    removal: .offset(x: geometry.seatPoint(seat.index).x - at.x, y: geometry.seatPoint(seat.index).y - at.y)
                        .combined(with: .opacity)))
        }
    }

    @ViewBuilder
    private func holeCards(_ seat: PokerSeat, _ geometry: PokerStageGeometry) -> some View {
        let cards = seat.parsedCards
        if !seat.empty && !cards.isEmpty && !seat.folded {
            let mine = seat.index == table.mySeat && table.seated
            let width = mine ? geometry.myCard : geometry.smallCard
            let at = geometry.cardsPoint(seat.index, mine: mine)
            let winning = table.winningCards
            let slot = PokerLayout.slot(of: seat.index, seatCount: table.seatCount, mySeat: table.mySeat)
            HStack(spacing: mine ? width * 0.08 : -width * 0.45) {
                ForEach(Array(cards.enumerated()), id: \.offset) { (entry: (offset: Int, element: PokerCard)) in
                    PokerCardView(
                        card: entry.element, width: width, lifted: winning.contains(entry.element.code),
                        dimmed: false, dealt: model.animates)
                        .rotationEffect(.degrees(mine ? (entry.offset == 0 ? -4 : 4) : (entry.offset == 0 ? -8 : 8)))
                        .id("hole-\(table.handNumber)-\(seat.index)-\(entry.offset)-\(entry.element.faceDown ? "down" : "up")")
                        .transition(
                            mine || entry.element.faceDown
                                ? dealTransition(from: geometry.dealerSpot, to: at, delay: Double(entry.offset * table.seatCount + slot) * 0.06)
                                : .opacity)
                }
            }
            .position(at)
            .zIndex(mine ? 2 : 1)
        }
    }

    private func dealTransition(from: CGPoint, to: CGPoint, delay: Double) -> AnyTransition {
        guard model.animates else { return .identity }
        return AnyTransition.asymmetric(
            insertion: AnyTransition.offset(x: from.x - to.x, y: from.y - to.y)
                .combined(with: .scale(scale: 0.4))
                .animation(.spring(response: 0.45, dampingFraction: 0.85).delay(delay)),
            removal: .opacity)
    }
}

struct PokerStageGeometry {
    let size: CGSize
    let landscape: Bool
    let seatCount: Int
    let mySeat: Int
    let table: CGSize
    let center: CGPoint
    let rail: CGFloat
    let avatar: CGFloat
    let positions: [Int: PokerPoint]

    init(size: CGSize, landscape: Bool, seatCount: Int, mySeat: Int) {
        self.size = size
        self.landscape = landscape
        self.seatCount = seatCount
        self.mySeat = mySeat
        let width: CGFloat
        let height: CGFloat
        if landscape {
            height = size.height * 0.80
            width = min(size.width * 0.80, height * 2.1)
        } else {
            width = size.width * 0.80
            height = min(size.height * 0.84, width * 1.75)
        }
        table = CGSize(width: width, height: height)
        center = CGPoint(x: size.width / 2, y: size.height / 2 + (landscape ? 4 : 10))
        rail = max(14, min(width, height) * 0.075)
        avatar = max(34, min(52, min(width, height) * 0.13))
        let inset = rail * 0.5
        positions = PokerLayout.positions(
            seatCount: seatCount, mySeat: mySeat, center: PokerPoint(x: Double(center.x), y: Double(center.y)),
            radiusX: Double(width / 2 - inset), radiusY: Double(height / 2 - inset))
    }

    var dealerWidth: CGFloat { max(90, min(150, table.width * 0.26)) }
    var puck: CGFloat { max(22, avatar * 0.55) }
    var chip: CGFloat { max(13, avatar * 0.36) }
    var boardCard: CGFloat { max(30, min(56, (table.width - rail * 2) * (landscape ? 0.085 : 0.12))) }
    var smallCard: CGFloat { max(20, avatar * 0.58) }
    var myCard: CGFloat { max(40, min(64, avatar * 1.15)) }

    var dealerSpot: CGPoint { CGPoint(x: center.x, y: center.y - table.height / 2 + rail + dealerWidth * 0.2) }
    var boardSpot: CGPoint { CGPoint(x: center.x, y: center.y + (landscape ? 4 : 0)) }
    var potSpot: CGPoint { CGPoint(x: center.x, y: center.y - boardCard * 1.4 * 0.5 - (landscape ? 30 : 38)) }
    var bannerSpot: CGPoint { CGPoint(x: center.x, y: center.y + boardCard * 0.7 + (landscape ? 24 : 32)) }

    func boardPoint(_ index: Int) -> CGPoint {
        let step = boardCard * 1.12
        return CGPoint(x: boardSpot.x + CGFloat(index - 2) * step, y: boardSpot.y)
    }

    func seatPoint(_ seat: Int) -> CGPoint {
        guard let point = positions[seat] else { return center }
        return CGPoint(x: point.x, y: point.y)
    }

    func toward(_ seat: Int, _ fraction: CGFloat) -> CGPoint {
        let from = seatPoint(seat)
        return CGPoint(x: from.x + (center.x - from.x) * fraction, y: from.y + (center.y - from.y) * fraction)
    }

    func betPoint(_ seat: Int) -> CGPoint {
        let mine = seat == mySeat
        return toward(seat, mine ? 0.44 : 0.4)
    }

    func cardsPoint(_ seat: Int, mine: Bool) -> CGPoint {
        if mine {
            let point = seatPoint(seat)
            return CGPoint(x: point.x + avatar * 0.6 + myCard * 1.05, y: point.y - myCard * 0.25)
        }
        return toward(seat, 0.2)
    }

    func puckPoint(_ seat: Int) -> CGPoint {
        let base = toward(seat, 0.3)
        let from = seatPoint(seat)
        let dx = center.x - from.x
        let dy = center.y - from.y
        let length = max(1, sqrt(dx * dx + dy * dy))
        let side = puck * 1.1
        return CGPoint(x: base.x - dy / length * side, y: base.y + dx / length * side)
    }
}

struct PokerSeatView: View {
    let model: PokerModel
    let table: PokerTable
    let seat: PokerSeat
    let diameter: CGFloat
    let mine: Bool
    let onSit: () -> Void

    var body: some View {
        if seat.empty {
            empty
        } else {
            occupied
        }
    }

    @ViewBuilder private var empty: some View {
        if !table.seated {
            Button(action: onSit) {
                VStack(spacing: 3) {
                    Circle()
                        .strokeBorder(Color.white.opacity(0.55), style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
                        .background(Circle().fill(Color.black.opacity(0.35)))
                        .overlay(Image(systemName: "plus").font(.system(size: diameter * 0.34, weight: .bold)).foregroundStyle(.white))
                        .frame(width: diameter, height: diameter)
                    Text("Sit here")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(Color.white.opacity(0.85))
                }
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Sit in seat \(seat.index + 1)")
            .accessibilityIdentifier("poker-sit-\(seat.index)")
        } else {
            Circle()
                .strokeBorder(Color.white.opacity(0.18), style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                .frame(width: diameter * 0.8, height: diameter * 0.8)
                .accessibilityHidden(true)
        }
    }

    private var occupied: some View {
        VStack(spacing: 2) {
            tag
            PokerAvatar(seat: seat, model: model, table: table, diameter: diameter)
            VStack(spacing: 0) {
                Text(seat.displayName)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(.white)
                    .lineLimit(1)
                    .truncationMode(.tail)
                Text(PokerText.chips(seat.stack))
                    .font(.system(size: 12, weight: .heavy, design: .rounded))
                    .monospacedDigit()
                    .foregroundStyle(PokerColors.gold)
                    .contentTransition(.numericText())
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .frame(maxWidth: diameter * 2.1)
            .background(
                Capsule().fill(Color.black.opacity(0.7))
                    .overlay(Capsule().strokeBorder(seat.isTurn ? PokerColors.clockGreen : (mine ? PokerColors.gold.opacity(0.7) : Color.white.opacity(0.15)), lineWidth: seat.isTurn ? 1.5 : 0.8)))
        }
        .contextMenu {
            if table.ownerIsMe && seat.bot {
                Button("Remove \(seat.displayName)", systemImage: "person.fill.xmark", role: .destructive) {
                    Task { _ = await model.removeBot(seat: seat.index) }
                }
            }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(accessibility)
    }

    @ViewBuilder private var tag: some View {
        let status = PokerText.status(seat)
        let action = seat.lastAction?.label ?? ""
        let text = !status.isEmpty && status != "Next hand" ? status : action
        if !text.isEmpty {
            Text(text)
                .font(.system(size: 10, weight: .heavy))
                .foregroundStyle(tagColor(text))
                .padding(.horizontal, 7)
                .padding(.vertical, 2)
                .background(Capsule().fill(Color.black.opacity(0.65)))
                .transition(.scale.combined(with: .opacity))
                .id(text)
        } else {
            Color.clear.frame(height: 14)
        }
    }

    private func tagColor(_ text: String) -> Color {
        if text.hasPrefix("All-in") { return PokerColors.clockRed }
        if text.hasPrefix("Raise") || text.hasPrefix("Bet") { return PokerColors.clockAmber }
        if text.hasPrefix("Fold") || text.hasPrefix("Sitting") || text.hasPrefix("Busted") { return Color.white.opacity(0.6) }
        return PokerColors.clockGreen
    }

    private var accessibility: String {
        var parts = [seat.displayName, "\(PokerText.chips(seat.stack)) chips"]
        let status = PokerText.status(seat)
        if !status.isEmpty { parts.append(status) }
        if let action = seat.lastAction, !action.kind.isEmpty { parts.append(action.label) }
        if seat.isTurn { parts.append("to act") }
        if seat.winner { parts.append("winner") }
        if seat.index == table.dealer { parts.append("dealer") }
        return parts.joined(separator: ", ")
    }
}

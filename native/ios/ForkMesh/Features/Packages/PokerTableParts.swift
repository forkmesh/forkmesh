import FMPackages
import SwiftUI

enum PokerColors {
    static let feltCenter = Color(red: 0x0d / 255, green: 0x5c / 255, blue: 0x3a / 255)
    static let feltEdge = Color(red: 0x08 / 255, green: 0x3d / 255, blue: 0x27 / 255)
    static let gold = Color(red: 0.86, green: 0.70, blue: 0.30)
    static let goldDeep = Color(red: 0.66, green: 0.49, blue: 0.16)
    static let walnut = Color(red: 0.36, green: 0.21, blue: 0.10)
    static let walnutLight = Color(red: 0.55, green: 0.34, blue: 0.17)
    static let walnutDark = Color(red: 0.20, green: 0.11, blue: 0.05)
    static let leather = Color(red: 0.09, green: 0.09, blue: 0.09)
    static let roomCenter = Color(red: 0.13, green: 0.10, blue: 0.08)
    static let roomEdge = Color(red: 0.02, green: 0.02, blue: 0.02)
    static let cardRed = Color(red: 0xc6 / 255, green: 0x28 / 255, blue: 0x28 / 255)
    static let cardBlack = Color(red: 0x1a / 255, green: 0x1a / 255, blue: 0x1a / 255)
    static let cardBack = Color(red: 0x1e / 255, green: 0x3a / 255, blue: 0x8a / 255)
    static let clockGreen = Color(red: 0.30, green: 0.85, blue: 0.45)
    static let clockAmber = Color(red: 0.98, green: 0.70, blue: 0.15)
    static let clockRed = Color(red: 0.95, green: 0.26, blue: 0.21)

    static func hex(_ text: String) -> Color {
        var digits = text
        if digits.hasPrefix("#") { digits.removeFirst() }
        let value = UInt32(digits, radix: 16) ?? 0xFFFFFF
        return Color(
            red: Double((value >> 16) & 0xFF) / 255, green: Double((value >> 8) & 0xFF) / 255,
            blue: Double(value & 0xFF) / 255)
    }

    static func chip(_ denomination: Int) -> Color { hex(PokerChips.colorHex(denomination)) }

    static func clock(_ tone: PokerClockTone) -> Color {
        switch tone {
        case .green: return clockGreen
        case .amber: return clockAmber
        case .red: return clockRed
        }
    }
}

struct PokerCardView: View {
    let card: PokerCard
    var width: CGFloat = 44
    var lifted = false
    var dimmed = false
    var dealt = false
    @State private var angle: Double

    init(card: PokerCard, width: CGFloat = 44, lifted: Bool = false, dimmed: Bool = false, dealt: Bool = false) {
        self.card = card
        self.width = width
        self.lifted = lifted
        self.dimmed = dimmed
        self.dealt = dealt
        _angle = State(initialValue: dealt && !card.faceDown ? 180 : 0)
    }

    private var height: CGFloat { width * 1.4 }

    var body: some View {
        Group {
            if card.faceDown {
                PokerCardBack(width: width)
            } else {
                PokerFlip(angle: angle, width: width, face: face)
            }
        }
        .frame(width: width, height: height)
        .offset(y: lifted ? -width * 0.22 : 0)
        .shadow(color: lifted ? PokerColors.gold.opacity(0.9) : Color.black.opacity(0.35), radius: lifted ? 8 : 2, y: lifted ? 0 : 1)
        .opacity(dimmed ? 0.45 : 1)
        .animation(.spring(response: 0.35, dampingFraction: 0.7), value: lifted)
        .onAppear {
            guard angle != 0 else { return }
            withAnimation(.easeOut(duration: 0.34).delay(0.22)) { angle = 0 }
        }
        .accessibilityElement()
        .accessibilityLabel(card.spokenName)
    }

    private var tint: Color { card.isRed ? PokerColors.cardRed : PokerColors.cardBlack }

    private var face: some View {
        let radius = width * 0.12
        return ZStack {
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .fill(LinearGradient(colors: [.white, Color(white: 0.94)], startPoint: .top, endPoint: .bottom))
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .strokeBorder(Color.black.opacity(0.18), lineWidth: 0.6)
            VStack(spacing: -width * 0.04) {
                Text(card.rankLabel)
                    .font(.system(size: width * 0.30, weight: .bold, design: .serif))
                Text(card.suit?.symbol ?? "")
                    .font(.system(size: width * 0.24))
            }
            .foregroundStyle(tint)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            .padding(.leading, width * 0.07)
            .padding(.top, width * 0.04)
            VStack(spacing: -width * 0.04) {
                Text(card.rankLabel)
                    .font(.system(size: width * 0.30, weight: .bold, design: .serif))
                Text(card.suit?.symbol ?? "")
                    .font(.system(size: width * 0.24))
            }
            .foregroundStyle(tint)
            .rotationEffect(.degrees(180))
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
            .padding(.trailing, width * 0.07)
            .padding(.bottom, width * 0.04)
            Text(card.suit?.symbol ?? "")
                .font(.system(size: width * 0.58))
                .foregroundStyle(tint)
                .offset(y: width * 0.03)
        }
    }
}

struct PokerFlip<Face: View>: View, Animatable {
    var angle: Double
    let width: CGFloat
    let face: Face

    nonisolated var animatableData: Double {
        get { angle }
        set { angle = newValue }
    }

    var body: some View {
        ZStack {
            if angle < 90 {
                face
            } else {
                PokerCardBack(width: width)
                    .rotation3DEffect(.degrees(180), axis: (x: 0, y: 1, z: 0))
            }
        }
        .rotation3DEffect(.degrees(angle), axis: (x: 0, y: 1, z: 0), perspective: 0.6)
    }
}

struct PokerCardBack: View {
    let width: CGFloat

    var body: some View {
        let radius = width * 0.12
        ZStack {
            RoundedRectangle(cornerRadius: radius, style: .continuous).fill(Color.white)
            RoundedRectangle(cornerRadius: radius * 0.7, style: .continuous)
                .fill(PokerColors.cardBack)
                .overlay(
                    Canvas { context, size in
                        let step = max(4, size.width / 7)
                        var path = Path()
                        var x = -size.height
                        while x < size.width + size.height {
                            path.move(to: CGPoint(x: x, y: 0))
                            path.addLine(to: CGPoint(x: x + size.height, y: size.height))
                            path.move(to: CGPoint(x: x + size.height, y: 0))
                            path.addLine(to: CGPoint(x: x, y: size.height))
                            x += step
                        }
                        context.stroke(path, with: .color(Color.white.opacity(0.28)), lineWidth: 0.7)
                    }
                    .clipShape(RoundedRectangle(cornerRadius: radius * 0.7, style: .continuous))
                )
                .overlay(
                    RoundedRectangle(cornerRadius: radius * 0.5, style: .continuous)
                        .strokeBorder(Color.white.opacity(0.35), lineWidth: 0.6)
                        .padding(width * 0.06)
                )
                .padding(width * 0.06)
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .strokeBorder(Color.black.opacity(0.2), lineWidth: 0.6)
        }
    }
}

struct PokerChip: View {
    let denomination: Int
    var size: CGFloat = 22

    var body: some View {
        let color = PokerColors.chip(denomination)
        ZStack {
            Circle().fill(color)
            Circle()
                .strokeBorder(Color.white, style: StrokeStyle(lineWidth: size * 0.16, dash: [size * 0.2, size * 0.2]))
            Circle()
                .inset(by: size * 0.2)
                .fill(color)
            Circle()
                .inset(by: size * 0.2)
                .strokeBorder(Color.white.opacity(0.75), lineWidth: size * 0.04)
            Circle()
                .strokeBorder(Color.black.opacity(0.35), lineWidth: 0.6)
        }
        .frame(width: size, height: size)
        .shadow(color: .black.opacity(0.35), radius: 1, y: 1)
    }
}

struct PokerChipPile: View {
    let amount: Int
    var size: CGFloat = 18

    var body: some View {
        let columns = PokerChips.columns(amount, maxColumns: 3, perColumn: 6)
        HStack(alignment: .bottom, spacing: -size * 0.25) {
            ForEach(Array(columns.enumerated()), id: \.offset) { (entry: (offset: Int, element: [Int])) in
                ZStack(alignment: .bottom) {
                    ForEach(Array(entry.element.enumerated()), id: \.offset) { (chip: (offset: Int, element: Int)) in
                        PokerChip(denomination: chip.element, size: size)
                            .scaleEffect(x: 1, y: 0.62)
                            .offset(y: -CGFloat(chip.offset) * size * 0.16)
                    }
                }
                .frame(width: size, height: size * 0.62 + CGFloat(max(0, entry.element.count - 1)) * size * 0.16, alignment: .bottom)
            }
        }
    }
}

struct PokerBetStack: View {
    let amount: Int
    var size: CGFloat = 16

    var body: some View {
        VStack(spacing: 2) {
            PokerChipPile(amount: amount, size: size)
            Text(PokerText.short(amount))
                .font(.system(size: max(10, size * 0.7), weight: .bold, design: .rounded))
                .monospacedDigit()
                .foregroundStyle(.white)
                .padding(.horizontal, 6)
                .padding(.vertical, 1)
                .background(Capsule().fill(Color.black.opacity(0.55)))
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(PokerText.chips(amount)) chips")
    }
}

struct PokerDealerButton: View {
    var size: CGFloat = 26

    var body: some View {
        ZStack {
            Circle().fill(LinearGradient(colors: [.white, Color(white: 0.86)], startPoint: .top, endPoint: .bottom))
            Circle().strokeBorder(Color.black.opacity(0.25), lineWidth: 0.8)
            Circle().inset(by: size * 0.1).strokeBorder(Color.black.opacity(0.12), lineWidth: 0.6)
            Text("DEALER")
                .font(.system(size: size * 0.22, weight: .heavy))
                .foregroundStyle(Color.black.opacity(0.85))
                .minimumScaleFactor(0.5)
        }
        .frame(width: size, height: size)
        .shadow(color: .black.opacity(0.45), radius: 2, y: 2)
        .accessibilityLabel("Dealer button")
    }
}

struct PokerTurnRing: View {
    let model: PokerModel
    let deadline: Int64
    let started: Int64
    let diameter: CGFloat

    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.25)) { _ in
            let remaining = max(0, deadline - model.serverNow)
            let total = max(1, deadline - started)
            let tone = PokerClock.tone(remainingMs: remaining)
            Circle()
                .trim(from: 0, to: PokerClock.fraction(remainingMs: remaining, totalMs: total))
                .stroke(PokerColors.clock(tone), style: StrokeStyle(lineWidth: max(3, diameter * 0.09), lineCap: .round))
                .rotationEffect(.degrees(-90))
                .frame(width: diameter, height: diameter)
                .shadow(color: PokerColors.clock(tone).opacity(0.7), radius: 4)
                .animation(.linear(duration: 0.25), value: remaining / 250)
        }
        .accessibilityHidden(true)
    }
}

struct PokerAvatar: View {
    let seat: PokerSeat
    let model: PokerModel
    let table: PokerTable
    let diameter: CGFloat

    private var tint: Color {
        let palette: [Color] = [
            Color(red: 0.36, green: 0.42, blue: 0.75), Color(red: 0.72, green: 0.35, blue: 0.30),
            Color(red: 0.30, green: 0.58, blue: 0.52), Color(red: 0.62, green: 0.45, blue: 0.22),
            Color(red: 0.52, green: 0.33, blue: 0.62), Color(red: 0.25, green: 0.50, blue: 0.70),
        ]
        let hash = seat.name.unicodeScalars.reduce(0) { ($0 &* 31 &+ Int($1.value)) & 0xFFFF }
        return palette[hash % palette.count]
    }

    var body: some View {
        ZStack {
            Circle()
                .fill(LinearGradient(colors: [tint.opacity(0.95), tint.opacity(0.65)], startPoint: .top, endPoint: .bottom))
            Circle().strokeBorder(Color.white.opacity(0.85), lineWidth: 1.5)
            if seat.bot {
                Image(systemName: "cpu")
                    .font(.system(size: diameter * 0.36, weight: .semibold))
                    .foregroundStyle(.white)
            } else {
                Text(seat.initial)
                    .font(.system(size: diameter * 0.44, weight: .bold, design: .rounded))
                    .foregroundStyle(.white)
            }
            if seat.isTurn && table.actDeadline > 0 {
                PokerTurnRing(model: model, deadline: table.actDeadline, started: table.actStartedAt, diameter: diameter + 8)
            }
        }
        .frame(width: diameter, height: diameter)
        .shadow(color: seat.winner ? PokerColors.gold : Color.black.opacity(0.5), radius: seat.winner ? 14 : 3)
        .overlay {
            if seat.winner {
                Circle()
                    .strokeBorder(PokerColors.gold, lineWidth: 3)
                    .frame(width: diameter + 10, height: diameter + 10)
            }
        }
        .saturation(seat.folded || seat.away ? 0.2 : 1)
        .opacity(seat.folded || seat.away ? 0.7 : 1)
    }
}

struct PokerFelt: View {
    let size: CGSize
    let rail: CGFloat

    var body: some View {
        let pad = rail * 0.42
        ZStack {
            Capsule()
                .fill(Color.black.opacity(0.6))
                .blur(radius: rail * 0.6)
                .offset(y: rail * 0.5)
            Capsule()
                .fill(LinearGradient(
                    colors: [PokerColors.walnutLight, PokerColors.walnut, PokerColors.walnutDark, PokerColors.walnut],
                    startPoint: .topLeading, endPoint: .bottomTrailing))
            Capsule()
                .strokeBorder(
                    LinearGradient(colors: [Color.white.opacity(0.35), Color.white.opacity(0.0)], startPoint: .top, endPoint: .bottom),
                    lineWidth: 1.5)
            PokerWoodGrain()
                .clipShape(Capsule())
                .opacity(0.22)
            Capsule()
                .fill(LinearGradient(colors: [Color(white: 0.17), PokerColors.leather, Color(white: 0.05)], startPoint: .top, endPoint: .bottom))
                .padding(rail - pad)
                .shadow(color: .black.opacity(0.6), radius: 3, y: 2)
            Capsule()
                .strokeBorder(Color.white.opacity(0.12), lineWidth: 1)
                .padding(rail - pad + 2)
            Capsule()
                .fill(RadialGradient(
                    colors: [PokerColors.feltCenter, PokerColors.feltEdge],
                    center: .center, startRadius: 0, endRadius: max(size.width, size.height) * 0.55))
                .overlay(PokerFeltWeave().clipShape(Capsule()).opacity(0.07))
                .overlay(
                    Capsule()
                        .stroke(Color.black.opacity(0.55), lineWidth: 6)
                        .blur(radius: 5)
                        .clipShape(Capsule())
                )
                .padding(rail)
            Capsule()
                .strokeBorder(PokerColors.gold.opacity(0.75), lineWidth: 1.2)
                .padding(rail + min(size.width, size.height) * 0.06)
            Image("Logo")
                .resizable()
                .renderingMode(.template)
                .scaledToFit()
                .foregroundStyle(Color.white)
                .frame(width: min(size.width, size.height) * 0.22)
                .opacity(0.07)
                .accessibilityHidden(true)
        }
        .frame(width: size.width, height: size.height)
        .accessibilityHidden(true)
    }
}

struct PokerFeltWeave: View {
    var body: some View {
        Canvas { context, size in
            var path = Path()
            var x: CGFloat = 0
            while x < size.width {
                path.move(to: CGPoint(x: x, y: 0))
                path.addLine(to: CGPoint(x: x, y: size.height))
                x += 3
            }
            var y: CGFloat = 0
            while y < size.height {
                path.move(to: CGPoint(x: 0, y: y))
                path.addLine(to: CGPoint(x: size.width, y: y))
                y += 3
            }
            context.stroke(path, with: .color(.black), lineWidth: 0.5)
        }
        .drawingGroup()
    }
}

struct PokerWoodGrain: View {
    var body: some View {
        Canvas { context, size in
            var path = Path()
            var y: CGFloat = 2
            var row = 0
            while y < size.height {
                path.move(to: CGPoint(x: 0, y: y))
                let wave = CGFloat(row % 3 + 1) * 2
                path.addCurve(
                    to: CGPoint(x: size.width, y: y + wave),
                    control1: CGPoint(x: size.width * 0.3, y: y - wave * 2),
                    control2: CGPoint(x: size.width * 0.7, y: y + wave * 3))
                y += CGFloat(5 + row % 4)
                row += 1
            }
            context.stroke(path, with: .color(PokerColors.walnutDark), lineWidth: 0.8)
        }
        .drawingGroup()
    }
}

struct PokerDealerFigure: View {
    let line: String
    let width: CGFloat

    var body: some View {
        VStack(spacing: 4) {
            ZStack {
                RoundedRectangle(cornerRadius: 5, style: .continuous)
                    .fill(LinearGradient(colors: [Color(white: 0.22), Color(white: 0.08)], startPoint: .top, endPoint: .bottom))
                    .frame(width: width, height: width * 0.22)
                    .overlay(
                        HStack(spacing: width * 0.025) {
                            ForEach([5000, 1000, 500, 100, 25, 5, 1], id: \.self) { (value: Int) in
                                Capsule()
                                    .fill(PokerColors.chip(value))
                                    .frame(width: width * 0.1, height: width * 0.13)
                                    .overlay(Capsule().strokeBorder(Color.white.opacity(0.5), lineWidth: 0.5))
                            }
                        }
                    )
                    .overlay(RoundedRectangle(cornerRadius: 5, style: .continuous).strokeBorder(PokerColors.gold.opacity(0.5), lineWidth: 0.8))
                ZStack {
                    Circle()
                        .fill(LinearGradient(colors: [Color(white: 0.96), Color(white: 0.8)], startPoint: .top, endPoint: .bottom))
                    Image(systemName: "person.fill")
                        .font(.system(size: width * 0.17))
                        .foregroundStyle(Color(white: 0.2))
                    Image(systemName: "seal.fill")
                        .font(.system(size: width * 0.07))
                        .foregroundStyle(PokerColors.cardRed)
                        .offset(y: width * 0.11)
                }
                .frame(width: width * 0.3, height: width * 0.3)
                .overlay(Circle().strokeBorder(PokerColors.gold, lineWidth: 1.5))
                .shadow(color: .black.opacity(0.5), radius: 3, y: 2)
                .offset(y: -width * 0.2)
            }
            if !line.isEmpty {
                Text(line)
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(Color.white.opacity(0.92))
                    .lineLimit(2)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Capsule().fill(Color.black.opacity(0.4)))
                    .transition(.opacity)
                    .id(line)
                    .accessibilityLabel("Dealer: \(line)")
            }
        }
    }
}

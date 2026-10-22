import FMControl
import SwiftUI
import UIKit

struct AgentPromptDeck: View {
    let deck: PromptDeck
    var accessibilityPrefix: String = "agent-prompt-deck"

    @State private var held: Int?
    @State private var motion: Motion = .arrive
    @State private var expanded = false
    @State private var drag: CGFloat = 0
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        if !deck.isEmpty {
            ZStack(alignment: .top) {
                card
                    .id(deck.count - 1 - index)
                    .transition(cardTransition)
            }
            .background(alignment: .top) { edges }
            .padding(.bottom, CGFloat(deck.behind(index)) * 3)
            .animation(spring, value: deck.behind(index))
            .onChange(of: deck.count) { (_: Int, _: Int) in
                if held != nil {
                    show(0, .arrive)
                }
            }
            .fmFeedback(.selection, trigger: held)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(index == 0 ? "Your last prompt" : "Your earlier prompt"))
            .accessibilityValue(Text([deck.prompt(at: index), deck.position(index)].filter { !$0.isEmpty }.joined(separator: ", ")))
            .accessibilityAdjustableAction { (direction: AccessibilityAdjustmentDirection) in
                switch direction {
                case .increment: step(older: false)
                case .decrement: step(older: true)
                @unknown default: break
                }
            }
            .accessibilityIdentifier(accessibilityPrefix)
        }
    }

    private var index: Int {
        deck.clamped(held.map { deck.count - 1 - $0 } ?? 0)
    }

    private var cardShape: RoundedRectangle {
        RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous)
    }

    private var edges: some View {
        ZStack(alignment: .top) {
            ForEach(Array((1...2).reversed()), id: \.self) { (depth: Int) in
                if depth <= deck.behind(index) {
                    cardShape
                        .fill(fm.accentSubtle.opacity(0.6))
                        .background(cardShape.fill(fm.bgRaised))
                        .overlay(cardShape.stroke(fm.border, lineWidth: 0.5))
                        .scaleEffect(x: 1 - CGFloat(depth) * 0.05, y: 1, anchor: .bottom)
                        .offset(y: CGFloat(depth) * 3)
                }
            }
        }
    }

    private var card: some View {
        let shown: Int = index
        return HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
            Image(systemName: shown == 0 ? "quote.opening" : "clock.arrow.circlepath")
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(fm.accent)
            Text(expanded ? deck.prompt(at: shown) : deck.headline(at: shown))
                .fmText(.bodySmall)
                .lineLimit(expanded ? 8 : 1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fixedSize(horizontal: false, vertical: expanded)
            if !deck.position(shown).isEmpty {
                Text(deck.position(shown))
                    .fmText(.labelSmall, color: fm.textTertiary, tabular: true)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(cardShape.fill(fm.accentSubtle))
        .background(cardShape.fill(fm.bgRaised))
        .overlay(cardShape.stroke(fm.border, lineWidth: 0.5))
        .contentShape(cardShape)
        .offset(x: drag)
        .rotationEffect(.degrees(reduceMotion ? 0 : Double(drag) * 0.04), anchor: .bottom)
        .onTapGesture {
            withAnimation(spring) { expanded.toggle() }
        }
        .gesture(swipe)
        .contextMenu {
            Button {
                UIPasteboard.general.string = deck.prompt(at: shown)
            } label: {
                Label("Copy prompt", systemImage: "doc.on.doc")
            }
            if shown != 0 {
                Button {
                    show(0, .forward)
                } label: {
                    Label("Back to the latest", systemImage: "arrow.up.to.line")
                }
            }
        }
    }

    private var swipe: some Gesture {
        DragGesture(minimumDistance: 12)
            .onChanged { (value: DragGesture.Value) in
                let dx: CGFloat = value.translation.width
                guard abs(dx) > abs(value.translation.height) else {
                    return
                }
                let free: Bool = dx > 0 ? deck.older(than: index) != index : deck.newer(than: index) != index
                drag = free ? dx * 0.6 : dx * 0.15
            }
            .onEnded { (value: DragGesture.Value) in
                let dx: CGFloat = value.predictedEndTranslation.width
                if abs(dx) > abs(value.predictedEndTranslation.height) {
                    if dx > 50 {
                        step(older: true)
                    } else if dx < -50 {
                        step(older: false)
                    }
                }
                withAnimation(spring) { drag = 0 }
            }
    }

    private func step(older: Bool) {
        let next: Int = older ? deck.older(than: index) : deck.newer(than: index)
        if next == index {
            return
        }
        show(next, older ? .back : .forward)
    }

    private func show(_ next: Int, _ how: Motion) {
        motion = how
        DispatchQueue.main.async {
            withAnimation(spring) {
                held = next == 0 ? nil : deck.count - 1 - next
                expanded = false
            }
            DispatchQueue.main.async {
                motion = .arrive
            }
        }
    }

    private var spring: Animation? {
        reduceMotion ? nil : .spring(response: 0.32, dampingFraction: 0.72)
    }

    private var cardTransition: AnyTransition {
        if reduceMotion {
            return .opacity
        }
        let fromLeft: AnyTransition = .offset(x: -80).combined(with: .opacity)
        let fromRight: AnyTransition = .offset(x: 80).combined(with: .opacity)
        let sink: AnyTransition = .scale(scale: 0.9, anchor: .bottom).combined(with: .opacity)
        switch motion {
        case .back: return .asymmetric(insertion: fromLeft, removal: fromRight)
        case .forward: return .asymmetric(insertion: fromRight, removal: fromLeft)
        case .arrive: return .asymmetric(insertion: .offset(y: -24).combined(with: .opacity), removal: sink)
        }
    }

    private enum Motion {
        case back
        case forward
        case arrive
    }
}

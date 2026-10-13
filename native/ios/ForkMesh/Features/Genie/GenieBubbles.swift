import FMAssistant
import FMBase
import SwiftUI
import UIKit

enum GenieText {
    @MainActor static func highlighted(_ segments: [GenieSegment], base: Color = Color.white) -> AttributedString {
        var out: AttributedString = AttributedString()
        for segment in segments {
            var run: AttributedString = AttributedString(segment.text)
            if segment.actionable {
                run.foregroundColor = GenieSheet.night
                run.backgroundColor = GenieInk.glow
                run.inlinePresentationIntent = .stronglyEmphasized
            } else {
                run.foregroundColor = base
            }
            out += run
        }
        return out
    }
}

struct GenieGrip: Equatable {
    var pinch: CGFloat = 1
    var pinching: Bool = false
    var dragging: Bool = false
}

struct GenieTickerView: View {
    let ticker: GenieTicker
    let listening: Bool
    let onLeft: Bool
    let onTap: () -> Void

    private var actionable: Bool { ticker.segments.contains(where: \.actionable) }

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x1) {
            Image(systemName: listening ? "ear.fill" : "waveform")
                .font(.caption2.weight(.bold))
                .foregroundStyle(GenieInk.glow)
                .padding(.top, 2)
            Text(GenieText.highlighted(ticker.segments, base: Color.white.opacity(ticker.final ? 0.95 : 0.8)))
                .font(.footnote)
                .lineLimit(3)
                .multilineTextAlignment(onLeft ? .trailing : .leading)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(GenieSheet.night.opacity(0.88)))
        .overlay(
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .strokeBorder(actionable ? GenieInk.glow.opacity(0.9) : Color.white.opacity(0.12), lineWidth: actionable ? 1.5 : 1))
        .shadow(color: Color.black.opacity(0.25), radius: 8, y: 3)
        .contentShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
        .onTapGesture(perform: onTap)
        .environment(\.colorScheme, .dark)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text("Heard: \(ticker.segments.map(\.text).joined())"))
        .accessibilityIdentifier("genie-ticker")
    }
}

struct GenieFlyerView: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let flyer: GenieFlyer
    let onRun: () -> Void
    @State private var floated: Bool = false

    var body: some View {
        Button(action: onRun) {
            VStack(spacing: 3) {
                ZStack {
                    Circle()
                        .fill(
                            LinearGradient(
                                colors: [GenieInk.violet, GenieSheet.night], startPoint: UnitPoint.top, endPoint: UnitPoint.bottom))
                    Circle().strokeBorder(GenieInk.glow.opacity(0.9), lineWidth: 1.5)
                    Image(systemName: flyer.spot.symbol)
                        .font(.system(size: 19, weight: .semibold))
                        .foregroundStyle(Color.white)
                    Image(systemName: "sparkle")
                        .font(.system(size: 10, weight: .bold))
                        .foregroundStyle(GenieInk.glow)
                        .offset(x: 16, y: -16)
                        .rotationEffect(.degrees(floated ? 20 : -20))
                }
                .frame(width: 46, height: 46)
                .shadow(color: GenieInk.glow.opacity(0.45), radius: 9)
                Text(flyer.running ? flyer.spot.page : flyer.spot.title)
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(Color.white)
                    .lineLimit(1)
                    .padding(.horizontal, 7)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(GenieSheet.night.opacity(0.85)))
                    .frame(maxWidth: 150)
            }
        }
        .buttonStyle(.plain)
        .disabled(flyer.running)
        .offset(y: floated ? -5 : 0)
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(.easeInOut(duration: 1.1).repeatForever(autoreverses: true)) {
                floated = true
            }
        }
        .accessibilityLabel(Text(flyer.spot.title))
        .accessibilityHint(Text(flyer.running ? "" : "Runs this for you"))
        .accessibilityIdentifier("genie-flyer")
    }
}

struct GenieMiniBubble: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let session: GenieSession
    let hub: GenieHub
    let onLeft: Bool
    @State private var typing: Bool = false
    @State private var typed: String = ""
    @FocusState private var typedFocus: Bool

    private var status: String {
        let caption: String = session.phase.caption
        if !caption.isEmpty && session.phase != .idle { return caption }
        return hub.alwaysListening ? "Always listening" : "Genie"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            header
            lines
            if !session.hearing.isEmpty {
                Text(GenieText.highlighted(GenieSpotter.segments(session.hearing, highlight: session.brain.toolbox.spot(session.hearing)?.phrase)))
                    .font(.footnote.italic())
                    .lineLimit(3)
                    .accessibilityAddTraits(.updatesFrequently)
            } else if let ticker = hub.ticker, !session.isBusy {
                Text(GenieText.highlighted(ticker.segments, base: Color.white.opacity(0.75)))
                    .font(.footnote.italic())
                    .lineLimit(3)
                    .accessibilityAddTraits(.updatesFrequently)
            }
            if case .confirming(let summary) = session.phase {
                confirmation(summary)
            } else if let offer = session.offer, !session.isBusy || session.phase == .listening {
                GenieHintChip(hint: offer) { hub.runHint(offer) }
            }
            if let problem = session.problem {
                Text(problem)
                    .font(.caption2)
                    .foregroundStyle(Color(red: 1, green: 0.62, blue: 0.62))
                    .lineLimit(3)
                    .fmMessageTools(problem, color: Color.white.opacity(0.7))
            }
            controls
        }
        .padding(FmSpace.x3)
        .background(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(LinearGradient(colors: [GenieSheet.night, GenieSheet.dusk], startPoint: UnitPoint.top, endPoint: UnitPoint.bottom)))
        .overlay(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .strokeBorder(GenieInk.glow.opacity(0.45), lineWidth: 1))
        .shadow(color: GenieInk.violet.opacity(0.45), radius: 14, y: 5)
        .environment(\.colorScheme, .dark)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("genie-bubble")
    }

    private var header: some View {
        HStack(spacing: FmSpace.x1) {
            Image(systemName: hub.alwaysListening ? "ear.fill" : "wand.and.rays")
                .font(.caption2.weight(.bold))
                .foregroundStyle(GenieInk.glow)
            Text(status)
                .font(.caption.weight(.semibold))
                .foregroundStyle(Color.white.opacity(0.85))
                .lineLimit(1)
                .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: status)
            Spacer(minLength: 0)
            small("xmark", label: "Close", id: "genie-bubble-close") { hub.closeBubble() }
        }
    }

    private var lines: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            ForEach(session.lines.suffix(3)) { (line: GenieLine) in
                HStack {
                    if line.fromUser { Spacer(minLength: FmSpace.x4) }
                    Text(line.fromUser ? GenieText.highlighted(GenieSpotter.segments(line.text, highlight: session.brain.toolbox.spot(line.text)?.phrase)) : AttributedString(line.text))
                        .font(.footnote)
                        .foregroundStyle(Color.white)
                        .lineLimit(line.fromUser ? 2 : 5)
                        .padding(.horizontal, FmSpace.x2)
                        .padding(.vertical, 5)
                        .background(
                            RoundedRectangle(cornerRadius: 11, style: .continuous)
                                .fill(line.fromUser ? Color.white.opacity(0.14) : GenieInk.violet.opacity(0.5)))
                    if !line.fromUser { Spacer(minLength: FmSpace.x4) }
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel(Text(line.fromUser ? "You: \(line.text)" : "Genie: \(line.text)"))
                .transition(.opacity.combined(with: .move(edge: .bottom)))
            }
        }
        .animation(reduceMotion ? nil : .easeOut(duration: 0.22), value: session.lines.count)
    }

    private func confirmation(_ summary: String) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(summary)
                .font(.caption)
                .foregroundStyle(Color.white)
                .lineLimit(3)
            HStack(spacing: FmSpace.x2) {
                Button("No") { session.confirm(false) }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .accessibilityIdentifier("genie-bubble-no")
                Button("Yes") { session.confirm(true) }
                    .buttonStyle(.borderedProminent)
                    .controlSize(.small)
                    .tint(GenieInk.violet)
                    .accessibilityIdentifier("genie-bubble-yes")
            }
        }
    }

    private var controls: some View {
        HStack(spacing: FmSpace.x2) {
            if typing {
                TextField("Ask the Genie", text: $typed)
                    .textFieldStyle(.plain)
                    .font(.footnote)
                    .foregroundStyle(Color.white)
                    .padding(.horizontal, FmSpace.x2)
                    .padding(.vertical, 6)
                    .background(Capsule().fill(Color.white.opacity(0.12)))
                    .focused($typedFocus)
                    .submitLabel(.send)
                    .onSubmit(sendTyped)
                    .accessibilityIdentifier("genie-bubble-typed")
            } else {
                Spacer(minLength: 0)
            }
            small(typing ? "keyboard.chevron.compact.down" : "keyboard", label: typing ? "Hide keyboard" : "Type instead", id: "genie-bubble-keyboard") {
                typing.toggle()
                typedFocus = typing
            }
            microphone
        }
    }

    private var microphone: some View {
        let listening: Bool = session.phase == .listening
        return Button {
            FmHaptics.impact(.light)
            session.talk()
        } label: {
            Image(systemName: listening ? "stop.fill" : "mic.fill")
                .font(.system(size: 14, weight: .bold))
                .foregroundStyle(Color.white)
                .frame(width: 34, height: 34)
                .background(
                    Circle().fill(
                        LinearGradient(
                            colors: listening
                                ? [Color(red: 1, green: 0.36, blue: 0.42), Color(red: 0.78, green: 0.16, blue: 0.32)]
                                : [GenieInk.glowSoft, GenieInk.violet],
                            startPoint: UnitPoint.top, endPoint: UnitPoint.bottom)))
                .overlay(Circle().strokeBorder(GenieInk.glow.opacity(0.7), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .disabled(session.phase == .handingOff)
        .accessibilityLabel(Text(listening ? "Stop listening" : "Talk to the Genie"))
        .accessibilityIdentifier("genie-bubble-microphone")
    }

    private func small(_ symbol: String, label: String, id: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 11, weight: .bold))
                .foregroundStyle(Color.white.opacity(0.85))
                .frame(width: 26, height: 26)
                .background(Circle().fill(Color.white.opacity(0.12)))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(label))
        .accessibilityIdentifier(id)
    }

    private func sendTyped() {
        let text: String = typed
        typed = ""
        session.submit(text)
    }
}

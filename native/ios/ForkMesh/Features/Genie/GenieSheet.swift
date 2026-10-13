import FMAssistant
import FMBase
import SwiftUI

struct GenieSheet: View {
    static let night: Color = Color(red: 0.07, green: 0.05, blue: 0.20)
    static let dusk: Color = Color(red: 0.20, green: 0.09, blue: 0.36)

    @Environment(\.dismiss) private var dismiss
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State var session: GenieSession
    @State private var typing: Bool = false
    @State private var typed: String = ""
    @FocusState private var typedFocus: Bool

    let modelName: String
    var hub: GenieHub = GenieHub.shared

    var body: some View {
        VStack(spacing: FmSpace.x3) {
            header
            GenieAvatar(
                mood: GenieMood(session.phase), level: session.hearing.isEmpty ? 0.2 : 0.8, cue: GenieCues.shared.cue,
                lean: reduceMotion ? .zero : GenieMotion.shared.lean, vitals: hub.vitals, show: GenieHandCoach.shared.showing)
                .frame(maxWidth: 320, maxHeight: 320)
                .frame(maxWidth: .infinity)
                .padding(.top, FmSpace.x2)
            caption
            conversation
            if case .confirming(let summary) = session.phase {
                confirmation(summary)
            } else if let offer = session.offer {
                GenieHintChip(hint: offer) { hub.runHint(offer) }
            }
            if let problem = session.problem {
                Text(problem)
                    .font(.footnote)
                    .foregroundStyle(Color(red: 1, green: 0.62, blue: 0.62))
                    .multilineTextAlignment(.center)
                    .fmMessageTools(problem, color: Color.white.opacity(0.7))
                    .padding(.horizontal, FmSpace.x5)
            }
            controls
        }
        .padding(.bottom, FmSpace.x4)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background { backdrop.ignoresSafeArea() }
        .environment(\.colorScheme, .dark)
        .task {
            GenieMotion.shared.begin()
            session.open()
        }
        .onDisappear {
            GenieMotion.shared.end()
            session.close()
        }
        .accessibilityIdentifier("genie-sheet")
    }

    private var header: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Genie")
                    .font(.headline)
                    .foregroundStyle(Color.white)
                Text(modelName)
                    .font(.caption)
                    .foregroundStyle(Color.white.opacity(0.6))
            }
            Spacer(minLength: 0)
            headerButton(hub.alwaysListening ? "ear.fill" : "ear", label: hub.alwaysListening ? "Stop always listening" : "Always listen", id: "genie-always") {
                hub.toggleAlwaysListening()
            }
            .foregroundStyle(hub.alwaysListening ? GenieInk.glow : Color.white.opacity(0.85))
            headerButton("arrow.down.right.and.arrow.up.left", label: "Small bubble", id: "genie-shrink") {
                hub.shrink()
            }
            Button {
                session.close()
                dismiss()
            } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Color.white.opacity(0.85))
                    .frame(width: 36, height: 36)
                    .background(Circle().fill(Color.white.opacity(0.12)))
            }
            .accessibilityLabel(Text("Close"))
            .accessibilityIdentifier("genie-close")
        }
        .padding(.horizontal, FmSpace.x5)
        .padding(.top, FmSpace.x4)
    }

    private func headerButton(_ symbol: String, label: String, id: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .semibold))
                .frame(width: 36, height: 36)
                .background(Circle().fill(Color.white.opacity(0.12)))
        }
        .foregroundStyle(Color.white.opacity(0.85))
        .accessibilityLabel(Text(label))
        .accessibilityIdentifier(id)
    }

    private var caption: some View {
        let words: String = session.hearing.isEmpty ? session.phase.caption : "“\(session.hearing)”"
        let spot: GenieSpot? = session.hearing.isEmpty ? nil : session.brain.toolbox.spot(session.hearing)
        let shown: AttributedString = spot.map { (found: GenieSpot) -> AttributedString in
            GenieText.highlighted(GenieSpotter.segments(words, highlight: (found.phrase.lowerBound + 1)..<(found.phrase.upperBound + 1)))
        } ?? AttributedString(words)
        return Text(shown)
            .font(session.hearing.isEmpty ? .subheadline : .title3.weight(.medium))
            .foregroundStyle(Color.white.opacity(session.hearing.isEmpty ? 0.7 : 1))
            .multilineTextAlignment(.center)
            .lineLimit(4)
            .padding(.horizontal, FmSpace.x5)
            .frame(minHeight: 28)
            .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: words)
            .accessibilityAddTraits(.updatesFrequently)
    }

    private var conversation: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                LazyVStack(spacing: FmSpace.x2) {
                    ForEach(session.lines) { (line: GenieLine) in
                        bubble(line)
                            .id(line.id)
                    }
                }
                .padding(.horizontal, FmSpace.x4)
            }
            .onChange(of: session.lines.count) { (_: Int, _: Int) in
                if let last = session.lines.last {
                    withAnimation(reduceMotion ? nil : .easeOut(duration: 0.25)) {
                        proxy.scrollTo(last.id, anchor: .bottom)
                    }
                }
            }
        }
        .frame(maxHeight: .infinity)
    }

    private func bubble(_ line: GenieLine) -> some View {
        HStack {
            if line.fromUser { Spacer(minLength: FmSpace.x7) }
            Text(line.text)
                .font(.callout)
                .foregroundStyle(Color.white)
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, FmSpace.x2)
                .background(
                    RoundedRectangle(cornerRadius: 14, style: .continuous)
                        .fill(line.fromUser ? Color.white.opacity(0.16) : GenieInk.violet.opacity(0.45)))
                .textSelection(.enabled)
            if !line.fromUser { Spacer(minLength: FmSpace.x7) }
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text(line.fromUser ? "You: \(line.text)" : "Genie: \(line.text)"))
    }

    private func confirmation(_ summary: String) -> some View {
        VStack(spacing: FmSpace.x2) {
            Text(summary)
                .font(.callout)
                .foregroundStyle(Color.white)
                .multilineTextAlignment(.center)
            HStack(spacing: FmSpace.x3) {
                Button("No") { session.confirm(false) }
                    .buttonStyle(.bordered)
                    .accessibilityIdentifier("genie-confirm-no")
                Button("Yes, go ahead") { session.confirm(true) }
                    .buttonStyle(.borderedProminent)
                    .tint(GenieInk.violet)
                    .accessibilityIdentifier("genie-confirm-yes")
            }
        }
        .padding(FmSpace.x3)
        .background(RoundedRectangle(cornerRadius: 16, style: .continuous).fill(Color.white.opacity(0.08)))
        .padding(.horizontal, FmSpace.x4)
    }

    private var controls: some View {
        VStack(spacing: FmSpace.x3) {
            if typing {
                HStack(spacing: FmSpace.x2) {
                    TextField("Ask the Genie", text: $typed)
                        .textFieldStyle(.plain)
                        .foregroundStyle(Color.white)
                        .padding(.horizontal, FmSpace.x3)
                        .padding(.vertical, FmSpace.x2)
                        .background(Capsule().fill(Color.white.opacity(0.12)))
                        .focused($typedFocus)
                        .submitLabel(.send)
                        .onSubmit(sendTyped)
                        .accessibilityIdentifier("genie-typed")
                    Button(action: sendTyped) {
                        Image(systemName: "arrow.up.circle.fill")
                            .font(.system(size: 30))
                            .foregroundStyle(GenieInk.glow)
                    }
                    .disabled(typed.dartTrimmed.isEmpty)
                    .accessibilityLabel(Text("Send"))
                }
                .padding(.horizontal, FmSpace.x4)
            }
            HStack(spacing: FmSpace.x6) {
                Button {
                    typing.toggle()
                    typedFocus = typing
                } label: {
                    Image(systemName: typing ? "keyboard.chevron.compact.down" : "keyboard")
                        .font(.system(size: 18, weight: .medium))
                        .foregroundStyle(Color.white.opacity(0.8))
                        .frame(width: 48, height: 48)
                        .background(Circle().fill(Color.white.opacity(0.1)))
                }
                .accessibilityLabel(Text(typing ? "Hide keyboard" : "Type instead"))
                microphone
                Color.clear.frame(width: 48, height: 48)
            }
        }
    }

    private var microphone: some View {
        let listening: Bool = session.phase == .listening
        return Button {
            FmHaptics.tap()
            session.talk()
        } label: {
            Image(systemName: listening ? "stop.fill" : "mic.fill")
                .font(.system(size: 26, weight: .semibold))
                .foregroundStyle(Color.white)
                .frame(width: 76, height: 76)
                .background(
                    Circle().fill(
                        LinearGradient(
                            colors: listening
                                ? [Color(red: 1, green: 0.36, blue: 0.42), Color(red: 0.78, green: 0.16, blue: 0.32)]
                                : [GenieInk.glowSoft, GenieInk.violet],
                            startPoint: UnitPoint.top, endPoint: UnitPoint.bottom)))
                .overlay(Circle().strokeBorder(GenieInk.glow.opacity(0.7), lineWidth: 1.5))
                .shadow(color: GenieInk.violet.opacity(0.5), radius: 14, y: 6)
        }
        .disabled(session.phase == .handingOff)
        .accessibilityLabel(Text(listening ? "Stop listening" : "Talk to the Genie"))
        .accessibilityIdentifier("genie-microphone")
    }

    private var backdrop: some View {
        ZStack {
            LinearGradient(colors: [GenieSheet.night, GenieSheet.dusk], startPoint: UnitPoint.top, endPoint: UnitPoint.bottom)
            Canvas { (context: inout GraphicsContext, size: CGSize) in
                var generator: GenieStarField = GenieStarField(seed: 7)
                for _ in 0..<70 {
                    let x: CGFloat = generator.next() * size.width
                    let y: CGFloat = generator.next() * size.height * 0.75
                    let radius: CGFloat = 0.4 + generator.next() * 1.3
                    context.fill(
                        Path(ellipseIn: CGRect(x: x, y: y, width: radius * 2, height: radius * 2)),
                        with: .color(Color.white.opacity(0.2 + Double(generator.next()) * 0.5)))
                }
            }
        }
    }

    private func sendTyped() {
        let text: String = typed
        typed = ""
        session.submit(text)
    }
}

struct GenieStarField {
    private var state: UInt64

    init(seed: UInt64) {
        state = seed &* 0x9E37_79B9_7F4A_7C15
    }

    mutating func next() -> CGFloat {
        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
        return CGFloat(Double(state >> 11) / Double(UInt64(1) << 53))
    }
}

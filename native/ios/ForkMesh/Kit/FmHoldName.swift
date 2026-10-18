import SwiftUI

struct FmHoldName: ViewModifier {
    static let holdSeconds: Double = 0.45
    static let shownSeconds: Double = 1.6

    @Environment(\.fmPalette) private var fm
    let name: String
    var held: Binding<Bool> = .constant(false)
    @State private var shown: Bool = false
    @State private var serial: Int = 0

    func body(content: Content) -> some View {
        content
            .simultaneousGesture(
                LongPressGesture(minimumDuration: Self.holdSeconds)
                    .onChanged { (_: Bool) in held.wrappedValue = false }
                    .onEnded { (_: Bool) in
                        held.wrappedValue = true
                        show()
                    }
            )
            .popover(isPresented: $shown, attachmentAnchor: .rect(.bounds), arrowEdge: .bottom) {
                Text(name)
                    .fmText(.labelMedium, color: fm.textPrimary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, FmSpace.x3)
                    .padding(.vertical, FmSpace.x2)
                    .presentationCompactAdaptation(.popover)
            }
    }

    private func show() {
        FmHaptics.selection(.open)
        serial += 1
        let mine: Int = serial
        shown = true
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(Self.shownSeconds))
            if serial == mine {
                shown = false
            }
        }
    }
}

struct FmHoldButton<Label: View>: View {
    let name: String
    let action: () -> Void
    @ViewBuilder let label: () -> Label
    @State private var held: Bool = false

    init(_ name: String, action: @escaping () -> Void, @ViewBuilder label: @escaping () -> Label) {
        self.name = name
        self.action = action
        self.label = label
    }

    var body: some View {
        Button {
            if held {
                held = false
                return
            }
            action()
        } label: {
            label()
        }
        .modifier(FmHoldName(name: name, held: $held))
    }
}

extension View {
    func fmHoldName(_ name: String) -> some View {
        modifier(FmHoldName(name: name))
    }
}

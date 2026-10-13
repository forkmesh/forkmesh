import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct FmMessageActionsModifier: ViewModifier {
    let text: String
    @State private var selecting: Bool = false

    func body(content: Content) -> some View {
        content
            .contentShape(Rectangle())
            .contextMenu {
                if !text.isEmpty {
                    Button(action: copy) { Label("Copy text", systemImage: "doc.on.doc") }
                    Button(action: { selecting = true }) { Label("Select text", systemImage: "selection.pin.in.out") }
                }
            }
            .sheet(isPresented: $selecting) { FmSelectableTextSheet(text: text) }
    }

    private func copy() {
        #if canImport(UIKit)
        UIPasteboard.general.string = text
        #endif
    }
}

struct FmSelectableTextSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let text: String

    var body: some View {
        NavigationStack {
            ScrollView {
                Text(text)
                    .fmText(.bodyLarge)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Select text")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}

extension View {
    func fmMessageActions(text: String) -> some View {
        return modifier(FmMessageActionsModifier(text: text))
    }
}

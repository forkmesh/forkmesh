import FMAssistant
import Foundation
import SwiftUI

@MainActor
final class GeniePageTurner {
    static let shared: GeniePageTurner = GeniePageTurner()

    private var turners: [(key: UUID, turn: @MainActor (GeniePageTurn) -> String)] = []

    var active: Bool { !turners.isEmpty }

    func attach(_ key: UUID, _ turn: @escaping @MainActor (GeniePageTurn) -> String) {
        turners.removeAll { $0.key == key }
        turners.append((key, turn))
    }

    func detach(_ key: UUID) {
        turners.removeAll { $0.key == key }
    }

    func turn(_ page: GeniePageTurn) -> String? {
        turners.last?.turn(page)
    }
}

private struct GenieTurnsPages: ViewModifier {
    let turn: @MainActor (GeniePageTurn) -> String
    @State private var key: UUID = UUID()

    func body(content: Content) -> some View {
        content
            .onAppear { GeniePageTurner.shared.attach(key, turn) }
            .onDisappear { GeniePageTurner.shared.detach(key) }
    }
}

extension View {
    func genieTurnsPages(_ turn: @escaping @MainActor (GeniePageTurn) -> String) -> some View {
        modifier(GenieTurnsPages(turn: turn))
    }
}

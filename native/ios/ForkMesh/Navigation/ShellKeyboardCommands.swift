import FMApi
import SwiftUI

struct ShellKeyboardCommands: View {
    let onTab: (AppTab) -> Void
    let onPalette: () -> Void
    let onCompose: () -> Void
    let onNewIssue: () -> Void
    let onBack: () -> Void
    let onSettings: () -> Void
    let onEscape: () -> Void

    var body: some View {
        ZStack {
            ForEach(Array(AppTab.barOrder.enumerated()), id: \.element) { (entry: (offset: Int, element: AppTab)) in
                Button(entry.element.title) { onTab(entry.element) }
                    .keyboardShortcut(KeyEquivalent(Character("\(entry.offset + 1)")), modifiers: .command)
            }
            Button("Command palette", action: onPalette)
                .keyboardShortcut("k", modifiers: .command)
            Button("New agent", action: onCompose)
                .keyboardShortcut("n", modifiers: .command)
            Button("New issue", action: onNewIssue)
                .keyboardShortcut("n", modifiers: [.command, .shift])
            Button("Back", action: onBack)
                .keyboardShortcut("[", modifiers: .command)
            Button("Settings", action: onSettings)
                .keyboardShortcut(",", modifiers: .command)
            Button("Dismiss", action: onEscape)
                .keyboardShortcut(.escape, modifiers: [])
        }
        .buttonStyle(.plain)
        .opacity(0)
        .frame(width: 0, height: 0)
        .clipped()
        .accessibilityHidden(true)
    }
}

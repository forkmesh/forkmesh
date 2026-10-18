// The presentation half of the guest gate: guestChatChannel, GuestSignInSheet
// and GuestComposeNotice. `isGuestMode`, `requireAccount` and `openSignIn` read
// AuthService and push AuthFlow, so the account feature builds them on top of
// these: the views here take the action text and closures, never a service.

import SwiftUI

/// The only chat channel a guest may post in.
let guestChatChannel: String = "general"

/// What the guest picked in GuestSignInSheet (`AuthFlowStart.login` /
/// `AuthFlowStart.signup`).
enum GuestSignInChoice {
    case login
    case signup
}

/// Bottom sheet explaining that `action` needs an account. `action` is a
/// lower-case verb phrase completing "Sign in to ...", e.g. "open an issue".
/// `onChoice` receives the pick, or nil for "Keep exploring"; the caller
/// closes the sheet and, for a pick, opens the sign-in flow.
struct GuestSignInSheet: View {
    @Environment(\.fmPalette) private var fm

    private let action: String
    private let onChoice: (GuestSignInChoice?) -> Void

    init(action: String, onChoice: @escaping (GuestSignInChoice?) -> Void) {
        self.action = action
        self.onChoice = onChoice
    }

    private var explanation: String {
        return "You are exploring ForkMesh as a guest. You can browse "
            + "repositories, issues, pull requests and chat in "
            + "#\(guestChatChannel). Sign in whenever you want to do more."
    }

    private var copy: some View {
        VStack(spacing: 0) {
            FmIcon("lock_outline_rounded", size: 28, color: fm.textSecondary)
            Text(verbatim: "Sign in to \(action)")
                .fmText(.titleLarge)
                .multilineTextAlignment(.center)
                .padding(.top, FmSpace.x3)
            Text(verbatim: explanation)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
                .padding(.top, FmSpace.x2)
        }
        .frame(maxWidth: .infinity)
    }

    private var buttons: some View {
        VStack(spacing: 0) {
            FmFilledButton(
                "Log in",
                background: fm.textPrimary,
                foreground: fm.dark ? fm.bgBase : fm.bgRaised,
                minHeight: 48,
                radius: FmRadius.md,
                fillWidth: true,
                role: .titleMedium,
                onPressed: { onChoice(GuestSignInChoice.login) }
            )
            .accessibilityIdentifier("guest-sheet-log-in")
            FmOutlinedButton(
                "Create an account",
                foreground: fm.textPrimary,
                minHeight: 48,
                radius: FmRadius.md,
                fillWidth: true,
                role: .titleMedium,
                onPressed: { onChoice(GuestSignInChoice.signup) }
            )
            .accessibilityIdentifier("guest-sheet-sign-up")
            .padding(.top, FmSpace.x2)
            FmTextButton(
                "Keep exploring",
                color: fm.textSecondary,
                minHeight: 44,
                fillWidth: true,
                onPressed: { onChoice(nil) }
            )
            .padding(.top, FmSpace.x1)
        }
    }

    var body: some View {
        VStack(spacing: FmSpace.x5) {
            copy
            buttons
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x5)
        .padding(.bottom, FmSpace.x4)
        .accessibilityIdentifier("guest-sign-in-sheet")
    }
}

/// `GuestSignInSheet.show`. The pick is handed over only once the sheet has
/// finished closing: opening the sign-in flow while the sheet is still on its
/// way down is a presentation SwiftUI drops. Swiping the sheet away reports
/// nil, like "Keep exploring".
struct GuestSignInSheetModifier: ViewModifier {
    @Environment(\.fmPalette) private var fm
    @State private var picked: GuestSignInChoice? = nil
    @Binding var isPresented: Bool

    let action: String
    let onChoice: (GuestSignInChoice?) -> Void

    private func closed() {
        let choice: GuestSignInChoice? = picked
        picked = nil
        onChoice(choice)
    }

    private var sheetBody: some View {
        GuestSignInSheet(action: action) { (choice: GuestSignInChoice?) in
            picked = choice
            isPresented = false
        }
        .fmSheetStyle(fitsContent: true, background: fm.bgRaised)
    }

    func body(content: Content) -> some View {
        content.sheet(isPresented: $isPresented, onDismiss: closed) {
            sheetBody
        }
    }
}

extension View {
    /// Presents GuestSignInSheet at its content height on the raised layer.
    func guestSignInSheet(
        isPresented: Binding<Bool>,
        action: String,
        onChoice: @escaping (GuestSignInChoice?) -> Void
    ) -> some View {
        return modifier(GuestSignInSheetModifier(isPresented: isPresented, action: action, onChoice: onChoice))
    }
}

/// Stands in for a compose bar (reply, comment, prompt) while in guest mode,
/// so the surface explains itself instead of accepting text it cannot send.
/// `onLogIn` is `openSignIn(context)`.
struct GuestComposeNotice: View {
    @Environment(\.fmPalette) private var fm

    private let action: String
    private let onLogIn: () -> Void

    init(action: String, onLogIn: @escaping () -> Void) {
        self.action = action
        self.onLogIn = onLogIn
    }

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon("lock_outline_rounded", size: 16, color: fm.textTertiary)
            Text(verbatim: "Sign in to \(action)")
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmTextButton("Log in", color: fm.accent, minHeight: 40, onPressed: onLogIn)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(fm.bgOverlay)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .overlay {
            FmRoundedShape(radius: FmRadius.md).strokeBorder(fm.border, lineWidth: 1)
        }
        .accessibilityIdentifier("guest-compose-notice")
    }
}

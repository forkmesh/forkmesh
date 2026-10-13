// The account gating that needs services: `isGuest`, `requireAccount` and
// `openSignIn`, plus the root modifier that presents the guest sign-in sheet
// and the sign-in flow over the app.
// The sheet itself is Kit/GuestGate.swift; the decisions are FMApi's
// AuthFlowLogic.

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation
import SwiftUI

/// Presents the guest sign-in sheet and the sign-in flow over the whole app,
/// wherever they were asked for, and lets the caller await the outcome.
@MainActor
@Observable
final class SignInCoordinator {
    /// The action phrase of the guest sheet ("open an issue").
    var guestSheetAction: String = ""
    /// Whether the guest sheet is up. The pick is reported once it finished
    /// closing (GuestSignInSheetModifier), not when this turns false.
    var guestSheetVisible: Bool = false
    /// The page the sign-in flow over the app opens on, while it is up.
    var signInStart: AuthFlowStart?

    @ObservationIgnored let auth: AuthService
    @ObservationIgnored let debugBuild: Bool
    @ObservationIgnored private var sheetWaiters: [CheckedContinuation<AuthFlowStart?, Never>] = []
    @ObservationIgnored private var signInWaiters: [CheckedContinuation<Void, Never>] = []

    init(auth: AuthService, debugBuild: Bool) {
        self.auth = auth
        self.debugBuild = debugBuild
    }

    var isGuest: Bool {
        return isGuestMode(authAvailable: true, isGuest: auth.isGuest)
    }

    /// `requireAccount(context, action:)`: true when the caller may go ahead
    /// with a write. A guest sees the sheet; choosing Log in or Create an
    /// account opens sign-in, and the answer is whether they are signed in
    /// once that flow closes.
    func requireAccount(action: String) async -> Bool {
        let auth: AuthService = self.auth
        return await FMApi.requireAccount(
            guest: isGuest,
            showSheet: { await self.showGuestSheet(action: action) },
            openSignIn: { (start: AuthFlowStart) in await self.openSignIn(start) },
            isAuthenticated: { auth.isAuthenticated })
    }

    /// `GuestSignInSheet.show` without the hand-off: the pick, or nil.
    func showGuestSheet(action: String) async -> AuthFlowStart? {
        return await withCheckedContinuation { (continuation: CheckedContinuation<AuthFlowStart?, Never>) in
            sheetWaiters.append(continuation)
            guestSheetAction = action
            guestSheetVisible = true
        }
    }

    /// The sheet finished closing with this pick.
    func guestSheetClosed(_ choice: GuestSignInChoice?) {
        guestSheetVisible = false
        let start: AuthFlowStart?
        switch choice {
        case .some(.login):
            start = .login
        case .some(.signup):
            start = .signup
        case .none:
            start = nil
        }
        let waiters = sheetWaiters
        sheetWaiters = []
        for waiter in waiters {
            waiter.resume(returning: start)
        }
    }

    /// `openSignIn`: login (or signup) over the app; returns once the flow
    /// has closed, signed in or not.
    func openSignIn(_ start: AuthFlowStart = .login) async {
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            signInWaiters.append(continuation)
            signInStart = start
        }
    }

    /// Fire-and-forget `openSignIn` for buttons.
    func presentSignIn(_ start: AuthFlowStart = .login) {
        Task { await self.openSignIn(start) }
    }

    /// The flow over the app closed.
    func signInClosed() {
        signInStart = nil
        let waiters = signInWaiters
        signInWaiters = []
        for waiter in waiters {
            waiter.resume()
        }
    }
}

/// Entry points other features use for account gating. Harnesses without an
/// AppServices behave as signed in.
@MainActor
enum AccountFeature {
    static func requireAccount(action: String) async -> Bool {
        guard let coordinator = Services.get(SignInCoordinator.self) else {
            return true
        }
        return await coordinator.requireAccount(action: action)
    }

    static func openSignIn(_ start: AuthFlowStart = .login) {
        Services.get(SignInCoordinator.self)?.presentSignIn(start)
    }

    static var isGuest: Bool {
        return Services.get(SignInCoordinator.self)?.isGuest ?? false
    }
}

/// Hosts the guest sheet and the sign-in flow at the root, so they appear
/// over whatever screen asked for them.
struct SignInPresenter: ViewModifier {
    @Bindable var coordinator: SignInCoordinator

    private var flowShown: Binding<Bool> {
        Binding<Bool>(
            get: { coordinator.signInStart != nil },
            set: { (shown: Bool) in
                if !shown {
                    coordinator.signInClosed()
                }
            })
    }

    func body(content: Content) -> some View {
        content
            .guestSignInSheet(
                isPresented: $coordinator.guestSheetVisible,
                action: coordinator.guestSheetAction,
                onChoice: { (choice: GuestSignInChoice?) in
                    coordinator.guestSheetClosed(choice)
                })
            .fullScreenCover(isPresented: flowShown) {
                AuthFlowView(
                    start: coordinator.signInStart ?? .login,
                    onDismiss: { coordinator.signInClosed() })
            }
    }
}

extension View {
    /// Apply once, at the app root.
    func signInPresenter(_ coordinator: SignInCoordinator) -> some View {
        return modifier(SignInPresenter(coordinator: coordinator))
    }
}

// The auth flow: the welcome page, login with the authenticator-code step and
// the revoked banner, and the web-only signup page. Logic lives in FMApi's
// AuthFlowModel.

import FMAccount
import FMApi
import FMBase
import SafariServices
import SwiftUI
import UIKit

/// The auth flow. `AuthFlowView()` stands in front of the app (the home, with
/// "Explore as guest"); `AuthFlowView(start:onDismiss:)` is pushed over it
/// (`AccountFeature.openSignIn`), where Back and a successful sign-in both
/// close it.
@MainActor
struct AuthFlowView: View {
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    @State var model: AuthFlowModel
    @State private var webPage: AuthWebPage?

    private let onDismiss: (() -> Void)?

    init() {
        let auth: AuthService? = Services.get(AuthService.self)
        _model = State(initialValue: AuthFlowModel(auth: auth, start: .welcome, debugBuild: AppServices.isDebugBuild))
        onDismiss = nil
    }

    init(start: AuthFlowStart, onDismiss: @escaping () -> Void) {
        let auth: AuthService? = Services.get(AuthService.self)
        _model = State(
            initialValue: AuthFlowModel(
                auth: auth, start: start, pushedOverApp: true, debugBuild: AppServices.isDebugBuild))
        self.onDismiss = onDismiss
    }

    private func back() {
        if model.back() {
            onDismiss?()
        }
    }

    private func authenticated() {
        model.finishAuthenticated()
        onDismiss?()
    }

    private func show(_ page: AuthFlowStart) {
        withAnimation(FmMotion.easeOutCubic(0.22, reduceMotion: reduceMotion)) {
            model.show(page)
        }
    }

    @ViewBuilder private var page: some View {
        switch model.mode {
        case .welcome:
            AuthWelcomePage(
                onLogin: { show(.login) },
                onSignup: { show(.signup) },
                onGuest: onDismiss == nil ? { model.continueAsGuest() } : nil)
        case .login:
            AuthLoginPage(
                model: model, onAuthenticated: authenticated, onSignup: { show(.signup) },
                onForgotPassword: { open(model.forgotPasswordURL) }, onBack: back)
        case .signup:
            AuthSignupPage(onOpenSignup: openSignup, onLogin: { show(.login) }, onBack: back)
        }
    }

    private func open(_ url: URL?) {
        guard let url else { return }
        webPage = AuthWebPage(url: url)
    }

    private func openSignup() {
        open(model.signupURL)
        withAnimation(FmMotion.easeOutCubic(0.22, reduceMotion: reduceMotion)) {
            model.signupOpened()
        }
    }

    var body: some View {
        ZStack {
            FmAuroraBackground()
            page
                .id(model.mode)
                .transition(AnyTransition.opacity)
        }
        .fmForcedDark()
        .sheet(item: $webPage) { page in
            AuthSafariView(url: page.url)
                .ignoresSafeArea()
        }
    }
}

private struct AuthWebPage: Identifiable {
    let url: URL
    var id: String { url.absoluteString }
}

private struct AuthSafariView: UIViewControllerRepresentable {
    let url: URL

    func makeUIViewController(context: Context) -> SFSafariViewController {
        SFSafariViewController(url: url)
    }

    func updateUIViewController(_ controller: SFSafariViewController, context: Context) {}
}

// MARK: Welcome

private struct AuthWelcomePage: View {
    let onLogin: () -> Void
    let onSignup: () -> Void
    let onGuest: (() -> Void)?

    private var copy: some View {
        VStack(spacing: FmSpace.x4) {
            FmRevealTitle(lines: AuthCopy.welcomeLines, role: .displayLarge, delay: 0.16)
            FmRevealLine(
                text: AuthCopy.welcomeBody, role: .bodyMedium, color: FmOnboarding.secondaryText, delay: 0.46, duration: 0.6
            )
            .multilineTextAlignment(.center)
        }
        .frame(maxWidth: 460)
    }

    private var buttons: some View {
        VStack(spacing: 14) {
            FmPillButton(label: "Log in", onPressed: onLogin)
                .accessibilityIdentifier("welcome-log-in")
            FmPillButton(label: "Sign up", onPressed: onSignup)
                .accessibilityIdentifier("welcome-sign-up")
            if let onGuest = onGuest {
                FmPillButton(label: "Explore as guest", kind: .dark, onPressed: onGuest)
                    .accessibilityIdentifier("continue-as-guest")
            }
        }
        .frame(maxWidth: 460)
        .fmBrighten(delay: 0.3)
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Spacer(minLength: 0)
                ProjectSwitcherButton(onDark: true)
            }
            Spacer(minLength: FmSpace.x4)
            FmGlassIcon(size: 148)
                .fmReveal(delay: 0, rise: 26, scale: 0.9)
            Spacer(minLength: FmSpace.x6)
            copy
            buttons
                .padding(.top, 30)
            AuthTermsCopy()
                .padding(.top, FmSpace.x5)
                .fmReveal(delay: 0.6, rise: 0, scale: 1)
        }
        .padding(EdgeInsets(top: FmSpace.x3, leading: 28, bottom: FmSpace.x3, trailing: 28))
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: Login

private struct AuthLoginPage: View {
    @Environment(\.fmPalette) var fm
    @Bindable var model: AuthFlowModel
    let onAuthenticated: () -> Void
    let onSignup: () -> Void
    let onForgotPassword: () -> Void
    let onBack: () -> Void

    @FocusState var focus: LoginField?

    enum LoginField: Hashable {
        case identifier, password, totp
    }

    init(
        model: AuthFlowModel,
        onAuthenticated: @escaping () -> Void,
        onSignup: @escaping () -> Void,
        onForgotPassword: @escaping () -> Void,
        onBack: @escaping () -> Void
    ) {
        self.model = model
        self.onAuthenticated = onAuthenticated
        self.onSignup = onSignup
        self.onForgotPassword = onForgotPassword
        self.onBack = onBack
    }

    private func submit() {
        Task { @MainActor in
            if await model.submit() {
                onAuthenticated()
            }
        }
    }

    private var fields: some View {
        VStack(alignment: .leading, spacing: 24) {
            FmPillField(label: "Email or username", focused: focus == LoginField.identifier) {
                TextField("", text: $model.identifier, prompt: Text(verbatim: "you@example.com").fmPlaceholder())
                    .textContentType(.username)
                    .keyboardType(.emailAddress)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
                    .submitLabel(.next)
                    .focused($focus, equals: LoginField.identifier)
                    .onSubmit { focus = LoginField.password }
            }
            FmPillField(label: "Password", focused: focus == LoginField.password) {
                SecureField("", text: $model.password, prompt: Text("Your password").fmPlaceholder())
                    .textContentType(.password)
                    .submitLabel(model.passwordSubmits ? .done : .next)
                    .focused($focus, equals: LoginField.password)
                    .onSubmit {
                        if model.passwordSubmits {
                            submit()
                        } else {
                            focus = LoginField.totp
                        }
                    }
            }
            if model.form.showTotp {
                FmPillField(label: "Authenticator code", focused: focus == LoginField.totp) {
                    TextField("", text: $model.totp, prompt: Text("123456").fmPlaceholder())
                        .textContentType(.oneTimeCode)
                        .keyboardType(.numberPad)
                        .submitLabel(.done)
                        .focused($focus, equals: LoginField.totp)
                        .onSubmit { submit() }
                }
            }
        }
    }

    @ViewBuilder private var hint: some View {
        if !model.form.hint.isEmpty {
            Text(model.form.hint)
                .fmText(.bodyMedium, color: model.form.bad ? fm.danger : fm.success, weight: FmType.medium)
                .fmMessageTools(model.form.hint)
                .padding(.top, 18)
                .accessibilityAddTraits(.updatesFrequently)
        }
    }

    var body: some View {
        AuthPageScaffold(pageTitle: "Login", onBack: onBack) {
            FmGlassIcon(size: 92)
                .frame(maxWidth: .infinity)
                .padding(.top, 38)
                .fmReveal(rise: 22, scale: 0.9)
            AuthTitle(title: AuthCopy.loginTitle, subtitle: model.relaySubtitle)
                .padding(.top, 26)
            if model.revokedBanner {
                AuthRevokedBanner()
                    .padding(.top, 34)
            }
            fields
                .padding(.top, model.revokedBanner ? 24 : 34)
                .fmReveal(delay: 0.28, rise: 14, scale: 1)
            hint
            AuthPrimaryButton(label: model.buttonLabel, onPressed: model.loading ? nil : { submit() })
                .padding(.top, 30)
            AuthSecondaryButton(label: AuthCopy.forgotPassword, onPressed: onForgotPassword)
                .padding(.top, 12)
                .fmReveal(delay: 0.5, rise: 0, scale: 1)
            AuthSecondaryButton(label: "Create an account", onPressed: onSignup)
                .fmReveal(delay: 0.56, rise: 0, scale: 1)
            AuthTermsCopy()
                .padding(.top, 20)
                .fmReveal(delay: 0.62, rise: 0, scale: 1)
        }
    }
}

// MARK: Signup

private struct AuthSignupPage: View {
    @Environment(\.fmPalette) var fm
    let onOpenSignup: () -> Void
    let onLogin: () -> Void
    let onBack: () -> Void

    var body: some View {
        AuthPageScaffold(pageTitle: "Sign up", onBack: onBack) {
            FmGlassIcon(size: 110)
                .frame(maxWidth: .infinity)
                .padding(.top, 58)
                .fmReveal(rise: 22, scale: 0.9)
            AuthTitle(title: AuthCopy.signupTitle, subtitle: AuthCopy.signupSubtitle)
                .padding(.top, 28)
            FmRevealLine(
                text: AuthCopy.signupBody, role: .bodyMedium, color: FmOnboarding.secondaryText, delay: 0.4, duration: 0.6
            )
            .multilineTextAlignment(.center)
            .frame(maxWidth: .infinity)
            .padding(.top, 26)
            AuthPrimaryButton(label: "Open secure signup", onPressed: onOpenSignup)
                .padding(.top, 32)
            AuthSecondaryButton(label: "I already have an account", onPressed: onLogin)
                .padding(.top, 12)
                .fmReveal(delay: 0.5, rise: 0, scale: 1)
            AuthTermsCopy()
                .padding(.top, 28)
                .fmReveal(delay: 0.58, rise: 0, scale: 1)
        }
    }
}

// MARK: Pieces

private struct AuthPageScaffold<Content: View>: View {
    let pageTitle: String
    let onBack: () -> Void
    let content: Content

    init(pageTitle: String, onBack: @escaping () -> Void, @ViewBuilder content: () -> Content) {
        self.pageTitle = pageTitle
        self.onBack = onBack
        self.content = content()
    }

    private var header: some View {
        HStack(spacing: FmSpace.x3) {
            FmOnboardingBackButton(onPressed: onBack)
            Text(pageTitle)
                .fmText(.titleMedium, color: Color.white, weight: .semibold)
                .accessibilityAddTraits(.isHeader)
            Spacer(minLength: 0)
            ProjectSwitcherButton(onDark: true)
        }
        .padding(.horizontal, 20)
        .padding(.top, FmSpace.x2)
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    content
                }
                .frame(maxWidth: 520)
                .frame(maxWidth: .infinity)
                .padding(EdgeInsets(top: 0, leading: 24, bottom: FmSpace.x5, trailing: 24))
            }
            .scrollDismissesKeyboard(.interactively)
        }
    }
}

/// The ForkMesh mark on black, rounded at a quarter of its size.
struct AuthLogo: View {
    let size: CGFloat
    let shadow: Bool

    private var mark: some View {
        Group {
            if let image = UIImage(named: "Logo") {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .padding(size * 0.11)
            } else {
                Text(verbatim: "F")
                    .font(Font.system(size: size * 0.5, weight: .semibold))
                    .foregroundStyle(Color.white)
            }
        }
    }

    var body: some View {
        mark
            .frame(width: size, height: size)
            .background(Color.black)
            .clipShape(RoundedRectangle(cornerRadius: size * 0.24, style: .continuous))
            .shadow(color: shadow ? Color(fmARGB: 0x1A000000) : Color.clear, radius: 9, x: 0, y: 8)
            .accessibilityIdentifier("forkmesh-auth-logo")
            .accessibilityHidden(true)
    }
}

private struct AuthTitle: View {
    let title: String
    let subtitle: String

    var body: some View {
        VStack(spacing: 10) {
            FmRevealTitle(lines: [title], delay: 0.1)
            FmRevealLine(
                text: subtitle, role: .bodyMedium, color: Color.white.opacity(0.72), delay: 0.24, duration: 0.6
            )
            .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
    }
}

/// "This device was signed out from the web." Shown once, after the Worker
/// reported this phone's node as revoked.
private struct AuthRevokedBanner: View {
    @Environment(\.fmPalette) var fm

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            FmIcon("phonelink_erase_outlined", size: 18, color: fm.warning)
            VStack(alignment: .leading, spacing: 2) {
                Text(AuthService.revokedBannerMessage)
                    .fmText(.bodyMedium, weight: FmType.emphasis)
                Text(AuthCopy.revokedDetail)
                    .fmText(.bodySmall)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            FmMessageTools("\(AuthService.revokedBannerMessage) \(AuthCopy.revokedDetail)")
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
        .background(fm.warning.fmWithAlpha(28))
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .accessibilityElement(children: .contain)
    }
}

private struct AuthPrimaryButton: View {
    let label: String
    let onPressed: (() -> Void)?

    var body: some View {
        FmPillButton(label: label, onPressed: onPressed)
            .accessibilityIdentifier("primary-auth-button-frame")
            .fmBrighten(delay: 0.38)
    }
}

private struct AuthSecondaryButton: View {
    let label: String
    let onPressed: () -> Void

    var body: some View {
        FmPillTextButton(label: label, onPressed: onPressed)
            .frame(maxWidth: .infinity)
    }
}

private struct AuthTermsCopy: View {
    private var text: AttributedString {
        var result: AttributedString = AttributedString("By continuing, you agree to our ")
        var terms: AttributedString = AttributedString("Terms of Service")
        terms.link = LegalLinks.terms
        terms.foregroundColor = Color.white
        var privacy: AttributedString = AttributedString("Privacy Policy")
        privacy.link = LegalLinks.privacy
        privacy.foregroundColor = Color.white
        result.append(terms)
        result.append(AttributedString(" and have read our "))
        result.append(privacy)
        result.append(AttributedString("."))
        return result
    }

    var body: some View {
        Text(text)
            .fmText(.bodySmall, color: FmOnboarding.secondaryText)
            .tint(Color.white)
            .multilineTextAlignment(.center)
            .padding(.horizontal, FmSpace.x3)
            .frame(maxWidth: .infinity)
    }
}

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
            page
                .id(model.mode)
                .transition(
                    AnyTransition.opacity.combined(with: AnyTransition.offset(x: 8, y: 0)))
        }
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

    private var background: some View {
        ZStack {
            Color(fmARGB: 0xFF050505)
            RadialGradient(
                gradient: Gradient(stops: [
                    Gradient.Stop(color: Color(fmARGB: 0xFFEFEAFF), location: 0),
                    Gradient.Stop(color: Color(fmARGB: 0xFF6F5AD9), location: 0.18),
                    Gradient.Stop(color: Color(fmARGB: 0xFF050505), location: 0.48),
                ]),
                center: UnitPoint(x: 0.175, y: 0.09),
                startRadius: 0,
                endRadius: 720)
            LinearGradient(
                gradient: Gradient(stops: [
                    Gradient.Stop(color: Color(fmARGB: 0x00000000), location: 0),
                    Gradient.Stop(color: Color(fmARGB: 0xFF050505), location: 0.44),
                    Gradient.Stop(color: Color(fmARGB: 0xFF160F35), location: 0.68),
                    Gradient.Stop(color: Color(fmARGB: 0xFF050505), location: 1),
                ]),
                startPoint: UnitPoint.topTrailing,
                endPoint: UnitPoint.bottomLeading)
        }
        .ignoresSafeArea()
    }

    private var copy: some View {
        VStack(alignment: .leading, spacing: FmSpace.x5) {
            Text(AuthCopy.welcomeHeadline)
                .fmText(.displayLarge, color: Color.white)
            Text(AuthCopy.welcomeBody)
                .fmText(.bodyLarge, color: Color(fmARGB: 0xFFD8D5DF))
        }
        .frame(maxWidth: 460, alignment: .leading)
    }

    private var buttons: some View {
        HStack(spacing: 12) {
            Button(action: onLogin) {
                Text("Log in")
                    .fmText(.labelLarge, color: Color.white)
                    .frame(maxWidth: .infinity, minHeight: 54)
                    .overlay { Capsule().strokeBorder(Color.white, lineWidth: 1.4) }
                    .contentShape(Capsule())
            }
            .buttonStyle(FmPressStyle(highlight: Color.white.opacity(0.12), cornerRadius: 27))
            Button(action: onSignup) {
                Text("Sign up")
                    .fmText(.labelLarge, color: Color.black)
                    .frame(maxWidth: .infinity, minHeight: 54)
                    .background(Color.white)
                    .clipShape(Capsule())
            }
            .buttonStyle(FmPressStyle(highlight: Color.black.opacity(0.08), cornerRadius: 27))
        }
    }

    @ViewBuilder private var guest: some View {
        if let onGuest = onGuest {
            Button(action: onGuest) {
                HStack(spacing: FmSpace.x2) {
                    Text("Explore as guest")
                        .fmText(.labelLarge, color: Color.white)
                        .lineLimit(1)
                    FmIcon("arrow_forward", size: 18, color: Color.white)
                }
                .padding(.horizontal, FmSpace.x3)
                .frame(minHeight: 48)
            }
            .buttonStyle(FmPressStyle(highlight: Color.white.opacity(0.10), cornerRadius: 24))
            .accessibilityIdentifier("continue-as-guest")
            .frame(maxWidth: .infinity)
            .padding(.top, 14)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .center, spacing: FmSpace.x3) {
                AuthLogo(size: 54, shadow: false)
                Spacer(minLength: 0)
                // A phone signed out of one project must still be able to get back to another.
                ProjectSwitcherButton(onDark: true)
            }
            Spacer(minLength: 0)
            copy
            buttons
                .padding(.top, 34)
            guest
        }
        .padding(EdgeInsets(top: 28, leading: 28, bottom: 24, trailing: 28))
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background { background }
        .environment(\.colorScheme, ColorScheme.dark)
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
            AuthField(label: "Email or username") {
                TextField("", text: $model.identifier, prompt: Text("you@example.com"))
                    .textContentType(.username)
                    .keyboardType(.emailAddress)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
                    .submitLabel(.next)
                    .focused($focus, equals: LoginField.identifier)
                    .onSubmit { focus = LoginField.password }
            }
            AuthField(label: "Password") {
                SecureField("", text: $model.password, prompt: Text("Your password"))
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
                AuthField(label: "Authenticator code") {
                    TextField("", text: $model.totp, prompt: Text("123456"))
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
            AuthLogo(size: 72, shadow: true)
                .frame(maxWidth: .infinity)
                .padding(.top, 74)
            AuthTitle(title: AuthCopy.loginTitle, subtitle: model.relaySubtitle)
                .padding(.top, 28)
            if model.revokedBanner {
                AuthRevokedBanner()
                    .padding(.top, 48)
            }
            fields
                .padding(.top, model.revokedBanner ? 24 : 48)
            hint
            AuthPrimaryButton(label: model.buttonLabel, onPressed: model.loading ? nil : { submit() })
                .padding(.top, 42)
            AuthSecondaryButton(label: AuthCopy.forgotPassword, onPressed: onForgotPassword)
                .padding(.top, 18)
            AuthSecondaryButton(label: "Create an account", onPressed: onSignup)
                .padding(.top, 8)
            AuthTermsCopy()
                .padding(.top, 72)
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
            AuthLogo(size: 78, shadow: true)
                .frame(maxWidth: .infinity)
                .padding(.top, 58)
            AuthTitle(title: AuthCopy.signupTitle, subtitle: AuthCopy.signupSubtitle)
                .padding(.top, 28)
            Text(AuthCopy.signupBody)
                .fmText(.bodyLarge, color: fm.textSecondary)
                .padding(.top, 42)
            AuthPrimaryButton(label: "Open secure signup", onPressed: onOpenSignup)
                .padding(.top, 32)
            AuthSecondaryButton(label: "I already have an account", onPressed: onLogin)
                .padding(.top, 18)
            AuthTermsCopy()
                .padding(.top, 72)
        }
    }
}

// MARK: Pieces

/// A raised header with Back and
/// the page title, then a scrolling column at most 520pt wide.
private struct AuthPageScaffold<Content: View>: View {
    @Environment(\.fmPalette) var fm
    let pageTitle: String
    let onBack: () -> Void
    let content: Content

    init(pageTitle: String, onBack: @escaping () -> Void, @ViewBuilder content: () -> Content) {
        self.pageTitle = pageTitle
        self.onBack = onBack
        self.content = content()
    }

    private var header: some View {
        FmPanelHeader(
            title: pageTitle,
            leading: FmIconButton(icon: "chevron_left", label: "Back", size: 30, onPressed: onBack).shellGlass(Circle()).fmAny,
            trailing: ProjectSwitcherButton().fmAny)
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
                .padding(EdgeInsets(top: FmSpace.x4, leading: FmSpace.x4, bottom: FmSpace.x5, trailing: FmSpace.x4))
            }
            .scrollDismissesKeyboard(.interactively)
        }
        .background(fm.canvas.ignoresSafeArea())
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
    @Environment(\.fmPalette) var fm
    let title: String
    let subtitle: String

    var body: some View {
        VStack(spacing: 10) {
            Text(title)
                .fmText(.displaySmall)
                .multilineTextAlignment(.center)
            Text(subtitle)
                .fmText(.bodyLarge, color: fm.textSecondary)
                .multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity)
    }
}

/// The label above a filled, borderless field that turns
/// accent-edged on focus.
private struct AuthField<Field: View>: View {
    @Environment(\.fmPalette) var fm
    let label: String
    let field: Field

    init(label: String, @ViewBuilder field: () -> Field) {
        self.label = label
        self.field = field()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(label)
                .fmText(.titleSmall)
                .accessibilityHidden(true)
            field
                .fmText(.bodyLarge)
                .padding(FmSpace.x4)
                .background(fm.bgRaised)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
                .accessibilityLabel(Text(label))
        }
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
    @Environment(\.fmPalette) var fm
    let label: String
    let onPressed: (() -> Void)?

    var body: some View {
        FmFilledButton(
            label,
            background: fm.textPrimary,
            foreground: fm.dark ? FmColors.darkBgBase : FmColors.bgRaised,
            minHeight: 48,
            radius: FmRadius.md,
            fillWidth: true,
            role: .titleMedium,
            onPressed: onPressed)
            .accessibilityIdentifier("primary-auth-button-frame")
    }
}

private struct AuthSecondaryButton: View {
    @Environment(\.fmPalette) var fm
    let label: String
    let onPressed: () -> Void

    var body: some View {
        FmTextButton(label, color: fm.textPrimary, fillWidth: true, onPressed: onPressed)
    }
}

/// "By continuing, you agree to our Terms of Service and have read our
/// Privacy Policy." with both links live.
private struct AuthTermsCopy: View {
    @Environment(\.fmPalette) var fm

    private var text: AttributedString {
        var result: AttributedString = AttributedString("By continuing, you agree to our ")
        var terms: AttributedString = AttributedString("Terms of Service")
        terms.link = LegalLinks.terms
        terms.foregroundColor = fm.accent
        terms.underlineStyle = Text.LineStyle.single
        var privacy: AttributedString = AttributedString("Privacy Policy")
        privacy.link = LegalLinks.privacy
        privacy.foregroundColor = fm.accent
        privacy.underlineStyle = Text.LineStyle.single
        result.append(terms)
        result.append(AttributedString(" and have read our "))
        result.append(privacy)
        result.append(AttributedString("."))
        return result
    }

    var body: some View {
        Text(text)
            .fmText(.bodyMedium, color: fm.textSecondary)
            .tint(fm.accent)
            .padding(EdgeInsets(top: 24, leading: 10, bottom: 0, trailing: 10))
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

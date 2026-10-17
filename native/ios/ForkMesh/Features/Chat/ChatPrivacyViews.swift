import FMBase
import FMChat
import LocalAuthentication
import SwiftUI
import UIKit

@MainActor
enum ChatLockPrompt {
    static var methodName: String? {
        let context: LAContext = LAContext()
        var probe: NSError?
        _ = context.canEvaluatePolicy(.deviceOwnerAuthenticationWithBiometrics, error: &probe)
        switch context.biometryType {
        case .faceID: return "Face ID"
        case .touchID: return "Touch ID"
        case .opticID: return "Optic ID"
        default: return nil
        }
    }

    static var available: Bool {
        var probe: NSError?
        return LAContext().canEvaluatePolicy(.deviceOwnerAuthentication, error: &probe)
    }

    static func authenticate() async -> ChatUnlockResult {
        let context: LAContext = LAContext()
        var probe: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &probe) else { return .unavailable }
        do {
            let passed: Bool = try await context.evaluatePolicy(
                .deviceOwnerAuthentication, localizedReason: ChatPrivacy.unlockReason)
            return passed ? .unlocked : .failed
        } catch {
            return .failed
        }
    }
}

struct ChatLockGate<Content: View>: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.shellTabHidden) private var tabHidden
    let privacy: ChatPrivacy?
    @ViewBuilder let content: () -> Content

    var body: some View {
        if let privacy, !privacy.showsChat {
            locked(privacy)
        } else {
            content()
        }
    }

    private func locked(_ privacy: ChatPrivacy) -> some View {
        VStack(spacing: FmSpace.x3) {
            Image(systemName: "lock.fill")
                .font(.system(size: 34, weight: .regular))
                .foregroundStyle(fm.textSecondary)
                .accessibilityHidden(true)
            Text(ChatPrivacy.lockedTitle)
                .fmText(.titleMedium)
            Text(ChatPrivacy.lockedMessage(ChatLockPrompt.methodName))
                .fmText(.bodySmall, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            Button(action: { unlock(privacy, manual: true) }) {
                Text(ChatPrivacy.unlockButton)
                    .font(.system(size: 15, weight: .semibold))
                    .padding(.horizontal, FmSpace.x3)
            }
            .buttonStyle(.borderedProminent)
            .tint(fm.accent)
            .disabled(privacy.unlocking)
            .padding(.top, FmSpace.x2)
            .accessibilityIdentifier("chat-lock-unlock")
        }
        .padding(FmSpace.x6)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(fm.canvas.ignoresSafeArea())
        .onAppear { autoUnlock(privacy) }
        .onChange(of: tabHidden) { _, _ in autoUnlock(privacy) }
        .onChange(of: scenePhase) { _, _ in autoUnlock(privacy) }
    }

    private func autoUnlock(_ privacy: ChatPrivacy) {
        guard !tabHidden, scenePhase == .active, privacy.shouldPromptAutomatically else { return }
        unlock(privacy, manual: false)
    }

    private func unlock(_ privacy: ChatPrivacy, manual: Bool) {
        Task { @MainActor in
            await privacy.unlock(manual: manual) { await ChatLockPrompt.authenticate() }
        }
    }
}

@MainActor
final class ChatPrivacyShield {
    static let shared: ChatPrivacyShield = ChatPrivacyShield()
    private var covers: [ObjectIdentifier: UIWindow] = [:]
    private var observers: [NSObjectProtocol] = []
    private var enabled: @MainActor () -> Bool = { true }

    func install(enabled: @escaping @MainActor () -> Bool) {
        self.enabled = enabled
        guard observers.isEmpty else { return }
        let center: NotificationCenter = NotificationCenter.default
        for name in [UIScene.willDeactivateNotification, UIScene.didEnterBackgroundNotification] {
            observers.append(
                center.addObserver(forName: name, object: nil, queue: .main) { (note: Notification) in
                    let scene: UIWindowScene? = note.object as? UIWindowScene
                    MainActor.assumeIsolated { ChatPrivacyShield.shared.cover(scene) }
                })
        }
        observers.append(
            center.addObserver(forName: UIScene.didActivateNotification, object: nil, queue: .main) { (note: Notification) in
                let scene: UIWindowScene? = note.object as? UIWindowScene
                MainActor.assumeIsolated { ChatPrivacyShield.shared.uncover(scene) }
            })
    }

    private func cover(_ scene: UIWindowScene?) {
        guard let scene, enabled() else { return }
        let key: ObjectIdentifier = ObjectIdentifier(scene)
        if let window = covers[key] {
            window.isHidden = false
            return
        }
        let window: UIWindow = UIWindow(windowScene: scene)
        window.windowLevel = UIWindow.Level.alert + 1
        window.rootViewController = ChatPrivacyCoverController()
        window.isHidden = false
        covers[key] = window
    }

    private func uncover(_ scene: UIWindowScene?) {
        guard let scene, let window = covers.removeValue(forKey: ObjectIdentifier(scene)) else { return }
        window.isHidden = true
        window.rootViewController = nil
    }
}

private final class ChatPrivacyCoverController: UIViewController {
    override func loadView() {
        let root: UIView = UIView()
        root.backgroundColor = UIColor(named: "LaunchBackground") ?? UIColor.systemBackground
        let logo: UIImageView = UIImageView(image: UIImage(named: "Logo"))
        logo.contentMode = .scaleAspectFit
        logo.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(logo)
        NSLayoutConstraint.activate([
            logo.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            logo.centerYAnchor.constraint(equalTo: root.centerYAnchor),
            logo.widthAnchor.constraint(equalToConstant: 88),
            logo.heightAnchor.constraint(equalToConstant: 88),
        ])
        root.accessibilityElementsHidden = true
        view = root
    }
}

struct ChatPrivacyCard: View {
    @Environment(\.fmPalette) private var fm
    let privacy: ChatPrivacy

    var body: some View {
        let method: String? = ChatLockPrompt.methodName
        let canLock: Bool = ChatLockPrompt.available
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            Toggle(isOn: Binding<Bool>(get: { privacy.coversAppSwitcher }, set: { privacy.setCoversAppSwitcher($0) })) {
                Text(ChatPrivacy.coverTitle).fmText(.bodyMedium)
            }
            .tint(fm.accent)
            .accessibilityIdentifier("settings-chat-app-switcher-cover")
            SettingsNote(text: ChatPrivacy.coverNote)
            FmHairline()
            Toggle(isOn: Binding<Bool>(get: { privacy.lockEnabled }, set: { privacy.setLockEnabled($0) })) {
                Text(ChatPrivacy.lockTitle(method)).fmText(.bodyMedium)
            }
            .tint(fm.accent)
            .disabled(!canLock && !privacy.lockEnabled)
            .accessibilityIdentifier("settings-chat-lock")
            SettingsNote(text: canLock ? ChatPrivacy.lockNote(method) : ChatPrivacy.lockUnavailable)
        }
    }
}

import FMAccount
import FMApi
import FMBase
import SwiftUI
import UIKit
import UIKit.UIGestureRecognizerSubclass

@MainActor
final class FmUserActivity {
    static let shared = FmUserActivity()
    static let kind = "ios"

    let log: UserActivityLog
    let sync: UserActivitySync
    private var observers: [NSObjectProtocol] = []
    private var loop: Task<Void, Never>?

    private init() {
        let log = UserActivityLog(store: Services.get(KeyValueStore.self) ?? UserDefaultsStore())
        self.log = log
        sync = UserActivitySync(log: log, kind: Self.kind) { (payload: JSONObject) in
            guard let api = Services.get(ApiService.self) else { throw CancellationError() }
            let result = try await api.syncActivityToday(payload)
            return (result.statusCode, result.body)
        }
    }

    private var account: String {
        guard let session = Services.get(AuthService.self)?.session, session.sessionKind != "preview" else { return "" }
        return session.nodeName
    }

    @discardableResult
    func syncNow(force: Bool) async -> Bool {
        await sync.sync(account: account, force: force)
    }

    private func runLoop() {
        loop?.cancel()
        loop = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                await self.syncNow(force: false)
                try? await Task.sleep(nanoseconds: UInt64(UserActivitySync.interval * 1_000_000_000))
            }
        }
    }

    func start() {
        guard observers.isEmpty else { return }
        let log = self.log
        let center = NotificationCenter.default
        let typing: [Notification.Name] = [UITextField.textDidChangeNotification, UITextView.textDidChangeNotification]
        for name in typing {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { (_: Notification) in
                log.record()
            })
        }
        observers.append(
            center.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) {
                (_: Notification) in
                log.flush()
            })
        AppLifecycle.shared.observe { [weak self] (state: AppLifecycleState) in
            guard let self else { return }
            if state == .resumed {
                self.runLoop()
            } else if state == .paused {
                self.loop?.cancel()
                self.loop = nil
                Task { await self.syncNow(force: false) }
            }
        }
        runLoop()
    }
}

final class FmActivityRecognizer: UIGestureRecognizer, UIGestureRecognizerDelegate {
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        FmUserActivity.shared.log.record()
        state = .failed
    }

    override func canPrevent(_ preventedGestureRecognizer: UIGestureRecognizer) -> Bool { false }

    override func canBePrevented(by preventingGestureRecognizer: UIGestureRecognizer) -> Bool { false }

    func gestureRecognizer(
        _ gestureRecognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer
    ) -> Bool { true }
}

private final class FmActivityAnchor: UIView {
    override func didMoveToWindow() {
        super.didMoveToWindow()
        isUserInteractionEnabled = false
        guard let window else { return }
        FmUserActivity.shared.start()
        if (window.gestureRecognizers ?? []).contains(where: { (recognizer: UIGestureRecognizer) in recognizer is FmActivityRecognizer }) {
            return
        }
        let recognizer = FmActivityRecognizer(target: nil, action: nil)
        recognizer.cancelsTouchesInView = false
        recognizer.delaysTouchesBegan = false
        recognizer.delaysTouchesEnded = false
        recognizer.delegate = recognizer
        window.addGestureRecognizer(recognizer)
    }
}

private struct FmActivityInstaller: UIViewRepresentable {
    func makeUIView(context: Context) -> FmActivityAnchor { FmActivityAnchor() }

    func updateUIView(_ uiView: FmActivityAnchor, context: Context) {}
}

extension View {
    func fmTracksUserActivity() -> some View {
        background(FmActivityInstaller().allowsHitTesting(false))
    }
}

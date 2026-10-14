import SwiftUI
import UIKit
import UIKit.UIGestureRecognizerSubclass

@MainActor
final class FmKeyboard {
    static let shared: FmKeyboard = FmKeyboard()

    private weak var input: UIView?
    private let keepers: NSHashTable<UIView> = NSHashTable<UIView>.weakObjects()
    private var observers: [NSObjectProtocol] = []

    var isEditing: Bool {
        return input?.isFirstResponder == true
    }

    func start() {
        guard observers.isEmpty else { return }
        let center: NotificationCenter = NotificationCenter.default
        let began: [Notification.Name] = [UITextField.textDidBeginEditingNotification, UITextView.textDidBeginEditingNotification]
        for name in began {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { (note: Notification) in
                MainActor.assumeIsolated {
                    FmKeyboard.shared.began(note.object as? UIView)
                }
            })
        }
    }

    func keep(_ view: UIView) {
        keepers.add(view)
    }

    func tapped(at point: CGPoint, in window: UIWindow) {
        guard let input, input.isFirstResponder, input.window === window else { return }
        let field: CGRect = input.convert(input.bounds, to: window)
        for keeper in keepers.allObjects where keeper.window === window {
            let area: CGRect = keeper.convert(keeper.bounds, to: window)
            if area.contains(point) && area.intersects(field) {
                return
            }
        }
        input.resignFirstResponder()
    }

    static func isInput(_ view: UIView?) -> Bool {
        var current: UIView? = view
        while let candidate = current {
            if candidate is UITextInput {
                return true
            }
            current = candidate.superview
        }
        return false
    }

    private func began(_ view: UIView?) {
        input = view
    }
}

final class FmKeyboardTapRecognizer: UIGestureRecognizer, UIGestureRecognizerDelegate {
    private var origin: CGPoint?

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        guard origin == nil, touches.count == 1, (event.allTouches?.count ?? 0) == 1, let touch = touches.first,
              FmKeyboard.shared.isEditing, !FmKeyboard.isInput(touch.view) else {
            state = .failed
            return
        }
        origin = touch.location(in: view)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) {
        guard let origin, let touch = touches.first else { return }
        let point: CGPoint = touch.location(in: view)
        if hypot(point.x - origin.x, point.y - origin.y) > 10 {
            state = .failed
        }
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) {
        if let origin, let window = view as? UIWindow {
            FmKeyboard.shared.tapped(at: origin, in: window)
        }
        state = .failed
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) {
        state = .failed
    }

    override func reset() {
        super.reset()
        origin = nil
    }

    override func canPrevent(_ preventedGestureRecognizer: UIGestureRecognizer) -> Bool { false }

    override func canBePrevented(by preventingGestureRecognizer: UIGestureRecognizer) -> Bool { false }

    func gestureRecognizer(
        _ gestureRecognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer
    ) -> Bool { true }
}

private final class FmKeyboardTapAnchor: UIView {
    override func didMoveToWindow() {
        super.didMoveToWindow()
        isUserInteractionEnabled = false
        guard let window else { return }
        FmKeyboard.shared.start()
        if (window.gestureRecognizers ?? []).contains(where: { (recognizer: UIGestureRecognizer) in recognizer is FmKeyboardTapRecognizer }) {
            return
        }
        let recognizer = FmKeyboardTapRecognizer(target: nil, action: nil)
        recognizer.cancelsTouchesInView = false
        recognizer.delaysTouchesBegan = false
        recognizer.delaysTouchesEnded = false
        recognizer.delegate = recognizer
        window.addGestureRecognizer(recognizer)
    }
}

private struct FmKeyboardTapInstaller: UIViewRepresentable {
    func makeUIView(context: Context) -> FmKeyboardTapAnchor { FmKeyboardTapAnchor() }

    func updateUIView(_ uiView: FmKeyboardTapAnchor, context: Context) {}
}

private final class FmKeyboardKeeperView: UIView {
    override func didMoveToWindow() {
        super.didMoveToWindow()
        isUserInteractionEnabled = false
        if window != nil {
            FmKeyboard.shared.keep(self)
        }
    }
}

private struct FmKeyboardKeeper: UIViewRepresentable {
    func makeUIView(context: Context) -> FmKeyboardKeeperView { FmKeyboardKeeperView() }

    func updateUIView(_ uiView: FmKeyboardKeeperView, context: Context) {}
}

extension View {
    func fmDismissesKeyboardOnTap() -> some View {
        background(FmKeyboardTapInstaller().allowsHitTesting(false))
    }

    func fmKeepsKeyboard() -> some View {
        background(FmKeyboardKeeper().allowsHitTesting(false))
    }
}

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

    func dismiss() {
        if let input, input.isFirstResponder {
            input.resignFirstResponder()
            return
        }
        ShellKeyboard.dismiss()
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
        guard let view else { return }
        input = view
        if let field = view as? UITextField, field.inputAccessoryView == nil {
            field.inputAccessoryView = FmKeyboardGrabber()
            field.reloadInputViews()
        } else if let text = view as? UITextView, text.isEditable, text.inputAccessoryView == nil {
            text.inputAccessoryView = FmKeyboardGrabber()
            text.reloadInputViews()
        }
    }
}

final class FmKeyboardGrabber: UIView {
    static let height: CGFloat = 22
    private let handle: UIView = UIView()

    init() {
        super.init(frame: CGRect(x: 0, y: 0, width: 0, height: FmKeyboardGrabber.height))
        autoresizingMask = .flexibleWidth
        backgroundColor = .clear
        handle.backgroundColor = .secondaryLabel
        handle.layer.cornerRadius = 2.5
        handle.layer.cornerCurve = .continuous
        handle.isUserInteractionEnabled = false
        addSubview(handle)
        addGestureRecognizer(UIPanGestureRecognizer(target: self, action: #selector(dragged(_:))))
        addGestureRecognizer(UITapGestureRecognizer(target: self, action: #selector(tapped(_:))))
        isAccessibilityElement = true
        accessibilityLabel = "Hide keyboard"
        accessibilityTraits = .button
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override var intrinsicContentSize: CGSize {
        return CGSize(width: UIView.noIntrinsicMetric, height: FmKeyboardGrabber.height)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        handle.bounds = CGRect(x: 0, y: 0, width: 36, height: 5)
        handle.center = CGPoint(x: bounds.midX, y: bounds.midY)
    }

    override func accessibilityActivate() -> Bool {
        FmKeyboard.shared.dismiss()
        return true
    }

    @objc private func tapped(_ tap: UITapGestureRecognizer) {
        if abs(tap.location(in: self).x - bounds.midX) <= 44 {
            FmKeyboard.shared.dismiss()
        }
    }

    @objc private func dragged(_ pan: UIPanGestureRecognizer) {
        let drop: CGFloat = max(0, pan.translation(in: self).y)
        switch pan.state {
        case .changed:
            handle.transform = CGAffineTransform(translationX: 0, y: min(drop, 8))
        case .ended:
            settle()
            if drop > 16 || pan.velocity(in: self).y > 300 {
                FmKeyboard.shared.dismiss()
            }
        default:
            settle()
        }
    }

    private func settle() {
        UIView.animate(withDuration: 0.2) {
            self.handle.transform = .identity
        }
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

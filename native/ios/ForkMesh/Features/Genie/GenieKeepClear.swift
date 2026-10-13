import SwiftUI
import UIKit

@MainActor
final class GenieKeepClear {
    static let shared: GenieKeepClear = GenieKeepClear()

    private var frames: [UUID: CGRect] = [:]

    func set(_ key: UUID, _ frame: CGRect?) {
        guard let frame = frame, !frame.isNull, !frame.isEmpty else {
            frames[key] = nil
            return
        }
        frames[key] = frame
    }

    var marked: [CGRect] {
        Array(frames.values)
    }

    var buttons: [CGRect] {
        marked + GenieKeepClear.bars() + GenieScreenScan.shared.blockers
    }

    static func keyWindow() -> UIWindow? {
        let scenes: [UIWindowScene] = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let scene: UIWindowScene? = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
        return scene?.windows.first(where: { $0.isKeyWindow })
    }

    static func bars() -> [CGRect] {
        guard let window = keyWindow(), let root = window.rootViewController?.view else {
            return []
        }
        var out: [CGRect] = []
        visit(root, in: window, into: &out)
        return out
    }

    private static func visit(_ view: UIView, in window: UIWindow, into out: inout [CGRect]) {
        guard !view.isHidden, view.alpha > 0.01 else { return }
        if view is UINavigationBar || view is UIToolbar {
            let frame: CGRect = view.convert(view.bounds, to: window).intersection(window.bounds)
            guard !frame.isNull, frame.width > 1, frame.height > 1 else { return }
            let probes: [CGPoint] = [0.1, 0.5, 0.9].map { CGPoint(x: frame.minX + frame.width * $0, y: frame.midY) }
            if probes.contains(where: { window.hitTest($0, with: nil)?.isDescendant(of: view) == true }) {
                out.append(frame)
            }
            return
        }
        for sub in view.subviews {
            visit(sub, in: window, into: &out)
        }
    }
}

private final class GenieKeepClearSlot {
    let key: UUID = UUID()
    var measured: CGRect = .null
}

private struct GenieKeepsClear: ViewModifier {
    let frame: CGRect?
    @State private var slot: GenieKeepClearSlot = GenieKeepClearSlot()

    func body(content: Content) -> some View {
        content
            .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { (next: CGRect) in
                slot.measured = next
                if frame == nil {
                    GenieKeepClear.shared.set(slot.key, next)
                }
            }
            .onChange(of: frame, initial: true) { (_: CGRect?, next: CGRect?) in
                GenieKeepClear.shared.set(slot.key, next ?? slot.measured)
            }
            .onAppear { GenieKeepClear.shared.set(slot.key, frame ?? slot.measured) }
            .onDisappear { GenieKeepClear.shared.set(slot.key, nil) }
    }
}

extension View {
    func genieKeepsClear(_ frame: CGRect? = nil) -> some View {
        modifier(GenieKeepsClear(frame: frame))
    }
}

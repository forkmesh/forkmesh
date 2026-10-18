import FMTools
import UIKit

@MainActor
enum EyeClicker {
    static let maxNodes: Int = 4000
    static let maxDepth: Int = 64

    static func activeScene() -> UIWindowScene? {
        let scenes: [UIWindowScene] = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        return scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
    }

    static func appWindow() -> UIWindow? {
        guard let scene = activeScene() else { return nil }
        let windows: [UIWindow] = scene.windows.filter { !$0.isHidden && !($0 is EyePassthroughWindow) }
        return windows.first { $0.isKeyWindow } ?? windows.last { $0.windowLevel == .normal } ?? windows.last
    }

    static func click(at point: CGPoint) -> Bool {
        guard let window = appWindow() else { return false }
        let hit: UIView? = window.hitTest(point, with: nil)
        if let hit, let input = ancestor(of: hit, where: { $0 is UITextField || $0 is UITextView }), input.canBecomeFirstResponder {
            return input.becomeFirstResponder()
        }
        if let hit, let control = ancestor(of: hit, where: { $0 is UIControl }) as? UIControl, control.isEnabled {
            press(control, at: control.convert(point, from: window))
            return true
        }
        let screenPoint: CGPoint = window.convert(point, to: window.screen.coordinateSpace)
        for element in elements(at: screenPoint, in: window) where element.accessibilityActivate() {
            return true
        }
        if let hit, select(cellAround: hit) {
            return true
        }
        return false
    }

    static let maxTargets: Int = 300
    static let minTargetSize: CGFloat = 6
    static let maxTargetShare: CGFloat = 0.5
    static let activeTraits: UIAccessibilityTraits = [.button, .link, .adjustable, .keyboardKey, .searchField, .toggleButton]

    static func targets() -> [EyeTarget] {
        guard let window = appWindow() else { return [] }
        var scan: TargetScan = TargetScan(window: window)
        scan.visit(window, owner: window, depth: 0)
        return scan.found
    }

    static func precision(of view: UIView) -> Bool? {
        switch view {
        case let control as UIControl where !control.isEnabled || !control.isUserInteractionEnabled: return nil
        case is UISlider, is UISegmentedControl, is UIStepper: return true
        case is UIControl, is UISearchBar: return false
        case let text as UITextView: return text.isEditable ? false : nil
        case is UITableViewCell, is UICollectionViewCell: return view.isUserInteractionEnabled ? false : nil
        default: return nil
        }
    }

    @MainActor
    private struct TargetScan {
        let window: UIWindow
        var found: [EyeTarget] = []
        var seen: Set<[Int]> = []
        var visited: Int = 0

        init(window: UIWindow) {
            self.window = window
        }

        mutating func visit(_ node: NSObject, owner: UIView, depth: Int) {
            guard depth < EyeClicker.maxDepth, visited < EyeClicker.maxNodes, found.count < EyeClicker.maxTargets else { return }
            visited += 1
            if node.accessibilityElementsHidden { return }
            var owner: UIView = owner
            if let view = node as? UIView {
                if view.isHidden || view.alpha < 0.01 || view is EyePassthroughWindow { return }
                owner = view
                if let precise = EyeClicker.precision(of: view) {
                    record(view.convert(view.bounds, to: window), precise: precise, owner: view, id: ObjectIdentifier(view).hashValue)
                    if view is UIControl { return }
                    if view is UITableViewCell || view is UICollectionViewCell {
                        for sub in view.subviews {
                            visit(sub, owner: view, depth: depth + 1)
                        }
                        return
                    }
                }
            }
            if node.isAccessibilityElement {
                let traits: UIAccessibilityTraits = node.accessibilityTraits
                if !traits.isDisjoint(with: EyeClicker.activeTraits), !traits.contains(.notEnabled) {
                    let frame: CGRect = window.convert(node.accessibilityFrame, from: window.screen.coordinateSpace)
                    record(frame, precise: traits.contains(.adjustable), owner: owner, id: ObjectIdentifier(node).hashValue)
                }
                return
            }
            for child in EyeClicker.children(of: node) {
                visit(child, owner: owner, depth: depth + 1)
            }
        }

        mutating func record(_ frame: CGRect, precise: Bool, owner: UIView, id: Int) {
            guard !frame.isNull, !frame.isInfinite else { return }
            let bounds: CGRect = window.bounds
            let visible: CGRect = frame.intersection(bounds)
            guard !visible.isNull, visible.width >= EyeClicker.minTargetSize, visible.height >= EyeClicker.minTargetSize,
                visible.width * visible.height <= bounds.width * bounds.height * EyeClicker.maxTargetShare
            else { return }
            let key: [Int] = [visible.minX, visible.minY, visible.width, visible.height].map { Int($0.rounded()) }
            guard !seen.contains(key) else { return }
            guard let hit = window.hitTest(CGPoint(x: visible.midX, y: visible.midY), with: nil),
                hit.isDescendant(of: owner) || owner.isDescendant(of: hit)
            else { return }
            seen.insert(key)
            found.append(EyeTarget(
                id: id,
                frame: EyeRect(x: Double(visible.minX), y: Double(visible.minY), width: Double(visible.width), height: Double(visible.height)),
                precise: precise))
        }
    }

    private static func ancestor(of view: UIView, where test: (UIView) -> Bool) -> UIView? {
        var current: UIView? = view
        while let node = current {
            if test(node) { return node }
            current = node.superview
        }
        return nil
    }

    private static func press(_ control: UIControl, at point: CGPoint) {
        switch control {
        case let toggle as UISwitch:
            toggle.setOn(!toggle.isOn, animated: true)
            toggle.sendActions(for: .valueChanged)
        case let segments as UISegmentedControl where segments.numberOfSegments > 0:
            let width: CGFloat = segments.bounds.width / CGFloat(segments.numberOfSegments)
            let index: Int = min(max(Int(point.x / max(width, 1)), 0), segments.numberOfSegments - 1)
            segments.selectedSegmentIndex = index
            segments.sendActions(for: .valueChanged)
        case let slider as UISlider:
            let fraction: Float = Float(min(max(point.x / max(slider.bounds.width, 1), 0), 1))
            slider.setValue(slider.minimumValue + fraction * (slider.maximumValue - slider.minimumValue), animated: true)
            slider.sendActions(for: .valueChanged)
        default:
            if #available(iOS 17.4, *) {
                control.performPrimaryAction()
            } else {
                control.sendActions(for: .primaryActionTriggered)
            }
            control.sendActions(for: .touchUpInside)
        }
    }

    private static func elements(at point: CGPoint, in window: UIWindow) -> [NSObject] {
        var found: [(element: NSObject, area: CGFloat, active: Bool)] = []
        var visited: Int = 0
        func visit(_ node: NSObject, depth: Int) {
            guard depth < maxDepth, visited < maxNodes else { return }
            visited += 1
            if node.accessibilityElementsHidden { return }
            if let view = node as? UIView, view.isHidden || view.alpha < 0.01 { return }
            if node.isAccessibilityElement {
                let frame: CGRect = node.accessibilityFrame
                if frame.contains(point) {
                    let traits: UIAccessibilityTraits = node.accessibilityTraits
                    let active: Bool = !traits.isDisjoint(with: [.button, .link, .adjustable, .keyboardKey, .searchField, .tabBar])
                    found.append((node, frame.width * frame.height, active))
                }
                return
            }
            for child in children(of: node) {
                visit(child, depth: depth + 1)
            }
        }
        visit(window, depth: 0)
        return found
            .sorted { ($0.active ? 0 : 1, $0.area) < ($1.active ? 0 : 1, $1.area) }
            .map(\.element)
    }

    private static func children(of node: NSObject) -> [NSObject] {
        if let listed = node.accessibilityElements, !listed.isEmpty {
            return listed.compactMap { $0 as? NSObject }
        }
        let count: Int = node.accessibilityElementCount()
        if count > 0, count != NSNotFound {
            return (0..<count).compactMap { node.accessibilityElement(at: $0) as? NSObject }
        }
        return (node as? UIView)?.subviews ?? []
    }

    private static func select(cellAround view: UIView) -> Bool {
        if let cell = ancestor(of: view, where: { $0 is UICollectionViewCell }) as? UICollectionViewCell,
            let list = ancestor(of: cell, where: { $0 is UICollectionView }) as? UICollectionView,
            let path = list.indexPath(for: cell)
        {
            guard list.delegate?.collectionView?(list, shouldSelectItemAt: path) ?? true else { return false }
            list.selectItem(at: path, animated: true, scrollPosition: [])
            list.delegate?.collectionView?(list, didSelectItemAt: path)
            return true
        }
        if let cell = ancestor(of: view, where: { $0 is UITableViewCell }) as? UITableViewCell,
            let table = ancestor(of: cell, where: { $0 is UITableView }) as? UITableView,
            let path = table.indexPath(for: cell)
        {
            table.selectRow(at: path, animated: true, scrollPosition: .none)
            table.delegate?.tableView?(table, didSelectRowAt: path)
            return true
        }
        return false
    }
}

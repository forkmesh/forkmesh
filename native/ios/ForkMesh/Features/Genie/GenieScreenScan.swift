import FMAccount
import FMAssistant
import FMBase
import Foundation
import UIKit

@MainActor
final class GenieScreenScan {
    static let shared: GenieScreenScan = GenieScreenScan()
    nonisolated static let budget: Int = 2400
    nonisolated static let deepest: Int = 48
    nonisolated static let ownPrefixes: [String] = ["genie", "shell-genie", "aquarium"]

    private(set) var landmarks: [GenieLandmark] = []
    private var barFrames: [CGRect] = []
    private var schedule: GenieScanSchedule = GenieScanSchedule()

    func invalidate() {
        schedule.invalidate()
    }

    func current() -> [GenieLandmark] {
        refresh()
        return landmarks
    }

    var blockers: [CGRect] { GenieLandmarks.blockers(current()) }

    var bars: [CGRect] {
        refresh()
        return barFrames
    }

    private func refresh() {
        let now: TimeInterval = ProcessInfo.processInfo.systemUptime
        if schedule.due(at: now) {
            rescan(now)
        }
    }

    private func rescan(_ now: TimeInterval) {
        guard let window = GenieKeepClear.keyWindow() else {
            schedule.scanned(at: now, taking: 0)
            landmarks = []
            barFrames = []
            return
        }
        let (marks, frames): ([GenieLandmark], [CGRect]) = Self.timed {
            var walk: Walk = Walk(window: window)
            walk.view(window.rootViewController?.view ?? window, depth: 0, inTabBar: false)
            return (GenieLandmarks.tidy(walk.found, in: window.bounds), GenieKeepClear.bars(in: window))
        }
        schedule.scanned(at: now, taking: ProcessInfo.processInfo.systemUptime - now)
        landmarks = marks
        barFrames = frames
    }

    private static func timed<T>(_ scan: () -> T) -> T {
        guard let monitor = Services.get(PerformanceMonitorService.self) else { return scan() }
        return monitor.timeSync("genie.screenScan", scan)
    }

    @MainActor
    private struct Walk {
        let window: UIWindow
        var found: [GenieLandmark] = []
        var visited: Int = 0
        var seen: Set<ObjectIdentifier> = []

        init(window: UIWindow) {
            self.window = window
        }

        var full: Bool { visited >= GenieScreenScan.budget || found.count >= GenieLandmarks.cap * 2 }

        mutating func view(_ view: UIView, depth: Int, inTabBar: Bool) {
            guard !full, depth < GenieScreenScan.deepest, !view.isHidden, view.alpha > 0.01 else { return }
            guard seen.insert(ObjectIdentifier(view)).inserted else { return }
            visited += 1
            if GenieScreenScan.isOwn(view.accessibilityIdentifier) { return }
            let tabs: Bool = inTabBar || view is UITabBar
            if let kind = GenieScreenScan.kind(of: view, inTabBar: tabs) {
                record(kind, frame: view.convert(view.bounds, to: window), text: GenieScreenScan.text(view))
            } else if view.isAccessibilityElement {
                element(view, depth: depth, inTabBar: tabs)
            }
            if view is UITableViewCell || view is UICollectionViewCell, view.isAccessibilityElement { return }
            children(of: view, depth: depth, inTabBar: tabs)
            for sub in view.subviews {
                self.view(sub, depth: depth + 1, inTabBar: tabs)
            }
        }

        mutating func children(of container: NSObject, depth: Int, inTabBar: Bool) {
            guard !full, depth < GenieScreenScan.deepest else { return }
            var items: [NSObject] = []
            if let listed = container.accessibilityElements {
                items = listed.compactMap { $0 as? NSObject }
            } else {
                let count: Int = container.accessibilityElementCount()
                if count > 0 && count != NSNotFound {
                    for index in 0..<min(count, GenieLandmarks.cap) {
                        if let item = container.accessibilityElement(at: index) as? NSObject { items.append(item) }
                    }
                }
            }
            for item in items {
                guard !full else { return }
                if let sub = item as? UIView {
                    view(sub, depth: depth + 1, inTabBar: inTabBar)
                    continue
                }
                guard seen.insert(ObjectIdentifier(item)).inserted else { continue }
                visited += 1
                if GenieScreenScan.isOwn((item as? UIAccessibilityIdentification)?.accessibilityIdentifier) { continue }
                let tabs: Bool = inTabBar || item.accessibilityTraits.contains(.tabBar)
                if item.isAccessibilityElement {
                    element(item, depth: depth, inTabBar: tabs)
                } else {
                    children(of: item, depth: depth + 1, inTabBar: tabs)
                }
            }
        }

        mutating func element(_ item: NSObject, depth: Int, inTabBar: Bool) {
            let identifier: String? = (item as? UIAccessibilityIdentification)?.accessibilityIdentifier
            guard !GenieScreenScan.isOwn(identifier), !item.accessibilityElementsHidden else { return }
            let value: String = item.accessibilityValue ?? ""
            guard let kind = GenieLandmarks.kind(GenieScreenScan.traits(item.accessibilityTraits), value: value, inTabBar: inTabBar) else { return }
            let frame: CGRect = window.convert(item.accessibilityFrame, from: window.screen.coordinateSpace)
            record(kind, frame: frame, text: item.accessibilityLabel ?? value)
        }

        mutating func record(_ kind: GenieLandmarkKind, frame: CGRect, text: String) {
            guard !frame.isNull, !frame.isInfinite, frame.width > 0, frame.height > 0 else { return }
            found.append(GenieLandmark(kind: kind, rect: frame, text: text))
        }
    }

    nonisolated static func isOwn(_ identifier: String?) -> Bool {
        guard let identifier = identifier, !identifier.isEmpty else { return false }
        return ownPrefixes.contains { identifier.hasPrefix($0) }
    }

    static func kind(of view: UIView, inTabBar: Bool) -> GenieLandmarkKind? {
        switch view {
        case is UISwitch: return .toggle
        case is UISlider, is UIStepper: return .slider
        case is UITextField, is UISearchBar: return .field
        case let text as UITextView: return text.isEditable ? .field : nil
        case is UIProgressView: return .progress
        case is UISegmentedControl: return .tab
        case is UIButton: return inTabBar ? .tab : .button
        case let control as UIControl: return control.isUserInteractionEnabled && control.bounds.width < 400 && view.isAccessibilityElement ? .button : nil
        case let image as UIImageView: return image.image != nil && image.bounds.width >= 16 && image.bounds.height >= 16 ? .image : nil
        default: return nil
        }
    }

    static func text(_ view: UIView) -> String {
        if let label = view.accessibilityLabel, !label.isEmpty { return label }
        if let button = view as? UIButton { return button.currentTitle ?? "" }
        if let field = view as? UITextField { return field.placeholder ?? "" }
        return ""
    }

    static func traits(_ traits: UIAccessibilityTraits) -> GenieTraits {
        var out: GenieTraits = []
        if traits.contains(.button) { out.insert(.button) }
        if traits.contains(.link) { out.insert(.link) }
        if traits.contains(.header) { out.insert(.header) }
        if traits.contains(.image) { out.insert(.image) }
        if traits.contains(.staticText) { out.insert(.staticText) }
        if traits.contains(.searchField) { out.insert(.searchField) }
        if traits.contains(.tabBar) { out.insert(.tabBar) }
        if traits.contains(.adjustable) { out.insert(.adjustable) }
        if traits.contains(.toggleButton) { out.insert(.toggle) }
        if traits.contains(.updatesFrequently) { out.insert(.updates) }
        if traits.contains(.keyboardKey) { out.insert(.keyboardKey) }
        return out
    }
}

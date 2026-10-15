import Foundation

#if canImport(CoreGraphics)
import CoreGraphics
#endif

public enum GenieLandmarkKind: String, Sendable, CaseIterable {
    case button, link, tab, field, toggle, slider, image, heading, text, progress

    public var keepsClear: Bool {
        switch self {
        case .button, .link, .tab, .field, .toggle, .slider, .image: return true
        case .heading, .text, .progress: return false
        }
    }
}

public struct GenieTraits: OptionSet, Sendable, Hashable {
    public let rawValue: Int

    public init(rawValue: Int) {
        self.rawValue = rawValue
    }

    public static let button = GenieTraits(rawValue: 1 << 0)
    public static let link = GenieTraits(rawValue: 1 << 1)
    public static let header = GenieTraits(rawValue: 1 << 2)
    public static let image = GenieTraits(rawValue: 1 << 3)
    public static let staticText = GenieTraits(rawValue: 1 << 4)
    public static let searchField = GenieTraits(rawValue: 1 << 5)
    public static let tabBar = GenieTraits(rawValue: 1 << 6)
    public static let adjustable = GenieTraits(rawValue: 1 << 7)
    public static let toggle = GenieTraits(rawValue: 1 << 8)
    public static let updates = GenieTraits(rawValue: 1 << 9)
    public static let keyboardKey = GenieTraits(rawValue: 1 << 10)
    public static let textEntry = GenieTraits(rawValue: 1 << 11)
    public static let progress = GenieTraits(rawValue: 1 << 12)
}

public struct GenieLandmark: Equatable, Sendable {
    public var kind: GenieLandmarkKind
    public var rect: CGRect
    public var text: String

    public init(kind: GenieLandmarkKind, rect: CGRect, text: String = "") {
        self.kind = kind
        self.rect = rect
        self.text = text
    }

    public var keepsClear: Bool { kind.keepsClear }

    public var center: CGPoint { CGPoint(x: rect.midX, y: rect.midY) }
}

public struct GenieScanSchedule: Equatable, Sendable {
    public private(set) var scannedAt: Double?
    public private(set) var spacing: Double = GenieLandmarks.refresh
    public private(set) var stale: Bool = true

    public init() {}

    public mutating func invalidate() {
        stale = true
    }

    public func due(at now: Double) -> Bool {
        guard let scannedAt else { return true }
        return stale || now - scannedAt >= spacing
    }

    public mutating func scanned(at now: Double, taking seconds: Double) {
        scannedAt = now
        stale = false
        spacing = GenieLandmarks.spacing(afterScanTaking: seconds)
    }
}

public enum GenieLandmarks {
    public nonisolated static let cap: Int = 300
    public nonisolated static let refresh: Double = 10
    public nonisolated static let poll: Double = 2
    public nonisolated static let mainShare: Double = 0.01
    public nonisolated static let slowest: Double = 60
    public nonisolated static let smallest: CGFloat = 6
    public nonisolated static let largest: Double = 0.35
    public nonisolated static let longest: Int = 80

    public static func spacing(afterScanTaking seconds: Double) -> Double {
        min(slowest, max(refresh, seconds / mainShare))
    }

    public static func kind(_ traits: GenieTraits, value: String = "", inTabBar: Bool = false) -> GenieLandmarkKind? {
        if traits.contains(.searchField) || traits.contains(.textEntry) { return .field }
        if traits.contains(.toggle) { return .toggle }
        if traits.contains(.button) && (value == "0" || value == "1") { return .toggle }
        if traits.contains(.adjustable) { return .slider }
        if traits.contains(.link) { return .link }
        if traits.contains(.button) || traits.contains(.keyboardKey) { return inTabBar || traits.contains(.tabBar) ? .tab : .button }
        if traits.contains(.tabBar) { return .tab }
        if traits.contains(.header) { return .heading }
        if traits.contains(.progress) || (traits.contains(.updates) && value.hasSuffix("%")) { return .progress }
        if traits.contains(.image) { return .image }
        if traits.contains(.staticText) { return .text }
        return nil
    }

    public static func clean(_ raw: String) -> String {
        let flat: String = raw.split(whereSeparator: { $0.isNewline || $0 == "\t" }).joined(separator: " ")
        let trimmed: String = flat.trimmingCharacters(in: .whitespaces)
        return trimmed.count > longest ? String(trimmed.prefix(longest)) : trimmed
    }

    public static func tidy(_ raw: [GenieLandmark], in screen: CGRect, cap: Int = GenieLandmarks.cap) -> [GenieLandmark] {
        let area: Double = Double(screen.width * screen.height)
        guard area > 0 else { return [] }
        var kept: [GenieLandmark] = []
        for mark in raw {
            let clipped: CGRect = mark.rect.intersection(screen)
            guard !clipped.isNull, clipped.width >= smallest, clipped.height >= smallest else { continue }
            let share: Double = Double(clipped.width * clipped.height) / area
            if share > (mark.keepsClear ? largest : 0.5) { continue }
            let next: GenieLandmark = GenieLandmark(kind: mark.kind, rect: clipped, text: clean(mark.text))
            if kept.contains(where: { $0.kind == next.kind && same($0.rect, next.rect) }) { continue }
            kept.append(next)
        }
        let blockers: [GenieLandmark] = kept.filter(\.keepsClear)
        let others: [GenieLandmark] = kept.filter { !$0.keepsClear }
        return Array((blockers + others).prefix(max(cap, 0)))
    }

    public static func blockers(_ marks: [GenieLandmark]) -> [CGRect] {
        marks.filter(\.keepsClear).map(\.rect)
    }

    public static func overlaps(_ box: CGRect, _ rects: [CGRect], slack: CGFloat = 1) -> Bool {
        rects.contains { (rect: CGRect) -> Bool in
            let cut: CGRect = rect.intersection(box)
            return !cut.isNull && cut.width > slack && cut.height > slack
        }
    }

    static func same(_ a: CGRect, _ b: CGRect) -> Bool {
        abs(a.minX - b.minX) <= 2 && abs(a.minY - b.minY) <= 2 && abs(a.maxX - b.maxX) <= 2 && abs(a.maxY - b.maxY) <= 2
    }
}

public struct GenieBag<Item: Equatable & Sendable>: Sendable {
    public let items: [Item]
    public private(set) var left: [Item] = []
    public private(set) var last: Item?

    public init(_ items: [Item]) {
        self.items = items
    }

    public mutating func next(random: () -> Double, fits: (Item) -> Bool = { _ in true }) -> Item? {
        if left.isEmpty { refill(random) }
        if let found = take(fits) { return found }
        guard items.contains(where: fits) else { return nil }
        refill(random)
        return take(fits)
    }

    private mutating func take(_ fits: (Item) -> Bool) -> Item? {
        guard let index = left.firstIndex(where: fits) else { return nil }
        let item: Item = left.remove(at: index)
        last = item
        return item
    }

    private mutating func refill(_ random: () -> Double) {
        var deck: [Item] = items
        if deck.count > 1 {
            for index in stride(from: deck.count - 1, to: 0, by: -1) {
                let other: Int = min(Int(random() * Double(index + 1)), index)
                deck.swapAt(index, other)
            }
            if let last = last, deck.first == last { deck.append(deck.removeFirst()) }
        }
        left = deck
    }
}

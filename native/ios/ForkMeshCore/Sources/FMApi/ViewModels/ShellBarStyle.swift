import FMBase
import Foundation

public enum ShellBarShape: String, CaseIterable, Codable, Sendable {
    case capsule
    case panel
    case squares
    case circles
    case icons

    public var title: String {
        switch self {
        case .capsule: return "Capsule"
        case .panel: return "Panel"
        case .squares: return "Squares"
        case .circles: return "Circles"
        case .icons: return "Icons only"
        }
    }

    public var symbol: String {
        switch self {
        case .capsule: return "capsule"
        case .panel: return "rectangle"
        case .squares: return "square.split.2x1"
        case .circles: return "circle.grid.2x1"
        case .icons: return "square.dashed"
        }
    }

    public var separate: Bool { self == .squares || self == .circles || self == .icons }

    public var cornered: Bool { self == .panel || self == .squares }

    public var backed: Bool { self != .icons }
}

public enum ShellBarFill: String, CaseIterable, Codable, Sendable {
    case glass
    case clear
    case tinted
    case solid

    public var title: String {
        switch self {
        case .glass: return "Glass"
        case .clear: return "Clear"
        case .tinted: return "Tinted"
        case .solid: return "Solid"
        }
    }

    public var symbol: String {
        switch self {
        case .glass: return "drop"
        case .clear: return "circle.dotted"
        case .tinted: return "drop.fill"
        case .solid: return "square.fill"
        }
    }
}

public enum ShellBarLabels: String, CaseIterable, Codable, Sendable {
    case always
    case selected
    case never

    public var title: String {
        switch self {
        case .always: return "Always"
        case .selected: return "Selected"
        case .never: return "Never"
        }
    }

    public func shows(selected: Bool) -> Bool {
        switch self {
        case .always: return true
        case .selected: return selected
        case .never: return false
        }
    }
}

public enum ShellBarSelection: String, CaseIterable, Codable, Sendable {
    case fill
    case outline
    case glow
    case dot
    case underline
    case none

    public var title: String {
        switch self {
        case .fill: return "Fill"
        case .outline: return "Outline"
        case .glow: return "Glow"
        case .dot: return "Dot"
        case .underline: return "Line"
        case .none: return "None"
        }
    }

    public var symbol: String {
        switch self {
        case .fill: return "square.fill"
        case .outline: return "square"
        case .glow: return "sun.max"
        case .dot: return "circle.fill"
        case .underline: return "minus"
        case .none: return "nosign"
        }
    }
}

public enum ShellBarSize: String, CaseIterable, Codable, Sendable {
    case small
    case medium
    case large
    case extraLarge

    public var title: String {
        switch self {
        case .small: return "S"
        case .medium: return "M"
        case .large: return "L"
        case .extraLarge: return "XL"
        }
    }

    public var name: String {
        switch self {
        case .small: return "Small"
        case .medium: return "Medium"
        case .large: return "Large"
        case .extraLarge: return "Extra large"
        }
    }

    public var scale: Double {
        switch self {
        case .small: return 0.85
        case .medium: return 1
        case .large: return 1.15
        case .extraLarge: return 1.3
        }
    }
}

public enum ShellBarWidth: String, CaseIterable, Codable, Sendable {
    case full
    case fit

    public var title: String {
        switch self {
        case .full: return "Full width"
        case .fit: return "Fit the icons"
        }
    }
}

public struct ShellBarMetrics: Equatable, Sendable {
    public let slot: Double
    public let height: Double
    public let gap: Double
    public let padding: Double
    public let width: Double
    public let scrolls: Bool

    public init(slot: Double, height: Double, gap: Double, padding: Double, width: Double, scrolls: Bool) {
        self.slot = slot
        self.height = height
        self.gap = gap
        self.padding = padding
        self.width = width
        self.scrolls = scrolls
    }
}

public struct ShellBarStyle: Equatable, Codable, Sendable {
    public static let key = "shell/bar-style"
    public static let limitChoices: [Int] = [3, 4, 5, 6, 7, 8]
    public static let minimumSlot: Double = 44
    public static let padding: Double = 4

    public var shape: ShellBarShape = .capsule
    public var fill: ShellBarFill = .glass
    public var icons: ShellHomeIconStyle = .outline
    public var size: ShellBarSize = .medium
    public var labels: ShellBarLabels = .always
    public var selection: ShellBarSelection = .fill
    public var width: ShellBarWidth = .full
    public var corner: Double = 0.5
    public var spacing: Double = 0
    public var limit: Int = ShellBarLayout.barLimit
    public var showCounts: Bool = false
    public var showActivity: Bool = true
    public var highlightFrequent: Bool = false

    public init() {}

    private enum CodingKeys: String, CodingKey {
        case shape, fill, icons, size, labels, selection, width, corner, spacing, limit, showCounts, showActivity
        case highlightFrequent
    }

    public init(from decoder: Decoder) throws {
        let values: KeyedDecodingContainer<CodingKeys> = try decoder.container(keyedBy: CodingKeys.self)
        let base: ShellBarStyle = ShellBarStyle()
        shape = (try? values.decodeIfPresent(ShellBarShape.self, forKey: .shape)) ?? base.shape
        fill = (try? values.decodeIfPresent(ShellBarFill.self, forKey: .fill)) ?? base.fill
        icons = (try? values.decodeIfPresent(ShellHomeIconStyle.self, forKey: .icons)) ?? base.icons
        size = (try? values.decodeIfPresent(ShellBarSize.self, forKey: .size)) ?? base.size
        labels = (try? values.decodeIfPresent(ShellBarLabels.self, forKey: .labels)) ?? base.labels
        selection = (try? values.decodeIfPresent(ShellBarSelection.self, forKey: .selection)) ?? base.selection
        width = (try? values.decodeIfPresent(ShellBarWidth.self, forKey: .width)) ?? base.width
        corner = ShellBarStyle.unit((try? values.decodeIfPresent(Double.self, forKey: .corner)) ?? base.corner)
        spacing = ShellBarStyle.unit((try? values.decodeIfPresent(Double.self, forKey: .spacing)) ?? base.spacing)
        limit = ShellBarStyle.clampedLimit((try? values.decodeIfPresent(Int.self, forKey: .limit)) ?? base.limit)
        showCounts = (try? values.decodeIfPresent(Bool.self, forKey: .showCounts)) ?? base.showCounts
        showActivity = (try? values.decodeIfPresent(Bool.self, forKey: .showActivity)) ?? base.showActivity
        highlightFrequent =
            (try? values.decodeIfPresent(Bool.self, forKey: .highlightFrequent)) ?? base.highlightFrequent
    }

    public static func unit(_ value: Double) -> Double {
        guard value.isFinite else { return 0 }
        return min(1, max(0, value))
    }

    public static func clampedLimit(_ value: Int) -> Int {
        min(limitChoices.last ?? value, max(limitChoices.first ?? value, value))
    }

    public var clamped: ShellBarStyle {
        var next: ShellBarStyle = self
        next.corner = ShellBarStyle.unit(corner)
        next.spacing = ShellBarStyle.unit(spacing)
        next.limit = ShellBarStyle.clampedLimit(limit)
        return next
    }

    public static func load(_ store: KeyValueStore) -> ShellBarStyle {
        guard let text = store.string(forKey: key), let data = text.data(using: .utf8),
            let style = try? JSONDecoder().decode(ShellBarStyle.self, from: data)
        else { return ShellBarStyle() }
        return style
    }

    public func save(_ store: KeyValueStore) {
        guard self != ShellBarStyle(), let data = try? JSONEncoder().encode(self),
            let text = String(data: data, encoding: .utf8)
        else {
            store.remove(ShellBarStyle.key)
            return
        }
        store.set(text, forKey: ShellBarStyle.key)
    }

    public var isDefault: Bool { self == ShellBarStyle() }

    public var gap: Double {
        shape.separate ? 4 + 12 * spacing : 2 + 10 * spacing
    }

    public func cellHeight(_ base: Double) -> Double {
        let scaled: Double = base * size.scale * (labels == .never ? 0.8 : 1)
        return max(ShellBarStyle.minimumSlot, scaled.rounded())
    }

    public func barRadius(height: Double) -> Double {
        let outer: Double = height / 2 + ShellBarStyle.padding
        switch shape {
        case .capsule: return outer
        case .panel: return 4 + corner * (outer - 4)
        case .squares, .circles, .icons: return 0
        }
    }

    public func cellRadius(side: Double) -> Double {
        switch shape {
        case .circles: return side / 2
        case .squares: return 2 + corner * (side / 2 - 2)
        case .capsule, .panel, .icons: return 0
        }
    }

    public func metrics(available: Double, count: Int, cell: Double, height base: Double) -> ShellBarMetrics {
        let n: Double = Double(max(1, count))
        let pad: Double = ShellBarStyle.padding
        var gap: Double = self.gap
        let height: Double = cellHeight(base)
        let room: Double = max(0, available - 2 * pad - gap * (n - 1)) / n
        let slot: Double
        let side: Double
        switch shape {
        case .squares, .circles:
            side = max(ShellBarStyle.minimumSlot, min(height, available > 0 ? room : height))
            slot = side
        case .capsule, .panel, .icons:
            slot = max(ShellBarStyle.minimumSlot, min(cell * size.scale, available > 0 ? room : cell * size.scale))
            side = height
        }
        var content: Double = 2 * pad + n * slot + gap * (n - 1)
        if available > 0, width == .full, shape.separate, count > 1, content < available {
            gap = (available - 2 * pad - n * slot) / (n - 1)
            content = available
        }
        let shown: Double = available > 0 ? (width == .full ? available : min(available, content)) : content
        return ShellBarMetrics(
            slot: slot.rounded(.down), height: side.rounded(.down), gap: gap, padding: pad, width: shown,
            scrolls: available > 0 && content > available + 0.5)
    }
}

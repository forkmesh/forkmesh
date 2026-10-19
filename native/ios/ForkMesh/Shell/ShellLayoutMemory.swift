import FMBase
import SwiftUI

@MainActor
enum ShellLayoutMemory {
    enum Slot: String, CaseIterable {
        case barHeight, safeBottom, chromeHeight, homeHeight, homeHeader
    }

    private static var values: [Slot: CGFloat] = [:]

    private static var store: KeyValueStore {
        Services.get(KeyValueStore.self) ?? UserDefaultsStore()
    }

    static func key(_ slot: Slot) -> String {
        "shell.layout.\(slot.rawValue)"
    }

    static func value(_ slot: Slot) -> CGFloat {
        if let known = values[slot] { return known }
        let saved: CGFloat = CGFloat(max(0, store.double(forKey: key(slot)) ?? 0))
        values[slot] = saved
        return saved
    }

    static func remember(_ slot: Slot, _ next: CGFloat) {
        guard next > 0, abs(value(slot) - next) >= 0.5 else { return }
        values[slot] = next
        store.set(Double(next), forKey: key(slot))
    }
}

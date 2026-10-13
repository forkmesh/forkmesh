// New for the native app (WS7): the "add host" form. The choices are the desktop's own
// (desktop/src/ControlNode.cpp vultrPlanChoices): every plan it would deploy a mirror on, cheapest
// first, each with the US regions it deploys in, in the desktop's order of preference. Leaving the
// defaults alone creates exactly what the desktop's Create button does.

import FMBase
import Foundation

public struct AddHostForm: Equatable, Sendable {
    public let options: HostCreateOptions
    /// Empty lets the desktop pick the next free mirror name.
    public var name: String
    public var planId: String
    public var region: String

    public init(options: HostCreateOptions) {
        self.options = options
        name = ""
        planId = options.defaultPlan.isEmpty ? (options.plans.first?.id ?? "") : options.defaultPlan
        region = ""
        region = defaultRegion(for: planId)
    }

    public var plan: HostPlanChoice? { options.plans.first { $0.id == planId } }

    /// Regions the chosen plan deploys in.
    public var regions: [String] { plan?.regions ?? [] }

    public mutating func choosePlan(_ id: String) {
        planId = id
        if !regions.contains(region) { region = defaultRegion(for: id) }
    }

    private func defaultRegion(for id: String) -> String {
        let regions = options.plans.first { $0.id == id }?.regions ?? []
        if id == options.defaultPlan, regions.contains(options.defaultRegion) { return options.defaultRegion }
        return regions.first ?? ""
    }

    /// The name the host will get: the typed one, else the desktop's suggestion.
    public var effectiveName: String {
        let typed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return typed.isEmpty ? options.suggestedName : typed
    }

    /// Why the form cannot be sent, or nil.
    public var problem: String? {
        if !options.vultr { return "The desktop has no Vultr API key. Add one on the desktop's Hosts page." }
        if options.plans.isEmpty { return "Vultr offers no plan a mirror can run on right now." }
        if plan == nil { return "Pick a plan." }
        if region.isEmpty { return "Pick a region." }
        let typed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if !typed.isEmpty && !AddHostForm.validName(typed) {
            return "Use letters, digits, dots, dashes or underscores, starting with a letter or digit."
        }
        return nil
    }

    /// The same alphabet the desktop accepts for a host name.
    public static func validName(_ name: String) -> Bool {
        guard let first = name.unicodeScalars.first, name.count <= 63 else { return false }
        let alnum = CharacterSet(charactersIn: "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
        let rest = alnum.union(CharacterSet(charactersIn: "._-"))
        return alnum.contains(first) && name.unicodeScalars.allSatisfy { rest.contains($0) }
    }

    /// Plan and region to send: empty when they are the desktop's defaults, so an older desktop
    /// that ignores them still creates the same server.
    public var requestedPlan: String { planId == options.defaultPlan ? "" : planId }
    public var requestedRegion: String {
        planId == options.defaultPlan && region == options.defaultRegion ? "" : region
    }
}

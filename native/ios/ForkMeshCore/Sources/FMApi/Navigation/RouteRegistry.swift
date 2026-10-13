// Gate 2 (docs/native/NAVIGATION.md §3.1): features register a destination for a route kind from
// their installer (`registry.register(.calendar) { route in AnyView(CalendarScreen()) }`); the shell
// asks `destination(for:)` in its single `navigationDestination(for: Route.self)`. A route nobody
// has registered yet gets the placeholder, which names the owning workstream, so the whole sitemap
// is navigable from day 1.
//
// Generic over the destination type so it builds and tests on Linux; the app uses
// `RouteRegistry<AnyView>`. WS8 owns this file after Gate 2.

import Foundation

@MainActor
public final class RouteRegistry<Destination> {
    public typealias Builder = @MainActor (Route) -> Destination

    private var builders: [Route.Kind: Builder] = [:]
    private var owners: [Route.Kind: String] = [:]
    private let placeholder: @MainActor (Route) -> Destination

    /// `placeholder` builds the "Coming soon" screen for an unregistered route.
    public init(placeholder: @escaping @MainActor (Route) -> Destination) {
        self.placeholder = placeholder
    }

    /// Registers (or replaces) the destination for every route of `kind`. `by` names the
    /// registrant for diagnostics; a second registration from another owner replaces the first
    /// and is reported by `conflicts`.
    public func register(_ kind: Route.Kind, by owner: String = "", _ builder: @escaping Builder) {
        if let previous = owners[kind], previous != owner, !previous.isEmpty, !owner.isEmpty {
            conflicts.append((kind, previous, owner))
        }
        builders[kind] = builder
        owners[kind] = owner
    }

    public func unregister(_ kind: Route.Kind) {
        builders.removeValue(forKey: kind)
        owners.removeValue(forKey: kind)
    }

    public func isRegistered(_ kind: Route.Kind) -> Bool { builders[kind] != nil }

    /// Kinds registered twice by different owners: (kind, earlier owner, later owner).
    public private(set) var conflicts: [(Route.Kind, String, String)] = []

    /// The kinds still showing the placeholder.
    public var unregisteredKinds: [Route.Kind] { Route.Kind.allCases.filter { builders[$0] == nil } }

    public func destination(for route: Route) -> Destination {
        if let builder = builders[route.kind] { return builder(route) }
        return placeholder(route)
    }
}

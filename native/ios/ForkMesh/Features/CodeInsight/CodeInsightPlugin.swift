import FMApi
import Foundation

enum CodeInsightPlugin: String, CaseIterable, Identifiable, Hashable {
    case ember
    case strata
    case synapse

    var id: String { rawValue }

    var packageId: String { rawValue }

    var title: String {
        switch self {
        case .ember: return "Ember"
        case .strata: return "Strata"
        case .synapse: return "Synapse"
        }
    }

    var systemImage: String {
        switch self {
        case .ember: return "flame"
        case .strata: return "square.3.layers.3d"
        case .synapse: return "brain"
        }
    }

    var tagline: String {
        switch self {
        case .ember: return "Hotspots and code health"
        case .strata: return "Architecture, rules and technical debt"
        case .synapse: return "The import graph as a 3D brain"
        }
    }

    func route(_ repo: RepoRef?) -> Route {
        switch self {
        case .ember: return .ember(repo)
        case .strata: return .strata(repo)
        case .synapse: return .synapse(repo)
        }
    }

    var isShown: Bool { PackageGate.shows(route(nil)) }
}

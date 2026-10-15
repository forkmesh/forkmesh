import FMApi
import FMSurfaces
import SwiftUI
import WidgetKit

struct SurfaceEntry: TimelineEntry {
    let date: Date
    let snapshot: SurfaceSnapshot?
    let preview: Bool

    var signedIn: Bool { snapshot?.signedIn ?? false }

    var agents: AgentsSurface { snapshot?.agents ?? .empty }

    var inbox: InboxSurface { snapshot?.inbox ?? .empty }

    var work: WorkSurface { snapshot?.work ?? .empty }

    var updated: Date? {
        guard let ms = snapshot?.generatedAtMs, ms > 0 else { return nil }
        return Date(timeIntervalSince1970: TimeInterval(ms) / 1_000)
    }
}

struct SurfaceProvider: TimelineProvider {
    func placeholder(in context: Context) -> SurfaceEntry {
        SurfaceEntry(date: Date(), snapshot: .placeholder, preview: true)
    }

    func getSnapshot(in context: Context, completion: @escaping (SurfaceEntry) -> Void) {
        if context.isPreview {
            completion(SurfaceEntry(date: Date(), snapshot: .placeholder, preview: true))
            return
        }
        completion(SurfaceProvider.current())
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<SurfaceEntry>) -> Void) {
        let entry = SurfaceProvider.current()
        var policy: TimelineReloadPolicy = .never
        if let ends = entry.snapshot?.timer?.endsAt, ends > Date() {
            policy = .after(ends.addingTimeInterval(1))
        }
        completion(Timeline(entries: [entry], policy: policy))
    }

    static func current() -> SurfaceEntry {
        SurfaceEntry(date: Date(), snapshot: SurfaceSnapshotStore.shared()?.read(), preview: false)
    }
}

enum WidgetRoute {
    static func url(_ route: Route) -> URL? { SurfaceLink.url(for: route) }
}

struct WidgetRouteLink<Content: View>: View {
    let route: Route
    let content: Content

    init(_ route: Route, @ViewBuilder content: () -> Content) {
        self.route = route
        self.content = content()
    }

    var body: some View {
        if let url = WidgetRoute.url(route) {
            Link(destination: url) { content }
        } else {
            content
        }
    }
}

struct SignedOutWidgetView: View {
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Image(systemName: "person.crop.circle.badge.questionmark")
                .font(.title3)
                .foregroundStyle(.secondary)
            Text("Sign in to ForkMesh")
                .font(.footnote.weight(.semibold))
            Text("Open the app to see your agents and inbox here.")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}

struct CountBadge: View {
    let value: Int
    let label: String
    let systemImage: String
    let tint: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Label {
                Text(label)
            } icon: {
                Image(systemName: systemImage)
            }
            .font(.caption2.weight(.semibold))
            .foregroundStyle(tint)
            Text("\(value)")
                .font(.title2.weight(.bold))
                .monospacedDigit()
                .contentTransition(.numericText())
        }
        .accessibilityElement(children: .combine)
    }
}

extension AgentRunState {
    var tint: Color {
        switch self {
        case .needsApproval: return .orange
        case .running, .waiting: return .blue
        case .queued: return .gray
        case .completed: return .green
        case .failed: return .red
        case .stopped: return .secondary
        }
    }
}

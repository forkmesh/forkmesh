import FMApi
import Foundation

public struct InboxSurfaceItem: Codable, Hashable, Sendable, Identifiable {
    public var id: String
    public var title: String
    public var subtitle: String
    public var kind: String
    public var atMs: Int64
    public var route: Route?

    public init(id: String, title: String, subtitle: String, kind: String, atMs: Int64, route: Route?) {
        self.id = id
        self.title = title
        self.subtitle = subtitle
        self.kind = kind
        self.atMs = atMs
        self.route = route
    }

    public var destination: Route { route ?? .inbox(.all) }
}

public struct InboxSurface: Codable, Hashable, Sendable {
    public static let maxItems = 3

    public var unread: Int
    public var items: [InboxSurfaceItem]

    public init(unread: Int = 0, items: [InboxSurfaceItem] = []) {
        self.unread = unread
        self.items = items
    }

    public static let empty = InboxSurface()

    public static func build(
        unread: Int, notifications: [ForkNotification], limit: Int = InboxSurface.maxItems
    ) -> InboxSurface {
        let unreadItems = notifications.filter { $0.readAt <= 0 }.sorted { $0.ts > $1.ts }
        let items = unreadItems.prefix(max(0, limit)).map { note -> InboxSurfaceItem in
            let title = note.title.isEmpty ? note.kindLabel : note.title
            let subtitle = note.repo.isEmpty ? note.kindLabel : note.repo
            return InboxSurfaceItem(
                id: SurfaceText.clip(note.id, 80),
                title: SurfaceText.clip(title, 80),
                subtitle: SurfaceText.clip(subtitle, 80),
                kind: SurfaceText.clip(note.kind, 40),
                atMs: Int64(note.ts),
                route: InboxRoutes.route(kind: note.kind, href: note.href, meta: note.meta, repo: note.repo))
        }
        return InboxSurface(unread: max(0, unread), items: Array(items))
    }
}

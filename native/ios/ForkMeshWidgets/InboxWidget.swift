import AppIntents
import FMApi
import FMSurfaces
import SwiftUI
import WidgetKit

struct InboxWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: SurfaceSection.inbox.widgetKind, provider: SurfaceProvider()) { entry in
            InboxWidgetView(entry: entry)
                .containerBackground(.fill.tertiary, for: .widget)
        }
        .configurationDisplayName("Inbox")
        .description("Unread pings, reviews and mentions.")
        .supportedFamilies([.systemSmall, .systemMedium, .accessoryCircular, .accessoryRectangular])
    }
}

struct InboxWidgetView: View {
    @Environment(\.widgetFamily) private var family
    let entry: SurfaceEntry

    private var inbox: InboxSurface { entry.inbox }

    var body: some View {
        Group {
            switch family {
            case .accessoryCircular:
                ZStack {
                    AccessoryWidgetBackground()
                    VStack(spacing: 0) {
                        Image(systemName: "tray.full")
                            .font(.caption)
                        Text("\(inbox.unread)")
                            .font(.title3.weight(.bold))
                            .monospacedDigit()
                    }
                }
                .accessibilityLabel("\(inbox.unread) unread")
            case .accessoryRectangular:
                VStack(alignment: .leading, spacing: 1) {
                    Label("\(inbox.unread) unread", systemImage: "tray.full")
                        .font(.caption.weight(.semibold))
                    if let first = inbox.items.first {
                        Text(first.title)
                            .font(.caption2)
                            .lineLimit(2)
                            .privacySensitive()
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            case .systemMedium:
                list(limit: 3)
            default:
                list(limit: 1)
            }
        }
        .widgetURL(WidgetRoute.url(.inbox(.all)))
    }

    @ViewBuilder private func list(limit: Int) -> some View {
        if !entry.signedIn {
            SignedOutWidgetView()
        } else {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Label("Inbox", systemImage: "tray.full")
                        .font(.caption.weight(.semibold))
                    Spacer()
                    Text("\(inbox.unread)")
                        .font(.title3.weight(.bold))
                        .monospacedDigit()
                        .foregroundStyle(inbox.unread > 0 ? Color.accentColor : .secondary)
                }
                if inbox.items.isEmpty {
                    Spacer(minLength: 0)
                    Text("All caught up")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 0)
                } else {
                    ForEach(inbox.items.prefix(limit)) { item in
                        Button(intent: OpenSurfaceRouteIntent(route: item.destination)) {
                            VStack(alignment: .leading, spacing: 0) {
                                Text(item.title)
                                    .font(.caption.weight(.semibold))
                                    .lineLimit(limit == 1 ? 3 : 1)
                                Text(item.subtitle)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .privacySensitive()
                    }
                    Spacer(minLength: 0)
                }
            }
        }
    }
}

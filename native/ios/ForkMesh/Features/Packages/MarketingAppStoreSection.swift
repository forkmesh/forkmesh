import Charts
import FMBase
import FMPackages
import SwiftUI

struct MarketingAppStoreSection: View {
    let model: MarketingModel

    var body: some View {
        Group {
            if let store = model.appStore {
                if store.isEmpty {
                    Section("App Store") {
                        Text(MarketingAppStore.emptyText).font(.callout).foregroundStyle(.secondary)
                            .fmMessageTools(MarketingAppStore.emptyText)
                    }
                } else {
                    downloads(store)
                    shares("Top countries", model.appStoreWindow?.territories ?? [], total: model.appStoreWindow?.territoryCount ?? 0,
                           countries: true)
                    shares("Devices", model.appStoreWindow?.devices ?? [], total: 0, countries: false)
                }
                reviews(store)
                versions(store)
                listing(store)
            } else if model.loadingAppStore {
                Section("App Store") { ProgressView() }
            }
        }
        .task { await model.loadAppStore() }
    }

    private func downloads(_ store: MarketingAppStore) -> some View {
        Section {
            Picker("Range", selection: Bindable(model).appStoreRange) {
                ForEach(MarketingAppStore.ranges, id: \.self) { Text(MarketingAppStore.rangeTitle($0)).tag($0) }
            }
            .pickerStyle(.segmented)
            MarketingAppStoreChart(days: model.appStoreChartDays)
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 100), spacing: 8)], spacing: 8) {
                ForEach(tiles(store), id: \.label) { tile in
                    MarketingAppStoreTile(value: tile.value, label: tile.label, detail: tile.detail, tint: tile.tint)
                }
            }
            .padding(.vertical, 4)
        } header: {
            Text("App Store downloads")
        } footer: {
            let sent = store.sentLine(now: model.now)
            Text((["Through " + MarketingAppStoreText.day(store.lastDay)] + (sent.isEmpty ? [] : [sent])).joined(separator: " · "))
        }
    }

    private func tiles(_ store: MarketingAppStore) -> [(value: String, label: String, detail: String, tint: Color)] {
        let window = model.appStoreWindow
        let change = window?.downloadsChange
        var result: [(value: String, label: String, detail: String, tint: Color)] = [
            (PackagesFormat.count(Int64(window?.downloads ?? 0)), "Downloads", window?.downloadsChangeText ?? "",
             change.map { $0 < 0 ? Color.orange : Color.green } ?? Color.secondary),
            (PackagesFormat.count(Int64(window?.redownloads ?? 0)), "Redownloads", "", .secondary),
            (PackagesFormat.count(Int64(window?.updates ?? 0)), "Updates", "", .secondary),
        ]
        result.append((store.ratingText.isEmpty ? "–" : store.ratingText, "Rating", "", .secondary))
        result.append((PackagesFormat.count(Int64(store.lookup.ratings)), "Ratings", "", .secondary))
        result.append((PackagesFormat.count(Int64(store.total.downloads)), "All-time downloads", "", .secondary))
        if let best = store.bestDay {
            result.append((PackagesFormat.count(Int64(best.downloads)), "Best day", MarketingAppStoreText.day(best.day), .secondary))
        }
        return result
    }

    @ViewBuilder private func shares(_ title: String, _ rows: [MarketingAppStoreShare], total: Int, countries: Bool) -> some View {
        if !rows.isEmpty {
            Section {
                ForEach(rows) { row in
                    LabeledContent(countries ? row.countryName : row.name, value: PackagesFormat.count(Int64(row.downloads)))
                        .monospacedDigit()
                }
            } header: {
                Text(title)
            } footer: {
                if total > rows.count { Text("Top \(rows.count) of \(total) countries") }
            }
        }
    }

    @ViewBuilder private func reviews(_ store: MarketingAppStore) -> some View {
        if !store.reviews.isEmpty {
            Section("Reviews") {
                ForEach(Array(store.reviews.prefix(10).enumerated()), id: \.offset) { _, review in
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Text(review.stars).font(.caption).foregroundStyle(.orange)
                            if !review.title.isEmpty { Text(review.title).font(.subheadline.weight(.medium)).lineLimit(2) }
                        }
                        if !review.body.isEmpty {
                            Text(review.body).font(.callout).lineLimit(6).fmMessageTools(review.body)
                        }
                        let meta = [review.territory, PackagesFormat.date(review.createdAt)].filter { !$0.isEmpty }
                        if !meta.isEmpty { Text(meta.joined(separator: " · ")).font(.caption2).foregroundStyle(.secondary) }
                    }
                    .accessibilityElement(children: .combine)
                }
            }
        }
    }

    @ViewBuilder private func versions(_ store: MarketingAppStore) -> some View {
        if !store.versions.isEmpty {
            Section("Versions") {
                ForEach(Array(store.versions.enumerated()), id: \.offset) { _, version in
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(version.title).font(.subheadline.monospacedDigit())
                            if version.createdAt > 0 {
                                Text(PackagesFormat.date(version.createdAt)).font(.caption2).foregroundStyle(.secondary)
                            }
                        }
                        Spacer()
                        Text(version.status.text).font(.caption.weight(.medium)).foregroundStyle(MarketingAppStoreText.color(version.status.tone))
                    }
                    .accessibilityElement(children: .combine)
                }
            }
        }
    }

    @ViewBuilder private func listing(_ store: MarketingAppStore) -> some View {
        let lookup = store.lookup
        if !lookup.isEmpty {
            Section {
                if !lookup.version.isEmpty { LabeledContent("Version", value: lookup.version) }
                if lookup.releasedAt > 0 { LabeledContent("Released", value: PackagesFormat.date(lookup.releasedAt)) }
                if !lookup.genre.isEmpty { LabeledContent("Genre", value: lookup.genre) }
                if !lookup.price.isEmpty { LabeledContent("Price", value: lookup.price) }
                if let url = lookup.link {
                    Link(destination: url) { Label("Open in the App Store", systemImage: "app.badge") }
                }
            } header: {
                Text(store.appName.isEmpty ? "On the App Store" : store.appName + " on the App Store")
            } footer: {
                if lookup.lookedUpAt > 0 {
                    Text("Read from the App Store " + PackagesFormat.relative(lookup.lookedUpAt))
                }
            }
        }
    }
}

struct MarketingAppStoreChart: View {
    let days: [MarketingAppStoreDay]

    private struct Point: Identifiable {
        let date: Date
        let kind: String
        let value: Int

        var id: String { kind + "\(date.timeIntervalSince1970)" }
    }

    private var points: [Point] {
        days.flatMap { day -> [Point] in
            guard let date = day.date else { return [] }
            return [Point(date: date, kind: "Downloads", value: day.downloads),
                    Point(date: date, kind: "Redownloads", value: day.redownloads),
                    Point(date: date, kind: "Updates", value: day.updates)]
        }
    }

    var body: some View {
        if days.isEmpty {
            Text("No days in this range.").font(.caption).foregroundStyle(.secondary)
        } else {
            Chart(points) { point in
                LineMark(x: .value("Day", point.date, unit: .day), y: .value("Count", point.value))
                    .foregroundStyle(by: .value("Kind", point.kind))
            }
            .chartForegroundStyleScale(["Downloads": Color.accentColor, "Redownloads": Color.teal, "Updates": Color.gray])
            .chartLegend(position: .bottom, alignment: .leading)
            .frame(height: 180)
            .accessibilityLabel("Daily downloads, redownloads and updates over the selected range")
        }
    }
}

struct MarketingAppStoreTile: View {
    let value: String
    let label: String
    var detail = ""
    var tint: Color = .secondary

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.headline.monospacedDigit()).lineLimit(1).minimumScaleFactor(0.7)
            Text(label).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
            if !detail.isEmpty {
                Text(detail).font(.caption2.monospacedDigit()).foregroundStyle(tint).lineLimit(1)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

enum MarketingAppStoreText {
    static func day(_ day: String) -> String {
        guard let date = MarketingAppStore.date(day) else { return day }
        var style = Date.FormatStyle.dateTime.month(.abbreviated).day().year()
        style.timeZone = TimeZone(identifier: "UTC") ?? .current
        return date.formatted(style)
    }

    static func color(_ tone: AppStoreTone) -> Color {
        switch tone {
        case .action: return .orange
        case .pending: return .blue
        case .good: return .green
        case .neutral: return .secondary
        }
    }
}

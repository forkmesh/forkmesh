import FMApi
import FMBase
import FMPackages
import SwiftUI
import UniformTypeIdentifiers

struct MarketDestination: View {
    let filter: String?

    var body: some View {
        if let client = PackagesFeature.client {
            MarketScreen(client: client, filter: MarketFilter(route: filter))
        } else {
            ContentUnavailableView("Market unavailable", systemImage: "shippingbox")
        }
    }
}

struct MarketScreen: View {
    private let client: PackagesClient
    @State private var model: MarketModel
    @State private var accountAction: AccountPackageAction?

    init(client: PackagesClient, filter: MarketFilter) {
        self.client = client
        _model = State(
            initialValue: MarketModel(
                client: client, filter: filter, account: PackagesFeature.accountPackages, ledger: .shared))
    }

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                if !model.errorText.isEmpty || !model.partialError.isEmpty {
                    VStack(alignment: .leading, spacing: FmSpace.x1) {
                        PackagesErrorBanner(text: model.errorText) { await refresh() }
                        PackagesErrorBanner(text: model.partialError)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
                    Divider()
                }
                let rows: [MarketPackage] = model.visible
                if rows.isEmpty && !model.loading && model.errorText.isEmpty {
                    emptyState
                }
                ForEach(rows) { (package: MarketPackage) in
                    MarketCard(
                        package: package, model: model, signedIn: PackagesFeature.signedIn,
                        onInstall: { (action: MarketCardAction) in Task { await run(action, package) } })
                    Divider()
                }
            }
            .padding(.bottom, FmSpace.x4)
        }
        .scrollDismissesKeyboard(.immediately)
        .safeAreaInset(edge: .top, spacing: 0) {
            MarketControls(model: model)
        }
        .navigationTitle("Market")
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $model.query, placement: .navigationBarDrawer(displayMode: .always), prompt: "Search packages")
        .overlay {
            if model.loading && model.packages.isEmpty { ProgressView() }
        }
        .fmRefreshable { await refresh() }
        .task { await refresh() }
        .confirmationDialog(
            accountAction?.title ?? "",
            isPresented: Binding(get: { accountAction != nil }, set: { if !$0 { accountAction = nil } }),
            titleVisibility: .visible, presenting: accountAction
        ) { (action: AccountPackageAction) in
            Button("Install") {
                guard let package = model.package(action.packageId) else { return }
                Task { await installForAccount(package) }
            }
            Button("Cancel", role: .cancel) {}
        } message: { (action: AccountPackageAction) in
            Text(action.message)
        }
    }

    @ViewBuilder private var emptyState: some View {
        if model.query.isEmpty {
            ContentUnavailableView(
                "No packages match", systemImage: "line.3.horizontal.decrease.circle",
                description: Text("Change the filters to see more of the Market."))
                .padding(.top, FmSpace.x6)
        } else {
            ContentUnavailableView.search(text: model.query)
                .padding(.top, FmSpace.x6)
        }
    }

    private func refresh() async {
        await model.refresh(organizations: await PackagesFeature.organizationNames())
    }

    private func run(_ action: MarketCardAction, _ package: MarketPackage) async {
        switch action {
        case .installAccount:
            let others: [String] = model.account?.missingDependencies(of: package.id).map(\.name) ?? []
            if others.isEmpty {
                await installForAccount(package)
            } else {
                accountAction = AccountPackageAction(
                    packageId: package.id, name: package.name, install: true, others: others,
                    project: model.account?.ownsInstalls ?? false)
            }
        case .installPhone:
            if package.accountSync {
                await installForAccount(package)
            } else {
                PhonePackages.setRemoved(package.id, removed: false)
                PackageGateSync.publish()
                FmHaptics.commit()
            }
        case .installDevice:
            PackageGateSync.setOnDevice(package.id, on: true)
            FmHaptics.commit()
        case .installOrganization(let organization):
            if await model.install(package, organization: organization) {
                FmHaptics.commit()
                await HubPackageState.shared.refresh()
            }
        default:
            return
        }
    }

    private func installForAccount(_ package: MarketPackage) async {
        if await model.installForAccount(package) {
            PackageGateSync.publish()
            FmHaptics.commit()
        }
    }
}

struct MarketControls: View {
    @Bindable var model: MarketModel

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                sortMenu
                statusMenu
                categoryMenu
                runsOnMenu
                scopeMenu
                moreMenu
                if model.filters.activeCount > 0 || model.filter != .all {
                    Button {
                        model.filters = MarketFilters()
                        model.filter = .all
                        FmHaptics.selection()
                    } label: {
                        Label("Clear", systemImage: "xmark")
                            .marketChip(active: false)
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("market-filter-clear")
                }
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
        .background(.bar)
        .overlay(alignment: .bottom) { Divider() }
    }

    private var sortMenu: some View {
        Menu {
            Picker("Sort by", selection: $model.sort) {
                ForEach(MarketSort.allCases, id: \.self) { (sort: MarketSort) in
                    Label(sort.title, systemImage: sort.symbol).tag(sort)
                }
            }
        } label: {
            Label(model.sort.title, systemImage: "arrow.up.arrow.down")
                .marketChip(active: model.sort != .featured)
        }
        .accessibilityLabel("Sort by \(model.sort.title)")
        .accessibilityIdentifier("market-sort")
    }

    private var statusMenu: some View {
        Menu {
            Picker("Show", selection: $model.filter) {
                ForEach(MarketFilter.allCases, id: \.self) { Text($0.title).tag($0) }
            }
        } label: {
            Label(model.filter == .all ? "Installed or not" : model.filter.title, systemImage: "checkmark.circle")
                .marketChip(active: model.filter != .all)
        }
        .accessibilityIdentifier("market-filter-status")
    }

    private var categoryMenu: some View {
        Menu {
            Picker("Category", selection: $model.filters.category) {
                Text("All categories").tag(String?.none)
                ForEach(model.categories, id: \.self) { (category: String) in
                    Text(category).tag(String?.some(category))
                }
            }
        } label: {
            Label(model.filters.category ?? "Category", systemImage: "square.grid.2x2")
                .marketChip(active: model.filters.category != nil)
        }
        .accessibilityIdentifier("market-filter-category")
    }

    private var runsOnMenu: some View {
        Menu {
            Picker("Runs on", selection: $model.filters.runsOn) {
                ForEach(MarketRunsOn.allCases, id: \.self) { Text($0.title).tag($0) }
            }
        } label: {
            Label(model.filters.runsOn == .any ? "Runs on" : model.filters.runsOn.title, systemImage: "iphone")
                .marketChip(active: model.filters.runsOn != .any)
        }
        .accessibilityIdentifier("market-filter-runs-on")
    }

    private var scopeMenu: some View {
        Menu {
            Picker("Installs for", selection: $model.filters.scope) {
                Text("Anyone").tag(MarketInstallScope?.none)
                ForEach(MarketInstallScope.allCases, id: \.self) { (scope: MarketInstallScope) in
                    Text(scope.title).tag(MarketInstallScope?.some(scope))
                }
            }
        } label: {
            Label(model.filters.scope?.title ?? "Installs for", systemImage: "person.crop.circle")
                .marketChip(active: model.filters.scope != nil)
        }
        .accessibilityIdentifier("market-filter-scope")
    }

    private var moreActive: Int {
        [model.filters.kind != nil, model.filters.price != .any, model.filters.publisher != nil,
         model.filters.verifiedOnly, model.filters.ratedOnly].filter { $0 }.count
    }

    private var moreMenu: some View {
        Menu {
            Picker("Kind", selection: $model.filters.kind) {
                Text("Every kind").tag(String?.none)
                ForEach(model.kinds, id: \.self) { (kind: String) in
                    Text(MarketCard.kindTitle(kind)).tag(String?.some(kind))
                }
            }
            .pickerStyle(.menu)
            Picker("Price", selection: $model.filters.price) {
                ForEach(MarketPrice.allCases, id: \.self) { Text($0.title).tag($0) }
            }
            .pickerStyle(.menu)
            Picker("Publisher", selection: $model.filters.publisher) {
                Text("Every publisher").tag(String?.none)
                ForEach(model.publishers, id: \.self) { (publisher: String) in
                    Text(publisher).tag(String?.some(publisher))
                }
            }
            .pickerStyle(.menu)
            Toggle("Verified publishers only", isOn: $model.filters.verifiedOnly)
            Toggle("Rated only", isOn: $model.filters.ratedOnly)
        } label: {
            Label(moreActive > 0 ? "More (\(moreActive))" : "More", systemImage: "line.3.horizontal.decrease")
                .marketChip(active: moreActive > 0)
        }
        .accessibilityIdentifier("market-filter-more")
    }
}

private struct MarketChip: ViewModifier {
    let active: Bool

    func body(content: Content) -> some View {
        content
            .font(.footnote.weight(active ? .semibold : .regular))
            .lineLimit(1)
            .padding(.horizontal, FmSpace.x3)
            .frame(minHeight: 32)
            .foregroundStyle(active ? Color.white : Color.primary)
            .background(
                Capsule().fill(active ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.quaternary)))
            .contentShape(Capsule())
    }
}

extension View {
    fileprivate func marketChip(active: Bool) -> some View {
        modifier(MarketChip(active: active))
    }
}

struct MarketCard: View {
    let package: MarketPackage
    let model: MarketModel
    let signedIn: Bool
    let onInstall: (MarketCardAction) -> Void

    static let thumbnailWidth: CGFloat = 112
    static let actionWidth: CGFloat = 76

    static func kindTitle(_ kind: String) -> String {
        switch kind {
        case "extension.native": return "Extension"
        case "extension.vscode": return "Editor extension"
        case "agent": return "Agent"
        case "theme": return "Theme"
        case "virtual.object": return "World object"
        case "virtual.world": return "World"
        default: return kind
        }
    }

    var body: some View {
        let action: MarketCardAction = model.cardAction(package, signedIn: signedIn)
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            NavigationLink(value: Route.package(id: package.id)) {
                HStack(alignment: .top, spacing: FmSpace.x3) {
                    thumbnail
                    details
                    Color.clear.frame(width: MarketCard.actionWidth, height: 1)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("market-card-" + package.id)
            .overlay(alignment: .topTrailing) {
                MarketActionButton(package: package, action: action, onInstall: onInstall)
                    .frame(width: MarketCard.actionWidth, alignment: .trailing)
            }
            if let error = model.installErrors[package.id] {
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .font(.caption)
                        .foregroundStyle(.red)
                        .fmMessageTools(error)
                    Spacer(minLength: 0)
                    Button {
                        model.dismissInstallError(package)
                    } label: {
                        Image(systemName: "xmark").font(.caption)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Dismiss")
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
    }

    @ViewBuilder private var thumbnail: some View {
        if let screenshot = package.screenshots.first {
            MarketScreenshotImage(screenshot: screenshot, cornerRadius: FmRadius.md)
                .frame(width: MarketCard.thumbnailWidth)
                .accessibilityHidden(true)
        } else {
            RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous)
                .fill(.quaternary)
                .aspectRatio(16 / 9, contentMode: .fit)
                .frame(width: MarketCard.thumbnailWidth)
                .overlay {
                    Image(systemName: "shippingbox").font(.title3).foregroundStyle(.secondary)
                }
                .accessibilityHidden(true)
        }
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 4) {
                Text(package.name)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                if package.verified {
                    Image(systemName: "checkmark.seal.fill")
                        .font(.caption2)
                        .foregroundStyle(Color.accentColor)
                        .accessibilityLabel("Verified publisher")
                }
            }
            if !package.summary.isEmpty {
                Text(package.summary)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .fixedSize(horizontal: false, vertical: true)
            }
            MarketStatsLine(
                package: package, rating: model.ratings[package.id], counts: model.counts[package.id],
                usage: model.usage[package.id], nestedCount: model.nested(in: package).count)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }
}

struct MarketStatsLine: View {
    let package: MarketPackage
    let rating: RatingSummary?
    let counts: PackageCounts?
    let usage: PackageUsage?
    var nestedCount = 0

    var body: some View {
        ViewThatFits(in: .horizontal) {
            line(full: true)
            line(full: false)
        }
        .font(.caption2.monospacedDigit())
        .foregroundStyle(.secondary)
    }

    private func line(full: Bool) -> some View {
        HStack(spacing: FmSpace.x2) {
            if let rating, rating.count > 0 {
                Label(String(format: "%.1f", rating.average), systemImage: "star.fill")
                    .foregroundStyle(.orange)
                    .accessibilityLabel(String(format: "Rated %.1f from %d ratings", rating.average, rating.count))
            }
            if let counts, counts.total > 0 {
                Label(PackagesFeature.count(Int64(counts.total)), systemImage: "arrow.down.app")
                    .accessibilityLabel("\(counts.total) installs")
            } else if let counts, counts.downloads > 0 {
                Label(PackagesFeature.count(Int64(counts.downloads)), systemImage: "arrow.down.circle")
                    .accessibilityLabel("\(counts.downloads) downloads")
            }
            if let usage, usage.people > 0 {
                Label(PackagesFeature.count(Int64(usage.people)), systemImage: "person.2")
                    .accessibilityLabel("\(usage.people) people use it")
            }
            if full {
                if !package.free {
                    Text("Paid")
                }
                if nestedCount > 0 {
                    Label("\(nestedCount)", systemImage: "square.stack.3d.up")
                        .accessibilityLabel("Includes \(nestedCount) packages")
                }
                PhoneSurfaceTag(surface: package.phoneSurface)
                    .labelStyle(.iconOnly)
            }
        }
        .labelStyle(MarketTightLabel())
        .lineLimit(1)
    }
}

private struct MarketTightLabel: LabelStyle {
    func makeBody(configuration: Configuration) -> some View {
        HStack(spacing: 2) {
            configuration.icon
            configuration.title
        }
    }
}

struct MarketActionButton: View {
    let package: MarketPackage
    let action: MarketCardAction
    let onInstall: (MarketCardAction) -> Void

    var body: some View {
        content
            .font(.footnote.weight(.semibold))
            .controlSize(.small)
            .accessibilityIdentifier("market-action-" + package.id)
    }

    @ViewBuilder private var content: some View {
        switch action {
        case .open:
            if let route = PackageRoutes.route(for: package) {
                NavigationLink(value: route) { Text("Open") }
                    .buttonStyle(.bordered)
                    .buttonBorderShape(.capsule)
                    .accessibilityLabel("Open \(package.name)")
            } else if let url = AppServices.current?.api.url(package.webRoute) {
                Link(destination: url) { Text("Open") }
                    .buttonStyle(.bordered)
                    .buttonBorderShape(.capsule)
                    .accessibilityLabel("Open \(package.name) on the web")
            } else {
                installedLabel
            }
        case .installed:
            installedLabel
        case .installAccount, .installDevice, .installPhone, .installOrganization:
            Button { onInstall(action) } label: { Text("Install") }
                .buttonStyle(.borderedProminent)
                .buttonBorderShape(.capsule)
                .accessibilityLabel("Install \(package.name)")
        case .chooseOrganization(let organizations):
            Menu {
                Section("Install for") {
                    ForEach(organizations, id: \.self) { (organization: String) in
                        Button(organization) { onInstall(.installOrganization(organization)) }
                    }
                }
            } label: {
                Text("Install")
            }
            .buttonStyle(.borderedProminent)
            .buttonBorderShape(.capsule)
            .accessibilityLabel("Install \(package.name) for an organization")
        case .createOrganization, .askAdmin, .signIn:
            NavigationLink(value: Route.package(id: package.id)) { Text("Get") }
                .buttonStyle(.bordered)
                .buttonBorderShape(.capsule)
                .accessibilityLabel(action == .askAdmin ? "Ask an admin to install \(package.name)" : "Get \(package.name)")
        case .desktop:
            let world: Bool = package.phoneSurface == .worldOnly
            Label(world ? "World" : "Desktop", systemImage: world ? "globe.europe.africa" : "desktopcomputer")
                .labelStyle(MarketTightLabel())
                .font(.caption2)
                .foregroundStyle(.secondary)
                .padding(.top, 6)
                .accessibilityLabel(
                    world ? "\(package.name) runs in World" : "Install \(package.name) from your desktop's Market")
        case .busy:
            ProgressView()
                .frame(height: 28)
                .accessibilityLabel("Installing \(package.name)")
        }
    }

    private var installedLabel: some View {
        Label("Installed", systemImage: "checkmark.circle.fill")
            .labelStyle(MarketTightLabel())
            .font(.caption2.weight(.semibold))
            .foregroundStyle(.green)
            .padding(.top, 6)
    }
}

struct MarketScreenshotImage: View {
    let screenshot: PackageScreenshot
    var cornerRadius: CGFloat = 10

    var body: some View {
        Color.clear
            .aspectRatio(16 / 9, contentMode: .fit)
            .overlay {
                AsyncImage(url: screenshot.url) { phase in
                    switch phase {
                    case .success(let image):
                        image.resizable().scaledToFill()
                    case .failure:
                        Image(systemName: "photo").foregroundStyle(.secondary)
                    default:
                        ProgressView()
                    }
                }
            }
            .background(.quaternary)
            .clipShape(RoundedRectangle(cornerRadius: cornerRadius))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(screenshot.alt.isEmpty ? "Screenshot" : screenshot.alt)
            .accessibilityAddTraits(.isImage)
    }
}

struct MarketScreenshotStrip: View {
    let screenshots: [PackageScreenshot]

    var body: some View {
        if screenshots.count == 1, let screenshot = screenshots.first {
            MarketScreenshotImage(screenshot: screenshot)
        } else {
            ScrollView(.horizontal, showsIndicators: false) {
                LazyHStack(spacing: 12) {
                    ForEach(screenshots) { screenshot in
                        MarketScreenshotImage(screenshot: screenshot)
                            .containerRelativeFrame(.horizontal) { width, _ in width * 0.85 }
                    }
                }
                .scrollTargetLayout()
            }
            .scrollTargetBehavior(.viewAligned)
        }
    }
}

struct PhoneSurfaceTag: View {
    let surface: PackagePhoneSurface

    var body: some View {
        switch surface {
        case .native: Label("On this phone", systemImage: "iphone")
        case .web: Label("On the web", systemImage: "globe")
        case .desktopOnly: Label("On your desktop", systemImage: "desktopcomputer")
        case .worldOnly: Label("In World", systemImage: "globe.europe.africa")
        case .everyDevice: Label("On all your devices", systemImage: "rectangle.on.rectangle")
        }
    }
}

struct PackageDestination: View {
    let id: String

    var body: some View {
        if let client = PackagesFeature.client {
            PackageDetailScreen(client: client, packageId: id)
        } else {
            ContentUnavailableView("Market unavailable", systemImage: "shippingbox")
        }
    }
}

struct PackageDetailScreen: View {
    private let client: PackagesClient
    private let packageId: String
    @State private var market: MarketModel
    @State private var reviews: PackageReviewsModel
    @State private var installTarget: OrgPackageState?
    @State private var removeTarget: OrgPackageState?
    @State private var exportURL: URL?
    @State private var exporting = false
    @State private var rating = false
    @State private var removingFromPhone = false
    @State private var quickRating = false
    @State private var accountAction: AccountPackageAction?
    @State private var monitorName = ""
    @State private var monitorURL = ""

    init(client: PackagesClient, packageId: String) {
        self.client = client
        self.packageId = packageId
        _market = State(initialValue: MarketModel(client: client, account: PackagesFeature.accountPackages, ledger: .shared))
        _reviews = State(initialValue: PackageReviewsModel(client: client, packageId: packageId))
    }

    var body: some View {
        List {
            if let package = market.package(packageId) {
                header(package)
                quickRateSection
                openSection(package)
                phoneInstallSection(package)
                installSection(package)
                if !package.permissions.isEmpty {
                    Section("Permissions") {
                        ForEach(package.permissions) { permission in
                            Label {
                                Text(permission.label)
                            } icon: {
                                Image(systemName: permission.risk == "high" ? "exclamationmark.shield" : "lock.shield")
                                    .foregroundStyle(permission.risk == "high" ? .red : .orange)
                            }
                            .font(.subheadline)
                        }
                    }
                }
                activitySection
                reviewsSection
            } else if market.loading {
                ProgressView()
            } else {
                PackagesErrorBanner(text: market.errorText.isEmpty ? "This package is not in the Market." : market.errorText) {
                    await refresh()
                }
            }
        }
        .navigationTitle(market.package(packageId)?.name ?? "Package")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await refresh() }
        .task {
            if PackagesFeature.signedIn { PackageActivity.shared.report(packageId, kind: .view, client: client) }
            await refresh()
        }
        .confirmationDialog(
            "Remove \(market.package(packageId)?.name ?? "") from this phone?",
            isPresented: $removingFromPhone, titleVisibility: .visible
        ) {
            Button("Uninstall", role: .destructive) {
                PhonePackages.setRemoved(packageId, removed: true)
                PackageGateSync.publish()
                FmHaptics.commit()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("It disappears from your home menu and shortcuts on this phone until you install it again here. Your data is kept.")
        }
        .confirmationDialog(
            "Install \(market.package(packageId)?.name ?? "") for \(installTarget?.organization ?? "")?",
            isPresented: Binding(get: { installTarget != nil }, set: { if !$0 { installTarget = nil } }),
            titleVisibility: .visible
        ) {
            Button("Install") {
                guard let package = market.package(packageId), let target = installTarget else { return }
                Task {
                    await market.install(package, organization: target.organization)
                    FmHaptics.commit()
                    await HubPackageState.shared.refresh()
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Everyone in the organization gets it. Removing it later exports and deletes its data.")
        }
        .confirmationDialog(
            "Remove \(market.package(packageId)?.name ?? "") from \(removeTarget?.organization ?? "")?",
            isPresented: Binding(get: { removeTarget != nil }, set: { if !$0 { removeTarget = nil } }),
            titleVisibility: .visible
        ) {
            Button("Remove", role: .destructive) {
                guard let package = market.package(packageId), let target = removeTarget else { return }
                Task {
                    guard await market.uninstall(package, organization: target.organization) else { return }
                    FmHaptics.commit()
                    await HubPackageState.shared.refresh()
                    exportURL = market.removed.flatMap(exportFile)
                    exporting = exportURL != nil
                }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Everyone in the organization loses it. Its data is deleted from the server; you get a copy to save right after.")
        }
        .fileExporter(
            isPresented: $exporting, document: market.removed?.export.map(PackageExportDocument.init(data:)), contentType: .json,
            defaultFilename: exportURL?.lastPathComponent
        ) { _ in }
        .sheet(isPresented: $rating) {
            NavigationStack { RatePackageSheet(model: reviews) }
        }
        .confirmationDialog(
            accountAction?.title ?? "",
            isPresented: Binding(get: { accountAction != nil }, set: { if !$0 { accountAction = nil } }),
            titleVisibility: .visible, presenting: accountAction
        ) { (action: AccountPackageAction) in
            Button(action.install ? "Install" : "Remove", role: action.install ? nil : .destructive) {
                Task { await runAccountAction(action) }
            }
            Button("Cancel", role: .cancel) {}
        } message: { (action: AccountPackageAction) in
            Text(action.message)
        }
    }

    private func runAccountAction(_ action: AccountPackageAction) async {
        guard let account = market.account else { return }
        let changed: [String]? = action.install ? await account.install(action.packageId) : await account.uninstall(action.packageId)
        if changed != nil { FmHaptics.commit() }
    }

    private func requestAccountChange(_ package: MarketPackage, install: Bool, account: AccountPackagesStore) {
        let others: [String] = (install ? account.missingDependencies(of: package.id) : account.dependents(of: package.id)).map(\.name)
        let action = AccountPackageAction(
            packageId: package.id, name: package.name, install: install, others: others, project: account.ownsInstalls)
        if others.isEmpty && install {
            Task { await runAccountAction(action) }
        } else {
            accountAction = action
        }
    }

    private func refresh() async {
        await market.refresh(organizations: await PackagesFeature.organizationNames())
        await reviews.refresh()
        await reviews.loadPeople()
    }

    @ViewBuilder private var quickRateSection: some View {
        if let page = reviews.page, page.viewer.canRate {
            Section {
                HStack(spacing: 4) {
                    ForEach(1...5, id: \.self) { (star: Int) in
                        Button {
                            Task { await quickRate(star) }
                        } label: {
                            Image(systemName: star <= (page.viewer.review?.stars ?? 0) ? "star.fill" : "star")
                                .font(.title2)
                                .foregroundStyle(.orange)
                                .frame(minWidth: 44, minHeight: 44)
                        }
                        .buttonStyle(.plain)
                        .disabled(quickRating)
                        .accessibilityLabel("Rate \(star) \(star == 1 ? "star" : "stars")")
                        .accessibilityIdentifier("market-package-quick-star-\(star)")
                    }
                    Spacer()
                    if quickRating { ProgressView() }
                }
            } header: {
                Text(page.viewer.review == nil ? "Tap to rate" : "Your rating")
            } footer: {
                if !reviews.errorText.isEmpty {
                    Text(reviews.errorText).foregroundStyle(.red)
                        .fmMessageTools(reviews.errorText)
                } else if page.rules.reviewRequiredBelow > 1 {
                    Text("Below \(page.rules.reviewRequiredBelow) stars, you also say what didn't work.")
                }
            }
        }
    }

    private func quickRate(_ stars: Int) async {
        quickRating = true
        defer { quickRating = false }
        switch await reviews.quickRate(stars) {
        case .submitted: FmHaptics.commit()
        case .needsReview: rating = true
        case .failed: break
        }
    }

    @ViewBuilder private func phoneInstallSection(_ package: MarketPackage) -> some View {
        if PhonePackages.canRemove(package) || PhonePackages.isRemoved(package.id) {
            Section {
                if PhonePackages.isRemoved(package.id) {
                    Button {
                        PhonePackages.setRemoved(package.id, removed: false)
                        PackageGateSync.publish()
                        FmHaptics.commit()
                    } label: {
                        Label("Install on this phone", systemImage: "arrow.down.circle")
                    }
                    .accessibilityIdentifier("market-package-phone-install")
                } else {
                    Button(role: .destructive) {
                        removingFromPhone = true
                    } label: {
                        Label("Uninstall from this phone", systemImage: "trash")
                    }
                    .accessibilityIdentifier("market-package-phone-uninstall")
                }
            } footer: {
                Text(PhonePackages.isRemoved(package.id)
                    ? "\(package.name) is not on your home menu. Install it to bring it back."
                    : "Removes \(package.name) from your home menu on this phone. Your data is kept.")
            }
        }
    }

    @ViewBuilder private var activitySection: some View {
        if let page = reviews.page {
            Section("Activity") {
                LabeledContent("Views", value: "\(page.activity.views)")
                LabeledContent("Opens", value: "\(page.activity.opens)")
                LabeledContent("People", value: "\(page.activity.people)")
                if page.viewer.signedIn {
                    LabeledContent(
                        "You",
                        value: "Opened \(page.viewer.opens) \(page.viewer.opens == 1 ? "time" : "times") · viewed \(page.viewer.views)")
                }
            }
            if page.viewer.canSeePeople, let people = reviews.people, !people.people.isEmpty {
                Section("Who opened it") {
                    ForEach(people.people) { (person: PackageActivityPerson) in
                        HStack {
                            Text(person.name.isEmpty ? "Unknown" : person.name)
                            Spacer()
                            Text("\(person.opens) opens · \(person.views) views")
                                .font(.caption.monospacedDigit())
                                .foregroundStyle(.secondary)
                        }
                        .accessibilityElement(children: .combine)
                    }
                }
            }
        }
    }

    @ViewBuilder private func header(_ package: MarketPackage) -> some View {
        Section {
            VStack(alignment: .leading, spacing: 8) {
                if !package.screenshots.isEmpty {
                    MarketScreenshotStrip(screenshots: package.screenshots)
                }
                Text(package.summary)
                HStack(spacing: 12) {
                    if !package.version.isEmpty { Text("v" + package.version) }
                    if !package.category.isEmpty { Text(package.category) }
                    if let counts = market.counts[package.id], counts.total > 0 {
                        Text("\(counts.total) installs")
                    }
                    if let counts = market.counts[package.id], counts.downloads > 0 {
                        Text("\(counts.downloads) downloads")
                    }
                    if let usage = market.usage[package.id], usage.people > 0 {
                        Text("\(usage.people) \(usage.people == 1 ? "person" : "people")")
                    }
                }
                .font(.caption)
                .foregroundStyle(.secondary)
                PhoneSurfaceTag(surface: package.phoneSurface).font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private func openSection(_ package: MarketPackage) -> some View {
        if (package.accountSync && !market.isInstalled(package)) || PhonePackages.isRemoved(package.id) {
            EmptyView()
        } else if let route = PackageRoutes.route(for: package) {
            Section {
                NavigationLink(value: route) { Label("Open \(package.name)", systemImage: "arrow.forward.circle") }
            }
        } else if case .web(let path) = package.phoneSurface, let url = webURL(path) {
            Section {
                Link(destination: url) { Label("Open on the web", systemImage: "safari") }
            }
        } else if package.phoneSurface == .desktopOnly && !package.accountSync {
            Section {
                Label("This package runs on your desktop. Install it from the desktop's Market.", systemImage: "desktopcomputer")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private func installSection(_ package: MarketPackage) -> some View {
        if package.accountSync, let account = market.account {
            accountInstallSection(package, account: account)
            if account.isInstalled(package.id) {
                placesSection(package, account: account)
            }
            if !market.nested(in: package).isEmpty {
                nestedSection(package, account: account)
            }
            if account.isInstalled(package.id) {
                if !package.settings.isEmpty {
                    accountSettingsSection(package, account: account)
                }
                if package.id == AccountPackagesStore.statusPage {
                    monitorsSection(account)
                }
            }
        } else if let on = PhonePackages.isOn(package.id) {
            Section {
                Toggle(isOn: Binding<Bool>(
                    get: { on },
                    set: { (value: Bool) in PackageGateSync.setOnDevice(package.id, on: value) })
                ) {
                    Label("Installed on this device", systemImage: "iphone")
                }
                .accessibilityIdentifier("market-package-phone-installed")
            } footer: {
                if package.id == PhonePackages.signIn {
                    Text("Turn it off to hide your avatar and Sign in from this phone's top bar and menu. Settings, Account always keeps Sign in and Sign out.")
                } else {
                    Text("Turn it off to remove \(package.name) from this device. It stays on your other devices.")
                }
            }
        } else if package.installsPerOrganization {
            Section("Organizations") {
                if market.organizations.isEmpty {
                    Text("You are not in an organization yet.").foregroundStyle(.secondary)
                    NavigationLink(value: Route.orgs) {
                        Label("Create an organization", systemImage: "person.3")
                    }
                    .accessibilityIdentifier("market-package-create-organization")
                }
                ForEach(market.organizations) { org in
                    HStack {
                        Text(org.organization)
                        Spacer()
                        if market.isBusy(package, organization: org.organization) {
                            ProgressView()
                        } else if org.isInstalled(package.id) {
                            Label("Installed", systemImage: "checkmark.circle.fill")
                                .labelStyle(.iconOnly)
                                .foregroundStyle(.green)
                                .accessibilityLabel("Installed")
                            if org.canManage {
                                Button("Remove", role: .destructive) { removeTarget = org }.buttonStyle(.bordered)
                            }
                        } else if org.canManage {
                            Button("Install") { installTarget = org }.buttonStyle(.bordered)
                        } else {
                            Text("Ask an admin").font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                PackagesErrorBanner(text: market.errorText)
            }
            if let removal = market.removed, removal.packageId == package.id {
                removalSection(removal)
            }
        }
    }

    @ViewBuilder private func accountInstallSection(_ package: MarketPackage, account: AccountPackagesStore) -> some View {
        Section {
            HStack {
                Toggle(isOn: Binding<Bool>(
                    get: { account.isInstalled(package.id) },
                    set: { (value: Bool) in requestAccountChange(package, install: value, account: account) })
                ) {
                    if account.ownsInstalls {
                        Label("Installed in this project", systemImage: "square.stack")
                    } else {
                        Label("Installed on all your devices", systemImage: "rectangle.on.rectangle")
                    }
                }
                .disabled(!PackagesFeature.signedIn || account.isBusy(package.id))
                .accessibilityIdentifier("market-package-account-installed")
                if account.isBusy(package.id) {
                    ProgressView()
                }
            }
            PackagesErrorBanner(text: account.errorText)
        } footer: {
            if PackagesFeature.signedIn && account.ownsInstalls {
                Text("Your other projects install it on their own. Its settings and where it shows stay with your account.")
            } else if PackagesFeature.signedIn {
                Text("Installing or removing \(package.name) applies to your phone, your desktop and the web.")
            } else {
                Text("Sign in to install or remove \(package.name) for your account.")
            }
        }
    }

    @ViewBuilder private func placesSection(_ package: MarketPackage, account: AccountPackagesStore) -> some View {
        let available: [PackagePlace] = account.availablePlaces(package.id, fallback: package.places)
        let chosen: [PackagePlace] = account.places(package.id).filter { available.contains($0) }
        Section {
            ForEach(available, id: \.self) { (place: PackagePlace) in
                let on: Bool = chosen.contains(place)
                Toggle(isOn: Binding<Bool>(
                    get: { on },
                    set: { (value: Bool) in
                        Task {
                            if await account.setPlace(package.id, place, on: value) { FmHaptics.commit() }
                        }
                    })
                ) {
                    Label(place.title, systemImage: place.symbol)
                }
                .disabled(!PackagesFeature.signedIn || account.isBusy(package.id) || (on && chosen.count <= 1))
                .accessibilityIdentifier("market-package-place-" + place.rawValue)
            }
        } header: {
            Text("Show on")
        } footer: {
            if !available.contains(PackagePlace.here) {
                Text("\(package.name) does not show on phones.")
            } else if !account.isPlaced(package.id) {
                Text("Not shown on this phone. Turn on Phone to show it here.")
            } else {
                Text("Choose where \(package.name) shows. To take it off everywhere, remove it.")
            }
        }
    }

    @ViewBuilder private func nestedSection(_ package: MarketPackage, account: AccountPackagesStore) -> some View {
        let parentInstalled = account.isInstalled(package.id)
        Section {
            ForEach(market.nested(in: package)) { child in
                Toggle(isOn: Binding<Bool>(
                    get: { parentInstalled && account.isInstalled(child.id) },
                    set: { (value: Bool) in requestAccountChange(child, install: value, account: account) })
                ) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(child.name)
                        if !child.summary.isEmpty {
                            Text(child.summary).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                        }
                    }
                }
                .disabled(!PackagesFeature.signedIn || !parentInstalled || account.isBusy(child.id))
                .accessibilityIdentifier("market-package-nested-" + child.id)
            }
        } header: {
            Text("Included packages")
        } footer: {
            Text("They are added and removed with \(package.name). Turn one off to hide it.")
        }
    }

    @ViewBuilder private func accountSettingsSection(_ package: MarketPackage, account: AccountPackagesStore) -> some View {
        Section("Settings") {
            ForEach(package.settings) { (setting: PackageSetting) in
                accountSettingRow(package, setting: setting, account: account)
            }
        }
        .disabled(!PackagesFeature.signedIn)
    }

    @ViewBuilder private func accountSettingRow(
        _ package: MarketPackage, setting: PackageSetting, account: AccountPackagesStore
    ) -> some View {
        switch setting.type {
        case .boolean:
            Toggle(setting.label, isOn: Binding<Bool>(
                get: { account.settings(package.id).optBool(setting.key) ?? (setting.defaultValue.boolValue ?? false) },
                set: { (value: Bool) in
                    Task { await account.setSetting(package.id, key: setting.key, value: .bool(value)) }
                }))
            .accessibilityIdentifier("market-package-setting-\(setting.key)")
        case .choice:
            Picker(setting.label, selection: Binding<String>(
                get: { account.settings(package.id).str(setting.key) },
                set: { (value: String) in
                    Task { await account.setSetting(package.id, key: setting.key, value: .string(value)) }
                })
            ) {
                ForEach(setting.options) { (option: PackageSettingOption) in
                    Text(option.label).tag(option.value)
                }
            }
            .accessibilityIdentifier("market-package-setting-\(setting.key)")
        }
    }

    @ViewBuilder private func monitorsSection(_ account: AccountPackagesStore) -> some View {
        Section {
            if account.monitors.isEmpty {
                Text("No monitors yet.").foregroundStyle(.secondary)
            }
            ForEach(account.monitors) { (monitor: StatusMonitor) in
                VStack(alignment: .leading, spacing: 2) {
                    Text(monitor.name)
                    Text(verbatim: monitor.url)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                .accessibilityElement(children: .combine)
                .swipeActions(edge: .trailing) {
                    Button("Remove", role: .destructive) {
                        Task { await account.removeMonitor(monitor.id) }
                    }
                }
                .contextMenu {
                    Button("Remove", systemImage: "trash", role: .destructive) {
                        Task { await account.removeMonitor(monitor.id) }
                    }
                }
            }
            if account.monitors.count < account.monitorLimit {
                TextField("Name", text: $monitorName)
                    .textContentType(.name)
                    .accessibilityIdentifier("status-monitor-name")
                TextField("https://example.com/", text: $monitorURL)
                    .keyboardType(.URL)
                    .textContentType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .accessibilityLabel("Address")
                    .accessibilityIdentifier("status-monitor-url")
                Button {
                    Task {
                        if await account.addMonitor(name: monitorName, url: monitorURL) {
                            FmHaptics.commit()
                            monitorName = ""
                            monitorURL = ""
                        }
                    }
                } label: {
                    Label("Add monitor", systemImage: "plus.circle")
                }
                .disabled(
                    account.monitorsBusy || monitorName.trimmingCharacters(in: .whitespaces).isEmpty
                        || monitorURL.trimmingCharacters(in: .whitespaces).isEmpty)
                .accessibilityIdentifier("status-monitor-add")
            } else {
                Text("You have \(account.monitorLimit) monitors, the most allowed.").font(.footnote).foregroundStyle(.secondary)
            }
            PackagesErrorBanner(text: account.monitorError)
        } header: {
            Text("Your monitors")
        } footer: {
            Text("Each of your devices loads these addresses itself and shows a green dot when they answer.")
        }
        .disabled(!PackagesFeature.signedIn)
    }

    @ViewBuilder private func removalSection(_ removal: PackageRemoval) -> some View {
        Section {
            Label("Removed from \(removal.organization).", systemImage: "trash")
            if let url = exportURL {
                Button { exporting = true } label: { Label("Save the exported data", systemImage: "square.and.arrow.down") }
                ShareLink(item: url) { Label("Share the exported data", systemImage: "square.and.arrow.up") }
                Text("This is the only copy of the data that was deleted. Save it before you leave this screen.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            Button("Done") {
                market.dismissRemoval()
                exportURL = nil
            }
        }
    }

    private func exportFile(_ removal: PackageRemoval) -> URL? {
        guard let data = removal.export else { return nil }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(removal.filename)
        do {
            try data.write(to: url, options: [.atomic, .completeFileProtection])
        } catch {
            return nil
        }
        return url
    }

    @ViewBuilder private var reviewsSection: some View {
        Section {
            if let page = reviews.page {
                RatingHistogram(summary: page.summary)
                if page.viewer.canRate {
                    Button(page.viewer.review == nil ? "Rate this package" : "Edit your rating") { rating = true }
                } else if page.viewer.signedIn {
                    Text(page.viewer.cannotRateReason).font(.footnote).foregroundStyle(.secondary)
                }
                ForEach(page.reviews) { review in
                    ReviewRow(review: review) { Task { await reviews.toggleHelpful(review) } }
                }
            } else if reviews.loading {
                ProgressView()
            }
            PackagesErrorBanner(text: reviews.errorText)
        } header: {
            Text("Ratings")
        }
    }

    private func webURL(_ path: String) -> URL? {
        guard let api = AppServices.current?.api else { return nil }
        return api.url(path)
    }
}

struct PackageExportDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.json] }

    let data: Data

    init(data: Data) {
        self.data = data
    }

    init(configuration: ReadConfiguration) throws {
        data = configuration.file.regularFileContents ?? Data()
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: data)
    }
}

struct RatingHistogram: View {
    let summary: RatingSummary

    var body: some View {
        HStack(alignment: .center, spacing: 16) {
            VStack {
                Text(summary.count == 0 ? "–" : String(format: "%.1f", summary.average)).font(.largeTitle.bold())
                Text("\(summary.count) ratings").font(.caption).foregroundStyle(.secondary)
            }
            VStack(spacing: 2) {
                ForEach((1...5).reversed(), id: \.self) { stars in
                    let count = summary.histogram[stars - 1]
                    HStack(spacing: 6) {
                        Text("\(stars)").font(.caption2.monospacedDigit()).frame(width: 12)
                        GeometryReader { proxy in
                            Capsule().fill(.quaternary)
                                .overlay(alignment: .leading) {
                                    Capsule().fill(.orange)
                                        .frame(width: summary.count == 0 ? 0 : proxy.size.width * CGFloat(count) / CGFloat(summary.count))
                                }
                        }
                        .frame(height: 6)
                    }
                }
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(summary.count == 0 ? "No ratings yet" : "\(summary.label), average out of 5 stars")
    }
}

struct ReviewRow: View {
    let review: MarketReview
    let onHelpful: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(String(repeating: "★", count: review.stars) + String(repeating: "☆", count: 5 - review.stars))
                    .foregroundStyle(.orange)
                    .accessibilityLabel("\(review.stars) stars")
                Text(review.reviewerLabel).font(.subheadline.weight(.semibold))
                if review.verifiedUse {
                    Image(systemName: "checkmark.seal").foregroundStyle(.green).accessibilityLabel("Verified use")
                }
                Spacer()
                Text(PackagesFormat.relative(review.updatedAt)).font(.caption).foregroundStyle(.secondary)
            }
            if !review.problem.isEmpty { LabeledText(title: "What didn't work", text: review.problem) }
            if !review.improvement.isEmpty { LabeledText(title: "How to improve", text: review.improvement) }
            if !review.comment.isEmpty { Text(review.comment).font(.subheadline) }
            if let reply = review.reply {
                LabeledText(title: "Reply from \(reply.by)", text: reply.text)
                    .padding(8)
                    .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 8))
            }
            HStack {
                if !review.addressedIn.isEmpty {
                    Label("Addressed in \(review.addressedIn)", systemImage: "checkmark").font(.caption).foregroundStyle(.green)
                }
                Spacer()
                Button(action: onHelpful) {
                    Label("\(review.helpful)", systemImage: review.viewerFoundHelpful ? "hand.thumbsup.fill" : "hand.thumbsup")
                }
                .buttonStyle(.borderless)
                .accessibilityLabel("Helpful, \(review.helpful)")
            }
        }
    }
}

struct LabeledText: View {
    let title: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            Text(text).font(.subheadline)
        }
    }
}

struct RatePackageSheet: View {
    @Bindable var model: PackageReviewsModel
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Form {
            Section {
                HStack {
                    ForEach(1...5, id: \.self) { star in
                        Button {
                            model.draft.stars = star
                        } label: {
                            Image(systemName: star <= model.draft.stars ? "star.fill" : "star")
                                .font(.title2)
                                .foregroundStyle(.orange)
                                .frame(minWidth: 44, minHeight: 44)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("\(star) stars")
                    }
                }
            }
            if model.draft.needsReview(model.rules) {
                Section("What didn't work or was missing") {
                    TextField("Required below five stars", text: $model.draft.problem, axis: .vertical).lineLimit(3...8)
                    fieldError("problem")
                }
                Section("How we can improve") {
                    TextField("What would make it five stars", text: $model.draft.improvement, axis: .vertical)
                        .lineLimit(3...8)
                    fieldError("improvement")
                }
                Section("Category") {
                    Picker("Category", selection: $model.draft.category) {
                        Text("None").tag("")
                        ForEach(model.rules.categories, id: \.id) { Text($0.label).tag($0.id) }
                    }
                }
            }
            Section("Anything else") {
                TextField("Optional", text: $model.draft.comment, axis: .vertical).lineLimit(2...6)
                fieldError("comment")
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("Rate")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
            ToolbarItem(placement: .confirmationAction) {
                Button("Submit") {
                    Task {
                        if await model.submit() {
                            FmHaptics.commit()
                            dismiss()
                        }
                    }
                }
                .disabled(!model.canSubmit)
            }
        }
    }

    @ViewBuilder private func fieldError(_ key: String) -> some View {
        if let code = model.fieldIssues[key] {
            Text(ReviewDraft.fieldMessage(code)).font(.caption).foregroundStyle(.red)
                .fmMessageTools(ReviewDraft.fieldMessage(code))
        }
    }
}

struct AccountPackageAction: Identifiable {
    let packageId: String
    let name: String
    let install: Bool
    let others: [String]
    var project: Bool = false

    var id: String { (install ? "install-" : "remove-") + packageId }

    var title: String {
        if project { return install ? "Install \(name) in this project?" : "Remove \(name) from this project?" }
        return install ? "Install \(name) on all your devices?" : "Remove \(name) from all your devices?"
    }

    var message: String {
        let list: String = others.formatted(.list(type: .and))
        if install {
            return others.count == 1
                ? "\(list) comes back too, because \(name) needs it."
                : "\(list) come back too, because \(name) needs them."
        }
        var base: String = project ? "Your other projects keep it." : "It goes from your phone, your desktop and the web."
        if packageId == AccountPackagesStore.statusPage && !project { base += " Your own monitors are deleted." }
        if others.isEmpty { return base }
        return base + " " + (others.count == 1 ? "\(list) needs it, so it is removed too." : "These need it and are removed too: \(list).")
    }
}

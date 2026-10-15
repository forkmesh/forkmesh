import FMApi
import FMPackages
import FMTools
import SwiftUI
import UniformTypeIdentifiers

struct PluginMenuActions {
    let name: String
    let rateLabel: String
    let package: MarketPackage?
    let uninstall: MarketUninstall?
    let rate: () -> Void
    let feedback: () -> Void
    let chooseUninstall: (String) -> Void
}

private struct PluginMenuActionsKey: EnvironmentKey {
    static let defaultValue: PluginMenuActions? = nil
}

extension EnvironmentValues {
    var pluginMenu: PluginMenuActions? {
        get { self[PluginMenuActionsKey.self] }
        set { self[PluginMenuActionsKey.self] = newValue }
    }
}

struct PluginMenuItems: View {
    @Environment(\.pluginMenu) private var actions

    var body: some View {
        if let actions {
            Divider()
            PluginMenuContent(actions: actions)
        }
    }
}

struct PluginMoreMenu: View {
    @Environment(\.pluginMenu) private var actions

    var body: some View {
        if let actions {
            PluginMoreButton(actions: actions)
        }
    }
}

struct PluginMoreButton: View {
    let actions: PluginMenuActions

    var body: some View {
        Menu {
            PluginMenuContent(actions: actions)
        } label: {
            Label("More", systemImage: "ellipsis.circle")
        }
        .accessibilityIdentifier(PluginMenu.buttonIdentifier)
    }
}

struct PluginMenuContent: View {
    let actions: PluginMenuActions

    var body: some View {
        ForEach(PluginMenu.items(canUninstall: actions.package != nil && actions.uninstall != nil), id: \.self) {
            (item: PluginMenu.Item) in
            switch item {
            case .rate:
                Button {
                    actions.rate()
                } label: {
                    Label(actions.rateLabel, systemImage: "star")
                }
                .accessibilityIdentifier("plugin-more-rate")
            case .feedback:
                Button {
                    actions.feedback()
                } label: {
                    Label(PluginMenu.feedbackLabel, systemImage: "exclamationmark.bubble")
                }
                .accessibilityIdentifier("plugin-more-feedback")
            case .uninstall:
                if let package = actions.package, let uninstall = actions.uninstall {
                    Divider()
                    PackageUninstallMenuItems(package: package, uninstall: uninstall, onChoose: actions.chooseUninstall)
                }
            }
        }
    }
}

struct PluginMenuHost: ViewModifier {
    let route: Route
    let packageId: String
    @State private var market: MarketModel
    @State private var reviews: PackageReviewsModel
    @State private var rating = false
    @State private var reporting = false
    @State private var uninstallRequest: PackageUninstallRequest?
    @State private var exportURL: URL?
    @State private var exporting = false
    @Environment(\.dismiss) private var dismiss

    init(client: PackagesClient, route: Route, packageId: String) {
        self.route = route
        self.packageId = packageId
        _market = State(initialValue: MarketModel(client: client, account: PackagesFeature.accountPackages, ledger: .shared))
        _reviews = State(initialValue: PackageReviewsModel(client: client, packageId: packageId))
    }

    private var package: MarketPackage? { market.package(packageId) }

    private var name: String { PluginMenu.name(catalog: package?.name, route: route) }

    private var uninstall: MarketUninstall? {
        package.flatMap { (package: MarketPackage) in market.uninstallAction(package, signedIn: PackagesFeature.signedIn) }
    }

    private var actions: PluginMenuActions {
        PluginMenuActions(
            name: name,
            rateLabel: PluginMenu.rateLabel(name, rated: reviews.page?.viewer.review != nil),
            package: package,
            uninstall: uninstall,
            rate: { rating = true },
            feedback: { reporting = true },
            chooseUninstall: { (organization: String) in
                guard let package, let uninstall else { return }
                uninstallRequest = PackageUninstallRequest(package, uninstall, organization: organization, account: market.account)
            })
    }

    func body(content: Content) -> some View {
        content
            .toolbar {
                if PluginMenu.addsButton(for: route) {
                    ToolbarItem(placement: .topBarTrailing) {
                        PluginMoreButton(actions: actions)
                    }
                }
            }
            .environment(\.pluginMenu, actions)
            .task(id: packageId) {
                await market.refreshInstalls(organizations: await PackagesFeature.organizationNames())
            }
            .packageUninstall(
                $uninstallRequest, market: market,
                onRemoved: { (removal: PackageRemoval) in
                    exportURL = PackageUninstallRequest.exportFile(removal)
                    exporting = exportURL != nil
                },
                onUninstalled: {
                    if !exporting { dismiss() }
                })
            .fileExporter(
                isPresented: $exporting, document: market.removed?.export.map(PackageExportDocument.init(data:)), contentType: .json,
                defaultFilename: exportURL?.lastPathComponent
            ) { _ in
                dismiss()
            }
            .sheet(isPresented: $rating) {
                NavigationStack { RatePackageSheet(model: reviews) }
                    .task { await reviews.refresh() }
            }
            .pluginFeedbackSheet(isPresented: $reporting, packageId: packageId, name: name, screen: route.title)
    }
}

struct PluginFeedbackSheet: ViewModifier {
    @Binding var isPresented: Bool
    let packageId: String
    let name: String
    let screen: String
    @State private var thanks: FmNotice?

    func body(content: Content) -> some View {
        content
            .sheet(isPresented: $isPresented) {
                ReportBugSheet(
                    screen: screen, package: packageId, packageName: name,
                    onFinished: { (filed: Bool) in
                        if filed {
                            FmHaptics.commit()
                            thanks = FmNotice(ReportBugModel.feedbackThanks(name))
                        }
                    })
            }
            .overlay(alignment: .top) {
                if thanks != nil {
                    FmNoticeLine($thanks)
                        .padding(.horizontal, FmSpace.x3)
                        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: FmRadius.md))
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.top, FmSpace.x2)
                        .task(id: thanks?.id) {
                            try? await Task.sleep(for: .seconds(4))
                            thanks = nil
                        }
                        .accessibilityIdentifier("plugin-feedback-thanks")
                }
            }
    }
}

extension View {
    @ViewBuilder
    func pluginMenu(for route: Route) -> some View {
        if let packageId = PluginMenu.packageId(for: route), let client = PackagesFeature.client {
            modifier(PluginMenuHost(client: client, route: route, packageId: packageId))
        } else {
            self
        }
    }

    func pluginFeedbackSheet(isPresented: Binding<Bool>, packageId: String, name: String, screen: String) -> some View {
        modifier(PluginFeedbackSheet(isPresented: isPresented, packageId: packageId, name: name, screen: screen))
    }
}

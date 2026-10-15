// Gate 2 (docs/native/NAVIGATION.md section 3.1): what an unregistered route shows, so every
// destination in the sitemap can be reached before its screen exists. WS8 owns this file.

import FMApi
import FMPackages
import SwiftUI

struct RoutePlaceholderView: View {
    let route: Route

    var body: some View {
        ContentUnavailableView {
            Label("Coming soon", systemImage: "hammer")
        } description: {
            Text("\(route.title) is not in this version of the app yet.")
        }
        .navigationTitle(route.title)
        .navigationBarTitleDisplayMode(.inline)
    }
}

/// A route's registered view (AppServices.routes), or the placeholder when the services are not up
/// (previews) or nobody has registered it yet. The one view every `navigationDestination(for:
/// Route.self)` returns.
struct RouteDestinationView: View {
    let route: Route

    var body: some View {
        if let package = PackageGate.packageId(for: route), PhonePackages.isRemoved(package) {
            RemovedPackageView(packageId: package, route: route)
        } else if let services = AppServices.current {
            services.routes.destination(for: route)
                .pluginMenu(for: route)
                .task(id: route) {
                    guard let package = PackageGate.packageId(for: route), PackagesFeature.signedIn else { return }
                    PackageActivity.shared.report(package, kind: .open, client: PackagesFeature.client)
                }
        } else {
            RoutePlaceholderView(route: route)
        }
    }
}

struct RemovedPackageView: View {
    let packageId: String
    let route: Route

    var body: some View {
        ContentUnavailableView {
            Label("\(route.title) is uninstalled", systemImage: "puzzlepiece.extension")
        } description: {
            Text("You removed it from this phone. Install it again from the Market to use it.")
        } actions: {
            NavigationLink(value: Route.package(id: packageId)) {
                Text("Open in the Market")
            }
            .buttonStyle(.borderedProminent)
            .accessibilityIdentifier("removed-package-open-market")
        }
        .navigationTitle(route.title)
        .navigationBarTitleDisplayMode(.inline)
    }
}

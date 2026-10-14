import FMTools
import SwiftUI

enum DocsBundle {
    static let shared: Result<DocsLibrary, DocsLibraryError> = load()

    static func load() -> Result<DocsLibrary, DocsLibraryError> {
        guard let url = Bundle.main.url(forResource: "DocsLibrary", withExtension: "json"),
            let data = try? Data(contentsOf: url)
        else {
            return .failure(.unreadable("DocsLibrary.json is not in the app bundle"))
        }
        do {
            return .success(try DocsLibrary(data: data))
        } catch let error as DocsLibraryError {
            return .failure(error)
        } catch {
            return .failure(.unreadable(String(describing: error)))
        }
    }
}

struct DocsPageLink: Hashable, Identifiable {
    let path: String
    let fragment: String
    var id: String { path + "#" + fragment }
}

struct DocsScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var library: DocsLibrary?
    @State private var failed: Bool = false
    @State private var query: String = ""
    @State private var searched: String = ""
    @State private var hits: [DocsHit] = []
    @State private var matches: [DocsDependency] = []
    @State private var opened: DocsPageLink?
    @State private var openedDependency: DocsDependency?
    @State private var showDependencies: Bool = false

    var body: some View {
        content
            .background(fm.canvas.ignoresSafeArea())
            .navigationTitle("Docs")
            .navigationBarTitleDisplayMode(.inline)
            .task { await load() }
    }

    @ViewBuilder private var content: some View {
        if let library = library {
            browser(library)
        } else if failed {
            FmEmptyState(icon: "book_outlined", title: "Docs unavailable", message: "This build does not include the documentation library.")
                .accessibilityIdentifier("docs-unavailable")
        } else {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .accessibilityLabel("Loading the documentation library")
        }
    }

    private func load() async {
        guard library == nil else { return }
        let result: Result<DocsLibrary, DocsLibraryError> = await Task.detached(priority: .userInitiated) { () -> Result<DocsLibrary, DocsLibraryError> in
            DocsBundle.shared
        }.value
        switch result {
        case let .success(loaded):
            library = loaded
        case .failure:
            failed = true
        }
    }

    private var trimmedQuery: String {
        query.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func browser(_ library: DocsLibrary) -> some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                if trimmedQuery.isEmpty {
                    overview(library)
                } else {
                    results(library)
                }
            }
            .frame(maxWidth: 820, alignment: .leading)
            .frame(maxWidth: .infinity)
            .padding(.bottom, FmSpace.x6)
        }
        .searchable(text: $query, prompt: "Search the docs")
        .task(id: query) { await search(library) }
        .navigationDestination(item: $opened) { (link: DocsPageLink) in
            DocsPageScreen(library: library, path: link.path, fragment: link.fragment)
                .shellPageClearance()
        }
        .navigationDestination(item: $openedDependency) { (dependency: DocsDependency) in
            DocsDependencyScreen(library: library, dependency: dependency)
                .shellPageClearance()
        }
        .navigationDestination(isPresented: $showDependencies) {
            DocsDependenciesScreen(library: library)
                .shellPageClearance()
        }
        .accessibilityIdentifier("docs-screen")
    }

    private func search(_ library: DocsLibrary) async {
        let text: String = trimmedQuery
        guard !text.isEmpty else {
            hits = []
            matches = []
            searched = ""
            return
        }
        try? await Task.sleep(for: .milliseconds(120))
        if Task.isCancelled { return }
        let found: ([DocsHit], [DocsDependency]) = await Task.detached(priority: .userInitiated) { () -> ([DocsHit], [DocsDependency]) in
            (library.search(text, limit: 60), library.searchDependencies(text))
        }.value
        if Task.isCancelled { return }
        hits = found.0
        matches = found.1
        searched = text
    }

    @ViewBuilder private func overview(_ library: DocsLibrary) -> some View {
        ForEach(library.areas) { (area: DocsArea) in
            if area.key == DocsLibrary.dependenciesArea {
                if !library.dependencies.isEmpty {
                    areaHeader(area, count: nil)
                    FmListRow(
                        title: area.label,
                        subtitle: "\(library.dependencies.count) open-source dependencies",
                        leading: AnyView(FmIconTile(icon: "extension_outlined", tint: fm.accent)),
                        onTap: { showDependencies = true })
                        .accessibilityIdentifier("docs-dependencies")
                }
            } else {
                let pages: [DocsPage] = library.pages(in: area.key)
                if !pages.isEmpty {
                    areaHeader(area, count: pages.count)
                    ForEach(pages) { (page: DocsPage) in
                        pageRow(page)
                        if page.id != pages.last?.id {
                            FmRowDivider()
                        }
                    }
                }
            }
        }
    }

    private func areaHeader(_ area: DocsArea, count: Int?) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            FmGroupHeader(label: area.label, count: count, padding: EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x5, FmSpace.x4, area.blurb.isEmpty ? FmSpace.x2 : FmSpace.x1))
                .accessibilityAddTraits(.isHeader)
            if !area.blurb.isEmpty {
                Text(area.blurb)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.bottom, FmSpace.x2)
            }
        }
    }

    private func pageRow(_ page: DocsPage) -> some View {
        FmListRow(
            title: page.title,
            subtitle: page.description,
            leading: AnyView(FmIconTile(icon: page.asset ? "data_object_rounded" : "article_outlined")),
            onTap: { opened = DocsPageLink(path: page.path, fragment: "") })
            .accessibilityIdentifier("docs-page-\(page.path)")
    }

    @ViewBuilder private func results(_ library: DocsLibrary) -> some View {
        if searched.isEmpty {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity)
                .padding(.top, FmSpace.x6)
        } else if hits.isEmpty && matches.isEmpty {
            FmEmptyState(icon: "search_off", title: "Nothing matches “\(searched)”", message: "Try fewer or different words.")
                .padding(.top, FmSpace.x6)
        } else {
            if !hits.isEmpty {
                FmGroupHeader(label: "Documents", count: hits.count)
                ForEach(hits) { (hit: DocsHit) in
                    hitRow(hit)
                    if hit.id != hits.last?.id {
                        FmRowDivider()
                    }
                }
            }
            if !matches.isEmpty {
                FmGroupHeader(label: library.dependenciesLabel, count: matches.count)
                ForEach(matches) { (dependency: DocsDependency) in
                    FmListRow(
                        title: dependency.title,
                        subtitle: dependency.ecosystemLabel + " \u{00B7} " + dependency.summary,
                        leading: AnyView(FmIconTile(icon: "extension_outlined", tint: fm.accent)),
                        onTap: { openedDependency = dependency })
                        .accessibilityIdentifier("docs-dependency-\(dependency.id)")
                }
            }
        }
    }

    private func hitRow(_ hit: DocsHit) -> some View {
        FmListRow(
            title: hit.title,
            subtitleView: AnyView(
                Text(hit.snippet)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .lineLimit(2)
                    .truncationMode(.tail)),
            leading: AnyView(FmIconTile(icon: "article_outlined")),
            onTap: { opened = DocsPageLink(path: hit.path, fragment: "") })
            .accessibilityIdentifier("docs-hit-\(hit.path)")
    }
}

import FMTools
import SwiftUI

struct DocsDependenciesScreen: View {
    @Environment(\.fmPalette) private var fm

    let library: DocsLibrary
    @State private var query: String = ""
    @State private var opened: DocsDependency?

    var body: some View {
        let groups: [DocsDependencyGroup] = DocsLibrary.groups(library.searchDependencies(query))
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                Text("\(library.dependencies.count) open-source dependencies, \(library.onGitHub) of them on GitHub. Each links its repository and its top three authors by commits.")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.top, FmSpace.x3)
                if groups.isEmpty {
                    FmEmptyState(icon: "search_off", title: "No dependency matches “\(query.trimmingCharacters(in: .whitespaces))”")
                        .padding(.top, FmSpace.x6)
                }
                ForEach(groups) { (group: DocsDependencyGroup) in
                    FmGroupHeader(label: group.label, count: group.dependencies.count, padding: EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x5, FmSpace.x4, FmSpace.x2))
                        .accessibilityAddTraits(.isHeader)
                    ForEach(group.dependencies) { (dependency: DocsDependency) in
                        FmListRow(
                            title: dependency.title,
                            subtitle: dependency.summary,
                            leading: AnyView(FmIconTile(icon: "extension_outlined", tint: dependency.repository.isEmpty ? nil : fm.accent)),
                            onTap: { opened = dependency })
                            .accessibilityIdentifier("docs-dependency-\(dependency.id)")
                        if dependency.id != group.dependencies.last?.id {
                            FmRowDivider()
                        }
                    }
                }
                if !library.dependenciesGenerated.isEmpty {
                    Text("Snapshot of \(library.dependenciesGenerated)")
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.vertical, FmSpace.x5)
                }
            }
            .frame(maxWidth: 820, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
        .searchable(text: $query, prompt: "Search dependencies or authors")
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle(library.dependenciesLabel)
        .navigationBarTitleDisplayMode(.inline)
        .navigationDestination(item: $opened) { (dependency: DocsDependency) in
            DocsDependencyScreen(library: library, dependency: dependency)
                .shellPageClearance()
        }
        .accessibilityIdentifier("docs-dependencies-screen")
    }
}

struct DocsDependencyScreen: View {
    @Environment(\.fmPalette) private var fm

    let library: DocsLibrary
    let dependency: DocsDependency

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x4) {
                summaryCard
                gitHubSection
                authorsSection
                usedBySection
                Text("Snapshot of \(dependency.fetched.isEmpty ? (library.dependenciesGenerated.isEmpty ? "an unknown date" : library.dependenciesGenerated) : dependency.fetched)")
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            .padding(FmSpace.x4)
            .frame(maxWidth: 820, alignment: .leading)
            .frame(maxWidth: .infinity)
        }
        .tint(fm.accent)
        .environment(\.openURL, referenceOpener)
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle(dependency.title)
        .navigationBarTitleDisplayMode(.inline)
        .accessibilityIdentifier("docs-dependency-screen")
    }

    private var summaryCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text(dependency.title)
                    .fmText(.titleLarge)
                    .fixedSize(horizontal: false, vertical: true)
                    .accessibilityAddTraits(.isHeader)
                if dependency.name != dependency.title {
                    Text(dependency.name)
                        .fmMono(size: 13, color: fm.textSecondary)
                        .textSelection(.enabled)
                        .accessibilityLabel("Package \(dependency.name)")
                }
                if !dependency.description.isEmpty {
                    Text(dependency.description)
                        .fmText(.bodyMedium)
                        .fixedSize(horizontal: false, vertical: true)
                }
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: FmSpace.x2) {
                        facts
                    }
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        facts
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    @ViewBuilder private var facts: some View {
        FmStatusBadge(label: dependency.ecosystemLabel, color: fm.accent)
        Text(dependency.version.isEmpty ? "Version unpinned" : "Version \(dependency.version)")
            .fmText(.bodySmall, color: fm.textSecondary, tabular: true)
        if dependency.development {
            FmStatusBadge(label: "Development only", color: fm.warning)
        }
    }

    private var gitHubSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "GitHub")
            if let url = URL(string: dependency.repository), !dependency.repository.isEmpty {
                Link(destination: url) {
                    linkLabel(dependency.repositoryName, icon: "link")
                }
                .accessibilityIdentifier("docs-dependency-github")
            } else {
                Text("No GitHub repository is known")
                    .fmText(.bodyMedium, color: fm.textSecondary)
            }
        }
    }

    private var authorsSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Top authors")
            if dependency.authors.isEmpty {
                Text(dependency.authorsNote.isEmpty ? "Top authors not recorded yet." : dependency.authorsNote)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                ForEach(Array(dependency.authors.enumerated()), id: \.element.id) { (pair: (offset: Int, element: DocsAuthor)) in
                    authorCard(pair.element, rank: pair.offset + 1)
                }
            }
        }
    }

    private func authorCard(_ author: DocsAuthor, rank: Int) -> some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                    Text("\(rank).")
                        .fmText(.titleSmall, color: fm.textTertiary, tabular: true)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(author.name)
                            .fmText(.titleSmall)
                            .fixedSize(horizontal: false, vertical: true)
                        if author.name != author.login {
                            Text("@\(author.login)")
                                .fmText(.bodySmall, color: fm.textSecondary)
                        }
                        Text(author.commitsLabel)
                            .fmText(.bodySmall, color: fm.textTertiary, tabular: true)
                    }
                }
                .accessibilityElement(children: .combine)
                ViewThatFits(in: .horizontal) {
                    HStack(spacing: FmSpace.x4) {
                        authorLinks(author)
                    }
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        authorLinks(author)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .accessibilityIdentifier("docs-author-\(author.login)")
    }

    @ViewBuilder private func authorLinks(_ author: DocsAuthor) -> some View {
        if let projects = URL(string: author.projects) {
            Link(destination: projects) {
                linkLabel(author.projectsLabel, icon: "folder_outlined")
            }
            .accessibilityLabel("\(author.projectsLabel) by \(author.name)")
        }
        if let profile = URL(string: author.profile) {
            Link(destination: profile) {
                linkLabel("Profile", icon: "person_outline")
            }
            .accessibilityLabel("\(author.name) on GitHub")
        }
        if !author.website.isEmpty, let website = URL(string: author.website) {
            Link(destination: website) {
                linkLabel("Website", icon: "public")
            }
            .accessibilityLabel("Website of \(author.name)")
        }
    }

    @ViewBuilder private var usedBySection: some View {
        if !dependency.usedBy.isEmpty {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                FmSectionHeader(title: "Used by")
                ForEach(dependency.usedBy, id: \.self) { (path: String) in
                    if let url = library.sourceURL(path) {
                        Link(destination: url) {
                            linkLabel(path, icon: "description_outlined", mono: true)
                        }
                    } else {
                        Text(path)
                            .fmMono(size: 13)
                    }
                }
            }
        }
    }

    private func linkLabel(_ title: String, icon: String, mono: Bool = false) -> some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon(icon, size: 18, color: fm.accent)
            if mono {
                Text(title)
                    .fmMono(size: 13, color: fm.accent)
                    .lineLimit(2)
                    .multilineTextAlignment(.leading)
            } else {
                Text(title)
                    .fmText(.bodyMedium, color: fm.accent)
                    .lineLimit(2)
                    .multilineTextAlignment(.leading)
            }
        }
        .frame(minHeight: 44, alignment: .leading)
        .contentShape(Rectangle())
    }
}

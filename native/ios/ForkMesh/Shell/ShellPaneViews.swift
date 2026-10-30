import FMApi
import FMBase
import Observation
import SwiftUI
import UIKit

enum ShellPaneRole: Equatable {
    case list
    case detail
}

private struct ShellPanesKey: EnvironmentKey {
    static let defaultValue: ShellPaneMetrics = .phone
}

private struct ShellPaneRoleKey: EnvironmentKey {
    static let defaultValue: ShellPaneRole? = nil
}

private struct ShellPaneOpenKey: EnvironmentKey {
    static let defaultValue: Route? = nil
}

extension EnvironmentValues {
    var shellPanes: ShellPaneMetrics {
        get { self[ShellPanesKey.self] }
        set { self[ShellPanesKey.self] = newValue }
    }

    var shellPaneRole: ShellPaneRole? {
        get { self[ShellPaneRoleKey.self] }
        set { self[ShellPaneRoleKey.self] = newValue }
    }

    var shellPaneOpen: Route? {
        get { self[ShellPaneOpenKey.self] }
        set { self[ShellPaneOpenKey.self] = newValue }
    }
}

struct ShellDetailPage {
    let id: AnyHashable
    let view: AnyView
    let release: @MainActor () -> Void
}

@MainActor
@Observable
final class ShellDetailHost {
    static let shared = ShellDetailHost()

    private(set) var pages: [AppTab: ShellDetailPage] = [:]
    var active: Bool = false

    func page(_ tab: AppTab) -> ShellDetailPage? { pages[tab] }

    func closes(_ tab: AppTab) -> Bool {
        guard active, pages[tab] != nil, let router = AppNavigator.shared?.router else { return false }
        let path: [Route] = router.path(for: tab)
        guard path == ShellPanes.listPrefix(ShellPanes.layout(path: path, tab: tab, wide: true)) else { return false }
        clear(tab)
        return true
    }

    func show(_ page: ShellDetailPage, on tab: AppTab) {
        let old: ShellDetailPage? = pages[tab]
        pages[tab] = page
        if let router = AppNavigator.shared?.router {
            let layout: ShellPaneLayout = ShellPanes.layout(path: router.path(for: tab), tab: tab, wide: true)
            let prefix: [Route] = ShellPanes.listPrefix(layout)
            if router.path(for: tab) != prefix {
                router.setPath(prefix, for: tab)
            }
        }
        if let old, old.id != page.id {
            old.release()
        }
    }

    func clear(_ tab: AppTab, id: AnyHashable? = nil) {
        guard let current = pages[tab] else { return }
        if let id, current.id != id { return }
        pages[tab] = nil
        current.release()
    }

    func clearAll() {
        for tab in Array(pages.keys) {
            clear(tab)
        }
    }
}

private struct ShellDetailDestination<Item: Hashable, Page: View>: ViewModifier {
    @Environment(\.shellPaneRole) private var role
    @Environment(\.shellStackTab) private var tab
    @Binding var item: Item?
    let page: (Item) -> Page

    @State private var shown: AnyHashable?

    private var hosted: Bool { role == .list && tab != nil }

    func body(content: Content) -> some View {
        content
            .navigationDestination(item: hosted ? .constant(nil) : $item, destination: page)
            .onChange(of: item, initial: true) { (_: Item?, next: Item?) in
                sync(next)
            }
    }

    private func sync(_ next: Item?) {
        guard hosted, let tab else { return }
        guard let next else {
            if let shown {
                ShellDetailHost.shared.clear(tab, id: shown)
            }
            shown = nil
            return
        }
        let id: AnyHashable = AnyHashable(next)
        shown = id
        let binding: Binding<Item?> = $item
        ShellDetailHost.shared.show(
            ShellDetailPage(
                id: id, view: AnyView(page(next)),
                release: {
                    if binding.wrappedValue != nil {
                        binding.wrappedValue = nil
                    }
                }),
            on: tab)
    }
}

extension View {
    func shellDetail<Item: Hashable, Page: View>(
        item: Binding<Item?>, @ViewBuilder destination: @escaping (Item) -> Page
    ) -> some View {
        modifier(ShellDetailDestination(item: item, page: destination))
    }

    func shellDetail<Page: View>(isPresented: Binding<Bool>, @ViewBuilder destination: @escaping () -> Page)
        -> some View
    {
        let item: Binding<Bool?> = Binding<Bool?>(
            get: { isPresented.wrappedValue ? true : nil },
            set: { (next: Bool?) in isPresented.wrappedValue = next != nil })
        return modifier(ShellDetailDestination(item: item, page: { (_: Bool) in destination() }))
    }

    func shellDetail<Page: View>(
        isPresented: Binding<Bool>, id: AnyHashable?, @ViewBuilder destination: @escaping () -> Page
    ) -> some View {
        let key: AnyHashable = id ?? AnyHashable(0)
        let item: Binding<AnyHashable?> = Binding<AnyHashable?>(
            get: { isPresented.wrappedValue ? key : nil },
            set: { (next: AnyHashable?) in isPresented.wrappedValue = next != nil })
        return modifier(ShellDetailDestination(item: item, page: { (_: AnyHashable) in destination() }))
    }
}

private struct ShellPaneSelection: ViewModifier {
    @Environment(\.shellPaneRole) private var role
    @Environment(\.fmPalette) private var fm

    let selected: Bool

    private var shown: Bool { role == .list && selected }

    func body(content: Content) -> some View {
        content
            .background {
                if shown {
                    RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous)
                        .fill(fm.accentSubtle)
                        .padding(.horizontal, FmSpace.x2)
                        .padding(.vertical, 2)
                        .accessibilityHidden(true)
                }
            }
            .accessibilityAddTraits(shown ? .isSelected : [])
    }
}

extension View {
    func shellPaneSelected(_ selected: Bool) -> some View {
        modifier(ShellPaneSelection(selected: selected))
    }
}

struct ShellPanePlaceholder: View {
    @Environment(\.fmPalette) private var fm

    let tab: AppTab
    let list: Route?

    private var symbol: String {
        list == nil ? tab.symbol : "sidebar.left"
    }

    private var title: String {
        guard list == nil else { return "Nothing open yet" }
        switch tab {
        case .agents: return "Pick an agent"
        case .inbox: return "Pick a ping"
        case .code: return "Pick a repository"
        case .chat: return "Pick a conversation"
        }
    }

    private var hint: String {
        guard list == nil else { return "Choose something on the left and it opens here." }
        switch tab {
        case .agents: return "Its run, transcript and changes open here, beside the list."
        case .inbox: return "Reviews, mentions and approvals open here, beside the list."
        case .code: return "Its code, pull requests and issues open here, beside the list."
        case .chat: return "The conversation opens here."
        }
    }

    var body: some View {
        VStack(spacing: FmSpace.x3) {
            Image(systemName: symbol)
                .font(.system(size: 34, weight: .light))
                .foregroundStyle(fm.accent)
                .frame(width: 76, height: 76)
                .modifier(ShellBarBackdrop(shape: AnyShape(Circle()), fill: .glass, interactive: false, lift: 8))
                .accessibilityHidden(true)
            Text(title)
                .font(.title3.weight(.semibold))
                .foregroundStyle(fm.textPrimary)
            Text(hint)
                .font(.subheadline)
                .foregroundStyle(fm.textSecondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 300)
        }
        .padding(FmSpace.x6)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(fm.canvas.ignoresSafeArea())
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("shell-pane-placeholder")
    }
}

struct ShellSideRail: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let items: [ShellBarItem]
    var onPackage: (String) -> Void = { (_: String) in }

    @State private var prefs: ShellBarPrefs = ShellBarPrefs.shared
    @State private var naming: ShellBarNaming?
    @State private var settingsShown: Bool = false

    @ScaledMetric(relativeTo: .body) private var iconSize: CGFloat = 20

    static let cellWidth: CGFloat = 60
    static let cellHeight: CGFloat = 54

    private var style: ShellBarStyle { prefs.barStyle }

    private var glyphSize: CGFloat { (iconSize * CGFloat(style.size.scale)).rounded() }

    private var cellShape: AnyShape {
        AnyShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
    }

    private var railShape: AnyShape {
        AnyShape(RoundedRectangle(cornerRadius: 30, style: .continuous))
    }

    var body: some View {
        container {
            ScrollView(.vertical, showsIndicators: false) {
                VStack(spacing: FmSpace.x1) {
                    ForEach(prefs.entries(items)) { (entry: ShellBarEntry) in
                        slot(entry)
                    }
                }
                .padding(.vertical, FmSpace.x2)
                .frame(maxWidth: .infinity)
            }
            .scrollBounceBehavior(.basedOnSize, axes: .vertical)
            .frame(width: ShellSideRail.cellWidth + FmSpace.x2)
            .frame(maxHeight: .infinity, alignment: .top)
            .shellBarBacked(style, shape: railShape)
        }
        .padding(.leading, FmSpace.x2)
        .padding(.vertical, FmSpace.x2)
        .dynamicTypeSize(...DynamicTypeSize.xxxLarge)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("shell-side-rail")
        .shellBarNaming($naming, prefs: prefs)
        .sheet(isPresented: $settingsShown) {
            ShellBarSettingsSheet(prefs: prefs)
        }
    }

    @ViewBuilder private func slot(_ entry: ShellBarEntry) -> some View {
        switch entry.slot {
        case let .item(item):
            cell(item)
                .contextMenu {
                    ShellBarItemMenu(
                        item: item, items: items, prefs: prefs,
                        onName: { (next: ShellBarNaming) in naming = next }, onPackage: onPackage)
                    Section {
                        Button {
                            settingsShown = true
                        } label: {
                            SwiftUI.Label("Bar settings…", systemImage: "slider.horizontal.3")
                        }
                    }
                }
        case let .folder(folder, members):
            ShellBarFolderCell(
                folder: folder, members: members, items: items, prefs: prefs, iconSize: glyphSize,
                cellWidth: ShellSideRail.cellWidth, cellHeight: ShellSideRail.cellHeight, shape: cellShape, look: style,
                onName: { (next: ShellBarNaming) in naming = next }, onPackage: onPackage,
                onSettings: { settingsShown = true })
        }
    }

    private func cell(_ item: ShellBarItem) -> some View {
        Button {
            FmHaptics.selection()
            ShellBottomBar.run(item, prefs: prefs, reduceMotion: reduceMotion)
        } label: {
            ShellBarCellLabel(item: item, iconSize: glyphSize, showsCounts: style.showCounts, look: style)
                .frame(width: ShellSideRail.cellWidth, height: ShellSideRail.cellHeight)
                .shellBarSlotChrome(cellShape, style: style, selected: item.selected)
                .contentShape(cellShape)
        }
        .buttonStyle(ShellBarButtonStyle())
        .accessibilityLabel(Text(item.title))
        .accessibilityValue(Text(ShellBottomBar.value(item)))
        .accessibilityAddTraits(item.selected ? [.isSelected, .isButton] : [.isButton])
        .accessibilityShowsLargeContentViewer {
            SwiftUI.Label(item.title, systemImage: item.symbol)
        }
        .accessibilityIdentifier("side-rail-\(item.id)")
    }

    @ViewBuilder private func container<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            GlassEffectContainer(spacing: FmSpace.x2) {
                content()
            }
        } else {
            content()
        }
        #else
        content()
        #endif
    }
}

struct ShellPaneDivider: View {
    @Environment(\.fmPalette) private var fm

    var body: some View {
        Rectangle()
            .fill(fm.border.opacity(0.7))
            .frame(width: 0.5)
            .ignoresSafeArea()
            .accessibilityHidden(true)
    }
}

struct ShellPaneLink<Label: View, Destination: View>: View {
    @Environment(\.shellPaneRole) private var role
    @Environment(\.shellStackTab) private var tab

    private let destination: Destination
    private let label: Label

    init(@ViewBuilder destination: () -> Destination, @ViewBuilder label: () -> Label) {
        self.destination = destination()
        self.label = label()
    }

    var body: some View {
        if role == .list, let tab {
            Button {
                ShellPaneLink.open(AnyView(destination), on: tab)
            } label: {
                label
            }
        } else {
            NavigationLink {
                destination
            } label: {
                label
            }
        }
    }

    @MainActor static func open(_ page: AnyView, on tab: AppTab, id: AnyHashable? = nil) {
        ShellDetailHost.shared.show(
            ShellDetailPage(id: id ?? AnyHashable(UUID()), view: page, release: {}), on: tab)
    }
}

private struct ShellPaneRowLink<Destination: View>: ViewModifier {
    @Environment(\.shellPaneRole) private var role
    @Environment(\.shellStackTab) private var tab

    let id: AnyHashable?
    let destination: Destination

    func body(content: Content) -> some View {
        if role == .list, let tab {
            content
                .onTapGesture {
                    ShellPaneLink<EmptyView, AnyView>.open(AnyView(destination), on: tab, id: id)
                }
                .accessibilityAddTraits(.isButton)
                .shellPaneSelected(id != nil && ShellDetailHost.shared.page(tab)?.id == id)
        } else {
            content
                .background {
                    NavigationLink {
                        destination
                    } label: {
                        EmptyView()
                    }
                    .opacity(0)
                }
        }
    }
}

extension View {
    func shellPaneRowLink<Destination: View>(id: AnyHashable? = nil, @ViewBuilder _ destination: () -> Destination)
        -> some View
    {
        modifier(ShellPaneRowLink(id: id, destination: destination()))
    }
}

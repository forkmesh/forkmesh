import FMApi
import FMBase
import FMPackages
import SwiftUI

struct ShellBarEntry: Identifiable {
    let slot: ShellBarSlot<ShellBarItem>

    var id: String { ShellBarLayout.slotId(slot, id: \.id) }

    func contains(_ itemId: String) -> Bool {
        switch slot {
        case let .item(item): return item.id == itemId
        case let .folder(_, members): return members.contains { $0.id == itemId }
        }
    }
}

enum ShellBarNaming: Equatable {
    case newFolder(item: String?)
    case rename(folder: String, current: String)

    var title: String {
        switch self {
        case .newFolder: return "New folder"
        case .rename: return "Rename folder"
        }
    }

    var confirm: String {
        switch self {
        case .newFolder: return "Create"
        case .rename: return "Rename"
        }
    }

    var initial: String {
        switch self {
        case .newFolder: return ""
        case let .rename(_, current): return current
        }
    }
}

struct ShellBarNamingAlert: ViewModifier {
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    @Binding var naming: ShellBarNaming?
    let prefs: ShellBarPrefs

    @State private var text: String = ""
    @State private var pending: ShellBarNaming?

    func body(content: Content) -> some View {
        content
            .alert(
                pending?.title ?? "",
                isPresented: Binding(get: { naming != nil }, set: { if !$0 { naming = nil } })
            ) {
                TextField("Folder", text: $text)
                    .accessibilityIdentifier("shell-bar-folder-name")
                Button("Cancel", role: .cancel) {}
                Button(pending?.confirm ?? "Save") { apply() }
                    .accessibilityIdentifier("shell-bar-folder-save")
            } message: {
                Text("Long press an icon to move it into a folder, or in and out of the bar.")
            }
            .onChange(of: naming) { (_: ShellBarNaming?, next: ShellBarNaming?) in
                guard let next = next else { return }
                pending = next
                text = next.initial
            }
    }

    private func apply() {
        guard let pending = pending else { return }
        let name: String = text
        withAnimation(reduceMotion ? nil : .snappy) {
            switch pending {
            case let .newFolder(item):
                prefs.arrange { (layout: inout ShellBarLayout) in _ = layout.createFolder(name, with: item) }
            case let .rename(folder, _):
                prefs.arrange { $0.renameFolder(folder, to: name) }
            }
        }
        FmHaptics.success()
    }
}

extension View {
    func shellBarNaming(_ naming: Binding<ShellBarNaming?>, prefs: ShellBarPrefs) -> some View {
        modifier(ShellBarNamingAlert(naming: naming, prefs: prefs))
    }
}

struct ShellBarStepItems: View {
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let slotId: String
    let items: [ShellBarItem]
    let prefs: ShellBarPrefs

    var body: some View {
        Menu {
            button("Move left", symbol: "arrow.left", step: .left)
            button("Move right", symbol: "arrow.right", step: .right)
            button("Move to the front", symbol: "arrow.left.to.line", step: .first)
            button("Move to the end", symbol: "arrow.right.to.line", step: .last)
        } label: {
            SwiftUI.Label("Move", systemImage: "arrow.left.arrow.right")
        }
        .accessibilityIdentifier("shell-bar-move")
    }

    private func button(_ title: String, symbol: String, step: ShellBarStep) -> some View {
        Button {
            withAnimation(reduceMotion ? nil : .snappy) {
                prefs.step(slotId, step, items: items)
            }
        } label: {
            SwiftUI.Label(title, systemImage: symbol)
        }
        .disabled(!prefs.canStep(slotId, step, items: items))
    }
}

struct ShellBarItemMenu: View {
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let item: ShellBarItem
    let items: [ShellBarItem]
    let prefs: ShellBarPrefs
    var inBar: Bool = true
    let onName: (ShellBarNaming) -> Void
    let onPackage: (String) -> Void

    var body: some View {
        let layout: ShellBarLayout = prefs.layout
        let docked: Bool = prefs.inBar(item.id, items: items)
        let current: ShellBarFolder? = layout.folderOf(item.id)
        if let package = PackageRoutes.packageId(forHubItem: item.id) {
            Section {
                Button {
                    onPackage(package)
                } label: {
                    SwiftUI.Label("\(item.title) settings", systemImage: "gearshape")
                }
                .accessibilityIdentifier("shell-bar-package-settings")
                Button(role: .destructive) {
                    onPackage(package)
                } label: {
                    SwiftUI.Label("Remove \(item.title)…", systemImage: "trash")
                }
                .accessibilityIdentifier("shell-bar-package-remove")
            }
        }
        Section {
            barItems(docked: docked)
        }
        Section {
            if inBar && docked {
                ShellBarStepItems(slotId: item.id, items: items, prefs: prefs)
            }
            Menu {
                ForEach(layout.folders) { (folder: ShellBarFolder) in
                    Button {
                        change { $0.move(item.id, toFolder: folder.id) }
                    } label: {
                        if folder.id == current?.id {
                            SwiftUI.Label(folder.name, systemImage: "checkmark")
                        } else {
                            Text(folder.name)
                        }
                    }
                    .disabled(folder.id == current?.id)
                }
                if !layout.folders.isEmpty {
                    Divider()
                }
                Button {
                    onName(.newFolder(item: item.id))
                } label: {
                    SwiftUI.Label("New folder…", systemImage: "folder.badge.plus")
                }
                .accessibilityIdentifier("shell-bar-new-folder")
            } label: {
                SwiftUI.Label("Move to folder", systemImage: "folder")
            }
            .accessibilityIdentifier("shell-bar-move-to-folder")
            if let current = current {
                Button {
                    change { $0.move(item.id, toFolder: nil) }
                } label: {
                    SwiftUI.Label("Take out of \(current.name)", systemImage: "folder.badge.minus")
                }
                .accessibilityIdentifier("shell-bar-take-out")
            }
        }
        if !layout.isEmpty {
            Section {
                ShellBarResetButton(prefs: prefs)
            }
        }
    }

    @ViewBuilder private func barItems(docked: Bool) -> some View {
        if ShellBarPrefs.keep.contains(item.id) {
            EmptyView()
        } else if docked {
            Button(role: .destructive) {
                withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.removeFromBar([item.id], items: items)
                }
                FmHaptics.success()
            } label: {
                SwiftUI.Label("Remove from bar", systemImage: "minus.circle")
            }
            .accessibilityIdentifier("shell-bar-hide")
        } else if !prefs.barIsFull(items) {
            Button {
                put(at: nil)
            } label: {
                SwiftUI.Label("Add to bar", systemImage: "plus.circle")
            }
            .accessibilityIdentifier("shell-bar-show")
        } else {
            Menu {
                ForEach(prefs.entries(items)) { (entry: ShellBarEntry) in
                    if !ShellBarPrefs.keep.contains(where: entry.contains) {
                        Button {
                            put(at: entry.id)
                        } label: {
                            SwiftUI.Label(ShellBarItemMenu.title(entry), systemImage: ShellBarItemMenu.symbol(entry))
                        }
                    }
                }
            } label: {
                SwiftUI.Label("Add to bar in place of", systemImage: "arrow.left.arrow.right.circle")
            }
            .accessibilityIdentifier("shell-bar-replace")
        }
    }

    private func put(at target: String?) {
        let placed: Bool = withAnimation(reduceMotion ? nil : .snappy) {
            prefs.putInBar(item.id, at: target, items: items)
        }
        if placed {
            FmHaptics.success()
        } else {
            FmHaptics.warning()
        }
    }

    static func title(_ entry: ShellBarEntry) -> String {
        switch entry.slot {
        case let .item(item): return item.title
        case let .folder(folder, _): return folder.name
        }
    }

    static func symbol(_ entry: ShellBarEntry) -> String {
        switch entry.slot {
        case let .item(item): return item.symbol
        case .folder: return "folder"
        }
    }

    private func change(_ edit: (inout ShellBarLayout) -> Void) {
        withAnimation(reduceMotion ? nil : .snappy) {
            prefs.arrange(edit)
        }
    }
}

struct ShellBarResetButton: View {
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let prefs: ShellBarPrefs

    var body: some View {
        Button(role: .destructive) {
            withAnimation(reduceMotion ? nil : .snappy) {
                prefs.arrange { $0 = ShellBarLayout() }
            }
        } label: {
            SwiftUI.Label("Reset bar layout", systemImage: "arrow.uturn.backward")
        }
        .disabled(prefs.layout.isEmpty)
        .accessibilityIdentifier("shell-bar-reset-layout")
    }
}

struct ShellBarFolderMenu: View {
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let folder: ShellBarFolder
    let items: [ShellBarItem]
    let prefs: ShellBarPrefs
    let onOpen: () -> Void
    let onName: (ShellBarNaming) -> Void
    var onSettings: (() -> Void)? = nil

    var body: some View {
        Section {
            Button {
                onOpen()
            } label: {
                SwiftUI.Label("Open \(folder.name)", systemImage: "folder")
            }
            ShellBarStepItems(slotId: folder.slotId, items: items, prefs: prefs)
        }
        Section {
            Button {
                onName(.rename(folder: folder.id, current: folder.name))
            } label: {
                SwiftUI.Label("Rename folder…", systemImage: "pencil")
            }
            .accessibilityIdentifier("shell-bar-rename-folder")
            Button {
                onName(.newFolder(item: nil))
            } label: {
                SwiftUI.Label("New folder…", systemImage: "folder.badge.plus")
            }
            Button {
                withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.removeFromBar(folder.items, items: items)
                }
            } label: {
                SwiftUI.Label("Remove folder from bar", systemImage: "minus.circle")
            }
            .accessibilityIdentifier("shell-bar-hide-folder")
        }
        Section {
            Button(role: .destructive) {
                withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.arrange { $0.removeFolder(folder.id) }
                }
            } label: {
                SwiftUI.Label("Remove folder (keeps its items)", systemImage: "folder.badge.minus")
            }
            .accessibilityIdentifier("shell-bar-remove-folder")
        }
        if let onSettings = onSettings {
            Section {
                Button {
                    onSettings()
                } label: {
                    SwiftUI.Label("Bottom bar settings…", systemImage: "slider.horizontal.3")
                }
                .accessibilityIdentifier("shell-bar-folder-settings")
            }
        }
    }
}

struct ShellBarFolderLabel: View {
    @Environment(\.fmPalette) var fm

    let folder: ShellBarFolder
    let members: [ShellBarItem]
    let iconSize: CGFloat
    var look: ShellBarStyle = ShellBarStyle()

    private var selected: Bool { members.contains { $0.selected } }

    private var badge: Int { members.reduce(0) { $0 + $1.badge } }

    private var active: Bool { members.contains(where: \.active) }

    private var live: Int { members.reduce(0) { $0 + $1.live } }

    private var labelSize: CGFloat { (10 * CGFloat(look.size.scale)).rounded() }

    var body: some View {
        VStack(spacing: 3) {
            mini
                .frame(width: iconSize + 6, height: iconSize + 4)
                .background(
                    RoundedRectangle(cornerRadius: 6)
                        .fill(selected ? fm.accent.opacity(0.18) : fm.textPrimary.opacity(0.08)))
                .background {
                    if active && look.showActivity {
                        ShellActivityHalo(size: iconSize + 14)
                            .transition(.scale.combined(with: .opacity))
                    }
                }
                .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { (frame: CGRect) in
                    if members.contains(where: { $0.flightTarget }) {
                        ShellLaunchFlight.shared.target = frame
                    }
                }
                .overlay(alignment: .topTrailing) {
                    if badge > 0 {
                        Text(verbatim: ShellBottomBar.badgeText(badge))
                            .fmText(.labelSmall, color: Color.white, tracking: 0, tabular: true)
                            .contentTransition(.numericText(value: Double(badge)))
                            .lineLimit(1)
                            .fixedSize()
                            .padding(.horizontal, 4)
                            .frame(minWidth: 16, minHeight: 16)
                            .background(Capsule().fill(fm.count))
                            .offset(x: 14, y: -6)
                            .accessibilityHidden(true)
                    }
                }
                .overlay(alignment: .topLeading) {
                    if live > 0 && look.showActivity {
                        ShellHeartbeatCount(count: live)
                            .offset(x: -14, y: -6)
                            .transition(.scale.combined(with: .opacity))
                    }
                }
            if look.labels.shows(selected: selected) {
                Text(folder.name)
                    .font(.system(size: labelSize, weight: selected ? .semibold : .medium))
                    .foregroundStyle(selected ? fm.accent : fm.textSecondary)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
        }
        .shellBarLift(!look.shape.backed, dark: fm.dark)
    }

    private var mini: some View {
        let shown: [ShellBarItem] = Array(members.prefix(4))
        let glyph: CGFloat = max(7, iconSize * 0.42)
        return Grid(horizontalSpacing: 2, verticalSpacing: 2) {
            GridRow {
                symbol(shown, 0, size: glyph)
                symbol(shown, 1, size: glyph)
            }
            GridRow {
                symbol(shown, 2, size: glyph)
                symbol(shown, 3, size: glyph)
            }
        }
    }

    @ViewBuilder private func symbol(_ shown: [ShellBarItem], _ index: Int, size: CGFloat) -> some View {
        if index < shown.count {
            Image(systemName: shown[index].symbol)
                .font(.system(size: size, weight: .medium))
                .foregroundStyle(shown[index].selected ? fm.accent : fm.textPrimary)
                .frame(width: size + 2, height: size + 2)
        } else {
            Color.clear.frame(width: size + 2, height: size + 2)
        }
    }
}

struct ShellBarFolderCell: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let folder: ShellBarFolder
    let members: [ShellBarItem]
    let items: [ShellBarItem]
    let prefs: ShellBarPrefs
    let iconSize: CGFloat
    let cellWidth: CGFloat
    let cellHeight: CGFloat
    let shape: AnyShape
    var look: ShellBarStyle = ShellBarStyle()
    let onName: (ShellBarNaming) -> Void
    let onPackage: (String) -> Void
    var onSettings: (() -> Void)? = nil

    @State private var open: Bool = false
    @State private var naming: ShellBarNaming?

    private var selected: Bool { members.contains { $0.selected } }

    var body: some View {
        Button {
            FmHaptics.selection()
            open.toggle()
        } label: {
            ShellBarFolderLabel(folder: folder, members: members, iconSize: iconSize, look: look)
                .frame(width: cellWidth, height: cellHeight)
                .shellBarSlotChrome(shape, style: look, selected: selected || open)
                .contentShape(shape)
        }
        .buttonStyle(ShellBarButtonStyle())
        .contextMenu {
            ShellBarFolderMenu(
                folder: folder, items: items, prefs: prefs, onOpen: { open = true }, onName: onName,
                onSettings: onSettings)
        }
        .popover(isPresented: $open, attachmentAnchor: .rect(.bounds), arrowEdge: .bottom) {
            popover
                .presentationCompactAdaptation(.popover)
                .shellBarNaming($naming, prefs: prefs)
        }
        .accessibilityLabel(Text("\(folder.name) folder"))
        .accessibilityValue(Text(members.map(\.title).joined(separator: ", ")))
        .accessibilityHint(Text("Opens the folder"))
        .accessibilityAddTraits(selected ? [.isSelected, .isButton] : [.isButton])
        .accessibilityShowsLargeContentViewer {
            SwiftUI.Label(folder.name, systemImage: "folder")
        }
        .accessibilityIdentifier("bottom-bar-folder-\(folder.id)")
    }

    private var popoverLook: ShellBarStyle {
        var next: ShellBarStyle = look
        next.labels = .always
        next.shape = .capsule
        return next
    }

    private var popoverHeight: CGFloat {
        look.labels == .never ? cellHeight / 0.8 : cellHeight
    }

    private var rows: [[ShellBarItem]] {
        let across: Int = 4
        return stride(from: 0, to: members.count, by: across).map { (start: Int) -> [ShellBarItem] in
            Array(members[start..<min(start + across, members.count)])
        }
    }

    private var popover: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(folder.name)
                .fmText(.labelSmall, color: fm.textTertiary)
                .padding(.leading, FmSpace.x1)
                .accessibilityAddTraits(.isHeader)
            Grid(horizontalSpacing: 2, verticalSpacing: 2) {
                ForEach(Array(rows.enumerated()), id: \.offset) { (row: (offset: Int, element: [ShellBarItem])) in
                    GridRow {
                        ForEach(row.element) { (item: ShellBarItem) in
                            tile(item)
                        }
                    }
                }
            }
        }
        .padding(FmSpace.x2)
        .fixedSize()
        .dynamicTypeSize(...DynamicTypeSize.accessibility2)
        .accessibilityIdentifier("shell-bar-folder-popover")
    }

    private func tile(_ item: ShellBarItem) -> some View {
        var shown: ShellBarItem = item
        shown.flightTarget = false
        return Button {
            FmHaptics.selection()
            open = false
            ShellBottomBar.run(item, prefs: prefs, reduceMotion: reduceMotion)
        } label: {
            ShellBarCellLabel(item: shown, iconSize: iconSize, showsCounts: look.showCounts, look: popoverLook)
                .frame(width: max(cellWidth, cellHeight), height: popoverHeight)
                .background {
                    if item.selected {
                        Capsule().fill(fm.accent.opacity(0.14))
                    }
                }
                .contentShape(Capsule())
        }
        .buttonStyle(ShellBarButtonStyle())
        .contextMenu {
            ShellBarItemMenu(
                item: item, items: items, prefs: prefs,
                onName: { (next: ShellBarNaming) in naming = next },
                onPackage: { (package: String) in
                    open = false
                    onPackage(package)
                })
        }
        .accessibilityLabel(Text(item.title))
        .accessibilityValue(Text(item.badge > 0 ? "\(ShellBottomBar.badgeText(item.badge)) new" : ""))
        .accessibilityAddTraits(item.selected ? [.isSelected, .isButton] : [.isButton])
        .accessibilityIdentifier("bottom-bar-\(item.id)")
    }
}

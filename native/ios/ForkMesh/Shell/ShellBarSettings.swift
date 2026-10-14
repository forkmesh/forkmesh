import FMApi
import FMBase
import SwiftUI

struct ShellBarSettingsSheet: View {
    @Environment(\.dismiss) private var dismiss

    let prefs: ShellBarPrefs

    var body: some View {
        NavigationStack {
            ShellBarSettingsForm(prefs: prefs)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                            .accessibilityIdentifier("shell-bar-settings-done")
                    }
                }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
    }
}

struct ShellBarSettingsForm: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let prefs: ShellBarPrefs

    @ScaledMetric(relativeTo: .body) private var iconSize: CGFloat = 20
    @ScaledMetric(relativeTo: .body) private var cellWidth: CGFloat = 64
    @ScaledMetric(relativeTo: .body) private var cellHeight: CGFloat = 52

    private var style: ShellBarStyle { prefs.barStyle }

    private func edit(haptic: Bool = true, _ change: @escaping (inout ShellBarStyle) -> Void) {
        if haptic {
            FmHaptics.selection()
        }
        withAnimation(reduceMotion || !haptic ? nil : .snappy) {
            prefs.styleBar(change)
        }
    }

    private func binding<Value>(_ path: WritableKeyPath<ShellBarStyle, Value>, haptic: Bool = true) -> Binding<Value> {
        Binding<Value>(
            get: { prefs.barStyle[keyPath: path] },
            set: { (next: Value) in edit(haptic: haptic) { $0[keyPath: path] = next } })
    }

    var body: some View {
        Form {
            previewSection
            shapeSection
            iconSection
            selectionSection
            contentSection
            orderSection
            resetSection
        }
        .navigationTitle("Bottom bar")
        .navigationBarTitleDisplayMode(.inline)
        .tint(fm.accent)
        .accessibilityIdentifier("shell-bar-settings")
    }

    private var previewSection: some View {
        Section {
            ShellBarPreview(style: style, iconSize: iconSize, cellWidth: cellWidth, cellHeight: cellHeight)
                .listRowInsets(EdgeInsets())
        } footer: {
            Text("Changes show here and on the bar at once. Touch and hold any icon in the bar to move it, put it in a folder or open these settings.")
        }
    }

    private var shapeSection: some View {
        Section {
            ShellBarChoices(
                choices: ShellBarShape.allCases, selection: binding(\.shape), title: \.title, symbol: \.symbol,
                identifier: "shell-bar-shape")
            ShellBarChoices(
                choices: ShellBarFill.allCases, selection: binding(\.fill), title: \.title, symbol: \.symbol,
                identifier: "shell-bar-fill")
                .disabled(!style.shape.backed)
            ShellBarSlider(
                title: "Corners", symbol: "square.on.circle", low: "Square", high: "Round",
                value: binding(\.corner, haptic: false), identifier: "shell-bar-corner")
                .disabled(!style.shape.cornered)
            ShellBarSlider(
                title: "Spacing", symbol: "arrow.left.and.right", low: "Tight", high: "Wide",
                value: binding(\.spacing, haptic: false), identifier: "shell-bar-spacing")
                .disabled(style.shape.separate && style.width == .full)
            Picker("Width", selection: binding(\.width)) {
                ForEach(ShellBarWidth.allCases, id: \.self) { (width: ShellBarWidth) in
                    Text(width.title).tag(width)
                }
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("shell-bar-width")
        } header: {
            Text("Shape")
        } footer: {
            Text(shapeFooter)
        }
    }

    private var shapeFooter: String {
        switch style.shape {
        case .capsule, .panel:
            return "One piece of glass holds every icon. Fit the icons shrinks it to its icons, centred."
        case .squares, .circles:
            return "Each icon sits on its own button. At full width they spread across the bar; fit them to set the spacing yourself."
        case .icons:
            return "Just the icons and labels, with a soft shadow so they read over any background."
        }
    }

    private var iconSection: some View {
        Section {
            Picker("Icon style", selection: binding(\.icons)) {
                ForEach(ShellHomeIconStyle.allCases, id: \.self) { (icons: ShellHomeIconStyle) in
                    Text(icons.title).tag(icons)
                }
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("shell-bar-icon-style")
            LabeledContent {
                Picker("Icon size", selection: binding(\.size)) {
                    ForEach(ShellBarSize.allCases, id: \.self) { (size: ShellBarSize) in
                        Text(size.title)
                            .accessibilityLabel(Text(size.name))
                            .tag(size)
                    }
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 200)
                .accessibilityIdentifier("shell-bar-size")
            } label: {
                SwiftUI.Label("Size", systemImage: "textformat.size")
            }
            LabeledContent {
                Picker("Labels", selection: binding(\.labels)) {
                    ForEach(ShellBarLabels.allCases, id: \.self) { (labels: ShellBarLabels) in
                        Text(labels.title).tag(labels)
                    }
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 220)
                .accessibilityIdentifier("shell-bar-labels")
            } label: {
                SwiftUI.Label("Labels", systemImage: "character.textbox")
            }
            Button {
                edit { $0.icons = prefs.home.icons }
            } label: {
                SwiftUI.Label("Match the Home icons (\(prefs.home.icons.title))", systemImage: "square.grid.3x3")
            }
            .disabled(style.icons == prefs.home.icons)
            .accessibilityIdentifier("shell-bar-match-home")
        } header: {
            Text("Icons")
        } footer: {
            Text("The same styles as the Home screen: Outline and Filled keep the icons plain, Accent draws them in your accent colour and Tiles sets each one on a rounded tile.")
        }
    }

    private var selectionSection: some View {
        Section {
            ShellBarChoices(
                choices: ShellBarSelection.allCases, selection: binding(\.selection), title: \.title,
                symbol: \.symbol, identifier: "shell-bar-selection")
        } header: {
            Text("Selected icon")
        } footer: {
            Text("The open page's icon is always drawn in the accent colour; this adds a mark around or under it.")
        }
    }

    private var contentSection: some View {
        Section {
            Stepper(value: binding(\.limit), in: limitRange) {
                HStack {
                    SwiftUI.Label("Icons in the bar", systemImage: "square.grid.3x1.below.line.grid.1x2")
                    Spacer(minLength: FmSpace.x2)
                    Text(verbatim: "\(style.limit)")
                        .font(.system(.body, design: .rounded).monospacedDigit())
                        .foregroundStyle(fm.textSecondary)
                }
            }
            .accessibilityIdentifier("shell-bar-limit")
            Toggle(isOn: binding(\.showCounts)) {
                SwiftUI.Label("Show counts", systemImage: "number.circle")
            }
            .accessibilityIdentifier("shell-bar-counts")
            Toggle(isOn: binding(\.showActivity)) {
                SwiftUI.Label("Show activity", systemImage: "waveform.path.ecg")
            }
            .accessibilityIdentifier("shell-bar-activity")
            Toggle(isOn: binding(\.highlightFrequent)) {
                SwiftUI.Label("Highlight frequently used", systemImage: "sparkle")
            }
            .accessibilityIdentifier("shell-bar-highlight")
        } header: {
            Text("What the bar shows")
        } footer: {
            Text("A folder counts as one icon. More icons than fit scroll sideways. Unread badges always show; activity is the ring and heartbeat count on anything running.")
        }
    }

    private var limitRange: ClosedRange<Int> {
        (ShellBarStyle.limitChoices.first ?? 3)...(ShellBarStyle.limitChoices.last ?? 8)
    }

    private var orderSection: some View {
        Section {
            ShellBarSettingsItems(prefs: prefs, includesHome: false)
        } header: {
            Text("Order")
        }
    }

    private var resetSection: some View {
        Section {
            Button(role: .destructive) {
                edit { $0 = ShellBarStyle() }
            } label: {
                SwiftUI.Label("Reset bar look", systemImage: "arrow.uturn.backward")
            }
            .disabled(style.isDefault)
            .accessibilityIdentifier("shell-bar-reset-style")
        }
    }
}

struct ShellBarChoices<Choice: Hashable>: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.isEnabled) var enabled

    let choices: [Choice]
    @Binding var selection: Choice
    let title: KeyPath<Choice, String>
    let symbol: KeyPath<Choice, String>
    let identifier: String

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            ForEach(choices, id: \.self) { (choice: Choice) in
                chip(choice)
            }
        }
        .padding(.vertical, FmSpace.x1)
        .opacity(enabled ? 1 : 0.45)
    }

    private func chip(_ choice: Choice) -> some View {
        let on: Bool = choice == selection
        let shape: RoundedRectangle = RoundedRectangle(cornerRadius: 10, style: .continuous)
        return Button {
            selection = choice
        } label: {
            VStack(spacing: 4) {
                Image(systemName: choice[keyPath: symbol])
                    .font(.system(size: 16, weight: on ? .semibold : .medium))
                    .frame(height: 22)
                Text(choice[keyPath: title])
                    .font(.system(size: 11, weight: on ? .semibold : .medium))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
            }
            .foregroundStyle(on ? fm.accent : fm.textSecondary)
            .frame(maxWidth: .infinity)
            .padding(.vertical, FmSpace.x2)
            .background(shape.fill(on ? fm.accent.opacity(0.14) : fm.textPrimary.opacity(0.05)))
            .overlay(shape.strokeBorder(on ? fm.accent : Color.clear, lineWidth: 1.25))
            .contentShape(shape)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(choice[keyPath: title]))
        .accessibilityAddTraits(on ? [.isSelected, .isButton] : [.isButton])
        .accessibilityIdentifier(
            "\(identifier)-\(choice[keyPath: title].lowercased().replacingOccurrences(of: " ", with: "-"))")
    }
}

struct ShellBarSlider: View {
    @Environment(\.fmPalette) var fm

    let title: String
    let symbol: String
    let low: String
    let high: String
    @Binding var value: Double
    let identifier: String

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            SwiftUI.Label(title, systemImage: symbol)
            HStack(spacing: FmSpace.x2) {
                Text(low)
                    .font(.caption)
                    .foregroundStyle(fm.textTertiary)
                Slider(
                    value: $value, in: 0...1,
                    onEditingChanged: { (editing: Bool) in
                        if !editing {
                            FmHaptics.selection()
                        }
                    })
                .accessibilityLabel(Text(title))
                .accessibilityValue(Text("\(Int((value * 100).rounded())) percent"))
                .accessibilityIdentifier(identifier)
                Text(high)
                    .font(.caption)
                    .foregroundStyle(fm.textTertiary)
            }
        }
        .padding(.vertical, 2)
    }
}

struct BottomBarSettingsCard: View {
    @Environment(\.fmPalette) var fm

    var body: some View {
        NavigationLink {
            ShellBarSettingsForm(prefs: ShellBarPrefs.shared)
                .shellPageClearance()
        } label: {
            SettingsLinkRow(
                icon: "view_agenda_outlined", title: "Bottom bar",
                subtitle: "Shape, glass, icon style and size, labels and the selected mark")
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
        .accessibilityIdentifier("settings-bottom-bar")
    }
}

import FMAccount
import SwiftUI
import UIKit

extension Color {
    init(fmRGB rgb: UInt32) {
        self.init(fmARGB: 0xFF000000 | (rgb & 0xFFFFFF))
    }

    var fmRGB: UInt32 {
        var red: CGFloat = 0
        var green: CGFloat = 0
        var blue: CGFloat = 0
        var alpha: CGFloat = 0
        UIColor(self).getRed(&red, green: &green, blue: &blue, alpha: &alpha)
        let channel: (CGFloat) -> UInt32 = { (value: CGFloat) -> UInt32 in UInt32((min(max(value, 0), 1) * 255).rounded()) }
        return (channel(red) << 16) | (channel(green) << 8) | channel(blue)
    }
}

@MainActor
final class ThemeWriter {
    private var pending: Task<Void, Never>?

    func write(_ theme: AppTheme, to settings: SettingsService, now: Bool = false) {
        pending?.cancel()
        if now {
            settings.setTheme(theme)
            return
        }
        pending = Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(180))
            if Task.isCancelled {
                return
            }
            settings.setTheme(theme)
        }
    }
}

struct ThemeSwatch<Fill: ShapeStyle>: View {
    @Environment(\.fmPalette) var fm
    let label: String
    let fill: Fill
    let ring: Color
    let check: Color
    let selected: Bool

    var body: some View {
        VStack(spacing: FmSpace.x1) {
            Circle()
                .fill(fill)
                .overlay {
                    if selected {
                        FmIcon("check_rounded", size: 20, color: check)
                    }
                }
                .padding(3)
                .frame(width: 44, height: 44)
                .overlay {
                    Circle().strokeBorder(selected ? ring : Color.clear, lineWidth: 2)
                }
            Text(label)
                .fmText(
                    .bodySmall,
                    color: selected ? fm.textPrimary : fm.textSecondary,
                    weight: selected ? FmType.emphasis : FmType.regular)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
        }
        .frame(maxWidth: .infinity)
        .contentShape(Rectangle())
    }
}

struct ThemeAccentPicker: View {
    @Environment(\.fmPalette) var fm
    let theme: AppTheme
    let dark: Bool
    let onChange: (AppTheme, Bool) -> Void

    private var rainbow: AngularGradient {
        AngularGradient(
            gradient: Gradient(colors: [.red, .orange, .yellow, .green, .cyan, .blue, .purple, .pink, .red]), center: .center)
    }

    private var customSelected: Bool {
        theme.customAccent != nil && theme.edits(dark: dark)[.accent] == nil
    }

    private func presetSelected(_ accent: AccentChoice) -> Bool {
        theme.customAccent == nil && theme.accent == accent && theme.edits(dark: dark)[.accent] == nil
    }

    private func resolved(_ accent: AccentChoice) -> ResolvedTheme {
        AppTheme(surface: theme.surface, accent: accent, readable: theme.readable).resolve(dark: dark)
    }

    private var customBinding: Binding<Color> {
        Binding<Color>(
            get: { Color(fmRGB: theme.customAccent ?? theme.resolve(dark: dark).rgb(.accent)) },
            set: { (color: Color) in
                var next: AppTheme = theme
                next.choose(customAccent: color.fmRGB)
                onChange(next, false)
            })
    }

    var body: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 64), spacing: FmSpace.x2)], spacing: FmSpace.x3) {
            ForEach(AccentChoice.allCases, id: \.self) { (accent: AccentChoice) in
                let colors: ResolvedTheme = resolved(accent)
                let selected: Bool = presetSelected(accent)
                ThemeSwatch(
                    label: accent.label, fill: Color(fmRGB: colors.rgb(.accent)), ring: Color(fmRGB: colors.rgb(.accent)),
                    check: Color(fmRGB: colors.rgb(.onAccent)), selected: selected
                )
                .onTapGesture {
                    if selected {
                        return
                    }
                    FmHaptics.selection()
                    var next: AppTheme = theme
                    next.choose(accent: accent)
                    onChange(next, true)
                }
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text(verbatim: "\(accent.label) accent"))
                .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
                .accessibilityIdentifier("appearance-accent-\(accent.rawValue)")
            }
            customSwatch
        }
    }

    @ViewBuilder
    private var customSwatch: some View {
        let colors: ResolvedTheme = theme.resolve(dark: dark)
        let ring: Color = customSelected ? Color(fmRGB: colors.rgb(.accent)) : fm.textSecondary
        Group {
            if customSelected {
                ThemeSwatch(
                    label: "Custom", fill: Color(fmRGB: colors.rgb(.accent)), ring: ring,
                    check: Color(fmRGB: colors.rgb(.onAccent)), selected: true)
            } else {
                ThemeSwatch(label: "Custom", fill: rainbow, ring: ring, check: Color.white, selected: false)
            }
        }
        .accessibilityHidden(true)
        .overlay(alignment: .top) {
            ColorPicker("Custom accent colour", selection: customBinding, supportsOpacity: false)
                .labelsHidden()
                .scaleEffect(1.6)
                .frame(width: 44, height: 44)
                .opacity(0.02)
                .accessibilityIdentifier("appearance-accent-custom")
        }
    }
}

struct ThemeSurfacePicker: View {
    @Environment(\.fmPalette) var fm
    let theme: AppTheme
    let onChange: (AppTheme, Bool) -> Void

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(ThemeSurface.allCases, id: \.self) { (surface: ThemeSurface) in
                    chip(surface)
                }
            }
            .padding(.vertical, 2)
        }
    }

    private func chip(_ surface: ThemeSurface) -> some View {
        let selected: Bool = theme.surface == surface
        let darkBase: UInt32 = surface.colors(dark: true)[.bgBase] ?? 0
        let lightBase: UInt32 = surface.colors(dark: false)[.bgOverlay] ?? 0
        return HStack(spacing: FmSpace.x2) {
            HStack(spacing: 0) {
                Rectangle().fill(Color(fmRGB: darkBase))
                Rectangle().fill(Color(fmRGB: lightBase))
            }
            .frame(width: 18, height: 18)
            .clipShape(Circle())
            .overlay { Circle().strokeBorder(fm.border, lineWidth: 1) }
            Text(surface.label)
                .fmText(.labelMedium, color: selected ? fm.textPrimary : fm.textSecondary, weight: selected ? FmType.emphasis : FmType.medium)
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(minHeight: 38)
        .background(selected ? fm.accentSubtle : fm.bgOverlay)
        .clipShape(Capsule())
        .overlay { Capsule().strokeBorder(selected ? fm.accent : Color.clear, lineWidth: 1.5) }
        .contentShape(Capsule())
        .onTapGesture {
            if selected {
                return
            }
            FmHaptics.selection()
            var next: AppTheme = theme
            next.choose(surface: surface)
            onChange(next, true)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: "\(surface.label) surfaces"))
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
        .accessibilityIdentifier("appearance-surface-\(surface.rawValue)")
    }
}

struct ThemePreview: View {
    let theme: AppTheme
    let dark: Bool

    var body: some View {
        let p: FmPalette = FmPalette(dark: dark, theme: theme)
        VStack(spacing: 0) {
            HStack {
                Text("Repositories").fmText(.titleSmall, color: p.textPrimary)
                Spacer()
                Text("Edit").fmText(.labelLarge, color: p.accent)
            }
            .padding(.horizontal, FmSpace.x4)
            .frame(height: 44)
            .background(p.bgRaised)
            Rectangle().fill(p.border).frame(height: 1)
            VStack(spacing: FmSpace.x2) {
                row(p, icon: "folder_outlined", title: "forkmesh", subtitle: "Updated 2 minutes ago", meta: "main")
                row(p, icon: "merge_type", title: "Theme picker", subtitle: "Pull request #42 by you", meta: "3 files")
                HStack(spacing: FmSpace.x2) {
                    chip(p, "Merged", p.success)
                    chip(p, "Failed", p.danger)
                    chip(p, "Pending", p.warning)
                    Spacer(minLength: 0)
                }
                HStack(spacing: FmSpace.x3) {
                    Text("Read the release notes").underline().fmText(.bodySmall, color: p.accent)
                    Spacer(minLength: 0)
                    Capsule()
                        .fill(p.accent)
                        .frame(width: 44, height: 26)
                        .overlay(alignment: .trailing) {
                            Circle().fill(Color.white).padding(3)
                        }
                }
                HStack {
                    Text("Open pull request").fmText(.labelLarge, color: p.onPrimary, weight: FmType.emphasis)
                }
                .frame(maxWidth: .infinity, minHeight: 40)
                .background(p.accent)
                .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                Text("Hints and captions look like this.").fmText(.bodySmall, color: p.textTertiary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(FmSpace.x3)
        }
        .background(p.bgBase)
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous).strokeBorder(p.border, lineWidth: 1)
        }
        .environment(\.colorScheme, dark ? .dark : .light)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: "Preview of the \(dark ? "dark" : "light") theme"))
        .accessibilityIdentifier("theme-preview")
    }

    private func row(_ p: FmPalette, icon: String, title: String, subtitle: String, meta: String) -> some View {
        HStack(spacing: FmSpace.x3) {
            FmIcon(icon, size: 18, color: p.accent)
                .frame(width: 34, height: 34)
                .background(p.accentSubtle)
                .clipShape(RoundedRectangle(cornerRadius: FmRadius.sm, style: .continuous))
            VStack(alignment: .leading, spacing: 2) {
                Text(title).fmText(.titleSmall, color: p.textPrimary)
                Text(subtitle).fmText(.bodySmall, color: p.textSecondary)
            }
            Spacer(minLength: 0)
            Text(meta).fmText(.labelSmall, color: p.textTertiary)
        }
        .padding(FmSpace.x2)
        .background(p.bgRaised)
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous).strokeBorder(p.border, lineWidth: 1)
        }
    }

    private func chip(_ p: FmPalette, _ label: String, _ tone: Color) -> some View {
        Text(label)
            .fmText(.labelSmall, color: tone, weight: FmType.emphasis)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, 3)
            .background(tone.opacity(dark ? 0.20 : 0.13))
            .clipShape(Capsule())
    }
}

private struct ThemeTokenRow: View {
    @Environment(\.fmPalette) var fm
    let token: ThemeToken
    let shown: UInt32
    let edited: Bool
    let onSet: (UInt32?) -> Void
    @State private var text: String = ""
    @FocusState private var focused: Bool

    private var binding: Binding<Color> {
        Binding<Color>(get: { Color(fmRGB: shown) }, set: { (color: Color) in onSet(color.fmRGB) })
    }

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            Text(token.label)
                .fmText(.bodyMedium, weight: edited ? FmType.emphasis : FmType.regular)
                .lineLimit(1)
                .minimumScaleFactor(0.85)
            Spacer(minLength: FmSpace.x1)
            if edited {
                Button {
                    FmHaptics.selection()
                    onSet(nil)
                } label: {
                    FmIcon("undo", size: 18, color: fm.textSecondary).frame(width: 32, height: 32)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text(verbatim: "Reset \(token.label)"))
            }
            TextField("#000000", text: $text)
                .fmMono(size: FmType.small, color: fm.textSecondary)
                .multilineTextAlignment(.trailing)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .submitLabel(.done)
                .focused($focused)
                .frame(width: 84)
                .onSubmit { commit() }
                .onChange(of: focused) { (_: Bool, now: Bool) in
                    if !now { commit() }
                }
                .accessibilityLabel(Text(verbatim: "\(token.label) hex value"))
            ColorPicker(token.label, selection: binding, supportsOpacity: false)
                .labelsHidden()
        }
        .frame(minHeight: 44)
        .onAppear { text = ThemeColor.hex(shown) }
        .onChange(of: shown) { (_: UInt32, value: UInt32) in
            if !focused { text = ThemeColor.hex(value) }
        }
        .accessibilityIdentifier("theme-token-\(token.rawValue)")
    }

    private func commit() {
        if let rgb = ThemeColor.parseHex(text) {
            if rgb != shown { onSet(rgb) }
        } else {
            text = ThemeColor.hex(shown)
        }
    }
}

struct ThemeEditorScreen: View {
    @Environment(\.fmPalette) var fm
    let settings: SettingsService
    @State private var draft: AppTheme = .standard
    @State private var previewDark: Bool = false
    @State private var loaded: Bool = false
    @State private var writer: ThemeWriter = ThemeWriter()

    private func apply(_ next: AppTheme, now: Bool) {
        draft = next
        writer.write(next, to: settings, now: now)
    }

    private func shown(_ token: ThemeToken, in resolved: ResolvedTheme) -> UInt32 {
        let argb: UInt32 = resolved[token]
        let alpha: UInt32 = argb >> 24
        if alpha == 0xFF {
            return argb & 0xFFFFFF
        }
        return ThemeColor.mix(resolved.rgb(.bgBase), argb & 0xFFFFFF, amount: Double(alpha) / 255)
    }

    var body: some View {
        Form {
            previewSection
            Section {
                ThemeAccentPicker(theme: draft, dark: previewDark, onChange: apply)
                    .padding(.vertical, FmSpace.x2)
            } header: {
                Text("Accent")
            }
            Section {
                ThemeSurfacePicker(theme: draft, onChange: apply)
            } header: {
                Text("Surfaces")
            }
            optionsSection
            colourSections
            Section {
                Button(role: .destructive) {
                    FmHaptics.selection()
                    apply(.standard, now: true)
                } label: {
                    Text("Reset theme")
                }
                .disabled(draft.isStandard)
                .accessibilityIdentifier("theme-reset")
            } footer: {
                Text("Puts back the ForkMesh surfaces and the blue accent. Light or dark and the background image stay as they are.")
            }
        }
        .navigationTitle("Theme")
        .navigationBarTitleDisplayMode(.inline)
        .tint(fm.accent)
        .onAppear {
            if !loaded {
                draft = settings.theme
                previewDark = fm.dark
                loaded = true
            }
        }
        .accessibilityIdentifier("theme-editor")
    }

    private var previewSection: some View {
        Section {
            ThemePreview(theme: draft, dark: previewDark)
                .padding(.vertical, FmSpace.x2)
            Picker("Preview", selection: $previewDark) {
                Text("Light").tag(false)
                Text("Dark").tag(true)
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("theme-preview-mode")
        } footer: {
            Text("The preview and the colours below are the \(previewDark ? "dark" : "light") mode's. Every change applies to the app at once.")
        }
    }

    private var optionsSection: some View {
        Section {
            Toggle(
                "Keep colours readable",
                isOn: Binding<Bool>(
                    get: { draft.readable },
                    set: { (on: Bool) in
                        var next: AppTheme = draft
                        next.readable = on
                        apply(next, now: true)
                    })
            )
            .accessibilityIdentifier("theme-readable")
            Button {
                FmHaptics.selection()
                var generator: SystemRandomNumberGenerator = SystemRandomNumberGenerator()
                apply(AppTheme.shuffled(using: &generator), now: true)
            } label: {
                Label("Shuffle", systemImage: "shuffle")
            }
            .accessibilityIdentifier("theme-shuffle")
        } footer: {
            Text("Readable colours keep the accent, text and status colours at 4.5:1 contrast or more against the backgrounds, darkening or lightening a colour you pick when it would be hard to read. Shuffle picks a random accent and surface family.")
        }
    }

    @ViewBuilder
    private var colourSections: some View {
        let resolved: ResolvedTheme = draft.resolve(dark: previewDark)
        let edits: [ThemeToken: UInt32] = draft.edits(dark: previewDark)
        ForEach(ThemeTokenGroup.allCases, id: \.self) { (group: ThemeTokenGroup) in
            Section {
                ForEach(group.tokens, id: \.self) { (token: ThemeToken) in
                    ThemeTokenRow(
                        token: token, shown: shown(token, in: resolved), edited: edits[token] != nil,
                        onSet: { (rgb: UInt32?) in
                            var next: AppTheme = draft
                            next.setColor(rgb, for: token, dark: previewDark)
                            apply(next, now: rgb == nil)
                        })
                }
            } header: {
                Text(group == .surfaces ? "Every colour, \(previewDark ? "dark" : "light"): \(group.label)" : group.label)
            }
        }
    }
}

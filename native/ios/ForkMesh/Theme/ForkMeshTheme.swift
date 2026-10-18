// FmTheme (as an environment value) and the parts of the light and dark
// themes that SwiftUI can carry from the root: the palette, the accent, the tint and the default text
// style. The component themes are views in Kit/FmMaterial.swift and
// Kit/FmOverlays.swift.

import FMAccount
import SwiftUI

private struct FmPaletteKey: EnvironmentKey {
    static let defaultValue: FmPalette = FmPalette.light
}

extension EnvironmentValues {
    /// FmTheme for the current brightness and accent. ForkMeshTheme installs
    /// it; without one the light palette with the blue accent is what views get.
    var fmPalette: FmPalette {
        get { return self[FmPaletteKey.self] }
        set { self[FmPaletteKey.self] = newValue }
    }
}

/// The app theme: wrap the root view (and the content of anything presented
/// outside it) in this. It resolves FmPalette from the system colour scheme
/// and the user's accent, and sets what applies globally: the tint (progress,
/// cursor, selection), the default text colour and the default text style
/// (bodyMedium).
///
/// `dark` forces a brightness, for the Settings preference and the kit gallery.
struct ForkMeshTheme<Content: View>: View {
    @Environment(\.colorScheme) private var systemScheme

    private let theme: AppTheme
    private let wallpaper: Bool
    private let forcedDark: Bool?
    private let content: Content

    init(
        theme: AppTheme = .standard, wallpaper: Bool = false, dark: Bool? = nil,
        @ViewBuilder content: () -> Content
    ) {
        self.theme = theme
        self.wallpaper = wallpaper
        self.forcedDark = dark
        self.content = content()
    }

    private var isDark: Bool {
        if let forced = forcedDark {
            return forced
        }
        return systemScheme == .dark
    }

    private var scheme: ColorScheme {
        return isDark ? ColorScheme.dark : ColorScheme.light
    }

    var body: some View {
        let palette: FmPalette = FmPalette(dark: isDark, theme: theme, wallpaper: wallpaper)
        content
            .fmText(.bodyMedium)
            .tint(palette.accent)
            .environment(\.fmPalette, palette)
            .environment(\.colorScheme, scheme)
    }
}

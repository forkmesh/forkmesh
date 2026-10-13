// Registers the bundled icon fonts (MaterialIcons, MingCute) with Core Text
// at launch, so no Info.plist UIAppFonts entry is needed.

import CoreText
import Foundation

enum FontRegistration {
    /// The bundled font files: resource name and extension.
    private static let files: [(name: String, ext: String)] = [
        (name: "material_icons_regular", ext: "otf"),
        (name: "mingcute_notification", ext: "ttf"),
    ]

    /// What went wrong, one line per file, for the kit gallery to show. Empty
    /// when both fonts registered.
    private(set) static var failures: [String] = []

    // A lazy static runs once and is thread-safe, which is all the guard
    // registration needs.
    private static let registered: Bool = {
        for file in files {
            register(name: file.name, ext: file.ext)
        }
        return true
    }()

    /// Call from the app's init. FmIcon also calls it, so a preview or a test
    /// host that skips the app entry point still draws glyphs.
    static func registerBundledFonts() {
        _ = registered
    }

    private static func locate(name: String, ext: String) -> URL? {
        // Synchronized folders copy resources to the bundle root; a folder
        // reference would keep the directory.
        if let url = Bundle.main.url(forResource: name, withExtension: ext) {
            return url
        }
        if let url = Bundle.main.url(forResource: name, withExtension: ext, subdirectory: "Fonts") {
            return url
        }
        return Bundle.main.url(forResource: name, withExtension: ext, subdirectory: "Resources/Fonts")
    }

    private static func register(name: String, ext: String) {
        guard let url = locate(name: name, ext: ext) else {
            failures.append("\(name).\(ext): not in the app bundle")
            return
        }
        var error: Unmanaged<CFError>?
        let ok: Bool = CTFontManagerRegisterFontsForURL(url as CFURL, CTFontManagerScope.process, &error)
        if ok {
            return
        }
        if let failure = error?.takeRetainedValue() {
            failures.append("\(name).\(ext): \(CFErrorCopyDescription(failure) as String)")
        } else {
            failures.append("\(name).\(ext): registration failed")
        }
    }
}

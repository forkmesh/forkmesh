import FMBase
import FMNet
import Foundation

extension AppTheme {
    public static func instanceColors(_ tokens: [String: UInt32]) -> [ThemeToken: UInt32] {
        var colors: [ThemeToken: UInt32] = [:]
        colors[.bgBase] = tokens["background"]
        colors[.bgRaised] = tokens["card"]
        colors[.bgOverlay] = tokens["popover"] ?? tokens["muted"]
        colors[.bgElevated] = tokens["muted"] ?? tokens["secondary"]
        colors[.border] = tokens["border"]
        colors[.textPrimary] = tokens["foreground"]
        if let muted = tokens["muted-foreground"] {
            colors[.textSecondary] = tokens["foreground"].map { ThemeColor.mix(muted, $0, amount: 0.35) } ?? muted
        } else {
            colors[.textSecondary] = tokens["secondary-foreground"]
        }
        colors[.textTertiary] = tokens["muted-foreground"]
        colors[.accent] = tokens["primary"]
        colors[.onAccent] = tokens["primary-foreground"]
        colors[.success] = tokens["success"]
        colors[.danger] = tokens["destructive"]
        colors[.warning] = tokens["attention"]
        return colors
    }

    public static func instance(themeColors: JSONObject, readable: Bool = true) -> AppTheme? {
        let decode: (String) -> [ThemeToken: UInt32] = { (mode: String) -> [ThemeToken: UInt32] in
            var tokens: [String: UInt32] = [:]
            for (name, value) in themeColors[mode]?.objectValue ?? [:] {
                if let text = value.stringValue, let rgb = ThemeColor.parseHex(text) {
                    tokens[name] = rgb
                }
            }
            return instanceColors(tokens)
        }
        let light: [ThemeToken: UInt32] = decode("light")
        let dark: [ThemeToken: UInt32] = decode("dark")
        guard !light.isEmpty || !dark.isEmpty else { return nil }
        return AppTheme(readable: readable, lightColors: light, darkColors: dark)
    }
}

public enum InstanceTheme {
    public static func themeColors(in body: JSONObject) -> JSONObject {
        if let resolved = body["themeColors"]?.objectValue {
            return resolved
        }
        guard let stored = body["theme"]?.objectValue else { return [:] }
        var modes: JSONObject = [:]
        for mode in ["light", "dark"] {
            modes[mode] = stored[mode]
        }
        return modes
    }

    public static func colorsJSON(in body: JSONObject) -> String {
        AppTheme.instance(themeColors: themeColors(in: body))?.colorsJSON() ?? ""
    }

    public static func fetch(http: HTTPClient, origin: String, timeout: TimeInterval = 8) async -> String? {
        guard !origin.isEmpty, let url = URL(string: "\(origin)/api/instance") else { return nil }
        let request = HTTPRequest(method: "GET", url: url, headers: ["Accept": "application/json"], timeout: timeout)
        guard let response = try? await http.send(request), response.status == 200 else { return nil }
        let body: JSONObject = response.jsonObject
        guard body["ok"]?.boolValue == true || body["theme"] != nil || body["themeColors"] != nil else { return nil }
        return colorsJSON(in: body)
    }
}

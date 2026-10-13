import FMApi
import Foundation

public enum SurfaceRouteCodec {
    public static let maxTokenLength = 2_048

    public static func encode(_ route: Route) -> String? {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        guard let data = try? encoder.encode(route) else { return nil }
        let token = data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
        return token.count <= maxTokenLength ? token : nil
    }

    public static func decode(_ token: String) -> Route? {
        guard !token.isEmpty, token.count <= maxTokenLength else { return nil }
        var base = token.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
        let pad = (4 - base.count % 4) % 4
        base += String(repeating: "=", count: pad)
        guard let data = Data(base64Encoded: base) else { return nil }
        return try? JSONDecoder().decode(Route.self, from: data)
    }
}

public enum SurfaceLink {
    public static let scheme = "forkmesh"
    public static let host = "surface"
    public static let routeItem = "route"

    public static func url(for route: Route) -> URL? {
        guard let token = SurfaceRouteCodec.encode(route) else { return nil }
        var components = URLComponents()
        components.scheme = scheme
        components.host = host
        components.path = "/open"
        components.queryItems = [URLQueryItem(name: routeItem, value: token)]
        return components.url
    }

    public static func isSurfaceLink(_ url: URL) -> Bool {
        url.scheme?.lowercased() == scheme && url.host?.lowercased() == host
    }

    public static func route(from url: URL) -> Route? {
        guard isSurfaceLink(url),
              let components = URLComponents(url: url, resolvingAgainstBaseURL: false),
              let token = components.queryItems?.first(where: { $0.name == routeItem })?.value
        else { return nil }
        return SurfaceRouteCodec.decode(token)
    }
}

import FMAccount
import FMBase
import Foundation
import ImageIO
import Observation
import UIKit
import UniformTypeIdentifiers

enum ProjectLogoStore {
    static let maxPixels: Int = 256

    static var directory: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("project-logos", isDirectory: true)
    }

    static func url(_ name: String) -> URL? {
        guard !name.isEmpty, !name.contains("/"), let directory else { return nil }
        return directory.appendingPathComponent(name)
    }

    static func save(_ data: Data, projectKey: String) async -> String? {
        await Task.detached(priority: .userInitiated) { saveNow(data, projectKey: projectKey) }.value
    }

    static func saveNow(_ data: Data, projectKey: String) -> String? {
        guard let png = shrink(data), let directory else { return nil }
        let slug = ProjectScope.slugFor(projectKey.isEmpty ? "project" : projectKey)
        let name = "\(slug.prefix(48))-\(Date().epochMillis).png"
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try png.write(to: directory.appendingPathComponent(name), options: .atomic)
        } catch {
            Log.warning("projects", "could not save the project logo: \(error)")
            return nil
        }
        return name
    }

    static func delete(_ name: String) {
        guard let url = url(name) else { return }
        _ = unlink(url.path)
    }

    static func shrink(_ data: Data) -> Data? {
        let sourceOptions: CFDictionary = [kCGImageSourceShouldCache: false] as CFDictionary
        guard let source = CGImageSourceCreateWithData(data as CFData, sourceOptions),
            CGImageSourceGetCount(source) > 0
        else { return nil }
        let options: CFDictionary = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixels,
        ] as CFDictionary
        guard let image = CGImageSourceCreateThumbnailAtIndex(source, 0, options) else { return nil }
        return UIImage(cgImage: image).pngData()
    }
}

@MainActor
@Observable
final class ProjectImages {
    static let shared: ProjectImages = ProjectImages()

    private(set) var logos: [String: UIImage] = [:]
    private(set) var favicons: [String: UIImage] = [:]
    @ObservationIgnored private var missing: Set<String> = []
    @ObservationIgnored private var fetching: Set<String> = []

    func image(for project: RelayProfile) -> UIImage? {
        if !project.logo.isEmpty, let logo = logos[project.logo] { return logo }
        if !project.logo.isEmpty || !project.hasInstance || AppEnv.isProdAlias(project.id) { return nil }
        return favicons[faviconKey(project)]
    }

    func load(_ project: RelayProfile) async {
        if !project.logo.isEmpty {
            await loadLogo(project.logo)
            return
        }
        if project.hasInstance && !AppEnv.isProdAlias(project.id) {
            await loadFavicon(project)
        }
    }

    func setLogo(_ name: String, image: UIImage) {
        logos[name] = image
    }

    private func loadLogo(_ name: String) async {
        if logos[name] != nil || missing.contains("logo:" + name) { return }
        guard let url = ProjectLogoStore.url(name) else { return }
        let image: UIImage? = await Task.detached(priority: .utility) {
            (try? Data(contentsOf: url)).flatMap { UIImage(data: $0) }
        }.value
        if let image {
            logos[name] = image
        } else {
            missing.insert("logo:" + name)
        }
    }

    private func faviconKey(_ project: RelayProfile) -> String {
        project.host.isEmpty ? project.id : project.host
    }

    private static var cacheDirectory: URL? {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first?
            .appendingPathComponent("project-favicons", isDirectory: true)
    }

    private func loadFavicon(_ project: RelayProfile) async {
        let key: String = faviconKey(project)
        if key.isEmpty || favicons[key] != nil || missing.contains(key) || fetching.contains(key) { return }
        fetching.insert(key)
        defer { fetching.remove(key) }
        let file: URL? = ProjectImages.cacheDirectory?.appendingPathComponent(ProjectScope.slugFor(key) + ".png")
        if let file, let data = try? Data(contentsOf: file), let image = UIImage(data: data) {
            favicons[key] = image
            return
        }
        let origin: String = project.httpOrigin
        for path in ["/favicon.ico", "/favicon/favicon.ico"] {
            guard let url = URL(string: origin + path) else { continue }
            if let image = await ProjectImages.fetch(url) {
                favicons[key] = image
                if let file, let png = image.pngData() {
                    try? FileManager.default.createDirectory(
                        at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try? png.write(to: file, options: .atomic)
                }
                return
            }
        }
        missing.insert(key)
    }

    func adoptInstanceLogo(_ projectId: String, logoUrl: String, branded: Bool = false) async {
        guard let relays = Services.get(SettingsService.self)?.relays,
            let project = relays.byId(projectId), project.hasInstance, project.logo.isEmpty
        else { return }
        let favicons: [String] = branded ? [] : ["/favicon.ico", "/favicon/favicon.ico"].map { project.httpOrigin + $0 }
        let candidates: [String] = [logoUrl] + favicons
        for url in candidates.filter({ !$0.isEmpty }).compactMap({ URL(string: $0) }) {
            guard let data = await ProjectImages.fetchData(url),
                let name = await ProjectLogoStore.save(data, projectKey: project.id)
            else { continue }
            if relays.byId(projectId)?.logo.isEmpty == true {
                relays.setLogo(projectId, name)
            } else {
                ProjectLogoStore.delete(name)
            }
            return
        }
    }

    private nonisolated static func fetch(_ url: URL) async -> UIImage? {
        await fetchData(url).flatMap { UIImage(data: $0) }
    }

    private nonisolated static func fetchData(_ url: URL) async -> Data? {
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 10)
        request.httpMethod = "GET"
        guard let (data, response) = try? await URLSession.shared.data(for: request),
            let http = response as? HTTPURLResponse, http.statusCode == 200, !data.isEmpty
        else { return nil }
        return data
    }
}

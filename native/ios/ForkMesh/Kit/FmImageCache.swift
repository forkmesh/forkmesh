import Foundation
import UIKit

enum FmImageCache {
    static let costLimit: Int = 48 * 1024 * 1024
    static let edge: Int = 512

    private static let cache: NSCache<NSString, UIImage> = {
        let cache: NSCache<NSString, UIImage> = NSCache<NSString, UIImage>()
        cache.totalCostLimit = costLimit
        return cache
    }()

    static func image(_ data: Data) -> UIImage? {
        guard !data.isEmpty else { return nil }
        let key: NSString = fingerprint(data) as NSString
        if let hit = cache.object(forKey: key) { return hit }
        guard let decoded = UIImage(data: data) else { return nil }
        let image: UIImage = decoded.preparingForDisplay() ?? decoded
        cache.setObject(image, forKey: key, cost: cost(image))
        return image
    }

    static func image(base64: String) -> UIImage? {
        guard !base64.isEmpty else { return nil }
        let key: NSString = ("b64:" + fingerprint(Data(base64.utf8))) as NSString
        if let hit = cache.object(forKey: key) { return hit }
        guard let data = Data(base64Encoded: base64, options: .ignoreUnknownCharacters), let decoded = UIImage(data: data) else {
            return nil
        }
        let image: UIImage = decoded.preparingForDisplay() ?? decoded
        cache.setObject(image, forKey: key, cost: cost(image))
        return image
    }

    static func fingerprint(_ data: Data) -> String {
        var hasher: Hasher = Hasher()
        hasher.combine(data.count)
        hasher.combine(data.prefix(edge))
        hasher.combine(data.suffix(edge))
        return "\(data.count):\(hasher.finalize())"
    }

    private static func cost(_ image: UIImage) -> Int {
        guard let cg = image.cgImage else { return Int(image.size.width * image.size.height * image.scale * image.scale * 4) }
        return cg.bytesPerRow * cg.height
    }
}

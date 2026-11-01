import FMAssistant
import Foundation
import SwiftUI
import UIKit

final class CloudDragonArt: @unchecked Sendable {
    nonisolated static let atlasName: String = "cloud-dragon-atlas"

    static let shared: CloudDragonArt? = CloudDragonArt.load(Bundle.main)

    let atlas: DragonAtlas
    let layers: [DragonLayer: Image]
    let props: [String: Image]
    let columns: [(image: Image, x: Double, w: Double)]
    let stripHeight: Double

    init?(atlas: DragonAtlas, picture: CGImage) {
        self.atlas = atlas
        func slice(_ x: Double, _ y: Double, _ w: Double, _ h: Double) -> Image? {
            let rect: CGRect = CGRect(x: x, y: y, width: w, height: h)
            guard rect.maxX <= Double(picture.width), rect.maxY <= Double(picture.height),
                let part = picture.cropping(to: rect)
            else { return nil }
            return Image(decorative: part, scale: 1).interpolation(.high)
        }
        var layers: [DragonLayer: Image] = [:]
        for layer in DragonLayer.allCases where layer != .tailStrip {
            let f: DragonLayerFrame = atlas.layer(layer)
            guard let image = slice(f.x, f.y, f.w, f.h) else { return nil }
            layers[layer] = image
        }
        var props: [String: Image] = [:]
        for (name, f) in atlas.props {
            guard let image = slice(f.x, f.y, f.w, f.h) else { return nil }
            props[name] = image
        }
        let strip: DragonLayerFrame = atlas.layer(.tailStrip)
        let n: Int = atlas.tail.curve.count - 1
        var columns: [(image: Image, x: Double, w: Double)] = []
        for i in 0..<n {
            let box: (x: Double, w: Double) = DragonTail.column(i, segments: n, stripWidth: strip.w)
            guard let image = slice(strip.x + box.x, strip.y, box.w, strip.h) else { return nil }
            columns.append((image, box.x, box.w))
        }
        self.layers = layers
        self.props = props
        self.columns = columns
        self.stripHeight = strip.h
    }

    static func load(_ bundle: Bundle) -> CloudDragonArt? {
        guard let json = bundle.url(forResource: atlasName, withExtension: "json"),
            let image = bundle.url(forResource: atlasName, withExtension: "webp"),
            let data = try? Data(contentsOf: json), let atlas = try? DragonAtlas(json: data),
            let decoded = UIImage(contentsOfFile: image.path)?.cgImage, let picture = CloudDragonArt.bitmap(decoded)
        else { return nil }
        return CloudDragonArt(atlas: atlas, picture: picture)
    }

    static func bitmap(_ source: CGImage) -> CGImage? {
        guard let context = CGContext(
            data: nil, width: source.width, height: source.height, bitsPerComponent: 8, bytesPerRow: 0,
            space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        context.draw(source, in: CGRect(x: 0, y: 0, width: source.width, height: source.height))
        return context.makeImage()
    }
}

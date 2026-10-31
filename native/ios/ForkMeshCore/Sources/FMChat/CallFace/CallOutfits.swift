import Foundation

public struct CallOutfit: Equatable, Sendable {
    public let id: String
    public let label: String
}

public struct CallOutfitFrame: Equatable, Sendable {
    public let center: CallFacePoint
    public let x: CallFacePoint
    public let y: CallFacePoint
    public let unit: Double
    public let roll: Double
}

public enum CallOutfits {
    public static let storageKey = "forkmesh.chat.callOutfit"
    public static let size = 1024.0
    public static let unit = 96.0
    public static let neck = CallFacePoint(512, 176)
    public static let drop = 1.35
    public static let chinSpans = 1.9
    public static let rollShare = 0.3
    public static let tile = 256
    public static let reach = 0.35

    public static let all: [CallOutfit] = [
        CallOutfit(id: "", label: "Your own clothes"),
        CallOutfit(id: "suit", label: "Suit and tie"),
        CallOutfit(id: "hoodie", label: "Hoodie"),
        CallOutfit(id: "sweater", label: "Cable-knit sweater"),
        CallOutfit(id: "denim", label: "Denim jacket"),
        CallOutfit(id: "flannel", label: "Flannel shirt"),
        CallOutfit(id: "turtleneck", label: "Turtleneck"),
    ]

    public static func byId(_ id: String) -> CallOutfit? {
        all.first { $0.id == id }
    }

    public static func frame(_ points: [CallFacePoint]?) -> CallOutfitFrame? {
        guard let points, points.count >= 68 else { return nil }
        let face = CallFaceGeometry.faceFrame(points)
        let chin = points[8]
        let unit = CallFaceJS.max(face.span, CallFaceJS.hypot(chin.x - face.mid.x, chin.y - face.mid.y) / chinSpans)
        let roll = face.roll * rollShare
        let ux = CallFacePoint(cos(roll), sin(roll))
        let uy = CallFacePoint(-ux.y, ux.x)
        let base = CallFacePoint((face.mid.x + chin.x) / 2, chin.y)
        return CallOutfitFrame(
            center: CallFacePoint(base.x + uy.x * drop * unit, base.y + uy.y * drop * unit),
            x: CallFacePoint(ux.x / unit, ux.y / unit),
            y: CallFacePoint(uy.x / unit, uy.y / unit),
            unit: unit,
            roll: roll
        )
    }

    public static func uv(_ frame: CallOutfitFrame, _ pixel: CallFacePoint) -> CallFacePoint {
        let d = CallFacePoint(pixel.x - frame.center.x, pixel.y - frame.center.y)
        let q = CallFacePoint(d.x * frame.x.x + d.y * frame.x.y, d.x * frame.y.x + d.y * frame.y.y)
        return CallFacePoint((neck.x + q.x * unit) / size, (neck.y + q.y * unit) / size)
    }
}

public final class CallOutfitSmoother {
    public let rate: Double
    private var last: CallOutfitFrame?

    public init(rate: Double = 0.35) {
        self.rate = rate
    }

    public func smooth(_ frame: CallOutfitFrame?) -> CallOutfitFrame? {
        guard let frame else { return last }
        guard let previous = last else {
            last = frame
            return frame
        }
        func mix(_ a: CallFacePoint, _ b: CallFacePoint) -> CallFacePoint {
            CallFacePoint(a.x + (b.x - a.x) * rate, a.y + (b.y - a.y) * rate)
        }
        let unit = previous.unit + (frame.unit - previous.unit) * rate
        let roll = previous.roll + (frame.roll - previous.roll) * rate
        let ux = CallFacePoint(cos(roll), sin(roll))
        let uy = CallFacePoint(-ux.y, ux.x)
        let next = CallOutfitFrame(center: mix(previous.center, frame.center), x: CallFacePoint(ux.x / unit, ux.y / unit), y: CallFacePoint(uy.x / unit, uy.y / unit), unit: unit, roll: roll)
        last = next
        return next
    }

    public func reset() {
        last = nil
    }
}

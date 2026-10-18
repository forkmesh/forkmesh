import Foundation

public struct PromptMarkupFit: Equatable, Sendable {
    public let x: Double
    public let y: Double
    public let width: Double
    public let height: Double

    public init(x: Double, y: Double, width: Double, height: Double) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }

    public static let empty = PromptMarkupFit(x: 0, y: 0, width: 0, height: 0)
}

public enum PromptMarkupRules {
    public static let captureName = "screenshot"

    public static func fit(imageWidth: Double, imageHeight: Double, boxWidth: Double, boxHeight: Double) -> PromptMarkupFit {
        if imageWidth <= 0 || imageHeight <= 0 || boxWidth <= 0 || boxHeight <= 0 { return .empty }
        let scale = min(boxWidth / imageWidth, boxHeight / imageHeight)
        let width = imageWidth * scale
        let height = imageHeight * scale
        return PromptMarkupFit(x: (boxWidth - width) / 2, y: (boxHeight - height) / 2, width: width, height: height)
    }

    public static func pixelSize(width: Double, height: Double) -> (width: Int, height: Int) {
        PromptPhotoRules.targetSize(width: max(1, Int(width.rounded())), height: max(1, Int(height.rounded())))
    }

    public static func inkScale(pixelWidth: Int, canvasWidth: Double) -> Double {
        if canvasWidth <= 0 || pixelWidth <= 0 { return 1 }
        return Double(pixelWidth) / canvasWidth
    }
}

public struct CaptureArea: Equatable, Sendable {
    public let x: Double
    public let y: Double
    public let width: Double
    public let height: Double

    public init(x: Double, y: Double, width: Double, height: Double) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }

    public static let empty = CaptureArea(x: 0, y: 0, width: 0, height: 0)
}

public enum CaptureAreaRules {
    public static let minimumSide: Double = 16
    public static let largestElementShare: Double = 0.5

    public static func area(
        fromX: Double, fromY: Double, toX: Double, toY: Double, boundsWidth: Double, boundsHeight: Double
    ) -> CaptureArea {
        if boundsWidth <= 0 || boundsHeight <= 0 { return .empty }
        let left = min(max(min(fromX, toX), 0), boundsWidth)
        let right = min(max(max(fromX, toX), 0), boundsWidth)
        let top = min(max(min(fromY, toY), 0), boundsHeight)
        let bottom = min(max(max(fromY, toY), 0), boundsHeight)
        return CaptureArea(x: left, y: top, width: right - left, height: bottom - top)
    }

    public static func isUsable(_ area: CaptureArea) -> Bool {
        area.width >= minimumSide && area.height >= minimumSide
    }

    public static func pixels(_ area: CaptureArea, scale: Double, pixelWidth: Int, pixelHeight: Int) -> CaptureArea {
        if scale <= 0 || pixelWidth <= 0 || pixelHeight <= 0 { return .empty }
        let left = min(max((area.x * scale).rounded(.down), 0), Double(pixelWidth))
        let top = min(max((area.y * scale).rounded(.down), 0), Double(pixelHeight))
        let right = min(max(((area.x + area.width) * scale).rounded(.up), left), Double(pixelWidth))
        let bottom = min(max(((area.y + area.height) * scale).rounded(.up), top), Double(pixelHeight))
        return CaptureArea(x: left, y: top, width: right - left, height: bottom - top)
    }

    public static func isContent(elementWidth: Double, elementHeight: Double, boundsWidth: Double, boundsHeight: Double) -> Bool {
        if elementWidth <= 0 || elementHeight <= 0 { return false }
        return elementWidth * elementHeight <= boundsWidth * boundsHeight * largestElementShare
    }
}

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

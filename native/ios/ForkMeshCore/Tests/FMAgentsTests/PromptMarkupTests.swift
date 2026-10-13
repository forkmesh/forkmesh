import FMApi
import Foundation
import XCTest
@testable import FMAgents

final class PromptMarkupTests: XCTestCase {
    func testFitCentersTheImageInsideTheBox() {
        XCTAssertEqual(
            PromptMarkupRules.fit(imageWidth: 2000, imageHeight: 1000, boxWidth: 400, boxHeight: 300),
            PromptMarkupFit(x: 0, y: 50, width: 400, height: 200))
        XCTAssertEqual(
            PromptMarkupRules.fit(imageWidth: 500, imageHeight: 1000, boxWidth: 400, boxHeight: 400),
            PromptMarkupFit(x: 100, y: 0, width: 200, height: 400))
        XCTAssertEqual(PromptMarkupRules.fit(imageWidth: 0, imageHeight: 10, boxWidth: 400, boxHeight: 400), .empty)
        XCTAssertEqual(PromptMarkupRules.fit(imageWidth: 10, imageHeight: 10, boxWidth: 400, boxHeight: 0), .empty)
    }

    func testPixelSizeKeepsTheAttachmentLimit() {
        XCTAssertTrue(PromptMarkupRules.pixelSize(width: 1179, height: 2556) == (945, 2048))
        XCTAssertTrue(PromptMarkupRules.pixelSize(width: 800, height: 600) == (800, 600))
        XCTAssertTrue(PromptMarkupRules.pixelSize(width: 0, height: 0) == (1, 1))
        let size = PromptMarkupRules.pixelSize(width: 4032, height: 3024)
        XCTAssertLessThanOrEqual(max(size.width, size.height), PromptAttachmentLimits.maxDimension)
    }

    func testInkScaleMapsTheCanvasOntoThePixels() {
        XCTAssertEqual(PromptMarkupRules.inkScale(pixelWidth: 945, canvasWidth: 315), 3, accuracy: 0.0001)
        XCTAssertEqual(PromptMarkupRules.inkScale(pixelWidth: 945, canvasWidth: 0), 1)
        XCTAssertEqual(PromptMarkupRules.inkScale(pixelWidth: 0, canvasWidth: 300), 1)
    }

    func testCapturesAreNamedScreenshot() {
        XCTAssertEqual(PromptPhotoRules.attachmentName(PromptMarkupRules.captureName), "screenshot.jpg")
        XCTAssertEqual(PromptPhotoRules.attachmentName("shared-1.jpg"), "shared-1.jpg")
    }
}

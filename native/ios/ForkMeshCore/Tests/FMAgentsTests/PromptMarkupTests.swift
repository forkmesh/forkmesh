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

final class CaptureAreaTests: XCTestCase {
    func testAreaIsTheDraggedBoxInAnyDirectionClampedToTheScreen() {
        XCTAssertEqual(
            CaptureAreaRules.area(fromX: 300, fromY: 500, toX: 100, toY: 200, boundsWidth: 390, boundsHeight: 844),
            CaptureArea(x: 100, y: 200, width: 200, height: 300))
        XCTAssertEqual(
            CaptureAreaRules.area(fromX: -20, fromY: 800, toX: 420, toY: 900, boundsWidth: 390, boundsHeight: 844),
            CaptureArea(x: 0, y: 800, width: 390, height: 44))
        XCTAssertEqual(CaptureAreaRules.area(fromX: 0, fromY: 0, toX: 10, toY: 10, boundsWidth: 0, boundsHeight: 844), .empty)
    }

    func testTinyBoxesAreNotUsable() {
        XCTAssertTrue(CaptureAreaRules.isUsable(CaptureArea(x: 0, y: 0, width: 16, height: 40)))
        XCTAssertFalse(CaptureAreaRules.isUsable(CaptureArea(x: 0, y: 0, width: 15, height: 400)))
        XCTAssertFalse(CaptureAreaRules.isUsable(.empty))
    }

    func testPixelsCoverTheWholeBoxInsideTheImage() {
        XCTAssertEqual(
            CaptureAreaRules.pixels(CaptureArea(x: 10.2, y: 20.5, width: 100, height: 50), scale: 3, pixelWidth: 1170, pixelHeight: 2532),
            CaptureArea(x: 30, y: 61, width: 301, height: 151))
        XCTAssertEqual(
            CaptureAreaRules.pixels(CaptureArea(x: 300, y: 800, width: 200, height: 100), scale: 3, pixelWidth: 1170, pixelHeight: 2532),
            CaptureArea(x: 900, y: 2400, width: 270, height: 132))
        XCTAssertEqual(CaptureAreaRules.pixels(CaptureArea(x: 0, y: 0, width: 10, height: 10), scale: 0, pixelWidth: 10, pixelHeight: 10), .empty)
    }

    func testOnlyElementsSmallerThanHalfTheScreenCountAsContent() {
        XCTAssertTrue(CaptureAreaRules.isContent(elementWidth: 80, elementHeight: 44, boundsWidth: 390, boundsHeight: 844))
        XCTAssertFalse(CaptureAreaRules.isContent(elementWidth: 390, elementHeight: 600, boundsWidth: 390, boundsHeight: 844))
        XCTAssertFalse(CaptureAreaRules.isContent(elementWidth: 0, elementHeight: 44, boundsWidth: 390, boundsHeight: 844))
    }
}

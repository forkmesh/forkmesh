import Foundation
import XCTest

@testable import FMChat

final class CallFaceStageTests: XCTestCase {
    private func livePoints(shift: Double = 0) -> [CallFacePoint] {
        CallFaces.facePoints(CallFaces.all[0]).map { CallFacePoint($0.x * 0.6 + 40 + shift, $0.y * 0.6 + 60) }
    }

    func testFrameMapRoundTripsEveryQuarterTurn() {
        for degrees in [0, 90, 180, 270] {
            let map = CallFaceFrameMap(bufferWidth: 640, bufferHeight: 480, degrees: degrees)
            let point = CallFacePoint(123.5, 77.25)
            let back = map.toBuffer(map.toUpright(point))
            XCTAssertEqual(back.x, point.x, accuracy: 1e-9)
            XCTAssertEqual(back.y, point.y, accuracy: 1e-9)
            let matrix = map.toBufferMatrix
            let upright = map.toUpright(point)
            XCTAssertEqual(matrix[0] * upright.x + matrix[2] * upright.y + matrix[4], point.x, accuracy: 1e-9)
            XCTAssertEqual(matrix[1] * upright.x + matrix[3] * upright.y + matrix[5], point.y, accuracy: 1e-9)
            let inverse = map.toUprightMatrix
            XCTAssertEqual(inverse[0] * point.x + inverse[2] * point.y + inverse[4], upright.x, accuracy: 1e-9)
            XCTAssertEqual(inverse[1] * point.x + inverse[3] * point.y + inverse[5], upright.y, accuracy: 1e-9)
        }
    }

    func testQuarterTurnMatchesVisionRightOrientation() {
        let map = CallFaceFrameMap(bufferWidth: 640, bufferHeight: 480, degrees: 90)
        XCTAssertEqual(map.uprightWidth, 480)
        XCTAssertEqual(map.uprightHeight, 640)
        XCTAssertEqual(map.toBuffer(CallFacePoint(0, 0)), CallFacePoint(0, 480))
        XCTAssertEqual(map.toUpright(CallFacePoint(0, 0)), CallFacePoint(480, 0))
        XCTAssertEqual(CallFaceFrameMap(bufferWidth: 4, bufferHeight: 2, degrees: -90).degrees, 270)
        XCTAssertEqual(CallFaceFrameMap(bufferWidth: 4, bufferHeight: 2, degrees: 450).degrees, 90)
    }

    func testReducedSizeFollowsTheUprightWidth() {
        let flat = CallFaceFrameMap(bufferWidth: 640, bufferHeight: 480, degrees: 0).reducedSize(160)
        XCTAssertEqual(flat.width, 160)
        XCTAssertEqual(flat.height, 120)
        XCTAssertEqual(flat.scale, 0.25, accuracy: 1e-12)
        let turned = CallFaceFrameMap(bufferWidth: 640, bufferHeight: 480, degrees: 90).reducedSize(160)
        XCTAssertEqual(turned.width, 213)
        XCTAssertEqual(turned.height, 160)
        XCTAssertEqual(turned.scale, 1.0 / 3.0, accuracy: 1e-12)
        let small = CallFaceFrameMap(bufferWidth: 100, bufferHeight: 80, degrees: 0).reducedSize(160)
        XCTAssertEqual(small.width, 100)
        XCTAssertEqual(small.scale, 1)
        let levels = CallFaceDrawPlan.levelSizes(width: 192, height: 145)
        XCTAssertEqual(levels.count, 6)
        XCTAssertTrue(levels[1] == (96, 73))
        XCTAssertTrue(levels[5] == (6, 5))
        XCTAssertEqual(CallFaceDrawPlan.haloSigma(0.25).sigma, 2.75, accuracy: 1e-12)
        XCTAssertEqual(CallFaceDrawPlan.haloSigma(0.25).passes, 2)
        XCTAssertEqual(CallFaceDrawPlan.blurStep(0.4), 0.5)
    }

    func testStageFadesInWhenFoundAndHidesWhenLost() {
        let stage = CallFaceStage()
        stage.hasFace = true
        stage.faceReady = true
        let first = stage.step(.found(points: livePoints(), pupils: nil, probe: nil, time: 1000), time: 1000)
        XCTAssertEqual(first.presence, 0.3, accuracy: 1e-12)
        XCTAssertEqual(first.hide, 0.7, accuracy: 1e-12)
        XCTAssertNotNil(first.drawn)
        let stale = stage.step(.stale, time: 1040)
        XCTAssertLessThan(stale.hide, first.hide)
        XCTAssertGreaterThan(stale.presence, first.presence)
        let missing = stage.step(.missing, time: 1080)
        XCTAssertGreaterThan(missing.hide, stale.hide)
        XCTAssertNotNil(missing.drawn)
        let gone = stage.step(.missing, time: 1600)
        XCTAssertNil(gone.drawn)
        XCTAssertLessThan(gone.presence, missing.presence)
    }

    func testStageWithoutFaceNeverHidesAndPlacesTheOutfit() {
        let stage = CallFaceStage()
        stage.hasOutfit = true
        let frame = stage.step(.found(points: livePoints(), pupils: nil, probe: nil, time: 0), time: 0)
        XCTAssertEqual(frame.hide, 0)
        XCTAssertNil(frame.drawn)
        XCTAssertNotNil(frame.outfit)
        XCTAssertEqual(frame.dressed, 0.3, accuracy: 1e-12)
        let plan = CallFaceDrawPlan.make(head: nil, frame: frame, hasOutfit: true)
        XCTAssertTrue(plan.wearing)
        XCTAssertFalse(plan.showFace)
        XCTAssertNotNil(plan.outfitFace)
        XCTAssertTrue(plan.drawsAnything)
    }

    func testDrawPlanBuildsTheWarpForAShownFace() {
        let stage = CallFaceStage()
        stage.hasFace = true
        stage.faceReady = true
        let head = CallFaces.head(CallFaces.all[1])
        let frame = stage.step(.found(points: livePoints(), pupils: nil, probe: nil, time: 0), time: 0)
        let plan = CallFaceDrawPlan.make(head: head, frame: frame, hasOutfit: false)
        XCTAssertTrue(plan.showFace)
        XCTAssertEqual(plan.vertices.count, head.mesh.triangles.count * 3 * CallFaceWarp.vertexFloats)
        XCTAssertNotNil(plan.zone)
        XCTAssertNil(plan.tongueFrame)
        XCTAssertEqual(plan.skinRed, CallFaceDrawPlan.defaultSkinRed)
        XCTAssertEqual(plan.look, [0, 0, 0, 0])
        XCTAssertEqual(plan.close, [0, 0])
        XCTAssertEqual(plan.presence, frame.presence)
        XCTAssertGreaterThan(plan.span, 1)
        let none = CallFaceDrawPlan.make(head: nil, frame: frame, hasOutfit: false)
        XCTAssertFalse(none.showFace)
        XCTAssertTrue(none.vertices.isEmpty)
        XCTAssertTrue(none.drawsAnything)
    }

    func testIdleStageDrawsNothing() {
        let stage = CallFaceStage()
        let frame = stage.step(.missing, time: 0)
        XCTAssertFalse(CallFaceDrawPlan.make(head: nil, frame: frame, hasOutfit: false).drawsAnything)
    }

    func testProbeImageSamplesPixelCentresAndBlacksOutTheOutside() {
        let crop = CallFaceProbeCrop(minX: -2, minY: 10, scale: 0.5, width: 4, height: 2)
        var seen: [CallFacePoint] = []
        let image = CallFaceStage.probeImage(crop: crop) { x, y in
            seen.append(CallFacePoint(x, y))
            return x < 0 ? nil : (300, 127.5, -4)
        }
        XCTAssertEqual(seen.first, CallFacePoint(-1, 11))
        XCTAssertEqual(seen[1], CallFacePoint(1, 11))
        XCTAssertEqual(Array(image.rgba[0..<4]), [0, 0, 0, 255])
        XCTAssertEqual(Array(image.rgba[4..<8]), [255, 128, 0, 255])
    }
}

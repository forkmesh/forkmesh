import Foundation
import XCTest

@testable import FMChat

final class CallFaceTrackingTests: XCTestCase {
    private func face(_ transform: (CallFacePoint) -> CallFacePoint) -> [CallFacePoint] {
        CallFaces.facePoints(CallFaces.all[0]).map { transform(CallFacePoint($0.x * 0.5 + 60, $0.y * 0.5 + 40)) }
    }

    private func mesh(_ transform: (CallFacePoint) -> CallFacePoint, depth: Bool = false) -> (vertices: [CallFacePoint], triangles: [Int]) {
        let columns = 14
        let rows = 16
        var vertices: [CallFacePoint] = []
        for r in 0..<rows {
            for c in 0..<columns {
                let p = CallFacePoint(40 + Double(c) * 22, 20 + Double(r) * 22)
                let moved = transform(p)
                let bulge = depth ? 0.6 - 0.02 * exp(-pow((p.x - 190) / 90, 2) - pow((p.y - 180) / 110, 2)) : nil
                vertices.append(CallFacePoint(moved.x, moved.y, bulge))
            }
        }
        var triangles: [Int] = []
        for r in 0..<(rows - 1) {
            for c in 0..<(columns - 1) {
                let a = r * columns + c
                triangles += [a, a + 1, a + columns, a + 1, a + columns + 1, a + columns]
            }
        }
        return (vertices, triangles)
    }

    private func pose(_ angle: Double, _ scale: Double, _ dx: Double, _ dy: Double) -> (CallFacePoint) -> CallFacePoint {
        { p in
            let x = p.x - 190
            let y = p.y - 180
            return CallFacePoint(190 + dx + scale * (x * cos(angle) - y * sin(angle)), 180 + dy + scale * (x * sin(angle) + y * cos(angle)), p.z)
        }
    }

    func testTemplateEyeZeroIsOnTheImageLeft() {
        let points = CallFaces.template
        XCTAssertLessThan(points[36].x, points[45].x)
        XCTAssertLessThan(CallFaceExpression.eyeBox(points, 0).center.x, CallFaceExpression.eyeBox(points, 1).center.x)
    }

    func testBlendShapesMapOntoTheProbeReading() {
        var shapes = CallFaceBlendShapes()
        shapes.eyeBlinkRight = 0.9
        shapes.eyeBlinkLeft = 0.1
        shapes.eyeLookInRight = 1
        shapes.eyeLookOutLeft = 1
        shapes.eyeLookDownLeft = 0.5
        shapes.tongueOut = 0.6
        let close = CallFaceBlendMapping.close(shapes)
        XCTAssertEqual(close[0], 1)
        XCTAssertEqual(close[1], 0)
        let look = CallFaceBlendMapping.look(shapes)
        XCTAssertEqual(look[0].x, CallFaceExpression.lookLimit, accuracy: 1e-12)
        XCTAssertEqual(look[1].x, CallFaceExpression.lookLimit, accuracy: 1e-12)
        XCTAssertEqual(look[1].y, CallFaceExpression.lookLimit * 0.5, accuracy: 1e-12)
        XCTAssertEqual(CallFaceBlendMapping.tongue(shapes), 1)
        shapes.tongueOut = .nan
        XCTAssertEqual(CallFaceBlendMapping.tongue(shapes), 0)
        let reading = CallFaceBlendMapping.reading(shapes, probe: nil)
        XCTAssertEqual(reading.skinRed, CallFaceDrawPlan.defaultSkinRed)
        XCTAssertEqual(reading.close, close)
    }

    func testStageUsesBlendShapesForTheExpression() {
        let stage = CallFaceStage()
        stage.hasFace = true
        stage.faceReady = true
        var shapes = CallFaceBlendShapes()
        shapes.eyeBlinkLeft = 1
        stage.blend = shapes
        let frame = stage.step(.found(points: face { $0 }, pupils: nil, probe: nil, time: 0), time: 0)
        XCTAssertEqual(frame.expression?.close, [0, 1])
        let drawn = try? XCTUnwrap(frame.drawn)
        XCTAssertNotEqual(drawn?[43], face { $0 }[43])
    }

    func testBindingLearnsThenFollowsTheMesh() throws {
        let start = mesh { $0 }
        let binding = CallFaceMeshBinding(triangles: start.triangles)
        XCTAssertTrue(binding.due(time: 0))
        for (i, move) in [pose(0, 1, 0, 0), pose(0.05, 1.02, 3, -2), pose(-0.04, 0.98, -4, 1)].enumerated() {
            XCTAssertNil(binding.anchors)
            binding.learn(landmarks: face(move), vertices: mesh(move).vertices, time: Double(i) * 33)
        }
        XCTAssertTrue(binding.ready)
        XCTAssertFalse(binding.due(time: 100))
        XCTAssertTrue(binding.due(time: 66 + CallFaceMeshBinding.refreshMs))
        let later = pose(0.2, 1.3, 30, 18)
        let placed = try XCTUnwrap(binding.place(mesh(later).vertices))
        let expected = face(later)
        for i in 0..<68 {
            XCTAssertEqual(placed[i].x, expected[i].x, accuracy: 1e-6)
            XCTAssertEqual(placed[i].y, expected[i].y, accuracy: 1e-6)
            XCTAssertNil(placed[i].z)
        }
        binding.learn(landmarks: expected, vertices: mesh(later).vertices, time: 3000)
        XCTAssertEqual(binding.misses, 0)
        XCTAssertLessThan(binding.lastError ?? 1, 1e-6)
    }

    func testBindingGivesDepthInImagePixels() throws {
        let binding = CallFaceMeshBinding(triangles: mesh { $0 }.triangles)
        for i in 0..<3 { binding.learn(landmarks: face { $0 }, vertices: mesh({ $0 }, depth: true).vertices, time: Double(i)) }
        let placed = try XCTUnwrap(binding.place(mesh({ $0 }, depth: true).vertices, focal: 500))
        XCTAssertTrue(placed.allSatisfy { $0.z != nil })
        XCTAssertGreaterThan(placed[30].z!, placed[0].z!)
        XCTAssertTrue(CallFaceGeometry.hasDepth(placed))
    }

    func testMirroredMeshNeverBinds() {
        let mirror: (CallFacePoint) -> CallFacePoint = { CallFacePoint(380 - $0.x, $0.y, $0.z) }
        let binding = CallFaceMeshBinding(triangles: mesh { $0 }.triangles)
        var time = 0.0
        for round in 0..<CallFaceMeshBinding.maxMisses {
            for (k, move) in [pose(0, 1, -40, 0), pose(0, 1, 0, 0), pose(0, 1, 40, 0)].enumerated() {
                binding.learn(landmarks: face(move), vertices: mesh({ mirror(move($0)) }).vertices, time: time)
                time += 33
                if round == 0 && k < 2 { XCTAssertTrue(binding.due(time: time)) }
            }
        }
        XCTAssertNil(binding.anchors)
        XCTAssertTrue(binding.unreliable)
        XCTAssertFalse(binding.due(time: time + 1e6))
        XCTAssertNil(binding.place(mesh { $0 }.vertices))
        binding.reset()
        XCTAssertTrue(binding.due(time: 0))
    }

    func testBindingRelearnsAfterTheMeshDrifts() {
        let binding = CallFaceMeshBinding(triangles: mesh { $0 }.triangles)
        for i in 0..<3 { binding.learn(landmarks: face { $0 }, vertices: mesh { $0 }.vertices, time: Double(i)) }
        XCTAssertTrue(binding.ready)
        let shifted: (CallFacePoint) -> CallFacePoint = { CallFacePoint($0.x + 30, $0.y) }
        binding.learn(landmarks: face(shifted), vertices: mesh { $0 }.vertices, time: 2500)
        XCTAssertEqual(binding.misses, 1)
        XCTAssertTrue(binding.ready)
        XCTAssertTrue(binding.due(time: 2501))
        binding.learn(landmarks: face(shifted), vertices: mesh { $0 }.vertices, time: 2533)
        binding.learn(landmarks: face(shifted), vertices: mesh { $0 }.vertices, time: 2566)
        XCTAssertEqual(binding.misses, 0)
        let placed = binding.place(mesh { $0 }.vertices)
        XCTAssertEqual(placed?[30].x ?? 0, face(shifted)[30].x, accuracy: 1e-6)
    }

    func testCaptureRotationFollowsTheWebRTCCapturer() {
        XCTAssertEqual(CallFaceCapture.rotation(deviceOrientation: 1, front: true, last: 0), 90)
        XCTAssertEqual(CallFaceCapture.rotation(deviceOrientation: 2, front: true, last: 0), 270)
        XCTAssertEqual(CallFaceCapture.rotation(deviceOrientation: 3, front: true, last: 0), 180)
        XCTAssertEqual(CallFaceCapture.rotation(deviceOrientation: 4, front: true, last: 0), 0)
        XCTAssertEqual(CallFaceCapture.rotation(deviceOrientation: 3, front: false, last: 0), 0)
        XCTAssertEqual(CallFaceCapture.rotation(deviceOrientation: 5, front: true, last: 270), 270)
        XCTAssertEqual(CallFaceCapture.rotation(deviceOrientation: 0, front: true, last: 90), 90)
    }

    func testFormatPickPrefersTheCallRateAndSize() {
        let formats = [
            CallFaceVideoFormat(width: 1280, height: 720, fps: 60),
            CallFaceVideoFormat(width: 1280, height: 720, fps: 30),
            CallFaceVideoFormat(width: 640, height: 480, fps: 30),
            CallFaceVideoFormat(width: 1920, height: 1080, fps: 30),
        ]
        XCTAssertEqual(CallFaceCapture.pick(formats, width: 1280, height: 720, fps: 30), 1)
        XCTAssertEqual(CallFaceCapture.pick(formats, width: 640, height: 360, fps: 24), 2)
        XCTAssertEqual(CallFaceCapture.pick(formats, width: 1280, height: 720, fps: 50), 0)
        XCTAssertEqual(CallFaceCapture.pick([CallFaceVideoFormat(width: 1280, height: 720, fps: 60)], width: 640, height: 480, fps: 30), 0)
        XCTAssertEqual(CallFaceCapture.pick(formats, width: 3840, height: 2160, fps: 30), 3)
        XCTAssertNil(CallFaceCapture.pick([], width: 640, height: 480, fps: 30))
    }

    func testFrameGateHalvesSixtyAndCapsThirty() {
        let gate = CallFaceFrameGate()
        var admitted = 0
        for i in 0..<120 where gate.admit(timeNs: Int64(Double(i) * 1e9 / 60), fps: 30) { admitted += 1 }
        XCTAssertEqual(admitted, 60)
        gate.reset()
        admitted = 0
        for i in 0..<90 where gate.admit(timeNs: Int64(Double(i) * 1e9 / 30), fps: 15) { admitted += 1 }
        XCTAssertEqual(admitted, 45)
        gate.reset()
        admitted = 0
        for i in 0..<30 where gate.admit(timeNs: Int64(Double(i) * 1e9 / 30), fps: 30) { admitted += 1 }
        XCTAssertEqual(admitted, 30)
        gate.reset()
        admitted = 0
        for i in 0..<300 where gate.admit(timeNs: Int64(Double(i) * 1e9 / 30), fps: 24) { admitted += 1 }
        XCTAssertGreaterThanOrEqual(admitted, 230)
        XCTAssertLessThanOrEqual(admitted, 260)
    }

    func testQualityStartsByDeviceAndStepsDownWhenSlowOrHot() {
        XCTAssertEqual(CallFaceQuality.startLevel(appleFamily: 9), 0)
        XCTAssertEqual(CallFaceQuality.startLevel(appleFamily: 6), 1)
        XCTAssertEqual(CallFaceQuality.startLevel(appleFamily: 5), 2)
        XCTAssertEqual(CallFaceQuality.startLevel(appleFamily: 0), 3)
        let governor = CallFaceQualityGovernor(start: 0)
        for _ in 0..<(CallFaceQuality.hold - 1) { XCTAssertEqual(governor.note(costMs: 60, thermal: .nominal), 0) }
        XCTAssertEqual(governor.note(costMs: 60, thermal: .nominal), 1)
        for _ in 0..<(CallFaceQuality.hold * 3) { governor.note(costMs: 60, thermal: .nominal) }
        XCTAssertEqual(governor.index, 3)
        for _ in 0..<(CallFaceQuality.hold * 8) { governor.note(costMs: 5, thermal: .nominal) }
        XCTAssertEqual(governor.index, 0)
        governor.note(costMs: 5, thermal: .serious)
        XCTAssertEqual(governor.index, 2)
        XCTAssertEqual(governor.level.guide, 256)
        governor.note(costMs: 5, thermal: .critical)
        XCTAssertEqual(governor.level, CallFaceQuality.levels[3])
        XCTAssertTrue(governor.level.runsHair(frame: 8))
        XCTAssertFalse(governor.level.runsHair(frame: 9))
        XCTAssertEqual(CallFaceQuality.capture(.critical, fps: 30).fps, 15)
        XCTAssertEqual(CallFaceQuality.capture(.serious, fps: 30).fps, 24)
        XCTAssertEqual(CallFaceQuality.capture(.fair, fps: 30).scale, 1)
        XCTAssertEqual(CallFaceQuality.capture(.serious, fps: 15).fps, 15)
    }

    func testMatteSizesFollowTheBrowserInUprightPixels() {
        let flat = CallFaceFrameMap(bufferWidth: 640, bufferHeight: 480, degrees: 0)
        let turned = CallFaceFrameMap(bufferWidth: 640, bufferHeight: 480, degrees: 90)
        XCTAssertEqual(CallFaceMatte.hairSize(map: flat), CallFaceSize(256, 192))
        XCTAssertEqual(CallFaceMatte.hairSize(map: turned), CallFaceSize(256, 341))
        XCTAssertEqual(CallFaceMatte.guideSize(320, map: flat), CallFaceSize(320, 240))
        XCTAssertEqual(CallFaceMatte.guideSize(320, map: turned), CallFaceSize(427, 320))
        XCTAssertEqual(CallFaceMatte.regionSize(rawWidth: 128, rawHeight: 96, map: flat), CallFaceSize(64, 48))
        XCTAssertEqual(CallFaceMatte.regionSize(rawWidth: 128, rawHeight: 96, map: turned), CallFaceSize(85, 64))
        XCTAssertEqual(CallFaceMatte.guideSize(320, map: CallFaceFrameMap(bufferWidth: 200, bufferHeight: 100, degrees: 0)), CallFaceSize(200, 100))
        let uv = turned.toUprightUvMatrix
        let at = turned.toUpright(CallFacePoint(0.25 * 640, 0.75 * 480))
        XCTAssertEqual(uv[0] * 0.25 + uv[2] * 0.75 + uv[4], at.x / 480, accuracy: 1e-12)
        XCTAssertEqual(uv[1] * 0.25 + uv[3] * 0.75 + uv[5], at.y / 640, accuracy: 1e-12)
        XCTAssertEqual(CallFaceMatte.faceSeed(nil, width: 640, height: 480), [0, 0, -1])
        let seed = CallFaceMatte.faceSeed(face { $0 }, width: 640, height: 480)
        let frame = CallFaceGeometry.faceFrame(face { $0 })
        XCTAssertEqual(seed[2], 0.8 * frame.span / 480, accuracy: 1e-12)
        XCTAssertEqual(CallFaceMatte.blurStrength("blur-strong"), 1)
        XCTAssertNil(CallFaceMatte.blurStrength("office"))
    }

    func testDepthMappingComposesWithTheFrameMap() {
        XCTAssertEqual(CallFaceMatte.depthUv(imageWidth: 640, imageHeight: 480, depthWidth: 320, depthHeight: 240), [1, 0, 0, 1, 0, 0])
        let wide = CallFaceMatte.depthUv(imageWidth: 1280, imageHeight: 720, depthWidth: 640, depthHeight: 480)
        XCTAssertEqual(wide[3], 0.75, accuracy: 1e-12)
        XCTAssertEqual(wide[5], 0.125, accuracy: 1e-12)
        let tall = CallFaceMatte.depthUv(imageWidth: 640, imageHeight: 480, depthWidth: 1280, depthHeight: 720)
        XCTAssertEqual(tall[0], 0.75, accuracy: 1e-12)
        XCTAssertEqual(tall[4], 0.125, accuracy: 1e-12)
        let map = CallFaceFrameMap(bufferWidth: 640, bufferHeight: 480, degrees: 270)
        let m = CallFaceMatte.uprightToBufferUv(map)
        let p = CallFacePoint(100, 300)
        let b = map.toBuffer(p)
        XCTAssertEqual(m[0] * p.x + m[2] * p.y + m[4], b.x / 640, accuracy: 1e-12)
        XCTAssertEqual(m[1] * p.x + m[3] * p.y + m[5], b.y / 480, accuracy: 1e-12)
        let both = CallFaceMatte.compose(wide, m)
        XCTAssertEqual(both[1] * p.x + both[3] * p.y + both[5], 0.75 * b.y / 480 + 0.125, accuracy: 1e-12)
    }

    func testDrawPlanCarriesTheHairRegionAndSeed() {
        let stage = CallFaceStage()
        stage.hasFace = true
        stage.faceReady = true
        let points = face { $0 }
        var frame = stage.step(.found(points: points, pupils: nil, probe: nil, time: 0), time: 0)
        for t in 1...10 { frame = stage.step(.found(points: points, pupils: nil, probe: nil, time: Double(t) * 33), time: Double(t) * 33) }
        let head = CallFaces.head(CallFaces.all[1])
        let plan = CallFaceDrawPlan.make(head: head, frame: frame, hasOutfit: false, width: 480, height: 640)
        XCTAssertTrue(plan.showFace)
        let region = try? XCTUnwrap(plan.hairRegion)
        XCTAssertNotNil(region)
        XCTAssertLessThanOrEqual(region?.size.x ?? 1e9, 480)
        XCTAssertEqual(plan.seed.count, 3)
        XCTAssertGreaterThan(plan.seed[2], 0)
        XCTAssertNil(CallFaceDrawPlan.make(head: head, frame: frame, hasOutfit: false).hairRegion)
    }
}

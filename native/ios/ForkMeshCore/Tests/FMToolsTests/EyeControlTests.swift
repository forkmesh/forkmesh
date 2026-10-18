import FMTools
import XCTest

final class EyeControlTests: XCTestCase {
    private let screen: EyeScreen = EyeScreen(width: 400, height: 800, pointsPerMeter: 6000, camera: EyePoint(200, 0))

    private func eyes(lookingAt target: EyeVector, face: EyeTransform) -> (EyeTransform, EyeTransform) {
        func eye(_ offset: Double) -> EyeTransform {
            let local: EyeVector = EyeVector(offset, 0, 0)
            let world: EyeVector = face.point(local)
            let toward: EyeVector = (target - world).normalized!
            let yaw: Double = atan2(toward.x, toward.z)
            let pitch: Double = -asin(toward.y)
            return EyeTransform.translation(local) * EyeTransform.rotationY(yaw) * EyeTransform.rotationX(pitch)
        }
        return (eye(-0.032), eye(0.032))
    }

    private func assertNear(_ a: EyePoint?, _ b: EyePoint, accuracy: Double = 0.5, file: StaticString = #filePath, line: UInt = #line) {
        guard let a else { return XCTFail("no point", file: file, line: line) }
        XCTAssertEqual(a.x, b.x, accuracy: accuracy, file: file, line: line)
        XCTAssertEqual(a.y, b.y, accuracy: accuracy, file: file, line: line)
    }

    func testGazeStraightAtTheCameraLandsOnTheCamera() {
        let face: EyeTransform = EyeTransform.translation(EyeVector(0, 0, -0.35))
        let (left, right): (EyeTransform, EyeTransform) = eyes(lookingAt: EyeVector(0, 0, 0), face: face)
        assertNear(EyeGaze.screenPoint(face: face, leftEye: left, rightEye: right, screen: screen), EyePoint(200, 0))
    }

    func testGazeBelowAndRightOfTheCameraMapsToScreenPoints() {
        let face: EyeTransform = EyeTransform.translation(EyeVector(0.01, -0.05, -0.3))
        let (left, right): (EyeTransform, EyeTransform) = eyes(lookingAt: EyeVector(0.02, -0.06, 0), face: face)
        assertNear(EyeGaze.screenPoint(face: face, leftEye: left, rightEye: right, screen: screen), EyePoint(320, 360), accuracy: 2)
    }

    func testGazeAwayFromThePhoneHasNoPoint() {
        let face: EyeTransform = EyeTransform.translation(EyeVector(0, 0, -0.3))
        let (left, right): (EyeTransform, EyeTransform) = eyes(lookingAt: EyeVector(0, 0, -1), face: face)
        XCTAssertNil(EyeGaze.screenPoint(face: face, leftEye: left, rightEye: right, screen: screen))
    }

    func testScreenFromEdgeAndInches() {
        let phone: EyeScreen = EyeScreen(width: 390, height: 844, pointsPerInch: 155, cameraEdge: .right)
        XCTAssertEqual(phone.camera, EyePoint(390, 422))
        XCTAssertEqual(phone.pointsPerMeter, 155 / 0.0254, accuracy: 1e-9)
        XCTAssertEqual(phone.clamp(EyePoint(-5, 900)), EyePoint(0, 844))
    }

    func testCalibrationFitRecoversAnAffineMap() {
        let truth: EyeCalibration = EyeCalibration(a: -1.2, b: 0.1, c: 420, d: 0.05, e: 0.9, f: -30)
        let raw: [EyePoint] = [EyePoint(10, 20), EyePoint(300, 40), EyePoint(150, 600), EyePoint(380, 700), EyePoint(50, 400)]
        let fitted: EyeCalibration? = EyeCalibration.fit(raw: raw, targets: raw.map(truth.apply))
        XCTAssertNotNil(fitted)
        assertNear(fitted?.apply(EyePoint(200, 300)), truth.apply(EyePoint(200, 300)), accuracy: 1e-6)
        XCTAssertEqual(EyeCalibration.decode(fitted?.encoded)?.c ?? 0, fitted?.c ?? 1, accuracy: 1e-9)
    }

    func testCalibrationFitRefusesCollinearPoints() {
        let raw: [EyePoint] = [EyePoint(0, 0), EyePoint(1, 1), EyePoint(2, 2), EyePoint(3, 3)]
        XCTAssertNil(EyeCalibration.fit(raw: raw, targets: raw))
        XCTAssertNil(EyeCalibration.fit(raw: [EyePoint(0, 0)], targets: [EyePoint(0, 0)]))
        XCTAssertNil(EyeCalibration.decode("not json"))
    }

    func testOneEuroSettlesOnAStillValueAndFollowsAMove() {
        var filter: EyeOneEuro = EyeOneEuro(minCutoff: 0.7, beta: 0.003)
        XCTAssertEqual(filter.filter(100, at: 0), 100)
        var out: Double = 0
        for frame in 1...120 {
            out = filter.filter(frame % 2 == 0 ? 104 : 96, at: Double(frame) / 60)
        }
        XCTAssertEqual(out, 100, accuracy: 2)
        for frame in 121...240 {
            out = filter.filter(300, at: Double(frame) / 60)
        }
        XCTAssertEqual(out, 300, accuracy: 1)
    }

    private func blink(_ detector: inout EyeBlinkDetector, from start: Double, closedFor seconds: Double) -> [EyeBlinkEvent] {
        var events: [EyeBlinkEvent] = []
        var t: Double = start
        while t < start + seconds {
            if let event = detector.update(time: t, left: 0.9, right: 0.85) { events.append(event) }
            t += 1.0 / 60
        }
        if let event = detector.update(time: t, left: 0.05, right: 0.1) { events.append(event) }
        return events
    }

    func testAFirmBlinkClicksAndAnOrdinaryBlinkDoesNot() {
        var detector: EyeBlinkDetector = EyeBlinkDetector(length: .deliberate)
        XCTAssertEqual(blink(&detector, from: 0, closedFor: 0.1), [.closing, .ignored])
        XCTAssertEqual(blink(&detector, from: 1, closedFor: 0.3), [.closing, .click])
        XCTAssertEqual(blink(&detector, from: 3, closedFor: 2.0), [.closing, .ignored])
        XCTAssertFalse(detector.closing)
    }

    func testOneEyeClosedIsNotAClick() {
        var detector: EyeBlinkDetector = EyeBlinkDetector(length: .quick)
        var events: [EyeBlinkEvent] = []
        for frame in 0..<30 {
            if let event = detector.update(time: Double(frame) / 60, left: 0.95, right: 0.1) { events.append(event) }
        }
        if let event = detector.update(time: 1, left: 0, right: 0) { events.append(event) }
        XCTAssertEqual(events, [.closing, .ignored])
    }

    func testBlinksCloserThanTheCooldownClickOnce() {
        var detector: EyeBlinkDetector = EyeBlinkDetector(length: .quick)
        XCTAssertEqual(blink(&detector, from: 0, closedFor: 0.1), [.closing, .click])
        XCTAssertEqual(blink(&detector, from: 0.2, closedFor: 0.1), [.closing, .ignored])
        XCTAssertEqual(blink(&detector, from: 1, closedFor: 0.1), [.closing, .click])
    }

    func testEngineFreezesDuringABlinkAndClicksWhereTheUserWasLooking() {
        var engine: EyeCursorEngine = EyeCursorEngine(smoothing: .quick, blinkLength: .deliberate)
        var t: Double = 0
        for _ in 0..<60 {
            XCTAssertNil(engine.update(time: t, raw: EyePoint(100, 200), blinkLeft: 0, blinkRight: 0, screen: screen))
            t += 1.0 / 60
        }
        assertNear(engine.cursor, EyePoint(100, 200), accuracy: 1)
        var clicks: [EyeCursorEvent] = []
        for _ in 0..<24 {
            if let event = engine.update(time: t, raw: EyePoint(100, 500), blinkLeft: 0.9, blinkRight: 0.9, screen: screen) { clicks.append(event) }
            t += 1.0 / 60
        }
        XCTAssertTrue(engine.frozen)
        assertNear(engine.cursor, EyePoint(100, 200), accuracy: 1)
        if let event = engine.update(time: t, raw: EyePoint(100, 500), blinkLeft: 0, blinkRight: 0, screen: screen) { clicks.append(event) }
        XCTAssertEqual(clicks.count, 1)
        guard case .click(let point)? = clicks.first else { return XCTFail("no click") }
        assertNear(point, EyePoint(100, 200), accuracy: 1)
    }

    func testEngineAppliesCalibrationAndClampsToTheScreen() {
        var engine: EyeCursorEngine = EyeCursorEngine(calibration: EyeCalibration(a: -1, b: 0, c: 400, d: 0, e: 1, f: 0))
        _ = engine.update(time: 0, raw: EyePoint(100, 2000), blinkLeft: 0, blinkRight: 0, screen: screen)
        XCTAssertEqual(engine.cursor, EyePoint(300, 800))
    }

    func testCalibrationRunWalksNineTargetsAndFitsThem() {
        var run: EyeCalibrationRun = EyeCalibrationRun(screen: screen)
        XCTAssertEqual(run.targets.count, 9)
        XCTAssertEqual(run.targets.first, EyePoint(48, 96))
        let truth: EyeCalibration = EyeCalibration(a: 0.8, b: 0, c: 30, d: 0, e: 1.1, f: -40)
        var t: Double = 0
        while let target = run.target {
            let raw: EyePoint = EyePoint((target.x - 30) / 0.8, (target.y + 40) / 1.1)
            run.update(time: t, raw: raw)
            t += 1.0 / 30
        }
        XCTAssertTrue(run.finished)
        XCTAssertEqual(run.progress, 1)
        assertNear(run.calibration?.apply(EyePoint(100, 100)), truth.apply(EyePoint(100, 100)), accuracy: 1e-6)
    }

    func testCalibrationRunWithoutAFaceHasNoResult() {
        var run: EyeCalibrationRun = EyeCalibrationRun(screen: screen)
        var t: Double = 0
        while run.target != nil {
            run.update(time: t, raw: nil)
            t += 0.1
        }
        XCTAssertNil(run.calibration)
    }

    private func grid(_ xs: [Double], _ ys: [Double]) -> [EyePoint] {
        ys.flatMap { (y: Double) in xs.map { EyePoint($0, y) } }
    }

    private func dots(_ truth: EyeCalibration, raw: [EyePoint]) -> [EyeCalibrationSample] {
        raw.map { EyeCalibrationSample(raw: $0, target: truth.apply($0), source: .dot) }
    }

    func testCurvedFitFollowsABentMapping() {
        let truth: EyeCalibration = EyeCalibration(a: 1.1, b: 0.05, c: -20, d: -0.03, e: 0.95, f: 15, qx: [40, -10, 5], qy: [-8, 25, 30])
        let raw: [EyePoint] = grid([0, 120, 240, 360, 480], [0, 200, 400, 600, 800])
        let affine: EyeCalibration? = EyeCalibration.fit(raw: raw, targets: raw.map(truth.apply))
        let curved: EyeCalibration? = EyeCalibration.fit(raw: raw, targets: raw.map(truth.apply), curved: true)
        let probe: EyePoint = EyePoint(420, 700)
        let affineMiss: Double = affine?.apply(probe).distance(to: truth.apply(probe)) ?? .infinity
        let curvedMiss: Double = curved?.apply(probe).distance(to: truth.apply(probe)) ?? .infinity
        XCTAssertLessThan(curvedMiss, 2)
        XCTAssertLessThan(curvedMiss, affineMiss)
        XCTAssertEqual(EyeCalibration.decode(curved?.encoded)?.qx?.count, 3)
    }

    func testWeightedFitIgnoresAZeroWeightPoint() {
        let truth: EyeCalibration = EyeCalibration(a: 0.9, b: 0, c: 10, d: 0, e: 1.2, f: -5)
        var raw: [EyePoint] = grid([0, 200, 400], [0, 400, 800])
        var targets: [EyePoint] = raw.map(truth.apply)
        raw.append(EyePoint(100, 100))
        targets.append(EyePoint(900, -400))
        let fitted: EyeCalibration? = EyeCalibration.fit(raw: raw, targets: targets, weights: Array(repeating: 1, count: 9) + [0])
        assertNear(fitted?.apply(EyePoint(300, 300)), truth.apply(EyePoint(300, 300)), accuracy: 1e-6)
    }

    func testLiveCalibrationShrugsOffAStrayDot() {
        let truth: EyeCalibration = EyeCalibration(a: 0.8, b: 0.02, c: 30, d: 0.01, e: 1.1, f: -40)
        var samples: [EyeCalibrationSample] = dots(truth, raw: grid([0, 200, 400], [0, 400, 800]))
        samples[4].target = EyePoint(samples[4].target.x + 150, samples[4].target.y - 120)
        let live: EyeLiveCalibration = EyeLiveCalibration(dots: samples)
        XCTAssertEqual(live.model, .affine)
        XCTAssertTrue(live.calibrated)
        XCTAssertLessThan(live.calibration.apply(EyePoint(300, 600)).distance(to: truth.apply(EyePoint(300, 600))), 8)
        let plain: EyeCalibration? = EyeCalibration.fit(raw: samples.map(\.raw), targets: samples.map(\.target))
        XCTAssertGreaterThan(plain?.apply(EyePoint(200, 400)).distance(to: truth.apply(EyePoint(200, 400))) ?? 0, 15)
        XCTAssertNotNil(live.error)
    }

    func testLiveCalibrationLearnsADriftFromClicks() {
        let truth: EyeCalibration = EyeCalibration(a: 1, b: 0, c: 0, d: 0, e: 1, f: 0)
        var live: EyeLiveCalibration = EyeLiveCalibration(dots: dots(truth, raw: grid([40, 200, 360], [80, 400, 720])))
        XCTAssertEqual(live.model, .affine)
        let moved: EyePoint = EyePoint(28, -22)
        let spots: [EyePoint] = [EyePoint(100, 300), EyePoint(300, 320), EyePoint(200, 500), EyePoint(120, 600), EyePoint(280, 150), EyePoint(220, 260)]
        for spot in spots {
            XCTAssertTrue(live.learn(EyeCalibrationSample(raw: EyePoint(spot.x - moved.x, spot.y - moved.y), target: spot, source: .click)))
        }
        let probe: EyePoint = EyePoint(210, 410)
        let before: Double = EyePoint(probe.x - moved.x, probe.y - moved.y).distance(to: probe)
        let after: Double = live.calibration.apply(EyePoint(probe.x - moved.x, probe.y - moved.y)).distance(to: probe)
        XCTAssertLessThan(after, before / 3)
        XCTAssertEqual(live.live.count, spots.count)
        XCTAssertFalse(live.learn(EyeCalibrationSample(raw: EyePoint(0, 0), target: EyePoint(390, 840), source: .click)))
        XCTAssertEqual(live.live.count, spots.count)
        XCTAssertNotEqual(live.drift, EyePoint(0, 0))
    }

    func testLiveCalibrationWithoutDotsCorrectsAnOffset() {
        var live: EyeLiveCalibration = EyeLiveCalibration()
        XCTAssertEqual(live.model, .rough)
        XCTAssertFalse(live.calibrated)
        live.learn(EyeCalibrationSample(raw: EyePoint(100, 100), target: EyePoint(160, 130), source: .click))
        live.learn(EyeCalibrationSample(raw: EyePoint(300, 500), target: EyePoint(360, 530), source: .click))
        XCTAssertEqual(live.model, .offset)
        let shifted: EyePoint = live.calibration.apply(EyePoint(200, 300))
        XCTAssertGreaterThan(shifted.x, 230)
        XCTAssertGreaterThan(shifted.y, 315)
    }

    func testLiveCalibrationReadsTheOldSavedFormat() {
        let legacy: EyeCalibration = EyeCalibration(a: 1, b: 0, c: 12, d: 0, e: 1, f: -7)
        let restored: EyeLiveCalibration? = EyeLiveCalibration.decode(legacy.encoded)
        XCTAssertEqual(restored?.calibration, legacy)
        XCTAssertTrue(restored?.calibrated ?? false)
        var live: EyeLiveCalibration = EyeLiveCalibration(dots: dots(.identity, raw: grid([0, 200, 400], [0, 400, 800])))
        live.learn(EyeCalibrationSample(raw: EyePoint(50, 50), target: EyePoint(60, 55), source: .dwell))
        XCTAssertEqual(EyeLiveCalibration.decode(live.encoded), live)
        XCTAssertNil(EyeLiveCalibration.decode("nope"))
    }

    private func button(_ id: Int, _ x: Double, _ y: Double, _ w: Double = 44, _ h: Double = 44) -> EyeTarget {
        EyeTarget(id: id, frame: EyeRect(x: x, y: y, width: w, height: h))
    }

    func testRectDistanceAndClamp() {
        let rect: EyeRect = EyeRect(x: 10, y: 20, width: 100, height: 40)
        XCTAssertEqual(rect.distance(to: EyePoint(50, 30)), 0)
        XCTAssertEqual(rect.distance(to: EyePoint(113, 64)), 5, accuracy: 1e-9)
        XCTAssertEqual(rect.clamp(EyePoint(0, 100), inset: 6), EyePoint(16, 54))
        XCTAssertEqual(rect.overlap(rect), 1, accuracy: 1e-9)
        XCTAssertEqual(rect.overlap(EyeRect(x: 500, y: 0, width: 1, height: 1)), 0)
    }

    func testSnapperPicksTheNearestAndTheSmallestInside() {
        var snapper: EyeSnapper = EyeSnapper()
        let card: EyeTarget = button(1, 0, 0, 380, 200)
        let small: EyeTarget = button(2, 100, 50)
        let far: EyeTarget = button(3, 300, 600)
        XCTAssertEqual(snapper.update(time: 0, gaze: EyePoint(110, 60), sigma: 30, targets: [card, small, far])?.id, 2)
        snapper.reset()
        XCTAssertEqual(snapper.update(time: 0, gaze: EyePoint(320, 570), sigma: 30, targets: [card, small, far])?.id, 3)
        snapper.reset()
        XCTAssertNil(snapper.update(time: 0, gaze: EyePoint(100, 450), sigma: 30, targets: [card, small, far]))
    }

    func testSnapperHoldsOnUntilTheEyesClearlyMove() {
        var snapper: EyeSnapper = EyeSnapper()
        let left: EyeTarget = button(1, 0, 0, 80, 44)
        let right: EyeTarget = button(2, 90, 0, 80, 44)
        XCTAssertEqual(snapper.update(time: 0, gaze: EyePoint(40, 20), sigma: 25, targets: [left, right])?.id, 1)
        XCTAssertEqual(snapper.update(time: 0.02, gaze: EyePoint(95, 20), sigma: 25, targets: [left, right])?.id, 1)
        XCTAssertEqual(snapper.update(time: 0.04, gaze: EyePoint(140, 20), sigma: 25, targets: [left, right])?.id, 1)
        XCTAssertEqual(snapper.update(time: 0.2, gaze: EyePoint(140, 20), sigma: 25, targets: [left, right])?.id, 2)
        let shifted: EyeTarget = EyeTarget(id: 9, frame: EyeRect(x: 92, y: 2, width: 80, height: 44))
        XCTAssertEqual(snapper.update(time: 0.3, gaze: EyePoint(140, 20), sigma: 25, targets: [left, shifted])?.id, 9)
        XCTAssertNil(snapper.update(time: 0.4, gaze: EyePoint(140, 600), sigma: 25, targets: [left, shifted]))
    }

    func testPreciseTargetsClickWhereTheEyesAre() {
        let slider: EyeTarget = EyeTarget(id: 1, frame: EyeRect(x: 20, y: 100, width: 300, height: 30), precise: true)
        XCTAssertEqual(slider.clickPoint(for: EyePoint(250, 90)), EyePoint(250, 106))
        XCTAssertEqual(button(2, 0, 0).clickPoint(for: EyePoint(3, 3)), EyePoint(22, 22))
    }

    func testEngineSnapsClicksAndLearnsFromThem() {
        var engine: EyeCursorEngine = EyeCursorEngine(smoothing: .quick, blinkLength: .deliberate)
        engine.accuracy = 25
        engine.targets = [button(7, 100, 200), button(8, 250, 200)]
        var t: Double = 0
        for _ in 0..<90 {
            XCTAssertNil(engine.update(time: t, raw: EyePoint(140, 235), blinkLeft: 0, blinkRight: 0, screen: screen))
            t += 1.0 / 60
        }
        XCTAssertEqual(engine.snapped?.id, 7)
        XCTAssertTrue(engine.fixating)
        XCTAssertGreaterThan(engine.confidence, 0.5)
        assertNear(engine.display, EyePoint(140 + (122 - 140) * 0.6, 235 + (222 - 235) * 0.6), accuracy: 1)
        let dwell: [EyeCalibrationSample] = engine.takeSamples()
        XCTAssertEqual(dwell.map(\.source), [.dwell])
        assertNear(dwell.first?.raw, EyePoint(140, 235), accuracy: 0.01)
        assertNear(dwell.first?.target, EyePoint(122, 222), accuracy: 0.01)
        var clicks: [EyeCursorEvent] = []
        for _ in 0..<24 {
            if let event = engine.update(time: t, raw: EyePoint(140, 500), blinkLeft: 0.9, blinkRight: 0.9, screen: screen) { clicks.append(event) }
            t += 1.0 / 60
        }
        if let event = engine.update(time: t, raw: EyePoint(140, 500), blinkLeft: 0, blinkRight: 0, screen: screen) { clicks.append(event) }
        XCTAssertEqual(clicks, [.click(EyePoint(122, 222))])
        XCTAssertEqual(engine.takeSamples().map(\.source), [.click])
        engine.learning = false
        engine.snapping = false
        XCTAssertNil(engine.snapped)
    }

    func testCalibrationRunDropsStrayGlances() {
        var run: EyeCalibrationRun = EyeCalibrationRun(screen: screen)
        var t: Double = 0
        var frame: Int = 0
        while let target = run.target {
            let stray: Bool = frame % 7 == 0
            run.update(time: t, raw: stray ? EyePoint(target.x + 300, target.y - 250) : EyePoint(target.x + (frame % 2 == 0 ? 2 : -2), target.y))
            t += 1.0 / 30
            frame += 1
        }
        XCTAssertEqual(run.samples.count, 9)
        for sample in run.samples {
            XCTAssertLessThan(sample.raw.distance(to: sample.target), 3)
        }
        XCTAssertNotNil(run.live)
    }

    func testSettingsReadBackWithDefaults() {
        XCTAssertEqual(EyeSmoothing.of(nil), .balanced)
        XCTAssertEqual(EyeSmoothing.of("steady"), .steady)
        XCTAssertEqual(EyeBlinkLength.of("bogus"), .deliberate)
        XCTAssertEqual(EyeBlinkLength.of("long").minSeconds, 0.5)
    }
}

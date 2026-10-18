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

    func testSettingsReadBackWithDefaults() {
        XCTAssertEqual(EyeSmoothing.of(nil), .balanced)
        XCTAssertEqual(EyeSmoothing.of("steady"), .steady)
        XCTAssertEqual(EyeBlinkLength.of("bogus"), .deliberate)
        XCTAssertEqual(EyeBlinkLength.of("long").minSeconds, 0.5)
    }
}

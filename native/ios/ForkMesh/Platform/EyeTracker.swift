import ARKit
import AVFoundation
import FMBase
import FMTools
import Observation
import UIKit

@MainActor
@Observable
final class EyeTracker: NSObject {
    static let shared: EyeTracker = EyeTracker()
    static let scanSeconds: Double = 0.25
    static let progressStep: Double = 0.04

    private(set) var running: Bool = false
    private(set) var faceVisible: Bool = false
    private(set) var problem: String?
    private(set) var calibrated: Bool = false
    private(set) var calibrating: Bool = false
    private(set) var calibrationTarget: EyePoint?
    private(set) var calibrationStep: Int = 0
    private(set) var calibrationDotProgress: Double = 0
    private(set) var clicks: Int = 0
    private(set) var accuracy: Double?
    private(set) var learned: Int = 0
    var smoothing: EyeSmoothing = .defaultValue {
        didSet {
            engine.smoothing = smoothing
            prefs?.set(smoothing.rawValue, forKey: EyeControlPrefs.smoothing)
        }
    }
    var blinkLength: EyeBlinkLength = .defaultValue {
        didSet {
            engine.blinkLength = blinkLength
            prefs?.set(blinkLength.rawValue, forKey: EyeControlPrefs.blinkLength)
        }
    }
    var snapping: Bool = true {
        didSet {
            engine.snapping = snapping
            if !snapping { engine.targets = [] }
            scannedAt = -.infinity
            prefs?.set(snapping, forKey: EyeControlPrefs.snapping)
        }
    }
    var learning: Bool = true {
        didSet {
            engine.learning = learning
            prefs?.set(learning, forKey: EyeControlPrefs.learning)
        }
    }

    @ObservationIgnored private var session: ARSession?
    @ObservationIgnored private var engine: EyeCursorEngine = EyeCursorEngine()
    @ObservationIgnored private var run: EyeCalibrationRun?
    @ObservationIgnored private var live: EyeLiveCalibration = EyeLiveCalibration()
    @ObservationIgnored private var scannedAt: Double = -.infinity
    @ObservationIgnored private let overlay: EyeCursorOverlay = EyeCursorOverlay()
    @ObservationIgnored private var orientation: UIInterfaceOrientation = .portrait
    @ObservationIgnored private var idleBefore: Bool = false

    private var prefs: KeyValueStore? { Services.get(KeyValueStore.self) }

    override init() {
        super.init()
        if let prefs = Services.get(KeyValueStore.self) {
            smoothing = EyeSmoothing.of(prefs.string(forKey: EyeControlPrefs.smoothing))
            blinkLength = EyeBlinkLength.of(prefs.string(forKey: EyeControlPrefs.blinkLength))
            snapping = prefs.bool(forKey: EyeControlPrefs.snapping) ?? true
            learning = prefs.bool(forKey: EyeControlPrefs.learning) ?? true
            engine = EyeCursorEngine(smoothing: smoothing, blinkLength: blinkLength)
            engine.snapping = snapping
            engine.learning = learning
            adopt(EyeLiveCalibration.decode(prefs.string(forKey: EyeControlPrefs.calibration)) ?? EyeLiveCalibration(), save: false)
        }
    }

    var supported: Bool { ARFaceTrackingConfiguration.isSupported }

    var cameraDenied: Bool {
        let status: AVAuthorizationStatus = AVCaptureDevice.authorizationStatus(for: .video)
        return status == .denied || status == .restricted
    }

    func start() {
        guard !running else { return }
        guard supported else {
            problem = "This device has no face-tracking front camera. Eye Control needs an iPhone or iPad with Face ID or an A12 chip or later."
            return
        }
        problem = nil
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            begin()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { (granted: Bool) in
                Task { @MainActor in
                    if granted { EyeTracker.shared.begin() } else { EyeTracker.shared.denied() }
                }
            }
        default:
            denied()
        }
    }

    func stop() {
        session?.pause()
        session = nil
        guard running else { return }
        running = false
        faceVisible = false
        endCalibration()
        engine.reset()
        overlay.hide()
        UIApplication.shared.isIdleTimerDisabled = idleBefore
    }

    func calibrate() {
        start()
        let screen: EyeScreen = currentScreen()
        run = EyeCalibrationRun(screen: screen)
        calibrating = true
        calibrationStep = 0
        calibrationDotProgress = 0
        calibrationTarget = run?.target
        overlay.setCursorHidden(true)
    }

    func cancelCalibration() {
        endCalibration()
    }

    func resetCalibration() {
        adopt(EyeLiveCalibration(), save: false)
        prefs?.remove(EyeControlPrefs.calibration)
    }

    private func adopt(_ next: EyeLiveCalibration, save: Bool) {
        live = next
        engine.calibration = next.calibration
        engine.accuracy = next.error
        calibrated = next.calibrated
        accuracy = next.error
        learned = next.live.count
        if save { prefs?.set(next.encoded, forKey: EyeControlPrefs.calibration) }
    }

    private func absorb(_ samples: [EyeCalibrationSample]) {
        guard learning, !samples.isEmpty else { return }
        var next: EyeLiveCalibration = live
        var changed: Bool = false
        for sample in samples {
            if next.learn(sample) { changed = true }
        }
        if changed { adopt(next, save: true) }
    }

    private func endCalibration() {
        run = nil
        calibrating = false
        calibrationTarget = nil
        overlay.setCursorHidden(false)
    }

    private func begin() {
        guard !running else { return }
        let configuration: ARFaceTrackingConfiguration = ARFaceTrackingConfiguration()
        configuration.maximumNumberOfTrackedFaces = 1
        configuration.isLightEstimationEnabled = false
        let session: ARSession = ARSession()
        session.delegate = self
        session.delegateQueue = DispatchQueue.main
        session.run(configuration, options: [.resetTracking, .removeExistingAnchors])
        self.session = session
        engine.reset()
        running = true
        idleBefore = UIApplication.shared.isIdleTimerDisabled
        UIApplication.shared.isIdleTimerDisabled = true
        overlay.show(in: EyeClicker.activeScene(), onStop: { EyeTracker.shared.stop() })
        overlay.setCursorHidden(calibrating)
    }

    private func denied() {
        problem = "Camera access is off for ForkMesh. Turn it on in Settings to use Eye Control."
    }

    private func currentScreen() -> EyeScreen {
        let scene: UIWindowScene? = EyeClicker.activeScene()
        let bounds: CGRect = scene?.coordinateSpace.bounds ?? CGRect(x: 0, y: 0, width: 390, height: 844)
        orientation = scene?.effectiveGeometry.interfaceOrientation ?? .portrait
        let edge: EyeCameraEdge
        switch orientation {
        case .portraitUpsideDown: edge = .bottom
        case .landscapeLeft: edge = .right
        case .landscapeRight: edge = .left
        default: edge = .top
        }
        let inch: Double = UIDevice.current.userInterfaceIdiom == .pad ? EyeScreen.tabletPointsPerInch : EyeScreen.phonePointsPerInch
        return EyeScreen(width: Double(bounds.width), height: Double(bounds.height), pointsPerInch: inch, cameraEdge: edge)
    }

    fileprivate func handle(camera: ARCamera, face: ARFaceAnchor?, time: Double) {
        guard running else { return }
        let screen: EyeScreen = currentScreen()
        guard let face, face.isTracked else {
            if faceVisible { faceVisible = false }
            overlay.update(cursor: engine.display, target: nil, confidence: 0, closing: false, tracked: false)
            return
        }
        if !faceVisible { faceVisible = true }
        let view: EyeTransform = EyeTransform(camera.viewMatrix(for: orientation))
        let raw: EyePoint? = EyeGaze.screenPoint(
            face: view * EyeTransform(face.transform),
            leftEye: EyeTransform(face.leftEyeTransform),
            rightEye: EyeTransform(face.rightEyeTransform),
            screen: screen)
        if var current = run {
            current.update(time: time, raw: raw)
            run = current
            if current.index != calibrationStep {
                calibrationStep = current.index
                calibrationTarget = current.target
                calibrationDotProgress = 0
            } else if abs(current.dotProgress - calibrationDotProgress) >= Self.progressStep || (current.dotProgress >= 1 && calibrationDotProgress < 1) {
                calibrationDotProgress = current.dotProgress
            }
            if current.finished { finishCalibration(current) }
            return
        }
        let left: Double = face.blendShapes[.eyeBlinkLeft]?.doubleValue ?? 0
        let right: Double = face.blendShapes[.eyeBlinkRight]?.doubleValue ?? 0
        if snapping, time - scannedAt >= Self.scanSeconds {
            scannedAt = time
            engine.targets = EyeClicker.targets() + [overlay.stopTarget()].compactMap { $0 }
        }
        let event: EyeCursorEvent? = engine.update(time: time, raw: raw, blinkLeft: left, blinkRight: right, screen: screen)
        absorb(engine.takeSamples())
        overlay.update(cursor: engine.display, target: engine.snapped, confidence: engine.confidence, closing: engine.frozen, tracked: raw != nil)
        if case .click(let point)? = event {
            click(at: CGPoint(x: point.x, y: point.y))
        }
    }

    private func finishCalibration(_ finished: EyeCalibrationRun) {
        if let fitted = finished.live {
            adopt(fitted, save: true)
            engine.reset()
            FmHaptics.success()
        } else {
            problem = "Calibration didn't see your eyes on enough dots. Hold the phone at reading distance with your face in view, then try again."
            FmHaptics.warning()
        }
        endCalibration()
    }

    private func click(at point: CGPoint) {
        if overlay.stopContains(point) {
            stop()
            return
        }
        let hit: Bool = EyeClicker.click(at: point)
        overlay.pulse(at: point, hit: hit)
        if hit {
            clicks += 1
            FmHaptics.tap()
        }
    }
}

extension EyeTracker: ARSessionDelegate {
    nonisolated func session(_ session: ARSession, didUpdate frame: ARFrame) {
        let camera: ARCamera = frame.camera
        let face: ARFaceAnchor? = frame.anchors.lazy.compactMap { $0 as? ARFaceAnchor }.first
        let time: Double = frame.timestamp
        MainActor.assumeIsolated {
            self.handle(camera: camera, face: face, time: time)
        }
    }

    nonisolated func session(_ session: ARSession, didFailWithError error: Error) {
        let message: String = error.localizedDescription
        MainActor.assumeIsolated {
            self.stop()
            self.problem = message
        }
    }
}

extension EyeTransform {
    init(_ m: simd_float4x4) {
        let columns: [simd_float4] = [m.columns.0, m.columns.1, m.columns.2, m.columns.3]
        self.init(columns: columns.flatMap { [Double($0.x), Double($0.y), Double($0.z), Double($0.w)] })
    }
}

final class EyePassthroughWindow: UIWindow {
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? {
        let hit: UIView? = super.hitTest(point, with: event)
        return hit is UIControl ? hit : nil
    }
}

@MainActor
final class EyeCursorOverlay {
    static let cursorSize: CGFloat = 30
    static let highlightPadding: CGFloat = 4
    static let highlightRadius: CGFloat = 12

    private var window: EyePassthroughWindow?
    private let cursor: UIView = UIView()
    private let highlight: UIView = UIView()
    private var highlighted: EyeTarget?
    private let stopButton: UIButton = UIButton(type: .system)
    private var cursorHidden: Bool = false

    func show(in scene: UIWindowScene?, onStop: @escaping () -> Void) {
        guard window == nil, let scene else { return }
        let window: EyePassthroughWindow = EyePassthroughWindow(windowScene: scene)
        window.windowLevel = UIWindow.Level.alert
        window.backgroundColor = .clear
        let root: UIViewController = UIViewController()
        root.view.backgroundColor = .clear
        window.rootViewController = root

        highlight.layer.borderWidth = 2.5
        highlight.layer.borderColor = UIColor.systemBlue.cgColor
        highlight.backgroundColor = UIColor.systemBlue.withAlphaComponent(0.12)
        highlight.isUserInteractionEnabled = false
        highlight.alpha = 0
        root.view.addSubview(highlight)

        cursor.frame = CGRect(x: 0, y: 0, width: Self.cursorSize, height: Self.cursorSize)
        cursor.layer.cornerRadius = Self.cursorSize / 2
        cursor.layer.borderWidth = 3
        cursor.layer.borderColor = UIColor.systemBlue.cgColor
        cursor.backgroundColor = UIColor.systemBlue.withAlphaComponent(0.18)
        cursor.layer.shadowColor = UIColor.black.cgColor
        cursor.layer.shadowOpacity = 0.35
        cursor.layer.shadowRadius = 3
        cursor.layer.shadowOffset = .zero
        cursor.isUserInteractionEnabled = false
        cursor.alpha = 0
        root.view.addSubview(cursor)

        var config: UIButton.Configuration = UIButton.Configuration.filled()
        config.title = "Stop eye control"
        config.image = UIImage(systemName: "eye.slash")
        config.imagePadding = 6
        config.cornerStyle = .capsule
        config.buttonSize = .mini
        config.baseBackgroundColor = UIColor.secondarySystemBackground.withAlphaComponent(0.92)
        config.baseForegroundColor = UIColor.label
        stopButton.configuration = config
        stopButton.accessibilityIdentifier = "eye-control-overlay-stop"
        stopButton.addAction(UIAction { _ in onStop() }, for: .primaryActionTriggered)
        stopButton.translatesAutoresizingMaskIntoConstraints = false
        root.view.addSubview(stopButton)
        NSLayoutConstraint.activate([
            stopButton.centerXAnchor.constraint(equalTo: root.view.centerXAnchor),
            stopButton.bottomAnchor.constraint(equalTo: root.view.safeAreaLayoutGuide.bottomAnchor, constant: -64),
        ])

        window.isHidden = false
        self.window = window
    }

    func hide() {
        window?.isHidden = true
        window?.rootViewController = nil
        window = nil
        cursor.removeFromSuperview()
        highlight.removeFromSuperview()
        highlighted = nil
        stopButton.removeFromSuperview()
    }

    func setCursorHidden(_ hidden: Bool) {
        cursorHidden = hidden
        cursor.isHidden = hidden
        highlight.isHidden = hidden
        stopButton.isHidden = hidden
    }

    func update(cursor point: EyePoint?, target: EyeTarget?, confidence: Double, closing: Bool, tracked: Bool) {
        guard !cursorHidden, let point else {
            cursor.alpha = 0
            show(target: nil, confidence: 0, closing: false)
            return
        }
        cursor.center = CGPoint(x: point.x, y: point.y)
        cursor.alpha = tracked ? 1 : 0.35
        let scale: CGFloat = closing ? 0.6 : (target == nil ? 1 : 0.75)
        cursor.transform = CGAffineTransform(scaleX: scale, y: scale)
        cursor.backgroundColor = UIColor.systemBlue.withAlphaComponent(closing ? 0.7 : 0.18)
        show(target: tracked ? target : nil, confidence: confidence, closing: closing)
    }

    func stopTarget() -> EyeTarget? {
        guard let root = window?.rootViewController?.view, !stopButton.isHidden else { return nil }
        let frame: CGRect = stopButton.convert(stopButton.bounds, to: root)
        return EyeTarget(id: -1, frame: EyeRect(x: Double(frame.minX), y: Double(frame.minY), width: Double(frame.width), height: Double(frame.height)))
    }

    private func show(target: EyeTarget?, confidence: Double, closing: Bool) {
        guard let target else {
            if highlighted != nil {
                highlighted = nil
                UIView.animate(withDuration: 0.15, delay: 0, options: [.beginFromCurrentState, .allowUserInteraction]) {
                    self.highlight.alpha = 0
                }
            }
            return
        }
        let frame: CGRect = CGRect(x: target.frame.x, y: target.frame.y, width: target.frame.width, height: target.frame.height)
            .insetBy(dx: -Self.highlightPadding, dy: -Self.highlightPadding)
        let alpha: CGFloat = closing ? 1 : CGFloat(0.5 + 0.5 * min(max(confidence, 0), 1))
        highlight.layer.cornerRadius = min(Self.highlightRadius, frame.height / 2)
        highlight.backgroundColor = UIColor.systemBlue.withAlphaComponent(closing ? 0.28 : 0.12)
        if highlighted == nil {
            highlight.layer.removeAllAnimations()
            place(frame)
            highlight.transform = CGAffineTransform(scaleX: 1.08, y: 1.08)
            UIView.animate(withDuration: 0.12, delay: 0, options: [.beginFromCurrentState, .allowUserInteraction, .curveEaseOut]) {
                self.highlight.transform = .identity
                self.highlight.alpha = alpha
            }
        } else if highlighted?.frame != target.frame {
            UIView.animate(withDuration: 0.12, delay: 0, options: [.beginFromCurrentState, .allowUserInteraction, .curveEaseOut]) {
                self.place(frame)
                self.highlight.alpha = alpha
            }
        } else {
            highlight.alpha = alpha
        }
        highlighted = target
    }

    private func place(_ frame: CGRect) {
        highlight.bounds = CGRect(origin: .zero, size: frame.size)
        highlight.center = CGPoint(x: frame.midX, y: frame.midY)
    }

    func stopContains(_ point: CGPoint) -> Bool {
        guard let root = window?.rootViewController?.view, !stopButton.isHidden else { return false }
        return stopButton.convert(stopButton.bounds, to: root).insetBy(dx: -8, dy: -8).contains(point)
    }

    func pulse(at point: CGPoint, hit: Bool) {
        guard let root = window?.rootViewController?.view else { return }
        let ring: UIView = UIView(frame: CGRect(x: 0, y: 0, width: Self.cursorSize, height: Self.cursorSize))
        ring.center = point
        ring.layer.cornerRadius = Self.cursorSize / 2
        ring.layer.borderWidth = 2
        ring.layer.borderColor = (hit ? UIColor.systemBlue : UIColor.systemGray).cgColor
        ring.isUserInteractionEnabled = false
        root.addSubview(ring)
        UIView.animate(withDuration: 0.35, animations: {
            ring.transform = CGAffineTransform(scaleX: 2.2, y: 2.2)
            ring.alpha = 0
        }, completion: { _ in ring.removeFromSuperview() })
    }
}

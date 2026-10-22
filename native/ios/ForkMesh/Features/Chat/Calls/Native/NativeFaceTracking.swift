#if canImport(WebRTC) && canImport(ARKit)
import ARKit
import AVFoundation
import FMChat
import Foundation
import UIKit

final class NativeFaceTrackingSource: NSObject, ARSessionDelegate, @unchecked Sendable {
    static var supported: Bool { ARFaceTrackingConfiguration.isSupported }

    private static let sessions = NSLock()
    private static var nextSession = 0

    var onFrame: ((CVPixelBuffer, Int, Int64, NativeFaceTrackingFrame) -> Void)?
    var onFailure: (() -> Void)?

    private let queue = DispatchQueue(label: "com.forkmesh.native-face-tracking", qos: .userInteractive)
    private let lock = NSLock()
    private var session: ARSession?
    private var configuration: ARFaceTrackingConfiguration?
    private var sessionId = 0
    private var triangles: [Int] = []
    private var depth: (buffer: CVPixelBuffer, time: TimeInterval)?
    private var rotation = 90
    private var orientationWatch: NSObjectProtocol?

    @MainActor
    func start(width: Int, height: Int, fps: Int) -> Bool {
        guard Self.supported else { return false }
        let made = ARFaceTrackingConfiguration()
        made.maximumNumberOfTrackedFaces = 1
        made.isLightEstimationEnabled = false
        let formats = ARFaceTrackingConfiguration.supportedVideoFormats
        let choices = formats.map {
            CallFaceVideoFormat(width: Int($0.imageResolution.width), height: Int($0.imageResolution.height), fps: $0.framesPerSecond)
        }
        if let index = CallFaceCapture.pick(choices, width: width, height: height, fps: fps) { made.videoFormat = formats[index] }
        Self.sessions.lock()
        Self.nextSession += 1
        let id = Self.nextSession
        Self.sessions.unlock()
        let running = ARSession()
        running.delegate = self
        running.delegateQueue = queue
        lock.lock()
        session = running
        configuration = made
        sessionId = id
        triangles = []
        depth = nil
        lock.unlock()
        followOrientation()
        running.run(made, options: [.resetTracking, .removeExistingAnchors])
        return true
    }

    @MainActor
    func stop() {
        lock.lock()
        let running = session
        session = nil
        configuration = nil
        depth = nil
        lock.unlock()
        running?.pause()
        if let orientationWatch { NotificationCenter.default.removeObserver(orientationWatch) }
        orientationWatch = nil
        UIDevice.current.endGeneratingDeviceOrientationNotifications()
    }

    @MainActor
    private func followOrientation() {
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        noteOrientation()
        if orientationWatch == nil {
            orientationWatch = NotificationCenter.default.addObserver(forName: UIDevice.orientationDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.noteOrientation() }
            }
        }
    }

    @MainActor
    private func noteOrientation() {
        let raw = UIDevice.current.orientation.rawValue
        lock.lock()
        rotation = CallFaceCapture.rotation(deviceOrientation: raw, front: true, last: rotation)
        lock.unlock()
    }

    func session(_ session: ARSession, didUpdate frame: ARFrame) {
        lock.lock()
        let current = session === self.session
        let id = sessionId
        let turn = rotation
        if current, let captured = frame.capturedDepthData {
            let converted = captured.depthDataType == kCVPixelFormatType_DepthFloat32 ? captured : captured.converting(toDepthDataType: kCVPixelFormatType_DepthFloat32)
            depth = (converted.depthDataMap, frame.capturedDepthDataTimestamp)
        }
        let recent = depth
        if current && triangles.isEmpty, let face = frame.anchors.lazy.compactMap({ $0 as? ARFaceAnchor }).first {
            let geometry = face.geometry
            triangles = (0..<(geometry.triangleCount * 3)).map { Int(geometry.triangleIndices[$0]) }
        }
        let topology = triangles
        lock.unlock()
        guard current, let deliver = onFrame else { return }
        let tracking = Self.tracking(frame, session: id, triangles: topology, depth: recent)
        deliver(frame.capturedImage, turn, Int64(frame.timestamp * 1_000_000_000), tracking)
    }

    static func tracking(_ frame: ARFrame, session: Int, triangles: [Int], depth: (buffer: CVPixelBuffer, time: TimeInterval)?) -> NativeFaceTrackingFrame {
        let camera = frame.camera
        let focal = Double(camera.intrinsics[0][0])
        let age = depth.map { (frame.timestamp - $0.time) * 1000 } ?? .infinity
        guard let face = frame.anchors.lazy.compactMap({ $0 as? ARFaceAnchor }).first, face.isTracked, !triangles.isEmpty else {
            return NativeFaceTrackingFrame(session: session, vertices: [], triangles: triangles, shapes: nil, focal: focal, faceMetres: 0, depth: depth?.buffer, depthAgeMs: age)
        }
        let size = camera.imageResolution
        let toCamera = simd_mul(simd_inverse(camera.transform), face.transform)
        var vertices: [CallFacePoint] = []
        vertices.reserveCapacity(face.geometry.vertices.count)
        for vertex in face.geometry.vertices {
            let local = SIMD4<Float>(vertex.x, vertex.y, vertex.z, 1)
            let world = simd_mul(face.transform, local)
            let projected = camera.projectPoint(SIMD3(world.x, world.y, world.z), orientation: .landscapeRight, viewportSize: size)
            let seen = simd_mul(toCamera, local)
            vertices.append(CallFacePoint(Double(projected.x), Double(projected.y), Double(-seen.z)))
        }
        func value(_ key: ARFaceAnchor.BlendShapeLocation) -> Double { face.blendShapes[key]?.doubleValue ?? 0 }
        var shapes = CallFaceBlendShapes()
        shapes.eyeBlinkLeft = value(.eyeBlinkLeft)
        shapes.eyeBlinkRight = value(.eyeBlinkRight)
        shapes.eyeLookInLeft = value(.eyeLookInLeft)
        shapes.eyeLookOutLeft = value(.eyeLookOutLeft)
        shapes.eyeLookUpLeft = value(.eyeLookUpLeft)
        shapes.eyeLookDownLeft = value(.eyeLookDownLeft)
        shapes.eyeLookInRight = value(.eyeLookInRight)
        shapes.eyeLookOutRight = value(.eyeLookOutRight)
        shapes.eyeLookUpRight = value(.eyeLookUpRight)
        shapes.eyeLookDownRight = value(.eyeLookDownRight)
        shapes.tongueOut = value(.tongueOut)
        return NativeFaceTrackingFrame(
            session: session, vertices: vertices, triangles: triangles, shapes: shapes, focal: focal,
            faceMetres: Double(-toCamera.columns.3.z), depth: depth?.buffer, depthAgeMs: age)
    }

    func session(_ session: ARSession, didFailWithError error: Error) {
        lock.lock()
        let current = session === self.session
        lock.unlock()
        if current { onFailure?() }
    }

    func sessionWasInterrupted(_ session: ARSession) {}

    func sessionInterruptionEnded(_ session: ARSession) {
        lock.lock()
        let current = session === self.session
        let made = configuration
        lock.unlock()
        guard current, let made else { return }
        session.run(made, options: [.resetTracking, .removeExistingAnchors])
    }
}
#endif

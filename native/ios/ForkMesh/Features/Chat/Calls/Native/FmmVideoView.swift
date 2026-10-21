#if canImport(Fmm)
import AVFoundation
import CoreMedia
import SwiftUI
import UIKit

final class FmmDisplayView: UIView {
    override class var layerClass: AnyClass { AVSampleBufferDisplayLayer.self }

    var displayLayer: AVSampleBufferDisplayLayer { layer as! AVSampleBufferDisplayLayer }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        displayLayer.videoGravity = .resizeAspectFill
    }

    required init?(coder: NSCoder) { nil }
}

final class FmmTileFeed: @unchecked Sendable {
    let peer: String
    private let lock = NSLock()
    private weak var view: FmmDisplayView?
    private var format: CMVideoFormatDescription?
    private var pending: CVPixelBuffer?
    private var scheduled = false

    init(peer: String) {
        self.peer = peer
    }

    @MainActor
    func attach(_ view: FmmDisplayView) {
        lock.withLock { self.view = view }
    }

    func show(_ image: CVPixelBuffer) {
        lock.lock()
        pending = image
        let schedule = !scheduled
        scheduled = true
        lock.unlock()
        guard schedule else { return }
        DispatchQueue.main.async { [weak self] in self?.flush() }
    }

    @MainActor
    private func flush() {
        lock.lock()
        let image = pending
        pending = nil
        scheduled = false
        let target = view
        lock.unlock()
        guard let image, let target, let sample = sample(image) else { return }
        let renderer = target.displayLayer.sampleBufferRenderer
        if renderer.status == .failed { renderer.flush() }
        renderer.enqueue(sample)
    }

    private func sample(_ image: CVPixelBuffer) -> CMSampleBuffer? {
        if format == nil || !CMVideoFormatDescriptionMatchesImageBuffer(format!, imageBuffer: image) {
            var made: CMVideoFormatDescription?
            CMVideoFormatDescriptionCreateForImageBuffer(allocator: nil, imageBuffer: image, formatDescriptionOut: &made)
            format = made
        }
        guard let format else { return nil }
        var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()), decodeTimeStamp: .invalid)
        var out: CMSampleBuffer?
        guard CMSampleBufferCreateReadyWithImageBuffer(
            allocator: nil, imageBuffer: image, formatDescription: format, sampleTiming: &timing, sampleBufferOut: &out) == noErr,
            let out
        else { return nil }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(out, createIfNecessary: true), CFArrayGetCount(attachments) > 0 {
            let first = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(
                first, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(),
                Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        return out
    }
}

struct FmmVideoView: UIViewRepresentable {
    let feed: FmmTileFeed
    var mirrored = false

    func makeUIView(context: Context) -> FmmDisplayView {
        let view = FmmDisplayView(frame: .zero)
        feed.attach(view)
        return view
    }

    func updateUIView(_ view: FmmDisplayView, context: Context) {
        view.transform = mirrored ? CGAffineTransform(scaleX: -1, y: 1) : .identity
        feed.attach(view)
    }
}
#endif

import FMPackages
import QuartzCore
import SwiftUI
import UIKit

struct DjSpinner: UIViewRepresentable {
    let deck: DjDeck
    let d: CGFloat
    let artwork: UIImage?
    let gain: Double
    let moving: Bool
    let scratching: Bool
    let rate: Double

    func makeUIView(context: Context) -> DjSpinView {
        DjSpinView(deck: deck)
    }

    func updateUIView(_ view: DjSpinView, context: Context) {
        view.deck = deck
        view.show(d: d, artwork: artwork, gain: gain)
        view.wake()
    }
}

@MainActor
final class DjSpinTicker: NSObject {
    weak var view: DjSpinView?

    init(_ view: DjSpinView) {
        self.view = view
    }

    @objc func tick(_ link: CADisplayLink) {
        guard let view else {
            link.invalidate()
            return
        }
        view.step(at: link.targetTimestamp)
    }
}

@MainActor
final class DjSpinView: UIView {
    static let key = "spin"
    static let fast = CAFrameRateRange(minimum: 60, maximum: 120, preferred: 120)
    static let slow = CAFrameRateRange(minimum: 8, maximum: 15, preferred: 15)
    static let settled = 0.0005
    static let slip = 0.01

    var deck: DjDeck
    private let disc = CALayer()
    private let art = CALayer()
    private let marks = CALayer()
    private var link: CADisplayLink?
    private var quick = true
    private var drawnFor: CGFloat = 0
    private var drawnScale: CGFloat = 0
    private var shownArt: UIImage?
    private var free: Double?
    private var freeAngle: Double = 0
    private var freeSince: CFTimeInterval = 0

    init(deck: DjDeck) {
        self.deck = deck
        super.init(frame: .zero)
        isUserInteractionEnabled = false
        isOpaque = false
        backgroundColor = .clear
        art.contentsGravity = .resizeAspectFill
        art.isHidden = true
        disc.addSublayer(art)
        disc.addSublayer(marks)
        layer.addSublayer(disc)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let side = min(bounds.width, bounds.height)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        disc.bounds = CGRect(x: 0, y: 0, width: side, height: side)
        disc.position = CGPoint(x: bounds.midX, y: bounds.midY)
        art.frame = disc.bounds.insetBy(dx: side * 0.09, dy: side * 0.09)
        marks.frame = disc.bounds
        CATransaction.commit()
        draw(side)
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard window != nil else {
            link?.invalidate()
            link = nil
            loose()
            return
        }
        draw(drawnFor)
        guard link == nil else { return }
        let link = CADisplayLink(target: DjSpinTicker(self), selector: #selector(DjSpinTicker.tick(_:)))
        link.preferredFrameRateRange = Self.fast
        quick = true
        link.add(to: .main, forMode: .common)
        self.link = link
        step(at: CACurrentMediaTime())
    }

    func show(d: CGFloat, artwork: UIImage?, gain: Double) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        if artwork !== shownArt {
            shownArt = artwork
            art.contents = artwork?.cgImage
            art.isHidden = artwork == nil
        }
        art.opacity = Float(0.4 + 0.6 * gain)
        CATransaction.commit()
    }

    func wake() {
        guard link != nil else { return }
        step(at: CACurrentMediaTime())
    }

    func step(at now: CFTimeInterval) {
        let angle = DjStudio.turnDegrees(deck.turned(at: now)) * Double.pi / 180
        if let speed = steadySpeed() {
            pace(Self.slow, quick: false)
            if let free, disc.animation(forKey: Self.key) != nil, abs(Self.wrapped(angle - freeAngle - free * (now - freeSince))) < Self.slip {
                return
            }
            spinFreely(angle, at: now, speed: speed)
            return
        }
        pace(deck.moving || deck.scratching ? Self.fast : Self.slow, quick: deck.moving || deck.scratching)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        loose()
        disc.setAffineTransform(CGAffineTransform(rotationAngle: angle))
        CATransaction.commit()
    }

    private func steadySpeed() -> Double? {
        guard deck.playing, !deck.scratching else { return nil }
        let drive = deck.effectiveRate
        let motor = deck.spin.motor
        guard drive > 0.05, abs(motor - drive) < Self.settled else { return nil }
        return motor * 2 * Double.pi / DjStudio.secondsPerTurn
    }

    private func spinFreely(_ angle: Double, at time: CFTimeInterval, speed: Double) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        disc.removeAnimation(forKey: Self.key)
        disc.setAffineTransform(CGAffineTransform(rotationAngle: angle))
        let spin = CABasicAnimation(keyPath: "transform.rotation.z")
        spin.fromValue = angle
        spin.toValue = angle + 2 * Double.pi
        spin.duration = 2 * Double.pi / speed
        spin.repeatCount = .infinity
        spin.beginTime = disc.convertTime(time, from: nil)
        spin.preferredFrameRateRange = Self.fast
        disc.add(spin, forKey: Self.key)
        CATransaction.commit()
        free = speed
        freeAngle = angle
        freeSince = time
    }

    private func loose() {
        guard free != nil else { return }
        disc.removeAnimation(forKey: Self.key)
        free = nil
    }

    private func pace(_ range: CAFrameRateRange, quick wanted: Bool) {
        guard let link, wanted != quick else { return }
        quick = wanted
        link.preferredFrameRateRange = range
    }

    private func draw(_ side: CGFloat) {
        let scale = traitCollection.displayScale > 0 ? traitCollection.displayScale : 2
        guard side > 0, side != drawnFor || scale != drawnScale else { return }
        drawnFor = side
        drawnScale = scale
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        marks.contents = Self.marks(side, scale: scale)
        marks.contentsScale = scale
        CATransaction.commit()
    }

    static func wrapped(_ radians: Double) -> Double {
        let turn = 2 * Double.pi
        var value = radians.truncatingRemainder(dividingBy: turn)
        if value > Double.pi { value -= turn } else if value < -Double.pi { value += turn }
        return value
    }

    static func marks(_ d: CGFloat, scale: CGFloat) -> CGImage? {
        let format = UIGraphicsImageRendererFormat()
        format.scale = scale
        format.opaque = false
        let image = UIGraphicsImageRenderer(size: CGSize(width: d, height: d), format: format).image { renderer in
            let context = renderer.cgContext
            let centre = CGPoint(x: d / 2, y: d / 2)
            let radius = d * 0.435
            let dot = max(1.2, d * 0.007)
            context.setFillColor(UIColor.white.withAlphaComponent(0.55).cgColor)
            for index in 0..<110 {
                let angle = Double(index) / 110 * 2 * Double.pi
                let point = CGPoint(x: centre.x + CGFloat(cos(angle)) * radius, y: centre.y + CGFloat(sin(angle)) * radius)
                context.fillEllipse(in: CGRect(x: point.x - dot / 2, y: point.y - dot / 2, width: dot, height: dot))
            }
            context.setFillColor(UIColor.white.withAlphaComponent(0.24).cgColor)
            context.fillEllipse(in: Self.circle(centre, d * 0.135))
            context.setFillColor(UIColor.white.withAlphaComponent(0.35).cgColor)
            context.fillEllipse(in: Self.circle(CGPoint(x: centre.x + d * 0.09, y: centre.y - d * 0.03), d * 0.009))
            let colours = [UIColor(DjTheme.rgb(0xD9DCE0)).cgColor, UIColor(DjTheme.rgb(0x6D7178)).cgColor] as CFArray
            guard let gradient = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colours, locations: [0, 1]) else { return }
            context.saveGState()
            context.addEllipse(in: Self.circle(centre, d * 0.013))
            context.clip()
            context.drawRadialGradient(
                gradient, startCenter: centre, startRadius: 0, endCenter: centre, endRadius: d * 0.014, options: [.drawsAfterEndLocation])
            context.restoreGState()
        }
        return image.cgImage
    }

    private static func circle(_ centre: CGPoint, _ radius: CGFloat) -> CGRect {
        CGRect(x: centre.x - radius, y: centre.y - radius, width: radius * 2, height: radius * 2)
    }
}

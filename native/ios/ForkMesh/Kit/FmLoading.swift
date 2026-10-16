// Loading and motion: FmSkeleton, FmSkeletonRow, FmSkeletonList, FmLoadingBar, fmLoading,
// FmEdgeFadeScroll, FmSwap and FmSliverFadeIn.

import SwiftUI

/// FmSkeleton: a single shimmering block standing in for content that has not
/// arrived.
///
/// A spinner destroys the page geometry on every load: the list vanishes, a dot
/// spins, the list snaps back, and the jump is what makes a wait feel long. A
/// placeholder that matches the shape of the row replacing it keeps the layout
/// still, so only the content changes.
///
/// The shimmer stops when the platform asks for reduced motion, leaving a
/// static block that still holds the right space. Without a `width` it fills
/// the available width. Base and highlight are both opaque, so fading the
/// highlight in over the base matches a lerp between them at every step.
struct FmSkeleton: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.shellTabHidden) private var tabHidden

    private let width: CGFloat?
    private let height: CGFloat
    private let radius: CGFloat

    init(width: CGFloat? = nil, height: CGFloat = 12, radius: CGFloat = FmRadius.sm) {
        self.width = width
        self.height = height
        self.radius = radius
    }

    static func glow(at date: Date) -> Double {
        let phase: Double = date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 2.2) / 1.1
        return phase <= 1 ? phase : 2 - phase
    }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion || tabHidden)) {
            (context: TimelineViewDefaultContext) in
            FmRoundedShape(radius: radius)
                .fill(fm.bgOverlay)
                .overlay {
                    FmRoundedShape(radius: radius)
                        .fill(fm.bgElevated)
                        .opacity(reduceMotion ? 0 : FmSkeleton.glow(at: context.date))
                }
        }
        .frame(width: width, height: height)
        .accessibilityHidden(true)
    }
}

/// FmSkeletonRow: a placeholder shaped like FmListRow, for use while a list
/// loads. The widths are deliberately uneven and vary by `index` so a column
/// of them reads as text that has not arrived rather than as a striped graphic.
struct FmSkeletonRow: View {
    private static let titleWidths: [CGFloat] = [148, 196, 122, 172]
    private static let subtitleWidths: [CGFloat] = [96, 132, 110, 84]

    private let index: Int
    private let hasLeading: Bool
    private let minHeight: CGFloat

    init(index: Int = 0, hasLeading: Bool = true, minHeight: CGFloat = 60) {
        self.index = index
        self.hasLeading = hasLeading
        self.minHeight = minHeight
    }

    private var slot: Int {
        let count: Int = FmSkeletonRow.titleWidths.count
        return ((index % count) + count) % count
    }

    private var lines: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSkeleton(width: FmSkeletonRow.titleWidths[slot], height: 13)
            FmSkeleton(width: FmSkeletonRow.subtitleWidths[slot], height: 10)
        }
    }

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            if hasLeading {
                FmSkeleton(width: 38, height: 38, radius: FmRadius.md)
            }
            lines
            Spacer(minLength: 0)
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .frame(maxWidth: .infinity, minHeight: minHeight, alignment: .leading)
    }
}

/// FmSkeletonList: a column of FmSkeletonRows with dividers, matching a loaded
/// explorer list. Announces itself as "Loading" to VoiceOver, which a bare set
/// of grey blocks would not.
///
/// Placeholders are often dropped into a box that is shorter than the rows
/// they stand in for. The column keeps its natural height, top-aligned, and
/// the last partial row is clipped: content continuing below the fold is what
/// the real list does.
struct FmSkeletonList: View {
    private let rows: Int
    private let hasLeading: Bool
    private let label: String

    init(rows: Int = 5, hasLeading: Bool = true, label: String = "Loading") {
        self.rows = rows
        self.hasLeading = hasLeading
        self.label = label
    }

    private var column: some View {
        VStack(spacing: 0) {
            ForEach(0..<max(rows, 0), id: \.self) { (position: Int) in
                if position > 0 {
                    FmRowDivider()
                }
                FmSkeletonRow(index: position, hasLeading: hasLeading)
            }
        }
    }

    var body: some View {
        column
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            .clipped()
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(label))
    }
}

/// Reports the frame of the scrolled content of FmEdgeFadeScroll.
private struct FmScrollContentFrameKey: PreferenceKey {
    static let defaultValue: CGRect = CGRect.zero

    static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        value = nextValue()
    }
}

/// FmEdgeFadeScroll: a horizontally scrolling strip whose clipped edges fade
/// out.
///
/// A half-visible chip sliced mid-word does not read as "this scrolls"; it
/// reads as a rendering fault. The fade is applied only on the side that
/// actually has more content, so a strip that fits entirely on screen gets no
/// fade at all and nothing is implied that is not there.
///
/// The strip owns the horizontal ScrollView and takes the row to scroll:
/// `FmEdgeFadeScroll { HStack { chips } }`.
struct FmEdgeFadeScroll<Content: View>: View {
    @State private var contentFrame: CGRect = CGRect.zero
    @State private var viewportWidth: CGFloat = 0

    private let fadeWidth: CGFloat
    private let content: Content

    // 32 is wide enough to read as a fade rather than as a slice. At 24 the
    // gradient was technically present but, on a near-black ground with grey
    // chip text, indistinguishable from a hard cut.
    init(fadeWidth: CGFloat = 32, @ViewBuilder content: () -> Content) {
        self.fadeWidth = fadeWidth
        self.content = content()
    }

    private static var spaceName: String {
        return "fm-edge-fade-scroll"
    }

    private var atStart: Bool {
        return contentFrame.minX >= -1
    }

    private var atEnd: Bool {
        return contentFrame.maxX <= viewportWidth + 1
    }

    /// With nothing to scroll both inner stops sit on the edges, which leaves
    /// the strip fully opaque.
    private var fadeMask: LinearGradient {
        let width: CGFloat = max(viewportWidth, 1)
        let fade: CGFloat = min(max(fadeWidth / width, 0), 0.5)
        let stops: [Gradient.Stop] = [
            Gradient.Stop(color: Color.black.opacity(0), location: 0),
            Gradient.Stop(color: Color.black, location: atStart ? 0 : fade),
            Gradient.Stop(color: Color.black, location: atEnd ? 1 : 1 - fade),
            Gradient.Stop(color: Color.black.opacity(0), location: 1),
        ]
        return LinearGradient(stops: stops, startPoint: UnitPoint.leading, endPoint: UnitPoint.trailing)
    }

    private var measuredContent: some View {
        content
            .background {
                GeometryReader { (proxy: GeometryProxy) in
                    Color.clear.preference(
                        key: FmScrollContentFrameKey.self,
                        value: proxy.frame(in: .named(FmEdgeFadeScroll.spaceName))
                    )
                }
            }
    }

    private var scroller: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            measuredContent
        }
        .coordinateSpace(name: FmEdgeFadeScroll.spaceName)
        .onPreferenceChange(FmScrollContentFrameKey.self) { (frame: CGRect) in
            contentFrame = frame
        }
        .background {
            GeometryReader { (proxy: GeometryProxy) in
                Color.clear
                    .onAppear {
                        viewportWidth = proxy.size.width
                    }
                    .onChange(of: proxy.size.width) { (_: CGFloat, newWidth: CGFloat) in
                        viewportWidth = newWidth
                    }
            }
        }
    }

    var body: some View {
        scroller
            .mask {
                fadeMask
            }
    }
}

/// FmSwap: cross-fades between whatever states a surface swaps through: a
/// skeleton giving way to content, an empty state to a populated list.
///
/// A cut reads as the screen being redrawn; a fade reads as the interface
/// responding. This is deliberately short and deliberately plain: the point is
/// continuity, not decoration. The child fades when `stateKey` changes, and
/// only then. Children stack top-aligned, so a tall outgoing skeleton does not
/// push the incoming content down. Under reduced motion the swap is instant.
struct FmSwap<Key: Hashable, Content: View>: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private let stateKey: Key
    private let duration: Double
    private let content: Content

    init(stateKey: Key, duration: Double? = nil, @ViewBuilder content: () -> Content) {
        self.stateKey = stateKey
        self.duration = duration ?? FmMotion.normal
        self.content = content()
    }

    var body: some View {
        ZStack(alignment: .top) {
            content
                .id(stateKey)
                .transition(AnyTransition.opacity)
        }
        .animation(FmMotion.easeOutCubic(duration, reduceMotion: reduceMotion), value: stateKey)
    }
}

/// FmSliverFadeIn: fades its content in whenever `stateKey` changes, without
/// holding the outgoing content. Use it for sections of a lazy stack, where
/// cross-fading two sections is not possible.
struct FmSliverFadeIn<Key: Equatable, Content: View>: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var opacity: Double = 1

    private let stateKey: Key
    private let duration: Double
    private let content: Content

    init(stateKey: Key, duration: Double? = nil, @ViewBuilder content: () -> Content) {
        self.stateKey = stateKey
        self.duration = duration ?? FmMotion.normal
        self.content = content()
    }

    private func fadeIn() {
        guard let animation = FmMotion.easeOutCubic(duration, reduceMotion: reduceMotion) else {
            return
        }
        opacity = 0
        // The drop to zero and the rise back must land in separate
        // transactions, or SwiftUI sees no change to animate.
        DispatchQueue.main.async {
            withAnimation(animation) {
                opacity = 1
            }
        }
    }

    var body: some View {
        content
            .opacity(opacity)
            .onChange(of: stateKey) { (_: Key, _: Key) in
                fadeIn()
            }
    }
}

struct FmLoadingBar: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var sweeping = false

    private let height: CGFloat
    private let period: Double

    init(height: CGFloat = 2, period: Double = 1.2) {
        self.height = height
        self.period = period
    }

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let width: CGFloat = proxy.size.width
            let segment: CGFloat = max(width * 0.3, 1)
            Capsule()
                .fill(fm.accent.opacity(0.75))
                .frame(width: reduceMotion ? width : segment, height: height)
                .opacity(reduceMotion ? 0.4 : 1)
                .offset(x: reduceMotion ? 0 : (sweeping ? width : -segment), y: 0)
        }
        .frame(height: height)
        .clipped()
        .allowsHitTesting(false)
        .onAppear {
            guard !reduceMotion else { return }
            withAnimation(Animation.easeInOut(duration: period).repeatForever(autoreverses: false)) {
                sweeping = true
            }
        }
        .accessibilityElement()
        .accessibilityLabel(Text(verbatim: "Loading"))
    }
}

private struct FmLoadingOverlay: ViewModifier {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var shown = false

    let active: Bool
    let delay: Duration

    func body(content: Content) -> some View {
        content
            .overlay(alignment: .top) {
                if shown {
                    FmLoadingBar()
                        .transition(AnyTransition.opacity)
                }
            }
            .animation(FmMotion.easeOutCubic(FmMotion.normal, reduceMotion: reduceMotion), value: shown)
            .task(id: active) {
                guard active else {
                    shown = false
                    return
                }
                try? await Task.sleep(for: delay)
                if !Task.isCancelled {
                    shown = true
                }
            }
    }
}

extension View {
    func fmLoading(_ active: Bool, delay: Duration = .milliseconds(250)) -> some View {
        modifier(FmLoadingOverlay(active: active, delay: delay))
    }
}

import SwiftUI

struct TranscriptFollowScroll<Tail: Equatable, Content: View>: View {
    let tail: Tail
    let pin: Int
    let accessibilityPrefix: String
    let content: () -> Content
    @State private var following: Bool = true
    @State private var fresh: Bool = false
    @State private var offset: CGFloat = 0
    @State private var viewport: CGFloat = 0

    private static var space: String { "transcript-follow-scroll" }
    private static var end: String { "transcript-follow-end" }
    private static var slack: CGFloat { 24 }

    init(tail: Tail, pin: Int = 0, accessibilityPrefix: String = "transcript", @ViewBuilder content: @escaping () -> Content) {
        self.tail = tail
        self.pin = pin
        self.accessibilityPrefix = accessibilityPrefix
        self.content = content
    }

    var body: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                VStack(spacing: 0) {
                    content()
                    Color.clear
                        .frame(height: 1)
                        .id(Self.end)
                }
                .background {
                    GeometryReader { (geo: GeometryProxy) in
                        Color.clear.preference(key: TranscriptFollowFrameKey.self, value: geo.frame(in: .named(Self.space)))
                    }
                }
            }
            .coordinateSpace(name: Self.space)
            .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { (height: CGFloat) in
                viewport = height
                if following {
                    settle(proxy)
                }
            }
            .onPreferenceChange(TranscriptFollowFrameKey.self) { (frame: CGRect) in
                track(frame)
            }
            .onChange(of: tail) { (_: Tail, _: Tail) in
                if following {
                    settle(proxy)
                } else {
                    fresh = true
                }
            }
            .onChange(of: pin) { (_: Int, _: Int) in
                following = true
                fresh = false
                settle(proxy)
            }
            .onAppear {
                settle(proxy)
            }
            .overlay(alignment: .bottom) {
                ZStack {
                    if !following {
                        TranscriptJumpButton(fresh: fresh, identifier: "\(accessibilityPrefix)-jump-latest") {
                            following = true
                            fresh = false
                            withAnimation(.easeOut(duration: 0.25)) {
                                proxy.scrollTo(Self.end, anchor: .bottom)
                            }
                        }
                        .padding(.bottom, FmSpace.x3)
                        .transition(.scale(scale: 0.7).combined(with: .opacity))
                    }
                }
                .animation(.easeOut(duration: 0.18), value: following)
            }
        }
    }

    private func settle(_ proxy: ScrollViewProxy) {
        DispatchQueue.main.async {
            proxy.scrollTo(Self.end, anchor: .bottom)
        }
    }

    private func track(_ frame: CGRect) {
        let top: CGFloat = -frame.minY
        if viewport > 0 && frame.maxY <= viewport + Self.slack {
            following = true
            fresh = false
        } else if top < offset - 1 {
            following = false
        }
        offset = top
    }
}

private struct TranscriptJumpButton: View {
    @Environment(\.fmPalette) private var fm
    let fresh: Bool
    let identifier: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: "arrow.down")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(fm.textPrimary)
                .frame(width: 40, height: 40)
                .chatGlass(Circle(), border: fm.border)
                .shadow(color: Color.black.opacity(fm.dark ? 0.35 : 0.12), radius: 8, x: 0, y: 3)
                .overlay(alignment: .topTrailing) {
                    if fresh {
                        Circle()
                            .fill(fm.accent)
                            .frame(width: 9, height: 9)
                            .offset(x: -2, y: 2)
                    }
                }
                .frame(width: 44, height: 44)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(fresh ? "New activity below. Jump to latest" : "Jump to latest")
        .accessibilityIdentifier(identifier)
    }
}

private struct TranscriptFollowFrameKey: PreferenceKey {
    static let defaultValue: CGRect = CGRect.zero

    static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        value = nextValue()
    }
}

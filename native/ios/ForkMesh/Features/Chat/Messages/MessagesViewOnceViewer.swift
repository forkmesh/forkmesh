import AVFoundation
import FMApi
import FMChat
import SwiftUI
import UIKit

struct MessagesViewOnceRow: View {
    let bubble: ChatBubble
    let entry: ViewOnceEntry
    let group: Bool
    let colors: MessagesColors
    let busy: Bool
    let onOpen: () -> Void
    let onUnsend: () -> Void
    @State private var confirmUnsend: Bool = false

    private var mine: Bool { entry.mine }

    var body: some View {
        VStack(alignment: mine ? .trailing : .leading, spacing: 2) {
            if bubble.showsSender {
                Text(bubble.row.message.senderName)
                    .font(.system(size: 12))
                    .foregroundStyle(colors.meta)
                    .padding(.leading, 46)
                    .padding(.top, 6)
            }
            HStack(alignment: .bottom, spacing: 6) {
                if mine {
                    Spacer(minLength: 56)
                } else if group {
                    if bubble.position.hasTail {
                        ChatSenderAvatar(message: bubble.row.message)
                    } else {
                        Color.clear.frame(width: 28, height: 1)
                    }
                }
                content
                if !mine { Spacer(minLength: 56) }
            }
            if entry.canUnsend {
                Button(action: { confirmUnsend = true }) {
                    Text("Unsend")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(MessagesColors.blue)
                        .padding(.horizontal, 12)
                        .frame(minHeight: 28)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Unsend \(entry.kind.label.lowercased()) before it is opened")
            }
        }
        .confirmationDialog("Unsend this \(entry.kind.label.lowercased())?", isPresented: $confirmUnsend, titleVisibility: .visible) {
            Button("Unsend", role: .destructive, action: onUnsend)
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("It is deleted from the server and can no longer be played.")
        }
        .frame(maxWidth: .infinity, alignment: mine ? .trailing : .leading)
        .padding(.top, bubble.position.startsRun ? 8 : 1)
        .padding(.horizontal, 10)
    }

    @ViewBuilder private var content: some View {
        if entry.canPlay {
            Button(action: onOpen) { shell }
                .buttonStyle(.plain)
                .disabled(busy)
                .accessibilityLabel(entry.accessibilityLabel)
                .accessibilityHint("Plays once, then it is deleted")
        } else {
            shell
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(entry.accessibilityLabel)
        }
    }

    private var shell: some View {
        let tint: Color = mine ? colors.sentText : colors.receivedText
        let live: Bool = entry.state == .ready || entry.state == .sending || entry.state == .uploading
        return HStack(spacing: 10) {
            ZStack {
                Circle()
                    .stroke(tint.opacity(live ? 0.9 : 0.4), style: StrokeStyle(lineWidth: 2, dash: live ? [] : [3, 3]))
                    .frame(width: 34, height: 34)
                if busy {
                    ProgressView().tint(tint)
                } else {
                    Image(systemName: entry.kind == .video ? "video.fill" : "mic.fill")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(tint.opacity(live ? 1 : 0.5))
                }
            }
            VStack(alignment: .leading, spacing: 1) {
                Text(entry.kind.label)
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(tint.opacity(live ? 1 : 0.6))
                Text("\(ViewOnceFrames.durationLabel(entry.durationMs)) · \(entry.stateLabel)")
                    .font(.system(size: 13).monospacedDigit())
                    .foregroundStyle(tint.opacity(0.75))
            }
        }
        .padding(.vertical, 8)
        .padding(.leading, mine ? 12 : 17)
        .padding(.trailing, mine ? 17 : 14)
        .background(
            MessagesBubbleShape(fromSelf: mine, tail: bubble.position.hasTail)
                .fill(mine ? colors.sentFill : colors.receivedFill))
        .contentShape(MessagesBubbleShape(fromSelf: mine, tail: bubble.position.hasTail))
    }
}

enum MessagesViewOnceStage: Equatable {
    case loading(Double)
    case playing
    case failed(String)
}

struct MessagesViewOnceViewer: View {
    @Environment(\.dismiss) private var dismiss
    let hub: ChatHub
    let entry: ViewOnceEntry
    @State private var stage: MessagesViewOnceStage = .loading(0)
    @State private var file: URL?
    @State private var player: AVPlayer?
    @State private var audio: AVAudioPlayer?
    @State private var levels: [CGFloat] = []
    @State private var progress: Double = 0
    @State private var captured: Bool = UIScreen.main.isCaptured
    @State private var screenshotNotice: Bool = false
    @State private var ticker: Task<Void, Never>?

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            content
            VStack(spacing: FmSpace.x2) {
                HStack {
                    Button(action: close) {
                        Image(systemName: "xmark")
                            .font(.system(size: 18, weight: .semibold))
                            .foregroundStyle(Color.white)
                            .frame(width: 44, height: 44)
                            .background(Circle().fill(Color.black.opacity(0.45)))
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Close. The message is deleted once it has started playing")
                    Spacer()
                    Text(entry.kind == .video ? "View-once video" : "View-once voice message")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(Color.white)
                    Spacer()
                    Color.clear.frame(width: 44, height: 44)
                }
                if screenshotNotice {
                    notice("A screenshot was taken. ForkMesh cannot prevent screenshots or screen recordings.")
                }
                if captured {
                    notice(
                        entry.kind == .video
                            ? "Screen recording is on, so the video is hidden. ForkMesh cannot stop other devices or cameras from recording."
                            : "Screen recording is on. ForkMesh cannot prevent it.")
                }
                Spacer(minLength: 0)
                if stage == .playing {
                    ProgressView(value: progress)
                        .tint(Color.white)
                        .accessibilityLabel("Playback")
                        .accessibilityValue("\(Int(progress * 100)) percent")
                }
            }
            .padding(FmSpace.x4)
        }
        .task { await load() }
        .onDisappear { teardown() }
        .onReceive(NotificationCenter.default.publisher(for: UIScreen.capturedDidChangeNotification)) { (note: Notification) in
            captured = (note.object as? UIScreen)?.isCaptured ?? UIScreen.main.isCaptured
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.userDidTakeScreenshotNotification)) { _ in
            screenshotNotice = true
        }
        .onReceive(NotificationCenter.default.publisher(for: AVPlayerItem.didPlayToEndTimeNotification)) {
            (note: Notification) in
            if let item = note.object as? AVPlayerItem, item === player?.currentItem { close() }
        }
    }

    private func notice(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 13, weight: .medium))
            .foregroundStyle(Color.white)
            .multilineTextAlignment(.center)
            .padding(FmSpace.x3)
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color.white.opacity(0.16)))
    }

    @ViewBuilder private var content: some View {
        switch stage {
        case .loading(let value):
            VStack(spacing: FmSpace.x3) {
                ProgressView(value: value).tint(Color.white).frame(maxWidth: 220)
                Text("Downloading and decrypting…").font(.system(size: 14)).foregroundStyle(Color.white)
            }
            .accessibilityElement(children: .combine)
            .accessibilityLabel("Downloading, \(Int(value * 100)) percent")
        case .failed(let text):
            Text(text)
                .font(.system(size: 16))
                .foregroundStyle(Color.white)
                .multilineTextAlignment(.center)
                .padding(FmSpace.x6)
        case .playing:
            if entry.kind == .video {
                if let player, !captured {
                    MessagesViewOncePlayerLayer(player: player)
                        .ignoresSafeArea()
                        .accessibilityLabel("Video message playing")
                } else {
                    Color.black.ignoresSafeArea().accessibilityHidden(true)
                }
            } else {
                MessagesViewOnceLevels(levels: levels, tint: Color.white)
                    .frame(height: 140)
                    .padding(.horizontal, FmSpace.x5)
                    .accessibilityLabel("Voice message playing")
            }
        }
    }

    private func load() async {
        guard let direct = hub.direct else {
            stage = .failed(ViewOnceError.unavailable.description)
            return
        }
        guard ViewOnceFrames.isPlayable(entry.mime) else {
            stage = .failed("This message was recorded in a format this device cannot play. It stays unopened.")
            return
        }
        do {
            let clip: ViewOnceClip = try await direct.playViewOnce(entry.messageId) { (value: Double) in
                stage = .loading(value)
            }
            guard !Task.isCancelled else { return }
            guard let url = MessagesViewOnceFiles.write(clip.data, ext: entry.kind == .video ? "mp4" : "m4a") else {
                stage = .failed("The message could not be prepared for playback.")
                return
            }
            file = url
            try AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
            try AVAudioSession.sharedInstance().setActive(true)
            if entry.kind == .video {
                let made: AVPlayer = AVPlayer(url: url)
                player = made
                stage = .playing
                made.play()
            } else {
                let made: AVAudioPlayer = try AVAudioPlayer(contentsOf: url)
                made.isMeteringEnabled = true
                audio = made
                stage = .playing
                made.play()
            }
            Task { await direct.consumeViewOnce(entry.messageId) }
            startTicker()
        } catch let error as ViewOnceError {
            stage = .failed(error.description)
            dropFile()
        } catch {
            stage = .failed("The message could not be played.")
            dropFile()
        }
    }

    private func startTicker() {
        ticker?.cancel()
        ticker = Task { @MainActor in
            var started: Bool = false
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 60_000_000)
                if let audio {
                    if audio.isPlaying {
                        started = true
                        audio.updateMeters()
                        let power: Float = audio.averagePower(forChannel: 0)
                        levels.append(CGFloat(max(0, min(1, (power + 50) / 50))))
                        if levels.count > 48 { levels.removeFirst(levels.count - 48) }
                        progress = audio.duration > 0 ? audio.currentTime / audio.duration : 0
                    } else if started {
                        close()
                        return
                    }
                } else if let player, let item = player.currentItem {
                    let total: Double = CMTimeGetSeconds(item.duration)
                    let now: Double = CMTimeGetSeconds(player.currentTime())
                    if total.isFinite && total > 0 { progress = min(1, max(0, now / total)) }
                }
            }
        }
    }

    private func close() {
        teardown()
        dismiss()
    }

    private func teardown() {
        ticker?.cancel()
        ticker = nil
        player?.pause()
        player?.replaceCurrentItem(with: nil)
        player = nil
        audio?.stop()
        audio = nil
        levels = []
        dropFile()
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    private func dropFile() {
        MessagesViewOnceFiles.drop(file)
        file = nil
    }
}

struct MessagesViewOncePlayerLayer: UIViewRepresentable {
    let player: AVPlayer

    final class LayerView: UIView {
        override class var layerClass: AnyClass { AVPlayerLayer.self }
        var playerLayer: AVPlayerLayer? { layer as? AVPlayerLayer }
    }

    func makeUIView(context: Context) -> LayerView {
        let view: LayerView = LayerView()
        view.backgroundColor = .black
        view.playerLayer?.videoGravity = .resizeAspect
        view.playerLayer?.player = player
        return view
    }

    func updateUIView(_ view: LayerView, context: Context) {
        if view.playerLayer?.player !== player { view.playerLayer?.player = player }
    }
}

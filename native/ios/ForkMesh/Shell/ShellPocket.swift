import FMApi
import FMAssistant
import FMBase
import FMPackages
import FMTools
import Observation
import SwiftUI
import UIKit

@MainActor
@Observable
final class ShellPocket {
    static let shared: ShellPocket = ShellPocket()

    var locked: Bool = false

    func lock() {
        FmHaptics.impact(.medium)
        locked = true
    }

    func unlock() {
        FmHaptics.success()
        locked = false
    }
}

struct ShellSuggestionChips: View {
    @Environment(\.fmPalette) private var fm
    private let suggestions: ShellSuggestions = ShellSuggestions.shared

    var body: some View {
        if suggestions.on, suggestions.contact != nil || suggestions.channel != nil {
            HStack(spacing: FmSpace.x1) {
                if let pick: GenieOutreachPick = suggestions.contact {
                    contactButton(pick)
                }
                if let channel: MarketingChannelAttention = suggestions.channel {
                    channelButton(channel)
                }
            }
            .padding(5)
            .shellGlass(Capsule())
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("top-suggestions")
        }
    }

    private func contactButton(_ pick: GenieOutreachPick) -> some View {
        Button { suggestions.contactTapped() } label: {
            portrait(pick)
                .overlay(alignment: .bottomTrailing) {
                    Image(systemName: pick.channel == .email ? "envelope.fill" : "bubble.left.fill")
                        .font(.system(size: 7, weight: .bold))
                        .foregroundStyle(Color.white)
                        .frame(width: 14, height: 14)
                        .background(Circle().fill(fm.accent))
                        .offset(x: 2, y: 2)
                }
                .contentShape(Circle())
        }
        .buttonStyle(ShellBarButtonStyle())
        .contextMenu {
            Button("Not now", systemImage: "forward") { suggestions.skipContact() }
            Button("Open the CRM", systemImage: "person.2") { SurfaceRouter.open(.crm(pipeline: nil)) }
        }
        .accessibilityLabel(Text(pick.caption))
        .accessibilityHint(Text("Writes the message and opens it in \(pick.channel == .email ? "Mail" : "Mastodon")"))
        .accessibilityIdentifier("top-suggestion-contact")
    }

    @ViewBuilder private func portrait(_ pick: GenieOutreachPick) -> some View {
        Group {
            if let image: UIImage = suggestions.contactImage {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                ZStack {
                    Circle().fill(fm.accent.opacity(0.18))
                    Text(pick.contact.initials)
                        .font(.system(size: 13, weight: .bold))
                        .foregroundStyle(fm.accent)
                }
            }
        }
        .frame(width: 34, height: 34)
        .clipShape(Circle())
    }

    private func channelButton(_ channel: MarketingChannelAttention) -> some View {
        let ink: Color = channel.channel.health == "attention" ? fm.danger : fm.warning
        return Button { suggestions.channelTapped() } label: {
            Image(systemName: "megaphone.fill")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(ink)
                .frame(width: 34, height: 34)
                .background(Circle().fill(ink.opacity(0.15)))
                .contentShape(Circle())
        }
        .buttonStyle(ShellBarButtonStyle())
        .accessibilityLabel(Text("Marketing: \(channel.caption)"))
        .accessibilityHint(Text("Opens the marketing channels"))
        .accessibilityIdentifier("top-suggestion-channel")
    }
}

struct PocketLockScreen: View {
    @Environment(\.fmPalette) private var fm
    let notifications: NotificationService?
    private let watcher: GlassesWatcher = GlassesWatcher.shared
    private let walk: WalkActivityController = WalkActivityController.shared

    var body: some View {
        VStack(spacing: FmSpace.x5) {
            clock
            Spacer(minLength: 0)
            glasses
            HStack(alignment: .top, spacing: FmSpace.x4) {
                pings
                walking
            }
            Spacer(minLength: 0)
            PocketUnlockSlider { ShellPocket.shared.unlock() }
        }
        .padding(.horizontal, FmSpace.x5)
        .padding(.vertical, FmSpace.x4)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.black.ignoresSafeArea())
        .foregroundStyle(Color.white)
        .environment(\.colorScheme, .dark)
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .accessibilityIdentifier("pocket-lock-screen")
    }

    private var clock: some View {
        TimelineView(.everyMinute) { (context: TimelineViewDefaultContext) in
            VStack(spacing: 2) {
                Text(context.date, format: .dateTime.weekday(.wide).month().day())
                    .font(.headline)
                    .foregroundStyle(Color.white.opacity(0.7))
                Text(context.date, format: .dateTime.hour().minute())
                    .font(.system(size: 72, weight: .semibold, design: .rounded).monospacedDigit())
            }
        }
        .padding(.top, FmSpace.x5)
    }

    @ViewBuilder private var glasses: some View {
        if watcher.watching {
            let nearby: Int = watcher.nearby.count
            let heard: Int = nearby + watcher.others.count
            let alarmed: Bool = watcher.ringing != nil || nearby > 0
            let blips: [GlassesRadarBlip] = GlassesRadarLayout.blips(
                others: watcher.others, glasses: watcher.nearby, homing: watcher.homing, heading: watcher.heading)
            VStack(spacing: FmSpace.x2) {
                GlassesRadar(blips: blips, color: alarmed ? fm.danger : fm.success, dot: Color.white.opacity(0.75), alert: fm.danger, size: 150)
                Text(nearby == 0 ? "No smart glasses" : nearby == 1 ? "Smart glasses nearby" : "\(nearby) smart glasses nearby")
                    .font(.title2.weight(.bold))
                    .foregroundStyle(alarmed ? fm.danger : Color.white)
                Text(heard == 1 ? "1 device heard" : "\(heard) devices heard")
                    .font(.subheadline)
                    .foregroundStyle(Color.white.opacity(0.6))
            }
            .accessibilityElement(children: .combine)
        } else {
            VStack(spacing: FmSpace.x2) {
                Image(systemName: "eyeglasses")
                    .font(.system(size: 64, weight: .regular))
                    .foregroundStyle(Color.white.opacity(0.4))
                Text("Glasses Alert is off")
                    .font(.title3.weight(.semibold))
                    .foregroundStyle(Color.white.opacity(0.6))
            }
            .accessibilityElement(children: .combine)
        }
    }

    private var pings: some View {
        let unread: Int = notifications?.unreadCount ?? 0
        let latest: [ForkNotification] = Array((notifications?.notifications ?? []).filter { $0.readAt == 0 }.prefix(3))
        return VStack(spacing: FmSpace.x2) {
            Image(systemName: unread > 0 ? "bell.badge.fill" : "bell.fill")
                .font(.system(size: 56, weight: .regular))
                .foregroundStyle(unread > 0 ? fm.danger : Color.white.opacity(0.4))
            Text(unread == 0 ? "No new pings" : unread == 1 ? "1 new ping" : "\(unread) new pings")
                .font(.title3.weight(.bold))
            ForEach(latest) { (ping: ForkNotification) in
                Text(ping.title)
                    .font(.caption)
                    .foregroundStyle(Color.white.opacity(0.7))
                    .lineLimit(1)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(FmSpace.x3)
        .background(RoundedRectangle(cornerRadius: 24).fill(Color.white.opacity(0.08)))
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("pocket-pings")
    }

    private var walking: some View {
        let moving: Bool = walk.moving
        let detecting: Bool = HealthFeature.prefs.bool(forKey: HealthPrefs.steps, or: false)
        return VStack(spacing: FmSpace.x2) {
            if walk.stepping {
                StepsWalkBadge(cadence: walk.cadence, running: walk.running, size: 76)
            } else {
                Image(systemName: moving ? "figure.walk" : "figure.stand")
                    .font(.system(size: 64, weight: .regular))
                    .foregroundStyle(moving ? fm.accent : Color.white.opacity(0.4))
                    .frame(height: 76)
            }
            Text(moving ? (walk.stepping ? walk.gaitText : "Walking") : detecting ? "Not walking" : "Walk detection is off")
                .font(.title3.weight(.bold))
                .multilineTextAlignment(.center)
            if walk.stepping {
                Text("\(walk.stepsPerMinute) steps/min")
                    .font(.caption)
                    .foregroundStyle(Color.white.opacity(0.7))
                    .monospacedDigit()
            }
        }
        .frame(maxWidth: .infinity)
        .padding(FmSpace.x3)
        .background(RoundedRectangle(cornerRadius: 24).fill(Color.white.opacity(0.08)))
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("pocket-walking")
    }
}

struct PocketUnlockSlider: View {
    let onUnlock: () -> Void
    @State private var drag: CGFloat = 0
    private let knob: CGFloat = 60

    var body: some View {
        GeometryReader { (geometry: GeometryProxy) in
            let travel: CGFloat = max(geometry.size.width - knob - 8, 1)
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.12))
                Text("slide to unlock")
                    .font(.title3.weight(.medium))
                    .foregroundStyle(Color.white.opacity(0.7 * (1 - drag / travel)))
                    .frame(maxWidth: .infinity)
                Circle()
                    .fill(Color.white)
                    .frame(width: knob, height: knob)
                    .overlay {
                        Image(systemName: "chevron.right.2")
                            .font(.system(size: 20, weight: .bold))
                            .foregroundStyle(Color.black)
                    }
                    .padding(4)
                    .offset(x: drag)
                    .gesture(
                        DragGesture(minimumDistance: 4)
                            .onChanged { (value: DragGesture.Value) in
                                drag = min(max(value.translation.width, 0), travel)
                            }
                            .onEnded { (_: DragGesture.Value) in
                                if drag >= travel * 0.85 {
                                    drag = travel
                                    onUnlock()
                                } else {
                                    withAnimation(.spring(response: 0.35, dampingFraction: 0.7)) { drag = 0 }
                                }
                            })
            }
        }
        .frame(height: knob + 8)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Slide to unlock"))
        .accessibilityAddTraits(.isButton)
        .accessibilityAction { onUnlock() }
        .accessibilityIdentifier("pocket-unlock")
    }
}

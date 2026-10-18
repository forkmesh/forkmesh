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
    private let autoSend: ShellAutoSend = ShellAutoSend.shared
    private let status: ShellMailStatus = ShellMailStatus.shared
    @State private var held: Bool = false

    private static let size: CGFloat = 44
    private static let badgeSide: CGFloat = 16
    private static let countSide: CGFloat = 18
    private static let ringGap: CGFloat = 6

    var body: some View {
        if suggestions.on, suggestions.contact != nil || autoSend.on || status.stage != nil {
            contactButton(suggestions.contact)
                .accessibilityIdentifier("top-suggestions")
        }
    }

    private func contactButton(_ pick: GenieOutreachPick?) -> some View {
        Button {
            if held {
                held = false
                return
            }
            suggestions.contactTapped()
        } label: {
            portrait(pick)
                .overlay {
                    if autoSend.on {
                        Circle().strokeBorder(fm.accent, lineWidth: 2.5)
                    }
                }
                .overlay(alignment: .topTrailing) { count }
                .overlay(alignment: .bottomTrailing) { badge(pick) }
                .contentShape(Circle())
        }
        .buttonStyle(ShellBarButtonStyle())
        .simultaneousGesture(LongPressGesture(minimumDuration: 0.5).onEnded { _ in
            CaptureHold.noteHandled()
            held = true
            autoSend.toggle()
        })
        .accessibilityLabel(Text(label(pick)))
        .accessibilityHint(Text(hint(pick)))
        .accessibilityAction(named: Text(toggleLabel)) { autoSend.toggle() }
        .accessibilityAction(named: Text("Not now")) { suggestions.skipContact() }
        .accessibilityAction(named: Text("Open the CRM")) { SurfaceRouter.open(.crm(pipeline: nil)) }
        .accessibilityIdentifier("top-suggestion-contact")
    }

    private var paused: Bool { autoSend.on && !autoSend.pausedFor.isEmpty }

    private func symbol(_ pick: GenieOutreachPick?) -> String {
        switch status.stage {
        case .finding: return "magnifyingglass"
        case .writing: return "pencil"
        case .sending: return "paperplane.fill"
        case .sent: return "checkmark"
        case nil:
            if paused { return "pause.fill" }
            return pick?.channel == .mastodon ? "bubble.left.fill" : "envelope.fill"
        }
    }

    private var stageLabel: String {
        switch status.stage {
        case .finding: return "Picking who to email."
        case .writing: return "Writing the email."
        case .sending: return "Sending the email."
        case .sent: return "Sent."
        case nil: return paused ? "Auto-send is waiting: \(autoSend.pausedFor)" : ""
        }
    }

    @ViewBuilder private func badge(_ pick: GenieOutreachPick?) -> some View {
        if pick != nil || status.stage != nil || paused {
            Image(systemName: symbol(pick))
                .font(.system(size: 8, weight: .bold))
                .foregroundStyle(Color.white)
                .contentTransition(.symbolEffect(.replace))
                .frame(width: ShellSuggestionChips.badgeSide, height: ShellSuggestionChips.badgeSide)
                .background(Circle().fill(status.stage == nil && paused ? fm.warning : fm.accent))
                .background {
                    if status.busy {
                        ShellMailSpinner(color: fm.accent, backing: fm.bgBase)
                            .frame(width: ShellSuggestionChips.badgeSide + ShellSuggestionChips.ringGap,
                                   height: ShellSuggestionChips.badgeSide + ShellSuggestionChips.ringGap)
                    }
                }
                .offset(x: 2, y: 2)
                .accessibilityHidden(true)
        }
    }

    private var toggleLabel: String { autoSend.on ? "Turn auto-send off" : "Turn auto-send on" }

    private func hint(_ pick: GenieOutreachPick?) -> String {
        let toggle: String = "Long-press to turn auto-send \(autoSend.on ? "off" : "on")."
        guard let pick else { return toggle }
        return "Writes the message and opens it in \(pick.channel == .email ? "Mail" : "Mastodon"). \(toggle)"
    }

    private func label(_ pick: GenieOutreachPick?) -> String {
        let sent: String = autoSend.sentToday == 1 ? "1 email sent today" : "\(autoSend.sentToday) emails sent today"
        var state: String = autoSend.on ? "Auto-send on, \(sent)" : sent
        if let window: DateInterval = autoSend.countdown(now: Date()) {
            let minutes: Int = max(1, Int((window.end.timeIntervalSinceNow / 60).rounded(.up)))
            state += minutes == 1 ? ". Next email in 1 minute" : ". Next email in \(minutes) minutes"
        }
        if !stageLabel.isEmpty { state = "\(stageLabel) \(state)" }
        guard let pick else { return state }
        return autoSend.on ? "Next: \(pick.contact.displayName). \(state)" : "\(pick.caption) \(state)"
    }

    @ViewBuilder private var count: some View {
        if autoSend.on || autoSend.sentToday > 0 {
            Text("\(autoSend.sentToday)")
                .font(.system(size: 10, weight: .bold).monospacedDigit())
                .lineLimit(1)
                .minimumScaleFactor(0.6)
                .foregroundStyle(Color.white)
                .padding(.horizontal, 2)
                .frame(width: ShellSuggestionChips.countSide, height: ShellSuggestionChips.countSide)
                .background(Circle().fill(autoSend.on ? fm.accent : fm.textSecondary))
                .background {
                    if autoSend.on {
                        countdown
                            .frame(width: ShellSuggestionChips.countSide + ShellSuggestionChips.ringGap,
                                   height: ShellSuggestionChips.countSide + ShellSuggestionChips.ringGap)
                    }
                }
                .offset(x: 3, y: -3)
                .accessibilityHidden(true)
        }
    }

    private var countdown: some View {
        TimelineView(.periodic(from: .now, by: 1)) { (context: TimelineViewDefaultContext) in
            if let window: DateInterval = autoSend.countdown(now: context.date), window.duration > 0 {
                let left: Double = min(1, max(0, window.end.timeIntervalSince(context.date) / window.duration))
                ZStack {
                    Circle().fill(fm.bgBase)
                    Circle().stroke(fm.accent.opacity(0.25), lineWidth: 2)
                    Circle()
                        .trim(from: 0, to: left)
                        .stroke(fm.accent, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                        .rotationEffect(.degrees(-90))
                        .animation(.linear(duration: 1), value: left)
                }
            }
        }
    }

    @ViewBuilder private func portrait(_ pick: GenieOutreachPick?) -> some View {
        Group {
            if pick != nil, let image: UIImage = suggestions.contactImage {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
            } else {
                ZStack {
                    Circle().fill(fm.accent.opacity(0.18))
                    if let pick {
                        Text(pick.contact.initials)
                            .font(.system(size: 16, weight: .bold))
                            .foregroundStyle(fm.accent)
                    } else {
                        Image(systemName: "envelope.fill")
                            .font(.system(size: 16, weight: .semibold))
                            .foregroundStyle(fm.accent)
                    }
                }
            }
        }
        .frame(width: ShellSuggestionChips.size, height: ShellSuggestionChips.size)
        .clipShape(Circle())
    }
}

struct ShellMailSpinner: View {
    let color: Color
    let backing: Color

    var body: some View {
        TimelineView(.animation) { (context: TimelineViewDefaultContext) in
            let turn: Double = context.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1)
            ZStack {
                Circle().fill(backing)
                Circle().stroke(color.opacity(0.25), lineWidth: 2)
                Circle()
                    .trim(from: 0, to: 0.3)
                    .stroke(color, style: StrokeStyle(lineWidth: 2, lineCap: .round))
                    .rotationEffect(.degrees(turn * 360))
            }
        }
        .accessibilityHidden(true)
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

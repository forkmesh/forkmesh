import FMAccount
import FMApi
import FMBase
import SwiftUI

enum PeopleColors {
    static func ring(_ ring: PeopleRing) -> Color {
        switch ring {
        case .blazing: return Color(red: 1.0, green: 0.42, blue: 0.17)
        case .glowing: return Color(red: 1.0, green: 0.64, blue: 0.23)
        case .kindling: return Color(red: 0.96, green: 0.81, blue: 0.42)
        case .warm: return Color(red: 0.37, green: 0.76, blue: 0.56)
        case .embers: return Color(red: 0.49, green: 0.65, blue: 0.77)
        }
    }

    static func icon(_ ring: PeopleRing) -> String {
        switch ring {
        case .blazing: return "local_fire_department_rounded"
        case .glowing: return "light_mode_outlined"
        case .kindling: return "auto_awesome_outlined"
        case .warm: return "favorite_outline"
        case .embers: return "bedtime_outlined"
        }
    }

    static let night = Color(red: 0.07, green: 0.05, blue: 0.08)
    static let hearth = Color(red: 0.24, green: 0.11, blue: 0.05)
}

struct PeopleScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: PeopleModel?
    @State private var picked: PeopleRanked?

    init() {
        let api: ApiService? = Services.get(ApiService.self)
        _model = State(initialValue: api.map { client in
            PeopleModel(client: client, viewer: { Services.get(AuthService.self)?.session?.nodeName ?? "" })
        })
    }

    var body: some View {
        Group {
            if let model {
                content(model)
            } else {
                FmEmptyState(icon: "people_outline", title: "People unavailable", message: "Sign in to a ForkMesh server to see people.")
            }
        }
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle("People")
        .navigationBarTitleDisplayMode(.inline)
        .sheet(item: $picked) { (person: PeopleRanked) in
            PeopleProfileSheet(person: person, viewer: model?.viewerName ?? "")
                .presentationDetents([.medium, .large])
                .presentationDragIndicator(.visible)
        }
    }

    @ViewBuilder
    private func content(_ model: PeopleModel) -> some View {
        if !model.loaded && !model.error.isEmpty {
            FmEmptyState(icon: "cloud_off_outlined", title: "Could not load people", message: model.error)
                .overlay(alignment: .bottom) {
                    FmTextButton("Retry", icon: "refresh", onPressed: { Task { await model.load() } })
                        .padding(.bottom, FmSpace.x6)
                }
        } else if !model.loaded {
            FmSkeletonList(rows: 6, label: "Loading people")
                .task { await model.load() }
        } else {
            page(model)
        }
    }

    private func page(_ model: PeopleModel) -> some View {
        let sections = model.sections
        let visible = model.visibleSections
        return ScrollView {
            LazyVStack(alignment: .leading, spacing: FmSpace.x4, pinnedViews: []) {
                PeopleCampfire(sections: sections, focus: model.focus, viewer: model.viewerName, onPick: { picked = $0 }, onRing: { ring in
                    FmHaptics.selection()
                    withAnimation(.easeInOut(duration: 0.2)) { model.toggleFocus(ring) }
                })
                PeopleLegend(sections: sections, focus: model.focus, onRing: { ring in
                    FmHaptics.selection()
                    withAnimation(.easeInOut(duration: 0.2)) { model.toggleFocus(ring) }
                })
                ForEach(visible) { (section: PeopleSection) in
                    PeopleSectionView(section: section, viewer: model.viewerName, onPick: { picked = $0 })
                }
                if visible.allSatisfy(\.people.isEmpty) && !model.query.isEmpty {
                    FmEmptyState(icon: "search_off", title: "No one matches “\(model.query)”")
                        .padding(.top, FmSpace.x4)
                }
            }
            .padding(FmSpace.x4)
        }
        .searchable(text: Binding(get: { model.query }, set: { model.query = $0 }), prompt: "Find a person")
        .fmRefreshable { await model.load() }
        .accessibilityIdentifier("people-screen")
    }
}

struct PeopleCampfire: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let sections: [PeopleSection]
    let focus: PeopleRing?
    let viewer: String
    let onPick: (PeopleRanked) -> Void
    let onRing: (PeopleRing) -> Void

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let size = min(proxy.size.width, 460)
            let seat = max(22, min(36, size * 0.066))
            let label = size * 0.2
            ZStack {
                backdrop(size)
                ForEach(PeopleRing.allCases, id: \.self) { (ring: PeopleRing) in
                    band(ring, size: size)
                }
                PeopleFire(reduceMotion: reduceMotion)
                    .frame(width: size * 0.2, height: size * 0.2)
                    .position(x: size / 2, y: size / 2)
                    .accessibilityHidden(true)
                ForEach(Array(PeopleRingLayout.seats(sections, size: size, seat: seat, label: label).enumerated()), id: \.offset) {
                    (pair: (offset: Int, element: PeopleSeat)) in
                    seatView(pair.element, seat: seat)
                        .position(x: pair.element.x, y: pair.element.y)
                }
                ForEach(PeopleRing.allCases, id: \.self) { (ring: PeopleRing) in
                    ringLabel(ring, size: size, width: label)
                }
            }
            .frame(width: size, height: size)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg * 2, style: .continuous))
            .frame(maxWidth: .infinity)
        }
        .aspectRatio(1, contentMode: .fit)
        .frame(maxWidth: 460)
        .frame(maxWidth: .infinity)
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Campfire of people, from the most active in the centre to the quietest at the edge")
    }

    private func backdrop(_ size: CGFloat) -> some View {
        RadialGradient(
            colors: [PeopleColors.hearth, Color(red: 0.12, green: 0.07, blue: 0.08), PeopleColors.night],
            center: .center, startRadius: 0, endRadius: size * 0.62)
    }

    private func band(_ ring: PeopleRing, size: CGFloat) -> some View {
        let r = PeopleRingLayout.radius(ring: ring.order, size: size)
        let color = PeopleColors.ring(ring)
        let dimmed = focus != nil && focus != ring
        return Circle()
            .stroke(color.opacity(dimmed ? 0.12 : 0.55), style: StrokeStyle(lineWidth: focus == ring ? 3 : 1.5, dash: ring == .embers ? [4, 5] : []))
            .background(Circle().stroke(color.opacity(dimmed ? 0.02 : 0.08), lineWidth: size * 0.06))
            .frame(width: r * 2, height: r * 2)
            .position(x: size / 2, y: size / 2)
            .shadow(color: color.opacity(ring.order < 2 && !dimmed ? 0.5 : 0), radius: 6)
    }

    private func ringLabel(_ ring: PeopleRing, size: CGFloat, width: CGFloat) -> some View {
        let r = PeopleRingLayout.radius(ring: ring.order, size: size)
        let count = sections.first { $0.ring == ring }?.people.count ?? 0
        let color = PeopleColors.ring(ring)
        return Button(action: { onRing(ring) }) {
            HStack(spacing: 3) {
                Text(ring.title)
                    .font(.system(size: 10, weight: .semibold))
                Text("\(count)")
                    .font(.system(size: 10, weight: .regular).monospacedDigit())
                    .opacity(0.8)
            }
            .foregroundStyle(focus == ring ? PeopleColors.night : color)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Capsule().fill(focus == ring ? color : PeopleColors.night.opacity(0.85)))
            .overlay(Capsule().stroke(color.opacity(0.7), lineWidth: 1))
            .frame(maxWidth: width)
        }
        .buttonStyle(.plain)
        .position(x: size / 2, y: size / 2 - r)
        .accessibilityLabel("\(ring.title) ring, \(count) \(count == 1 ? "person" : "people")")
        .accessibilityHint("Shows only this ring")
    }

    @ViewBuilder
    private func seatView(_ seat: PeopleSeat, seat size: CGFloat) -> some View {
        let color = PeopleColors.ring(seat.ring)
        let dimmed = focus != nil && focus != seat.ring
        if let person = seat.person {
            Button(action: { onPick(person) }) {
                FmAvatar(name: person.member.name, avatarPng: person.member.avatar, size: size, fontSize: size * 0.42)
                    .overlay(Circle().stroke(person.member.id == viewer ? Color.white : color, lineWidth: 2))
                    .shadow(color: color.opacity(0.55), radius: seat.ring == .blazing ? 6 : 2)
            }
            .buttonStyle(.plain)
            .opacity(dimmed ? 0.25 : 1)
            .accessibilityLabel("@\(person.member.name), \(seat.ring.title)")
            .accessibilityHint("Opens their profile")
        } else {
            Button(action: { onRing(seat.ring) }) {
                Text("+\(seat.overflow)")
                    .font(.system(size: size * 0.32, weight: .bold).monospacedDigit())
                    .foregroundStyle(PeopleColors.night)
                    .frame(width: size, height: size)
                    .background(Circle().fill(color))
            }
            .buttonStyle(.plain)
            .opacity(dimmed ? 0.25 : 1)
            .accessibilityLabel("\(seat.overflow) more in \(seat.ring.title)")
        }
    }
}

struct PeopleFire: View {
    let reduceMotion: Bool

    var body: some View {
        if reduceMotion {
            flame(0)
        } else {
            TimelineView(.animation(minimumInterval: 1 / 24)) { (context: TimelineViewDefaultContext) in
                flame(context.date.timeIntervalSinceReferenceDate)
            }
        }
    }

    private func flame(_ t: Double) -> some View {
        GeometryReader { (proxy: GeometryProxy) in
            let s = proxy.size.width
            ZStack {
                Circle()
                    .fill(RadialGradient(colors: [Color.orange.opacity(0.55), .clear], center: .center, startRadius: 0, endRadius: s * 0.7))
                    .scaleEffect(1.3 + 0.06 * sin(t * 2.1))
                Image(systemName: "flame.fill")
                    .resizable()
                    .scaledToFit()
                    .foregroundStyle(LinearGradient(colors: [Color(red: 1, green: 0.85, blue: 0.3), Color(red: 1, green: 0.35, blue: 0.1)], startPoint: .top, endPoint: .bottom))
                    .frame(width: s * 0.62)
                    .scaleEffect(x: 1 + 0.04 * sin(t * 5.3), y: 1 + 0.07 * sin(t * 7.1), anchor: .bottom)
                Image(systemName: "flame.fill")
                    .resizable()
                    .scaledToFit()
                    .foregroundStyle(Color(red: 1, green: 0.95, blue: 0.7))
                    .frame(width: s * 0.28)
                    .offset(y: s * 0.1)
                    .scaleEffect(y: 1 + 0.1 * sin(t * 9.7), anchor: .bottom)
            }
            .frame(width: s, height: s)
        }
    }
}

struct PeopleLegend: View {
    @Environment(\.fmPalette) private var fm

    let sections: [PeopleSection]
    let focus: PeopleRing?
    let onRing: (PeopleRing) -> Void

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(sections) { (section: PeopleSection) in
                    let color = PeopleColors.ring(section.ring)
                    let on = focus == section.ring
                    Button(action: { onRing(section.ring) }) {
                        HStack(spacing: FmSpace.x1) {
                            Circle().fill(color).frame(width: 10, height: 10)
                            Text(section.ring.title).fmText(.labelMedium, color: on ? PeopleColors.night : fm.textPrimary)
                            Text("\(section.people.count)").fmText(.bodySmall, color: on ? PeopleColors.night : fm.textSecondary)
                        }
                        .padding(.horizontal, FmSpace.x3)
                        .frame(minHeight: 36)
                        .background(Capsule().fill(on ? color : fm.bgRaised))
                        .overlay(Capsule().stroke(on ? color : fm.border, lineWidth: 1))
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(on ? .isSelected : [])
                    .accessibilityLabel("\(section.ring.title), \(section.people.count)")
                }
            }
            .padding(.vertical, 2)
        }
    }
}

struct PeopleSectionView: View {
    @Environment(\.fmPalette) private var fm

    let section: PeopleSection
    let viewer: String
    let onPick: (PeopleRanked) -> Void

    var body: some View {
        let color = PeopleColors.ring(section.ring)
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: FmSpace.x3) {
                ZStack {
                    Circle().fill(color.opacity(0.18))
                    FmIcon(PeopleColors.icon(section.ring), size: 18, color: color)
                }
                .frame(width: 34, height: 34)
                VStack(alignment: .leading, spacing: 2) {
                    Text(section.ring.title).fmText(.titleMedium)
                    Text(section.ring.blurb).fmText(.bodySmall, color: fm.textSecondary)
                }
                Spacer(minLength: 0)
                Text("\(section.people.count)")
                    .fmText(.labelMedium, color: color)
                    .padding(.horizontal, FmSpace.x2)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(color.opacity(0.15)))
            }
            .padding(FmSpace.x3)
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isHeader)
            if section.people.isEmpty {
                FmHairline()
                Text("No one in this ring right now.")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .padding(FmSpace.x3)
            }
            ForEach(section.people) { (person: PeopleRanked) in
                FmHairline()
                row(person, color: color)
            }
        }
        .background(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous).fill(fm.bgRaised))
        .overlay(alignment: .leading) {
            UnevenRoundedRectangle(topLeadingRadius: FmRadius.lg, bottomLeadingRadius: FmRadius.lg, style: .continuous)
                .fill(color)
                .frame(width: 4)
        }
        .overlay(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous).stroke(fm.border, lineWidth: 1))
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
    }

    private func row(_ person: PeopleRanked, color: Color) -> some View {
        Button(action: { onPick(person) }) {
            HStack(spacing: FmSpace.x3) {
                FmAvatar(name: person.member.name, avatarPng: person.member.avatar, size: 40)
                    .overlay(Circle().stroke(color, lineWidth: 2))
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: FmSpace.x1) {
                        Text("@\(person.member.name)").fmText(.titleSmall).lineLimit(1)
                        if person.member.id == viewer {
                            Text("You").fmText(.labelSmall, color: fm.accent)
                        }
                    }
                    Text(subtitle(person)).fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
                }
                Spacer(minLength: 0)
                FmIcon("chevron_right", size: 18, color: fm.textTertiary)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .frame(minHeight: 56)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
        .accessibilityHint("Opens their profile")
    }

    private func subtitle(_ person: PeopleRanked) -> String {
        let status = [person.member.statusEmoji, person.member.statusText].filter { !$0.isEmpty }.joined(separator: " ")
        return status.isEmpty ? PeopleRings.lastActive(person.member.activityBucket) : status
    }
}

struct PeopleProfileSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: PeopleProfileModel?

    let person: PeopleRanked

    init(person: PeopleRanked, viewer: String) {
        self.person = person
        let api: ApiService? = Services.get(ApiService.self)
        _model = State(initialValue: api.map { PeopleProfileModel(name: person.member.name, viewer: viewer, client: $0) })
    }

    var body: some View {
        let color = PeopleColors.ring(person.ring)
        ScrollView {
            VStack(spacing: FmSpace.x4) {
                header(color)
                if let model {
                    details(model, color: color)
                }
            }
            .padding(.bottom, FmSpace.x5)
        }
        .background(fm.sheetSurface.ignoresSafeArea())
        .task { await model?.load() }
        .accessibilityIdentifier("people-profile-sheet")
    }

    private func header(_ color: Color) -> some View {
        let avatar = model?.profile?.avatar.isEmpty == false ? model?.profile?.avatar ?? "" : person.member.avatar
        return VStack(spacing: FmSpace.x2) {
            FmAvatar(name: person.member.name, avatarPng: avatar, size: 88, fontSize: 36)
                .overlay(Circle().stroke(color, lineWidth: 3))
                .shadow(color: color.opacity(0.6), radius: 12)
                .padding(.top, FmSpace.x5)
            Text("@\(person.member.name)").fmText(.titleLarge, color: Color.white)
            HStack(spacing: FmSpace.x1) {
                FmIcon(PeopleColors.icon(person.ring), size: 14, color: PeopleColors.night)
                Text("\(person.ring.title) ring").fmText(.labelMedium, color: PeopleColors.night)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x1)
            .background(Capsule().fill(color))
            .accessibilityElement(children: .combine)
            Text(PeopleRings.lastActive(person.member.activityBucket))
                .fmText(.bodySmall, color: Color.white.opacity(0.75))
                .padding(.bottom, FmSpace.x4)
        }
        .frame(maxWidth: .infinity)
        .background(
            LinearGradient(colors: [color.opacity(0.55), PeopleColors.hearth, PeopleColors.night], startPoint: .top, endPoint: .bottom))
    }

    @ViewBuilder
    private func details(_ model: PeopleProfileModel, color: Color) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x4) {
            actions(model, color: color)
            if !model.actionError.isEmpty {
                Text(model.actionError).fmText(.bodySmall, color: fm.danger)
            }
            if let profile = model.profile {
                if profile.isPrivate {
                    Text("This profile is private.").fmText(.bodyMedium, color: fm.textSecondary)
                } else {
                    stats(profile)
                    about(profile)
                }
            } else if !model.error.isEmpty {
                Text(model.error).fmText(.bodyMedium, color: fm.textSecondary)
            } else {
                ProgressView().frame(maxWidth: .infinity)
            }
            Button(action: { open(.profile(name: person.member.name)) }) {
                HStack {
                    Text("View full profile").fmText(.labelLarge, color: fm.accent)
                    Spacer()
                    FmIcon("chevron_right", size: 18, color: fm.accent)
                }
                .frame(minHeight: 44)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, FmSpace.x4)
    }

    @ViewBuilder
    private func actions(_ model: PeopleProfileModel, color: Color) -> some View {
        if model.isSelf {
            Text("This is you.").fmText(.bodyMedium, color: fm.textSecondary)
        } else {
            HStack(spacing: FmSpace.x3) {
                let following = model.profile?.isFollowing == true
                if following {
                    FmOutlinedButton(model.busy ? "…" : "Following", icon: "check", minHeight: 44, fillWidth: true, onPressed: { follow(model) })
                        .accessibilityLabel("Unfollow @\(person.member.name)")
                        .accessibilityIdentifier("people-follow")
                } else {
                    FmFilledButton(model.busy ? "…" : "Follow", icon: "person_add", minHeight: 44, fillWidth: true, onPressed: { follow(model) })
                        .accessibilityLabel("Follow @\(person.member.name)")
                        .accessibilityIdentifier("people-follow")
                }
                FmOutlinedButton("Message", icon: "chat_bubble_outline", minHeight: 44, fillWidth: true, onPressed: { message() })
                    .accessibilityLabel("Message @\(person.member.name)")
                    .accessibilityIdentifier("people-message")
            }
            .disabled(model.profile == nil || model.busy)
        }
    }

    private func stats(_ profile: PeopleProfile) -> some View {
        HStack(spacing: 0) {
            stat("\(profile.followers)", "Followers")
            Divider().frame(height: 32)
            stat("\(profile.following)", "Following")
            Divider().frame(height: 32)
            stat(profile.createdAt > 0 ? Date(epochMillis: profile.createdAt).formatted(.dateTime.month(.abbreviated).year()) : "—", "Joined")
        }
        .padding(.vertical, FmSpace.x3)
        .background(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous).fill(fm.bgRaised))
        .overlay(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous).stroke(fm.border, lineWidth: 1))
    }

    private func stat(_ value: String, _ label: String) -> some View {
        VStack(spacing: 2) {
            Text(value).fmText(.titleMedium, tabular: true)
            Text(label).fmText(.bodySmall, color: fm.textSecondary)
        }
        .frame(maxWidth: .infinity)
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder
    private func about(_ profile: PeopleProfile) -> some View {
        let status = [profile.statusEmoji, profile.statusText].filter { !$0.isEmpty }.joined(separator: " ")
        if !status.isEmpty || !profile.bio.isEmpty || !profile.location.isEmpty {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                if !status.isEmpty {
                    Text(status).fmText(.bodyMedium)
                }
                if !profile.bio.isEmpty {
                    Text(profile.bio).fmText(.bodyLarge)
                }
                if !profile.location.isEmpty {
                    HStack(spacing: FmSpace.x1) {
                        FmIcon("location_on", size: 16, color: fm.textSecondary)
                        Text(profile.location).fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func follow(_ model: PeopleProfileModel) {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "follow people") else { return }
            await model.toggleFollow()
            if model.actionError.isEmpty { FmHaptics.success() } else { FmHaptics.error() }
        }
    }

    private func message() {
        Task { @MainActor in
            guard await AccountFeature.requireAccount(action: "send a message") else { return }
            open(.dm(user: person.member.name))
        }
    }

    private func open(_ route: Route) {
        dismiss()
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 350_000_000)
            AppNavigator.shared?.open(route)
        }
    }
}

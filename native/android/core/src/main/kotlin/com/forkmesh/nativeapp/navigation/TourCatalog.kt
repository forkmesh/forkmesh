package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.api.PrefStore
import kotlin.math.ceil
import kotlin.math.max

enum class TourTrack(val raw: String, val title: String, val summary: String, val icon: String) {
    ESSENTIALS("essentials", "Essentials", "Home, agents, the new agent button, Inbox and your hub.", "eco"),
    ADVANCED("advanced", "Advanced", "Adds voice, review and merge, commands, search and the bar.", "bolt"),
    EVERYTHING("everything", "The whole shebang", "Every corner: chat, projects, desktops, plugins, widgets and looks.", "auto_awesome");

    val rank: Int get() = ordinal

    fun includes(other: TourTrack): Boolean = other.rank <= rank

    companion object {
        fun of(raw: String?): TourTrack? = entries.firstOrNull { it.raw == raw }
    }
}

enum class TourScene(val raw: String) {
    HOME("home"), AGENTS("agents"), NEW_AGENT("newAgent"), INBOX("inbox"), HUB("hub"), VOICE("voice"), CODE("code"),
    PALETTE("palette"), SEARCH("search"), BAR("bar"), CHAT("chat"), PROJECTS("projects"), DESKTOPS("desktops"),
    WORKSPACE("workspace"), MARKET("market"), PERSONAL("personal"), SYSTEM("system"), LOOKS("looks"),
}

sealed class TourJump {
    data class Tab(val tab: AppTab) : TourJump()
    data object Home : TourJump()
    data object Hub : TourJump()
    data object Composer : TourJump()
    data object Palette : TourJump()
    data object Projects : TourJump()
    data class Open(val route: Route) : TourJump()

    val needsShell: Boolean get() = this !is Open
}

data class TourStop(
    val scene: TourScene,
    val track: TourTrack,
    val icon: String,
    val title: String,
    val message: String,
    val tips: List<String>,
    val tryIt: String,
    val jump: TourJump?,
    val jumpLabel: String,
    val needs: String? = null,
    val offers: List<String> = emptyList(),
    val fromMarket: Boolean = false,
) {
    val id: String get() = scene.raw

    fun resolved(installed: (String) -> Boolean): TourStop? {
        if (needs != null && !installed(needs)) return null
        if (offers.isEmpty() || offers.any(installed)) return this
        return copy(jump = TourJump.Open(Route.Market(null)), jumpLabel = "Find it in the Market", fromMarket = true)
    }
}

data class TourPlace(val track: TourTrack, val stop: String) {
    val encoded: String get() = "${track.raw}:$stop"

    companion object {
        fun decode(encoded: String?): TourPlace? {
            val parts = encoded?.split(":", limit = 2) ?: return null
            if (parts.size != 2 || parts[1].isEmpty()) return null
            val track = TourTrack.of(parts[0]) ?: return null
            return TourPlace(track, parts[1])
        }
    }
}

class TourProgress(private val prefs: PrefStore?) {
    val seen: Boolean get() = prefs?.getBool(SEEN_KEY) == true

    fun markSeen() {
        prefs?.putBool(SEEN_KEY, true)
    }

    val done: Set<TourTrack>
        get() = prefs?.getString(DONE_KEY).orEmpty().split(",").mapNotNull(TourTrack::of).toSet()

    fun markDone(track: TourTrack) {
        val next = done + TourTrack.entries.filter(track::includes)
        prefs?.putString(DONE_KEY, TourTrack.entries.filter(next::contains).joinToString(",") { it.raw })
    }

    val left: Int get() = TourTrack.entries.size - done.size

    val place: TourPlace? get() = TourPlace.decode(prefs?.getString(PLACE_KEY))

    fun remember(place: TourPlace?) {
        prefs?.putString(PLACE_KEY, place?.encoded.orEmpty())
    }

    fun nextAfter(track: TourTrack): TourTrack? {
        val finished = done
        return TourTrack.entries.firstOrNull { it.rank > track.rank && it !in finished }
    }

    companion object {
        const val SEEN_KEY = "nav/tour.v1"
        const val DONE_KEY = "nav/tour.done.v1"
        const val PLACE_KEY = "nav/tour.place.v1"
    }
}

object TourCatalog {
    const val SECONDS_PER_STOP = 20

    val stops: List<TourStop> = listOf(
        TourStop(
            TourScene.HOME, TourTrack.ESSENTIALS, "grid_view", "Home holds everything",
            "Every destination is an icon on Home, with live counts and a ring around anything busy. The glass bar along the bottom keeps your favourites one tap away.",
            listOf("Counts show what is new or waiting.", "Home leaves out what is already in the bar, so each icon shows once."),
            "Tap an icon", TourJump.Home, "Show me Home",
        ),
        TourStop(
            TourScene.AGENTS, TourTrack.ESSENTIALS, "auto_awesome", "Agents first",
            "The Agents tab lists every agent: on your desktop, in the cloud and on runners. Ones that need you come first, then what is running, then what finished.",
            listOf("Tap a run to read its transcript and steer it.", "Anything waiting on an answer sits at the top under Needs you."),
            "Tap Allow", TourJump.Tab(AppTab.Agents), "Open Agents", needs = "agents",
        ),
        TourStop(
            TourScene.NEW_AGENT, TourTrack.ESSENTIALS, "add_circle", "One tap starts an agent",
            "The glowing button in the top bar opens the composer, aimed at whatever is on screen. Each star inside it is an agent running right now.",
            listOf("Chips pick the repository, model and mode.", "Starting from a pull request or issue attaches it for you."),
            "Tap the glowing button", TourJump.Composer, "Open the composer", needs = "agents",
        ),
        TourStop(
            TourScene.INBOX, TourTrack.ESSENTIALS, "inbox", "One inbox for what needs you",
            "Pings, review requests, assigned issues and tasks, mentions and agent approvals land in Inbox. The app icon badge matches its unread count.",
            listOf("Filters narrow it to reviews, mentions or approvals.", "Notifications carry Approve, Reply and Merge buttons of their own."),
            "Tap a row to read it", TourJump.Tab(AppTab.Inbox), "Open Inbox", needs = "inbox",
        ),
        TourStop(
            TourScene.HUB, TourTrack.ESSENTIALS, "account_circle", "Your avatar is your hub",
            "Tap your avatar for your status, Pocket mode, Suggestion mode, Settings and signing out. The dot on it shows whether you are connected.",
            listOf("Set a status so teammates know you are away.", "What's new and Send diagnostics sit at the bottom."),
            "Tap your avatar", TourJump.Hub, "Open my hub",
        ),
        TourStop(
            TourScene.VOICE, TourTrack.ADVANCED, "graphic_eq", "Hold to talk",
            "Touch and hold the new agent button and say what the agent should do. Words appear as you speak, and letting go starts the agent.",
            listOf("Type moves the words into the composer instead.", "Commands opens the command palette from the same screen."),
            "Touch and hold the button", null, "", needs = "agents",
        ),
        TourStop(
            TourScene.CODE, TourTrack.ADVANCED, "merge", "Review and merge from your phone",
            "Repos lists your repositories and Code returns to the one you were in. Open a pull request to read the diff, review it and merge it.",
            listOf("Browse files, commits, branches and releases too.", "The new agent button on a pull request attaches it to the agent."),
            "Tap Merge", TourJump.Tab(AppTab.Code), "Open Repos", needs = "code",
        ),
        TourStop(
            TourScene.PALETTE, TourTrack.ADVANCED, "keyboard_command_key", "Commands for everything",
            "The command palette runs actions and jumps anywhere by name. Open it with Commands on the voice screen, or press Ctrl-K on a keyboard.",
            listOf("Suggestions change with the screen you are on.", "Switch project lives here too."),
            "Watch it find a command", TourJump.Palette, "Open the palette",
        ),
        TourStop(
            TourScene.SEARCH, TourTrack.ADVANCED, "search", "Search everywhere at once",
            "One field searches repositories, people, commits, issues, pull requests, code and chat. Pick a scope to narrow it, or keep All.",
            listOf("Save a search you run often.", "Offline, it searches what is already on the phone."),
            "Tap a scope", TourJump.Open(Route.Search("", SearchScope.Everything)), "Open Search",
        ),
        TourStop(
            TourScene.BAR, TourTrack.ADVANCED, "dock", "Make the bar yours",
            "Touch and hold any icon in the bottom bar to move it, put it in a folder or hide it. Bottom bar settings change its shape, glass, size and counts.",
            listOf("Sort it by usual order, recently used or most used.", "Two rows hold twice as many icons."),
            "Tap the bar to change its shape", TourJump.Home, "Show me the bar",
        ),
        TourStop(
            TourScene.CHAT, TourTrack.EVERYTHING, "forum", "Chat where the work lives",
            "Channels, direct messages, #mesh and office rooms share one tab. Send text, files, photos, voice and reactions; private rooms are encrypted.",
            listOf("In a room, the composer posts to that room.", "Mentions and direct messages badge the Chat icon."),
            "Tap a message to react", TourJump.Tab(AppTab.Chat), "Open Chat", needs = "chat",
        ),
        TourStop(
            TourScene.PROJECTS, TourTrack.EVERYTHING, "layers", "Projects keep work apart",
            "Each project has its own account, repositories and layout. Tap the project icon in the top bar to switch, or use Switch project in the palette.",
            listOf("Invite people from the project switcher.", "Your bottom bar layout is shared by every project."),
            "Tap a project", TourJump.Projects, "Switch project",
        ),
        TourStop(
            TourScene.DESKTOPS, TourTrack.EVERYTHING, "desktop_windows", "Your desktops in your pocket",
            "Desktops shows every ForkMesh desktop you are signed in to, with its agents and shortcuts. Hosts and Network show the machines and relays behind them.",
            listOf("Run a desktop shortcut and watch its progress.", "An orange dot means a desktop went offline."),
            "Tap Run", TourJump.Open(Route.Desktops), "Open Desktops",
            offers = listOf("desktops", "hosts", "network"),
        ),
        TourStop(
            TourScene.WORKSPACE, TourTrack.EVERYTHING, "description", "Notes, tasks and your calendar",
            "Notes, Tasks, Calendar, Timers and Files cover the work around the code, each one a plugin you can add or remove.",
            listOf("Turn a note into a task or an issue.", "Timers keep counting in the notification shade."),
            "Tap a task to finish it", TourJump.Open(Route.Notes), "Open Notes",
            offers = listOf("notes", "tasks", "calendar", "timers", "files"),
        ),
        TourStop(
            TourScene.MARKET, TourTrack.EVERYTHING, "shopping_bag", "Add more from the Market",
            "Plugins add icons to Home and pages to the app. Install and remove them from the Market; anything you remove disappears everywhere it showed.",
            listOf("Each plugin lists its permissions before you install it.", "Touch and hold an icon for its settings or to remove it."),
            "Tap Get", TourJump.Open(Route.Market(null)), "Open the Market",
        ),
        TourStop(
            TourScene.PERSONAL, TourTrack.EVERYTHING, "shield", "Private tools for you",
            "Vault keeps secrets, Coves share encrypted files, and trackers count water, steps, sleep and breaks. On-device AI answers without leaving the phone.",
            listOf("Each one is a plugin, so keep only what you use.", "Vault passwords can fill in other apps."),
            "Tap the glass to add water", TourJump.Open(Route.Vault(null)), "Open Vault",
            offers = listOf("vault", "coves", "water", "steps", "sleep", "on-device-ai"),
        ),
        TourStop(
            TourScene.SYSTEM, TourTrack.EVERYTHING, "notifications_active", "Outside the app",
            "Notifications let you approve, reply or merge without opening the app. Widgets, the running-agents notification and a Quick Settings tile show and steer your agents from anywhere.",
            listOf("Add the Agents or Inbox widget to your home screen.", "Add the New agent tile to Quick Settings."),
            "Tap Approve", null, "",
        ),
        TourStop(
            TourScene.LOOKS, TourTrack.EVERYTHING, "palette", "Make it look like yours",
            "Pick outline, filled, accent or tile icons, how many fit in a row, and the app's theme and wallpaper in Settings.",
            listOf("Home screen settings also hide categories you never use.", "Turning off animations in Android's accessibility settings calms the effects."),
            "Tap to change the icons", TourJump.Open(Route.Settings(null)), "Open Settings",
        ),
    )

    fun stops(track: TourTrack, installed: (String) -> Boolean = { true }): List<TourStop> =
        stops.filter { track.includes(it.track) }.mapNotNull { it.resolved(installed) }

    fun minutes(count: Int): Int = max(1, ceil(count * SECONDS_PER_STOP / 60.0).toInt())

    fun index(place: TourPlace, stops: List<TourStop>): Int? = stops.indexOfFirst { it.id == place.stop }.takeIf { it >= 0 }

    fun symbols(track: TourTrack): List<String> = stops.filter { it.track == track }.map { it.icon }
}

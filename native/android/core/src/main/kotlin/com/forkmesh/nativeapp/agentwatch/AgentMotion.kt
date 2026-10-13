package com.forkmesh.nativeapp.agentwatch

enum class AgentMotion(
    val hue: Double,
    val reach: Double,
    val seconds: Double,
    val weight: Double,
    private val spokeCount: Double,
    val inbound: Boolean,
    val dashed: Boolean,
    val label: String,
    val description: String,
    val sound: String,
) {
    start(0.33, 2.6, 1.8, 1.6, 0.0, false, false, "Start", "a run opening, or a session announcing itself", "a bell on the root: the phrase opens"),
    speak(0.60, 1.5, 1.3, 1.0, 0.0, false, false, "Speaking", "the agent writing prose to you", "a mallet wandering up and down the scale"),
    think(0.78, 1.2, 2.0, 0.7, 0.0, false, false, "Thinking", "reasoning, under its breath", "a soft pad breathed in slowly, drifting down"),
    read(0.52, 1.8, 1.1, 0.8, 0.0, false, false, "Reading", "a file read, a folder listed", "a bright glass note, a step or two up"),
    search(0.16, 2.2, 1.3, 0.8, 3.0, false, true, "Searching", "grep, glob, find: looking through the tree", "a glass note leaping two to four steps up"),
    web(0.555, 2.6, 1.5, 0.9, 0.0, false, false, "Web", "a page fetched, the web searched", "a far glass note leaping three to five steps up"),
    write(0.87, 1.7, 1.4, 1.9, 4.0, false, false, "Writing", "an edit or a new file landing on disk", "a ringing bell, climbing"),
    run(0.08, 1.4, 0.9, 1.2, 4.0, false, false, "Command", "a shell command being run", "a dry wooden knock that bends down, climbing"),
    delegate(0.70, 2.4, 1.6, 1.1, 6.0, false, false, "Delegating", "work handed to a subagent, a skill or an MCP server", "a bell leaping three or four steps up"),
    plan(0.28, 1.6, 1.5, 1.0, 0.0, false, false, "Planning", "a todo list written, a plan put forward", "a mallet ticking off a list, up a step and back"),
    arrive(0.42, 1.9, 1.1, 0.9, 0.0, true, false, "Result", "a tool's answer coming back", "a mallet falling, further for a bigger answer"),
    prompt(0.95, 2.2, 1.7, 1.5, 0.0, true, false, "Prompt", "you, steering it", "a loud bell back on the root"),
    notice(0.00, 2.0, 1.4, 1.4, 5.0, false, true, "Notice", "a guardrail, a protocol error, a refusal, a stop", "a bright glass note held level; a failed run falls to the root"),
    finish(0.24, 3.0, 2.4, 1.7, 0.0, false, false, "Done", "a turn's result landing, a run finishing", "a bell on the root; a finished run rings a rising cadence"),
    scan(0.50, 1.6, 1.0, 0.8, 0.0, false, true, "Listing", "a folder listed, a glob expanded", "a short pure ping, a step or two up"),
    create(0.82, 2.2, 1.6, 1.8, 0.0, false, false, "New file", "a file written from nothing", "a note swooping up into place, climbing"),
    test(0.30, 1.8, 1.5, 1.2, 0.0, false, false, "Tests", "a test suite running", "a trill, shaken back and forth"),
    build(0.05, 2.0, 1.4, 1.6, 0.0, false, false, "Build", "a compile, a bundle, a make", "a low drum an octave down, bent hard"),
    git(0.98, 1.9, 1.4, 1.2, 0.0, false, false, "Git", "git or gh: history read, a commit, a push", "a sagging string, through history and back"),
    install(0.14, 2.0, 1.6, 1.2, 0.0, true, false, "Install", "a package manager fetching dependencies", "a low drum piling up a step at a time"),
    spawn(0.62, 2.8, 1.6, 1.2, 0.0, false, false, "Background", "a command sent off to run on its own", "a swoop leaping four or five steps up"),
    wait(0.66, 1.3, 2.2, 0.8, 0.0, false, true, "Waiting", "polling background work, sleeping on it", "a slow swell, held nearly level"),
    ask(0.90, 2.4, 2.0, 1.6, 0.0, false, false, "Question", "the agent asking you something", "a bell going up at the end, like a question"),
    skill(0.74, 2.2, 1.5, 1.2, 0.0, false, false, "Skill", "a skill called in", "a twanging string leaping up"),
    mcp(0.48, 2.4, 1.5, 1.1, 0.0, false, false, "MCP", "a tool on an MCP server", "a far glass note, two to five steps up"),
    todo(0.26, 1.5, 1.4, 1.0, 0.0, false, false, "Todo", "the todo list or task list updated", "a mallet ticking up and back"),
    fumble(0.02, 2.0, 1.3, 1.5, 0.0, true, false, "Error", "a tool call that came back as an error", "a rough buzz, falling"),
    denied(0.97, 2.2, 1.5, 1.7, 0.0, true, true, "Refused", "a tool call refused: a permission, a hook, you", "a harder buzz, falling further"),
    image(0.20, 2.0, 1.4, 1.0, 0.0, true, false, "Image", "a picture back: a screenshot, an image read", "a glass note, shaded by the picture"),
    compact(0.68, 3.0, 2.2, 1.4, 0.0, true, false, "Compacting", "the context folded down to make room", "a swell sinking four or five steps"),
    retry(0.11, 1.6, 1.3, 1.0, 0.0, false, true, "Retry", "an API call failed and is being tried again", "a trill held level"),
    fail(0.00, 3.0, 2.4, 1.8, 0.0, true, false, "Failed", "a turn that ended in an error", "a buzz sinking to the fifth under the root"),
    frame(0.64, 0.9, 0.7, 0.6, 0.0, false, false, "Framing", "a message or block opening in the stream", "a dry wooden click just above the line"),
    compose(0.46, 1.1, 0.9, 0.6, 0.0, false, false, "Tool call", "a tool call's arguments streaming in, piece by piece", "soft glass taps stepping upwards, like typing"),
    usage(0.12, 1.0, 1.1, 0.7, 0.0, true, false, "Usage", "tokens counted, rate limits reported, a turn's stop reason", "a low soft mallet tap under the line"),
    system(0.36, 1.2, 1.2, 0.8, 6.0, false, false, "System", "a hook firing, a status change, a compaction, a retry", "a small bell a fifth above, held a moment"),
    tick(0.58, 0.7, 0.6, 0.5, 0.0, false, false, "Other", "anything else the log records: heartbeats, control frames", "a faint high sparkle between the beats"),
    close(0.60, 0.9, 0.7, 0.6, 0.0, true, false, "Closing", "a message or block closing in the stream", "a dry wooden click, under the opening one"),
    seal(0.15, 0.8, 0.6, 0.5, 0.0, false, false, "Seal", "a thinking block's signature, a citation", "a glinting ping at the very top"),
    hook(0.40, 1.1, 1.1, 0.8, 0.0, false, false, "Hook", "a hook starting or answering", "a sagging string a little above the line"),
    network(0.50, 1.0, 1.0, 0.6, 0.0, false, false, "Network", "bytes an agent sent or received over the network", "a faint tick: higher for sent, lower for received"),
    connect(0.53, 2.2, 1.4, 1.0, 0.0, false, false, "New host", "a host joining the field: the first bytes to somewhere new", "a chirp swooping up high"),
    task(0.10, 1.4, 1.2, 0.9, 0.0, false, false, "Desktop work", "the desktop's background work starting or finishing", "a soft wooden knock a step above the line"),
    logError(0.99, 2.2, 1.6, 1.5, 0.0, true, false, "Log: error", "an ERROR line in the desktop's log", "a rough buzz falling below the line"),
    logStall(0.11, 2.0, 1.8, 1.2, 0.0, false, true, "Log: stall", "a STALL or BGBLOCK line: the interface held up, work turned away", "a low swell, breathed in"),
    logSync(0.50, 1.6, 1.2, 0.9, 0.0, false, false, "Log: mesh", "a MIRROR, SYNC, PIN, PEER, NODE, FORKED or STATUS line", "a glass note a few steps up"),
    logGit(0.61, 1.7, 1.3, 1.0, 0.0, false, false, "Log: git", "a GIT, PULL, MERGE, PUBLISH, ISSUE, BOUNTY or ACTIONS line", "a twanging string a step or two up"),
    logCloud(0.08, 1.6, 1.1, 0.9, 0.0, true, false, "Log: cloud", "a CLOUD, NETWORK or DB line: the site and its database", "a short pure ping high over the line"),
    logTask(0.04, 1.3, 1.1, 0.8, 0.0, false, false, "Log: background", "a BGTASK or BGNOTE line: background work reported", "a dry wooden tap on the line"),
    logAccount(0.79, 1.8, 1.4, 1.0, 0.0, false, false, "Log: account", "a SESSION, ACCOUNT, IDENTITY, CRYPTO, ADMIN or PROMPT line", "a small bell a fifth up"),
    logInfo(0.57, 1.2, 1.0, 0.7, 0.0, false, false, "Log: info", "an INFO line, or any badge without a family", "the faintest glass glint at the top");

    val spokes: Int get() = spokeCount.toInt()

    val isBookkeeping: Boolean
        get() = when (this) {
            frame, compose, usage, system, tick, close, seal, hook -> true
            else -> false
        }

    val isFaint: Boolean get() = isBookkeeping || this == network

    val isLog: Boolean get() = ordinal in logError.ordinal..logInfo.ordinal

    val isCue: Boolean get() = isLog || this == connect || this == task

    companion object {
        fun withLabel(label: String): AgentMotion? = entries.firstOrNull { it.label == label }

        fun named(name: String): AgentMotion? = entries.firstOrNull { it.name == name }
    }
}

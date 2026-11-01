#include "AppArmorModel.h"
#include "AppArmorSystem.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace forkmesh::apparmor;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition)
        qInfo("PASS: %s", qPrintable(message));
    else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

void writeFile(const QString &path, const QString &contents)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(contents.toUtf8());
}

void writeBytes(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).dir().path());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(contents);
}

const QString firefoxProfile = QStringLiteral(
    "# This profile allows everything\n"
    "abi <abi/4.0>,\n"
    "include <tunables/global>\n"
    "\n"
    "profile firefox /{usr/lib/firefox{,-esr},opt/firefox}/firefox{,-esr,-bin} flags=(unconfined) {\n"
    "  userns,\n"
    "\n"
    "  # Site-specific additions and overrides. See local/README for details. {not a block}\n"
    "  include if exists <local/firefox>\n"
    "}\n");

const QString pathProfile = QStringLiteral(
    "#include <tunables/global>\n"
    "\n"
    "/usr/sbin/tcpdump flags=(complain,attach_disconnected) {\n"
    "  #include <abstractions/base>\n"
    "  /etc/{passwd,group} r,\n"
    "  \"/srv/with space/**\" rw,\n"
    "  ^hat {\n"
    "    /tmp/** rw,\n"
    "  }\n"
    "  profile child {\n"
    "    capability net_raw,\n"
    "  }\n"
    "  #include <local/usr.sbin.tcpdump>\n"
    "}\n");

const QString twoProfiles = QStringLiteral(
    "profile one /usr/bin/one {\n"
    "  /etc/one r,\n"
    "}\n"
    "profile two /usr/bin/two flags=(complain) {\n"
    "  /etc/two r,}\n");

void testDeclarations()
{
    const QList<ProfileDeclaration> firefox = parseProfileDeclarations(firefoxProfile);
    check(firefox.size() == 1, QStringLiteral("glob braces in an attachment and braces in comments do not open blocks"));
    check(firefox.value(0).name == QStringLiteral("firefox"), QStringLiteral("profile keyword gives the name"));
    check(firefox.value(0).attachment == QStringLiteral("/{usr/lib/firefox{,-esr},opt/firefox}/firefox{,-esr,-bin}"),
          QStringLiteral("attachment keeps its glob"));
    check(firefox.value(0).flags == QStringList{QStringLiteral("unconfined")}, QStringLiteral("flags are read"));
    check(declaredMode(firefox.value(0).flags) == Mode::Unconfined, QStringLiteral("unconfined flag gives unconfined mode"));
    check(firefox.value(0).localInclude == QStringLiteral("firefox"), QStringLiteral("include if exists <local/...> is found"));

    const QList<ProfileDeclaration> tcpdump = parseProfileDeclarations(pathProfile);
    check(tcpdump.size() == 1, QStringLiteral("hats and child profiles are not top-level profiles"));
    check(tcpdump.value(0).name == QStringLiteral("/usr/sbin/tcpdump"), QStringLiteral("path-named profile"));
    check(tcpdump.value(0).attachment == QStringLiteral("/usr/sbin/tcpdump"), QStringLiteral("path-named profile attaches to its name"));
    check(declaredMode(tcpdump.value(0).flags) == Mode::Complain, QStringLiteral("complain among several flags"));
    check(tcpdump.value(0).localInclude == QStringLiteral("usr.sbin.tcpdump"), QStringLiteral("old #include <local/...> is found"));
    check(pathProfile.at(tcpdump.value(0).bodyClose) == QLatin1Char('}') && tcpdump.value(0).bodyClose == pathProfile.size() - 2,
          QStringLiteral("body close is the profile's last brace"));

    const QList<ProfileDeclaration> two = parseProfileDeclarations(twoProfiles);
    check(two.size() == 2, QStringLiteral("two profiles in one file"));
    check(fileMode(two) == Mode::Mixed, QStringLiteral("a file with different modes is mixed"));
    check(parseProfileDeclarations(QStringLiteral("@{HOME}=/home/*/\n")).isEmpty(), QStringLiteral("variables are not profiles"));
}

void testWithMode()
{
    const QString complain = withMode(twoProfiles, Mode::Complain);
    const QList<ProfileDeclaration> complained = parseProfileDeclarations(complain);
    check(complain.contains(QStringLiteral("profile one /usr/bin/one flags=(complain) {")), QStringLiteral("complain adds flags"));
    check(complain.contains(QStringLiteral("profile two /usr/bin/two flags=(complain) {")), QStringLiteral("complain keeps an existing flag"));
    check(fileMode(complained) == Mode::Complain, QStringLiteral("whole file is in complain"));

    const QString enforce = withMode(complain, Mode::Enforce);
    check(enforce == twoProfiles.left(twoProfiles.indexOf(QStringLiteral("profile two")))
              + QStringLiteral("profile two /usr/bin/two {\n  /etc/two r,}\n"),
          QStringLiteral("enforce removes the flags clause entirely"));

    const QString tcpdump = withMode(pathProfile, Mode::Enforce);
    check(tcpdump.contains(QStringLiteral("/usr/sbin/tcpdump flags=(attach_disconnected) {")), QStringLiteral("enforce keeps other flags"));
    check(withMode(pathProfile, Mode::Complain).contains(QStringLiteral("flags=(complain,attach_disconnected)")),
          QStringLiteral("complain is idempotent"));
    check(withMode(tcpdump, Mode::Complain).contains(QStringLiteral("/usr/sbin/tcpdump flags=(complain,attach_disconnected) {")),
          QStringLiteral("complain puts the mode first"));
    check(withMode(QStringLiteral("no profile here\n"), Mode::Complain) == QStringLiteral("no profile here\n"),
          QStringLiteral("text without profiles is unchanged"));
}

void testRules()
{
    const QString added = withRule(twoProfiles, QStringLiteral("one"), QStringLiteral("/etc/extra r,"));
    check(added.contains(QStringLiteral("  /etc/one r,\n  /etc/extra r,\n}\nprofile two")), QStringLiteral("rule lands before the closing brace"));
    const QString addedTwo = withRule(twoProfiles, QStringLiteral("/usr/bin/two"), QStringLiteral("capability chown,"));
    check(addedTwo.endsWith(QStringLiteral("/etc/two r,\n  capability chown,\n}\n")), QStringLiteral("brace on a rule line is split"));
    check(withRule(twoProfiles, QStringLiteral("missing"), QStringLiteral("x r,")).isEmpty(), QStringLiteral("unknown profile gives nothing"));
    check(containsRule(added, QStringLiteral("  /etc/extra r,")), QStringLiteral("containsRule ignores indentation"));
    check(withLocalRule(QString(), QStringLiteral("/a r,")) == QStringLiteral("/a r,\n"), QStringLiteral("empty local file"));
    check(withLocalRule(QStringLiteral("# x"), QStringLiteral("/a r,")) == QStringLiteral("# x\n/a r,\n"),
          QStringLiteral("local file without a final newline"));
    check(withLocalRule(QStringLiteral("/a r,\n"), QStringLiteral("/a r,")) == QStringLiteral("/a r,\n"), QStringLiteral("no duplicate rules"));
}

void testTemplate()
{
    const QString text = newProfileTemplate(QStringLiteral("myapp"), QStringLiteral("/opt/my app/bin/run"), QStringLiteral("4.0"));
    const QList<ProfileDeclaration> declarations = parseProfileDeclarations(text);
    check(text.startsWith(QStringLiteral("abi <abi/4.0>,\n")), QStringLiteral("template has the abi line"));
    check(declarations.size() == 1 && declarations.first().name == QStringLiteral("myapp"), QStringLiteral("template parses back"));
    check(declarations.value(0).attachment == QStringLiteral("/opt/my app/bin/run"), QStringLiteral("quoted attachment with a space"));
    check(declaredMode(declarations.value(0).flags) == Mode::Complain, QStringLiteral("new profiles start in complain"));
    check(declarations.value(0).localInclude == QStringLiteral("myapp"), QStringLiteral("template includes local additions"));
    check(!newProfileTemplate(QStringLiteral("a"), QStringLiteral("/a"), QString()).contains(QStringLiteral("abi")),
          QStringLiteral("no abi line when none is installed"));
    check(isSafeProfileName(QStringLiteral("usr.bin.foo")) && isSafeProfileName(QStringLiteral("1password")), QStringLiteral("normal names are safe"));
    check(!isSafeProfileName(QStringLiteral("../etc")) && !isSafeProfileName(QStringLiteral("a/b")) && !isSafeProfileName(QStringLiteral(".x"))
              && !isSafeProfileName(QStringLiteral("a b")),
          QStringLiteral("paths, dots and spaces are refused"));
    check(fileNameForProfile(QStringLiteral("/usr/bin/foo")) == QStringLiteral("usr.bin.foo"), QStringLiteral("path names map like Ubuntu"));
    check(!isPolicyFileName(QStringLiteral("foo.dpkg-old")) && !isPolicyFileName(QStringLiteral("README"))
              && !isPolicyFileName(QStringLiteral("foo~")) && isPolicyFileName(QStringLiteral("foo")),
          QStringLiteral("package manager leftovers are skipped"));
}

void testGlobs()
{
    const QString firefox = QStringLiteral("/{usr/lib/firefox{,-esr},opt/firefox}/firefox{,-esr,-bin}");
    check(attachmentMatches(firefox, QStringLiteral("/usr/lib/firefox-esr/firefox-esr")), QStringLiteral("nested alternation matches"));
    check(attachmentMatches(firefox, QStringLiteral("/opt/firefox/firefox-bin")), QStringLiteral("second branch matches"));
    check(!attachmentMatches(firefox, QStringLiteral("/usr/lib/firefox/firefoxx")), QStringLiteral("anchored at the end"));
    check(attachmentMatches(QStringLiteral("/usr/bin/*"), QStringLiteral("/usr/bin/ls")), QStringLiteral("star matches a name"));
    check(!attachmentMatches(QStringLiteral("/usr/bin/*"), QStringLiteral("/usr/bin/x/ls")), QStringLiteral("star stops at a slash"));
    check(attachmentMatches(QStringLiteral("/opt/**"), QStringLiteral("/opt/a/b/c")), QStringLiteral("double star crosses folders"));
    check(attachmentMatches(QStringLiteral("/usr/bin/python3.[0-9]"), QStringLiteral("/usr/bin/python3.8")), QStringLiteral("character class"));
    check(attachmentMatches(QStringLiteral("@{HOME}/bin/tool"), QStringLiteral("/home/a/bin/tool")) == false,
          QStringLiteral("attachments that start with a variable are not guessed"));
    check(!attachmentMatches(QStringLiteral("/a{b"), QStringLiteral("/ab")), QStringLiteral("broken glob never matches"));
    check(!attachmentMatches(QStringLiteral("firefox"), QStringLiteral("firefox")), QStringLiteral("names without a path never match"));
}

void testEvents()
{
    const QString audit = QStringLiteral(
        "type=AVC msg=audit(1761990000.123:456): apparmor=\"DENIED\" operation=\"open\" class=\"file\" profile=\"/usr/sbin/tcpdump\" "
        "name=\"/etc/shadow\" pid=1234 comm=\"tcpdump\" requested_mask=\"r\" denied_mask=\"r\" fsuid=0 ouid=0\n");
    const QString kernel = QStringLiteral(
        "2025-11-01T10:20:00+0000 host kernel: audit: type=1400 audit(1761990060.500:457): apparmor=\"ALLOWED\" operation=\"mknod\" "
        "class=\"file\" profile=\"myapp\" name=2F746D702F6120622A63 pid=55 comm=\"myapp\" requested_mask=\"c\" denied_mask=\"c\" fsuid=1000 ouid=1000\n"
        "2025-11-01T10:20:01+0000 host kernel: audit: type=1400 audit(1761990061.000:458): apparmor=\"DENIED\" operation=\"capable\" "
        "class=\"cap\" profile=\"myapp\" pid=55 comm=\"myapp\" capability=12 capname=\"net_admin\"\n"
        "2025-11-01T10:20:02+0000 host kernel: audit: type=1400 audit(1761990062.000:459): apparmor=\"DENIED\" operation=\"create\" "
        "class=\"net\" profile=\"myapp\" pid=55 comm=\"myapp\" family=\"inet6\" sock_type=\"dgram\" protocol=17 requested=\"create\" denied=\"create\"\n"
        "2025-11-01T10:20:03+0000 host kernel: audit: type=1400 audit(1761990063.000:460): apparmor=\"STATUS\" operation=\"profile_replace\" "
        "info=\"same as current profile, skipping\" profile=\"unconfined\" name=\"myapp\" pid=77 comm=\"apparmor_parser\"\n"
        "2025-11-01T10:20:04+0000 host kernel: audit: type=1400 audit(1761990064.000:461): apparmor=\"DENIED\" operation=\"signal\" "
        "class=\"signal\" profile=\"myapp\" pid=55 comm=\"myapp\" requested_mask=\"send\" denied_mask=\"send\" signal=term peer=\"unconfined\"\n"
        "2025-11-01T10:20:05+0000 host kernel: audit: type=1400 audit(1761990065.000:462): apparmor=\"DENIED\" operation=\"exec\" "
        "class=\"file\" profile=\"myapp\" name=\"/usr/bin/ls\" pid=55 comm=\"myapp\" requested_mask=\"x\" denied_mask=\"x\" target=\"myapp//null-/usr/bin/ls\"\n"
        "unrelated line\n");
    const QList<Event> events = parseEvents(audit + kernel + audit);
    check(events.size() == 7, QStringLiteral("events parse and the same audit serial is counted once"));
    check(events.first().operation == QStringLiteral("exec"), QStringLiteral("newest event first"));
    const Event shadow = events.last();
    check(shadow.result == QStringLiteral("DENIED") && shadow.name == QStringLiteral("/etc/shadow") && shadow.pid == 1234,
          QStringLiteral("audit.log fields"));
    check(shadow.time == QDateTime::fromMSecsSinceEpoch(1761990000123LL), QStringLiteral("time comes from the audit stamp"));
    check(suggestedRule(shadow) == QStringLiteral("/etc/shadow r,"), QStringLiteral("file read rule"));

    auto find = [&](const QString &operation) {
        for (const Event &event : events) {
            if (event.operation == operation)
                return event;
        }
        return Event{};
    };
    const Event mknod = find(QStringLiteral("mknod"));
    check(mknod.name == QStringLiteral("/tmp/a b*c"), QStringLiteral("hex-encoded names are decoded"));
    check(suggestedRule(mknod) == QStringLiteral("\"/tmp/a b\\*c\" w,"), QStringLiteral("create becomes w, glob characters escaped, quoted for spaces"));
    check(suggestedRule(find(QStringLiteral("capable"))) == QStringLiteral("capability net_admin,"), QStringLiteral("capability rule"));
    check(find(QStringLiteral("capable")).target == QStringLiteral("capability net_admin"), QStringLiteral("capability target"));
    check(suggestedRule(find(QStringLiteral("create"))) == QStringLiteral("network inet6 dgram,"), QStringLiteral("network rule"));
    check(suggestedRule(find(QStringLiteral("signal"))) == QStringLiteral("signal (send) set=(term) peer=unconfined,"), QStringLiteral("signal rule"));
    check(suggestedRule(find(QStringLiteral("exec"))) == QStringLiteral("/usr/bin/ls ix,"), QStringLiteral("exec rule inherits the profile"));
    check(suggestedRule(find(QStringLiteral("profile_replace"))).isEmpty(), QStringLiteral("status events suggest nothing"));

    Event injected = shadow;
    injected.name = QStringLiteral("/tmp/x r,\n/** rwx");
    check(suggestedRule(injected).isEmpty(), QStringLiteral("a name with a newline cannot inject rules"));
    Event cap = find(QStringLiteral("capable"));
    cap.capability = QStringLiteral("net_admin, /** rwx");
    check(suggestedRule(cap).isEmpty(), QStringLiteral("odd capability names suggest nothing"));

    check(topLevelProfile(QStringLiteral("myapp//null-/usr/bin/ls")) == QStringLiteral("myapp"), QStringLiteral("child profile maps to its parent"));

    const QList<HourBucket> buckets = hourlyBuckets(events, QDateTime::fromMSecsSinceEpoch(1761990065000LL), 3);
    int denied = 0;
    int allowed = 0;
    for (const HourBucket &bucket : buckets) {
        denied += bucket.denied;
        allowed += bucket.allowed;
    }
    check(buckets.size() == 3 && denied == 5 && allowed == 1, QStringLiteral("hourly buckets count denials and complain-mode allowances"));
}

void testLabelsAndSections()
{
    check(parseLabel(QStringLiteral("unconfined\n")).mode == Mode::Unconfined, QStringLiteral("unconfined label"));
    const ProcessLabel label = parseLabel(QStringLiteral("snap.firefox.firefox (enforce)\n"));
    check(label.profile == QStringLiteral("snap.firefox.firefox") && label.mode == Mode::Enforce, QStringLiteral("profile label with mode"));
    check(parseLabel(QStringLiteral("a//&b (complain)")).mode == Mode::Complain, QStringLiteral("stacked label"));
    const QList<LoadedProfile> loaded = parseLoadedProfiles(QStringLiteral("firefox (unconfined)\n/usr/sbin/tcpdump (enforce)\nbad line\nmy app (complain)\n"));
    check(loaded.size() == 3, QStringLiteral("loaded profile list"));
    check(loaded.value(2).name == QStringLiteral("my app") && loaded.value(2).mode == Mode::Complain, QStringLiteral("names with spaces"));
    const QHash<QString, QString> sections = splitSections(QStringLiteral("noise\n@@events\na\nb\n@@profiles\nx (enforce)\n"));
    check(sections.value(QStringLiteral("events")) == QStringLiteral("a\nb\n"), QStringLiteral("events section"));
    check(sections.value(QStringLiteral("profiles")) == QStringLiteral("x (enforce)\n\n"), QStringLiteral("profiles section"));
}

void testSnapshot()
{
    QTemporaryDir dir;
    const QString root = dir.path();
    writeFile(root + QStringLiteral("/sys/module/apparmor/parameters/enabled"), QStringLiteral("Y\n"));
    writeFile(root + QStringLiteral("/sys/kernel/security/lsm"), QStringLiteral("lockdown,capability,landlock,yama,apparmor"));
    writeFile(root + QStringLiteral("/usr/sbin/apparmor_parser"), QStringLiteral(""));
    writeFile(root + QStringLiteral("/etc/apparmor.d/firefox"), firefoxProfile);
    writeFile(root + QStringLiteral("/etc/apparmor.d/usr.sbin.tcpdump"), pathProfile);
    writeFile(root + QStringLiteral("/etc/apparmor.d/usr.sbin.tcpdump.dpkg-old"), pathProfile);
    writeFile(root + QStringLiteral("/etc/apparmor.d/two"), twoProfiles);
    writeFile(root + QStringLiteral("/etc/apparmor.d/abstractions/base"), QStringLiteral("/etc/ld.so.cache r,\n"));
    writeFile(root + QStringLiteral("/etc/apparmor.d/tunables-like"), QStringLiteral("@{HOME}=/home/*/\n"));
    writeFile(root + QStringLiteral("/etc/apparmor.d/abi/4.0"), QString());
    writeFile(root + QStringLiteral("/etc/apparmor.d/disable/two"), QString());
    writeFile(root + QStringLiteral("/var/lib/snapd/apparmor/profiles/snap.hello.hello"), QStringLiteral("profile \"snap.hello.hello\" {\n}\n"));
    writeBytes(root + QStringLiteral("/proc/10/cmdline"), QByteArray("/usr/sbin/tcpdump\0-i\0any\0", 26));
    writeFile(root + QStringLiteral("/proc/10/attr/apparmor/current"), QStringLiteral("/usr/sbin/tcpdump (complain)\n"));
    writeBytes(root + QStringLiteral("/proc/11/cmdline"), QByteArray("/usr/lib/firefox/firefox\0", 25));
    writeFile(root + QStringLiteral("/proc/11/attr/current"), QStringLiteral("unconfined\n"));
    writeFile(root + QStringLiteral("/proc/12/comm"), QStringLiteral("kthreadd\n"));
    writeBytes(root + QStringLiteral("/proc/13/cmdline"), QByteArray("postgres: 17/main: checkpointer ", 32));
    writeFile(root + QStringLiteral("/proc/13/comm"), QStringLiteral("postgres\n"));
    writeFile(root + QStringLiteral("/proc/13/attr/apparmor/current"), QStringLiteral("unconfined\n"));
    writeFile(root + QStringLiteral("/proc/self/cmdline"), QStringLiteral("x"));

    const Snapshot snapshot = readSnapshot(root, false);
    check(snapshot.supported && snapshot.enabled && snapshot.inLsmList, QStringLiteral("kernel state is read"));
    check(snapshot.parserPath == QStringLiteral("/usr/sbin/apparmor_parser"), QStringLiteral("parser is found under the root"));
    check(snapshot.abiVersions == QStringList{QStringLiteral("4.0")}, QStringLiteral("abi versions"));
    QStringList names;
    for (const ProfileFile &file : snapshot.files)
        names << file.source + QLatin1Char(':') + file.fileName;
    check(names == QStringList{QStringLiteral("system:firefox"), QStringLiteral("system:two"), QStringLiteral("system:usr.sbin.tcpdump"),
                               QStringLiteral("snap:snap.hello.hello")},
          QStringLiteral("policy files, leftovers and non-profile files skipped: ") + names.join(QLatin1Char(',')));
    bool disabled = false;
    bool snapEditable = true;
    for (const ProfileFile &file : snapshot.files) {
        if (file.fileName == QStringLiteral("two"))
            disabled = file.disabled;
        if (file.source == QStringLiteral("snap"))
            snapEditable = file.editable;
    }
    check(disabled, QStringLiteral("disable/ link marks a profile disabled"));
    check(!snapEditable, QStringLiteral("snap profiles are read-only"));
    check(snapshot.processes.size() == 3, QStringLiteral("kernel threads and non-numeric entries are skipped"));
    check(snapshot.processes.value(0).label.mode == Mode::Complain && snapshot.processes.value(0).command == QStringLiteral("/usr/sbin/tcpdump -i any"),
          QStringLiteral("process label and command"));
    check(snapshot.processes.value(1).label.mode == Mode::Unconfined && snapshot.processes.value(1).executable == QStringLiteral("/usr/lib/firefox/firefox"),
          QStringLiteral("legacy attr/current and executable from the command line"));
    check(snapshot.processes.value(1).name == QStringLiteral("firefox"), QStringLiteral("name from the executable"));
    check(snapshot.processes.value(2).name == QStringLiteral("postgres") && snapshot.processes.value(2).executable.isEmpty(),
          QStringLiteral("a rewritten process title is not taken for a path; comm gives the name"));
}

void testInstallScript()
{
    const QString sh = QStandardPaths::findExecutable(QStringLiteral("sh"));
    if (sh.isEmpty()) {
        qInfo("SKIP: no sh");
        return;
    }
    QTemporaryDir dir;
    const QString root = dir.path();
    const QString bin = root + QStringLiteral("/bin");
    writeFile(bin + QStringLiteral("/apparmor_parser"), QStringLiteral("#!/bin/sh\necho \"$@\" >> \"$PARSER_LOG\"\ngrep -q REJECT \"$3\" && exit 1\nexit 0\n"));
    QFile::setPermissions(bin + QStringLiteral("/apparmor_parser"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString dest = root + QStringLiteral("/policy/app");
    const QString backups = root + QStringLiteral("/backups");
    writeFile(dest, QStringLiteral("old\n"));
    writeFile(root + QStringLiteral("/good"), QStringLiteral("new\n"));
    writeFile(root + QStringLiteral("/bad"), QStringLiteral("REJECT\n"));

    QString script = adminInstallScript();
    script.replace(QStringLiteral("PATH=/usr/sbin:/usr/bin:/sbin:/bin"), QStringLiteral("PATH=%1:/usr/bin:/bin").arg(bin));
    auto run = [&](const QString &src, const QString &target) {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("PARSER_LOG"), root + QStringLiteral("/parser.log"));
        process.setProcessEnvironment(environment);
        process.start(sh, {QStringLiteral("-c"), script, QStringLiteral("sh"), src, target, backups, target});
        process.waitForFinished(10000);
        return process.exitCode();
    };
    auto read = [](const QString &path) {
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        return QString::fromUtf8(file.readAll());
    };
    check(run(root + QStringLiteral("/good"), dest) == 0, QStringLiteral("install succeeds"));
    check(read(dest) == QStringLiteral("new\n"), QStringLiteral("new file installed"));
    check(QDir(backups).entryList(QDir::Files).size() == 1, QStringLiteral("old file kept in the backup folder"));
    check(read(root + QStringLiteral("/parser.log")).contains(QStringLiteral("-r -W ") + dest), QStringLiteral("profile reloaded"));
    check(run(root + QStringLiteral("/bad"), dest) == 3, QStringLiteral("rejected profile exits 3"));
    check(read(dest) == QStringLiteral("new\n"), QStringLiteral("rejected profile is rolled back"));
    const QString fresh = root + QStringLiteral("/policy/local/fresh");
    check(run(root + QStringLiteral("/bad"), fresh) == 3 && !QFileInfo::exists(fresh), QStringLiteral("rejected new file is moved aside, not left loaded"));
    bool movedAside = false;
    for (const QString &name : QDir(backups).entryList(QDir::Files))
        movedAside = movedAside || name.contains(QStringLiteral(".rejected."));
    check(movedAside, QStringLiteral("rejected new file is kept in the backup folder"));
}

void testAdvice()
{
    check(programDescription(QStringLiteral("/usr/sbin/cupsd")).contains(QStringLiteral("printing")), QStringLiteral("path profile names are described"));
    check(programDescription(QStringLiteral("libreoffice-soffice")).contains(QStringLiteral("LibreOffice")), QStringLiteral("prefix descriptions"));
    check(programDescription(QStringLiteral("snap.firefox.firefox")).contains(QStringLiteral("Snap")), QStringLiteral("snap profiles are described"));
    check(programDescription(QStringLiteral("codex")).isEmpty(), QStringLiteral("exact names do not match longer ones"));
    check(!modeExplanation(Mode::Complain).isEmpty() && modeExplanation(Mode::Unknown).isEmpty(), QStringLiteral("mode explanations"));

    ProfileFacts facts;
    facts.hasFile = true;
    facts.liveKnown = true;
    facts.loaded = true;
    facts.logKnown = true;
    facts.mode = Mode::Enforce;
    QList<Advice> advice = profileAdvice(facts);
    check(advice.size() == 1 && advice.first().tone == AdviceTone::Good, QStringLiteral("quiet enforce profile is fine"));
    facts.denied = 3;
    advice = profileAdvice(facts);
    check(advice.size() == 1 && advice.first().action == AdviceAction::Events && advice.first().text.contains(QStringLiteral("3 accesses")),
          QStringLiteral("enforce with denials points at the events"));
    facts.mode = Mode::Complain;
    facts.denied = 0;
    facts.processes = 2;
    advice = profileAdvice(facts);
    check(advice.size() == 1 && advice.first().action == AdviceAction::Enforce, QStringLiteral("quiet running complain profile suggests enforce"));
    facts.allowed = 1;
    advice = profileAdvice(facts);
    check(advice.size() == 1 && advice.first().action == AdviceAction::Events && advice.first().text.contains(QStringLiteral("1 access ")),
          QStringLiteral("complain with allowed accesses points at the events"));
    facts.loaded = false;
    facts.missed = 1;
    advice = profileAdvice(facts);
    check(advice.size() == 3 && advice.first().action == AdviceAction::Reload && advice.at(1).text.contains(QStringLiteral("1 running process")),
          QStringLiteral("unloaded profile and missed processes"));
    facts.logKnown = false;
    advice = profileAdvice(facts);
    check(advice.last().action == AdviceAction::LiveView, QStringLiteral("unreadable log suggests live view"));
    facts.disabled = true;
    advice = profileAdvice(facts);
    check(advice.size() == 1 && advice.first().action == AdviceAction::Enable, QStringLiteral("disabled profile suggests enable"));
    facts.unconfinedFlag = true;
    advice = profileAdvice(facts);
    check(advice.size() == 1 && advice.first().action == AdviceAction::None, QStringLiteral("unconfined flag is left alone"));
    facts.hasFile = false;
    advice = profileAdvice(facts);
    check(advice.size() == 1 && advice.first().text.contains(QStringLiteral("another tool")), QStringLiteral("profile without a file"));
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testDeclarations();
    testWithMode();
    testRules();
    testTemplate();
    testGlobs();
    testEvents();
    testLabelsAndSections();
    testSnapshot();
    testInstallScript();
    testAdvice();
    if (failures)
        qCritical("%d failure(s)", failures);
    return failures == 0 ? 0 : 1;
}

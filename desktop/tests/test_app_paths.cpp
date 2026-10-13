// The projects layout: where every ForkMesh file lives, how legacy locations
// move into it, and what the storage map reports. Runs entirely inside a
// temporary directory standing in for the home folder.

#include "AppPaths.h"
#include "ProjectRegistry.h"
#include "SourceCheckout.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::paths;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what)
{
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

Environment environmentIn(const QString &home)
{
    Environment environment;
    environment.userHome = home;
    environment.legacyData = home + "/.local/share/ForkMesh/ForkMesh";
    environment.legacyConfig = home + "/.config/ForkMesh";
    environment.legacyCache = home + "/.cache/ForkMesh/ForkMesh";
    environment.legacyState = home + "/.forkmesh";
    environment.userSshDir = home + "/.ssh";
    return environment;
}

void seedLegacyInstall(const QString &home)
{
    writeFile(home + "/.local/share/ForkMesh/ForkMesh/identity/ed25519.pem", "KEY");
    writeFile(home + "/.local/share/ForkMesh/ForkMesh/ssh/vultr_mirror_ed25519", "FLEET");
    writeFile(home + "/.local/share/ForkMesh/ForkMesh/ssh/known_hosts", "hosts");
    writeFile(home + "/.config/ForkMesh/ForkMesh.conf", "[General]\ntheme=dark\n");
    writeFile(home + "/.config/git/config", "[user]\n");  // not ours
    writeFile(home + "/.cache/ForkMesh/ForkMesh/chat-media/a", "x");
    writeFile(home + "/.forkmesh/diagnostics/stalls.log", "stall");
    writeFile(home + "/.forkmesh/toolchain/jdk/release", "21");
    writeFile(home + "/.forkmesh/forkmesh.lock", "1");
    writeFile(home + "/.ssh/forkmesh_jett_ed25519", "PRIVATE");
    writeFile(home + "/.ssh/forkmesh_jett_ed25519.pub", "PUBLIC");
    writeFile(home + "/.ssh/forkmesh_known_hosts", "known");
    writeFile(home + "/.ssh/id_ed25519", "PERSONAL");
    writeFile(home + "/.ssh/config",
              "Host ssh.forkmesh.com\n"
              "  User git\n"
              "  IdentityFile " + home.toUtf8() + "/.ssh/forkmesh_jett_ed25519\n"
              "  UserKnownHostsFile ~/.ssh/forkmesh_known_hosts\n"
              "Host personal\n"
              "  IdentityFile ~/.ssh/id_ed25519\n");
}

void testSlugs()
{
    check(slugify("My Project!") == "my-project", "slugify collapses punctuation");
    check(slugify("  --Alpha__Beta--  ") == "alpha-beta", "slugify trims separators");
    check(slugify("../../etc") == "etc", "slugify cannot climb out of the root");
    check(slugify("???").isEmpty(), "slugify of nothing usable is empty");
    check(isValidSlug("forkmesh") && isValidSlug("a") && isValidSlug("a-1"),
          "valid slugs accepted");
    check(!isValidSlug("") && !isValidSlug("-a") && !isValidSlug("a-") &&
              !isValidSlug("a/b") && !isValidSlug("..") && !isValidSlug("A"),
          "invalid slugs rejected");
}

void testFreshInstallUsesLayout()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    const Layout layout = resolve(environmentIn(home));
    check(layout.projectsRoot == home + "/projects", "projects root defaults to ~/projects");
    check(layout.homeDir == home + "/projects/forkmesh", "home project is ~/projects/forkmesh");
    check(layout.isHomeProject() && layout.rerootAllowed, "fresh install may use the layout");
    check(layout.data == home + "/projects/forkmesh/data", "fresh data root is in the layout");
    check(layout.config == home + "/projects/forkmesh/config",
          "fresh config root ignores an unrelated ~/.config");
    check(layout.ssh == home + "/projects/forkmesh/ssh", "fresh ssh root is in the layout");
    check(layout.sourceDir() == home + "/projects/forkmesh/src", "source lives in src/");
    check(planMigration(environmentIn(home), layout).size() > 0 &&
              applyMigration(planMigration(environmentIn(home), layout), false)
                      .first().outcome == MoveResult::Outcome::AlreadyDone,
          "migrating nothing is a no-op");
}

void testCheckoutBlocksReroot()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    seedLegacyInstall(home);
    QDir().mkpath(home + "/projects/forkmesh/.git");  // home is still the checkout
    const Environment environment = environmentIn(home);
    const Layout layout = resolve(environment);
    check(!layout.rerootAllowed, "a git checkout blocks re-rooting");
    check(!layout.rerootBlockedReason.isEmpty(), "the block explains itself");
    check(layout.data == environment.legacyData, "blocked: data stays legacy");
    check(layout.config == home + "/.config", "blocked: config root stays ~/.config");
    check(layout.state == home + "/.forkmesh", "blocked: state stays ~/.forkmesh");
    check(layout.diagnosticsDir() == home + "/.forkmesh/diagnostics",
          "blocked: diagnostics stay where tools read them");
    check(layout.ssh == environment.legacyData + "/ssh", "blocked: ssh stays in app data");
    check(planMigration(environment, layout).isEmpty(), "blocked: nothing is planned");
    check(!QFileInfo::exists(home + "/projects/forkmesh/data"),
          "blocked: nothing is written into the checkout");
}

void testMigrationMovesEverything()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    seedLegacyInstall(home);
    QDir().mkpath(home + "/projects/forkmesh/src/.git");  // checkout already in src/
    const Environment environment = environmentIn(home);
    Layout layout = resolve(environment);
    check(layout.rerootAllowed, "a checkout in src/ does not block re-rooting");
    check(layout.data == environment.legacyData,
          "before migrating, legacy data is still what the app would use");

    const QVector<Move> plan = planMigration(environment, layout);
    const QVector<MoveResult> dry = applyMigration(plan, /*dryRun=*/true);
    check(QFileInfo::exists(environment.legacyData + "/identity/ed25519.pem") &&
              !QFileInfo(environment.legacyData).isSymLink(),
          "a dry run moves nothing");
    int wouldMove = 0;
    for (const MoveResult &result : dry)
        wouldMove += result.outcome == MoveResult::Outcome::Moved ? 1 : 0;
    check(wouldMove >= 8, "the dry run reports every legacy location");
    bool sawCarvedSsh = false;
    for (const MoveResult &result : dry) {
        if (result.move.to.endsWith("/projects/forkmesh/ssh"))
            sawCarvedSsh = result.outcome == MoveResult::Outcome::Moved;
    }
    check(sawCarvedSsh, "the dry run sees through its own pretended moves");

    const QVector<MoveResult> results = applyMigration(plan, false);
    for (const MoveResult &result : results) {
        if (result.outcome == MoveResult::Outcome::Failed ||
            result.outcome == MoveResult::Outcome::Skipped) {
            std::printf("     %s -> %s : %s\n", qPrintable(result.move.from),
                        qPrintable(result.move.to), qPrintable(result.detail));
        }
    }
    const QString root = home + "/projects/forkmesh";
    check(readFile(root + "/data/identity/ed25519.pem") == "KEY", "identity key moved");
    check(readFile(root + "/ssh/vultr_mirror_ed25519") == "FLEET",
          "managed ssh keys carved out of data into ssh/");
    check(readFile(root + "/config/ForkMesh/ForkMesh.conf").contains("theme=dark"),
          "settings file moved to config/ForkMesh");
    check(QFileInfo::exists(home + "/.config/git/config"), "unrelated ~/.config untouched");
    check(readFile(root + "/logs/diagnostics/stalls.log") == "stall", "diagnostics moved");
    check(readFile(root + "/toolchain/jdk/release") == "21", "toolchains moved");
    check(readFile(root + "/cache/chat-media/a") == "x", "cache moved");
    check(readFile(root + "/ssh/user/forkmesh_jett_ed25519") == "PRIVATE",
          "ForkMesh-named user key moved");
    check((QFile::permissions(root + "/ssh/user") &
           (QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther |
            QFileDevice::ExeOther)) == 0,
          "the folder made for user keys is owner-only");
    check(readFile(root + "/ssh/user/forkmesh_known_hosts") == "known",
          "ForkMesh known_hosts moved");
    check(readFile(home + "/.ssh/id_ed25519") == "PERSONAL", "personal key left alone");
    check(!QFileInfo::exists(home + "/.ssh/forkmesh_jett_ed25519") &&
              !QFileInfo(home + "/.ssh/forkmesh_jett_ed25519").isSymLink(),
          "no key material or link left in ~/.ssh");

    check(updateUserSshConfig(environment, results), "~/.ssh/config was updated");
    const QByteArray sshConfig = readFile(home + "/.ssh/config");
    check(sshConfig.contains("IdentityFile " + root.toUtf8() +
                             "/ssh/user/forkmesh_jett_ed25519\n"),
          "~/.ssh/config follows the moved key (absolute form)");
    check(sshConfig.contains("UserKnownHostsFile " + root.toUtf8() +
                             "/ssh/user/forkmesh_known_hosts\n"),
          "~/.ssh/config follows the moved known_hosts (~ form)");
    check(sshConfig.contains("IdentityFile ~/.ssh/id_ed25519"),
          "~/.ssh/config keeps personal entries");
    check(readFile(home + "/.ssh/config.before-forkmesh-projects")
              .contains("/.ssh/forkmesh_jett_ed25519"),
          "~/.ssh/config backup holds the original");
    check(!updateUserSshConfig(environment, results),
          "updating ~/.ssh/config twice changes nothing");

    // Compatibility links keep every outside reference working.
    check(QFileInfo(environment.legacyData).isSymLink() &&
              readFile(environment.legacyData + "/identity/ed25519.pem") == "KEY",
          "old data path still resolves through a link");
    check(readFile(environment.legacyData + "/ssh/vultr_mirror_ed25519") == "FLEET",
          "old managed-key path still resolves (ssh_config entries keep working)");
    check(readFile(home + "/.config/ForkMesh/ForkMesh.conf").contains("theme=dark"),
          "old settings path still resolves through a link");

    layout = resolve(environment);
    check(layout.state == home + "/.forkmesh",
          "the state root stays legacy while its lock file exists");
    QFile::remove(home + "/.forkmesh/forkmesh.lock");
    layout = resolve(environment);
    check(layout.state == root + "/state",
          "once the legacy lock is gone the state root resolves into the project");
    check(layout.data == root + "/data" && layout.config == root + "/config" &&
              layout.ssh == root + "/ssh" && layout.logs == root + "/logs" &&
              layout.cache == root + "/cache",
          "after migrating, every root resolves inside the project");

    const QVector<MoveResult> again = applyMigration(planMigration(environment, layout), false);
    bool idempotent = true;
    for (const MoveResult &result : again)
        idempotent = idempotent && result.outcome == MoveResult::Outcome::AlreadyDone;
    check(idempotent, "running the migration twice changes nothing");
}

void testToolFilesInConfigDoNotHideLegacySettings()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    seedLegacyInstall(home);
    // The box-level script has already moved tool secrets into config/.
    writeFile(home + "/projects/forkmesh/config/tools/secrets/worker.data-key", "k");
    const Environment environment = environmentIn(home);
    Layout layout = resolve(environment);
    check(layout.config == home + "/.config",
          "settings stay legacy until the settings folder itself has moved");
    applyMigration(planMigration(environment, layout), false);
    layout = resolve(environment);
    check(layout.config == home + "/projects/forkmesh/config" &&
              readFile(layout.config + "/ForkMesh/ForkMesh.conf").contains("theme=dark") &&
              readFile(layout.config + "/tools/secrets/worker.data-key") == "k",
          "settings move in beside the tool files already there");
}

void testXdgSandboxIsRespected()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    // A migrated install...
    writeFile(home + "/projects/forkmesh/data/identity/ed25519.pem", "LIVE");
    // ...and a run sandboxed with scratch XDG folders, as the startup benchmark does.
    Environment environment = environmentIn(home);
    environment.legacyData = home + "/scratch/data/ForkMesh/ForkMesh";
    environment.legacyConfig = home + "/scratch/config/ForkMesh";
    environment.legacyCache = home + "/scratch/cache/ForkMesh/ForkMesh";
    environment.xdgSandbox = true;
    Layout layout = resolve(environment);
    check(!layout.rerootAllowed && layout.data == environment.legacyData &&
              layout.config == home + "/scratch/config",
          "a sandboxed run never touches the live project data");
    check(planMigration(environment, layout).isEmpty(), "a sandboxed run migrates nothing");
    environment.homeEnv = home + "/elsewhere/forkmesh";
    layout = resolve(environment);
    check(layout.rerootAllowed && layout.data == home + "/elsewhere/forkmesh/data",
          "FORKMESH_HOME places the layout explicitly, sandbox or not");
}

void testOccupiedDestinationIsNeverMerged()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    seedLegacyInstall(home);
    writeFile(home + "/projects/forkmesh/data/identity/ed25519.pem", "NEWER");
    const Environment environment = environmentIn(home);
    const Layout layout = resolve(environment);
    const QVector<MoveResult> results =
        applyMigration(planMigration(environment, layout), false);
    check(results.first().outcome == MoveResult::Outcome::Skipped,
          "an occupied destination is skipped");
    check(readFile(home + "/projects/forkmesh/data/identity/ed25519.pem") == "NEWER",
          "existing project data is not overwritten");
    check(readFile(environment.legacyData + "/identity/ed25519.pem") == "KEY",
          "legacy data is left intact");
}

void testSshConfigRewrite()
{
    const QString text =
        "Host ssh.forkmesh.com\n"
        "  IdentityFile /home/u/.ssh/forkmesh_jett_ed25519\n"
        "  UserKnownHostsFile=\"~/.ssh/forkmesh_known_hosts\"\n"
        "  CertificateFile /home/u/.ssh/forkmesh_jett_ed25519-cert.pub\n"
        "Host mirror\n"
        "  identityfile /home/u/.local/share/ForkMesh/ForkMesh/ssh/vultr_mirror_ed25519\n"
        "  # IdentityFile /home/u/.ssh/forkmesh_jett_ed25519 stays a comment\n"
        "Host other\n"
        "  IdentityFile /home/u/.ssh/forkmesh_jett_ed25519_other\n";
    const QString out = rewriteSshConfig(
        text, {{"/home/u/.ssh/forkmesh_jett_ed25519", "/p/ssh/user/forkmesh_jett_ed25519"},
               {"~/.ssh/forkmesh_known_hosts", "/p/ssh/user/forkmesh_known_hosts"},
               {"/home/u/.local/share/ForkMesh/ForkMesh/ssh", "/p/ssh"}});
    check(out.contains("  IdentityFile /p/ssh/user/forkmesh_jett_ed25519\n"),
          "ssh_config: exact path rewritten, indentation kept");
    check(out.contains("UserKnownHostsFile=\"/p/ssh/user/forkmesh_known_hosts\""),
          "ssh_config: '=' form and quotes kept");
    check(out.contains("  identityfile /p/ssh/vultr_mirror_ed25519\n"),
          "ssh_config: directory relocation and any keyword case");
    check(out.contains("CertificateFile /home/u/.ssh/forkmesh_jett_ed25519-cert.pub"),
          "ssh_config: a different file sharing the prefix is untouched");
    check(out.contains("forkmesh_jett_ed25519_other"),
          "ssh_config: a longer file name is untouched");
    check(out.contains("# IdentityFile /home/u/.ssh/forkmesh_jett_ed25519 stays"),
          "ssh_config: comments are untouched");
    check(rewriteSshConfig(text, {}) == text, "ssh_config: no relocations, no change");
}

void testSavedPathsAreRepointed()
{
    const QString text =
        "mirrorPath=/home/u/.local/share/ForkMesh/ForkMesh/mirrors/a.git\n"
        "json=\"[{\\\"dir\\\":\\\"/home/u/.local/share/ForkMesh/ForkMesh\\\"}]\"\n"
        "key=/home/u/.local/share/ForkMesh/ForkMesh/ssh/fleet\n"
        "other=/home/u/.local/share/ForkMesh/ForkMesh-other/x\n"
        "already=/p/data/mirrors/b.git\n";
    const QString out = repointText(
        text, {{"/home/u/.local/share/ForkMesh/ForkMesh", "/p/data"},
               {"/home/u/.local/share/ForkMesh/ForkMesh/ssh", "/p/ssh"}});
    check(out.contains("mirrorPath=/p/data/mirrors/a.git\n"), "repoint: a path beneath a root");
    check(out.contains("\\\"dir\\\":\\\"/p/data\\\""), "repoint: a whole path inside escaped JSON");
    check(out.contains("key=/p/ssh/fleet\n"), "repoint: the longer root wins over its parent");
    check(out.contains("other=/home/u/.local/share/ForkMesh/ForkMesh-other/x"),
          "repoint: a look-alike sibling is untouched");
    check(out.contains("already=/p/data/mirrors/b.git"), "repoint: new paths are untouched");
    check(repointText(out, {{"/home/u/.local/share/ForkMesh/ForkMesh", "/p/data"},
                            {"/home/u/.local/share/ForkMesh/ForkMesh/ssh", "/p/ssh"}}) == out,
          "repoint: running it twice changes nothing");
}

void testOtherProjectIsIsolated()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    seedLegacyInstall(home);
    Environment environment = environmentIn(home);
    environment.projectArg = "Side-Quest";
    const Layout layout = resolve(environment);
    const QString dir = home + "/projects/side-quest";
    check(layout.projectSlug == "side-quest" && layout.projectDir == dir,
          "--project selects ~/projects/<slug>");
    check(layout.data == dir + "/data" && layout.config == dir + "/config" &&
              layout.logs == dir + "/logs" && layout.ssh == dir + "/ssh" &&
              layout.state == dir + "/state" && layout.cache == dir + "/cache",
          "another project never falls back to legacy or home-project files");
    check(layout.toolchainDir() == home + "/projects/forkmesh/toolchain",
          "toolchains stay shared in the ForkMesh home");
    check(planMigration(environment, layout).isEmpty(),
          "opening another project migrates nothing");

    environment.projectArg = "../../etc";
    check(resolve(environment).isHomeProject(), "a hostile project name falls back to home");
    environment.projectArg.clear();
    environment.projectEnv = "envproj";
    check(resolve(environment).projectSlug == "envproj", "FORKMESH_PROJECT selects a project");
    environment.projectArg = "argproj";
    check(resolve(environment).projectSlug == "argproj", "--project beats the environment");
}

void testStorageMap()
{
    QTemporaryDir tmp;
    const QString home = tmp.path();
    seedLegacyInstall(home);
    writeFile(home + "/.cache/forkmesh-builds/a/CMakeCache.txt", "x");
    writeFile(home + "/.forkmesh_mcp_token", "t");
    writeFile(home + "/projects/side-quest/project.json", "{}");
    writeFile(home + "/projects/not-a-project/readme", "x");
    const Environment environment = environmentIn(home);
    Layout layout = resolve(environment);

    auto has = [](const QVector<Location> &map, const QString &path) {
        for (const Location &location : map)
            if (location.path == path)
                return true;
        return false;
    };
    QVector<Location> map = storageMap(environment, layout);
    check(has(map, home + "/.cache/forkmesh-builds"), "map finds uncatalogued cache folders");
    check(has(map, home + "/.forkmesh_mcp_token"), "map finds home-level token files");
    check(has(map, home + "/.ssh/forkmesh_jett_ed25519"), "map finds ForkMesh keys in ~/.ssh");
    check(!has(map, home + "/.ssh/id_ed25519"), "map ignores personal keys");
    check(has(map, home + "/projects/side-quest"), "map lists sibling projects");
    check(!has(map, home + "/projects/not-a-project"), "map ignores folders that are not projects");

    applyMigration(planMigration(environment, layout), false);
    layout = resolve(environment);
    map = storageMap(environment, layout);
    bool linkFlagged = false;
    for (const Location &location : map) {
        if (location.path == environment.legacyData)
            linkFlagged = location.isCompatLink && location.scope == Location::Scope::Legacy;
    }
    check(linkFlagged, "map marks what is left outside as a compatibility link");
    check(has(map, home + "/projects/forkmesh/ssh"), "map lists the project's ssh folder");
}

void testProjectRegistry()
{
    namespace projects = forkmesh::projects;
    QTemporaryDir tmp;
    const QString home = tmp.path();
    writeFile(home + "/projects/forkmesh/data/identity/ed25519.pem", "KEY");
    writeFile(home + "/projects/forkmesh/logo.png", "PNG");
    writeFile(home + "/logo-source.png", "NEWLOGO");
    const Layout layout = resolve(environmentIn(home));

    QVector<projects::Project> all = projects::list(layout.projectsRoot, layout.homeDir);
    check(all.size() == 1 && all.first().isHome && all.first().name == "ForkMesh" &&
              all.first().logoPath.endsWith("/forkmesh/logo.png"),
          "the home project is always listed, with its logo");

    projects::CreateRequest request;
    request.name = "Side Quest";
    request.description = "An experiment";
    request.logoSource = home + "/logo-source.png";
    request.instances = {{"Work", "wss://work.example.com/room"},
                         {"ForkMesh", "wss://app.forkmesh.com/room"},
                         {"Again", "wss://work.example.com/room"},
                         {"Blank", "  "}};
    request.packages = {"notes"};
    request.removedPackages = {"crm", "notes", "calendar"};
    projects::Project created;
    QString error;
    check(projects::create(layout, request, &created, &error), "a project can be created");
    const QString dir = home + "/projects/side-quest";
    check(created.slug == "side-quest" && created.dir == dir && created.name == "Side Quest" &&
              created.offline && created.packages == QStringList{"notes"},
          "the created project reads back from project.json");
    bool roots = true;
    for (const char *root : {"data", "config", "cache", "state", "logs", "ssh"})
        roots = roots && QFileInfo(dir + "/" + root).isDir();
    check(roots, "every isolated root exists");
    check(readFile(dir + "/logo.png") == "NEWLOGO", "the logo is copied in");
    check(readFile(dir + "/data/identity/ed25519.pem") == "KEY" &&
              (QFile::permissions(dir + "/data/identity/ed25519.pem") &
               (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0,
          "the identity key is copied, owner-only");
    {
        QSettings seed(projects::settingsFilePath(dir), QSettings::IniFormat);
        check(seed.value("market/packages/crm/state").toString() == "removed" &&
                  seed.value("market/packages/calendar/state").toString() == "removed",
              "plugins the project starts without are seeded as removed");
        check(seed.value("market/packages/notes/state").toString() == "installed",
              "a chosen plugin wins over the removal list");
        check(seed.value("network/offlineMode").toBool() &&
                  !seed.value("network/offlineModeAutomatic").toBool(),
              "a new project starts offline, connected to no instance");
        check(seed.value(projects::kInstancePendingSetting).toBool(),
              "the instance is left for the project to choose when it opens");
        const QVector<projects::InstanceChoice> choices = projects::decodeInstanceChoices(
            seed.value(projects::kInstanceChoicesSetting).toString());
        check(choices.size() == 2 && choices.at(0).name == "Work" &&
                  choices.at(0).url == "wss://work.example.com/room" &&
                  choices.at(1).url == "wss://app.forkmesh.com/room",
              "the instances to offer are seeded once each, in order, without blanks");
        check(!seed.contains("servers/items") && !seed.contains("server/url"),
              "no instance is saved for the project before it chooses one");
    }
    check(projects::decodeInstanceChoices("not json").isEmpty(),
          "unreadable instance choices decode to none");
    check(!projects::create(layout, request, nullptr, &error) && !error.isEmpty(),
          "an existing folder is never adopted");
    request.name = "forkmesh";
    check(!projects::create(layout, request, nullptr, &error), "the home name is reserved");
    request.name = "???";
    check(!projects::create(layout, request, nullptr, &error), "an unusable name is refused");
    request.name = "Bad Logo";
    request.logoSource = home + "/projects/forkmesh/data/identity/ed25519.pem";
    check(!projects::create(layout, request, nullptr, &error) &&
              !QFileInfo::exists(home + "/projects/bad-logo"),
          "a non-image logo is refused before anything is created");

    request.name = "No Identity";
    request.logoSource.clear();
    request.shareIdentity = false;
    check(projects::create(layout, request, nullptr, &error) &&
              !QFileInfo::exists(home + "/projects/no-identity/data/identity"),
          "an isolated identity is possible");

    writeFile(home + "/projects/plain-folder/readme", "x");
    all = projects::list(layout.projectsRoot, layout.homeDir);
    writeFile(home + "/projects/Not A Slug/readme", "x");
    writeFile(home + "/projects/.hidden/readme", "x");
    all = projects::list(layout.projectsRoot, layout.homeDir);
    check(all.size() == 4 && all.at(0).isHome && all.at(1).slug == "no-identity" &&
              all.at(2).slug == "plain-folder" && all.at(3).slug == "side-quest",
          "list: home first, then every folder by name, plain folders included");
    check(all.at(1).ownsFolder && !all.at(2).ownsFolder && all.at(2).name == "plain-folder" &&
              !all.at(0).ownsFolder,
          "only folders ForkMesh created are its own");

    check(!projects::remove(layout.projectsRoot, "forkmesh", false, &error) &&
              QFileInfo(home + "/projects/forkmesh/logo.png").isFile(),
          "the home project cannot be removed");
    check(!projects::remove(layout.projectsRoot, "plain-folder", false, &error) &&
              QFileInfo(home + "/projects/plain-folder/readme").isFile(),
          "a folder that is not a project is never deleted");
    check(projects::updateDetails(home + "/projects/plain-folder", "Plain", "", &error) &&
              !projects::remove(layout.projectsRoot, "plain-folder", false, &error) &&
              QFileInfo(home + "/projects/plain-folder/readme").isFile(),
          "saving a plain folder's details does not make it deletable");
    QFile::remove(home + "/projects/plain-folder/project.json");
    check(projects::remove(layout.projectsRoot, "plain-folder", true, &error) &&
              QFileInfo(home + "/projects/plain-folder/readme").isFile() &&
              QFileInfo(home + "/projects/plain-folder/project.json.removed").isFile(),
          "a plain folder can stop being listed and keeps its files");
    {
        QLockFile open(home + "/projects/side-quest/state/forkmesh.lock");
        check(open.tryLock(0) &&
                  !projects::remove(layout.projectsRoot, "side-quest", false, &error) &&
                  QFileInfo(dir + "/project.json").isFile(),
              "a project open in another process is refused");
    }
    check(projects::remove(layout.projectsRoot, "side-quest", true, &error) &&
              QFileInfo(dir + "/logo.png").isFile() &&
              QFileInfo(dir + "/project.json.removed").isFile() &&
              !QFileInfo::exists(dir + "/project.json"),
          "delete keeping the folder sets project.json aside and leaves the files");
    QString movedTo;
    check(projects::remove(layout.projectsRoot, "no-identity", false, &error, &movedTo) &&
              !QFileInfo::exists(home + "/projects/no-identity") &&
              movedTo.startsWith(QDir::cleanPath(home + "/projects/no-identity") +
                                 ".deleted-") &&
              QFileInfo(movedTo).isDir(),
          "remove without keeping the folder moves it aside and reports where");
    all = projects::list(layout.projectsRoot, layout.homeDir);
    check(all.size() == 1 && all.first().isHome, "deleted projects are no longer listed");
    check(!projects::remove(layout.projectsRoot, "side-quest", false, &error) &&
              QFileInfo(dir + "/logo.png").isFile(),
          "a kept folder is not a project any more");

    const QStringList args = projects::launchArguments(
        {"forkmesh", "--project", "old", "--verbose", "--project=older",
         "forkmesh://fork/x"},
        "side-quest");
    check(args == QStringList({"--verbose", "--project", "side-quest"}),
          "launch arguments replace the project and drop one-shot links");
    check(projects::launchArguments({"forkmesh"}, "forkmesh") ==
              QStringList({"--project", "forkmesh"}),
          "returning home is explicit, so an inherited environment cannot win");
}

// The build knows its sources only as the desktop/ CMake directory inside the
// checkout. Crash and error-log repairs keep their checkpoint under the
// repository that directory belongs to; an exact-path match never found it,
// so the checkpoint followed the Issues tab and a repair completed under
// another repository was never credited.
void testCheckoutContaining()
{
    const QStringList checkouts = {"/home/u/Documents/marketing",
                                   "/home/u/projects/forkmesh/src",
                                   "/home/u/Documents/private-repo"};
    check(checkoutContaining(checkouts, "/home/u/projects/forkmesh/src/desktop") == 1,
          "the desktop source dir belongs to the checkout that contains it");
    check(checkoutContaining(checkouts, "/home/u/projects/forkmesh/src") == 1,
          "a checkout contains its own root");
    check(checkoutContaining(checkouts,
                             "/home/u/projects/forkmesh/src/.worktrees/400/desktop") == 1,
          "a worktree build under the checkout still resolves to it");
    check(checkoutContaining(checkouts, "/home/u/projects/forkmesh/src-old/desktop") == -1,
          "a sibling sharing a name prefix is not inside the checkout");
    check(checkoutContaining(checkouts, "/opt/forkmesh/desktop") == -1,
          "a build outside every checkout matches none");
    check(checkoutContaining({"/home/u/projects/forkmesh/src/", ""},
                             "/home/u/projects/forkmesh/src/desktop/") == 0,
          "trailing slashes and empty paths do not change the answer");
    check(checkoutContaining({"/home/u/projects", "/home/u/projects/forkmesh/src"},
                             "/home/u/projects/forkmesh/src/desktop") == 1,
          "the deepest containing checkout wins");
    check(checkoutContaining(checkouts, QString()) == -1,
          "no source dir matches nothing");
}

} // namespace

// Issue #727: on Windows QSettings::fileName() is a registry key name that
// QFileInfo calls rooted. No path may be derived from it, and a legacy config
// folder must never resolve to <drive>:\HKEY_CURRENT_USER\Software\ForkMesh
// (or, on macOS, to the shared ~/Library/Preferences).
void testRegistrySettingsNameIsNotAFolder()
{
    using namespace forkmesh::paths;
    for (const QString &key :
         {QStringLiteral("\\HKEY_CURRENT_USER\\Software\\ForkMesh\\ForkMesh"),
          QStringLiteral("/HKEY_CURRENT_USER/Software/ForkMesh/ForkMesh"),
          QStringLiteral("\\hkey_local_machine\\Software\\ForkMesh"),
          QStringLiteral("\\HKEY_CURRENT_USER")}) {
        check(isRegistrySettingsName(key), "a registry key name is recognised");
        check(settingsFilePathFor(key, QSettings::NativeFormat).isEmpty() &&
                  settingsFolderFor(key, QSettings::NativeFormat).isEmpty() &&
                  settingsFolderFor(key, QSettings::IniFormat).isEmpty(),
              "a registry key name yields no settings file or folder");
    }
    for (const QString &path :
         {QStringLiteral("/home/u/.config/ForkMesh/ForkMesh.conf"),
          QStringLiteral("/home/u/HKEY_CURRENT_USER/ForkMesh.ini"),
          QStringLiteral("/HKEY-notes/ForkMesh.ini")}) {
        check(!isRegistrySettingsName(path), "a real settings path is not a registry key");
    }
    check(settingsFolderFor(QStringLiteral("/home/u/.config/ForkMesh/ForkMesh.conf"),
                            QSettings::NativeFormat) ==
              QStringLiteral("/home/u/.config/ForkMesh"),
          "a Linux native settings file keeps its folder");
    check(settingsFolderFor(QStringLiteral("/p/forkmesh/config/ForkMesh/ForkMesh.ini"),
                            QSettings::IniFormat) ==
              QStringLiteral("/p/forkmesh/config/ForkMesh"),
          "an INI settings file keeps its folder");
    check(settingsFolderFor(QStringLiteral("relative/ForkMesh.ini"),
                            QSettings::IniFormat).isEmpty() &&
              settingsFolderFor(QString(), QSettings::IniFormat).isEmpty(),
          "relative or empty settings names yield no folder");
}

void testUnattachedProjectBecomesPending()
{
    namespace projects = forkmesh::projects;
    QTemporaryDir temp;
    check(temp.isValid(), "a temporary folder for project settings");
    const QString defaultUrl = QStringLiteral("wss://app.forkmesh.com/room");
    int serial = 0;
    auto settingsAt = [&temp, &serial] {
        return temp.filePath(QStringLiteral("settings-%1.ini").arg(++serial));
    };
    {
        QSettings settings(settingsAt(), QSettings::IniFormat);
        settings.setValue("servers/items",
                          "[{\"id\":\"a\",\"name\":\"app.forkmesh.com\","
                          "\"url\":\"wss://app.forkmesh.com/room\"}]");
        settings.setValue("network/offlineMode", false);
        check(projects::markUnattachedProjectPending(settings, false, defaultUrl),
              "an older project holding only the default instance is marked pending");
        check(settings.value(projects::kInstancePendingSetting).toBool() &&
                  settings.value("network/offlineMode").toBool() &&
                  !settings.value("network/offlineModeAutomatic").toBool(),
              "the older project starts offline until an instance is chosen");
        const QVector<projects::InstanceChoice> choices = projects::decodeInstanceChoices(
            settings.value(projects::kInstanceChoicesSetting).toString());
        check(choices.size() == 1 && choices.at(0).url == defaultUrl,
              "the instance it held is offered back as a choice");
        check(!projects::markUnattachedProjectPending(settings, false, defaultUrl),
              "a pending project is left as it is");
    }
    {
        QSettings settings(settingsAt(), QSettings::IniFormat);
        check(projects::markUnattachedProjectPending(settings, false, defaultUrl) &&
                  settings.value(projects::kInstancePendingSetting).toBool() &&
                  !settings.contains(projects::kInstanceChoicesSetting),
              "a project with no saved instance at all is marked pending");
    }
    {
        QSettings settings(settingsAt(), QSettings::IniFormat);
        check(!projects::markUnattachedProjectPending(settings, true, defaultUrl) &&
                  !settings.contains(projects::kInstancePendingSetting) &&
                  !settings.contains("network/offlineMode"),
              "the home project keeps the default instance");
    }
    {
        QSettings settings(settingsAt(), QSettings::IniFormat);
        settings.setValue("servers/items",
                          "[{\"url\":\"wss://app.forkmesh.com/room\"}]");
        settings.setValue(projects::kInstanceAttachedSetting, true);
        check(!projects::markUnattachedProjectPending(settings, false, defaultUrl) &&
                  !settings.contains(projects::kInstancePendingSetting),
              "a project whose instance was chosen keeps it");
    }
    {
        QSettings settings(settingsAt(), QSettings::IniFormat);
        settings.setValue("servers/items",
                          "[{\"url\":\"wss://app.forkmesh.com/room\"},"
                          "{\"url\":\"wss://work.example.com/room\"}]");
        check(!projects::markUnattachedProjectPending(settings, false, defaultUrl) &&
                  !settings.contains(projects::kInstancePendingSetting),
              "a project that added its own instance keeps it");
    }
    {
        QSettings settings(settingsAt(), QSettings::IniFormat);
        settings.setValue("server/url", "relay.example.com");
        check(!projects::markUnattachedProjectPending(settings, false, defaultUrl),
              "a legacy single relay other than the default is kept");
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testSlugs();
    testFreshInstallUsesLayout();
    testCheckoutBlocksReroot();
    testMigrationMovesEverything();
    testToolFilesInConfigDoNotHideLegacySettings();
    testXdgSandboxIsRespected();
    testOccupiedDestinationIsNeverMerged();
    testSshConfigRewrite();
    testSavedPathsAreRepointed();
    testOtherProjectIsIsolated();
    testStorageMap();
    testProjectRegistry();
    testCheckoutContaining();
    testRegistrySettingsNameIsNotAFolder();
    testUnattachedProjectBecomesPending();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

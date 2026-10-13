#include "SuggestedApps.h"

#include <QHash>

namespace forkmesh::environment {
namespace {

enum class PackageManager {
    None,
    Homebrew,
    Apt,
    Dnf,
    Pacman,
    Zypper,
    Apk,
    Winget,
    Chocolatey,
};

// The fields match PackageManager's system-manager values. An empty field is
// intentional: some utilities are supplied by the OS on that platform, or
// need a vendor-specific installer instead of a safe generic package command.
struct PackageNames {
    const char *homebrew = nullptr;
    const char *apt = nullptr;
    const char *dnf = nullptr;
    const char *pacman = nullptr;
    const char *zypper = nullptr;
    const char *apk = nullptr;
    const char *winget = nullptr;
    const char *chocolatey = nullptr;
};

bool hasInstaller(const QStringList &installers, const char *program)
{
    return installers.contains(QString::fromLatin1(program),
                               Qt::CaseInsensitive);
}

PackageManager systemPackageManager(const QStringList &installers)
{
#if defined(Q_OS_WIN)
    if (hasInstaller(installers, "winget"))
        return PackageManager::Winget;
    if (hasInstaller(installers, "choco"))
        return PackageManager::Chocolatey;
#elif defined(Q_OS_MACOS)
    if (hasInstaller(installers, "brew"))
        return PackageManager::Homebrew;
#else
    // Prefer the operating system's native package manager. Homebrew remains
    // a fallback on Linux where it is the signed-in user's only manager.
    if (hasInstaller(installers, "apt-get"))
        return PackageManager::Apt;
    if (hasInstaller(installers, "dnf"))
        return PackageManager::Dnf;
    if (hasInstaller(installers, "pacman"))
        return PackageManager::Pacman;
    if (hasInstaller(installers, "zypper"))
        return PackageManager::Zypper;
    if (hasInstaller(installers, "apk"))
        return PackageManager::Apk;
    if (hasInstaller(installers, "brew"))
        return PackageManager::Homebrew;
#endif
    return PackageManager::None;
}

QString packageName(const QString &id, PackageManager manager)
{
    static const QHash<QString, PackageNames> packages = {
        {QStringLiteral("git"),
         {"git", "git", "git", "git", "git", "git", "Git.Git", "git"}},
        {QStringLiteral("jq"),
         {"jq", "jq", "jq", "jq", "jq", "jq", "jqlang.jq", "jq"}},
        {QStringLiteral("ripgrep"),
         {"ripgrep", "ripgrep", "ripgrep", "ripgrep", "ripgrep", "ripgrep",
          "BurntSushi.ripgrep.MSVC", "ripgrep"}},
        {QStringLiteral("cmake"),
         {"cmake", "cmake", "cmake", "cmake", "cmake", "cmake", "Kitware.CMake",
          "cmake"}},
        {QStringLiteral("cxx"),
         {"llvm", "build-essential", "gcc-c++", "base-devel", "gcc-c++", "build-base",
          "LLVM.LLVM", "llvm"}},
        {QStringLiteral("node"),
         {"node", "nodejs", "nodejs", "nodejs", "nodejs", "nodejs",
          "OpenJS.NodeJS.LTS", "nodejs-lts"}},
        {QStringLiteral("python"),
         {"python", "python3", "python3", "python", "python3", "python3", nullptr,
          "python"}},
        {QStringLiteral("go"),
         {"go", "golang-go", "golang", "go", "go", "go", "Golang.Go", "golang"}},
        {QStringLiteral("tar"),
         {nullptr, "tar", "tar", "tar", "tar", "tar", nullptr, "tar"}},
        {QStringLiteral("age"),
         {"age", "age", "age", "age", "age", "age", "FiloSottile.age", "age"}},
        {QStringLiteral("openssh"),
         {"openssh", "openssh-client", "openssh-clients", "openssh", "openssh", "openssh",
          nullptr, "openssh"}},
        {QStringLiteral("curl"),
         {"curl", "curl", "curl", "curl", "curl", "curl", "curl.curl", "curl"}},
        {QStringLiteral("audio"),
         {"ffmpeg", "ffmpeg", "ffmpeg", "ffmpeg", "ffmpeg", "ffmpeg", "Gyan.FFmpeg",
          "ffmpeg"}},
        {QStringLiteral("ollama"),
         {"ollama", nullptr, "ollama", "ollama", "ollama", nullptr, "Ollama.Ollama",
          "ollama"}},
        {QStringLiteral("lima"),
         {"lima", "lima", "lima", "lima", "lima", nullptr, nullptr, "lima"}},
        {QStringLiteral("bubblewrap"),
         {nullptr, "bubblewrap", "bubblewrap", "bubblewrap", "bubblewrap", "bubblewrap",
          nullptr, "bubblewrap"}},
        {QStringLiteral("cloudflared"),
         {"cloudflared", nullptr, "cloudflared", "cloudflared", "cloudflared", nullptr,
          "Cloudflare.cloudflared", "cloudflared"}},
    };

    const auto it = packages.constFind(id);
    if (it == packages.cend())
        return {};
    const PackageNames &names = it.value();
    const char *name = nullptr;
    switch (manager) {
    case PackageManager::Homebrew:
        name = names.homebrew;
        break;
    case PackageManager::Apt:
        name = names.apt;
        break;
    case PackageManager::Dnf:
        name = names.dnf;
        break;
    case PackageManager::Pacman:
        name = names.pacman;
        break;
    case PackageManager::Zypper:
        name = names.zypper;
        break;
    case PackageManager::Apk:
        name = names.apk;
        break;
    case PackageManager::Winget:
        name = names.winget;
        break;
    case PackageManager::Chocolatey:
        name = names.chocolatey;
        break;
    case PackageManager::None:
        break;
    }
    return name ? QString::fromLatin1(name) : QString();
}

} // namespace

const QVector<SuggestedApp> &suggestedApps()
{
    static const QVector<SuggestedApp> apps = {
        {QStringLiteral("git"), QStringLiteral("Git"),
         {QStringLiteral("git")}, {QStringLiteral("--version")},
         QStringLiteral("Required for repositories, sync, branches and in-app updates.")},
        {QStringLiteral("jq"), QStringLiteral("jq"),
         {QStringLiteral("jq")}, {QStringLiteral("--version")},
         QStringLiteral("A compact JSON tool for inspecting ForkMesh workflow output and scripts.")},
        {QStringLiteral("ripgrep"), QStringLiteral("Ripgrep (rg)"),
         {QStringLiteral("rg")}, {QStringLiteral("--version")},
         QStringLiteral("Fast code search for agent sessions and developer workflows.")},
        {QStringLiteral("cmake"), QStringLiteral("CMake"),
         {QStringLiteral("cmake")}, {QStringLiteral("--version")},
         QStringLiteral("Used by ForkMesh rebuilds and the local Whisper voice setup.")},
        {QStringLiteral("cxx"), QStringLiteral("C++ build tools"),
         {QStringLiteral("c++"), QStringLiteral("clang++"), QStringLiteral("g++")},
         {QStringLiteral("--version")},
         QStringLiteral("Needed to compile ForkMesh and the Whisper voice engine.")},
        {QStringLiteral("node"), QStringLiteral("Node.js / npx"),
         {QStringLiteral("npx")}, {QStringLiteral("--version")},
         QStringLiteral("Provides npx, which ForkMesh uses for local control-node tooling.")},
        {QStringLiteral("python"), QStringLiteral("Python 3"),
         {QStringLiteral("python3"), QStringLiteral("python")},
         {QStringLiteral("--version")},
         QStringLiteral("Runs MCP helper scripts and the optional Parakeet voice engine.")},
        {QStringLiteral("go"), QStringLiteral("Go"),
         {QStringLiteral("go")}, {QStringLiteral("version")},
         QStringLiteral("Builds the optional ForkMesh mirror-node companion during updates.")},
        {QStringLiteral("tar"), QStringLiteral("tar"),
         {QStringLiteral("tar")}, {QStringLiteral("--version")},
         QStringLiteral("Creates and restores ForkMesh configuration backups.")},
        {QStringLiteral("age"), QStringLiteral("age"),
         {QStringLiteral("age")}, {QStringLiteral("--version")},
         QStringLiteral("Encrypts public mirror archives on the local machine.")},
        {QStringLiteral("openssh"), QStringLiteral("OpenSSH"),
         {QStringLiteral("ssh")}, {QStringLiteral("-V")},
         QStringLiteral("Connects SSH mirrors and remote hosts.")},
        {QStringLiteral("curl"), QStringLiteral("curl"),
         {QStringLiteral("curl")}, {QStringLiteral("--version")},
         QStringLiteral("Used by setup and infrastructure-provisioning scripts.")},
        {QStringLiteral("claude"), QStringLiteral("Claude Code"),
         {QStringLiteral("claude")}, {QStringLiteral("--version")},
         QStringLiteral("Enables Claude Code agent sessions."), true},
        {QStringLiteral("codex"), QStringLiteral("Codex CLI"),
         {QStringLiteral("codex")}, {QStringLiteral("--version")},
         QStringLiteral("Enables Codex agent sessions."), true},
        {QStringLiteral("ollama"), QStringLiteral("Ollama"),
         {QStringLiteral("ollama")}, {QStringLiteral("--version")},
         QStringLiteral("Runs optional local models from the Marketplace."), true},
        {QStringLiteral("audio"), QStringLiteral("Audio recorder"),
         {QStringLiteral("ffmpeg"), QStringLiteral("arecord"), QStringLiteral("parecord")},
         {QStringLiteral("--version")},
         QStringLiteral("Supplies local microphone capture for voice input."), true},
        {QStringLiteral("lima"), QStringLiteral("Lima"),
         {QStringLiteral("limactl")}, {QStringLiteral("--version")},
         QStringLiteral("Enables the optional isolated virtual-machine runtime."), true},
        {QStringLiteral("bubblewrap"), QStringLiteral("Bubblewrap"),
         {QStringLiteral("bwrap")}, {QStringLiteral("--version")},
         QStringLiteral("Adds Linux action sandboxing when the host supports it."), true},
        {QStringLiteral("cloudflared"), QStringLiteral("cloudflared"),
         {QStringLiteral("cloudflared")}, {QStringLiteral("--version")},
         QStringLiteral("Supports optional Cloudflare tunnel and deployment workflows."), true},
    };
    return apps;
}

const SuggestedApp *suggestedAppById(const QString &id)
{
    const auto &apps = suggestedApps();
    for (const SuggestedApp &app : apps) {
        if (app.id == id)
            return &app;
    }
    return nullptr;
}

SuggestedAppInstallPlan suggestedAppInstallPlan(
    const SuggestedApp &app, const QStringList &availableInstallers)
{
    SuggestedAppInstallPlan plan;
    if (app.id == QLatin1String("claude") || app.id == QLatin1String("codex")) {
        if (!hasInstaller(availableInstallers, "npm")) {
            plan.unavailableReason =
                QStringLiteral("Install Node.js first; it provides npm for this CLI.");
            return plan;
        }
        plan.program = QStringLiteral("npm");
        plan.arguments = {QStringLiteral("install"), QStringLiteral("--global"),
                          app.id == QLatin1String("claude")
                              ? QStringLiteral("@anthropic-ai/claude-code")
                              : QStringLiteral("@openai/codex")};
        plan.managerName = QStringLiteral("npm");
        return plan;
    }

    const PackageManager manager = systemPackageManager(availableInstallers);
    if (manager == PackageManager::None) {
        plan.unavailableReason = QStringLiteral(
            "No supported package manager was found (Homebrew, apt, dnf, pacman, "
            "zypper, apk, winget or Chocolatey).");
        return plan;
    }

    const QString package = packageName(app.id, manager);
    if (package.isEmpty()) {
        plan.unavailableReason = QStringLiteral(
            "This tool has no safe automatic install command for the detected "
            "package manager.");
        return plan;
    }

    switch (manager) {
    case PackageManager::Homebrew:
        plan = {QStringLiteral("brew"), {QStringLiteral("install"), package},
                QStringLiteral("Homebrew")};
        break;
    case PackageManager::Apt:
        plan = {QStringLiteral("apt-get"),
                {QStringLiteral("install"), QStringLiteral("--yes"), package},
                QStringLiteral("apt"), {}, true};
        break;
    case PackageManager::Dnf:
        plan = {QStringLiteral("dnf"),
                {QStringLiteral("install"), QStringLiteral("--assumeyes"), package},
                QStringLiteral("dnf"), {}, true};
        break;
    case PackageManager::Pacman:
        plan = {QStringLiteral("pacman"),
                {QStringLiteral("-S"), QStringLiteral("--needed"),
                 QStringLiteral("--noconfirm"), package},
                QStringLiteral("pacman"), {}, true};
        break;
    case PackageManager::Zypper:
        plan = {QStringLiteral("zypper"),
                {QStringLiteral("--non-interactive"), QStringLiteral("install"), package},
                QStringLiteral("zypper"), {}, true};
        break;
    case PackageManager::Apk:
        plan = {QStringLiteral("apk"), {QStringLiteral("add"), package},
                QStringLiteral("apk"), {}, true};
        break;
    case PackageManager::Winget:
        plan = {QStringLiteral("winget"),
                {QStringLiteral("install"), QStringLiteral("--id"), package,
                 QStringLiteral("--exact"), QStringLiteral("--accept-package-agreements"),
                 QStringLiteral("--accept-source-agreements")},
                QStringLiteral("winget")};
        break;
    case PackageManager::Chocolatey:
        plan = {QStringLiteral("choco"),
                {QStringLiteral("install"), package, QStringLiteral("--yes")},
                QStringLiteral("Chocolatey")};
        break;
    case PackageManager::None:
        break;
    }
    return plan;
}

} // namespace forkmesh::environment

#include "SuggestedApps.h"

#include <QCoreApplication>

#include <iostream>

using forkmesh::environment::SuggestedApp;
using forkmesh::environment::suggestedAppById;
using forkmesh::environment::suggestedAppInstallPlan;
using forkmesh::environment::suggestedApps;

namespace {

const SuggestedApp *requiredApp(const char *id)
{
    const SuggestedApp *app = suggestedAppById(QString::fromLatin1(id));
    if (!app)
        std::cerr << "Missing suggested app: " << id << '\n';
    return app;
}

bool includes(const QStringList &values, const QString &value)
{
    return values.contains(value);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // Git is the core desktop dependency; jq and rg are explicitly promoted
    // to first-class tools in Settings -> Apps.
    const SuggestedApp *git = requiredApp("git");
    const SuggestedApp *jq = requiredApp("jq");
    const SuggestedApp *ripgrep = requiredApp("ripgrep");
    const SuggestedApp *codex = requiredApp("codex");
    const SuggestedApp *claude = requiredApp("claude");
    if (!git || !jq || !ripgrep || !codex || !claude ||
        !includes(git->executableNames, QStringLiteral("git")) ||
        !includes(jq->executableNames, QStringLiteral("jq")) ||
        !includes(ripgrep->executableNames, QStringLiteral("rg"))) {
        return 1;
    }

    const auto aptJq = suggestedAppInstallPlan(*jq, {QStringLiteral("apt-get")});
    if (!aptJq.isAvailable() || aptJq.program != QLatin1String("apt-get") ||
        !aptJq.requiresAdministrator || !includes(aptJq.arguments, QStringLiteral("jq"))) {
        return 1;
    }

    const auto brewRg =
        suggestedAppInstallPlan(*ripgrep, {QStringLiteral("brew")});
    if (!brewRg.isAvailable() || brewRg.program != QLatin1String("brew") ||
        !includes(brewRg.arguments, QStringLiteral("ripgrep"))) {
        return 1;
    }

    const auto npmCodex =
        suggestedAppInstallPlan(*codex, {QStringLiteral("npm")});
    const auto npmClaude =
        suggestedAppInstallPlan(*claude, {QStringLiteral("npm")});
    if (!npmCodex.isAvailable() || npmCodex.program != QLatin1String("npm") ||
        !includes(npmCodex.arguments, QStringLiteral("@openai/codex")) ||
        !npmClaude.isAvailable() ||
        !includes(npmClaude.arguments, QStringLiteral("@anthropic-ai/claude-code"))) {
        return 1;
    }

    if (suggestedApps().size() < 12 ||
        suggestedAppInstallPlan(*git, {}).isAvailable()) {
        return 1;
    }

    std::cout << "suggested apps tests passed\n";
    return 0;
}

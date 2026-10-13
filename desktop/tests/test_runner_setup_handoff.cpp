#include "RunnerSetupHandoff.h"

#include <iostream>

using namespace forkmesh::runners;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

int main()
{
    const QString token = QStringLiteral("fmrt_") + QString(64, QLatin1Char('a'));

    const RunnerSetupHandoff handoff = parseRunnerSetupHandoff(
        QStringLiteral("runner"), token, QStringLiteral("Jett-Box"),
        QStringLiteral("https://app.forkmesh.com/"));
    check(handoff.isValid(), "a runner handoff parses");
    check(handoff.name == QStringLiteral("jett-box"), "the name is lowercased");
    check(handoff.appUrl == QStringLiteral("https://app.forkmesh.com"),
          "the trailing slash is dropped");
    check(!parseRunnerSetupHandoff(QStringLiteral("node"), token,
                                   QStringLiteral("box"),
                                   QStringLiteral("https://app.forkmesh.com"))
               .isValid(),
          "only role=runner is a runner handoff");
    check(!parseRunnerSetupHandoff(QStringLiteral("runner"),
                                   QStringLiteral("fmrt_x"),
                                   QStringLiteral("box"),
                                   QStringLiteral("https://app.forkmesh.com"))
               .isValid(),
          "a malformed token is refused");
    check(!parseRunnerSetupHandoff(QStringLiteral("runner"), token,
                                   QStringLiteral("box;rm"),
                                   QStringLiteral("https://app.forkmesh.com"))
               .isValid(),
          "a name with shell characters is refused");
    check(!parseRunnerSetupHandoff(QStringLiteral("runner"), token,
                                   QStringLiteral("box"),
                                   QStringLiteral("http://app.forkmesh.com"))
               .isValid(),
          "a plain-http app is refused");
    check(!parseRunnerSetupHandoff(QStringLiteral("runner"), token,
                                   QStringLiteral("box"),
                                   QStringLiteral("https://evil.test/path"))
               .isValid(),
          "an app URL with a path is refused");

    check(runnerInstallScriptUrl(QStringLiteral("https://forkmesh.com/install.sh")) ==
              QStringLiteral("https://forkmesh.com/install-runner.sh"),
          "the runner script sits next to install.sh");
    check(runnerInstallScriptUrl(QStringLiteral("http://x/install.sh")) ==
              QStringLiteral("https://forkmesh.com/install-runner.sh"),
          "a non-https installer falls back to forkmesh.com");

    const QString script = QStringLiteral("https://forkmesh.com/install-runner.sh");
    const QString root = runnerInstallRemoteCommand(
        handoff, script, QStringLiteral("root"), true);
    check(root == QStringLiteral(
              "curl -fsSL 'https://forkmesh.com/install-runner.sh' | bash -s -- "
              "--token '") + token + QStringLiteral(
              "' --name 'jett-box' --app 'https://app.forkmesh.com'"),
          "root runs the installer without sudo");
    const QString withPassword = runnerInstallRemoteCommand(
        handoff, script, QStringLiteral("deploy"), true);
    check(withPassword.startsWith(QStringLiteral("sudo -S -p '' -v && curl")),
          "a password user validates sudo from stdin first");
    check(withPassword.contains(QStringLiteral("| sudo -n bash -s --")),
          "the installer runs under the cached sudo");
    const QString withKey = runnerInstallRemoteCommand(
        handoff, script, QStringLiteral("deploy"), false);
    check(withKey.startsWith(QStringLiteral("sudo -n true 2>/dev/null ||")),
          "a key login needs passwordless sudo");
    check(runnerInstallRemoteCommand(RunnerSetupHandoff(), script,
                                     QStringLiteral("root"), false)
              .isEmpty(),
          "no command without a valid handoff");

    check(redactRunnerToken(QStringLiteral("x --token ") + token) ==
              QStringLiteral("x --token fmrt_[hidden]"),
          "the token is hidden in the local log");

    const QStringList prints = parseHostKeyFingerprints(QStringLiteral(
        "256 SHA256:AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/abcde 203.0.113.9 (ED25519)\n"
        "3072 SHA256:ZyXwVuTsRqPoNmLkJiHgFeDcBa9876543210+/zyxwv 203.0.113.9 (RSA)\n"
        "garbage\n"
        "256 SHA256:AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/abcde 203.0.113.9 (ED25519)\n"));
    check(prints.size() == 2, "fingerprints are parsed once each");
    check(prints.value(0) == QStringLiteral(
              "ED25519 SHA256:AbCdEfGhIjKlMnOpQrStUvWxYz0123456789+/abcde"),
          "the key type leads the fingerprint");
    const QString text = hostKeyConfirmationText(QStringLiteral("203.0.113.9"), prints);
    check(text.contains(QStringLiteral("ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub")),
          "the prompt says how to compare");
    check(text.contains(prints.value(1)), "the prompt lists every key");

    if (failures == 0)
        std::cout << "runner setup handoff tests passed\n";
    return failures == 0 ? 0 : 1;
}

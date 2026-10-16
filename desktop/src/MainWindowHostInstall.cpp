
#include "ForkMeshBuildCommit.h"
#include "DesktopBuildCommit.h"
#include "ForkMeshVersion.h"
#include "Tooltips.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowNetworkInternal.h"
#include "SystemPressure.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "AgentWorktree.h"
#include "RepoStatsStore.h"
#include "ActionStore.h"
#include "ControlNode.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "D1QuerySource.h"
#include "DatabaseManagerWidget.h"
#include "KebabHeaderView.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PromptDraftStore.h"
#include "PromptSpellCheckHighlighter.h"
#include "PublicMirrorRuntime.h"
#include "QtRuntimeDiagnostics.h"
#include "RewardPoolSigner.h"
#include "RunnerSetupHandoff.h"
#include "TerminalWidget.h"
#include "WorldSpeechBridge.h"

#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <signal.h>
#endif

using namespace forkmesh::ui;

namespace {
int installLogAnsiFg(int idx, bool dark)
{
    static const int kDark[16] = {
        0x6e7681, 0xff7b72, 0x3fb950, 0xd29922, 0x58a6ff, 0xbc8cff, 0x39c5cf,
        0xb1bac4, 0x6e7681, 0xffa198, 0x56d364, 0xe3b341, 0x79c0ff, 0xd2a8ff,
        0x56d4dd, 0xf0f6fc};
    static const int kLight[16] = {
        0x24292f, 0xcf222e, 0x1a7f37, 0x9a6700, 0x0550ae, 0x8250df, 0x1b7c83,
        0x6e7781, 0x57606a, 0xa40e26, 0x116329, 0x7d4e00, 0x0969da, 0x6639ba,
        0x3192aa, 0x424a53};
    idx = qBound(0, idx, 15);
    return dark ? kDark[idx] : kLight[idx];
}

int installLogXterm256(int n)
{
    if (n < 232) {
        n -= 16;
        const int r = (n / 36) % 6, g = (n / 6) % 6, b = n % 6;
        auto comp = [](int v) { return v ? v * 40 + 55 : 0; };
        return (comp(r) << 16) | (comp(g) << 8) | comp(b);
    }
    const int v = (n - 232) * 10 + 8;
    return (v << 16) | (v << 8) | v;
}

void appendAnsiLog(QPlainTextEdit *log, QString &carry, int &fg, bool &bold,
                   bool dark, const QString &text)
{
    if (!log || text.isEmpty())
        return;

    QString data = carry + text;
    carry.clear();

    QTextCursor cursor(log->document());
    cursor.movePosition(QTextCursor::End);

    auto currentFormat = [&fg, &bold]() {
        QTextCharFormat fmt;
        if (fg >= 0)
            fmt.setForeground(
                QColor((fg >> 16) & 0xFF, (fg >> 8) & 0xFF, fg & 0xFF));
        if (bold)
            fmt.setFontWeight(QFont::Bold);
        return fmt;
    };

    QString run;
    auto flush = [&]() {
        if (!run.isEmpty()) {
            cursor.insertText(run, currentFormat());
            run.clear();
        }
    };

    auto applySgr = [&fg, &bold, dark](const QString &paramStr) {
        const QStringList parts =
            paramStr.isEmpty() ? QStringList{QStringLiteral("0")}
                               : paramStr.split(QLatin1Char(';'));
        for (int k = 0; k < parts.size(); ++k) {
            bool ok = false;
            const int code = parts.at(k).toInt(&ok);
            if (!ok)
                continue;
            if (code == 0) {
                fg = -1;
                bold = false;
            } else if (code == 1) {
                bold = true;
            } else if (code == 22) {
                bold = false;
            } else if (code == 39) {
                fg = -1;
            } else if (code >= 30 && code <= 37) {
                fg = installLogAnsiFg(code - 30, dark);
            } else if (code >= 90 && code <= 97) {
                fg = installLogAnsiFg(8 + code - 90, dark);
            } else if (code == 38 && k + 2 < parts.size() &&
                       parts.at(k + 1).toInt() == 5) {
                const int idx = parts.at(k + 2).toInt();
                fg = idx < 16 ? installLogAnsiFg(idx, dark)
                              : installLogXterm256(idx);
                k += 2;
            } else if (code == 38 && k + 4 < parts.size() &&
                       parts.at(k + 1).toInt() == 2) {
                fg = ((parts.at(k + 2).toInt() & 0xFF) << 16) |
                     ((parts.at(k + 3).toInt() & 0xFF) << 8) |
                     (parts.at(k + 4).toInt() & 0xFF);
                k += 4;
            }
        }
    };

    int i = 0;
    const int len = data.size();
    while (i < len) {
        if (data.at(i).unicode() != 0x1B) { // ordinary text
            run += data.at(i);
            ++i;
            continue;
        }
        if (i + 1 >= len) { // dangling ESC: wait for the rest
            carry = data.mid(i);
            break;
        }
        const QChar kind = data.at(i + 1);
        if (kind == QLatin1Char('[')) { // CSI: ESC [ params... final(0x40-0x7E)
            int j = i + 2;
            while (j < len) {
                const ushort u = data.at(j).unicode();
                if (u >= 0x40 && u <= 0x7E)
                    break;
                ++j;
            }
            if (j >= len) { // sequence not finished yet
                carry = data.mid(i);
                break;
            }
            if (data.at(j) == QLatin1Char('m')) { // SGR: change the style
                flush();
                applySgr(data.mid(i + 2, j - (i + 2)));
            }
            i = j + 1;
        } else if (kind == QLatin1Char(']')) { // OSC: ESC ] ... BEL or ST
            int j = i + 2;
            bool done = false;
            while (j < len) {
                if (data.at(j).unicode() == 0x07) { // BEL terminator
                    ++j;
                    done = true;
                    break;
                }
                if (data.at(j).unicode() == 0x1B && j + 1 < len &&
                    data.at(j + 1) == QLatin1Char('\\')) { // ST terminator
                    j += 2;
                    done = true;
                    break;
                }
                ++j;
            }
            if (!done) {
                carry = data.mid(i);
                break;
            }
            i = j;
        } else { // other two-byte escape (charset selection, etc.): drop both
            i += 2;
        }
    }
    flush();

    if (carry.size() > 256) {
        cursor.insertText(carry, currentFormat());
        carry.clear();
    }

    log->moveCursor(QTextCursor::End);
    log->ensureCursorVisible();
}
} // namespace

void MainWindow::appendHostInstallLog(const QString &text)
{
    appendAnsiLog(m_hostInstallLog, m_hostInstallLogCarry, m_hostInstallLogFg,
                  m_hostInstallLogBold, currentThemeIsDark(), text);
    if (m_vultrProvisionActive)
        scheduleVultrProvisionLogSave();
}

void MainWindow::appendHostDeployLog(HostDeploySession *session,
                                     const QString &text)
{
    if (!session)
        return;
    appendAnsiLog(session->log, session->ansiCarry, session->ansiFg,
                  session->ansiBold, currentThemeIsDark(), text);
}

namespace {
const QString kHostUploadMarker = QStringLiteral("__FORKMESH_UPLOAD__");
const QString kVultrMirrorUploadMarker =
    QStringLiteral("__FORKMESH_GO_MIRROR_V1__");

QString packagedMirrorFile(const QString &relative)
{
    const QStringList candidates{
        QDir(QStringLiteral(FORKMESH_SOURCE_DIR)).filePath(relative),
        QDir(QStringLiteral(FORKMESH_SOURCE_DIR))
            .filePath(QStringLiteral("../") + relative),
        QDir(QCoreApplication::applicationDirPath()).filePath(relative),
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("../share/forkmesh/") + relative),
    };
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile() && !info.isSymLink())
            return info.absoluteFilePath();
    }
    return {};
}

QString controllerReleaseManifestDigest(const QString &expectedBuildCommit,
                                        QString *errorOut)
{
    QString configured =
        qEnvironmentVariable("FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256")
            .trimmed()
            .toLower();
#ifdef FORKMESH_WINDOW_TESTS
    const QString testDigest =
        qEnvironmentVariable("FORKMESH_TEST_RELEASE_MANIFEST_SHA256")
            .trimmed()
            .toLower();
    if (!testDigest.isEmpty())
        configured = testDigest;
#endif
    static const QRegularExpression exactDigest(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!configured.isEmpty()) {
        if (exactDigest.match(configured).hasMatch())
            return configured;
        if (errorOut) {
            *errorOut = QStringLiteral(
                "FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256 must be an exact "
                "64-hex SHA-256 digest.");
        }
        return {};
    }

    // A source checkout can authenticate the separately committed release
    // metadata directly. Packaged controllers, where that checkout is absent,
    // must receive the digest over their authenticated control channel through
    // the environment above; they never trust a hash copied from the remote
    // install response.
    const QString manifestPath =
        QDir(QStringLiteral(FORKMESH_SOURCE_DIR))
            .filePath(QStringLiteral(
                ".forkmesh/releases/latest/release.json"));
    QFile manifestFile(manifestPath);
    if (manifestFile.open(QIODevice::ReadOnly) &&
        manifestFile.size() > 0 && manifestFile.size() <= 1024 * 1024) {
        const QByteArray manifestBytes = manifestFile.readAll();
        QJsonParseError parseError;
        const QJsonObject manifest =
            QJsonDocument::fromJson(manifestBytes, &parseError).object();
        const QString expectedVersion =
            QStringLiteral(FORKMESH_VERSION);
        const QString tag =
            manifest.value(QStringLiteral("tag")).toString();
        const QString checksumsDigest =
            manifest.value(QStringLiteral("checksums_sha256"))
                .toString()
                .trimmed()
                .toLower();
        if (parseError.error == QJsonParseError::NoError &&
            manifest.value(QStringLiteral("schema")).toString() ==
                QStringLiteral("forkmesh-release-v2") &&
            manifest.value(QStringLiteral("build_commit"))
                    .toString()
                    .trimmed()
                    .toLower() == expectedBuildCommit &&
            (tag == expectedVersion ||
             tag == QStringLiteral("v") + expectedVersion) &&
            exactDigest.match(checksumsDigest).hasMatch()) {
            return QString::fromLatin1(
                QCryptographicHash::hash(
                    manifestBytes, QCryptographicHash::Sha256)
                    .toHex());
        }
    }
    if (errorOut) {
        *errorOut = QStringLiteral(
            "No controller-trusted release manifest matches this build. "
            "Refresh the checked-in signed release metadata or set "
            "FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256 from an authenticated "
            "control channel before deploying a fleet binary.");
    }
    return {};
}
} // namespace

bool MainWindow::buildVultrMirrorNodeInstallCommand(
    const QString &node, const QString &publicAddress, QString *remoteCmd,
    QByteArray *uploadBytes,
    QString *errorOut) const
{
    QString mirrorBinary =
        QStandardPaths::findExecutable(QStringLiteral("forkmesh-mirror-node"));
    if (mirrorBinary.isEmpty()) {
        const QString sibling =
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("forkmesh-mirror-node"));
        if (QFileInfo(sibling).isExecutable())
            mirrorBinary = sibling;
    }
    if (mirrorBinary.isEmpty()) {
        const QString installed =
            QFileInfo(runningClientExecutable())
                .dir()
                .filePath(QStringLiteral("forkmesh-mirror-node"));
        if (QFileInfo(installed).isExecutable())
            mirrorBinary = installed;
    }
    if (mirrorBinary.isEmpty()) {
        const QString sourceBinary =
            QDir(QStringLiteral(FORKMESH_SOURCE_DIR))
                .filePath(QStringLiteral("../server/forkmesh-mirror-node"));
        if (QFileInfo(sourceBinary).isExecutable())
            mirrorBinary = sourceBinary;
    }
    struct PayloadFile {
        QString name;
        QString path;
        QByteArray data;
        bool executable = false;
    };
    QList<PayloadFile> files{
        {QStringLiteral("forkmesh-mirror-node"), mirrorBinary, {}, true},
        {QStringLiteral("mirror_gateway.py"),
         packagedMirrorFile(QStringLiteral("tools/mirror_gateway.py"))},
        {QStringLiteral("mirror_replica.py"),
         packagedMirrorFile(QStringLiteral("app/src/mirror_replica.py"))},
        {QStringLiteral("public_snapshots.py"),
         packagedMirrorFile(QStringLiteral("app/src/public_snapshots.py"))},
        {QStringLiteral("portable_sessions.py"),
         packagedMirrorFile(QStringLiteral("app/src/portable_sessions.py"))},
        {QStringLiteral("direct_https_bootstrap.py"),
         packagedMirrorFile(QStringLiteral("tools/direct_https_bootstrap.py"))},
        {QStringLiteral("node_operator_policy.py"),
         packagedMirrorFile(QStringLiteral("tools/node_operator_policy.py"))},
        // Loaded by node_operator_policy.py when a policy enables `secrets`.
        {QStringLiteral("secret_gate.py"),
         packagedMirrorFile(QStringLiteral("tools/secret_gate.py"))},
        {QStringLiteral("forkmesh-mirror-node.service"),
         packagedMirrorFile(
             QStringLiteral(
                 "server/packaging/systemd/forkmesh-mirror-node.service"))},
    };
    for (PayloadFile &file : files) {
        if (file.path.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "The Go mirror-node package is incomplete (%1 is missing). "
                    "Reinstall this ForkMesh release and retry.")
                                .arg(file.name);
            return false;
        }
        QFile source(file.path);
        if (!source.open(QIODevice::ReadOnly) ||
            (file.data = source.readAll()).isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral("Could not read packaged %1.")
                                .arg(file.name);
            return false;
        }
    }

    QByteArray payload = kVultrMirrorUploadMarker.toUtf8() + '\n' +
                         QByteArray::number(files.size()) + '\n';
    for (const PayloadFile &file : std::as_const(files)) {
        const QByteArray digest =
            QCryptographicHash::hash(file.data, QCryptographicHash::Sha256)
                .toHex();
        payload += file.name.toUtf8() + '\n' +
                   QByteArray::number(file.data.size()) + '\n' + digest + '\n';
        payload += file.data;
    }

    auto shq = [](QString value) {
        value.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        return QStringLiteral("'") + value + QStringLiteral("'");
    };
    const QString hostname = m_vultrDnsHostname.trimmed().toLower();
    const QString zone = hostname.section(QLatin1Char('.'), 1);
    const QUrl catalog = catalogApiUrl();
    QUrl accounts = catalog;
    accounts.setPath(QStringLiteral("/api/accounts"));
    accounts.setQuery(QString());
    const QString relayHost = catalog.host();
    if (hostname.isEmpty() || zone.isEmpty() || relayHost.isEmpty() ||
        publicAddress.trimmed().isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral(
                "The Vultr mirror hostname, public address, or relay endpoint "
                "is unavailable.");
        return false;
    }

    QString pipeline = QStringLiteral(
        "set -eu; umask 077; IFS= read -r fm_cf_token || exit 68; "
        "IFS= read -r marker; [ \"$marker\" = %1 ] || exit 69; "
        "IFS= read -r count; [ \"$count\" = 9 ] || exit 69; "
        "stage=\"$(mktemp -d /tmp/forkmesh-go-node.XXXXXX)\" || exit 70; "
        "trap 'rm -rf \"$stage\"; unset fm_cf_token' EXIT HUP INT TERM; "
        "fm_log() { printf '[%s] %s\\n' \"$(date -u '+%Y-%m-%dT%H:%M:%SZ')\" \"$1\"; }; "
        "i=0; while [ \"$i\" -lt \"$count\" ]; do "
        "IFS= read -r name; IFS= read -r size; IFS= read -r expected; "
        "case \"$name\" in forkmesh-mirror-node|mirror_gateway.py|mirror_replica.py|public_snapshots.py|portable_sessions.py|direct_https_bootstrap.py|node_operator_policy.py|secret_gate.py|forkmesh-mirror-node.service) ;; *) exit 69;; esac; "
        "dd iflag=fullblock bs=1 count=\"$size\" of=\"$stage/$name\" status=none; "
        "actual=\"$(sha256sum \"$stage/$name\")\"; actual=\"${actual%% *}\"; "
        "[ \"$actual\" = \"$expected\" ] || exit 69; i=$((i+1)); done; "
        "fm_log 'Native mirror package received and SHA-256 verified.'; "
        "export DEBIAN_FRONTEND=noninteractive; "
        "missing=0; for command_name in curl git python3 openssl caddy; do command -v \"$command_name\" >/dev/null 2>&1 || missing=1; done; "
        "python3 -c 'import cryptography' >/dev/null 2>&1 || missing=1; "
        "if [ \"$missing\" -ne 0 ]; then fm_log 'Installing required Debian packages and the host HTTPS server.'; apt-get update -qq && apt-get install -y -qq --no-install-recommends ca-certificates curl git python3 python3-cryptography openssl caddy; else fm_log 'Required Debian packages are already present.'; fi; "
        "if command -v ufw >/dev/null 2>&1 && ufw status | grep -Fq 'Status: active'; then ufw allow 80/tcp >/dev/null && ufw allow 443/tcp >/dev/null && ufw allow 443/udp >/dev/null; fi; "
        // Keys only: the desktop reaches the host with the shared ForkMesh
        // key, so a password prompt only serves bots guessing at it. The
        // lines are server/packaging/ssh/10-forkmesh-hardening.conf's; never
        // on a host root has no key for, and reverted if sshd will not take them.
        "fm_log 'Locking SSH to key logins only.'; "
        "if [ -s /root/.ssh/authorized_keys ] && command -v sshd >/dev/null 2>&1; then "
        "hardening=/etc/ssh/sshd_config.d/10-forkmesh-hardening.conf; install -d -m 0755 /etc/ssh/sshd_config.d; "
        "printf '%s\\n' 'PasswordAuthentication no' 'KbdInteractiveAuthentication no' 'PermitEmptyPasswords no' 'PermitRootLogin prohibit-password' 'MaxAuthTries 3' 'LoginGraceTime 20' 'MaxStartups 10:30:60' >\"$hardening\"; chmod 0644 \"$hardening\"; "
        "grep -q '^Include /etc/ssh/sshd_config.d/\\*.conf' /etc/ssh/sshd_config || sed -i '1i Include /etc/ssh/sshd_config.d/*.conf' /etc/ssh/sshd_config; "
        "if sshd -t && sshd -T | grep -qx 'passwordauthentication no'; then systemctl reload ssh 2>/dev/null || systemctl reload sshd 2>/dev/null || true; "
        "else rm -f \"$hardening\"; fm_log 'sshd refused the key-only settings; SSH left as it was.'; fi; "
        "else fm_log 'Root has no SSH key on this host; SSH password logins left as they were.'; fi; "
        // Ban addresses that keep failing SSH. The lines are
        // server/packaging/fail2ban/forkmesh-sshd.local's (tools/mirror_fail2ban.sh
        // installs the same jail on existing hosts). Never fatal: key-only sshd
        // above is the protection; this only quiets the log.
        "fm_log 'Banning repeat SSH failures with fail2ban.'; "
        "if command -v fail2ban-client >/dev/null 2>&1 || { apt-get update -qq && apt-get install -y -qq --no-install-recommends fail2ban python3-systemd; }; then "
        "printf '%s\\n' '[DEFAULT]' 'bantime = 1h' 'bantime.increment = true' 'bantime.maxtime = 1w' 'findtime = 10m' 'maxretry = 5' '' '[sshd]' 'enabled = true' 'mode = aggressive' 'backend = systemd' >/etc/fail2ban/jail.d/forkmesh-sshd.local; "
        "systemctl enable fail2ban >/dev/null 2>&1; systemctl restart fail2ban 2>/dev/null || fm_log 'fail2ban did not start; SSH stays key-only.'; "
        "else fm_log 'fail2ban could not be installed; SSH stays key-only.'; fi; "
        "install -m 0755 \"$stage/forkmesh-mirror-node\" /usr/local/bin/forkmesh-mirror-node; "
        "install -d -m 0755 /usr/local/share/forkmesh/tools; "
        "for tool in mirror_gateway.py mirror_replica.py public_snapshots.py portable_sessions.py direct_https_bootstrap.py node_operator_policy.py secret_gate.py; do install -m 0644 \"$stage/$tool\" \"/usr/local/share/forkmesh/tools/$tool\"; done; "
        "install -m 0644 \"$stage/forkmesh-mirror-node.service\" /etc/systemd/system/forkmesh-mirror-node.service; "
        "id forkmesh-node >/dev/null 2>&1 || useradd --system --home-dir /var/lib/forkmesh --create-home --shell /usr/sbin/nologin forkmesh-node; "
        "install -d -o forkmesh-node -g forkmesh-node -m 0700 /var/lib/forkmesh /var/lib/forkmesh/tmp /etc/forkmesh; "
        // This desktop may follow the host's journal over QUIC: Caddy serves
        // the gateway's /v1/node/logs on HTTP/3 (UDP 443, opened above).
        "viewer_key=%12; if [ -n \"$viewer_key\" ]; then keys=/etc/forkmesh/log-viewer-keys; touch \"$keys\"; grep -qxF \"$viewer_key\" \"$keys\" || printf '%s\\n' \"$viewer_key\" >>\"$keys\"; chown forkmesh-node:forkmesh-node \"$keys\"; chmod 0644 \"$keys\"; fi; "
        "fm_log 'Fetching the ForkMesh router identity.'; "
        "router_body=\"$stage/router-identity.json\"; router_attempt=1; router_max_attempts=4; "
        "while :; do : >\"$router_body\"; router_status=; "
        "if router_status=\"$(curl -sS --max-time 20 --output \"$router_body\" --write-out '%{http_code}' %2)\"; then "
        "case \"$router_status\" in 2?" "?) break ;; 5?" "?) ;; *) printf 'Mirror router identity request was rejected (HTTP %s).\\n' \"$router_status\" >&2; exit 71 ;; esac; "
        "fi; if [ \"$router_attempt\" -ge \"$router_max_attempts\" ]; then printf 'Mirror router identity remained unavailable after %s attempts.\\n' \"$router_attempt\" >&2; exit 71; fi; "
        "sleep \"$router_attempt\"; router_attempt=$((router_attempt + 1)); done; "
        "router_json=\"$(cat \"$router_body\")\"; "
        "if ! router_key=\"$(printf '%s' \"$router_json\" | python3 -c 'import json,sys; value=json.load(sys.stdin).get(\"routerPublicKey\",\"\"); print(value if isinstance(value,str) else \"\")' 2>/dev/null)\"; then printf 'Mirror router identity response was invalid.\\n' >&2; exit 71; fi; "
        "if ! printf '%s' \"$router_key\" | grep -Eq '^[A-Za-z0-9_-]{43}$'; then printf 'Mirror router public key was invalid.\\n' >&2; exit 71; fi; "
        "runuser -u forkmesh-node -- /usr/local/bin/forkmesh-mirror-node --init --config /etc/forkmesh/mirror-node.json --state-dir /var/lib/forkmesh --node %3 --owner forkmesh --repository forkmesh --upstream %4 --catalog-url %5 --public-origin %6 --mirror-url %6 --disable-cloudflared --router-public-key \"$router_key\" --version %7 >/tmp/forkmesh-node-public-key; "
        "node_key=\"$(tr -d '\\r\\n' </tmp/forkmesh-node-public-key)\"; rm -f /tmp/forkmesh-node-public-key; "
        "runuser -u forkmesh-node -- git init --bare --quiet --initial-branch=main /var/lib/forkmesh/repositories/forkmesh-forkmesh.git; "
        "fm_log 'Repository container initialized; the desktop seeds it after install, independent of mesh health.'; "
        "fm_log 'Mirror identity initialized; provisioning unproxied DNS and host-owned HTTPS.'; "
        "CLOUDFLARE_API_TOKEN=\"$fm_cf_token\" python3 /usr/local/share/forkmesh/tools/direct_https_bootstrap.py --hostname %8 --zone %9 --address %11 --adopt-dedicated-host >/dev/null; "
        "fm_log 'Independent HTTPS configured; linking the mirror account.'; "
        "unset fm_cf_token; link_number=\"$(od -An -N4 -tu4 /dev/urandom)\"; link_number=\"${link_number// /}\"; link_code=\"$((1000000 + link_number % 1000000))\"; link_code=\"${link_code#1}\"; "
        "printf 'FORKMESH LINK CODE: %s\\n' \"$link_code\"; "
        "runuser -u forkmesh-node -- /usr/local/bin/forkmesh-mirror-node --config /etc/forkmesh/mirror-node.json --accounts-url %10 --register-link-code \"$link_code\"; "
        "fm_log 'Starting the ForkMesh mirror service.'; "
        "systemctl daemon-reload; systemctl enable --now forkmesh-mirror-node.service; systemctl enable caddy.service; systemctl reload-or-restart caddy.service; "
        "for n in 1 2 3 4 5 6 7 8 9 10; do curl -fsS http://127.0.0.1:8791/healthz >/dev/null && break; sleep 1; done; "
        "curl -fsS http://127.0.0.1:8791/healthz >/dev/null; public_ok=0; "
        "for n in 1 2 3 4 5 6 7 8 9 10 11 12; do if curl -fsS --connect-timeout 3 --max-time 10 %6/health >/dev/null 2>&1 && curl -fsS --connect-timeout 3 --max-time 10 %6/forkmesh-mirror.json | grep -Fq '\"kind\": \"public-https-origin\"'; then public_ok=1; break; fi; sleep 5; done; "
        "[ \"$public_ok\" = 1 ] || { printf 'Independent public HTTPS did not become healthy.\\n' >&2; exit 72; }; "
        "systemctl disable --now cloudflared.service cloudflared-forkmesh.service >/dev/null 2>&1 || true; "
        "fm_log 'Go mirror-node installed and running (no Qt/GTK packages).'")
                           .arg(shq(kVultrMirrorUploadMarker))
                           .arg(shq(QStringLiteral("https://") + relayHost +
                                    QStringLiteral("/api/mirrors/https")))
                           .arg(shq(node))
                           .arg(shq(QStringLiteral("https://") + relayHost +
                                    QStringLiteral("/forkmesh/forkmesh")))
                           .arg(shq(catalog.toString()))
                           .arg(shq(QStringLiteral("https://") + hostname))
                           .arg(shq(QStringLiteral(FORKMESH_VERSION)))
                           .arg(shq(hostname))
                           .arg(shq(zone))
                           .arg(shq(accounts.toString()));
    pipeline = pipeline.arg(shq(publicAddress.trimmed()))
                   .arg(shq(m_profileIdentity.publicKey()));
    if (remoteCmd)
        *remoteCmd = pipeline;
    if (uploadBytes)
        *uploadBytes = payload;
    return true;
}

bool MainWindow::buildHostInstallCommand(const QString &ip, const QString &user,
                                         const QString &node, bool uploadBinary,
                                         bool reinstall, bool fromSource,
                                         bool requirePublishedBinary,
                                         QString *remoteCmd, QByteArray *uploadBytes,
                                         QString *errorOut,
                                         const PublishedReleaseDeployment
                                             &publishedRelease)
{
    if (m_vultrProvisionActive) {
        return buildVultrMirrorNodeInstallCommand(
            node, ip, remoteCmd, uploadBytes, errorOut);
    }
    const QString installUrl = installScriptUrl();
    if (installUrl.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("Could not resolve the installer URL.");
        return false;
    }

    const bool doUpload =
        uploadBinary && !fromSource && !requirePublishedBinary;
    QByteArray bytes;
    if (doUpload) {
        QFile self(QCoreApplication::applicationFilePath());
        if (!self.open(QIODevice::ReadOnly) ||
            (bytes = self.readAll()).isEmpty()) {
            if (errorOut)
                *errorOut = QString::fromUtf8(
                                "Could not read this app's binary (%1) to "
                                "upload it.")
                                .arg(QCoreApplication::applicationFilePath());
            return false;
        }
    }

    auto shq = [](const QString &s) {
        QString out = s;
        out.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        return QStringLiteral("'") + out + QStringLiteral("'");
    };
    QString expectedBuildCommit;
    QString expectedReleaseManifestDigest;
    QString expectedReleaseVersion = QStringLiteral(FORKMESH_VERSION);
    if (requirePublishedBinary) {
        if (publishedRelease.isValid()) {
            expectedBuildCommit =
                publishedRelease.buildCommit.trimmed().toLower();
            expectedReleaseVersion =
                publishedRelease.version.trimmed();
            expectedReleaseManifestDigest =
                publishedRelease.manifestSha256.trimmed().toLower();
        } else {
            expectedBuildCommit =
                QStringLiteral(FORKMESH_BUILD_COMMIT).trimmed().toLower();
#ifdef FORKMESH_WINDOW_TESTS
            const QString testBuildCommit =
                qEnvironmentVariable("FORKMESH_TEST_BUILD_COMMIT")
                    .trimmed()
                    .toLower();
            if (!testBuildCommit.isEmpty())
                expectedBuildCommit = testBuildCommit;
#endif
        }
        static const QRegularExpression exactCommit(
            QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
        static const QRegularExpression exactDigest(
            QStringLiteral("^[0-9a-f]{64}$"));
        static const QRegularExpression exactVersion(
            QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$"));
        if (!exactCommit.match(expectedBuildCommit).hasMatch()) {
            if (errorOut) {
                *errorOut = QString::fromUtf8(
                    "This ForkMesh build has no exact source revision, so it "
                    "cannot prove that a published same-version binary is "
                    "current. Rebuild from a Git checkout (or configure a "
                    "release archive with FORKMESH_BUILD_COMMIT_OVERRIDE), "
                    "then retry.");
            }
            return false;
        }
        if (!publishedRelease.isValid()) {
            expectedReleaseManifestDigest =
                controllerReleaseManifestDigest(expectedBuildCommit, errorOut);
            if (expectedReleaseManifestDigest.isEmpty())
                return false;
        }
        if (!exactDigest.match(expectedReleaseManifestDigest).hasMatch() ||
            !exactVersion.match(expectedReleaseVersion).hasMatch()) {
            if (errorOut) {
                *errorOut = QStringLiteral(
                    "The selected published release has invalid version or "
                    "manifest provenance.");
            }
            return false;
        }
    }

    // A checked release targets native Linux mirror services, including the
    // original controller-provisioned Vultr layout. The general desktop
    // installer intentionally refuses that layout as "unmanaged" and would
    // also install an unnecessary Qt runtime. Download the exact native asset
    // hash authenticated above, preserve either supported config location,
    // replace the service binary atomically, and health-check the restart.
    if (requirePublishedBinary && publishedRelease.isValid()) {
        QUrl blobBase(installUrl);
        blobBase.setPath(QStringLiteral(
            "/api/repo/forkmesh/forkmesh/releases/blob/sha256/"));
        blobBase.setQuery(QString());
        blobBase.setFragment(QString());
        const QString blobPrefix = blobBase.toString(QUrl::FullyEncoded);
        if (!blobBase.isValid() || blobBase.host().isEmpty() ||
            blobPrefix.isEmpty()) {
            if (errorOut)
                *errorOut = QStringLiteral(
                    "Could not resolve the native release artifact endpoint.");
            return false;
        }
        const QString pipeline = QStringLiteral(
            "set -eu; "
            "[ \"$(id -u)\" = 0 ] || { printf 'Native mirror updates require "
            "root.\\n' >&2; exit 77; }; "
            "unit=/etc/systemd/system/forkmesh-mirror-node.service; "
            "bin=/usr/local/bin/forkmesh-mirror-node; "
            "[ -f \"$unit\" ] && [ ! -L \"$unit\" ] && "
            "[ \"$(stat -c '%u' \"$unit\")\" = 0 ] && "
            "[ -x \"$bin\" ] && [ ! -L \"$bin\" ] || "
            "{ printf 'The managed native mirror service is missing or "
            "unsafe.\\n' >&2; exit 76; }; "
            "grep -Fqx 'User=forkmesh-node' \"$unit\" && "
            "grep -Fqx 'Group=forkmesh-node' \"$unit\" && "
            "id forkmesh-node >/dev/null 2>&1 || "
            "{ printf 'The managed mirror service account is invalid.\\n' "
            ">&2; exit 76; }; "
            "if grep -Fqx 'ExecStart=/usr/local/bin/forkmesh-mirror-node "
            "--config /etc/forkmesh/mirror-node.json' \"$unit\"; then "
            "config=/etc/forkmesh/mirror-node.json; "
            "elif grep -Fqx 'ExecStart=/usr/local/bin/forkmesh-mirror-node "
            "--config /var/lib/forkmesh/mirror-node.json' \"$unit\"; then "
            "config=/var/lib/forkmesh/mirror-node.json; "
            "else printf 'The mirror service command is not controller "
            "managed.\\n' >&2; exit 76; fi; "
            "grep -Fqx \"ExecStartPre=$bin --config $config --check\" "
            "\"$unit\" && [ -f \"$config\" ] && [ ! -L \"$config\" ] || "
            "{ printf 'The managed mirror configuration is unsafe.\\n' >&2; "
            "exit 76; }; "
            "case \"$(uname -m)\" in "
            "x86_64|amd64) expected=%1 ;; "
            "aarch64|arm64) expected=%2 ;; "
            "*) printf 'No native mirror-node release supports this "
            "architecture.\\n' >&2; exit 75 ;; esac; "
            "[ -n \"$expected\" ] || { printf 'The signed release has no "
            "native asset for this architecture.\\n' >&2; exit 75; }; "
            "installed=\"$(sha256sum \"$bin\")\"; "
            "installed=\"${installed%% *}\"; "
            "if [ \"$installed\" = \"$expected\" ] && "
            "curl -fsS --connect-timeout 1 --max-time 3 "
            "http://127.0.0.1:8791/v1/status 2>/dev/null | "
            "grep -Fq \"\\\"version\\\":\\\"\"%4\"\\\"\"; then "
            "printf 'ForkMesh mirror-node %s is already deployed and running "
            "(sha256 %s); nothing to change.\\n' %4 \"$installed\"; "
            "exit 0; fi; "
            "blob_base=%3; "
            "candidate=\"$(mktemp "
            "/usr/local/bin/.forkmesh-mirror-node.XXXXXX)\" || exit 70; "
            "trap 'rm -f -- \"$candidate\"' EXIT HUP INT TERM; "
            "curl -fsSL --connect-timeout 20 --max-time 180 "
            "-H 'Cache-Control: no-cache' "
            "\"${blob_base}${expected}\" -o \"$candidate\" || "
            "{ printf 'Native mirror-node download failed.\\n' >&2; exit 70; "
            "}; "
            "actual=\"$(sha256sum \"$candidate\")\"; "
            "actual=\"${actual%% *}\"; [ \"$actual\" = \"$expected\" ] || "
            "{ printf 'Native mirror-node checksum verification failed.\\n' "
            ">&2; exit 74; }; "
            "chown root:root \"$candidate\"; chmod 0755 \"$candidate\"; "
            "mv -f -- \"$candidate\" \"$bin\"; "
            "deployed=\"$(sha256sum \"$bin\")\"; "
            "deployed=\"${deployed%% *}\"; "
            "[ \"$deployed\" = \"$expected\" ] || exit 74; "
            "runuser -u forkmesh-node -- \"$bin\" --config \"$config\" "
            "--set-version %4 >/dev/null || "
            "{ printf 'The native release could not update its managed "
            "configuration.\\n' >&2; exit 73; }; "
            "systemctl restart forkmesh-mirror-node.service; healthy=0; "
            "for n in 1 2 3 4 5 6 7 8 9 10; do "
            "if curl -fsS --connect-timeout 1 --max-time 2 "
            "http://127.0.0.1:8791/healthz >/dev/null 2>&1; then "
            "healthy=1; break; fi; sleep 1; done; "
            "[ \"$healthy\" = 1 ] || { printf 'The updated mirror service "
            "did not become healthy.\\n' >&2; exit 72; }; "
            "printf 'Verified and deployed ForkMesh mirror-node %s "
            "(sha256 %s).\\n' %4 \"$deployed\"")
                                     .arg(
                                         shq(publishedRelease
                                                 .linuxX86_64Sha256),
                                         shq(publishedRelease
                                                 .linuxArm64Sha256),
                                         shq(blobPrefix),
                                         shq(expectedReleaseVersion));
        // A host update also refreshes the agent CLIs the mirror serves, so
        // they never fall behind what the website's models need.
        const QString updateThenRefresh =
            forkmesh::control::withAgentCliServiceRefresh(pipeline);
        if (remoteCmd)
            *remoteCmd =
                user == QLatin1String("root")
                    ? updateThenRefresh
                    : QStringLiteral("sudo -S -p '' -- bash -c %1")
                          .arg(shq(updateThenRefresh));
        if (uploadBytes)
            uploadBytes->clear();
        if (errorOut)
            errorOut->clear();
        return true;
    }
    QString envPrefix = QStringLiteral("FORKMESH_NODE_NAME=%1").arg(shq(node));
    const QString linkUser = hostLinkUserName();
    if (!linkUser.isEmpty())
        envPrefix += QStringLiteral(" FORKMESH_OWNER=%1").arg(shq(linkUser));
    if (reinstall)
        envPrefix += QStringLiteral(" FORKMESH_REINSTALL=1");
    if (fromSource)
        envPrefix += QStringLiteral(" FORKMESH_FROM_SOURCE=1 FORKMESH_RESTART=1");
    if (requirePublishedBinary) {
        envPrefix += QStringLiteral(
            " FORKMESH_RELEASE=latest FORKMESH_NO_SOURCE_FALLBACK=1 "
            "FORKMESH_EXPECTED_BUILD_COMMIT=%1 "
            "FORKMESH_EXPECTED_RELEASE_VERSION=%2 "
            "FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256=%3")
                         .arg(shq(expectedBuildCommit),
                              shq(expectedReleaseVersion),
                              shq(expectedReleaseManifestDigest));
        if (!reinstall)
            envPrefix += QStringLiteral(" FORKMESH_RESTART=1");
    }
    const bool provisionDirectHttps =
        m_vultrProvisionActive && !m_vultrDnsHostname.isEmpty() &&
        !m_vultrTunnelApiToken.isEmpty();
    if (provisionDirectHttps) {
        const QString zone = m_vultrDnsHostname.section(QLatin1Char('.'), 1);
        envPrefix +=
            QStringLiteral(
                " FORKMESH_PUBLIC_HOSTNAME=%1 FORKMESH_PUBLIC_ZONE=%2 "
                "FORKMESH_RELAY_HOSTNAME=%3 "
                "FORKMESH_REQUIRE_DIRECT_HTTPS=1")
                .arg(shq(m_vultrDnsHostname), shq(zone),
                     shq(catalogApiUrl().host()));
    }

    QString pipeline;
    if (requirePublishedBinary) {
        const QString expected =
            QStringLiteral("ForkMesh ") + expectedReleaseVersion;
        pipeline =
            QStringLiteral(
                "installer=\"$(mktemp \"${TMPDIR:-/tmp}/"
                "forkmesh-installer.XXXXXX\")\" || exit 70; "
                "if ! command -v sha256sum >/dev/null 2>&1 && "
                "! command -v shasum >/dev/null 2>&1; then "
                "printf 'A SHA-256 tool is required for a verified ForkMesh "
                "binary install.\\n' >&2; rm -f \"$installer\"; exit 67; fi; "
                "if ! curl -fsSL -H 'Cache-Control: no-cache' %1 "
                "-o \"$installer\"; then "
                "printf 'ForkMesh installer download failed.\\n' >&2; "
                "rm -f \"$installer\"; exit 70; fi; "
                "%2 bash \"$installer\"; st=$?; rm -f \"$installer\"; "
                "if [ \"$st\" -ne 0 ]; then exit \"$st\"; fi; "
                "bin=\"$HOME/.local/bin/forkmesh\"; "
                "if [ ! -x \"$bin\" ]; then "
                "printf 'ForkMesh binary missing after install: %s\\n' "
                "\"$bin\" >&2; exit 66; fi; "
                "actual=\"$(\"$bin\" --version 2>&1)\"; expected=%3; "
                "if [ \"$actual\" != \"$expected\" ]; then "
                "printf 'ForkMesh version check failed: expected %s, "
                "got %s\\n' \"$expected\" \"$actual\" >&2; exit 65; fi; "
                "commit_output=\"$(mktemp \"${TMPDIR:-/tmp}/"
                "forkmesh-build-commit.XXXXXX\")\" || exit 63; "
                "\"$bin\" --build-commit >\"$commit_output\" 2>&1 & "
                "commit_pid=$!; "
                "probe_ticks=0; "
                "while kill -0 \"$commit_pid\" 2>/dev/null && "
                "[ \"$probe_ticks\" -lt 50 ]; do sleep 0.1; "
                "probe_ticks=$((probe_ticks + 1)); done; "
                "commit_timed_out=0; "
                "if kill -0 \"$commit_pid\" 2>/dev/null; then "
                "commit_timed_out=1; "
                "kill \"$commit_pid\" 2>/dev/null || true; "
                "sleep 0.2; "
                "kill -KILL \"$commit_pid\" 2>/dev/null || true; fi; "
                "commit_status=0; "
                "wait \"$commit_pid\" || commit_status=$?; "
                "actual_commit=\"$(head -c 128 \"$commit_output\" | "
                "tr -d '\\r\\n')\"; rm -f \"$commit_output\"; "
                "expected_commit=%4; "
                "if [ \"$commit_timed_out\" -ne 0 ] || "
                "[ \"$commit_status\" -ne 0 ] || "
                "[ \"$actual_commit\" != \"$expected_commit\" ]; then "
                "printf 'ForkMesh source-revision check failed: the installed "
                "artifact did not report expected commit %s.\\n' "
                "\"$expected_commit\" >&2; "
                "exit 64; fi; "
                "printf 'Verified %s from source commit %s using the "
                "published checksum-verified release.\\n' \"$actual\" "
                "\"$actual_commit\"")
                .arg(shq(installUrl), envPrefix, shq(expected),
                     shq(expectedBuildCommit));
    } else {
        pipeline =
            QStringLiteral("curl -fsSL %1 | %2 bash")
                .arg(shq(installUrl), envPrefix);
    }
    const bool needSudo = user != QStringLiteral("root");
    if (doUpload) {
        QString os = QSysInfo::kernelType(); // "linux" / "darwin" / "winnt"
        if (os == QStringLiteral("darwin"))
            os = QStringLiteral("macos");
        else if (os == QStringLiteral("winnt"))
            os = QStringLiteral("windows");
        const QString localBinarySha256 =
            QString::fromLatin1(
                QCryptographicHash::hash(
                    bytes, QCryptographicHash::Sha256)
                    .toHex());
        pipeline =
            QStringLiteral(
                "up=\"$(mktemp \"${TMPDIR:-/tmp}/forkmesh-upload.XXXXXX\")\" && "
                "while IFS= read -r l; do [ \"$l\" = %1 ] && break; done && "
                "cat > \"$up\" && curl -fsSL %2 | %3 "
                "FORKMESH_LOCAL_BINARY=\"$up\" FORKMESH_LOCAL_OS=%4 "
                "FORKMESH_LOCAL_ARCH=%5 "
                "FORKMESH_LOCAL_BINARY_SHA256=%6 bash; "
                "st=$?; rm -f \"$up\"; exit $st")
                .arg(shq(kHostUploadMarker), shq(installUrl), envPrefix,
                     shq(os), shq(QSysInfo::currentCpuArchitecture()),
                     shq(localBinarySha256));
    }
    if (provisionDirectHttps) {
        // The Cloudflare credential is the first SSH stdin line. The command
        // contains only this fixed reader and non-secret hostname/zone values,
        // so process listings and the install transcript cannot expose it.
        pipeline =
            QStringLiteral(
                "IFS= read -r fm_cf_token || exit 68; "
                "CLOUDFLARE_API_TOKEN=\"$fm_cf_token\"; "
                "export CLOUDFLARE_API_TOKEN; unset fm_cf_token; ") +
            pipeline;
    }
    // Update, Install (binary) and the fleet runs all come through here:
    // each also refreshes the agent CLIs a mirror already serves to
    // forkmesh-node (a no-op on hosts without them).
    pipeline = forkmesh::control::withAgentCliServiceRefresh(pipeline);
    const QString cmd =
        needSudo
            ? QStringLiteral("sudo -S -p '' -- bash -c %1").arg(shq(pipeline))
            : pipeline;

    if (remoteCmd)
        *remoteCmd = cmd;
    if (uploadBytes)
        *uploadBytes = bytes;
    return true;
}

void MainWindow::runHostInstall(bool forceUploadBinary,
                                std::function<void(bool)> onFinished,
                                bool reinstall, bool fromSource,
                                bool suppressFailureStatus)
{
    const QString attemptBanner = m_hostInstallAttemptBanner;
    m_hostInstallAttemptBanner.clear();
    if ((m_hostInstallProcess &&
         m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("An install is already running."));
        if (onFinished)
            onFinished(false);
        return;
    }

    const QString ip = m_hostIpEdit ? m_hostIpEdit->text().trimmed() : QString();
    const QString user = m_hostUserEdit ? m_hostUserEdit->text().trimmed() : QString();
    const QString pass = m_hostPassEdit ? m_hostPassEdit->text() : QString();
    const QString node = m_hostNameEdit ? m_hostNameEdit->text().trimmed() : QString();
    if (ip.isEmpty() || user.isEmpty() || node.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QString::fromUtf8(
                "Enter the host IP, SSH username and a node name first. "
                "Leave the password blank to use the shared ForkMesh SSH "
                "key."));
        if (onFinished)
            onFinished(false);
        return;
    }
    const QString installUrl = installScriptUrl();
    if (installUrl.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("Could not resolve the installer URL."));
        if (onFinished)
            onFinished(false);
        return;
    }

    const bool uploadBinary = !fromSource &&
        (forceUploadBinary ||
         (m_hostUploadBinaryCheck && m_hostUploadBinaryCheck->isChecked()));
    const bool needSudo = user != QStringLiteral("root");
    QString remoteCmd;
    QByteArray uploadBytes;
    QString buildErr;
    forkmesh::runners::RunnerSetupHandoff runnerSetup;
    runnerSetup.token = m_hostRunnerToken;
    runnerSetup.name = m_hostRunnerName;
    runnerSetup.appUrl = m_hostRunnerApp;
    const bool runnerInstall = runnerSetup.isValid() &&
        node.toLower() == runnerSetup.name && !reinstall && !fromSource;
    if (runnerInstall) {
        if (!confirmHostKeyOnFirstUse(ip)) {
            if (m_hostInstallStatus)
                m_hostInstallStatus->setText(QStringLiteral(
                    "Runner setup cancelled: the server's host key was not "
                    "confirmed. Nothing was sent to it."));
            if (onFinished)
                onFinished(false);
            return;
        }
        remoteCmd = forkmesh::runners::runnerInstallRemoteCommand(
            runnerSetup, forkmesh::runners::runnerInstallScriptUrl(installUrl),
            user, !pass.isEmpty());
    } else if (!buildHostInstallCommand(ip, user, node, uploadBinary, reinstall,
                                 fromSource, false, &remoteCmd,
                                 &uploadBytes, &buildErr)) {
        m_hostInstallLastFailure = buildErr;
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(buildErr);
        if (onFinished)
            onFinished(false);
        return;
    }
    const bool hasUpload = !uploadBytes.isEmpty();
    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(
            ip, user, pass, remoteCmd, &sshError,
            savedHostIdentityFile(node, ip, user));
    if (ssh.program.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(sshError);
        if (onFinished)
            onFinished(false);
        return;
    }

    if (!runnerInstall)
        rememberHost(node, ip, user, pass, QStringLiteral("installing"));

    if (attemptBanner.isEmpty())
        m_hostInstallLog->clear();
    m_hostInstallLogCarry.clear();
    m_hostInstallLogFg = -1;
    m_hostInstallLogBold = false;
    if (!attemptBanner.isEmpty())
        appendHostInstallLog(
            QString::fromUtf8("\n\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94"
                              "\x80 %1 \xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
                              "\xE2\x94\x80\n")
                .arg(attemptBanner));
    m_hostInstallRawTail.clear();
    m_hostInstallLastFailure.clear();
    m_hostInstallLinkTail.clear();
    m_hostLinkPrompted = false;
    appendHostInstallLog(
        QStringLiteral("$ ssh %1@%2 %3\n").arg(
            user, ip, forkmesh::runners::redactRunnerToken(remoteCmd)));
    appendHostInstallLog(timestampedHostInstallEvent(
        m_vultrProvisionActive
            ? QStringLiteral(
                  "Connecting to %1 as %2 and starting the native mirror bootstrap ...\n\n")
                  .arg(ip, user)
            : QStringLiteral("Connecting to %1 as %2 and running %3 ...\n\n")
                  .arg(ip, user, installUrl)));
    if (hasUpload)
        appendHostInstallLog(timestampedHostInstallEvent(
            QString::fromUtf8("Uploading the native mirror package (%1 MB) "
                              "over the SSH session\xE2\x80\xA6\n")
                .arg(QString::number(uploadBytes.size() / (1024.0 * 1024.0),
                                     'f', 1))));
    if (m_hostInstallStatus)
        m_hostInstallStatus->setText(
            hasUpload
                ? QString::fromUtf8(
                      "Uploading the release and installing on %1\xE2\x80\xA6")
                      .arg(ip)
                : QString::fromUtf8("Installing on %1\xE2\x80\xA6").arg(ip));
    if (m_hostInstallButton)
        m_hostInstallButton->setEnabled(false);

    auto *proc = new QProcess(this);
    m_hostInstallProcess = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProcessEnvironment(ssh.environment);

    // Drive the Network rail lamp from the process state itself: starting,
    // failing to start and finishing all pass through here, so no exit path
    // can leave the lamp blinking after the install is over.
    connect(proc, &QProcess::stateChanged, this,
            [this](QProcess::ProcessState state) {
                m_hostInstallDeploying = state != QProcess::NotRunning;
                updateNetworkDeployBlink();
            });

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        const QString chunk = forkmesh::runners::redactRunnerToken(
            QString::fromUtf8(proc->readAllStandardOutput()));
        appendHostInstallLog(chunk);
        m_hostInstallRawTail = (m_hostInstallRawTail + chunk).right(4000);
        if (!m_hostLinkPrompted) {
            m_hostInstallLinkTail = (m_hostInstallLinkTail + chunk).right(512);
            static const QRegularExpression linkRe(
                QStringLiteral("FORKMESH LINK CODE:\\s*([0-9]{6})"));
            const QRegularExpressionMatch m = linkRe.match(m_hostInstallLinkTail);
            if (m.hasMatch()) {
                m_hostLinkPrompted = true;
                const QString code = m.captured(1);
                QString linkUser;
                const QString signer = hostLinkSigningAccountName(&linkUser);
                if (!signer.isEmpty()) {
                    appendHostInstallLog(QString::fromUtf8(
                        "\nLinking node to your user account (%1)\xE2\x80\xA6\n")
                        .arg(linkUser));
                    submitHostLinkCode(code);
                } else {
                    appendHostInstallLog(QString::fromUtf8(
                        "\n[warning] Not auto-linking this host: this node is "
                        "not linked to a user account. Link this node to your "
                        "user account, then reinstall the host to attach it.\n"));
                    promptHostLinkCode(code);
                }
            }
        }
    });
    connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            appendHostInstallLog(QString::fromUtf8(
                "\n[error] Could not start SSH. Install OpenSSH (and sshpass "
                "only when using password login) and try again.\n"));
    });
    connect(proc, &QProcess::finished, this,
            [this, ip, user, node, pass, onFinished, reinstall,
             suppressFailureStatus, runnerInstall](int code,
                                                   QProcess::ExitStatus status) {
                if (m_hostInstallButton)
                    m_hostInstallButton->setEnabled(true);
                const bool ok = status == QProcess::NormalExit && code == 0;
                const QString verb = reinstall ? QStringLiteral("Reinstall")
                                               : QStringLiteral("Install");
                if (ok && runnerInstall) {
                    m_hostRunnerToken.clear();
                    m_hostRunnerName.clear();
                    m_hostRunnerApp.clear();
                    appendHostInstallLog(
                        QStringLiteral("\n") + timestampedHostInstallEvent(
                            QString::fromUtf8(
                                "\xE2\x9C\x94 Runner \"%1\" is installed and "
                                "registered. Open Runners on the dashboard and "
                                "press Connect Claude to sign its agent CLI "
                                "in.\n").arg(node)));
                    if (m_hostInstallStatus)
                        m_hostInstallStatus->setText(QString::fromUtf8(
                            "\xE2\x9C\x94 Runner \"%1\" set up on %2.")
                            .arg(node, ip));
                } else if (ok) {
                    appendHostInstallLog(
                        QStringLiteral("\n") + timestampedHostInstallEvent(
                            QString::fromUtf8(
                                "\xE2\x9C\x94 %1 finished. Node \"%2\" will join "
                                "the network and appear on Network > Nodes "
                                "shortly.\n")
                                .arg(verb, node)));
                    if (m_hostInstallStatus)
                        m_hostInstallStatus->setText(QString::fromUtf8(
                            "\xE2\x9C\x94 %1ed on %2 as node \"%3\".")
                            .arg(reinstall ? QStringLiteral("Reinstall")
                                           : QStringLiteral("Install"),
                                 ip, node));
                    rememberHost(node, ip, user, pass);
                } else {
                    appendHostInstallLog(
                        QStringLiteral("\n") + timestampedHostInstallEvent(
                            QString::fromUtf8(
                                "\xE2\x9C\x98 %1 failed (exit %2).\n")
                                .arg(verb)
                                .arg(code)));
                    m_hostInstallLastFailure =
                        forkmesh::control::sshFailureSummary(
                            code, m_hostInstallRawTail);
                    const QString hint = forkmesh::control::sshConnectionFailureHint(
                        code, m_hostInstallRawTail, ip);
                    if (!hint.isEmpty())
                        appendHostInstallLog(
                            QStringLiteral("\n%1\n").arg(hint));
                    if (!suppressFailureStatus) {
                        if (m_hostInstallStatus)
                            m_hostInstallStatus->setText(QString::fromUtf8(
                                "\xE2\x9C\x98 Install failed \xE2\x80\x94 see "
                                "the output above."));
                        if (!runnerInstall)
                            rememberHost(node, ip, user, pass,
                                         QStringLiteral("install failed"));
                    }
                }
                if (m_hostInstallProcess) {
                    m_hostInstallProcess->deleteLater();
                    m_hostInstallProcess = nullptr;
                }
                if (onFinished)
                    onFinished(ok);
            });

    proc->start(ssh.program, ssh.arguments);
    if (needSudo)
        proc->write((pass + QStringLiteral("\n")).toUtf8());
    if (m_vultrProvisionActive && !m_vultrDnsHostname.isEmpty() &&
        !m_vultrTunnelApiToken.isEmpty())
        proc->write((m_vultrTunnelApiToken + QStringLiteral("\n")).toUtf8());
    if (hasUpload) {
        if (!m_vultrProvisionActive)
            proc->write((kHostUploadMarker + QStringLiteral("\n")).toUtf8());
        proc->write(uploadBytes);
    }
    proc->closeWriteChannel();
}

bool MainWindow::confirmHostKeyOnFirstUse(const QString &host)
{
    const QString dir = forkmesh::control::sharedHostKeyDirectory();
    if (dir.isEmpty())
        return false;
    const QString knownHosts = QDir(dir).filePath(QStringLiteral("known_hosts"));
    QProcess lookup;
    lookup.start(QStringLiteral("ssh-keygen"),
                 {QStringLiteral("-F"), host, QStringLiteral("-f"), knownHosts});
    if (lookup.waitForFinished(5000) && lookup.exitCode() == 0 &&
        !lookup.readAllStandardOutput().trimmed().isEmpty())
        return true;
    QProcess scan;
    scan.start(QStringLiteral("ssh-keyscan"),
               {QStringLiteral("-T"), QStringLiteral("8"), QStringLiteral("-t"),
                QStringLiteral("ed25519,ecdsa,rsa"), host});
    if (!scan.waitForFinished(15000) || scan.exitCode() != 0) {
        appendHostInstallLog(QStringLiteral(
            "[error] Could not read %1's SSH host key (ssh-keyscan).\n").arg(host));
        return false;
    }
    const QByteArray keys = scan.readAllStandardOutput();
    QProcess fingerprint;
    fingerprint.start(QStringLiteral("ssh-keygen"),
                      {QStringLiteral("-lf"), QStringLiteral("-")});
    fingerprint.write(keys);
    fingerprint.closeWriteChannel();
    if (!fingerprint.waitForFinished(5000))
        return false;
    const QStringList prints = forkmesh::runners::parseHostKeyFingerprints(
        QString::fromUtf8(fingerprint.readAllStandardOutput()));
    if (prints.isEmpty()) {
        appendHostInstallLog(QStringLiteral(
            "[error] %1 answered with no SSH host key ForkMesh accepts.\n").arg(host));
        return false;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Confirm the server's host key"),
        forkmesh::runners::hostKeyConfirmationText(host, prints),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return false;
    QFile file(knownHosts);
    if (!file.open(QIODevice::Append))
        return false;
    for (const QByteArray &line : keys.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !trimmed.startsWith('#'))
            file.write(trimmed + '\n');
    }
    file.close();
    appendHostInstallLog(QStringLiteral("Trusted %1's host key: %2\n")
                             .arg(host, prints.join(QStringLiteral(", "))));
    return true;
}

void MainWindow::runHostInstallAllFromBinary()
{
    runHostDeployAllParallel(FleetDeployMode::InstallBinary);
}

MainWindow::FleetDeployOptions
MainWindow::fleetDeployOptions(FleetDeployMode mode)
{
    switch (mode) {
    case FleetDeployMode::InstallBinary:
        return {/*uploadBinary=*/false, /*reinstall=*/false,
                /*fromSource=*/false, /*requirePublishedBinary=*/true};
    case FleetDeployMode::Reinstall:
        return {/*uploadBinary=*/false, /*reinstall=*/true,
                /*fromSource=*/false, /*requirePublishedBinary=*/true};
    case FleetDeployMode::UpdateSource:
        return {/*uploadBinary=*/false, /*reinstall=*/false,
                /*fromSource=*/true, /*requirePublishedBinary=*/false};
    }
    return {};
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testFleetBinaryInstallRemoteCommand(
    bool reinstall, qsizetype *uploadByteCount, QString *errorOut)
{
    const FleetDeployOptions options = fleetDeployOptions(
        reinstall ? FleetDeployMode::Reinstall
                  : FleetDeployMode::InstallBinary);
    QString remoteCommand;
    QByteArray uploadBytes;
    QString error;
    const bool ok = buildHostInstallCommand(
        QStringLiteral("host.example"), QStringLiteral("root"),
        QStringLiteral("test-node"), options.uploadBinary, options.reinstall,
        options.fromSource, options.requirePublishedBinary, &remoteCommand,
        &uploadBytes, &error);
    if (uploadByteCount)
        *uploadByteCount = uploadBytes.size();
    if (errorOut)
        *errorOut = error;
    return ok ? remoteCommand : QString();
}

QString MainWindow::testPublishedReleaseInstallRemoteCommand(QString *errorOut)
{
    PublishedReleaseDeployment publishedRelease;
    publishedRelease.tag = QStringLiteral("v9.8.7");
    publishedRelease.version = QStringLiteral("9.8.7");
    publishedRelease.buildCommit = QString(40, QLatin1Char('a'));
    publishedRelease.manifestSha256 = QString(64, QLatin1Char('b'));
    publishedRelease.linuxX86_64Sha256 = QString(64, QLatin1Char('c'));
    publishedRelease.linuxArm64Sha256 = QString(64, QLatin1Char('d'));
    publishedRelease.artifactSha256s = {
        publishedRelease.linuxX86_64Sha256,
        publishedRelease.linuxArm64Sha256};
    QString remoteCommand;
    QByteArray uploadBytes;
    QString error;
    const bool ok = buildHostInstallCommand(
        QStringLiteral("host.example"), QStringLiteral("root"),
        QStringLiteral("test-node"), /*uploadBinary=*/false,
        /*reinstall=*/false, /*fromSource=*/false,
        /*requirePublishedBinary=*/true, &remoteCommand, &uploadBytes, &error,
        publishedRelease);
    if (errorOut)
        *errorOut = error;
    return ok && uploadBytes.isEmpty() ? remoteCommand : QString();
}

QString MainWindow::testDirectBinaryInstallRemoteCommand(
    qsizetype *uploadByteCount, QString *errorOut)
{
    QString remoteCommand;
    QByteArray uploadBytes;
    QString error;
    const bool ok = buildHostInstallCommand(
        QStringLiteral("host.example"), QStringLiteral("root"),
        QStringLiteral("test-node"), /*uploadBinary=*/true,
        /*reinstall=*/false, /*fromSource=*/false,
        /*requirePublishedBinary=*/false, &remoteCommand, &uploadBytes,
        &error);
    if (uploadByteCount)
        *uploadByteCount = uploadBytes.size();
    if (errorOut)
        *errorOut = error;
    uploadBytes.fill('\0');
    uploadBytes.clear();
    return ok ? remoteCommand : QString();
}

QString MainWindow::testVultrGoMirrorInstallRemoteCommand(
    qsizetype *uploadByteCount, QString *errorOut)
{
    const QString previousHostname = m_vultrDnsHostname;
    m_vultrDnsHostname = QStringLiteral("mirror17.forkmesh.com");
    QString remoteCommand;
    QByteArray uploadBytes;
    QString error;
    const bool ok = buildVultrMirrorNodeInstallCommand(
        QStringLiteral("mirror17"), QStringLiteral("203.0.113.17"),
        &remoteCommand, &uploadBytes, &error);
    m_vultrDnsHostname = previousHostname;
    if (uploadByteCount)
        *uploadByteCount = uploadBytes.size();
    if (errorOut)
        *errorOut = error;
    uploadBytes.fill('\0');
    uploadBytes.clear();
    return ok ? remoteCommand : QString();
}
#endif

void MainWindow::runHostReinstallAllFromBinary()
{
    if (!m_hostsTable || m_hostsTable->rowCount() == 0) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QStringLiteral("No saved hosts to reinstall."));
        return;
    }
    const int rowCount = m_hostsTable->rowCount();
    const QMessageBox::StandardButton choice = QMessageBox::warning(
        this, QStringLiteral("Uninstall + reinstall all hosts"),
        QString::fromUtf8(
            "This will REMOVE ForkMesh and ALL of its data (identity key, "
            "mirrors, chat) from every one of your %1 saved host(s), then "
            "install the published ForkMesh v" FORKMESH_VERSION
            " binary and re-link each one to your account.\n\nThis cannot be "
            "undone. Continue?")
            .arg(rowCount),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice != QMessageBox::Yes)
        return;
    runHostDeployAllParallel(FleetDeployMode::Reinstall);
}

void MainWindow::runHostUpdateAllFromSource()
{
    if (!m_hostsTable || m_hostsTable->rowCount() == 0) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QStringLiteral("No saved hosts to update."));
        return;
    }
    const int rowCount = m_hostsTable->rowCount();
    const QMessageBox::StandardButton choice = QMessageBox::question(
        this, QStringLiteral("Update all hosts from source"),
        QString::fromUtf8(
            "For each of your %1 saved host(s): SSH in, pull the latest ForkMesh "
            "source, rebuild the client from it and restart the node. Each host "
            "keeps its identity key and mirrored data.\n\nBuilding from source on "
            "the host runs on all hosts at once — each streams into its own pane "
            "below. Continue?")
            .arg(rowCount),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice != QMessageBox::Yes)
        return;
    runHostDeployAllParallel(FleetDeployMode::UpdateSource);
}

void MainWindow::runHostDeployAllParallel(FleetDeployMode mode)
{
    runHostDeployAllParallel(mode, {});
}

void MainWindow::runHostDeployAllParallel(
    FleetDeployMode mode,
    const PublishedReleaseDeployment &publishedRelease)
{
    if ((m_hostInstallProcess &&
         m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("A host session is already running."));
        return;
    }
    if (!publishedRelease.isValid()) {
        m_hostDeployReleaseOwner.clear();
        m_hostDeployReleaseName.clear();
        m_hostDeployReleaseTag.clear();
        m_hostDeployReleaseRefsReady = true;
    }
    // Load non-sensitive host metadata. Every session authenticates with the
    // shared ForkMesh key; a password cached in this process (or, failing
    // both, an SSH agent/default key) is only the fallback.
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    FleetDeployOptions options = fleetDeployOptions(mode);
    options.publishedRelease = publishedRelease;
    options.activeHostsOnly = publishedRelease.isValid();
    struct Target { QString node, ip, user, pass; };
    QList<Target> targets;
    int skipped = 0;
    for (const QJsonValue &v : hosts) {
        const QJsonObject h = v.toObject();
        if (options.activeHostsOnly &&
            !forkmesh::control::savedHostMayBeActive(h)) {
            ++skipped;
            continue;
        }
        Target t{h.value("name").toString().trimmed(),
                 h.value("ip").toString().trimmed(),
                 h.value("user").toString().trimmed(),
                 QString()};
        t.pass = m_hostSessionPasswords.value(
            forkmesh::control::savedHostCredentialKey(
                t.node, t.ip, t.user));
        if (t.node.isEmpty() || t.ip.isEmpty() || t.user.isEmpty()) {
            ++skipped;
            continue;
        }
        targets.append(t);
    }
    if (targets.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QString::fromUtf8(
                "No saved host has complete node, address, and SSH username "
                "metadata."));
        return;
    }

    qDeleteAll(m_hostDeploySessions);
    m_hostDeploySessions.clear();
    if (m_hostDeployGrid) {
        while (QLayoutItem *item = m_hostDeployGrid->takeAt(0)) {
            if (QWidget *w = item->widget())
                w->deleteLater();
            delete item;
        }
    }

    int cols = 1;
    while (cols * cols < targets.size())
        ++cols;

    for (int idx = 0; idx < targets.size(); ++idx) {
        const Target &t = targets.at(idx);
        QLabel *header = nullptr;
        QPlainTextEdit *log = nullptr;
        if (m_hostDeployGrid) {
            auto *card = new QFrame;
            card->setObjectName("leaderboardCard");
            card->setFrameShape(QFrame::StyledPanel);
            auto *col = new QVBoxLayout(card);
            col->setContentsMargins(10, 8, 10, 8);
            col->setSpacing(6);

            header = new QLabel(
                QString::fromUtf8("%1 \xE2\x80\x94 %2@%3 \xE2\x80\xA6")
                    .arg(t.node, t.user, t.ip));
            QFont hf = header->font();
            hf.setBold(true);
            header->setFont(hf);
            header->setWordWrap(true);
            col->addWidget(header);

            log = new QPlainTextEdit;
            log->setObjectName("actionLog");
            log->setReadOnly(true);
            log->setLineWrapMode(QPlainTextEdit::NoWrap);
            log->setMinimumHeight(200);
            log->setMinimumWidth(260);
            applyLogFont(log);
            col->addWidget(log, 1);

            m_hostDeployGrid->addWidget(card, idx / cols, idx % cols);
        }

        auto *s = new HostDeploySession;
        s->node = t.node;
        s->ip = t.ip;
        s->user = t.user;
        s->pass = t.pass;
        s->log = log;
        s->header = header;
        m_hostDeploySessions.append(s);
    }

    if (m_hostDeployLabel) {
        m_hostDeployLabel->setText(
            QString::fromUtf8("Live output \xE2\x80\x94 %1 host(s) deploying in "
                              "parallel").arg(targets.size()));
        m_hostDeployLabel->setVisible(true);
    }
    if (m_hostDeployPanel)
        m_hostDeployPanel->setVisible(true);

    m_hostDeployRemaining = m_hostDeploySessions.size();
    m_hostDeployFailed = 0;
    updateNetworkDeployBlink();
    if (m_hostInstallAllButton)
        m_hostInstallAllButton->setEnabled(false);
    if (m_hostReinstallAllButton)
        m_hostReinstallAllButton->setEnabled(false);
    if (m_hostUpdateAllSourceButton)
        m_hostUpdateAllSourceButton->setEnabled(false);
    if (m_hostInstallButton)
        m_hostInstallButton->setEnabled(false);
    if (m_hostInstallStatus)
        m_hostInstallStatus->setText(
            QString::fromUtf8("Deploying to %1 host(s) in parallel\xE2\x80\xA6%2")
                .arg(m_hostDeployRemaining)
                .arg(skipped ? QString::fromUtf8(" (%1 skipped \xE2\x80\x94 "
                                                 "incomplete metadata)").arg(skipped)
                             : QString()));
    if (publishedRelease.isValid()) {
        logSystem(QStringLiteral(
                      "Release: deploying signed %1 mirror-node artifact to "
                      "%2 installed host%3 in parallel.")
                      .arg(publishedRelease.tag)
                      .arg(m_hostDeployRemaining)
                      .arg(m_hostDeployRemaining == 1
                               ? QString()
                               : QStringLiteral("s")));
    }

    // Snapshot the list first: startHostDeploySession may fail synchronously and
    // finish a session (mutating m_hostDeploySessions is not expected, but the
    // finished callback can fire re-entrantly), so iterate a stable copy.
    const QList<HostDeploySession *> toStart = m_hostDeploySessions;
    for (HostDeploySession *s : toStart)
        startHostDeploySession(s, options);
}

void MainWindow::startHostDeploySession(HostDeploySession *session,
                                        const FleetDeployOptions &options)
{
    if (!session)
        return;

    QString remoteCmd;
    QByteArray uploadBytes;
    QString buildErr;
    if (!buildHostInstallCommand(session->ip, session->user, session->node,
                                 options.uploadBinary, options.reinstall,
                                 options.fromSource,
                                 options.requirePublishedBinary, &remoteCmd,
                                 &uploadBytes, &buildErr,
                                 options.publishedRelease)) {
        appendHostDeployLog(session,
                            QString::fromUtf8("\n\xE2\x9C\x98 %1\n").arg(buildErr));
        onHostDeploySessionFinished(session, false);
        return;
    }
    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(
            session->ip, session->user, session->pass, remoteCmd, &sshError,
            savedHostIdentityFile(session->node, session->ip, session->user));
    if (ssh.program.isEmpty()) {
        appendHostDeployLog(
            session, QString::fromUtf8("\n\xE2\x9C\x98 %1\n").arg(sshError));
        onHostDeploySessionFinished(session, false);
        return;
    }

    const bool needSudo = session->user != QStringLiteral("root");
    appendHostDeployLog(
        session, QString::fromUtf8("$ ssh %1@%2 \xE2\x80\xA6\nConnecting and "
                                   "running the installer\xE2\x80\xA6\n\n")
                     .arg(session->user, session->ip));
    if (options.requirePublishedBinary)
        appendHostDeployLog(
            session,
            QStringLiteral(
                "Installing the published ForkMesh v%1 binary; the host will "
                "verify its release checksum, reported version and exact "
                "source commit.\n")
                .arg(options.publishedRelease.isValid()
                         ? options.publishedRelease.version
                         : QStringLiteral(FORKMESH_VERSION)));
    else if (options.uploadBinary && !options.fromSource)
        appendHostDeployLog(
            session, QString::fromUtf8("Uploading this app's release binary "
                                       "(%1 MB) over the SSH session\xE2\x80\xA6\n")
                         .arg(QString::number(uploadBytes.size() /
                                                  (1024.0 * 1024.0),
                                              'f', 1)));

    auto *proc = new QProcess(this);
    session->proc = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProcessEnvironment(ssh.environment);

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, session, proc] {
        const QString chunk = QString::fromUtf8(proc->readAllStandardOutput());
        appendHostDeployLog(session, chunk);
        if (session->linkPrompted)
            return;
        session->linkTail = (session->linkTail + chunk).right(512);
        static const QRegularExpression linkRe(
            QStringLiteral("FORKMESH LINK CODE:\\s*([0-9]{6})"));
        const QRegularExpressionMatch m = linkRe.match(session->linkTail);
        if (!m.hasMatch())
            return;
        session->linkPrompted = true;
        QString linkUser;
        const QString signer = hostLinkSigningAccountName(&linkUser);
        if (!signer.isEmpty()) {
            appendHostDeployLog(
                session, QString::fromUtf8("\nLinking node to your user account "
                                           "(%1)\xE2\x80\xA6\n").arg(linkUser));
            submitHostLinkCode(m.captured(1));
        } else {
            appendHostDeployLog(
                session, QString::fromUtf8(
                             "\n[warning] Not auto-linking: this app is not "
                             "linked to a user account.\n"));
        }
    });
    connect(proc, &QProcess::errorOccurred, this,
            [this, session](QProcess::ProcessError e) {
                if (e != QProcess::FailedToStart)
                    return;
                appendHostDeployLog(
                    session,
                    QString::fromUtf8(
                        "\n[error] Could not start SSH. Install OpenSSH (and "
                        "sshpass only when using password login).\n"));
                if (session->proc) {
                    session->proc->deleteLater();
                    session->proc = nullptr;
                }
                onHostDeploySessionFinished(session, false);
            });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, session](int code, QProcess::ExitStatus status) {
                const bool ok = status == QProcess::NormalExit && code == 0;
                if (ok)
                    appendHostDeployLog(
                        session, QString::fromUtf8(
                                     "\n\xE2\x9C\x94 Finished.\n"));
                else
                    appendHostDeployLog(
                        session, QString::fromUtf8(
                                     "\n\xE2\x9C\x98 Failed (exit %1).\n")
                                     .arg(code));
                if (session->proc) {
                    session->proc->deleteLater();
                    session->proc = nullptr;
                }
                onHostDeploySessionFinished(session, ok);
            });

    proc->start(ssh.program, ssh.arguments);
    if (needSudo)
        proc->write((session->pass + QStringLiteral("\n")).toUtf8());
    if (options.uploadBinary && !options.fromSource) {
        proc->write((kHostUploadMarker + QStringLiteral("\n")).toUtf8());
        proc->write(uploadBytes);
    }
    proc->closeWriteChannel();
}

void MainWindow::onHostDeploySessionFinished(HostDeploySession *session, bool ok)
{
    if (!session || session->finished)
        return;
    session->finished = true;
    if (!ok)
        ++m_hostDeployFailed;
    const bool releaseRollout = !m_hostDeployReleaseTag.isEmpty();
    rememberHost(session->node, session->ip, session->user, session->pass,
                 (ok || releaseRollout) ? QStringLiteral("installed")
                                        : QStringLiteral("install failed"));
    if (session->header) {
        const QString glyph = ok ? QString::fromUtf8("\xE2\x9C\x94")
                                 : QString::fromUtf8("\xE2\x9C\x98");
        session->header->setText(
            QString::fromUtf8("%1 %2 \xE2\x80\x94 %3@%4")
                .arg(glyph, session->node, session->user, session->ip));
    }
    if (m_hostDeployRemaining > 0)
        --m_hostDeployRemaining;
    updateNetworkDeployBlink();
    if (m_hostDeployRemaining == 0) {
        const QString releaseOwner = m_hostDeployReleaseOwner;
        const QString releaseName = m_hostDeployReleaseName;
        const QString releaseTag = m_hostDeployReleaseTag;
        const bool releaseRefsReady = m_hostDeployReleaseRefsReady;
        m_hostDeployReleaseOwner.clear();
        m_hostDeployReleaseName.clear();
        m_hostDeployReleaseTag.clear();
        m_hostDeployReleaseRefsReady = true;
        if (!releaseTag.isEmpty()) {
            if (m_hostDeployFailed == 0 && releaseRefsReady) {
                clearReleaseRollout(releaseOwner, releaseName, releaseTag);
                logSystem(QStringLiteral(
                              "Release: %1 deployed successfully to every "
                              "installed mirror host.")
                              .arg(releaseTag));
            } else if (m_hostDeployFailed > 0) {
                logSystem(QStringLiteral(
                              "Release: %1 deployment failed on %2 host%3; "
                              "the rollout remains pending and will resume "
                              "after restart or a successful release rerun.")
                              .arg(releaseTag)
                              .arg(m_hostDeployFailed)
                              .arg(m_hostDeployFailed == 1
                                       ? QString()
                                       : QStringLiteral("s")));
            } else {
                logSystem(QStringLiteral(
                              "Release: %1 installed on every host, but at "
                              "least one managed repository ref push failed; "
                              "the rollout remains pending for a safe retry.")
                              .arg(releaseTag));
            }
        }
        if (m_hostInstallAllButton)
            m_hostInstallAllButton->setEnabled(true);
        if (m_hostReinstallAllButton)
            m_hostReinstallAllButton->setEnabled(true);
        if (m_hostUpdateAllSourceButton)
            m_hostUpdateAllSourceButton->setEnabled(true);
        if (m_hostInstallButton)
            m_hostInstallButton->setEnabled(true);
        if (m_hostInstallStatus) {
            const int total = m_hostDeploySessions.size();
            if (m_hostDeployFailed == 0 && !releaseTag.isEmpty() &&
                !releaseRefsReady) {
                m_hostInstallStatus->setText(QStringLiteral(
                    "The binaries are installed, but repository ref sync is "
                    "still pending a safe retry."));
            } else {
                m_hostInstallStatus->setText(
                    m_hostDeployFailed == 0
                        ? QString::fromUtf8(
                              "\xE2\x9C\x94 Finished deploying to all %1 "
                              "host(s).")
                              .arg(total)
                        : QString::fromUtf8(
                              "Finished deploying to %1 host(s) "
                              "\xE2\x80\x94 %2 failed (see panes above).")
                              .arg(total)
                              .arg(m_hostDeployFailed));
            }
        }
    } else if (m_hostInstallStatus) {
        const int total = m_hostDeploySessions.size();
        m_hostInstallStatus->setText(
            QString::fromUtf8("Deploying\xE2\x80\xA6 %1/%2 done%3")
                .arg(total - m_hostDeployRemaining).arg(total)
                .arg(m_hostDeployFailed
                         ? QString::fromUtf8(", %1 failed").arg(m_hostDeployFailed)
                         : QString()));
    }
}

void MainWindow::runHostUninstall()
{
    if (m_hostInstallProcess &&
        m_hostInstallProcess->state() != QProcess::NotRunning) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("A host session is already running."));
        return;
    }

    const QString ip = m_hostIpEdit ? m_hostIpEdit->text().trimmed() : QString();
    const QString user = m_hostUserEdit ? m_hostUserEdit->text().trimmed() : QString();
    const QString pass = m_hostPassEdit ? m_hostPassEdit->text() : QString();
    const QString node = m_hostNameEdit ? m_hostNameEdit->text().trimmed() : QString();
    if (ip.isEmpty() || user.isEmpty() || node.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QString::fromUtf8(
                "Enter the host IP, SSH username and a node name first. "
                "Leave the password blank to use the shared ForkMesh SSH "
                "key."));
        return;
    }
    const QString uninstallUrl = uninstallScriptUrl();
    if (uninstallUrl.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("Could not resolve the uninstaller URL."));
        return;
    }

    auto shq = [](const QString &s) {
        QString out = s;
        out.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        return QStringLiteral("'") + out + QStringLiteral("'");
    };
    // The confirmation promised the identity key and repositories go too; the
    // uninstaller keeps user data unless it is told to purge.
    const QString pipeline = QStringLiteral(
                                 "curl -fsSL %1 | FORKMESH_ASSUME_YES=1 FORKMESH_PURGE=1 bash")
                                  .arg(shq(uninstallUrl));
    const bool needSudo = user != QStringLiteral("root");
    const QString remoteCmd =
        needSudo
            ? QStringLiteral("sudo -S -p '' -- bash -c %1").arg(shq(pipeline))
            : pipeline;

    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(
            ip, user, pass, remoteCmd, &sshError,
            savedHostIdentityFile(node, ip, user));
    if (ssh.program.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(sshError);
        return;
    }

    rememberHost(node, ip, user, pass, QStringLiteral("uninstalling"));

    m_hostInstallLog->clear();
    m_hostInstallLogCarry.clear();
    m_hostInstallLogFg = -1;
    m_hostInstallLogBold = false;
    appendHostInstallLog(
        QStringLiteral("$ ssh %1@%2 %3\n").arg(user, ip, remoteCmd));
    appendHostInstallLog(
        QStringLiteral("Connecting to %1 as %2 and running %3 ...\n\n")
            .arg(ip, user, uninstallUrl));
    if (m_hostInstallStatus)
        m_hostInstallStatus->setText(
            QString::fromUtf8("Uninstalling from %1\xE2\x80\xA6").arg(ip));
    if (m_hostInstallButton)
        m_hostInstallButton->setEnabled(false);

    auto *proc = new QProcess(this);
    m_hostInstallProcess = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProcessEnvironment(ssh.environment);

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        appendHostInstallLog(QString::fromUtf8(proc->readAllStandardOutput()));
    });
    connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            appendHostInstallLog(QString::fromUtf8(
                "\n[error] Could not start SSH. Install OpenSSH (and sshpass "
                "only when using password login) and try again.\n"));
    });
    connect(proc, &QProcess::finished, this,
            [this, ip, user, node, pass](int code, QProcess::ExitStatus status) {
                if (m_hostInstallButton)
                    m_hostInstallButton->setEnabled(true);
                const bool ok = status == QProcess::NormalExit && code == 0;
                if (ok) {
                    appendHostInstallLog(QString::fromUtf8(
                        "\n\xE2\x9C\x94 Uninstall finished. ForkMesh has been "
                        "removed from \"%1\".\n").arg(node));
                    if (m_hostInstallStatus)
                        m_hostInstallStatus->setText(QString::fromUtf8(
                            "\xE2\x9C\x94 Uninstalled from %1.").arg(ip));
                    rememberHost(node, ip, user, pass, QStringLiteral("uninstalled"));
                } else {
                    appendHostInstallLog(QString::fromUtf8(
                        "\n\xE2\x9C\x98 Uninstall failed (exit %1).\n").arg(code));
                    if (m_hostInstallStatus)
                        m_hostInstallStatus->setText(QString::fromUtf8(
                            "\xE2\x9C\x98 Uninstall failed \xE2\x80\x94 see the "
                            "output above."));
                    rememberHost(node, ip, user, pass, QStringLiteral("uninstall failed"));
                }
                if (m_hostInstallProcess) {
                    m_hostInstallProcess->deleteLater();
                    m_hostInstallProcess = nullptr;
                }
            });

    proc->start(ssh.program, ssh.arguments);
    if (needSudo)
        proc->write((pass + QStringLiteral("\n")).toUtf8());
    proc->closeWriteChannel();
}

// The freshly-installed node printed a link code. Confirming here
// offers that code to the relay signed with THIS account's key, so the new
// node is attached to the user behind this account. The code is prefilled from
// the install stream but stays editable — the person at the keyboard can also
// type a code read off any machine's screen.
void MainWindow::promptHostLinkCode(const QString &code)
{
    QString linkUser;
    if (hostLinkSigningAccountName(&linkUser).isEmpty())
        return;
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Link new node to your account"));
    auto *layout = new QVBoxLayout(dialog);
    auto *label = new QLabel(
        QStringLiteral("The machine being installed shows a link code. Confirm "
                       "it below to register the new node under your account "
                       "(<b>%1</b>).").arg(linkUser.toHtmlEscaped()));
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *codeEdit = new QLineEdit(code);
    codeEdit->setAlignment(Qt::AlignCenter);
    codeEdit->setMaxLength(6);
    codeEdit->setPlaceholderText(QStringLiteral("6-digit code"));
    layout->addWidget(codeEdit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *linkButton =
        buttons->addButton(QStringLiteral("Link node"), QDialogButtonBox::AcceptRole);
    linkButton->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, codeEdit] {
        const QString entered = codeEdit->text().trimmed();
        static const QRegularExpression sixDigits(QStringLiteral("^[0-9]{6}$"));
        if (!sixDigits.match(entered).hasMatch())
            return;
        submitHostLinkCode(entered);
        dialog->accept();
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::submitHostLinkCode(const QString &code)
{
    QString linkUser;
    const QString signer = hostLinkSigningAccountName(&linkUser);
    if (signer.isEmpty())
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-link-v1\n" + signer + "\n" + code + "\n" + ts).toUtf8();
    const QJsonObject body{{"nodeName", signer},
                           {"code", code},
                           {"ts", ts},
                           {"sig", m_profileIdentity.signData(canonical)}};
    QNetworkRequest request(accountsApiUrl("link-node"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, linkUser]() {
        const QJsonObject resp = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        QString message;
        if (resp.value(QStringLiteral("linked")).toBool()) {
            message = QString::fromUtf8(
                "\n\xE2\x9C\x94 Node \"%1\" is now linked to user \"%2\".\n")
                .arg(resp.value(QStringLiteral("node")).toString(),
                     resp.value(QStringLiteral("user")).toString(linkUser));
        } else if (resp.value(QStringLiteral("pending")).toBool()) {
            message = QString::fromUtf8(
                "\n\xE2\x9C\x94 Link code accepted; the new node will be linked "
                "to user \"%1\" as soon as it registers.\n")
                .arg(resp.value(QStringLiteral("user")).toString(linkUser));
        } else {
            message = QString::fromUtf8(
                "\n\xE2\x9C\x98 Could not link the new node (%1).\n")
                .arg(resp.value(QStringLiteral("error"))
                         .toString(QStringLiteral("network error")));
        }
        appendHostInstallLog(message);
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(message.trimmed());
    });
}

QString forkmesh::desktopBuildCommit()
{
    return QStringLiteral(FORKMESH_BUILD_COMMIT).trimmed().toLower();
}

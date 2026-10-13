#include "AgentAnnouncerVoice.h"

#include "AgentAnnouncer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>

namespace forkmesh::announce {

namespace {

constexpr int kMaxHops = 5;

// Piper 2023.11.14-2, the last self-contained release: the binary, its
// phonemiser and the ONNX runtime in one archive, nothing to install.
const char kPiperBase[] = "https://github.com/rhasspy/piper/releases/download/2023.11.14-2/";
// "Ryan", medium quality: a clear, energetic US English voice that renders
// several times faster than real time on an ordinary laptop CPU. The high
// quality model of the same voice is slower than real time on a busy machine,
// which a live commentary cannot afford.
const char kVoiceBase[] =
    "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/ryan/medium/";

VoiceAsset asset(const char *label, const QString &url, const char *sha256, qint64 bytes,
                 bool unpack)
{
    VoiceAsset a;
    a.label = QString::fromLatin1(label);
    a.url = QUrl(url);
    a.fileName = a.url.fileName();
    a.sha256 = QByteArray(sha256);
    a.bytes = bytes;
    a.unpack = unpack;
    return a;
}

} // namespace

QList<VoiceAsset> voiceAssets()
{
    QList<VoiceAsset> assets;
#if defined(Q_OS_LINUX)
    const QString cpu = QSysInfo::currentCpuArchitecture();
    if (cpu == QLatin1String("x86_64")) {
        assets << asset("speech engine",
                        QLatin1String(kPiperBase) + QStringLiteral("piper_linux_x86_64.tar.gz"),
                        "a50cb45f355b7af1f6d758c1b360717877ba0a398cc8cbe6d2a7a3a26e225992",
                        26460462, true);
    } else if (cpu == QLatin1String("arm64")) {
        assets << asset("speech engine",
                        QLatin1String(kPiperBase) + QStringLiteral("piper_linux_aarch64.tar.gz"),
                        "fea0fd2d87c54dbc7078d0f878289f404bd4d6eea6e7444a77835d1537ab88eb",
                        26004717, true);
    } else {
        return {};
    }
    assets << asset("voice", QLatin1String(kVoiceBase) + QStringLiteral("en_US-ryan-medium.onnx"),
                    "abf4c274862564ed647ba0d2c47f8ee7c9b717d27bdad9219100eb310db4047a",
                    63201294, false);
    assets << asset("voice", QLatin1String(kVoiceBase) + QStringLiteral("en_US-ryan-medium.onnx.json"),
                    "44034c056cb15681b2ad494307c7f3f2e4499d1253c700c711fa0a4607ffe78d", 4883,
                    false);
#endif
    return assets;
}

qint64 voiceDownloadBytes()
{
    qint64 total = 0;
    for (const VoiceAsset &a : voiceAssets())
        total += a.bytes;
    return total;
}

VoiceInstaller::VoiceInstaller(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent)
    , m_network(network)
{
}

VoiceInstaller::~VoiceInstaller()
{
    cancel();
}

void VoiceInstaller::start()
{
    if (m_running)
        return;
    m_assets = voiceAssets();
    if (m_assets.isEmpty() || !m_network) {
        if (onFinished)
            onFinished(false, QStringLiteral("There is no announcer voice to download for "
                                             "this kind of machine."));
        return;
    }
    if (!QDir().mkpath(voiceDirectory())) {
        if (onFinished)
            onFinished(false, QStringLiteral("Could not create %1.").arg(voiceDirectory()));
        return;
    }
    m_running = true;
    m_index = -1;
    m_doneBefore = 0;
    nextAsset();
}

void VoiceInstaller::cancel()
{
    if (!m_running)
        return;
    m_running = false;
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    if (m_tar) {
        m_tar->disconnect(this);
        m_tar->kill();
        m_tar->deleteLater();
        m_tar = nullptr;
    }
    if (m_file) {
        m_file->close();
        m_file->remove();
        m_file.reset();
    }
}

void VoiceInstaller::finish(bool ok, const QString &error)
{
    if (!m_running)
        return;
    if (!ok)
        cancel(); // also clears the half-written file
    m_running = false;
    if (onFinished)
        onFinished(ok, error);
}

void VoiceInstaller::nextAsset()
{
    if (m_index >= 0)
        m_doneBefore += m_assets.at(m_index).bytes;
    ++m_index;
    if (m_index >= m_assets.size()) {
        const bool usable = locateVoice().ok();
        finish(usable, usable ? QString()
                              : QStringLiteral("The voice downloaded but could not be found "
                                               "afterwards in %1.")
                                    .arg(voiceDirectory()));
        return;
    }
    const VoiceAsset &a = m_assets.at(m_index);
    // A retry after a dropped connection keeps what already arrived. Files
    // only ever reach their final name after the checksum passed.
    const QString installed = voiceDirectory() + QLatin1Char('/') + a.fileName;
    const bool have = a.unpack ? !locateVoice().piper.isEmpty()
                                     && locateVoice().piper.startsWith(voiceDirectory())
                               : QFileInfo(installed).size() == a.bytes;
    if (have) {
        if (onProgress)
            onProgress(m_doneBefore + a.bytes, voiceDownloadBytes());
        nextAsset();
        return;
    }
    m_file = std::make_unique<QFile>(voiceDirectory() + QLatin1Char('/') + a.fileName
                                     + QStringLiteral(".part"));
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finish(false, QStringLiteral("Could not write %1.").arg(m_file->fileName()));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    request(a.url, 0);
}

void VoiceInstaller::request(const QUrl &url, int hop)
{
    if (hop > kMaxHops || url.scheme() != QLatin1String("https")) {
        finish(false, QStringLiteral("The %1 download was redirected somewhere it should "
                                     "not go.")
                          .arg(m_assets.at(m_index).label));
        return;
    }
    QNetworkRequest req(url);
    // Every hop is its own request, so every host meets the firewall.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setRawHeader("Accept", "application/octet-stream");
    m_reply = m_network->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_reply || !m_file)
            return;
        const int status =
            m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200)
            return; // a redirect's body is not the file
        const QByteArray chunk = m_reply->readAll();
        m_hash->addData(chunk);
        m_file->write(chunk);
        const VoiceAsset &a = m_assets.at(m_index);
        if (m_file->pos() > a.bytes) {
            finish(false, QStringLiteral("The %1 download is larger than it should be.")
                              .arg(a.label));
            return;
        }
        if (onProgress)
            onProgress(m_doneBefore + m_file->pos(), voiceDownloadBytes());
    });
    connect(m_reply, &QNetworkReply::finished, this, [this, hop] { onReplyFinished(hop); });
}

void VoiceInstaller::onReplyFinished(int hop)
{
    QNetworkReply *reply = m_reply.data();
    if (!reply || !m_running)
        return;
    m_reply = nullptr;
    reply->deleteLater();
    const VoiceAsset &a = m_assets.at(m_index);
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        const QUrl target =
            reply->url().resolved(QUrl::fromEncoded(reply->rawHeader("Location")));
        request(target, hop + 1);
        return;
    }
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        finish(false, QStringLiteral("The %1 download failed: %2")
                          .arg(a.label, reply->errorString()));
        return;
    }
    // Whatever readyRead has not drained yet.
    const QByteArray rest = reply->readAll();
    m_hash->addData(rest);
    m_file->write(rest);
    verifyAndInstall();
}

void VoiceInstaller::verifyAndInstall()
{
    const VoiceAsset &a = m_assets.at(m_index);
    const qint64 size = m_file->pos();
    m_file->close();
    if (size != a.bytes || m_hash->result().toHex() != a.sha256) {
        finish(false, QStringLiteral("The %1 download did not match its published "
                                     "checksum, so it was thrown away.")
                          .arg(a.label));
        return;
    }
    const QString finalPath = voiceDirectory() + QLatin1Char('/') + a.fileName;
    QFile::remove(finalPath);
    if (!m_file->rename(finalPath)) {
        finish(false, QStringLiteral("Could not move the %1 into place.").arg(a.label));
        return;
    }
    m_file.reset();
    if (!a.unpack) {
        nextAsset();
        return;
    }
    const QString tar = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tar.isEmpty()) {
        finish(false, QStringLiteral("`tar` is needed to unpack the speech engine and "
                                     "was not found."));
        return;
    }
    m_tar = new QProcess(this);
    connect(m_tar, &QProcess::finished, this,
            [this, finalPath](int exitCode, QProcess::ExitStatus exitStatus) {
                QProcess *tarProcess = m_tar.data();
                m_tar = nullptr;
                if (tarProcess)
                    tarProcess->deleteLater();
                QFile::remove(finalPath);
                if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                    finish(false, QStringLiteral("The speech engine could not be unpacked."));
                    return;
                }
                nextAsset();
            });
    connect(m_tar, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false, QStringLiteral("The speech engine could not be unpacked."));
    });
    m_tar->start(tar, {QStringLiteral("-xzf"), finalPath, QStringLiteral("-C"),
                       voiceDirectory(), QStringLiteral("--no-same-owner")});
}

} // namespace forkmesh::announce

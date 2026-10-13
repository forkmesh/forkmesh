#pragma once

// Fetching the announcer's voice (AgentAnnouncer.h).
//
// The voice is Piper and one voice model, neither of which ships with the
// app: together they are ~90 MB and most people will never switch the
// announcer on. They are downloaded the first time it is, with consent, from
// the two places they are published - the Piper release on GitHub and the
// voice on Hugging Face - and every file is checked against a SHA-256 pinned
// here before anything is unpacked or run. A download that does not match is
// deleted; what the redirect chain or a mirror served is not trusted, only the
// bytes.
//
// Redirects are followed by hand, one request per hop through the window's
// own network manager, so every host involved meets the request firewall and
// shows up on the Network page like any other call.

#include <QList>
#include <QObject>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

class QCryptographicHash;
class QFile;
class QNetworkAccessManager;

namespace forkmesh::announce {

struct VoiceAsset {
    QString label;    // "speech engine", "voice"
    QUrl url;
    QString fileName; // inside voiceDirectory()
    QByteArray sha256; // hex
    qint64 bytes = 0;
    bool unpack = false; // a .tar.gz to extract in place, then delete
};

// What this platform needs, or empty where no pinned build of Piper exists
// (the app then only uses a voice that is already installed).
QList<VoiceAsset> voiceAssets();
qint64 voiceDownloadBytes();

class VoiceInstaller : public QObject
{
public:
    explicit VoiceInstaller(QNetworkAccessManager *network, QObject *parent = nullptr);
    ~VoiceInstaller() override;

    // Bytes so far and in total, across every asset.
    std::function<void(qint64 done, qint64 total)> onProgress;
    // Once, at the end. `error` is a sentence for the person, empty on success.
    std::function<void(bool ok, const QString &error)> onFinished;

    void start();
    void cancel();
    bool isRunning() const { return m_running; }

private:
    void nextAsset();
    void request(const QUrl &url, int hop);
    void onReplyFinished(int hop);
    void verifyAndInstall();
    void finish(bool ok, const QString &error);

    QNetworkAccessManager *m_network = nullptr;
    QList<VoiceAsset> m_assets;
    int m_index = -1;
    bool m_running = false;
    qint64 m_doneBefore = 0; // bytes of the assets already finished
    // Guarded: the reply belongs to the network manager, which at shutdown
    // can be torn down before this is.
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QFile> m_file;
    std::unique_ptr<QCryptographicHash> m_hash;
    QPointer<QProcess> m_tar;
};

} // namespace forkmesh::announce

#include "../src/ActionFailureSummary.h"
#include "../src/ActionFile.h"
#include "../src/ActionPushRange.h"
#include "../src/ActionStore.h"
#include "../src/AccountCapability.h"
#include "../src/AccountSessionCache.h"
#include "../src/AgentJail.h"
#include "../src/AgentStore.h"
#include "../src/AgentWorktree.h"
#include "../src/BackgroundActivity.h"
#include "../src/BackoffNetworkAccessManager.h"
#include "../src/ChatHistoryLimits.h"
#include "../src/ChatMessageRetention.h"
#include "../src/ChatLinkText.h"
#include "../src/AppViewMode.h"
#include "../src/ChannelGroups.h"
#include "../src/RailFolders.h"
#include "../src/RailGroups.h"
#include "../src/RailShortcuts.h"
#include "../src/ChatVisitorPresence.h"
#include "../src/ClientErrorReports.h"
#include "../src/GitRefusal.h"
#include "../src/PullDecision.h"
#include "../src/ReleaseManifestTag.h"
#include "../src/PlannedRestarts.h"
#include "../src/CodeSymbols.h"
#include "../src/PingSyncState.h"
#include "../src/CoveCrypto.h"
#include "../src/CoveStore.h"
#include "../src/DirectorySizeScan.h"
#include "../src/DiscussionInboxBackoff.h"
#include "../src/DiscussionStore.h"
#include "../src/EventSigning.h"
#include "../src/ForkMeshIdentity.h"
#include "../src/GracefulStop.h"
#include "../src/GlobalSearchMatch.h"
#include "../src/IssueBurnup.h"
#include "../src/IssueStore.h"
#include "../src/LocalBackupStore.h"
#include "../src/MarkdownRender.h"
#include "../src/MirrorCrypto.h"
#include "../src/PrivateMirrorRuntime.h"
#include "../src/PrivateMirrorStore.h"
#include "../src/McpConnector.h"
#include "../src/MergeQueue.h"
#include "../src/NetworkBackoff.h"
#include "../src/NetworkReplyError.h"
#include "../src/NodeDiagnostics.h"
#include "../src/NoteListEntry.h"
#include "../src/PlatformLogFilter.h"
#include "../src/ProjectStore.h"
#include "../src/PullAiReview.h"
#include "../src/PullReviewModel.h"
#include "../src/PullStore.h"
#include "../src/ReferenceLinks.h"
#include "../src/RepoSecurity.h"
#include "../src/RoomCrypto.h"
#include "../src/RoomSeal.h"
#include "../src/StrictGitReader.h"
#include "../src/SystemStats.h"
#include "../src/UsageLimitCalendar.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <openssl/evp.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <openssl/crypto.h>

#if defined(Q_OS_UNIX)
#include <sys/resource.h> // descriptor-cap tests
#endif

namespace {

int failures = 0;

QStringList *capturedMessages = nullptr;

void captureMessages(QtMsgType, const QMessageLogContext &,
                     const QString &message)
{
    if (capturedMessages)
        *capturedMessages << message;
}

void check(bool condition, const char *what)
{
    if (condition) {
        qInfo("PASS: %s", what);
    } else {
        qCritical("FAIL: %s", what);
        ++failures;
    }
}

class StubNetworkReply : public QNetworkReply
{
public:
    StubNetworkReply(const QNetworkRequest &request,
                     QNetworkAccessManager::Operation op, int status,
                     const QByteArray &body, QObject *parent,
                     const QList<QPair<QByteArray, QByteArray>> &headers = {})
        : QNetworkReply(parent), m_body(body)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
        for (const auto &header : headers)
            setRawHeader(header.first, header.second);
        if (status >= 400)
            setError(QNetworkReply::ContentOperationNotPermittedError,
                     QStringLiteral("HTTP %1").arg(status));
        setOpenMode(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}

    qint64 bytesAvailable() const override
    {
        return (m_body.size() - m_offset) + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        if (m_offset >= m_body.size())
            return -1;
        const qint64 n = qMin(maxSize, qint64(m_body.size() - m_offset));
        memcpy(data, m_body.constData() + m_offset, size_t(n));
        m_offset += n;
        return n;
    }

private:
    QByteArray m_body;
    qint64 m_offset = 0;
};

class TransportFailedStubReply : public StubNetworkReply
{
public:
    TransportFailedStubReply(const QNetworkRequest &request,
                             QNetworkReply::NetworkError error,
                             const QString &text)
        : StubNetworkReply(request, QNetworkAccessManager::GetOperation, 0,
                           QByteArray(), nullptr)
    {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, QVariant());
        setError(error, text);
    }
};

class StubBackoffNetworkAccessManager : public BackoffNetworkAccessManager
{
public:
    QList<int> responseStatuses;
    // Headers for the reply that takes the matching status; a shorter list
    // just means the remaining replies carry none.
    QList<QList<QPair<QByteArray, QByteArray>>> responseHeaders;
    QStringList seenPaths;

protected:
    QNetworkReply *createNetworkRequest(Operation op,
                                        const QNetworkRequest &request,
                                        QIODevice *) override
    {
        seenPaths.append(request.url().path());
        const int status =
            responseStatuses.isEmpty() ? 200 : responseStatuses.takeFirst();
        const QList<QPair<QByteArray, QByteArray>> headers =
            responseHeaders.isEmpty() ? QList<QPair<QByteArray, QByteArray>>()
                                      : responseHeaders.takeFirst();
        const QByteArray body =
            status == 429 ? QByteArray("{\"error\":\"rate_limited\"}") : QByteArray();
        return new StubNetworkReply(request, op, status, body, this, headers);
    }
};

// Models the reply Qt gives us when the network is unavailable: it has an
// error and finishes, but it never opened a response body.
class ClosedOfflineNetworkReply : public QNetworkReply
{
public:
    ClosedOfflineNetworkReply(const QNetworkRequest &request,
                              QNetworkAccessManager::Operation op,
                              QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        setError(QNetworkReply::HostNotFoundError,
                 QStringLiteral("Host offline.test not found"));
        QTimer::singleShot(0, this, [this] {
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}

protected:
    qint64 readData(char *, qint64) override { return -1; }
};

class OfflineBackoffNetworkAccessManager : public BackoffNetworkAccessManager
{
protected:
    QNetworkReply *createNetworkRequest(Operation op,
                                        const QNetworkRequest &request,
                                        QIODevice *) override
    {
        return new ClosedOfflineNetworkReply(request, op, this);
    }
};

// Verify a base64url Ed25519 signature against a base64url raw public key, the
// same encoding ForkMeshIdentity uses, so the test mirrors the worker's
// ed25519_verify rather than trusting the signer's own code path.
bool verifyEd25519(const QString &pubB64Url, const QString &sigB64Url,
                   const QByteArray &message)
{
    const QByteArray rawKey = QByteArray::fromBase64(
        pubB64Url.toLatin1(), QByteArray::Base64UrlEncoding);
    const QByteArray sig = QByteArray::fromBase64(
        sigB64Url.toLatin1(), QByteArray::Base64UrlEncoding);
    if (rawKey.size() != 32 || sig.size() != 64)
        return false;
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(rawKey.constData()), rawKey.size());
    if (!key)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    bool ok = ctx &&
              EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
              EVP_DigestVerify(
                  ctx, reinterpret_cast<const unsigned char *>(sig.constData()),
                  sig.size(),
                  reinterpret_cast<const unsigned char *>(message.constData()),
                  message.size()) == 1;
    if (ctx)
        EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

bool runTestGit(const QString &dir, const QStringList &args,
                QByteArray *output = nullptr,
                const QProcessEnvironment *environment = nullptr)
{
    QProcess process;
    if (environment)
        process.setProcessEnvironment(*environment);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), dir} + args);
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool runTestGitInput(const QString &dir, const QStringList &args,
                     const QByteArray &input)
{
    QProcess process;
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), dir} + args);
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool writeTestFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(contents) == contents.size();
}

bool commitTestTree(const QString &dir, const QString &message,
                    const QString &timestamp, const QString &name,
                    const QString &email,
                    const QString &committerTimestamp = QString())
{
    if (!runTestGit(dir, {QStringLiteral("add"), QStringLiteral("-A")}))
        return false;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_AUTHOR_NAME"), name);
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"), email);
    environment.insert(QStringLiteral("GIT_AUTHOR_DATE"), timestamp);
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"), name);
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"), email);
    environment.insert(QStringLiteral("GIT_COMMITTER_DATE"),
                       committerTimestamp.isEmpty() ? timestamp
                                                    : committerTimestamp);
    return runTestGit(dir,
                      {QStringLiteral("commit"), QStringLiteral("-q"),
                       QStringLiteral("-m"), message},
                      nullptr, &environment);
}

QString testGitHead(const QString &dir)
{
    QByteArray output;
    if (!runTestGit(dir,
                    {QStringLiteral("rev-parse"), QStringLiteral("HEAD")},
                    &output))
        return {};
    return QString::fromUtf8(output).trimmed();
}

QByteArray legacyPullCanonical(const PullRequest &pull)
{
    const QChar nul(QChar::Null);
    const QString content = pull.title + nul + pull.base + nul + pull.head +
                            nul + pull.patch;
    const QByteArray contentHash =
        QCryptographicHash::hash(content.toUtf8(), QCryptographicHash::Sha256)
            .toHex();
    return QByteArrayLiteral("forkmesh-pull-event-v1\n") +
           pull.author.toUtf8() + QByteArrayLiteral("\n") +
           QByteArray::number(pull.ts) + QByteArrayLiteral("\n") +
           contentHash;
}

} // namespace

int main(int argc, char *argv[])
{
    // A small machine-readable mode used by the cross-runtime CTest.  It emits
    // the exact random envelope produced by Qt; the Python side passes that
    // object verbatim through the Worker's real validator.
    if (argc == 2 &&
        QByteArray(argv[1]) == QByteArrayLiteral("--emit-owner-envelope")) {
        const MirrorCrypto::Identity owner =
            MirrorCrypto::generateIdentity();
        const QByteArray plaintext =
            QByteArrayLiteral("agent E2EE cross-runtime contract payload");
        const QJsonObject envelope = MirrorCrypto::sealOwnerPayload(
            plaintext, owner.publicBundle());
        if (!owner.isValid() || envelope.isEmpty())
            return 2;
        const QByteArray output =
            QJsonDocument(QJsonObject{
                              {"envelope", envelope},
                              {"keyId", owner.keyId()},
                              {"plaintext",
                               QString::fromLatin1(plaintext.toBase64())},
                          })
                .toJson(QJsonDocument::Compact);
        if (std::fwrite(output.constData(), 1, size_t(output.size()), stdout) !=
            size_t(output.size()))
            return 3;
        return 0;
    }

    // Seal to an owner device whose keys were generated by another runtime
    // (a phone app's OwnerCrypto), so the cross-runtime CTest can prove
    // that device opens exactly what this desktop produces. The desktop's own
    // wrap entry stays first, as in a real session-snapshot push.
    if (argc == 4 &&
        QByteArray(argv[1]) ==
            QByteArrayLiteral("--seal-owner-envelope-to")) {
        const MirrorCrypto::Identity desktop =
            MirrorCrypto::generateIdentity();
        const QJsonObject companion{
            {"v", 1},
            {"x25519", QString::fromUtf8(argv[2])},
            {"mlkem768", QString::fromUtf8(argv[3])}};
        if (MirrorCrypto::publicKeyId(companion).isEmpty()) {
            std::fprintf(stderr, "companion bundle rejected\n");
            return 2;
        }
        const QByteArray plaintext =
            QJsonDocument(QJsonObject{
                              {"kind", "forkmesh.agent-session"},
                              {"id", 4242},
                              {"status", "running"},
                              {"transcript",
                               "private transcript only owner devices read"},
                          })
                .toJson(QJsonDocument::Compact);
        QString sealError;
        const QJsonObject envelope = MirrorCrypto::sealOwnerPayload(
            plaintext, QList<QJsonObject>{desktop.publicBundle(), companion},
            &sealError);
        if (!desktop.isValid() || envelope.isEmpty()) {
            std::fprintf(stderr, "seal failed: %s\n",
                         sealError.toUtf8().constData());
            return 3;
        }
        const QByteArray output =
            QJsonDocument(QJsonObject{
                              {"envelope", envelope},
                              {"desktopKeyId", desktop.keyId()},
                              {"companionKeyId",
                               MirrorCrypto::publicKeyId(companion)},
                              {"plaintext",
                               QString::fromLatin1(plaintext.toBase64())},
                          })
                .toJson(QJsonDocument::Compact);
        if (std::fwrite(output.constData(), 1, size_t(output.size()), stdout) !=
            size_t(output.size()))
            return 4;
        return 0;
    }

    QTemporaryDir dataDir;
    if (!dataDir.isValid()) {
        qCritical("FAIL: could not create temporary data directory");
        return 1;
    }
    qputenv("XDG_DATA_HOME", dataDir.path().toUtf8());

    QCoreApplication app(argc, argv);
    app.setApplicationName("ForkMeshCryptoTest");
    app.setOrganizationName("ForkMesh");

    {
        const QString linked = ReferenceLinks::linkifyMarkdownReferences(
            "See #154, issue #6, PR #7, pull #8, pull request #9, abcdef1, and "
            "forkmesh://pull/owner/repo/7#open.");
        check(linked.contains("[#154](fm-issue:154)"),
              "plain # references render as issue links");
        check(linked.contains("[issue #6](fm-issue:6)"),
              "explicit issue references render as issue links");
        check(linked.contains("[PR #7](fm-pull:7)"),
              "explicit PR references render as pull links");
        check(linked.contains("[pull #8](fm-pull:8)"),
              "explicit pull references render as pull links");
        check(linked.contains("[pull request #9](fm-pull:9)"),
              "explicit pull request references render as pull links");
        check(linked.contains("[abcdef1](fm-commit:abcdef1)"),
              "pasted commit hashes render as commit links");
        check(linked.contains("[forkmesh://pull/owner/repo/7#open]"
                              "(forkmesh://pull/owner/repo/7#open)"),
              "pasted ForkMesh deep links render as clickable links");
    }

    {
        const QString input =
            "Already [#154](https://example.test/issues/154), `#155`, "
            "https://example.test/#156\n```\n#157 abcdef1\n```";
        const QString linked = ReferenceLinks::linkifyMarkdownReferences(input);
        check(linked == input,
              "reference linker skips existing links, URLs, inline code, and code blocks");
    }

    // README rendering: QTextDocument::setMarkdown cannot render raw HTML, and
    // an unclosed <img> makes it swallow every following plain-text run, so
    // the HTML a README ships is rewritten as markdown before the document
    // sees it.
    {
        const QString flattened = MarkdownRender::withHtmlFlattened(
            "# ForkMesh\n\n<p align=\"center\">\n"
            "  <img src=\"logo.png\" alt=\"ForkMesh\" width=\"120\">\n</p>\n\n"
            "<p align=\"center\"><strong>Own your Git home.</strong></p>\n\n"
            "<p align=\"center\">\n  <a href=\"https://app.forkmesh.com\">App</a> &middot;\n"
            "  <a href=\"https://forkmesh.com/status\">Status</a>\n</p>\n\n"
            "First line<br>second line, &amp; more.\n");
        check(flattened.contains("![ForkMesh](logo.png#w=120)"),
              "an <img> becomes a markdown image carrying its declared width");
        check(flattened.contains("**Own your Git home.**"),
              "<strong> inside an HTML block becomes markdown emphasis");
        check(flattened.contains("[App](https://app.forkmesh.com)") &&
                  flattened.contains("[Status](https://forkmesh.com/status)"),
              "an HTML link row becomes markdown links");
        check(flattened.contains("·") && flattened.contains("& more."),
              "HTML entities are decoded");
        check(flattened.contains("First line  \nsecond line"),
              "<br> becomes a markdown hard line break");
        check(!flattened.contains('<'), "no raw tag survives the rewrite");
        check(flattened.startsWith("# ForkMesh\n\n") &&
                  !flattened.contains("\n\n\n"),
              "the blank lines a dropped wrapper left behind are collapsed");
    }

    {
        const QString source =
            "Text with `<div>` in a code span.\n\n"
            "```html\n<p align=\"center\">kept verbatim</p>\n\n\n```\n\n"
            "Docs at <https://forkmesh.com> stay linkable.\n\n"
            "<!-- reviewers: this note is not content -->\n"
            "Escaped &lt;script&gt; text.\n";
        const QString flattened = MarkdownRender::withHtmlFlattened(source);
        check(flattened.contains("`<div>`"), "inline code spans are left alone");
        check(flattened.contains("```html\n<p align=\"center\">kept verbatim</p>\n\n\n```"),
              "fenced code keeps its HTML and its blank lines");
        check(flattened.contains("<https://forkmesh.com>"), "autolinks survive");
        check(!flattened.contains("reviewers:"), "HTML comments are dropped");
        check(flattened.contains("\\<script>"),
              "text that decoded into a tag is escaped, not re-parsed as HTML");
    }

    // Links inside a rendered markdown file: GitHub heading anchors, and
    // relative links resolved against the file's own directory.
    {
        check(MarkdownRender::headingAnchor("2. Add an independent mirror") ==
                  "2-add-an-independent-mirror",
              "heading anchors drop punctuation and hyphenate spaces");
        check(MarkdownRender::headingAnchor("Clone `again`") == "clone-again",
              "heading anchors ignore inline code ticks");

        const auto doc = MarkdownRender::resolveRepoLink(
            "README.md", "www/docs/product-promise.md#2-add-an-independent-mirror");
        check(doc && doc->path == "www/docs/product-promise.md" &&
                  doc->fragment == "2-add-an-independent-mirror" && !doc->directory,
              "a relative link from the root README names a repository file and anchor");

        const auto up = MarkdownRender::resolveRepoLink("www/docs/intro.md", "../README.md");
        const auto here = MarkdownRender::resolveRepoLink("www/docs/intro.md", "./guide%20one.md");
        const auto folder = MarkdownRender::resolveRepoLink("www/docs/intro.md", "operations/");
        check(up && up->path == "www/README.md", "../ climbs out of the file's directory");
        check(here && here->path == "www/docs/guide one.md",
              "./ stays in the file's directory and percent-escapes decode");
        check(folder && folder->path == "www/docs/operations" && folder->directory,
              "a trailing slash marks a folder link");

        check(!MarkdownRender::resolveRepoLink("README.md", "https://forkmesh.com") &&
                  !MarkdownRender::resolveRepoLink("README.md", "mailto:a@b.test") &&
                  !MarkdownRender::resolveRepoLink("README.md", "#setup") &&
                  !MarkdownRender::resolveRepoLink("README.md", "/forkmesh/forkmesh"),
              "URLs, same-document anchors and absolute paths are not repository links");
    }

    // Ctrl+K search: the rules that decide what the overlay finds, and the
    // parsers for the git output it searches (adhoc #28 widened it from
    // issues/PRs/code to everything this node holds).
    {
        using namespace forkmesh::search;

        QString where;
        Issue issue;
        issue.number = 7;
        issue.title = QStringLiteral("Relay drops a heartbeat");
        IssueEvent opened;
        opened.type = QStringLiteral("open");
        opened.body = QStringLiteral("The node stops advertising after an hour.");
        IssueEvent comment;
        comment.type = QStringLiteral("comment");
        comment.body = QStringLiteral("Reproduced on mirror6 with a cold cache.");
        issue.events = {opened, comment};
        check(matchIssue(issue, QStringLiteral("HEARTBEAT"), &where) &&
                  where == QStringLiteral("title"),
              "search matches an issue title case-insensitively");
        check(matchIssue(issue, QStringLiteral("advertising"), &where) &&
                  !where.startsWith(QStringLiteral("comment: ")),
              "search reports an open-event body hit as the description");
        check(matchIssue(issue, QStringLiteral("mirror6"), &where) &&
                  where.startsWith(QStringLiteral("comment: ")),
              "search flags an issue comment hit as a comment");
        check(!matchIssue(issue, QStringLiteral("solana"), &where),
              "search leaves an unrelated issue alone");

        PullRequest pull;
        pull.title = QStringLiteral("Widen the search overlay");
        pull.description = QStringLiteral("Also covers branches and worktrees.");
        check(matchPull(pull, QStringLiteral("worktrees"), &where) &&
                  where.contains(QStringLiteral("branches and worktrees")),
              "search matches a pull request description and quotes it");

        Discussion discussion;
        discussion.title = QStringLiteral("Roadmap");
        DiscussionEvent reply;
        reply.type = QStringLiteral("comment");
        reply.body = QStringLiteral("What about federated relays?");
        discussion.events = {reply};
        check(matchDiscussion(discussion, QStringLiteral("federated"), &where) &&
                  where.startsWith(QStringLiteral("comment: ")),
              "search matches a discussion comment");

        Project project;
        project.title = QStringLiteral("Phase 3");
        project.body = QStringLiteral("Ship the global search overlay.");
        check(matchProject(project, QStringLiteral("overlay"), &where),
              "search matches a project description");

        Issue searchableIssue = issue;
        searchableIssue.number = 17;
        searchableIssue.title = QStringLiteral("Make every record searchable");
        IssueMilestone searchableMilestone;
        searchableMilestone.title = QStringLiteral("Search parity");
        searchableMilestone.description =
            QStringLiteral("Keep every surface searchable.");
        searchableMilestone.status = QStringLiteral("open");
        PullRequest searchablePull = pull;
        searchablePull.number = 18;
        searchablePull.description =
            QStringLiteral("Makes collaboration records searchable.");
        Discussion searchableDiscussion = discussion;
        searchableDiscussion.number = 19;
        searchableDiscussion.events.first().body =
            QStringLiteral("The thread should be searchable too.");
        Project searchableProject = project;
        searchableProject.number = 20;
        searchableProject.body =
            QStringLiteral("Make project planning searchable.");
        const QVector<RecordHit> recordHits = matchRepositoryRecords(
            QStringLiteral("searchable"), {searchableIssue},
            {searchableMilestone}, {searchablePull}, {searchableDiscussion},
            {searchableProject});
        QStringList recordKinds;
        for (const RecordHit &hit : recordHits)
            recordKinds.append(hit.kind);
        check(recordKinds ==
                  QStringList({QStringLiteral("issue"),
                               QStringLiteral("milestone"),
                               QStringLiteral("pull"),
                               QStringLiteral("discussion"),
                               QStringLiteral("project")}),
              "one repository search projects every collaboration record kind");
        check(recordHits.first().primary.startsWith(QStringLiteral("#17")) &&
                  recordHits.first().status == QStringLiteral("open") &&
                  !recordHits.first().detail.startsWith(QStringLiteral("open")),
              "record search results retain navigation identity and status");

        check(statusBadge(QStringLiteral("issue"), QStringLiteral("open")).label ==
                      QStringLiteral("Open") &&
                  statusBadge(QStringLiteral("issue"), QStringLiteral("closed"))
                          .label == QStringLiteral("Closed") &&
                  statusBadge(QStringLiteral("pull"), QStringLiteral("merged"))
                          .icon == QStringLiteral("git-merge") &&
                  statusBadge(QStringLiteral("code"), QStringLiteral("open"))
                      .label.isEmpty(),
              "search badges name an issue or pull request's open/closed state");

        const QVector<HighlightRun> runs = highlightRuns(
            QStringLiteral("Relay drops a relay heartbeat"),
            QStringLiteral("RELAY"));
        check(runs.size() == 4 && runs.at(0).match &&
                  runs.at(0).text == QStringLiteral("Relay") &&
                  !runs.at(1).match && runs.at(2).match &&
                  runs.at(2).text == QStringLiteral("relay") &&
                  runs.at(3).text == QStringLiteral(" heartbeat"),
              "search highlighting marks every case-insensitive occurrence");
        check(highlightRuns(QStringLiteral("abc"), QString()).size() == 1 &&
                  !highlightRuns(QStringLiteral("abc"), QString()).first().match,
              "an empty query highlights nothing");

        check(!containsFold(QString(), QStringLiteral("x")) &&
                  !containsFold(QStringLiteral("x"), QString()),
              "empty fields and empty queries never match");

        const QString snippet = snippetAround(
            QStringLiteral("alpha\nbeta gamma delta epsilon zeta eta theta iota "
                           "kappa lambda mu nu xi omicron pi rho sigma NEEDLE "
                           "tail"),
            QStringLiteral("needle"));
        check(snippet.contains(QStringLiteral("NEEDLE")) &&
                  snippet.startsWith(QString::fromUtf8("\xE2\x80\xA6")) &&
                  !snippet.contains(QLatin1Char('\n')),
              "snippets are single-line, elided, and centred on the match");
        const QString longSnippet =
            snippetAround(QStringLiteral("needle ") + QString(200, QLatin1Char('x')),
                          QStringLiteral("needle"));
        check(longSnippet.startsWith(QStringLiteral("needle")) &&
                  longSnippet.endsWith(QString::fromUtf8("\xE2\x80\xA6")),
              "snippets mark text cut after the match");

        // `git worktree list --porcelain`: records are separated by blank lines,
        // but a new "worktree " line alone must also close the previous record.
        const QVector<WorktreeRecord> worktrees = parseWorktreePorcelain(
            {QStringLiteral("worktree /home/tester/projects/forkmesh"),
             QStringLiteral("HEAD 1111111111111111111111111111111111111111"),
             QStringLiteral("branch refs/heads/main"),
             QStringLiteral("worktree /tmp/forkmesh-worktrees/issue-0-s28"),
             QStringLiteral("HEAD 2222222222222222222222222222222222222222"),
             QStringLiteral("branch refs/heads/agent/adhoc-28-search"),
             QStringLiteral("worktree /tmp/detached"),
             QStringLiteral("HEAD 3333333333333333333333333333333333333333"),
             QStringLiteral("detached")});
        check(worktrees.size() == 3, "every worktree record is parsed");
        check(worktrees.at(0).branch == QStringLiteral("main") &&
                  worktrees.at(0).path ==
                      QStringLiteral("/home/tester/projects/forkmesh"),
              "worktree branches lose their refs/heads/ prefix");
        check(worktrees.at(1).branch ==
                  QStringLiteral("agent/adhoc-28-search"),
              "an agent worktree is parsed without a blank separator line");
        check(worktrees.at(2).detached && worktrees.at(2).branch.isEmpty(),
              "a detached worktree is flagged and carries no branch");
        check(parseWorktreePorcelain({QStringLiteral("branch refs/heads/main")})
                  .isEmpty(),
              "an attribute with no worktree line is ignored");

        // `git grep -n <rev>` prefixes each row with "<rev>:".
        const CodeRow row = parseGrepRow(
            QStringLiteral("main:desktop/src/MainWindowSearch.cpp:42:  const "
                           "int gen = ++m_searchGen;"),
            QStringLiteral("main"));
        check(row.valid && row.line == 42 &&
                  row.path ==
                      QStringLiteral("desktop/src/MainWindowSearch.cpp") &&
                  row.text.startsWith(QStringLiteral("const int gen")),
              "a git grep row is split into path, line and trimmed text");
        check(!parseGrepRow(QStringLiteral("no line number here"),
                            QStringLiteral("main"))
                   .valid,
              "a malformed git grep row is dropped");

        check(categoryIndexOf(QStringLiteral("repo")) <
                      categoryIndexOf(QStringLiteral("branch")) &&
                  categoryIndexOf(QStringLiteral("branch")) <
                      categoryIndexOf(QStringLiteral("code")),
              "search groups run from identity matches down to content matches");
        check(categoryIndexOf(QStringLiteral("worktree")) < categoryCount() &&
                  categoryIndexOf(QStringLiteral("agent")) < categoryCount() &&
                  categoryIndexOf(QStringLiteral("log")) < categoryCount(),
              "worktrees, agent sessions, and logs are searchable categories");
        check(categoryIndexOf(QStringLiteral("nonsense")) == categoryCount(),
              "an unknown hit kind sorts last instead of vanishing");
        check(categoryIndexOf(QStringLiteral("worktree-code")) < categoryCount() &&
                  categoryIndexOf(QStringLiteral("code")) <
                      categoryIndexOf(QStringLiteral("worktree-code")),
              "code found only in a worktree is its own group after the tree's code");
        check(categoryIndexOf(QStringLiteral("project-file")) < categoryCount() &&
                  categoryIndexOf(QStringLiteral("code")) <
                      categoryIndexOf(QStringLiteral("project-file")),
              "files on disk in the project folder are their own group after the tree's code");

        // Branch, worktree and commit search cover every branch except the pull
        // ledger's, local or remote-tracking.
        check(isLedgerBranch(QStringLiteral("forkmesh/pulls")) &&
                  isLedgerBranch(QStringLiteral("forkmesh/pull/12")) &&
                  isLedgerBranch(QStringLiteral("origin/forkmesh/pulls")) &&
                  isLedgerBranch(QStringLiteral("refs/heads/forkmesh/pull/3")),
              "the pull ledger branches are recognised in every spelling");
        check(!isLedgerBranch(QStringLiteral("agent/adhoc-2144-search")) &&
                  !isLedgerBranch(QStringLiteral("forkmesh/pulls-ui")) &&
                  !isLedgerBranch(QStringLiteral("origin/main")) &&
                  !isLedgerBranch(QString()),
              "ordinary and look-alike branches stay searchable");
        const QStringList revisions =
            allBranchRevisionArgs(QStringLiteral("main"));
        check(revisions.contains(QStringLiteral("--branches")) &&
                  revisions.contains(QStringLiteral("--remotes")) &&
                  revisions.contains(QStringLiteral("--source")) &&
                  revisions.last() == QStringLiteral("main") &&
                  revisions.indexOf(QStringLiteral("--exclude=forkmesh/pulls")) <
                      revisions.indexOf(QStringLiteral("--branches")),
              "commit search walks every branch, excluding the ledger before "
              "--branches consumes the excludes");
        check(!allBranchRevisionArgs(QString()).contains(QString()),
              "no browsed ref adds no empty revision argument");
    }

    // The Files editor traces a symbol through the repository with `git grep`
    // plus these heuristics (adhoc #1607): which lines define a name, which
    // call it, which function a call sits inside (the backwards trace) and what
    // a function body calls (the forwards one).
    {
        using namespace forkmesh::code;

        // Definitions, across the shapes the languages in this repo use.
        check(definitionName(QStringLiteral(
                  "void MainWindow::openRepoFile(const QString &path)")) ==
                  QStringLiteral("openRepoFile"),
              "a C++ member definition names its function");
        check(definitionName(QStringLiteral("    def load_tree(self, path):")) ==
                  QStringLiteral("load_tree"),
              "a Python def names its function");
        check(definitionName(QStringLiteral(
                  "export async function fetchThing(a) {")) ==
                  QStringLiteral("fetchThing"),
              "an exported async JS function is a definition");
        check(definitionName(QStringLiteral("const handler = (event) => {")) ==
                  QStringLiteral("handler"),
              "an arrow function bound to a name is a definition");
        check(definitionName(QStringLiteral(
                  "func (r *Repo) Sync(ctx context.Context) error {")) ==
                  QStringLiteral("Sync"),
              "a Go method is named past its receiver");
        check(definitionName(QStringLiteral("pub fn parse_row(line: &str) {")) ==
                  QStringLiteral("parse_row"),
              "a public Rust fn is a definition");
        check(definitionName(QStringLiteral("    render() {")) ==
                  QStringLiteral("render"),
              "a method shorthand with no return type is a definition");

        // Statements that merely look like signatures are not definitions —
        // this is what keeps a trace from listing every `if` in the tree.
        check(definitionName(QStringLiteral("if (ready) {")).isEmpty(),
              "a control-flow statement is not a definition");
        check(definitionName(QStringLiteral("    return buildTree(path);"))
                  .isEmpty(),
              "a returned call is not a definition");
        check(definitionName(QStringLiteral(
                  "    connect(m_tree, &QTreeWidget::itemClicked, this, [this] {"))
                  .isEmpty(),
              "a call that opens a trailing lambda is not a definition");
        check(definitionName(QStringLiteral("void MainWindow::foo();")).isEmpty(),
              "a forward declaration has no body to trace");
        check(definitionName(QStringLiteral("// void notReal(int x) {")).isEmpty(),
              "a commented-out signature is not a definition");

        // Read on its own, the continuation line of a wrapped expression has
        // the shape of a signature. These are the three that turn up most in
        // this codebase's own 80-column style, and each is ruled out by
        // something a real return type can never do.
        check(definitionName(QStringLiteral(
                  "        ref, QStringLiteral(\"--\"), QStringLiteral(\":!pulls\"),"))
                  .isEmpty(),
              "a wrapped argument list is not a definition");
        check(definitionName(QStringLiteral("        : formatByteSize(info.size()),"))
                  .isEmpty(),
              "the far half of a wrapped ternary is not a definition");
        check(definitionName(QStringLiteral("    : QStackedWidget(parent)")).isEmpty(),
              "a constructor's initialiser list is not a definition");
        check(definitionName(QStringLiteral(
                  "            QString::fromUtf8(\"%1\").arg(name)")).isEmpty(),
              "a wrapped static call with no return type is not a definition");
        // …while the shapes those rules run closest to still resolve. A
        // one-word return type must not read as a bare scope chain, and a
        // constructor — the one thing a scope chain alone can introduce, since
        // its name repeats the scope — has no return type by definition, at
        // whatever indent the namespace it is written in puts it.
        check(definitionName(QStringLiteral("    void paintEvent(QPaintEvent *) override")) ==
                  QStringLiteral("paintEvent"),
              "an indented override with a one-word return type is a definition");
        check(definitionName(QStringLiteral("    QSize sizeHint() const override")) ==
                  QStringLiteral("sizeHint"),
              "an indented const member with a one-word return type is a definition");
        check(definitionName(QStringLiteral("MainWindow::MainWindow(QWidget *parent)")) ==
                  QStringLiteral("MainWindow"),
              "a constructor at file scope is a definition");
        check(definitionName(QStringLiteral("MainWindow::~MainWindow()")) ==
                  QStringLiteral("MainWindow"),
              "a destructor at file scope is a definition");
        check(definitionName(QStringLiteral("    FileTabBar::FileTabBar(QWidget *parent)")) ==
                  QStringLiteral("FileTabBar"),
              "a constructor indented inside a namespace is a definition");
        check(definitionName(QStringLiteral("    Outer::Inner::Inner(int seed)")) ==
                  QStringLiteral("Inner"),
              "a nested constructor is named by its innermost scope");

        check(identifierAt(QStringLiteral("  openRepoFile(path);"), 5) ==
                  QStringLiteral("openRepoFile"),
              "the identifier under the caret is the symbol to trace");
        check(identifierAt(QStringLiteral("  openRepoFile(path);"), 14) ==
                  QStringLiteral("openRepoFile"),
              "a caret just past a name still means that name");
        check(identifierAt(QStringLiteral("  openRepoFile(path);"), 1).isEmpty(),
              "a caret on whitespace names nothing");
        check(identifierAt(QStringLiteral("x = 42;"), 5).isEmpty(),
              "a number is not a symbol");

        check(callsSymbol(QStringLiteral("    beta(1);"), QStringLiteral("beta")),
              "a call site is recognised");
        check(!callsSymbol(QStringLiteral("    alphabeta(1);"),
                           QStringLiteral("beta")),
              "a longer name that merely contains the symbol is not a call");
        check(!callsSymbol(QStringLiteral("void beta(int x)"),
                           QStringLiteral("beta")),
              "the definition itself is not one of its own call sites");

        const QStringList cpp = QStringLiteral(
                                    "void MainWindow::alpha()\n"
                                    "{\n"
                                    "    beta(1);\n"
                                    "    gamma();\n"
                                    "    // beta(2) in a comment\n"
                                    "}\n"
                                    "\n"
                                    "void MainWindow::delta()\n"
                                    "{\n"
                                    "    alpha();\n"
                                    "}\n")
                                    .split(QLatin1Char('\n'));
        const Enclosing inAlpha = enclosingDefinition(cpp, 3);
        check(inAlpha.name == QStringLiteral("alpha") && inAlpha.line == 1,
              "a call is traced back to the function it sits inside");
        check(enclosingDefinition(cpp, 10).name == QStringLiteral("delta"),
              "the walk stops at the enclosing definition, not the one before it");
        check(definitionBodyEnd(cpp, 1) == 6,
              "a brace-language body ends at its closing brace");
        QStringList outgoing;
        for (const CallSite &call : callsInBody(cpp, 1))
            outgoing.append(call.name);
        check(outgoing == QStringList({QStringLiteral("beta"),
                                       QStringLiteral("gamma")}),
              "the forwards trace lists what the body calls, skipping comments");

        const QStringList py = QStringLiteral(
                                   "class Store:\n"
                                   "    def load(self, path):\n"
                                   "        data = read_json(path)\n"
                                   "        return normalise(data)\n"
                                   "\n"
                                   "    def save(self):\n"
                                   "        pass\n")
                                   .split(QLatin1Char('\n'));
        check(enclosingDefinition(py, 3).name == QStringLiteral("load"),
              "an indentation language traces back to the enclosing def");
        check(definitionBodyEnd(py, 2) == 4,
              "an indented body ends where the indentation does");
        QStringList pyCalls;
        for (const CallSite &call : callsInBody(py, 2))
            pyCalls.append(call.name);
        check(pyCalls == QStringList({QStringLiteral("read_json"),
                                      QStringLiteral("normalise")}),
              "the forwards trace works the same in an indentation language");
    }

    {
        const qint64 resetMs = QDateTime::fromString(
                                   QStringLiteral("2026-08-01T12:30:00Z"),
                                   Qt::ISODate)
                                   .toMSecsSinceEpoch();
        const QString calendar = UsageLimitCalendar::eventText(
            QStringLiteral("claude"), QStringLiteral("5h"),
            QStringLiteral("Claude Code"), QStringLiteral("5-hour"), resetMs,
            resetMs - 60 * 1000);
        check(calendar.contains(QStringLiteral("BEGIN:VCALENDAR\r\n")) &&
                  calendar.contains(QStringLiteral("METHOD:PUBLISH\r\n")) &&
                  calendar.contains(QStringLiteral(
                      "UID:forkmesh-usage-claude-5h-1785587400000@local\r\n")) &&
                  calendar.contains(QStringLiteral("DTSTART:20260801T123000Z\r\n")) &&
                  calendar.contains(QStringLiteral("TRIGGER:PT0M\r\n")) &&
                  calendar.contains(QStringLiteral(
                      "SUMMARY:ForkMesh: Claude Code usage is ready\r\n")),
              "usage-limit calendar export is a timed iCalendar event with an alarm");
        check(UsageLimitCalendar::eventText(
                  QStringLiteral("claude"), QStringLiteral("5h"),
                  QStringLiteral("Claude Code"), QStringLiteral("5-hour"), 0)
                  .isEmpty(),
              "usage-limit calendar export rejects a missing reset time");

        const QString path = UsageLimitCalendar::writeEvent(
            QStringLiteral("codex"), QStringLiteral("weekly"),
            QStringLiteral("Codex"), QStringLiteral("weekly"), resetMs);
        QFile file(path);
        check(!path.isEmpty() && file.open(QIODevice::ReadOnly) &&
                  QString::fromUtf8(file.readAll()).contains(
                      QStringLiteral("ForkMesh: Codex usage is ready")),
              "usage-limit calendar event is written atomically under app data");
    }

    {
        const qint64 resetMs = QDateTime::fromString(
                                   QStringLiteral("2026-08-01T12:30:00Z"),
                                   Qt::ISODate)
                                   .toMSecsSinceEpoch();
        const QString calendar = UsageLimitCalendar::eventText(
            QStringLiteral("claude"), QStringLiteral("5h"),
            QStringLiteral("Claude Code"), QStringLiteral("5-hour"), resetMs,
            resetMs - 60 * 1000);
        check(calendar.contains(QStringLiteral("BEGIN:VCALENDAR\r\n")) &&
                  calendar.contains(QStringLiteral("METHOD:PUBLISH\r\n")) &&
                  calendar.contains(QStringLiteral(
                      "UID:forkmesh-usage-claude-5h-1785587400000@local\r\n")) &&
                  calendar.contains(QStringLiteral("DTSTART:20260801T123000Z\r\n")) &&
                  calendar.contains(QStringLiteral("TRIGGER:PT0M\r\n")) &&
                  calendar.contains(QStringLiteral(
                      "SUMMARY:ForkMesh: Claude Code usage is ready\r\n")),
              "usage-limit calendar export is a timed iCalendar event with an alarm");
        check(UsageLimitCalendar::eventText(
                  QStringLiteral("claude"), QStringLiteral("5h"),
                  QStringLiteral("Claude Code"), QStringLiteral("5-hour"), 0)
                  .isEmpty(),
              "usage-limit calendar export rejects a missing reset time");

        const QString path = UsageLimitCalendar::writeEvent(
            QStringLiteral("codex"), QStringLiteral("weekly"),
            QStringLiteral("Codex"), QStringLiteral("weekly"), resetMs);
        QFile file(path);
        check(!path.isEmpty() && file.open(QIODevice::ReadOnly) &&
                  QString::fromUtf8(file.readAll()).contains(
                      QStringLiteral("ForkMesh: Codex usage is ready")),
              "usage-limit calendar event is written atomically under app data");
    }

    {
        // Notes sidebar rows. The list used to show only storage + version,
        // so a note that was never published and one that is live on the web
        // read identically — the report was "my published note did not show
        // up on the web and did not show in the list of notes locally".
        const QJsonObject draft{{"title", "Draft"}, {"version", 3}};
        check(NoteListEntry::lines(draft, {}, "local", false) ==
                  QStringList{"Draft", "Storage: Local only · Visibility: On this computer · Rev 3"},
              "a local-only note names its storage and stays off the web");

        // Asked for the cloud but never got there: the row says so instead of
        // looking exactly like a saved-but-private note.
        check(NoteListEntry::lines(draft, {}, "both", false).at(1) ==
                  QStringLiteral("Storage: Local + cloud · Visibility: Not published yet · Rev 3"),
              "a note with no cloud copy is labelled as not published yet");

        const QJsonObject published{
            {"title", "Launch plan"}, {"version", 4}, {"visibility", "public"},
            {"views", 12}, {"readers", 4},
            {"shares", QJsonArray{
                QJsonObject{{"name", "bob"}, {"role", "editor"}},
                QJsonObject{{"name", "acme"}, {"role", "viewer"}}}}};
        const QStringList row =
            NoteListEntry::lines(published, published, "both", true);
        check(row.at(1) == QStringLiteral(
                  "Storage: Local + cloud · Visibility: Public · Views: 12 views from 4 readers · Rev 4"),
              "a published note shows its status and read count");
        check(row.at(2) ==
                  QStringLiteral("Shared with bob (editor), acme (viewer)"),
              "the row names every collaborator the note is shared with");

        // Shared but not published, and read exactly once by one reader.
        const QJsonObject shared{
            {"title", "Spec"}, {"version", 1}, {"visibility", "private"},
            {"views", 1}, {"readers", 1},
            {"shares", QJsonArray{
                QJsonObject{{"name", "bob"}, {"role", "viewer"}}}}};
        check(NoteListEntry::lines(shared, shared, "cloud", true) ==
                  QStringList{"Spec", "Storage: Cloud · Visibility: Shared · Views: 1 view · Rev 1",
                              "Shared with bob (viewer)"},
              "a shared private note reads as Shared, not Public");

        // Signed out: the cloud copy is unreadable, but the mirrored
        // visibility on the local record still tells the truth.
        const QJsonObject mirrored{{"title", "Launch plan"}, {"version", 4},
                                   {"visibility", "public"}};
        check(NoteListEntry::lines(mirrored, {}, "both", true) ==
                  QStringList{"Launch plan", "Storage: Local + cloud · Visibility: Public · Rev 4",
                              "Not shared with anyone"},
              "a published note still reads as Public without the listing");

        // An unread draft is not labelled "0 views".
        check(NoteListEntry::viewsLabel(QJsonObject{{"views", 0}}).isEmpty(),
              "a note nobody has read carries no view count");
    }

    {
        // Saving a note. The confirmation used to be a muted label in the page
        // heading that said "Saved locally." whether or not the cloud half of a
        // "Local + cloud" save had landed — so the alert now names every
        // destination the write actually reached, and the one it missed.
        check(NoteListEntry::savedAlert("Draft", "local", true, false) ==
                  QStringLiteral("Saved “Draft” on this computer."),
              "a local-only save says where the note landed");

        const QJsonObject stored{
            {"visibility", "private"},
            {"shares", QJsonArray{
                QJsonObject{{"name", "bob"}, {"role", "editor"}}}}};
        check(NoteListEntry::savedAlert("Spec", "both", true, true, stored) ==
                  QStringLiteral("Saved “Spec” on this computer and in the "
                                 "cloud. Shared with bob (editor)."),
              "a local + cloud save names both copies and who can read it");

        const QJsonObject published{{"visibility", "public"}};
        check(NoteListEntry::savedAlert("Launch plan", "cloud", false, true,
                                        published) ==
                  QStringLiteral("Saved “Launch plan” in the cloud. "
                                 "Published on the web."),
              "publishing a note is called out in the alert");

        // The cloud half failed: the note is on disk, but claiming a saved
        // cloud copy is exactly the lie this alert exists to prevent.
        check(NoteListEntry::savedAlert("Spec", "both", true, false, {},
                                        QStringLiteral("HTTP 409")) ==
                  QStringLiteral("Saved “Spec” on this computer only — the "
                                 "cloud copy failed: HTTP 409."),
              "a failed cloud copy is named, not papered over");

        // Cloud-only mode still falls back to disk when the relay refuses, and
        // an error that is already a sentence is not given a second full stop.
        check(NoteListEntry::savedAlert(
                  "Spec", "cloud", true, false, {},
                  QStringLiteral("Cloud notes need a password sign-in.")) ==
                  QStringLiteral("Saved “Spec” on this computer only — the "
                                 "cloud copy failed: Cloud notes need a "
                                 "password sign-in."),
              "a cloud-only save that fell back to disk says so once");

        check(NoteListEntry::savedAlert("Spec", "both", false, true, {}) ==
                  QStringLiteral("Saved “Spec” in the cloud only — the copy on "
                                 "this computer could not be written."),
              "a failed local write is named even when the cloud copy landed");

        check(NoteListEntry::savedAlert("Spec", "local", false, false) ==
                  QStringLiteral("Could not save “Spec” on this computer."),
              "a save that landed nowhere never reads as saved");

        check(NoteListEntry::savedAlert("", "cloud", false, false, {},
                                        QStringLiteral("HTTP 500")) ==
                  QStringLiteral("Could not save “Untitled note” anywhere: "
                                 "HTTP 500."),
              "an unnamed note is still named in its alert");
    }

    {
        // Converting a note into a task or an issue. The work item inherits the
        // note's own title when it has one, so the row in the tracker matches
        // the row in the notes list.
        check(NoteListEntry::conversionTitle(
                  QStringLiteral("Ship the installer"),
                  QStringLiteral("# Notes\n\nSigning is the blocker."), 160) ==
                  QStringLiteral("Ship the installer"),
              "a titled note keeps its title through the conversion");

        // An untitled note falls back to its first line of prose, with the
        // Markdown peeled off: "## Ship the installer" is not a title.
        check(NoteListEntry::conversionTitle(
                  QStringLiteral("Untitled note"),
                  QStringLiteral("\n## Ship the installer\n\nSigning first."),
                  160) == QStringLiteral("Ship the installer"),
              "an untitled note takes its first heading without the hashes");

        check(NoteListEntry::conversionTitle(
                  QString(), QStringLiteral("- [ ] Rotate the relay key\n"),
                  160) == QStringLiteral("Rotate the relay key"),
              "a checklist line loses its bullet and its box");

        // A rule is syntax with no words in it, so the title comes from the
        // line after it rather than reading as empty.
        check(NoteListEntry::conversionTitle(
                  QString(), QStringLiteral("---\nRotate the relay key\n"),
                  160) == QStringLiteral("Rotate the relay key"),
              "a leading horizontal rule is skipped, not turned into a title");

        check(NoteListEntry::conversionTitle(QString(), QString(), 160) ==
                  QStringLiteral("Untitled note"),
              "an empty note still reaches the composer with a name");

        // The relay caps a task title at 160 characters, so a long first line
        // is elided rather than rejected on arrival.
        const QString longTitle =
            NoteListEntry::conversionTitle(QString::fromUtf8("x").repeated(400),
                                           QString(), 160);
        check(longTitle.size() == 160 &&
                  longTitle.endsWith(QStringLiteral("…")),
              "an over-long title is elided to the field it is going into");

        // The body carries the note verbatim plus one line saying where it
        // came from — converting copies the note, it does not move it.
        check(NoteListEntry::conversionDetails(
                  QStringLiteral("Launch plan"),
                  QStringLiteral("Signing is the blocker.\n")) ==
                  QStringLiteral("Signing is the blocker.\n\n---\nConverted "
                                 "from the note “Launch plan”."),
              "the converted body keeps the note and names its source");

        check(NoteListEntry::conversionDetails(
                  QStringLiteral("Launch plan"), QString(),
                  QStringLiteral("https://forkmesh.com/notes/abc")) ==
                  QStringLiteral("Converted from the note “Launch plan” "
                                 "(published at https://forkmesh.com/notes/abc)."),
              "a published note hands over its public address");

        check(NoteListEntry::conversionDetails(QString(), QString()) ==
                  QStringLiteral("Converted from the note “Untitled note”."),
              "an unnamed note is still named in the body it hands over");
    }

    {
        namespace retention = forkmesh::chatretention;
        check(retention::kDefaultMs == qint64(7) * 24 * 60 * 60 * 1000,
              "chat messages expire after seven days by default");
        check(retention::clampMs(0) == retention::kDefaultMs &&
                  retention::clampMs(-5) == retention::kDefaultMs,
              "an unset chat retention falls back to seven days");
        check(retention::clampMs(30 * retention::kDayMs) ==
                  retention::kDefaultMs,
              "chat retention cannot be longer than seven days");
        check(retention::clampMs(1000) == retention::kMinMs,
              "chat retention has a one-hour floor");
        check(retention::clampMs(retention::kDayMs) == retention::kDayMs,
              "a shorter chat retention is kept as chosen");
        const QList<retention::Choice> choices = retention::choices();
        bool ordered = !choices.isEmpty() &&
                       choices.first().ms == retention::kDefaultMs;
        for (int i = 1; i < choices.size(); ++i)
            ordered = ordered && choices.at(i).ms < choices.at(i - 1).ms &&
                      retention::clampMs(choices.at(i).ms) == choices.at(i).ms;
        check(ordered, "chat retention choices start at seven days and only "
                       "get shorter");

        const retention::Preferences parsed = retention::fromJson(QJsonObject{
            {"defaultMs", retention::kDayMs},
            {"chats", QJsonObject{{"#general", retention::kHourMs},
                                  {"channel:abc", 30 * retention::kDayMs},
                                  {"@peer", "soon"},
                                  {"#bad\nkey", retention::kHourMs}}},
            {"updatedAt", 42}});
        check(parsed.defaultMs == retention::kDayMs &&
                  parsed.effectiveMs("#general") == retention::kHourMs &&
                  parsed.effectiveMs("channel:abc") == retention::kDefaultMs &&
                  parsed.effectiveMs("@peer") == retention::kDayMs &&
                  !parsed.chats.contains("#bad\nkey") && parsed.updatedAt == 42,
              "synced chat retention reads the default, clamps overrides and "
              "drops bad entries");
        check(retention::fromJson(QJsonObject()).defaultMs == retention::kDefaultMs,
              "an account with no chat retention keeps messages seven days");
        check(retention::fromJson(retention::toJson(parsed)) == parsed,
              "chat retention round-trips through its cached JSON");
        const retention::Preferences cleared =
            retention::withChat(parsed, "#general", 0);
        check(!cleared.chats.contains("#general") &&
                  cleared.effectiveMs("#general") == retention::kDayMs,
              "clearing a chat's time falls back to the default");
        check(retention::chatBody("#general", 0)
                      .value("retentionMs").isNull() &&
                  retention::chatBody("#general", retention::kHourMs)
                          .value("retentionMs").toInteger() == retention::kHourMs &&
                  retention::defaultBody(0).value("defaultMs").isNull(),
              "chat retention requests send null to clear");
        check(retention::officeKey("abc") == QStringLiteral("channel:abc") &&
                  retention::label(retention::kHourMs) == QStringLiteral("1 hour"),
              "office chats share the web's channel key");
    }

    {
        // Bounded chat history (issue #428): a mirror node must not retain
        // unlimited chat frames — or huge file payloads — in RAM.
        QList<QJsonObject> history;
        QStringList evicted;
        const int overflow = ChatHistoryLimits::kMaxEntriesPerChannel + 25;
        for (int i = 0; i < overflow; ++i) {
            QJsonObject msg{{"id", QStringLiteral("m%1").arg(i)},
                            {"channel", QStringLiteral("#general")},
                            {"text", QStringLiteral("hello %1").arg(i)}};
            evicted += ChatHistoryLimits::appendBounded(history, msg);
        }
        check(history.size() == ChatHistoryLimits::kMaxEntriesPerChannel,
              "chat history keeps at most kMaxEntriesPerChannel entries");
        check(evicted.size() == 25 && evicted.first() == "m0",
              "chat history evicts oldest entries first and reports their ids");
        check(history.last().value("id").toString() ==
                  QStringLiteral("m%1").arg(overflow - 1),
              "chat history keeps the newest entry after eviction");

        // A handful of large file messages must not pin unbounded RAM: the
        // char budget evicts older entries even when the count cap is far off.
        QList<QJsonObject> fileHistory;
        const QString bigPayload(ChatHistoryLimits::kMaxCharsPerChannel / 2, 'A');
        QStringList fileEvicted;
        for (int i = 0; i < 6; ++i) {
            QJsonObject msg{{"id", QStringLiteral("f%1").arg(i)},
                            {"channel", QStringLiteral("#general")},
                            {"fileName", QStringLiteral("blob.bin")},
                            {"file", bigPayload}};
            fileEvicted += ChatHistoryLimits::appendBounded(fileHistory, msg);
        }
        qsizetype totalChars = 0;
        for (const QJsonObject &entry : std::as_const(fileHistory))
            totalChars += ChatHistoryLimits::entryCost(entry);
        check(totalChars <= ChatHistoryLimits::kMaxCharsPerChannel,
              "chat history stays within the per-channel char budget");
        check(!fileEvicted.isEmpty() && fileEvicted.first() == "f0",
              "char budget evicts oldest file messages first");

        // One message bigger than the whole budget is kept, but without its
        // file body, so a single huge transfer can't pin the budget's worth.
        QList<QJsonObject> oversized;
        QJsonObject huge{{"id", QStringLiteral("huge")},
                         {"channel", QStringLiteral("#general")},
                         {"fileName", QStringLiteral("huge.bin")},
                         {"file", QString(ChatHistoryLimits::kMaxCharsPerChannel + 1,
                                          'B')}};
        ChatHistoryLimits::appendBounded(oversized, huge);
        check(oversized.size() == 1 && !oversized.first().contains("file") &&
                  oversized.first().value("fileName").toString() == "huge.bin",
              "oversized file payloads are stripped from stored history");
    }

    {
        const QString deployLog = QString::fromUtf8(
            "==> \xF0\x9F\x9A\x80 Workflow: Deploy nodes (.forkmesh/deploy-nodes.yml)\n"
            "==> \xF0\x9F\x94\xA7 Deploy main to n1-n3 through the node deploy agent\n"
            "02:53:17 $ python3 tools/next_deploy.py migrate --execute\n"
            "02:53:33 next_deploy: SSH tunnel to n1 is up, but no. Fix: allow loopback TCP\n"
            "02:53:33 notify: 2 Ping(s)\n"
            "FAILED: next_deploy migrate exited 1: SSH tunnel to n1 is up, but no. "
            "Fix: allow loopback TCP\n"
            "\n"
            "==> \xE2\x9D\x8C FAILED: Step failed with exit code 1.\n");
        const ActionFailureSummary::Summary deploy =
            ActionFailureSummary::summarize(deployLog);
        check(deploy.step ==
                  QStringLiteral("Deploy main to n1-n3 through the node deploy agent"),
              "failed action ping names the failing step");
        check(deploy.cause == QStringLiteral(
                  "next_deploy migrate exited 1: SSH tunnel to n1 is up, but no."),
              "failed action ping carries the step's own error line");
        check(deploy.fix == QStringLiteral("allow loopback TCP"),
              "failed action ping splits out the suggested fix");
        check(ActionFailureSummary::pingBody(QStringLiteral("Deploy nodes"), deployLog) ==
                  QStringLiteral("Deploy nodes\nStep: Deploy main to n1-n3 through the "
                                 "node deploy agent\nCause: next_deploy migrate exited 1: "
                                 "SSH tunnel to n1 is up, but no.\nFix: allow loopback TCP"),
              "failed action ping body lists step, cause and fix");

        const QString testLog = QString::fromUtf8(
            "==> \xF0\x9F\x94\xA7 Build\n"
            "==> \xF0\x9F\x94\xA7 Test\n"
            "\x1B[31mFAILED tests/test_a.py::test_b - AssertionError\x1B[0m\n"
            "===== 1 failed, 9 passed in 2.0s =====\n"
            "==> \xE2\x9D\x8C FAILED: Step failed with exit code 1.\n");
        const ActionFailureSummary::Summary tests =
            ActionFailureSummary::summarize(testLog);
        check(tests.step == QStringLiteral("Test") &&
                  tests.cause == QStringLiteral(
                      "FAILED tests/test_a.py::test_b - AssertionError") &&
                  tests.fix.isEmpty(),
              "failed action ping picks the last error line of the last step");

        const QString ctestLog = QString::fromUtf8(
            "==> \xF0\x9F\x94\xA7 Critical Qt client contracts\n"
            "PASS: one file's row ends before the next file's row begins\n"
            "FAIL: a short rolling diff window fills the viewport (settled=1)\n"
            "93% tests passed, 1 tests failed out of 15\n"
            "The following tests FAILED:\n"
            "\t118 - forkmesh-diff-stream-window-tests (Failed)        critical\n"
            "Errors while running CTest\n"
            "[critical-tests] qt: 8.94s\n"
            "\n"
            "==> \xE2\x9D\x8C FAILED: Step failed with exit code 8.\n");
        check(ActionFailureSummary::summarize(ctestLog).cause ==
                  QStringLiteral("FAIL: a short rolling diff window fills the "
                                 "viewport (settled=1)"),
              "failed action ping names the window test check that failed, not "
              "the runner's timing line");

        const ActionFailureSummary::Summary quiet = ActionFailureSummary::summarize(
            QString::fromUtf8("==> \xF0\x9F\x94\xA7 Lint\nchecking 3 files\n"
                              "==> \xE2\x9D\x8C FAILED: Step failed with exit code 2.\n"));
        check(quiet.cause == QStringLiteral("checking 3 files"),
              "failed action ping falls back to the step's last output line");
        const ActionFailureSummary::Summary silent = ActionFailureSummary::summarize(
            QString::fromUtf8("==> \xF0\x9F\x94\xA7 Lint\n"
                              "==> \xE2\x9D\x8C FAILED: Step timed out.\n"));
        check(silent.cause == QStringLiteral("Step timed out."),
              "failed action ping falls back to the runner's verdict");
        check(ActionFailureSummary::pingBody(QStringLiteral("CI"), QString()) ==
                  QStringLiteral("CI"),
              "failed action ping without a log keeps the heading alone");
        const ActionFailureSummary::Summary longLine = ActionFailureSummary::summarize(
            QStringLiteral("Error: ") + QString(900, QLatin1Char('x')));
        check(longLine.cause.size() == ActionFailureSummary::kMaxCauseChars,
              "failed action ping clips a long cause");
    }

    {
        // Links inside chat text (issue #539): a URL typed into a sentence is
        // clickable where it was written, instead of having to be sent as a
        // message of its own.
        auto knownMember = [](const QString &handle) {
            return handle.toLower() == QLatin1String("jett")
                       ? QStringLiteral("jett")
                       : QString();
        };

        const QString bare = ChatLinkText::renderBodyHtml(
            QStringLiteral("status is at https://forkmesh.com/status, see it."),
            knownMember);
        check(bare.contains("<a href=\"https://forkmesh.com/status\""),
              "a bare URL inside a sentence becomes an anchor");
        check(bare.endsWith(", see it."),
              "the sentence's own punctuation stays outside the anchor");

        const QString labelled = ChatLinkText::renderBodyHtml(
            QStringLiteral("read the [release notes](https://forkmesh.com/blog) now"),
            knownMember);
        check(labelled.contains(
                  "<a href=\"https://forkmesh.com/blog\" style=\"color:#58a6ff;\">"
                  "release notes</a>"),
              "a [label](url) link renders as the label pointing at the target");
        check(!labelled.contains(QLatin1Char('[')),
              "the link syntax itself is consumed, not shown");

        const QString unsafe = ChatLinkText::renderBodyHtml(
            QStringLiteral("[click me](javascript:alert(1)) and "
                           "[home](/dashboard)"),
            knownMember);
        check(!unsafe.contains("<a ") && unsafe.contains("click me") &&
                  unsafe.contains("home"),
              "a script or relative target is not a link; its label stays text");

        const QString escaped = ChatLinkText::renderBodyHtml(
            QStringLiteral("<b>hi</b> https://example.test/it's/here"),
            knownMember);
        check(escaped.startsWith("&lt;b&gt;hi&lt;/b&gt; "),
              "message text around a link is still escaped");
        check(escaped.contains("<a href=\"https://example.test/it's/here\""),
              "an apostrophe in a URL cannot close the href attribute");

        const QString mentions = ChatLinkText::renderBodyHtml(
            QStringLiteral("@jett see https://forkmesh.com and @nobody too"),
            knownMember);
        check(mentions.contains("href=\"forkmesh-mention:jett\"") &&
                  mentions.contains("@nobody") &&
                  !mentions.contains("forkmesh-mention:nobody"),
              "mentions of people in the room still link, unknown names do not");

        check(ChatLinkText::sanitizedHref(QStringLiteral("mailto:hi@forkmesh.com"))
                  == QStringLiteral("mailto:hi@forkmesh.com"),
              "mailto: targets are allowed");
        check(ChatLinkText::sanitizedHref(QStringLiteral("java\tscript:alert(1)"))
                  .isEmpty(),
              "a scheme smuggled past with control characters is rejected");

        // The web composer sends a rich source plus a flattened "text" that
        // drops [label](url) down to "label" — so the address behind a linked
        // word only survives if the source is flattened here instead.
        const QString flattened = ChatLinkText::flattenRichSourceKeepingLinks(
            QStringLiteral("- **ship** the [notes](https://forkmesh.com/b_*x*)\n"
                           "> `code` and *soon*"));
        check(flattened == QStringLiteral(
                               "ship the [notes](https://forkmesh.com/b_*x*)\n"
                               "code and soon"),
              "rich markers are dropped, link syntax and link targets survive");

        // Composing a link: wrap the selection, and take a URL already on the
        // clipboard as the target.
        const ChatLinkText::Insertion wrapped = ChatLinkText::linkInsertion(
            QStringLiteral("see the notes here"), 8, 13);
        check(wrapped.text == QStringLiteral("see the [notes](https://) here"),
              "the selected words become the link label");
        check(wrapped.text.mid(wrapped.selectionStart, wrapped.selectionLength) ==
                  QStringLiteral("https://"),
              "the caret lands on the target the author still has to type");

        const ChatLinkText::Insertion pasted = ChatLinkText::linkInsertion(
            QStringLiteral("look: "), 6, 6,
            QStringLiteral("https://forkmesh.com/status"));
        check(pasted.text ==
                  QStringLiteral("look: [link text](https://forkmesh.com/status)"),
              "a URL on the clipboard fills in the target");
        check(pasted.text.mid(pasted.selectionStart, pasted.selectionLength) ==
                  QStringLiteral("link text"),
              "with the target known, the placeholder label is selected instead");
    }

    {
        // Transient visitors (adhoc #404): a browser guest is forgotten after
        // ten idle minutes, while nodes and accounts keep their offline row.
        using namespace ChatVisitorPresence;
        check(isTransientVisitor(QStringLiteral("guest"), QStringLiteral("jett")),
              "an advertised guest account kind marks a transient visitor");
        check(isTransientVisitor(QString(), QStringLiteral("Guest 1667")) &&
                  isTransientVisitor(QString(), QStringLiteral("World Guest f49ab8")) &&
                  isTransientVisitor(QString(),
                                     QString::fromUtf8("World visitor \xC2\xB7 jett")),
              "guest and World-visitor names mark a transient visitor");
        check(!isTransientVisitor(QStringLiteral("node"), QStringLiteral("mirror-1")) &&
                  !isTransientVisitor(QStringLiteral("user"), QStringLiteral("jett")) &&
                  !isTransientVisitor(QString(), QStringLiteral("guesthouse")),
              "nodes, accounts, and guest-lookalike names are not visitors");

        const qint64 now = 1700000000000LL;
        check(!visitorIsIdle(now - kVisitorIdleMs + 1000, now),
              "a visitor seen inside the idle window is kept");
        check(visitorIsIdle(now - kVisitorIdleMs - 1000, now),
              "a visitor silent past the idle window is forgotten");
        check(!visitorIsIdle(0, now),
              "a visitor with no sighting yet is left alone");
    }

    {
        const QString input =
            "Already [#154][issue], ``#155 abcdef1``, and a definition.\n"
            "\n"
            "[issue]: forkmesh://issue/owner/repo/154";
        const QString linked = ReferenceLinks::linkifyMarkdownReferences(input);
        check(linked == input,
              "reference linker skips reference-style links and multi-backtick code");
    }

    ForkMeshIdentity identity;
    const bool identityLoaded = identity.load();
    if (!identityLoaded)
        qCritical("Identity load error: %s", qPrintable(identity.errorString()));
    check(identityLoaded, "Ed25519 identity loads or generates");
    check(identity.isValid(), "Ed25519 identity is valid");
    check(!identity.publicKey().isEmpty(), "public key is exported");

    const QJsonObject signedProfile =
        identity.signedProfile("node-a", "mainnode:node-a",
                               "bitcoincash:qp...");
    check(signedProfile.value("kind").toString() == "forkmesh.identity",
          "identity profile has the expected kind");
    check(signedProfile.value("profile").toObject().value("pubkey").toString() ==
              identity.publicKey(),
          "signed profile includes the identity public key");
    check(!signedProfile.value("signature").toString().isEmpty(),
          "signed profile includes a signature");

    // Admin-moderation deletes are signed with signData() and authenticated by
    // every peer via ForkMeshIdentity::verifySignature against the signer's key.
    {
        const QByteArray canonical =
            QByteArrayLiteral("forkmesh-admin-delete-v1\n#general\nmsg-1\n") +
            identity.publicKey().toUtf8() + QByteArrayLiteral("\n1700000000000");
        const QString sig = identity.signData(canonical);
        check(!sig.isEmpty(), "admin-delete canonical signs");
        check(ForkMeshIdentity::verifySignature(identity.publicKey(), sig, canonical),
              "verifySignature accepts a valid admin-delete signature");
        check(!ForkMeshIdentity::verifySignature(identity.publicKey(), sig,
                                                 canonical + "x"),
              "verifySignature rejects a tampered payload");
        // Tamper with the FIRST base64 character, not the last: the trailing
        // characters of a 64-byte signature carry bits that decode to nothing,
        // so rewriting them sometimes yields the very same signature and the
        // check flaked. Every bit of the first character is significant.
        const QString tamperedSignature =
            (sig.startsWith(QLatin1Char('A')) ? QStringLiteral("B")
                                              : QStringLiteral("A")) +
            sig.mid(1);
        check(!ForkMeshIdentity::verifySignature(identity.publicKey(),
                                                 tamperedSignature, canonical),
              "verifySignature rejects a tampered signature");
    }

    // The #welcome greeting must survive a restart (a fresh ForkMeshIdentity
    // reloading the same on-disk key), which was the point of colocating the
    // flag with the identity key instead of QSettings (adhoc #109).
    {
        identity.markWelcomeAnnounced();
        ForkMeshIdentity reloaded;
        check(reloaded.load(), "identity reloads from the same on-disk key");
        check(reloaded.hasAnnouncedWelcome(),
              "welcome-announced flag survives a fresh identity load");
    }

    // --- Identity key backup, export & rotation (issue #368) -------------
    // Losing the machine must not mean losing the identity: the private key can
    // be exported to a passphrase-encrypted keyfile and imported on a new node,
    // reproducing the exact same public key (hence every signature binding).
    {
        const QString keyfile = identity.exportEncryptedKeyfile("correct horse");
        check(!keyfile.isEmpty(), "identity exports to an encrypted keyfile");
        check(keyfile.contains("forkmesh.identity.keyfile"),
              "keyfile carries the age-style envelope kind");
        check(ForkMeshIdentity::keyfilePublicKey(keyfile) == identity.publicKey(),
              "keyfilePublicKey reads the pubkey without the passphrase");

        // Wrong passphrase must fail (GCM tag mismatch), never yield a key.
        ForkMeshIdentity wrongPass;
        check(!wrongPass.importEncryptedKeyfile(keyfile, "WRONG passphrase"),
              "importing with the wrong passphrase fails");

        // Correct passphrase restores the same key: identical public key and a
        // signature that verifies against the original identity's public key.
        ForkMeshIdentity restored;
        check(restored.importEncryptedKeyfile(keyfile, "correct horse"),
              "importing with the correct passphrase restores the identity");
        check(restored.publicKey() == identity.publicKey(),
              "restored identity reproduces the original public key");
        const QByteArray probe = QByteArrayLiteral("forkmesh-backup-probe");
        const QString probeSig = restored.signData(probe);
        check(ForkMeshIdentity::verifySignature(identity.publicKey(), probeSig, probe),
              "the restored key produces signatures valid under the original pubkey");
        check(restored.hasBackedUp(),
              "a restored key is marked as already backed up");

        check(ForkMeshIdentity::keyfilePublicKey("not a keyfile").isEmpty(),
              "keyfilePublicKey rejects non-keyfile text");

        // Rotation: the old key signs a successor pubkey. The record verifies
        // against the old key and is bound to that exact successor.
        const QString successor =
            QStringLiteral("Zm9ya21lc2gtc3VjY2Vzc29yLWtleS0zMi1ieXRlcw");
        const QJsonObject rotate = identity.signRotation(successor);
        check(rotate.value("kind").toString() == "forkmesh.rotate",
              "rotation record has the expected kind");
        check(rotate.value("oldPubkey").toString() == identity.publicKey() &&
                  rotate.value("newPubkey").toString() == successor,
              "rotation record binds the old and successor keys");
        check(ForkMeshIdentity::verifyRotation(rotate),
              "a well-formed rotation record verifies against the old key");
        QJsonObject tampered = rotate;
        tampered["newPubkey"] = QStringLiteral("attacker-key");
        check(!ForkMeshIdentity::verifyRotation(tampered),
              "a rotation record retargeted to a different successor is rejected");
    }

    // The desktop recovery kit uses the exact public container shared by the
    // web and mobile clients: 600k PBKDF2-SHA-256 rounds, AES-256-GCM, and the
    // portable-identity authenticated-data string.  A restored key must be
    // the same signing identity; a wrong passphrase never installs anything.
    {
        const QString portable = identity.exportPortableIdentity(
            "correct horse battery staple");
        check(portable.contains("forkmesh.portable-identity") &&
                  portable.contains("PBKDF2-HMAC-SHA-256") &&
                  portable.contains("AES-256-GCM") &&
                  portable.contains("600000"),
              "desktop exports the portable identity envelope");
        ForkMeshIdentity wrongPass;
        check(!wrongPass.importPortableIdentity(
                  portable, "not the same passphrase"),
              "portable identity rejects a wrong passphrase");
        ForkMeshIdentity restored;
        check(restored.importPortableIdentity(
                  portable, "correct horse battery staple") &&
                  restored.publicKey() == identity.publicKey(),
              "desktop portable identity restores the original signing key");
    }

    RoomCrypto crypto("repo:mainnode/forkmesh:room:general",
                      "correct horse battery staple");
    check(crypto.isValid(), "mainnode room crypto key derives");
    const QJsonObject secret{{"type", "chat"}, {"text", "encrypted hello"}};
    const QJsonObject encrypted = crypto.encryptObject(secret);
    check(encrypted.value("kind").toString() == "cipher" &&
              !encrypted.value("body").toString().isEmpty(),
          "room crypto encrypts to an opaque envelope");
    check(crypto.decryptObject(encrypted).value("text").toString() ==
              "encrypted hello",
          "room crypto decrypts a valid envelope");

    RoomCrypto wrongCrypto("repo:mainnode/forkmesh:room:general",
                           "wrong passphrase");
    check(wrongCrypto.decryptObject(encrypted).isEmpty(),
          "room crypto rejects the wrong passphrase");

    // --- Passphrase-free shared rooms (baked-in app key) -----------------
    // Two independent instances built only from the room name must interoperate,
    // so every node joins the same shared room with no passphrase.
    RoomCrypto sharedA("general");
    RoomCrypto sharedB("general");
    check(sharedA.isValid(), "passphrase-free room key derives");
    const QJsonObject sharedMsg{{"type", "chat"}, {"text", "hello shared"}};
    const QJsonObject sharedEnv = sharedA.encryptObject(sharedMsg);
    check(sharedEnv.value("kind").toString() == "cipher",
          "shared room encrypts to an opaque envelope");
    check(sharedB.decryptObject(sharedEnv).value("text").toString() == "hello shared",
          "a second node decrypts the shared room with the baked-in key");
    check(RoomCrypto("random").decryptObject(sharedEnv).isEmpty(),
          "a different room name does not decrypt the envelope");

    {
        const QString sealRoom = QStringLiteral("world-general");
        auto senderDevice = forkmesh::chatv3::ChatDevice::inMemory(QByteArray(32, '\x41'));
        auto receiverDevice = forkmesh::chatv3::ChatDevice::inMemory(QByteArray(32, '\x42'));
        RoomSeal sender(sealRoom, sealRoom, RoomSeal::Policy::Open, QStringLiteral("ada"),
                        QStringLiteral("ada-node"), senderDevice);
        RoomSeal receiver(sealRoom, sealRoom, RoomSeal::Policy::Open, QStringLiteral("bob"),
                          QStringLiteral("bob-node"), receiverDevice);
        check(sender.isValid() && receiver.isValid() &&
                  RoomSeal::isKeyFrame(sender.keyFrame()),
              "v3 room seals sign a bundle with the device identity");
        receiver.acceptKeyFrame(sender.keyFrame());
        sender.acceptKeyFrame(receiver.keyFrame());
        const QJsonObject sealPlain{{"type", QStringLiteral("chat")},
                                    {"id", QStringLiteral("m1")},
                                    {"senderId", QStringLiteral("ada-node")},
                                    {"text", QStringLiteral("no setup required")}};
        const QJsonObject sealed = sender.sealObject(sealPlain);
        check(RoomSeal::isSealedFrame(sealed) && sealed.value("v").toInt() == 3 &&
                  sealed.value("keys").toArray().size() == 2 &&
                  !QJsonDocument(sealed).toJson().contains("no setup required"),
              "a v3 frame wraps its content key for both devices and hides the text");
        check(receiver.unsealObject(sealed).value("text").toString() ==
                  QStringLiteral("no setup required"),
              "the other device opens the signed frame");
        QJsonObject legacy = sealed;
        legacy["v"] = 2;
        check(RoomSeal::isLegacyFrame(legacy) && !RoomSeal::isSealedFrame(legacy) &&
                  receiver.unsealObject(legacy).isEmpty(),
              "v2 frames are ignored, so nothing can be downgraded");
    }

    // --- Cove vault crypto (password + per-cove salt) --------------------
    // A 256-bit AES key derived from a shared password; the GCM tag is the
    // password check. Two crypto objects built from the same password + salt
    // must interoperate, a wrong password must fail to decrypt, and the same
    // password with a different salt must derive a different key.
    {
        const QByteArray coveSalt = CoveCrypto::randomSalt();
        check(coveSalt.size() == 16, "cove salt is 16 random bytes");
        CoveCrypto coveA("team-shared-password", coveSalt, CoveCrypto::defaultRounds());
        check(coveA.isValid(), "cove crypto key derives from password + salt");
        const QByteArray secret = QByteArrayLiteral("prod db password: hunter2");
        const QJsonObject sealed = coveA.encrypt(secret);
        check(sealed.contains("nonce") && sealed.contains("tag") &&
                  !sealed.value("body").toString().isEmpty(),
              "cove crypto encrypts to a nonce/tag/body envelope");
        CoveCrypto coveB("team-shared-password", coveSalt, CoveCrypto::defaultRounds());
        check(coveB.decrypt(sealed) == secret,
              "a teammate with the same password decrypts the cove payload");
        CoveCrypto coveWrong("WRONG-password", coveSalt, CoveCrypto::defaultRounds());
        check(coveWrong.decrypt(sealed).isEmpty(),
              "the wrong password fails the GCM tag and decrypts to nothing");
        CoveCrypto coveOtherSalt("team-shared-password", CoveCrypto::randomSalt(),
                                 CoveCrypto::defaultRounds());
        check(coveOtherSalt.decrypt(sealed).isEmpty(),
              "the same password with a different salt cannot decrypt the cove");
    }

    // --- Private-mirror crypto (hybrid X25519 + ML-KEM-768) --------------
    // Issue #362: a private repo is mirrored as one opaque encrypted archive.
    // The owner encrypts the archive under a random content key and wraps that
    // key to each collaborator's hybrid identity. Only a wrapped recipient can
    // recover the archive; a non-recipient, a tampered ciphertext, or one KEM
    // half swapped between wraps must all fail closed.
    {
        check(!PrivateMirrorRuntime::privateRepositoryCloudAccessAllowed(
                  true, false),
              "a local-only private repository cannot enter the cloud control plane");
        check(PrivateMirrorRuntime::privateRepositoryCloudAccessAllowed(
                  true, true),
              "explicitly served private repositories may use the encrypted cloud flow");
        check(!PrivateMirrorRuntime::privateRepositoryCloudAccessAllowed(
                  false, true),
              "the private cloud policy cannot be applied to a public repository");

        // The periodic safety pass opens with a recipient-key fetch whose URL
        // names the owner and repository. An untouched private repository must
        // therefore stay off the network entirely, while every uncertain input
        // falls back to a real sync.
        const qint64 window = 6 * 60 * 60 * 1000;
        const QString sealedRefs = QStringLiteral("a1b2c3");
        check(PrivateMirrorRuntime::privateSyncQuietPassIsIdle(
                  sealedRefs, sealedRefs, 1000, 1000 + window - 1, window),
              "an unchanged private repository skips the background pass");
        check(!PrivateMirrorRuntime::privateSyncQuietPassIsIdle(
                  sealedRefs, QStringLiteral("d4e5f6"), 1000,
                  1000 + window - 1, window),
              "a local commit moves the refs and re-enters the private sync");
        check(!PrivateMirrorRuntime::privateSyncQuietPassIsIdle(
                  sealedRefs, sealedRefs, 1000, 1000 + window, window),
              "an idle private repository re-checks its recipients after the window");
        check(!PrivateMirrorRuntime::privateSyncQuietPassIsIdle(
                  QString(), sealedRefs, 1000, 1000 + 5, window),
              "a repository with no recorded fingerprint syncs normally");
        check(!PrivateMirrorRuntime::privateSyncQuietPassIsIdle(
                  sealedRefs, QString(), 1000, 1000 + 5, window),
              "an unreadable local source syncs normally instead of going quiet");
        check(!PrivateMirrorRuntime::privateSyncQuietPassIsIdle(
                  sealedRefs, sealedRefs, 0, 1000 + 5, window),
              "a repository that never sealed successfully syncs normally");
        check(!PrivateMirrorRuntime::privateSyncQuietPassIsIdle(
                  sealedRefs, sealedRefs, 5000, 1000, window),
              "a clock that moved backwards does not grant an unbounded skip");

        {
            QString reason;
            const bool pq = MirrorCrypto::postQuantumAvailable(&reason);
            check(pq == (OpenSSL_version_num() >= 0x30500000L),
                  "ML-KEM-768 is reported available exactly when OpenSSL 3.5+ is loaded");
            check(pq ? reason.isEmpty()
                     : reason.contains(QStringLiteral("OpenSSL 3.5")) &&
                           reason.contains(QString::fromLatin1(
                               OpenSSL_version(OPENSSL_VERSION))),
                  "an unavailable ML-KEM names the required and the loaded OpenSSL");
            if (!pq) {
                QString sealError;
                check(!MirrorCrypto::generateIdentity().isValid() &&
                          MirrorCrypto::sealArchive(QByteArrayLiteral("x"),
                                                    {QJsonObject{}}, &sealError)
                              .isEmpty() &&
                          sealError == reason,
                      "without ML-KEM, private mirrors fail closed with the reason");
            }
        }
        if (MirrorCrypto::postQuantumAvailable()) {
            MirrorCrypto::Identity owner = MirrorCrypto::generateIdentity();
            MirrorCrypto::Identity alice = MirrorCrypto::generateIdentity();
            MirrorCrypto::Identity mallory = MirrorCrypto::generateIdentity();
            check(owner.isValid() && alice.isValid() && mallory.isValid(),
                  "hybrid identities generate with X25519 + ML-KEM-768 keypairs");
            check(owner.x25519Pub.size() == 32 && owner.mlkemPub.size() == 1184 &&
                      owner.mlkemPriv.size() == 2400,
                  "identity key sizes match X25519 and ML-KEM-768");
            check(MirrorCrypto::publicKeyId(owner.publicBundle()) == owner.keyId() &&
                      owner.keyId() != alice.keyId(),
                  "public bundle yields a stable, identity-specific key id");

            const QByteArray privateAgentPayload =
                QJsonDocument(QJsonObject{
                                  {"id", 42},
                                  {"prompt", "private owner-only steering prompt"},
                                  {"transcript", "private transcript tail"},
                              })
                    .toJson(QJsonDocument::Compact);
            QString ownerSealError;
            const QJsonObject ownerEnvelope = MirrorCrypto::sealOwnerPayload(
                privateAgentPayload, owner.publicBundle(), &ownerSealError);
            const QJsonArray ownerRecipients =
                ownerEnvelope.value("recipients").toArray();
            const QJsonObject ownerWrap =
                ownerRecipients.isEmpty() ? QJsonObject{}
                                          : ownerRecipients.first().toObject();
            auto fromUrl = [](const QJsonValue &value) {
                return QByteArray::fromBase64(
                    value.toString().toLatin1(),
                    QByteArray::Base64UrlEncoding |
                        QByteArray::AbortOnBase64DecodingErrors);
            };
            check(!ownerEnvelope.isEmpty() && ownerSealError.isEmpty() &&
                      ownerEnvelope.value("kind").toString() ==
                          "forkmesh.owner-sealed" &&
                      ownerEnvelope.value("alg").toString() ==
                          "x25519+mlkem768/aes256gcm" &&
                      !ownerEnvelope.contains("kid") &&
                      ownerRecipients.size() == 1 &&
                      ownerWrap.value("kid").toString() == owner.keyId(),
                  "owner control payload uses the single-recipient Worker contract");
            check(fromUrl(ownerEnvelope.value("nonce")).size() == 12 &&
                      fromUrl(ownerEnvelope.value("tag")).size() == 16 &&
                      fromUrl(ownerWrap.value("x25519")).size() == 32 &&
                      fromUrl(ownerWrap.value("mlkem768")).size() == 1088 &&
                      fromUrl(ownerWrap.value("nonce")).size() == 12 &&
                      fromUrl(ownerWrap.value("tag")).size() == 16 &&
                      fromUrl(ownerWrap.value("key")).size() == 32,
                  "owner envelope carries complete bounded X25519, ML-KEM and GCM framing");
            check(MirrorCrypto::ownerPayloadKeyId(ownerEnvelope) == owner.keyId() &&
                      MirrorCrypto::openOwnerPayload(
                          ownerEnvelope, owner, &ownerSealError) ==
                          privateAgentPayload &&
                      ownerSealError.isEmpty(),
                  "owner payload seal/open round-trips on the desktop identity");
            check(MirrorCrypto::openOwnerPayload(
                      ownerEnvelope, alice, &ownerSealError).isEmpty() &&
                      !ownerSealError.isEmpty(),
                  "another local identity cannot open owner-only agent data");
            // Companion owner devices: one envelope, one wrap entry per enrolled
            // device. The sealing device stays first (the relay reads that entry
            // as the producer) and every recipient opens the same plaintext,
            // while a non-recipient identity still cannot.
            const MirrorCrypto::Identity phone = MirrorCrypto::generateIdentity();
            QString multiSealError;
            const QJsonObject multiEnvelope = MirrorCrypto::sealOwnerPayload(
                privateAgentPayload,
                QList<QJsonObject>{owner.publicBundle(), phone.publicBundle()},
                &multiSealError);
            const QJsonArray multiRecipients =
                multiEnvelope.value("recipients").toArray();
            check(!multiEnvelope.isEmpty() && multiSealError.isEmpty() &&
                      multiRecipients.size() == 2 &&
                      multiRecipients.at(0).toObject().value("kid").toString() ==
                          owner.keyId() &&
                      multiRecipients.at(1).toObject().value("kid").toString() ==
                          phone.keyId(),
                  "owner envelope carries one ordered wrap entry per owner device");
            check(MirrorCrypto::ownerPayloadKeyId(multiEnvelope) == owner.keyId() &&
                      MirrorCrypto::ownerPayloadKeyIds(multiEnvelope) ==
                          QStringList{owner.keyId(), phone.keyId()},
                  "owner envelope key ids report the producer first");
            check(MirrorCrypto::openOwnerPayload(multiEnvelope, owner) ==
                          privateAgentPayload &&
                      MirrorCrypto::openOwnerPayload(multiEnvelope, phone) ==
                          privateAgentPayload,
                  "each enrolled owner device opens the shared owner envelope");
            check(MirrorCrypto::openOwnerPayload(
                      multiEnvelope, mallory, &multiSealError).isEmpty() &&
                      !multiSealError.isEmpty(),
                  "a device outside the owner set cannot open the envelope");
            // A wrap entry that is not this identity's must never be tried on its
            // behalf: swapping the ids leaves both entries unopenable by name.
            QJsonObject swappedEnvelope = multiEnvelope;
            QJsonArray swappedRecipients = multiRecipients;
            QJsonObject firstEntry = swappedRecipients.at(0).toObject();
            QJsonObject secondEntry = swappedRecipients.at(1).toObject();
            const QJsonValue firstKid = firstEntry.value("kid");
            firstEntry["kid"] = secondEntry.value("kid");
            secondEntry["kid"] = firstKid;
            swappedRecipients.replace(0, firstEntry);
            swappedRecipients.replace(1, secondEntry);
            swappedEnvelope["recipients"] = swappedRecipients;
            check(MirrorCrypto::openOwnerPayload(swappedEnvelope, owner).isEmpty() &&
                      MirrorCrypto::openOwnerPayload(swappedEnvelope, phone).isEmpty(),
                  "a recipient id cannot be pointed at another device's wrap entry");
            QJsonObject duplicatedEnvelope = multiEnvelope;
            duplicatedEnvelope["recipients"] =
                QJsonArray{multiRecipients.at(0), multiRecipients.at(0)};
            check(MirrorCrypto::ownerPayloadKeyIds(duplicatedEnvelope).isEmpty() &&
                      MirrorCrypto::openOwnerPayload(
                          duplicatedEnvelope, owner).isEmpty(),
                  "a duplicated recipient id invalidates the owner envelope");
            check(MirrorCrypto::sealOwnerPayload(
                      privateAgentPayload, QList<QJsonObject>{}).isEmpty() &&
                      MirrorCrypto::sealOwnerPayload(
                          privateAgentPayload,
                          QList<QJsonObject>(7, owner.publicBundle())).isEmpty(),
                  "owner envelopes refuse an empty or over-long recipient list");

            QJsonObject tamperedOwnerEnvelope = ownerEnvelope;
            QByteArray ownerBody =
                fromUrl(ownerEnvelope.value("body"));
            ownerBody[0] = char(ownerBody.at(0) ^ 0x01);
            tamperedOwnerEnvelope["body"] =
                QString::fromLatin1(
                    ownerBody.toBase64(
                        QByteArray::Base64UrlEncoding |
                        QByteArray::OmitTrailingEquals));
            check(MirrorCrypto::openOwnerPayload(
                      tamperedOwnerEnvelope, owner).isEmpty(),
                  "tampered owner-only agent ciphertext fails authentication");

            const QByteArray archive =
                QByteArrayLiteral("PACK\x00\x02") + QByteArray(50000, '\x7f') +
                QByteArrayLiteral("private repo pack bytes");
            QString sealErr;
            const QJsonObject envelope = MirrorCrypto::sealArchive(
                archive, {owner.publicBundle(), alice.publicBundle()}, &sealErr);
            check(!envelope.isEmpty() && sealErr.isEmpty() &&
                      envelope.value("alg").toString() == "x25519+mlkem768/aes256gcm",
                  "sealArchive produces a hybrid-KEM envelope for two recipients");
            check(envelope.value("recipients").toArray().size() == 2,
                  "the envelope wraps the content key once per recipient");
            check(!envelope.value("body").toString().toUtf8().contains(
                      QByteArrayLiteral("private repo pack bytes").toBase64()),
                  "the archive body is ciphertext, not the plaintext pack");

            QString openErr;
            check(MirrorCrypto::openArchive(envelope, owner, &openErr) == archive &&
                      openErr.isEmpty(),
                  "the owner decrypts the whole-mirror archive from its wrap");
            check(MirrorCrypto::openArchive(envelope, alice) == archive,
                  "a wrapped collaborator decrypts the same archive");
            check(MirrorCrypto::openArchive(envelope, mallory, &openErr).isEmpty() &&
                      !openErr.isEmpty(),
                  "a non-recipient identity cannot recover the archive");

            // Flip one byte of the archive ciphertext: GCM must reject it.
            QJsonObject tamperedBody = envelope;
            QByteArray body = QByteArray::fromBase64(
                envelope.value("body").toString().toLatin1());
            body[10] = char(body.at(10) ^ 0x01);
            tamperedBody["body"] = QString::fromLatin1(body.toBase64());
            check(MirrorCrypto::openArchive(tamperedBody, alice).isEmpty(),
                  "a tampered archive body fails the GCM tag");

            // Swap the ML-KEM ciphertext of alice's wrap for mallory's: the HKDF
            // info binds both KEM outputs, so the mismatched KEK must fail closed.
            const QJsonObject solo = MirrorCrypto::sealArchive(
                archive, {mallory.publicBundle()}, nullptr);
            QJsonArray recips = envelope.value("recipients").toArray();
            QJsonObject aliceWrap;
            for (const QJsonValue &rv : recips)
                if (rv.toObject().value("kid").toString() == alice.keyId())
                    aliceWrap = rv.toObject();
            aliceWrap["mlkem768"] = solo.value("recipients").toArray()
                                        .at(0).toObject().value("mlkem768");
            QJsonArray swapped;
            for (const QJsonValue &rv : recips) {
                if (rv.toObject().value("kid").toString() == alice.keyId())
                    swapped.append(aliceWrap);
                else
                    swapped.append(rv);
            }
            QJsonObject swappedEnv = envelope;
            swappedEnv["recipients"] = swapped;
            check(MirrorCrypto::openArchive(swappedEnv, alice).isEmpty(),
                  "swapping one hybrid-KEM half between wraps fails to decrypt");

            check(MirrorCrypto::sealArchive(archive, {}, &sealErr).isEmpty() &&
                      !sealErr.isEmpty(),
                  "sealing to zero recipients is rejected");
            check(MirrorCrypto::sealArchive(
                      archive,
                      {owner.publicBundle(), owner.publicBundle()},
                      &sealErr).isEmpty(),
                  "sealing rejects duplicate private-mirror recipients");
            QJsonObject malformedBundle = owner.publicBundle();
            malformedBundle["v"] = 9;
            check(MirrorCrypto::publicKeyId(malformedBundle).isEmpty(),
                  "malformed recipient bundles do not produce a key id");
        }
    }

    // --- Durable opaque private-mirror replicas -------------------------
    // Ciphertext is stored under a random id with owner-only permissions. A
    // recipient removal rotates the content key and prevents that identity
    // from opening the new epoch; private identity halves are never persisted
    // alongside the replica.
    if (MirrorCrypto::postQuantumAvailable()) {
        QTemporaryDir store;
        MirrorCrypto::Identity owner = MirrorCrypto::generateIdentity();
        MirrorCrypto::Identity alice = MirrorCrypto::generateIdentity();
        MirrorCrypto::Identity bob = MirrorCrypto::generateIdentity();
        const QByteArray archive =
            QByteArrayLiteral("super-private-project-name\0PACK") +
            QByteArray(8192, '\x4a');
        QString error;
        const QString opaqueId = PrivateMirrorStore::createReplica(
            store.path(), archive,
            {owner.publicBundle(), alice.publicBundle()}, &error);
        check(PrivateMirrorStore::isOpaqueId(opaqueId) && error.isEmpty(),
              "private replica is created under an unguessable opaque id");

        const QStringList files =
            QDir(store.path()).entryList(QDir::Files | QDir::NoDotAndDotDot);
        check(files == QStringList{opaqueId + ".fm-private"} &&
                  !files.value(0).contains("private-project"),
              "private replica filename discloses no owner or repository name");
        QFile encryptedFile(
            QDir(store.path()).filePath(opaqueId + ".fm-private"));
        check(encryptedFile.open(QIODevice::ReadOnly),
              "encrypted private replica can be read as ciphertext");
        QByteArray storedBytes = encryptedFile.readAll();
        encryptedFile.close();
        check(!storedBytes.contains("super-private-project-name") &&
                  !storedBytes.contains(owner.x25519Priv.toBase64()) &&
                  !storedBytes.contains(owner.mlkemPriv.toBase64()),
              "replica storage contains no plaintext name or private identity keys");
        const QFileDevice::Permissions permissions =
            QFileInfo(encryptedFile).permissions();
        check(!(permissions &
                (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                 QFileDevice::ExeGroup | QFileDevice::ReadOther |
                 QFileDevice::WriteOther | QFileDevice::ExeOther)),
              "private replica file permissions exclude group and other users");

        PrivateMirrorStore::Metadata before;
        QStringList expectedRecipients{alice.keyId(), owner.keyId()};
        expectedRecipients.sort();
        check(PrivateMirrorStore::inspectReplica(
                  store.path(), opaqueId, &before, &error) &&
                  before.keyEpoch == 1 &&
                  before.recipientKeyIds == expectedRecipients,
              "private replica metadata authenticates epoch and recipients");
        check(PrivateMirrorStore::openReplica(
                  store.path(), opaqueId, owner, nullptr, &error) == archive &&
                  PrivateMirrorStore::openReplica(
                      store.path(), opaqueId, alice, nullptr, &error) == archive,
              "only configured recipients decrypt the persisted private replica");

        check(PrivateMirrorStore::rotateRecipients(
                  store.path(), opaqueId, owner,
                  {owner.publicBundle(), bob.publicBundle()}, &error),
              "recipient revocation atomically rotates the private replica key");
        PrivateMirrorStore::Metadata after;
        check(PrivateMirrorStore::inspectReplica(
                  store.path(), opaqueId, &after, &error) &&
                  after.keyEpoch == 2 &&
                  after.ciphertextSha256 != before.ciphertextSha256,
              "private replica rotation advances its authenticated key epoch");
        check(PrivateMirrorStore::openReplica(
                  store.path(), opaqueId, alice, nullptr, &error).isEmpty() &&
                  !error.isEmpty(),
              "a revoked recipient cannot decrypt the rotated replica");
        check(PrivateMirrorStore::openReplica(
                  store.path(), opaqueId, bob, nullptr, &error) == archive,
              "a newly authorized recipient decrypts the rotated replica");
        check(PrivateMirrorStore::openReplica(
                  store.path(), "../not-an-id", owner, nullptr, &error).isEmpty(),
              "private replica lookup rejects path traversal");

        QFile corrupt(
            QDir(store.path()).filePath(opaqueId + ".fm-private"));
        check(corrupt.open(QIODevice::ReadWrite),
              "private replica opens for local corruption test");
        QJsonObject stored =
            QJsonDocument::fromJson(corrupt.readAll()).object();
        QJsonObject envelope = stored.value("envelope").toObject();
        QByteArray body =
            QByteArray::fromBase64(envelope.value("body").toString().toLatin1());
        body[0] = char(body.at(0) ^ 0x20);
        envelope["body"] = QString::fromLatin1(body.toBase64());
        stored["envelope"] = envelope;
        corrupt.resize(0);
        corrupt.write(QJsonDocument(stored).toJson(QJsonDocument::Compact));
        corrupt.close();
        check(!PrivateMirrorStore::inspectReplica(
                  store.path(), opaqueId, &after, &error),
              "ciphertext corruption is rejected before private-replica decryption");
    }

    const QString deviceProofKey = QStringLiteral(
        "ERERERERERERERERERERERERERERERERERERERERERE");
    const QString deviceProofTimestamp = QStringLiteral("1783000000000");
    const QByteArray deviceProofCanonical =
        ForkMeshIdentity::deviceBindCanonical(
            QStringLiteral(" Alice-Node "), deviceProofKey,
            deviceProofTimestamp);
    check(deviceProofCanonical ==
              QByteArrayLiteral("forkmesh-device-bind-v1\n"
                                "alice-node\n"
                                "ERERERERERERERERERERERERERERERERERERERERERE\n"
                                "1783000000000"),
          "device-bind canonical matches the cross-language vector");
    const QString deviceProofSignature =
        identity.signData(ForkMeshIdentity::deviceBindCanonical(
            QStringLiteral("Alice-Node"), identity.publicKey(),
            deviceProofTimestamp));
    check(ForkMeshIdentity::verifySignature(
              identity.publicKey(), deviceProofSignature,
              ForkMeshIdentity::deviceBindCanonical(
                  QStringLiteral("alice-node"), identity.publicKey(),
                  deviceProofTimestamp)) &&
              !ForkMeshIdentity::verifySignature(
                  identity.publicKey(), deviceProofSignature,
                  ForkMeshIdentity::deviceBindCanonical(
                      QStringLiteral("other-node"), identity.publicKey(),
                      deviceProofTimestamp)),
          "device-bind proof signs the exact normalized account canonical");

    check(AccountCapability::allowsPasswordOnlyFallback(
              QStringLiteral("pubkey_mismatch")) &&
              AccountCapability::allowsPasswordOnlyFallback(
                  QStringLiteral("device_proof_required")) &&
              AccountCapability::allowsPasswordOnlyFallback(
                  QStringLiteral("device_key_conflict")) &&
              !AccountCapability::allowsPasswordOnlyFallback(
                  QStringLiteral("invalid_credentials")),
          "only device-binding errors allow safe password-only login fallback");
    {
        const QString unreachable = AccountCapability::loginFailureFallbackMessage(
            0, QString(), QStringLiteral("app.forkmesh.com"),
            QStringLiteral("SSL handshake failed"));
        const QString noVerdict = AccountCapability::loginFailureFallbackMessage(
            403, QString(), QStringLiteral("app.forkmesh.com"), QString());
        check(unreachable.contains(QStringLiteral("Could not reach app.forkmesh.com")) &&
                  unreachable.contains(QStringLiteral("SSL handshake failed")) &&
                  unreachable.contains(QStringLiteral("not checked")) &&
                  noVerdict.contains(QStringLiteral("HTTP 403")) &&
                  noVerdict.contains(QStringLiteral("not checked")) &&
                  AccountCapability::loginFailureFallbackMessage(
                      401, QStringLiteral("unknown_code"),
                      QStringLiteral("app.forkmesh.com"), QString()) ==
                      QStringLiteral("Login failed: unknown_code.") &&
                  !AccountCapability::loginFailureFallbackMessage(
                       0, QString(), QString(), QString())
                       .contains(QStringLiteral("Login failed")),
              "a login with no relay verdict names the transport failure, not bad credentials");
    }
    check(AccountCapability::ownerSigningAllowed(
              true, true, QStringLiteral("Alice-Node")) &&
              !AccountCapability::ownerSigningAllowed(
                  true, true, QString()) &&
              !AccountCapability::ownerSigningAllowed(
                  true, false, QStringLiteral("Alice-Node")) &&
              !AccountCapability::ownerSigningAllowed(
                  false, true, QStringLiteral("Alice-Node")) &&
              AccountCapability::ownerSigningAllowed(
                  true, true, QStringLiteral("Alice-Node"),
                  QStringLiteral(" alice-node ")) &&
              !AccountCapability::ownerSigningAllowed(
                  true, true, QStringLiteral("alice-node"),
                  QStringLiteral("other-node")),
          "owner signing requires an active key-capable desktop session");
    check(AccountCapability::persistedMarkerMatches(
              QStringLiteral("Alice-Node"), QStringLiteral("primary-key"),
              QStringLiteral(" alice-node "), QStringLiteral("primary-key")) &&
              !AccountCapability::persistedMarkerMatches(
                  QStringLiteral("alice-node"), QStringLiteral("primary-key"),
                  QStringLiteral("other-node"), QStringLiteral("primary-key")) &&
              !AccountCapability::persistedMarkerMatches(
                  QStringLiteral("alice-node"), QStringLiteral("primary-key"),
                  QStringLiteral("alice-node"), QStringLiteral("rotated-key")) &&
              !AccountCapability::persistedMarkerMatches(
                  QStringLiteral("alice-node"), QStringLiteral(" primary-key "),
                  QStringLiteral("alice-node"), QStringLiteral("primary-key")) &&
              !AccountCapability::persistedMarkerMatches(
                  QString(), QString(), QStringLiteral("alice-node"),
                  QStringLiteral("primary-key")),
          "desktop capability persistence is scoped to the exact account and key");
    check(AccountCapability::loginResponseMatchesRequestedAccount(
              QStringLiteral(" Alice "), QStringLiteral("alice")) &&
              !AccountCapability::loginResponseMatchesRequestedAccount(
                  QStringLiteral("alice"), QStringLiteral("bob")) &&
              !AccountCapability::loginResponseMatchesRequestedAccount(
                  QStringLiteral("alice"), QString()),
          "login responses are correlated to the explicitly requested account");

    {
        using forkmesh::accounts::CachedSession;
        using forkmesh::accounts::SessionCache;
        const QString appRoom = QStringLiteral(
            "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/"
            "world-general/ws");
        const QString remoteRoom = QStringLiteral(
            "wss://relay.example.test/api/repo/mainnode/forkmesh/rooms/"
            "world-general/ws");
        SessionCache sessions;
        check(sessions.store(
                  {QStringLiteral(" Alice "), appRoom,
                   QStringLiteral("v2.alice.secret"),
                   QStringLiteral("local-key"), true}) &&
                  sessions.store(
                      {QStringLiteral("bob"), remoteRoom,
                       QStringLiteral("v2.bob.secret"), QString(), false}),
              "account sessions enter the memory-only cache");
        CachedSession alice;
        check(sessions.lookup(QStringLiteral("alice"), appRoom, &alice) &&
                  alice.token == QStringLiteral("v2.alice.secret") &&
                  !sessions.lookup(
                      QStringLiteral("alice"),
                      QStringLiteral(
                          "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/"
                          "rooms/another/ws"),
                      nullptr) &&
                  !sessions.lookup(QStringLiteral("alice"), remoteRoom,
                                   nullptr),
              "cached sessions are keyed by normalized account and exact room");
        check(SessionCache::maySendTo(
                  alice,
                  QUrl(QStringLiteral(
                      "https://app.forkmesh.com/api/accounts/profile"))) &&
                  !SessionCache::maySendTo(
                      alice,
                      QUrl(QStringLiteral(
                          "https://relay.example.test/api/accounts/profile"))) &&
                  !SessionCache::maySendTo(
                      alice,
                      QUrl(QStringLiteral(
                          "http://app.forkmesh.com/api/accounts/profile"))) &&
                  !SessionCache::maySendTo(
                      alice,
                      QUrl(QStringLiteral(
                          "https://app.forkmesh.com:8443/api/accounts/profile"))),
              "a cached bearer can only leave for its exact HTTPS origin");
        check(SessionCache::mayRestoreDesktopCapability(
                  alice, QStringLiteral("local-key"),
                  QStringLiteral("local-key")) &&
                  !SessionCache::mayRestoreDesktopCapability(
                      alice, QStringLiteral("rotated-local-key"),
                      QStringLiteral("local-key")) &&
                  !SessionCache::mayRestoreDesktopCapability(
                      alice, QStringLiteral("local-key"),
                      QStringLiteral("remote-key")),
              "cached signing capability requires the original and freshly "
              "validated local key");
        alice.clearSensitive();
        check(sessions.remove(QStringLiteral("alice"), appRoom,
                              QStringLiteral("v2.alice.secret")) &&
                  sessions.size() == 1,
              "explicit logout removes only its exact cached account session");
        check(sessions.store(
                  {QStringLiteral("carol"), remoteRoom,
                   QStringLiteral("v2.carol.secret"), QString(), false}) &&
                  sessions.removeInstance(remoteRoom) == 2 &&
                  sessions.size() == 0,
              "removing a relay scrubs every cached bearer for that exact instance");
        sessions.clear();
        check(sessions.size() == 0,
              "application teardown clears every in-memory account session");
    }

    // --- Issue event signing ---------------------------------------------
    // Pin the canonical byte format so the C++ client, the Python seed
    // generator, and the worker's ed25519_verify all agree.
    IssueEvent vector;
    vector.type = "open";
    vector.author = "TESTPUB";
    vector.ts = 1000;
    vector.title = "Hello";
    vector.body = "World";
    vector.attachments = {"attachments/x.png"};
    const QByteArray expected =
        "forkmesh-issue-event-v1\nopen\n1\nTESTPUB\n1000\n"
        "7b66aa5f19006b73598497bef2f5d6ed94700c48d479279df3ff5eb147fb0513";
    check(IssueStore::canonicalString(1, vector) == expected,
          "issue-event canonical string matches the cross-language vector");

    // "title" event (issue rename): content is just the title. Pin it so the
    // client and the worker's issue_event_content stay byte-identical.
    IssueEvent titleVec;
    titleVec.type = "title";
    titleVec.author = "TESTPUB";
    titleVec.ts = 2000;
    titleVec.title = "Renamed issue";
    const QByteArray expectedTitle =
        "forkmesh-issue-event-v1\ntitle\n3\nTESTPUB\n2000\n"
        "37f2b6516c608aafe45958eabd5bb4c20b9c6765d1eb39a5c27b4588e566ec97";
    check(IssueStore::canonicalString(3, titleVec) == expectedTitle,
          "title-event canonical string matches the cross-language vector");

    // "reporter" event (re-crediting a report filed on someone's behalf):
    // content is just the display name. Pin it so the client and the worker's
    // issue_event_content stay byte-identical.
    IssueEvent reporterVec;
    reporterVec.type = "reporter";
    reporterVec.author = "TESTPUB";
    reporterVec.ts = 2500;
    reporterVec.reporter = "sammy";
    const QByteArray expectedReporter =
        "forkmesh-issue-event-v1\nreporter\n3\nTESTPUB\n2500\n"
        "aaf5ad63ac417e5002bdac202e07287cf90f35b1d419464d2c4fc79e508a1e4c";
    check(IssueStore::canonicalString(3, reporterVec) == expectedReporter,
          "reporter-event canonical string matches the cross-language vector");

    // "claim" event (a contributor assigning themselves from the website,
    // issue #570): content is the claimed account plus the direction, joined by
    // NUL. Pin it so the client, the browser's webIssueEventContent and the
    // worker's issue_event_content stay byte-identical.
    IssueEvent claimVec;
    claimVec.type = "claim";
    claimVec.author = "TESTPUB";
    claimVec.ts = 4000;
    claimVec.assignee = "alice";
    claimVec.claimState = "assigned";
    const QByteArray expectedClaim =
        "forkmesh-issue-event-v1\nclaim\n42\nTESTPUB\n4000\n"
        "653b945284569a5ea948885cb71b6bb81043fa3bc446e0fc47871fb5ad798876";
    check(IssueStore::canonicalString(42, claimVec) == expectedClaim,
          "claim-event canonical string matches the cross-language vector");

    // A release signs a different content string, so a captured "assigned"
    // signature can never be replayed as an "unassign" (or the reverse).
    IssueEvent releaseVec = claimVec;
    releaseVec.claimState = "released";
    check(IssueStore::canonicalString(42, releaseVec) !=
              IssueStore::canonicalString(42, claimVec),
          "claim direction is bound by the signature");

    // "reaction" event (a bot's "seen it" on the comment that mentioned it, or
    // a person's chip): content is the event reacted to plus the vocabulary
    // slug. Pin it so the client and the worker's issue_event_content stay
    // byte-identical.
    IssueEvent reactionVec;
    reactionVec.type = "reaction";
    reactionVec.author = "TESTPUB";
    reactionVec.ts = 2750;
    reactionVec.target = "comment-abc123";
    reactionVec.reaction = "eyes";
    const QByteArray expectedReaction =
        "forkmesh-issue-event-v1\nreaction\n5\nTESTPUB\n2750\n"
        "96451960378a49c2e1aa32b017fa16cbbbd46eb7fe45f12936250c027974d018";
    check(IssueStore::canonicalString(5, reactionVec) == expectedReaction,
          "reaction-event canonical string matches the cross-language vector");

    // "delete" event (used to delete a comment, or "self" to tombstone the
    // whole issue): content is just the target event id. Pin it so the client
    // and the worker's issue_event_content stay byte-identical.
    IssueEvent deleteVec;
    deleteVec.type = "delete";
    deleteVec.author = "TESTPUB";
    deleteVec.ts = 3000;
    deleteVec.target = "comment-abc123";
    const QByteArray expectedDelete =
        "forkmesh-issue-event-v1\ndelete\n7\nTESTPUB\n3000\n"
        "3f7946e4dbf24af4c78058a30f4221dcf4379c5b62dbf1a3f5f17e8039b6ea95";
    check(IssueStore::canonicalString(7, deleteVec) == expectedDelete,
          "delete-event canonical string matches the cross-language vector");

    // --- PR conversation event signing -----------------------------------
    // Pin the canonical byte format so the C++ client and the worker's
    // verify_pull_comment_event stay byte-identical. The number is bound.
    PullEvent pullComment;
    pullComment.type = "comment";
    pullComment.author = "TESTPUB";
    pullComment.ts = 1000;
    pullComment.body = "Looks good";
    const QByteArray expectedPullComment =
        "forkmesh-pull-comment-v1\ncomment\n5\nTESTPUB\n1000\n"
        "5fc87d339144090b0ad2e192e6a6fe58e98d3a5a062467c7e62049c7d8c3db01";
    check(PullStore::canonicalString(5, pullComment) == expectedPullComment,
          "pull-comment canonical string matches the cross-language vector");

    PullEvent pullReview;
    pullReview.type = "review";
    pullReview.author = "TESTPUB";
    pullReview.ts = 2000;
    pullReview.state = "approved";
    pullReview.body = "LGTM";
    const QByteArray expectedPullReview =
        "forkmesh-pull-comment-v1\nreview\n5\nTESTPUB\n2000\n"
        "b863bbc11dd8fea92da94a7da47f815aceeaa9418483992d0a952273894a0731";
    check(PullStore::canonicalString(5, pullReview) == expectedPullReview,
          "pull-review canonical string matches the cross-language vector");

    PullEvent pullLine;
    pullLine.type = "line-comment";
    pullLine.author = "TESTPUB";
    pullLine.ts = 2500;
    pullLine.path = "src/x.cpp";
    pullLine.side = "new";
    pullLine.line = 42;
    pullLine.body = "needs a guard";
    const QByteArray expectedPullLine =
        "forkmesh-pull-comment-v1\nline-comment\n5\nTESTPUB\n2500\n"
        "a2574c2b392fd0db6b7e4b0d025d4094bb410691dca7686ddf095d63881f4985";
    check(PullStore::canonicalString(5, pullLine) == expectedPullLine,
          "pull line-comment canonical string matches the cross-language vector");

    PullEvent pullThread;
    pullThread.type = "thread-comment";
    pullThread.author = "TESTPUB";
    pullThread.ts = 2600;
    pullThread.threadId = "thread-1";
    pullThread.path = "src/x.cpp";
    pullThread.side = "new";
    pullThread.lineStart = 42;
    pullThread.lineEnd = 44;
    pullThread.body = "Use guard";
    pullThread.suggestionPatch = "@@ -1 +1 @@\n-old\n+new\n";
    const QByteArray expectedPullThread =
        "forkmesh-pull-comment-v1\nthread-comment\n5\nTESTPUB\n2600\n"
        "adbf1313d68a9d32f69734e23b1b7713d6c30dcd77b628513f8b04b5a459fe81";
    check(PullStore::canonicalString(5, pullThread) == expectedPullThread,
          "pull thread-comment canonical string matches the cross-language vector");

    // Patchset anchors (issue #483): a line-anchored event that names the head
    // commit it was written against and the commented text signs both. The
    // same vectors are pinned in app/tests/test_crypto.py and the mobile
    // inbox contract test; an event without an anchor signs the bytes above.
    PullEvent anchoredThread = pullThread;
    anchoredThread.patchset = "0123456789abcdef0123456789abcdef01234567";
    anchoredThread.anchorLines = "int x = 1;\nreturn x;\n}";
    const QByteArray expectedAnchoredThread =
        "forkmesh-pull-comment-v1\nthread-comment\n5\nTESTPUB\n2600\n"
        "3e917b7d28e0e5ac2f551e30dc6d30f986325bc015c7b2dd6d1095cd11ebd894";
    check(PullStore::canonicalString(5, anchoredThread) == expectedAnchoredThread,
          "pull anchored thread-comment canonical string matches the cross-language vector");
    PullEvent anchoredLine = pullLine;
    anchoredLine.line = 7;
    anchoredLine.patchset = "0123456789abcdef0123456789abcdef01234567";
    anchoredLine.anchorLines = "if (!p)";
    const QByteArray expectedAnchoredLine =
        "forkmesh-pull-comment-v1\nline-comment\n5\nTESTPUB\n2500\n"
        "de9b901105f5216175574abb59e8dd1e28e192c85864c7cd07e28a629adebdfa";
    check(PullStore::canonicalString(5, anchoredLine) == expectedAnchoredLine,
          "pull anchored line-comment canonical string matches the cross-language vector");

    PullEvent pullReply;
    pullReply.type = "thread-reply";
    pullReply.author = "TESTPUB";
    pullReply.ts = 2700;
    pullReply.threadId = "thread-1";
    pullReply.parentId = "event-1";
    pullReply.body = "I pushed a fix";
    const QByteArray expectedPullReply =
        "forkmesh-pull-comment-v1\nthread-reply\n5\nTESTPUB\n2700\n"
        "cd9fc0caa43648948f6a976d570191b9a4d85bee1d984a7aacc483a7487196dd";
    check(PullStore::canonicalString(5, pullReply) == expectedPullReply,
          "pull thread-reply canonical string matches the cross-language vector");

    PullEvent pullThreadState;
    pullThreadState.type = "thread-state";
    pullThreadState.author = "TESTPUB";
    pullThreadState.ts = 2800;
    pullThreadState.threadId = "thread-1";
    pullThreadState.state = "resolved";
    pullThreadState.body = "resolved after update";
    const QByteArray expectedPullThreadState =
        "forkmesh-pull-comment-v1\nthread-state\n5\nTESTPUB\n2800\n"
        "b652054bd993fa340a565f6ec3e0c88dc728c8b69d7fdf723acc6bdccc1c07df";
    check(PullStore::canonicalString(5, pullThreadState) == expectedPullThreadState,
          "pull thread-state canonical string matches the cross-language vector");

    PullEvent pullSuggestionState;
    pullSuggestionState.type = "suggestion-state";
    pullSuggestionState.author = "TESTPUB";
    pullSuggestionState.ts = 2900;
    pullSuggestionState.threadId = "thread-1";
    pullSuggestionState.state = "applied";
    pullSuggestionState.appliedCommit = "abc123def456";
    pullSuggestionState.body = "applied in follow-up";
    const QByteArray expectedPullSuggestionState =
        "forkmesh-pull-comment-v1\nsuggestion-state\n5\nTESTPUB\n2900\n"
        "07ce71665f21bcb839faeb956910a9ca369d79167e6f6e52f60c4d01344f3c52";
    check(PullStore::canonicalString(5, pullSuggestionState) ==
              expectedPullSuggestionState,
          "pull suggestion-state canonical string matches the cross-language vector");

    // --- Discussion event signing ---------------------------------------
    // Pin the canonical byte format so the C++ client and worker verifier
    // stay byte-identical. The discussion number is bound.
    DiscussionEvent openDiscussion;
    openDiscussion.type = "open";
    openDiscussion.author = "TESTPUB";
    openDiscussion.ts = 1000;
    openDiscussion.title = "Welcome";
    openDiscussion.category = "Announcements";
    openDiscussion.body = "Hello discussion";
    const QByteArray expectedDiscussionOpen =
        "forkmesh-discussion-event-v1\nopen\n1\nTESTPUB\n1000\n"
        "8e31495ce2e5559ce11564b67a45710aac9654c0f70b0fcfd0ab08dc51c83ab2";
    check(DiscussionStore::canonicalString(1, openDiscussion) ==
              expectedDiscussionOpen,
          "discussion open canonical string matches the worker vector");
    const QByteArray expectedDiscussionInboxOpen =
        "forkmesh-discussion-event-v1\nopen\n0\nTESTPUB\n1000\n"
        "8e31495ce2e5559ce11564b67a45710aac9654c0f70b0fcfd0ab08dc51c83ab2";
    check(DiscussionStore::canonicalString(0, openDiscussion) ==
              expectedDiscussionInboxOpen,
          "discussion inbox-open canonical string matches the worker vector");

    DiscussionEvent discussionComment;
    discussionComment.type = "comment";
    discussionComment.author = "TESTPUB";
    discussionComment.ts = 2000;
    discussionComment.body = "Reply body";
    const QByteArray expectedDiscussionComment =
        "forkmesh-discussion-event-v1\ncomment\n1\nTESTPUB\n2000\n"
        "b87e74db2baf019fb26d1a764aa329723024c6be7f13e5a92a60690b301bc3e9";
    check(DiscussionStore::canonicalString(1, discussionComment) ==
              expectedDiscussionComment,
          "discussion comment canonical string matches the worker vector");

    DiscussionInboxBackoff inboxBackoff;
    const QString discussionInboxKey =
        "https://app.forkmesh.com/api/repo/alice/project/discussions";
    check(!inboxBackoff.shouldBackOff(discussionInboxKey, 1000),
          "discussion inbox sync starts without a capability backoff");
    inboxBackoff.markUnsupported(discussionInboxKey, 1000);
    check(inboxBackoff.shouldBackOff(discussionInboxKey, 1000),
          "discussion inbox 404 backs off repeated sync attempts");
    check(inboxBackoff.shouldBackOff(discussionInboxKey, 1000 + 599999),
          "discussion inbox backoff lasts for the cooldown window");
    check(!inboxBackoff.shouldBackOff(discussionInboxKey, 1000 + 600000),
          "discussion inbox backoff expires after the cooldown window");
    inboxBackoff.markUnsupported(discussionInboxKey, 2000);
    inboxBackoff.clear(discussionInboxKey);
    check(!inboxBackoff.shouldBackOff(discussionInboxKey, 2000),
          "successful discussion inbox sync clears the unsupported backoff");

    // --- Exponential poll backoff ----------------------------------------
    // NetworkBackoff spaces out retries after a run of failures. Delay for the
    // nth failure is min(base*2^(n-1), cap) plus <base/8 jitter, so we assert on
    // bounds that hold for any jitter value: still blocked strictly before the
    // base delay, and definitely ready once base + its jitter span has elapsed.
    NetworkBackoff pollBackoff;
    const QString pollKey = "https://app.forkmesh.com/api/repo/alice/project/pulls";
    check(pollBackoff.ready(pollKey, 0),
          "a fresh poll channel is ready with no backoff");
    pollBackoff.noteFailure(pollKey, 0, 1000, 8000); // 1st failure: ~1000ms
    check(!pollBackoff.ready(pollKey, 999),
          "poll backoff blocks a retry before the base delay elapses");
    check(pollBackoff.ready(pollKey, 1125),
          "poll backoff clears once the base delay (+jitter span) elapses");
    // What's left of the delay is readable, so a caller can say "try again in
    // ~1s" instead of only "not yet".
    check(pollBackoff.msUntilReady(pollKey, 0) >= 1000
              && pollBackoff.msUntilReady(pollKey, 0) <= 1125,
          "the remaining backoff is reported in milliseconds");
    check(pollBackoff.msUntilReady(pollKey, 1125) == 0,
          "a channel that is ready reports no remaining backoff");
    pollBackoff.noteFailure(pollKey, 0, 1000, 8000); // 2nd failure: ~2000ms
    check(!pollBackoff.ready(pollKey, 1999),
          "a second consecutive failure at least doubles the backoff");
    pollBackoff.noteFailure(pollKey, 0, 1000, 8000); // 3rd failure: ~4000ms
    check(!pollBackoff.ready(pollKey, 3999),
          "the backoff keeps growing exponentially with each failure");
    // Independent channels don't inherit each other's backoff.
    check(pollBackoff.ready("https://app.forkmesh.com/api/accounts/heartbeat", 0),
          "poll backoff is tracked per channel");
    // Many failures stay bounded by the cap, never exploding past it.
    for (int i = 0; i < 20; ++i)
        pollBackoff.noteFailure(pollKey, 0, 1000, 8000);
    check(pollBackoff.ready(pollKey, 9000),
          "poll backoff is capped and does not grow without bound");
    pollBackoff.noteSuccess(pollKey);
    check(pollBackoff.ready(pollKey, 0),
          "a successful poll clears the exponential backoff");

    // --- How a failed reply reads in a log line (adhoc #1613) -------------
    // A relay 503 used to be logged through Qt's errorString(), which repeats
    // the URL the line already carries and stops dead at "server replied: "
    // when the response had no reason phrase — Cloudflare Workers send none.
    {
        using forkmesh::networkFailureText;
        using forkmesh::networkLogUrl;
        using forkmesh::networkResponseSnippet;
        const QString qtBoilerplate =
            "Error transferring https://app.forkmesh.com/api/repo/forkmesh/forkmesh"
            "/releases/blob/sha256/c832d0e0 - server replied: ";
        check(networkFailureText(503, QString(), qtBoilerplate) == "503",
              "a status code with no reason phrase logs as the bare code, not "
              "Qt's URL-repeating boilerplate");
        check(networkFailureText(429, "Too Many Requests", qtBoilerplate) ==
                  "429 Too Many Requests",
              "a reason phrase the server did send is what gets quoted");
        check(networkFailureText(0, QString(), "Host forkmesh.com not found") ==
                  "Host forkmesh.com not found",
              "a transport failure with no HTTP status still reports Qt's "
              "description, the only one there is");
        check(networkResponseSnippet("{\"error\":\n \"mirror_unavailable\"}") ==
                  "{\"error\": \"mirror_unavailable\"}",
              "the server's own explanation is flattened onto one line");
        check(networkResponseSnippet(QByteArray(400, 'x'), 200).size() == 201,
              "an upstream that answers with a whole HTML page is capped");
        check(networkResponseSnippet(QByteArray()).isEmpty(),
              "an empty body adds nothing to the line");
        const QString credentialPayload = networkResponseSnippet(
            R"({"room":"shared","passphrase":"room-secret","webSocketUrl":"/ws?ticket=live-ticket","ok":true})",
            0);
        check(!credentialPayload.contains(QStringLiteral("room-secret")) &&
                  !credentialPayload.contains(QStringLiteral("live-ticket")) &&
                  credentialPayload.contains(QStringLiteral("<redacted>")),
              "network body previews redact room credentials");
        const QString safeUrl = networkLogUrl(QUrl(
            QStringLiteral("https://app.forkmesh.com/api/room-access?"
                           "node=jett&ts=123&sig=live-signature&ticket=live-ticket")));
        check(safeUrl.contains(QStringLiteral("node=jett")) &&
                  safeUrl.contains(QStringLiteral("ts=123")) &&
                  !safeUrl.contains(QStringLiteral("live-signature")) &&
                  !safeUrl.contains(QStringLiteral("live-ticket")) &&
                  safeUrl.count(QStringLiteral("REDACTED")) == 2,
              "network URLs keep useful routing fields but redact credentials");

        // adhoc #134: a lookup's 404 is its answer. The status a caller names
        // rides the request in a slot of its own, beside the manager's.
        QNetworkRequest lookup(QUrl(QStringLiteral(
            "https://mastodon.social/api/v1/accounts/lookup?acct=dctrjack")));
        forkmesh::markExpectedHttpStatus(lookup, 404);
        forkmesh::markExpectedHttpStatus(lookup, 404);
        const QVariantList expected =
            lookup.attribute(forkmesh::kExpectedHttpStatusAttribute).toList();
        check(expected.size() == 1 && expected.first().toInt() == 404,
              "a request names each expected HTTP status once");
        check(forkmesh::kExpectedHttpStatusAttribute !=
                      BackoffNetworkAccessManager::kBackoffSuppressedAttribute &&
                  forkmesh::kExpectedHttpStatusAttribute !=
                      BackoffNetworkAccessManager::kReachabilityProbeAttribute &&
                  forkmesh::kExpectedHttpStatusAttribute !=
                      BackoffNetworkAccessManager::kOfflineBlockedAttribute,
              "the expected-status attribute does not share the manager's slots");
        check(!forkmesh::networkReplyStatusIsExpected(nullptr),
              "no reply is not an expected answer");

        // adhoc #157: qoto.org 308s /favicon.ico to social.cleverlibre.org,
        // whose landing /about page 500s. The net log read the finished URL,
        // missed the favicon path and logged "net GET ERR 500 … /about". A
        // redirect request is a copy of the original, so the mark rides along.
        QNetworkRequest probe(QUrl(QStringLiteral("https://qoto.org/favicon.ico")));
        forkmesh::markFaviconProbe(probe);
        QNetworkRequest redirected(probe);
        redirected.setUrl(QUrl(QStringLiteral("https://social.cleverlibre.org/about")));
        StubNetworkReply landed(redirected, QNetworkAccessManager::GetOperation,
                                500, QByteArrayLiteral("<!DOCTYPE html>"), nullptr);
        check(forkmesh::networkReplyIsFaviconProbe(&landed),
              "a favicon probe redirected to a failing page is still a probe");
        StubNetworkReply ordinary(
            QNetworkRequest(QUrl(QStringLiteral("https://qoto.org/favicon.ico"))),
            QNetworkAccessManager::GetOperation, 500, QByteArray(), nullptr);
        check(!forkmesh::networkReplyIsFaviconProbe(&ordinary) &&
                  !forkmesh::networkReplyIsFaviconProbe(nullptr),
              "only a marked request counts as a favicon probe, whatever its path");
        check(forkmesh::kFaviconProbeAttribute !=
                      forkmesh::kExpectedHttpStatusAttribute &&
                  forkmesh::kFaviconProbeAttribute !=
                      BackoffNetworkAccessManager::kBackoffSuppressedAttribute &&
                  forkmesh::kFaviconProbeAttribute !=
                      BackoffNetworkAccessManager::kReachabilityProbeAttribute &&
                  forkmesh::kFaviconProbeAttribute !=
                      BackoffNetworkAccessManager::kOfflineBlockedAttribute,
              "the favicon-probe attribute does not share another slot");

        QNetworkRequest instance(QUrl(QStringLiteral(
            "https://mastodon.mxstbr.com/api/v1/accounts/lookup?acct=mxstbr")));
        forkmesh::markExpectedTransportFailure(instance);
        TransportFailedStubReply expired(
            instance, QNetworkReply::SslHandshakeFailedError,
            QStringLiteral("SSL handshake failed: The certificate has expired"));
        check(forkmesh::networkReplyTransportFailureIsExpected(&expired),
              "a contact's instance with an expired certificate is an answer");
        TransportFailedStubReply unmarked(
            QNetworkRequest(instance.url()),
            QNetworkReply::SslHandshakeFailedError,
            QStringLiteral("SSL handshake failed: The certificate has expired"));
        check(!forkmesh::networkReplyTransportFailureIsExpected(&unmarked) &&
                  !forkmesh::networkReplyTransportFailureIsExpected(nullptr),
              "only a marked request reads a transport failure as an answer");
        StubNetworkReply answered(instance, QNetworkAccessManager::GetOperation,
                                  500, QByteArray(), nullptr);
        StubNetworkReply found(instance, QNetworkAccessManager::GetOperation,
                               200, QByteArray("{}"), nullptr);
        check(!forkmesh::networkReplyTransportFailureIsExpected(&answered) &&
                  !forkmesh::networkReplyTransportFailureIsExpected(&found),
              "a marked request the host answered is judged by its status");
        check(forkmesh::kExpectedTransportFailureAttribute !=
                      forkmesh::kFaviconProbeAttribute &&
                  forkmesh::kExpectedTransportFailureAttribute !=
                      forkmesh::kExpectedHttpStatusAttribute &&
                  forkmesh::kExpectedTransportFailureAttribute !=
                      forkmesh::kOutdatedClientBlockedAttribute &&
                  forkmesh::kExpectedTransportFailureAttribute !=
                      BackoffNetworkAccessManager::kBackoffSuppressedAttribute &&
                  forkmesh::kExpectedTransportFailureAttribute !=
                      BackoffNetworkAccessManager::kReachabilityProbeAttribute &&
                  forkmesh::kExpectedTransportFailureAttribute !=
                      BackoffNetworkAccessManager::kOfflineBlockedAttribute,
              "the expected-transport-failure attribute has a slot of its own");
    }

    // --- Closed offline replies stay readable (adhoc #275) ----------------
    // A real QNetworkReplyHttpImpl closes before its finished signal when the
    // machine has no route.  The diagnostics used to peek that closed device,
    // which emitted several QIODevice warnings instead of naming the request.
    {
        OfflineBackoffNetworkAccessManager manager;
        const QUrl url(QStringLiteral("https://offline.test/api/sync"));
        QNetworkReply *reply = manager.get(QNetworkRequest(url));
        QStringList captured;
        capturedMessages = &captured;
        QtMessageHandler previous = qInstallMessageHandler(captureMessages);
        QElapsedTimer timer;
        timer.start();
        while (!reply->isFinished() && timer.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        qInstallMessageHandler(previous);
        capturedMessages = nullptr;

        const QList<BackoffNetworkAccessManager::RequestRecord> requests =
            manager.endpointRequests(QStringLiteral("GET"), url.toString());
        const QString diagnostics = captured.join(QLatin1Char('\n'));
        const bool hasReadableRequest =
            requests.size() == 1 && requests.first().method == QLatin1String("GET") &&
            requests.first().url == url.toString() &&
            requests.first().error.contains(
                QStringLiteral("Connection unavailable - you may be offline"));
        reply->deleteLater();
        QCoreApplication::processEvents();

        check(!diagnostics.contains(QStringLiteral("device not open")),
              "a closed offline reply is not peeked or read for diagnostics");
        check(hasReadableRequest,
              "an offline diagnostic names the underlying GET URL and says the "
              "connection may be offline");
    }

    {
        StubBackoffNetworkAccessManager manager;
        QList<QNetworkReply *> replies;
        for (int i = 0; i < 15; ++i) {
            manager.responseStatuses.append(502);
            replies.append(manager.get(QNetworkRequest(QUrl(
                QStringLiteral("http://burst.test/api/chat/room-%1").arg(i)))));
        }
        check(manager.seenPaths.size() == 15,
              "a burst sent before any failure reaches the network handoff");
        QElapsedTimer timer;
        timer.start();
        const auto allFinished = [&replies] {
            for (QNetworkReply *reply : replies)
                if (!reply->isFinished())
                    return false;
            return true;
        };
        while (!allFinished() && timer.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        for (QNetworkReply *reply : replies)
            reply->deleteLater();
        const qint64 remaining =
            manager.hostCooldownRemainingMs(QStringLiteral("burst.test"));
        check(remaining > 0 && remaining <= 3000 + 3000 / 8,
              "fifteen 502s from one in-flight burst open one base cooldown, "
              "not a five-minute one");

        StubBackoffNetworkAccessManager hinted;
        hinted.responseStatuses = {502, 429};
        hinted.responseHeaders = {
            {}, {{QByteArrayLiteral("Retry-After"), QByteArrayLiteral("20")}}};
        QNetworkReply *first = hinted.get(QNetworkRequest(
            QUrl(QStringLiteral("http://hinted.test/api/a"))));
        QNetworkReply *second = hinted.get(QNetworkRequest(
            QUrl(QStringLiteral("http://hinted.test/api/b"))));
        timer.restart();
        while ((!first->isFinished() || !second->isFinished()) &&
               timer.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        first->deleteLater();
        second->deleteLater();
        check(hinted.hostCooldownRemainingMs(QStringLiteral("hinted.test")) >
                  15000,
              "a longer Retry-After from the same burst still extends the "
              "cooldown");
    }

    // --- BackoffNetworkAccessManager: host-wide 429 gate (adhoc #78) -----
    // An in-process reply stub stands in for the relay so createRequest's
    // routing/gating logic runs end-to-end without needing a loopback listener
    // (some CI/sandbox profiles deny bind()). A 429 on any path gates the whole
    // host, so neither API calls nor website probes can keep touching a
    // rate-limited Cloudflare zone during its cooldown.
    {
        StubBackoffNetworkAccessManager manager;
        const QString base = QStringLiteral("http://relay.test");
        bool lastSuppressed = false;
        const auto runRequest = [&](const QString &path) {
            const QUrl url(path.contains(QStringLiteral("://")) ? path
                                                                 : base + path);
            QNetworkReply *reply = manager.get(QNetworkRequest(url));
            bool done = false;
            QObject::connect(reply, &QNetworkReply::finished, [&done] { done = true; });
            QElapsedTimer timer;
            timer.start();
            while (!done && timer.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const QString err = reply->errorString();
            lastSuppressed =
                BackoffNetworkAccessManager::isBackoffSuppressed(reply);
            reply->deleteLater();
            return err;
        };

        manager.responseStatuses.append(429);
        runRequest(QStringLiteral("/"));
        check(manager.seenPaths == QStringList{QStringLiteral("/")},
              "first website request reaches the network handoff");

        manager.seenPaths.clear();
        const QString suppressedErr = runRequest(QStringLiteral("/api/version"));
        check(manager.seenPaths.isEmpty(),
              "an API request during the website's cooldown never reaches the network handoff");
        check(suppressedErr.contains(QStringLiteral("rate-limited")),
              "the suppressed reply reports a rate-limited error");
        // The reply carries no HTTP status (it never left the machine), so a
        // call site that wants to say "the relay is busy" rather than show a
        // dialog reading "(HTTP 0)" has to recognise it without the error text.
        check(lastSuppressed,
              "a reply the cooldown answered locally is marked as such");
        check(manager.hostCooldownRemainingMs(QStringLiteral("relay.test")) > 0,
              "the cooldown reports how long it still has to run");
        check(manager.hostCooldownRemainingMs(QStringLiteral("other.test")) == 0,
              "a host that never failed has no cooldown left to report");

        manager.seenPaths.clear();
        runRequest(QStringLiteral("/static/app.js"));
        check(manager.seenPaths.isEmpty(),
              "website assets share the host-wide cooldown");
        check(lastSuppressed,
              "a locally suppressed website reply is marked as suppressed");

        manager.seenPaths.clear();
        QNetworkReply *otherReply = manager.get(QNetworkRequest(
            QUrl(QStringLiteral("http://other.test/static/app.js"))));
        bool otherDone = false;
        QObject::connect(otherReply, &QNetworkReply::finished,
                         [&otherDone] { otherDone = true; });
        QElapsedTimer otherTimer;
        otherTimer.start();
        while (!otherDone && otherTimer.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        check(manager.seenPaths == QStringList{QStringLiteral("/static/app.js")},
              "a different host is not caught in the cooldown");
        check(!BackoffNetworkAccessManager::isBackoffSuppressed(otherReply),
              "a different host's reply reaches the network handoff");
        otherReply->deleteLater();

        manager.setFirewallEnabled(true);
        manager.seenPaths.clear();
        const QString deniedErr = runRequest(
            QStringLiteral("http://firewall.test/static/firewall-block"));
        check(manager.seenPaths.isEmpty(),
              "firewall-enabled manager blocks non-whitelisted requests locally");
        check(deniedErr.contains(QStringLiteral("firewall"), Qt::CaseInsensitive),
              "the firewall-denied reply reports a firewall block");

        int prompts = 0;
        manager.setFirewallPrompt(
            [&](const QString &, const QUrl &url, QString *ruleOut) {
                ++prompts;
                if (ruleOut)
                    *ruleOut = BackoffNetworkAccessManager::firewallRuleForUrl(url, true);
                return BackoffNetworkAccessManager::FirewallAllow;
            });
        manager.seenPaths.clear();
        runRequest(QStringLiteral(
            "http://firewall.test/static/firewall-allow"));
        check(prompts == 1 && !manager.seenPaths.isEmpty(),
              "allowing from the firewall prompt lets the request reach the network");

        manager.seenPaths.clear();
        runRequest(QStringLiteral(
            "http://firewall.test/static/firewall-allow-again"));
        check(prompts == 1 && !manager.seenPaths.isEmpty(),
              "the accepted firewall rule whitelists subsequent requests");

        // Every external destination ends up approved or denied: a denial is
        // remembered too, so the next request is blocked without asking.
        manager.setFirewallPrompt(
            [&](const QString &, const QUrl &url, QString *ruleOut) {
                ++prompts;
                if (ruleOut)
                    *ruleOut = BackoffNetworkAccessManager::firewallRuleForUrl(url);
                return BackoffNetworkAccessManager::FirewallDeny;
            });
        manager.seenPaths.clear();
        const QString deniedByPromptErr = runRequest(
            QStringLiteral("http://denied.test/static/firewall-deny"));
        check(prompts == 2 && manager.seenPaths.isEmpty() &&
                  deniedByPromptErr.contains(QStringLiteral("firewall"),
                                             Qt::CaseInsensitive),
              "denying from the firewall prompt blocks the request locally");
        check(manager.firewallDenyRules() ==
                  QStringList{QStringLiteral("host:denied.test")},
              "a denial from the prompt is remembered as a deny rule");
        runRequest(QStringLiteral("http://denied.test/static/firewall-deny-again"));
        check(prompts == 2 && manager.seenPaths.isEmpty(),
              "a remembered denial blocks later requests without prompting");

        // A favicon for an unlisted host is decoration: it is skipped without
        // a prompt (one used to hold the main window closed at startup) and
        // leaves no rule behind.
        {
            QNetworkRequest favicon(
                QUrl(QStringLiteral("http://decoration.test/favicon.ico")));
            forkmesh::markFaviconProbe(favicon);
            manager.seenPaths.clear();
            int decisions = 0;
            const QMetaObject::Connection decided = QObject::connect(
                &manager, &BackoffNetworkAccessManager::firewallRequestDecided,
                [&decisions](const QString &, const QUrl &, const QString &,
                             bool) { ++decisions; });
            QNetworkReply *reply = manager.get(favicon);
            QElapsedTimer waited;
            waited.start();
            while (!reply->isFinished() && waited.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const bool blocked = reply->error() != QNetworkReply::NoError;
            reply->deleteLater();
            check(prompts == 2 && blocked && manager.seenPaths.isEmpty() &&
                      manager.firewallDenyRules() ==
                          QStringList{QStringLiteral("host:denied.test")},
                  "an unlisted favicon host is skipped without a firewall "
                  "prompt or a remembered rule");
            QObject::disconnect(decided);
            check(decisions == 0,
                  "a skipped favicon is no firewall decision, so it raises "
                  "no Firewall blocked notice or error report");
        }

        manager.addFirewallRule(QStringLiteral("*.test"));
        runRequest(QStringLiteral("http://denied.test/static/firewall-deny-wild"));
        check(prompts == 2 && manager.seenPaths.isEmpty() &&
                  manager.firewallMatch(QUrl(QStringLiteral("http://denied.test/"))) ==
                      BackoffNetworkAccessManager::FirewallDenied,
              "a deny rule wins over an allow rule that also matches");
        check(manager.firewallMatch(QUrl(QStringLiteral("http://elsewhere.test/"))) ==
                  BackoffNetworkAccessManager::FirewallAllowed,
              "the wildcard allow rule still covers the hosts nobody denied");

        check(manager.addFirewallRule(QStringLiteral("denied.test")) &&
                  manager.firewallDenyRules().isEmpty() &&
                  manager.firewallMatch(QUrl(QStringLiteral("http://denied.test/"))) ==
                      BackoffNetworkAccessManager::FirewallAllowed,
              "allowing a denied destination moves its rule to the allow list");

        check(manager.firewallMatch(QUrl(QStringLiteral("http://unasked.example/"))) ==
                  BackoffNetworkAccessManager::FirewallUnlisted,
              "an external destination with no rule is unlisted and must be asked");
        manager.seenPaths.clear();
        runRequest(QStringLiteral("http://127.0.0.1:9/static/firewall-loopback"));
        check(prompts == 2 && !manager.seenPaths.isEmpty(),
              "loopback stays on this machine and needs no firewall decision");

        // The Stats page charts requests per destination with the verdict the
        // rules give each destination right now.
        manager.addFirewallDenyRule(QStringLiteral("blocked.test"));
        runRequest(QStringLiteral("http://blocked.test/static/firewall-denied-rule"));
        {
            using Destination = BackoffNetworkAccessManager::DestinationStats;
            const QList<Destination> destinations = manager.destinationStats();
            const auto find = [&](const QString &host) -> Destination {
                for (const Destination &row : destinations)
                    if (row.host == host)
                        return row;
                return Destination{};
            };
            const Destination denied = find(QStringLiteral("denied.test"));
            check(denied.calls == 3 && denied.blocked == 3 && denied.allowed == 0 &&
                      denied.match == BackoffNetworkAccessManager::FirewallAllowed,
                  "destination stats sum a host's requests and follow the current rules");
            const Destination blocked = find(QStringLiteral("blocked.test"));
            check(blocked.calls == 1 && blocked.blocked == 1 &&
                      blocked.match == BackoffNetworkAccessManager::FirewallDenied,
                  "a destination behind a deny rule reports denied");
            check(find(QStringLiteral("127.0.0.1")).onThisMachine,
                  "loopback destinations are marked as this machine");
            bool busiestFirst = !destinations.isEmpty();
            for (int i = 1; i < destinations.size(); ++i)
                busiestFirst &= destinations.at(i - 1).calls >= destinations.at(i).calls;
            check(busiestFirst, "destination stats list the busiest host first");
        }

        // adhoc #142: the Firewall Rules page counts what each rule decided.
        // The rule that decided is credited -- the more specific deny, not the
        // wildcard allow that also matches -- and loopback, which no rule
        // decides, credits nothing.
        {
            using Hits = BackoffNetworkAccessManager::FirewallRuleHits;
            const QHash<QString, Hits> hits = manager.firewallRuleHits();
            check(hits.value(QStringLiteral("host:blocked.test")).requests == 1 &&
                      hits.value(QStringLiteral("host:blocked.test")).blocked == 1,
                  "a deny rule is credited with the request it blocked");
            QString decidedBy;
            manager.firewallDecision(
                QUrl(QStringLiteral("http://elsewhere.test/")), &decidedBy);
            const Hits allowed = hits.value(decidedBy);
            manager.recordFirewallMatch(
                QUrl(QStringLiteral("http://elsewhere.test/x")));
            const Hits allowedAfter = manager.firewallRuleHits().value(decidedBy);
            check(!decidedBy.isEmpty() &&
                      allowedAfter.requests == allowed.requests + 1 &&
                      allowedAfter.blocked == 0,
                  "each connection an allow rule lets through adds a request, "
                  "never a block");
            const auto before = manager.firewallRuleHits();
            manager.recordFirewallMatch(QUrl(QStringLiteral("http://127.0.0.1:9/")));
            manager.recordFirewallMatch(
                QUrl(QStringLiteral("http://unasked.example/")));
            check(manager.firewallRuleHits().size() == before.size(),
                  "loopback and unlisted destinations credit no rule");
        }
    }

    // --- The firewall prompt suggests a one-click wildcard ---------------
    {
        const auto suggest = [](const char *url) {
            return BackoffNetworkAccessManager::suggestedFirewallWildcard(
                QUrl(QString::fromLatin1(url)));
        };
        check(suggest("https://app.forkmesh.com/api") == QStringLiteral("*.forkmesh.com") &&
                  suggest("https://forkmesh.com/") == QStringLiteral("*.forkmesh.com"),
              "the suggested wildcard covers the registrable domain");
        check(suggest("https://a.b.example.co.uk/") == QStringLiteral("*.example.co.uk"),
              "a country second-level suffix keeps one more label");
        check(suggest("http://127.0.0.1:8080/").isEmpty() &&
                  suggest("http://[::1]/").isEmpty() && suggest("http://intranet/").isEmpty(),
              "addresses and single-label hosts get no wildcard suggestion");
        check(BackoffNetworkAccessManager::canonicalFirewallRule(
                  suggest("https://app.forkmesh.com/")) ==
                  QStringLiteral("hostwild:forkmesh.com"),
              "the suggested wildcard stores as a hostwild rule");

        // The prompt's ladder of rules, narrowest first, each covering the URL.
        {
            using FW = BackoffNetworkAccessManager;
            const QUrl issues(QStringLiteral(
                "https://app.forkmesh.com/api/repo/acme/widget/issues?state=open"));
            const QList<FW::FirewallRuleSuggestion> ladder =
                FW::firewallRuleSuggestions(issues);
            QStringList rules;
            bool allCover = true;
            bool allExplained = true;
            for (const FW::FirewallRuleSuggestion &suggestion : ladder) {
                rules.append(FW::firewallRuleEditText(suggestion.rule));
                allCover &= FW::firewallRuleMatches(suggestion.rule, issues) &&
                            FW::canonicalFirewallRule(suggestion.rule) == suggestion.rule;
                allExplained &= !suggestion.scope.isEmpty() &&
                                !suggestion.explanation.isEmpty();
            }
            const QStringList expected = {
                QStringLiteral("https://app.forkmesh.com/api/repo/acme/widget/issues?state=open"),
                QStringLiteral("app.forkmesh.com/api/repo/acme/widget/issues"),
                QStringLiteral("app.forkmesh.com/api/repo/acme/widget/*"),
                QStringLiteral("app.forkmesh.com/api/repo/acme/*"),
                QStringLiteral("app.forkmesh.com/api/repo/*"),
                QStringLiteral("app.forkmesh.com/api/*"),
                QStringLiteral("app.forkmesh.com"),
                QStringLiteral("*.forkmesh.com/api/*"),
                QStringLiteral("*.forkmesh.com"),
                QStringLiteral("scheme:https")};
            check(rules == expected,
                  "the prompt suggests the URL, the path, every folder above it, the "
                  "host, the domain's /api/ and the domain, narrowest first");
            if (rules != expected)
                qCritical("  got: %s", qPrintable(rules.join(QStringLiteral(" | "))));
            check(allCover && allExplained,
                  "every suggestion is a canonical rule that covers the URL, with a "
                  "scope and an explanation");

            const QList<FW::FirewallRuleSuggestion> withCatalog = FW::firewallRuleSuggestions(
                issues, QStringLiteral("app.forkmesh.com/api/repo/*/issues*"));
            qsizetype catalogAt = -1;
            for (qsizetype i = 0; i < withCatalog.size(); ++i) {
                if (withCatalog.at(i).rule ==
                    FW::canonicalFirewallRule(
                        QStringLiteral("app.forkmesh.com/api/repo/*/issues*")))
                    catalogAt = i;
            }
            check(withCatalog.size() == ladder.size() + 1 && catalogAt > 1 &&
                      catalogAt < 6 &&
                      withCatalog.at(catalogAt).scope == QStringLiteral("This kind of call"),
                  "the catalog pattern joins the path rules where its specificity puts it");
            const QList<FW::FirewallRuleSuggestion> sameAsHost = FW::firewallRuleSuggestions(
                QUrl(QStringLiteral("wss://gateway.discord.gg/?v=10")),
                QStringLiteral("gateway.discord.gg"));
            bool marked = false;
            for (const FW::FirewallRuleSuggestion &suggestion : sameAsHost)
                marked |= suggestion.rule == QStringLiteral("host:gateway.discord.gg") &&
                          suggestion.explanation.contains(QStringLiteral("catalog"));
            check(marked, "a catalog pattern equal to a suggestion marks it, not a duplicate");

            QStringList deep;
            for (const FW::FirewallRuleSuggestion &suggestion : FW::firewallRuleSuggestions(
                     QUrl(QStringLiteral("https://a.b.example.co.uk:8443/x"))))
                deep.append(FW::firewallRuleEditText(suggestion.rule));
            check(deep.contains(QStringLiteral("a.b.example.co.uk:8443")) &&
                      deep.indexOf(QStringLiteral("*.b.example.co.uk")) >= 0 &&
                      deep.indexOf(QStringLiteral("*.b.example.co.uk")) <
                          deep.indexOf(QStringLiteral("*.example.co.uk")) &&
                      !deep.contains(QStringLiteral("*.co.uk")),
                  "a deep host offers its port and each parent domain down to the "
                  "registrable one");
            QStringList bare;
            for (const FW::FirewallRuleSuggestion &suggestion : FW::firewallRuleSuggestions(
                     QUrl(QStringLiteral("https://forkmesh.com/"))))
                bare.append(FW::firewallRuleEditText(suggestion.rule));
            check(bare.contains(QStringLiteral("*.forkmesh.com")),
                  "a bare domain still offers its wildcard");
            bool addressWild = false;
            for (const FW::FirewallRuleSuggestion &suggestion : FW::firewallRuleSuggestions(
                     QUrl(QStringLiteral("http://10.0.0.5:8080/api/status"))))
                addressWild |= suggestion.rule.startsWith(QStringLiteral("hostwild:")) ||
                               suggestion.rule.contains(QStringLiteral("://*."));
            check(!addressWild, "an address gets no wildcard-host suggestions");
        }

        // URL rules: firewall a path or a path pattern, not just a host.
        using FW = BackoffNetworkAccessManager;
        check(FW::canonicalFirewallRule(QStringLiteral("forkmesh.com/api/request/*")) ==
                  QStringLiteral("urlglob:*://forkmesh.com/api/request/*"),
              "a scheme-less path pattern stores as a URL pattern rule");
        check(FW::canonicalFirewallRule(QStringLiteral("https://*.forkmesh.com/api/*?x=1")) ==
                  QStringLiteral("urlglob:https://*.forkmesh.com/api/*"),
              "a pattern keeps a named scheme and a wildcard host, and drops the query");
        check(FW::canonicalFirewallRule(QStringLiteral("forkmesh.com/api/status")) ==
                  QStringLiteral("urlglob:*://forkmesh.com/api/status"),
              "a scheme-less path without * is one path on any scheme");
        check(FW::canonicalFirewallRule(QStringLiteral("https://forkmesh.com/api/status?a=1"))
                  .startsWith(QStringLiteral("url:")),
              "a full URL with a scheme and no * stays one exact URL");
        check(FW::canonicalFirewallRule(QStringLiteral("fork*mesh.com/api/*")).isEmpty(),
              "a * inside a host name is not a valid rule");
        bool roundTrips = true;
        for (const QString &rule :
             {QStringLiteral("urlglob:*://forkmesh.com/api/request/*"),
              QStringLiteral("urlglob:https://*.forkmesh.com:8443/a/*"),
              QStringLiteral("host:forkmesh.com"), QStringLiteral("hostport:forkmesh.com:443"),
              QStringLiteral("hostwild:forkmesh.com"), QStringLiteral("scheme:https"),
              QStringLiteral("*")})
            roundTrips &= FW::canonicalFirewallRule(FW::firewallRuleEditText(rule)) == rule;
        check(roundTrips, "every rule's edit text reads back as the same rule");
        check(FW::firewallRuleForPath(QUrl(QStringLiteral("https://api.test/request/42?x=1")), true) ==
                      QStringLiteral("urlglob:*://api.test/request/*") &&
                  FW::firewallRuleForPath(QUrl(QStringLiteral("https://api.test/request/42")), false) ==
                      QStringLiteral("urlglob:*://api.test/request/42") &&
                  FW::firewallRuleForPath(QUrl(QStringLiteral("https://api.test/status")), true).isEmpty(),
              "the prompt offers this path and this path and below");

        BackoffNetworkAccessManager rules;
        rules.setFirewallEnabled(true);
        rules.addFirewallDenyRule(QStringLiteral("api.test"));
        rules.addFirewallRule(QStringLiteral("api.test/request/*"));
        const auto verdict = [&rules](const char *url) {
            return rules.firewallMatch(QUrl(QString::fromLatin1(url)));
        };
        check(verdict("https://api.test/request/42") == FW::FirewallAllowed &&
                  verdict("http://api.test/request/a/b?c=d") == FW::FirewallAllowed &&
                  verdict("https://api.test/request") == FW::FirewallAllowed,
              "an allowed path pattern beats a denied host for the URLs under it");
        check(verdict("https://api.test/other") == FW::FirewallDenied &&
                  verdict("https://api.test/requests/1") == FW::FirewallDenied,
              "the denied host still covers every other path");

        rules.addFirewallRule(QStringLiteral("*.site.test"));
        rules.addFirewallDenyRule(QStringLiteral("https://www.site.test/private/*"));
        check(verdict("https://www.site.test/private/a") == FW::FirewallDenied &&
                  verdict("https://www.site.test/public") == FW::FirewallAllowed &&
                  verdict("http://www.site.test/private/a") == FW::FirewallAllowed,
              "a denied path pattern carves URLs out of an allowed domain, on its scheme only");
        rules.addFirewallRule(QStringLiteral("hostwild:tie.test"));
        rules.addFirewallDenyRule(QStringLiteral("*://*.tie.test/*"));
        check(verdict("https://a.tie.test/") == FW::FirewallDenied,
              "on the same wildcard host, a rule with a path outranks one without");

        // The list shipped with every download: valid rules, each with a
        // purpose, covering the traffic ForkMesh cannot work without and
        // nothing broader.
        QFile shipped(QStringLiteral(":/firewall/default-approved.json"));
        check(shipped.open(QIODevice::ReadOnly),
              "the default approved firewall list ships as a resource");
        QString listError;
        const QList<FW::FirewallRuleEntry> defaults =
            FW::parseFirewallRuleList(shipped.readAll(), &listError);
        bool purposes = !defaults.isEmpty();
        for (const FW::FirewallRuleEntry &entry : defaults)
            purposes &= !entry.purpose.isEmpty();
        check(listError.isEmpty() && purposes,
              "every shipped default is a valid rule with a stated purpose");
        BackoffNetworkAccessManager seeded;
        seeded.setFirewallEnabled(true);
        for (const FW::FirewallRuleEntry &entry : defaults)
            seeded.addFirewallRule(entry.rule);
        const auto seededVerdict = [&seeded](const char *url) {
            return seeded.firewallMatch(QUrl(QString::fromLatin1(url)));
        };
        check(seededVerdict("https://app.forkmesh.com/api/accounts/login") == FW::FirewallAllowed &&
                  seededVerdict("wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws") ==
                      FW::FirewallAllowed &&
                  seededVerdict("wss://app.forkmesh.com/api/nodes/events?owner=a&sig=b") ==
                      FW::FirewallAllowed &&
                  seededVerdict("https://app.forkmesh.com/api/repositories") == FW::FirewallAllowed &&
                  seededVerdict("https://app.forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/ab") ==
                      FW::FirewallAllowed &&
                  seededVerdict("https://app.forkmesh.com/api/version") == FW::FirewallAllowed &&
                  seededVerdict("https://app.forkmesh.com/status") == FW::FirewallAllowed,
              "the shipped defaults cover sign-in, sync, the relay sockets, updates and status");
        check(seededVerdict("https://app.forkmesh.com/api/crm/contacts") == FW::FirewallUnlisted &&
                  seededVerdict("https://forkmesh.com/market") == FW::FirewallUnlisted &&
                  seededVerdict("https://tracker.example/") == FW::FirewallUnlisted,
              "the shipped defaults approve critical URLs, not the whole relay or domain");
        QString badError;
        const QList<FW::FirewallRuleEntry> partial = FW::parseFirewallRuleList(
            R"({"rules":[{"rule":"fo*o.test/x"},{"rule":"a.test","purpose":"p"},{"rule":"A.test"}]})",
            &badError);
        check(partial.size() == 1 && partial.first().rule == QStringLiteral("host:a.test") &&
                  badError.contains(QStringLiteral("fo*o.test/x")),
              "a rule list skips invalid and duplicate entries and names the invalid ones");

        // The endpoint catalog describes each kind of call; the most specific
        // pattern wins and a named method beats an unnamed one.
        QString catalogError;
        const QList<FW::EndpointProfile> profiles = FW::parseEndpointCatalog(
            R"({"endpoints":[
                {"pattern":"api.test/*","description":"any api call","importance":3,
                 "modestPerHour":60,"requestNeeded":50},
                {"pattern":"api.test/items/*","description":"read items","importance":6,
                 "modestPerHour":12,"requestNeeded":20,"alternative":"item events"},
                {"pattern":"api.test/items/*","method":"post","description":"save an item",
                 "importance":9,"modestPerHour":30,"requestNeeded":100},
                {"pattern":"fo*o.test/x","description":"bad pattern","importance":5,
                 "modestPerHour":1,"requestNeeded":1},
                {"pattern":"api.test/y","description":"","importance":5,
                 "modestPerHour":1,"requestNeeded":1},
                {"pattern":"api.test/z","description":"score too high","importance":11,
                 "modestPerHour":1,"requestNeeded":1}
            ]})",
            &catalogError);
        check(profiles.size() == 3 && catalogError.contains(QStringLiteral("fo*o.test/x")) &&
                  catalogError.contains(QStringLiteral("api.test/z")) &&
                  profiles.at(2).method == QStringLiteral("POST"),
              "the endpoint catalog skips invalid entries and names them");
        const auto profileFor = [&profiles](const char *method, const char *url) {
            return FW::matchEndpointProfile(profiles, QString::fromLatin1(method),
                                            QUrl(QString::fromLatin1(url)));
        };
        check(profileFor("GET", "https://api.test/items/7") == 1 &&
                  profileFor("POST", "https://api.test/items/7") == 2 &&
                  profileFor("GET", "https://api.test/other") == 0 &&
                  profileFor("GET", "https://elsewhere.test/items/7") == -1,
              "a call matches its most specific profile, and a named method wins");
        FW::EndpointStats rateStats;
        rateStats.calls = 30;
        rateStats.firstCalledMs = 1000;
        const double perHour = FW::observedCallsPerHour(rateStats, 1000 + 30 * 60 * 1000);
        rateStats.calls = 2;
        const double fresh = FW::observedCallsPerHour(rateStats, 1000 + 1000);
        check(qAbs(perHour - 60.0) < 0.01 && qAbs(fresh - 12.0) < 0.01,
              "the observed rate is calls per hour over at least ten minutes");

        check(FW::firewallRuleSpecificity(QStringLiteral("urlglob:*://api.test/tasks")) >
                  FW::firewallRuleSpecificity(QStringLiteral("urlglob:*://api.test/tasks/*")),
              "an exact path outranks a wildcard path that also covers it");

        // The catalog shipped with every download: every entry valid, and the
        // calls that matter most described by the entry meant for them.
        QFile shippedCatalog(QStringLiteral(":/network/endpoint-catalog.json"));
        check(shippedCatalog.open(QIODevice::ReadOnly),
              "the network endpoint catalog ships as a resource");
        QString shippedError;
        const QList<FW::EndpointProfile> shippedProfiles =
            FW::parseEndpointCatalog(shippedCatalog.readAll(), &shippedError);
        check(shippedError.isEmpty() && shippedProfiles.size() >= 100,
              "every shipped endpoint catalog entry is valid");
        {
            // Each entry names where its call is made, and the file exists.
            const QDir repoRoot(QStringLiteral(FORKMESH_SOURCE_DIR "/.."));
            QStringList missing;
            for (const FW::EndpointProfile &profile : shippedProfiles) {
                if (profile.sources.isEmpty())
                    missing.append(profile.pattern + QStringLiteral(" (no source)"));
                for (const QString &source : profile.sources) {
                    const QString path = source.section(QStringLiteral(": "), 0, 0);
                    if (!QFileInfo::exists(repoRoot.filePath(path)))
                        missing.append(source);
                }
            }
            check(missing.isEmpty(),
                  "every shipped endpoint catalog entry names source files that exist");
            if (!missing.isEmpty())
                qCritical("  %s", qPrintable(missing.join(QStringLiteral("\n  "))));
        }
        const auto shippedProfile = [&shippedProfiles](const char *method,
                                                       const char *url) {
            const int index = FW::matchEndpointProfile(
                shippedProfiles, QString::fromLatin1(method), QUrl(QString::fromLatin1(url)));
            return index >= 0 ? shippedProfiles.at(index) : FW::EndpointProfile{};
        };
        const FW::EndpointProfile login =
            shippedProfile("POST", "https://app.forkmesh.com/api/accounts/login");
        const FW::EndpointProfile roomSocket = shippedProfile(
            "WS", "wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws");
        const FW::EndpointProfile taskCatalog =
            shippedProfile("GET", "https://app.forkmesh.com/api/tasks?offset=2000");
        const FW::EndpointProfile oneTask =
            shippedProfile("GET", "https://app.forkmesh.com/api/tasks/42/history");
        const FW::EndpointProfile notePoll =
            shippedProfile("GET", "https://app.forkmesh.com/api/notes/9");
        const FW::EndpointProfile noteVersions =
            shippedProfile("GET", "https://app.forkmesh.com/api/notes/9/versions");
        const FW::EndpointProfile issueDrain = shippedProfile(
            "GET", "https://app.forkmesh.com/api/repo/acme/widget/issues?owner=a&ts=1&sig=b");
        const FW::EndpointProfile issueFile = shippedProfile(
            "POST", "https://app.forkmesh.com/api/repo/acme/widget/issues");
        check(login.importance == 10 && login.requestNeeded == 100 &&
                  roomSocket.importance == 10 &&
                  roomSocket.description.contains(QStringLiteral("room socket")),
              "sign-in and the room socket are described as critical requests");
        check(taskCatalog.description.contains(QStringLiteral("catalog")) &&
                  oneTask.description.startsWith(QStringLiteral("One task")) &&
                  notePoll.requestNeeded <= 20 &&
                  noteVersions.description.contains(QStringLiteral("version")),
              "an exact path and its wildcard siblings each get their own description");
        check(issueDrain.requestNeeded < issueFile.requestNeeded &&
                  !issueDrain.alternative.isEmpty(),
              "inbox drains that push already covers score below the user writes they sit beside");
        check(shippedProfile("POST", "https://api.anthropic.com/v1/messages").importance > 0 &&
                  shippedProfile("POST", "https://api.mainnet-beta.solana.com/").importance > 0 &&
                  shippedProfile("GET", "https://mirror3.forkmesh.com/health").importance > 0 &&
                  shippedProfile("GET", "https://unknown.example/api").importance == 0,
              "external services and mirror probes are catalogued, unknown hosts are not");
        {
            // An ad is never needed, and the catalog says so: both calls sit
            // in the lowest class, under their own entries.
            const FW::EndpointProfile adServe = shippedProfile(
                "GET", "https://app.forkmesh.com/api/ads/serve?placement=desktop");
            const FW::EndpointProfile adClick = shippedProfile(
                "POST",
                "https://app.forkmesh.com/api/ads/0123456789abcdef0123456789abcdef/click");
            check(adServe.importance == 1 && adClick.importance == 1 &&
                      adServe.description.contains(QStringLiteral("Sponsored")) &&
                      adClick.description.contains(QStringLiteral("click")) &&
                      adServe.modestPerHour <= 3,
                  "the sponsored line's two calls are catalogued as cosmetic");
        }
    }

    // --- A deliberate degraded 5xx is not a host outage (adhoc #111) ------
    // Deleting a node fail-closes the public catalog snapshots for a couple of
    // minutes, so /api/repositories and /api/repo/forkmesh/forkmesh/mirrors
    // answer 503 by design, marked x-forkmesh-expected-degraded and carrying
    // their own Retry-After. That used to open the host-wide cooldown and take
    // relay sync, chat and pings down with them -- doubling with every poll
    // that met the same barrier, from one node deletion.
    {
        StubBackoffNetworkAccessManager manager;
        const QString base = QStringLiteral("http://barrier.test");
        const auto runRequest = [&](const QString &path) {
            QNetworkReply *reply =
                manager.get(QNetworkRequest(QUrl(base + path)));
            QElapsedTimer timer;
            timer.start();
            while (!reply->isFinished() && timer.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            reply->deleteLater();
        };

        manager.responseStatuses.append(503);
        manager.responseHeaders.append(
            {{QByteArray("x-forkmesh-expected-degraded"), QByteArray("1")},
             {QByteArray("Retry-After"), QByteArray("5")}});
        runRequest(QStringLiteral("/api/repo/forkmesh/forkmesh/mirrors"));
        check(manager.hostCooldownRemainingMs(QStringLiteral("barrier.test")) == 0,
              "a 5xx the Worker marked expected-degraded opens no host cooldown");

        manager.seenPaths.clear();
        runRequest(QStringLiteral("/api/sync"));
        check(manager.seenPaths == QStringList{QStringLiteral("/api/sync")},
              "relay sync still reaches the network while a snapshot barrier "
              "is up");

        // The narrower API-only 5xx gate itself is untouched: an unmarked 500
        // is an unexplained fault and still gates the host.
        manager.responseStatuses.append(500);
        manager.seenPaths.clear();
        runRequest(QStringLiteral("/api/repositories"));
        check(manager.hostCooldownRemainingMs(QStringLiteral("barrier.test")) > 0,
              "an unmarked API 5xx still opens the host cooldown");
        manager.seenPaths.clear();
        runRequest(QStringLiteral("/api/sync"));
        check(manager.seenPaths.isEmpty(),
              "the unmarked failure's cooldown suppresses the next API call");
    }

    {
        StubBackoffNetworkAccessManager manager;
        const QString base = QStringLiteral("http://quota.test");
        const auto runRequest = [&](const QString &path) {
            QNetworkReply *reply =
                manager.get(QNetworkRequest(QUrl(base + path)));
            QElapsedTimer timer;
            timer.start();
            while (!reply->isFinished() && timer.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const QString error = reply->errorString();
            reply->deleteLater();
            return error;
        };

        manager.responseStatuses.append(429);
        manager.responseHeaders.append(
            {{QByteArray("x-forkmesh-expected-degraded"), QByteArray("1")},
             {QByteArray("Retry-After"), QByteArray("450")}});
        runRequest(QStringLiteral("/api/ai/workspace-name"));
        check(manager.hostCooldownRemainingMs(QStringLiteral("quota.test")) == 0,
              "a per-account quota 429 marked expected-degraded opens no host "
              "cooldown");
        manager.seenPaths.clear();
        runRequest(QStringLiteral("/api/tasks"));
        check(manager.seenPaths == QStringList{QStringLiteral("/api/tasks")},
              "the task catalog still reaches the relay after an AI quota 429");

        manager.responseStatuses.append(502);
        runRequest(QStringLiteral("/api/chat/channels/x/room-access"));
        manager.seenPaths.clear();
        const QString suppressed = runRequest(QStringLiteral("/api/tasks"));
        check(manager.seenPaths.isEmpty() &&
                  suppressed.contains(QStringLiteral("HTTP 502")) &&
                  !suppressed.contains(QStringLiteral("429")),
              "a cooldown opened by a 5xx names that status, not a 429");
    }

    {
        StubBackoffNetworkAccessManager manager;
        manager.responseStatuses.append(429);
        QNetworkReply *first = manager.get(QNetworkRequest(
            QUrl(QStringLiteral("http://edge.test/api/sync"))));
        QElapsedTimer timer;
        timer.start();
        while (!first->isFinished() && timer.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        first->deleteLater();
        QNetworkReply *second = manager.get(QNetworkRequest(
            QUrl(QStringLiteral("http://edge.test/api/tasks"))));
        timer.restart();
        while (!second->isFinished() && timer.elapsed() < 3000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        check(second->errorString().contains(
                  QStringLiteral("rate-limited (HTTP 429)")),
              "an unmarked 429 still closes the host and says so");
        second->deleteLater();
    }

    // --- BackoffNetworkAccessManager: off-machine block (adhoc #1642) -----
    // The window-test builds turn this on at compile time so a ctest run stops
    // polling production for its own fixture repositories. Loopback is exempt,
    // so a test that stands up its own listener still reaches it.
    {
        check(!BackoffNetworkAccessManager::isOnThisMachine(
                  QUrl(QStringLiteral("https://app.forkmesh.com/api/version"))),
              "the relay is not on this machine");
        for (const QString &url :
             {QStringLiteral("http://127.0.0.1:8788/world/"),
              QStringLiteral("http://localhost:9099/world/"),
              QStringLiteral("http://[::1]:8788/"),
              QStringLiteral("qrc:/icons/logo.svg")}) {
            check(BackoffNetworkAccessManager::isOnThisMachine(QUrl(url)),
                  qPrintable(QStringLiteral("%1 counts as this machine").arg(url)));
        }

        StubBackoffNetworkAccessManager manager;
        check(!manager.offMachineRequestsBlocked(),
              "the app build sends off-machine requests as before");
        manager.setOffMachineRequestsBlocked(true);
        const auto runRequest = [&](const QString &url) {
            QNetworkReply *reply = manager.get(QNetworkRequest(QUrl(url)));
            QElapsedTimer timer;
            timer.start();
            while (!reply->isFinished() && timer.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const QNetworkReply::NetworkError error = reply->error();
            reply->deleteLater();
            return error;
        };

        manager.seenPaths.clear();
        check(runRequest(QStringLiteral(
                  "https://app.forkmesh.com/api/repo/me/wtrepo/pending")) ==
                  QNetworkReply::ContentAccessDenied,
              "a blocked manager denies an off-machine request locally");
        check(manager.seenPaths.isEmpty(),
              "the denied request never reaches the network handoff");

        manager.seenPaths.clear();
        check(runRequest(QStringLiteral("http://127.0.0.1:8788/api/version")) ==
                  QNetworkReply::NoError,
              "a blocked manager still talks to a loopback listener");
        check(manager.seenPaths == QStringList{QStringLiteral("/api/version")},
              "the loopback request reaches the network handoff");
    }

    // --- BackoffNetworkAccessManager: offline mode -------------------------
    // Offline mode uses the same shared manager as the pollers and UI actions,
    // so a request is answered immediately instead of spending its timeout
    // while the machine has no route to the relay. A marked probe and local
    // services stay available: the former lets automatic mode recover, and
    // the latter keeps a self-hosted relay usable without internet access.
    {
        StubBackoffNetworkAccessManager manager;
        const auto waitForReply = [](QNetworkReply *reply) {
            QElapsedTimer timer;
            timer.start();
            while (!reply->isFinished() && timer.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            return reply->isFinished();
        };

        manager.setOfflineMode(true);
        QNetworkReply *blocked = manager.get(QNetworkRequest(
            QUrl(QStringLiteral("https://relay.test/api/version"))));
        const bool blockedFinished = waitForReply(blocked);
        check(blockedFinished &&
                  blocked->error() ==
                      QNetworkReply::TemporaryNetworkFailureError &&
                  BackoffNetworkAccessManager::isOfflineBlocked(blocked) &&
                  manager.seenPaths.isEmpty(),
              "offline mode answers relay requests locally with a transport "
              "failure rather than handing them to the network");
        // Issue #712: a request this app chose not to send must not read as an
        // unreachable host. Every panel that quotes a failure summary -- the
        // relay table's tooltip, the endpoint diagnostics, the account sync --
        // used to tell the reader their connection might be down.
        const QString blockedSummary =
            forkmesh::networkReplyFailureSummary(blocked);
        check(forkmesh::networkReplyBlockedByOfflineMode(blocked) &&
                  blockedSummary.contains(QStringLiteral("Offline mode is on")) &&
                  blockedSummary.contains(QStringLiteral("was not sent")) &&
                  !blockedSummary.contains(
                      QStringLiteral("Connection unavailable")),
              "an offline-mode refusal names the switch instead of reporting "
              "an unreachable host");
        blocked->deleteLater();

        QNetworkRequest probe(
            QUrl(QStringLiteral("https://relay.test/api/version")));
        BackoffNetworkAccessManager::markReachabilityProbe(probe);
        QNetworkReply *probeReply = manager.get(probe);
        const bool probeFinished = waitForReply(probeReply);
        check(probeFinished && probeReply->error() == QNetworkReply::NoError &&
                  !BackoffNetworkAccessManager::isOfflineBlocked(probeReply) &&
                  !forkmesh::networkReplyBlockedByOfflineMode(probeReply) &&
                  manager.seenPaths == QStringList{QStringLiteral("/api/version")},
              "the marked reachability probe can leave automatic offline mode "
              "to detect a restored relay");
        probeReply->deleteLater();

        manager.seenPaths.clear();
        QNetworkReply *localReply = manager.get(QNetworkRequest(
            QUrl(QStringLiteral("http://127.0.0.1:8788/api/version"))));
        const bool localFinished = waitForReply(localReply);
        check(localFinished && localReply->error() == QNetworkReply::NoError &&
                  manager.seenPaths == QStringList{QStringLiteral("/api/version")},
              "offline mode keeps loopback relay requests available");
        localReply->deleteLater();
    }

    // A build older than the minimum supported release is kept off the network
    // entirely: every off-machine request is refused here rather than failing
    // one at a time against a relay that no longer answers this build the way
    // it expects, and the reader is told to update. Loopback still goes
    // through, on the same reasoning as offline mode.
    {
        check(forkmesh::clientVersionTooOldForNetwork(QStringLiteral("0.9.9")) &&
                  forkmesh::clientVersionTooOldForNetwork(
                      QStringLiteral("v0.8.10")) &&
                  !forkmesh::clientVersionTooOldForNetwork(
                      QStringLiteral("1.0.0")) &&
                  !forkmesh::clientVersionTooOldForNetwork(
                      QStringLiteral("v1.2.2")) &&
                  !forkmesh::clientVersionTooOldForNetwork(
                      QStringLiteral("1.2.2-rc1")),
              "the version gate retires builds below the minimum and leaves "
              "the minimum itself, later releases and pre-releases alone");
        // A build with no release number -- a developer build, an empty stamp,
        // a branch name -- is not older than anything, and blocking it would
        // take the network away from everyone working on this app.
        check(!forkmesh::clientVersionTooOldForNetwork(QString()) &&
                  !forkmesh::clientVersionTooOldForNetwork(
                      QStringLiteral("dev")),
              "a build that names no release number is not treated as older "
              "than the minimum");

        StubBackoffNetworkAccessManager manager;
        const auto waitForReply = [](QNetworkReply *reply) {
            QElapsedTimer timer;
            timer.start();
            while (!reply->isFinished() && timer.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            return reply->isFinished();
        };

        manager.setClientVersion(QStringLiteral("0.9.9"));
        check(manager.blockedByClientVersion(),
              "a build below the minimum reports itself blocked so the window "
              "can raise the update popup");

        QStringList blockedMethods;
        QObject::connect(
            &manager, &BackoffNetworkAccessManager::outdatedClientBlockedRequest,
            &manager, [&blockedMethods](const QString &method, const QUrl &) {
                blockedMethods.append(method);
            });

        QNetworkRequest probe(
            QUrl(QStringLiteral("https://relay.test/api/version")));
        // Even the reachability probe is refused: there is no state it could
        // discover that would let a retired build back onto the network.
        BackoffNetworkAccessManager::markReachabilityProbe(probe);
        QNetworkReply *blocked = manager.get(probe);
        const bool blockedFinished = waitForReply(blocked);
        const QString blockedSummary =
            forkmesh::networkReplyFailureSummary(blocked);
        check(blockedFinished &&
                  blocked->error() == QNetworkReply::ContentAccessDenied &&
                  BackoffNetworkAccessManager::isOutdatedClientBlocked(blocked) &&
                  forkmesh::networkReplyBlockedByOutdatedClient(blocked) &&
                  !BackoffNetworkAccessManager::isOfflineBlocked(blocked) &&
                  manager.seenPaths.isEmpty() &&
                  blockedMethods == QStringList{QStringLiteral("GET")},
              "an out-of-date build answers off-machine requests locally with "
              "an access denial and never hands them to the network");
        check(blockedSummary.contains(QStringLiteral("out of date")) &&
                  blockedSummary.contains(QStringLiteral("was not sent")) &&
                  !blockedSummary.contains(
                      QStringLiteral("Connection unavailable")),
              "an outdated-build refusal tells the reader to update instead of "
              "reporting an unreachable host");
        blocked->deleteLater();

        QNetworkReply *localReply = manager.get(QNetworkRequest(
            QUrl(QStringLiteral("http://127.0.0.1:8788/api/version"))));
        const bool localFinished = waitForReply(localReply);
        check(localFinished && localReply->error() == QNetworkReply::NoError &&
                  manager.seenPaths == QStringList{QStringLiteral("/api/version")},
              "the version gate keeps loopback requests available");
        localReply->deleteLater();

        // A shipping build is left alone, and the floor is configurable so a
        // later minimum retires more builds without another code change.
        manager.seenPaths.clear();
        manager.setClientVersion(QStringLiteral("1.2.2"));
        check(!manager.blockedByClientVersion(),
              "a build at or above the minimum keeps its network access");
        QNetworkReply *allowed = manager.get(QNetworkRequest(
            QUrl(QStringLiteral("https://relay.test/api/version"))));
        const bool allowedFinished = waitForReply(allowed);
        check(allowedFinished && allowed->error() == QNetworkReply::NoError &&
                  manager.seenPaths == QStringList{QStringLiteral("/api/version")},
              "a supported build still reaches the relay through the version "
              "gate");
        allowed->deleteLater();

        manager.setMinimumNetworkVersion(QStringLiteral("2.0.0"));
        check(manager.blockedByClientVersion() &&
                  manager.minimumNetworkVersion() == QStringLiteral("2.0.0"),
              "raising the minimum retires a build that was allowed a moment "
              "ago");
    }

    // A burn-up series must reconstruct historical state, including a close
    // and a later reopening, rather than repeating today's status backwards.
    Issue firstIssue;
    firstIssue.createdAt = 1000;
    IssueEvent firstClosed;
    firstClosed.type = "status";
    firstClosed.status = "closed";
    firstClosed.ts = 3000;
    firstIssue.events.append(firstClosed);
    Issue secondIssue;
    secondIssue.createdAt = 2000;
    IssueEvent secondClosed = firstClosed;
    secondClosed.ts = 4000;
    IssueEvent secondReopened;
    secondReopened.type = "status";
    secondReopened.status = "open";
    secondReopened.ts = 5000;
    secondIssue.events = {secondReopened, secondClosed}; // intentionally unsorted
    const QList<IssueBurnupPoint> burnup =
        buildIssueBurnupSeries({firstIssue, secondIssue}, 1000, 5000, 4);
    check(burnup.size() == 5 && burnup.at(0).openCount == 1 &&
              burnup.at(0).closedCount == 0 && burnup.at(2).openCount == 1 &&
              burnup.at(2).closedCount == 1 && burnup.at(3).openCount == 0 &&
              burnup.at(3).closedCount == 2 && burnup.at(4).openCount == 1 &&
              burnup.at(4).closedCount == 1,
          "issue burn-up reconstructs close and reopen history");
    check(firstIssueHistoryTimestamp({firstIssue, secondIssue}, 9999) == 1000,
          "issue burn-up finds the all-time starting point");

    // Sign a real event with the node identity and verify it independently.
    IssueStore store(QString(), QString(), &identity, "tester");
    IssueEvent open;
    open.type = "open";
    open.title = "First issue";
    open.body = "Body text";
    IssueEvent signed_ = store.makeSignedEvent(7, open);
    check(signed_.author == identity.publicKey(),
          "signed event is stamped with the node public key");
    check(!signed_.sig.isEmpty(), "signed event carries a signature");
    check(verifyEd25519(signed_.author, signed_.sig,
                        IssueStore::canonicalString(7, signed_)),
          "issue-event signature verifies against the public key");
    IssueEvent tampered = signed_;
    tampered.body = "Body text!";
    check(!verifyEd25519(tampered.author, tampered.sig,
                         IssueStore::canonicalString(7, tampered)),
          "tampered issue-event signature is rejected");

    IssueStore boundStore(QString(), QString(), &identity, "tester");
    boundStore.setSigningRepository("Alice", "Project");
    IssueEvent bound = boundStore.makeSignedEvent(7, open);
    PullStore boundPullStore(QString(), QString(), &identity, "tester");
    boundPullStore.setSigningRepository("O", "R");
    PullRequest boundPull = boundPullStore.makeSignedPull(PullRequest{});
    check(verifyEd25519(boundPull.author, boundPull.sig,
                        PullStore::canonicalString(boundPull, "o/r")),
          "pull submissions sign v2 for their repository");
    PullEvent boundComment;
    boundComment.type = "comment";
    boundComment.body = "hi";
    boundComment = boundPullStore.makeSignedEvent(3, boundComment);
    check(verifyEd25519(boundComment.author, boundComment.sig,
                        PullStore::canonicalString(3, boundComment, "o/r")),
          "pull comments sign v2 for their repository");
    DiscussionStore boundDiscussions(QString(), QString(), &identity, "tester");
    boundDiscussions.setSigningRepository("o", "r");
    DiscussionEvent reply;
    reply.type = "comment";
    reply.body = "hi";
    reply = boundDiscussions.makeSignedEvent(2, reply);
    check(verifyEd25519(reply.author, reply.sig,
                        DiscussionStore::canonicalString(2, reply, "o/r")),
          "discussion events sign v2 for their repository");
    IssueStore badKeyStore(QString(), QString(), &identity, "tester");
    badKeyStore.setSigningRepository("a/b", "c");
    IssueEvent badKey = badKeyStore.makeSignedEvent(7, open);
    check(verifyEd25519(badKey.author, badKey.sig, IssueStore::canonicalString(7, badKey)),
          "an unbindable repository name falls back to v1");
    check(IssueStore::canonicalString(7, bound, "alice/project").startsWith(
              "forkmesh-issue-event-v2\nalice/project\nopen\n7\n"),
          "issue-event v2 canonical string names the repository");
    check(verifyEd25519(bound.author, bound.sig,
                        IssueStore::canonicalString(7, bound, "alice/project")),
          "repo-bound issue-event signature verifies for its repository");
    check(!verifyEd25519(bound.author, bound.sig,
                         IssueStore::canonicalString(7, bound, "mallory/other")),
          "repo-bound issue-event signature is refused for another repository");
    check(!verifyEd25519(bound.author, bound.sig,
                         IssueStore::canonicalString(7, bound)),
          "repo-bound issue-event signature does not verify as v1");
    check(PullStore::canonicalString(5, PullEvent{}, "o/r").startsWith(
              "forkmesh-pull-comment-v2\no/r\n"),
          "pull-comment v2 canonical string names the repository");
    check(PullStore::canonicalString(PullRequest{}, "o/r").startsWith(
              "forkmesh-pull-event-v2\no/r\n"),
          "pull-event v2 canonical string names the repository");
    check(DiscussionStore::canonicalString(1, DiscussionEvent{}, "o/r").startsWith(
              "forkmesh-discussion-event-v2\no/r\n"),
          "discussion-event v2 canonical string names the repository");


    // "dates" event (issue #384, planned start/end): content is
    // "<startMs>\0<endMs>" as plain base-10 integers. Pin it so the client and
    // the worker's issue_event_content stay byte-identical.
    IssueEvent datesVec;
    datesVec.type = "dates";
    datesVec.author = "TESTPUB";
    datesVec.ts = 4000;
    datesVec.startDate = 1700000000000LL;
    datesVec.endDate = 1702000000000LL;
    const QByteArray expectedDates =
        "forkmesh-issue-event-v1\ndates\n7\nTESTPUB\n4000\n"
        "e07365adb1daf83b068e4f38034e0eef07af8d9fdda7b763836f9af9ef091daa";
    check(IssueStore::canonicalString(7, datesVec) == expectedDates,
          "issue dates-event canonical string matches the cross-language vector");

    // --- Project event signing (issue #384) --------------------------------
    // Pin the canonical byte format so the C++ client and the worker's
    // verifier stay byte-identical. The project number is bound.
    {
        ProjectEvent openProject;
        openProject.type = "open";
        openProject.author = "TESTPUB";
        openProject.ts = 1000;
        openProject.title = "Roadmap";
        openProject.body = "Ship the mesh";
        const QByteArray expectedProjectOpen =
            "forkmesh-project-event-v1\nopen\n1\nTESTPUB\n1000\n"
            "734c32c3b5b0e94aaa58bac54fcd507746c3e64ef8273f5e537819a268f576ee";
        check(ProjectStore::canonicalString(1, openProject) == expectedProjectOpen,
              "project open canonical string matches the cross-language vector");

        ProjectEvent projectDates;
        projectDates.type = "dates";
        projectDates.author = "TESTPUB";
        projectDates.ts = 2000;
        projectDates.startDate = 1700000000000LL;
        projectDates.endDate = 1702000000000LL;
        const QByteArray expectedProjectDates =
            "forkmesh-project-event-v1\ndates\n1\nTESTPUB\n2000\n"
            "e07365adb1daf83b068e4f38034e0eef07af8d9fdda7b763836f9af9ef091daa";
        check(ProjectStore::canonicalString(1, projectDates) ==
                  expectedProjectDates,
              "project dates canonical string matches the cross-language vector");

        // Linked issues sign as a comma-joined ascending list ("384,385") no
        // matter the stored order, so both sides hash identical bytes.
        ProjectEvent projectIssues;
        projectIssues.type = "issues";
        projectIssues.author = "TESTPUB";
        projectIssues.ts = 3000;
        projectIssues.issues = {385, 384};
        const QByteArray expectedProjectIssues =
            "forkmesh-project-event-v1\nissues\n1\nTESTPUB\n3000\n"
            "452ebda460e1290f69693757d3971639ab5f8164af5014014061b1704b47e157";
        check(ProjectStore::canonicalString(1, projectIssues) ==
                  expectedProjectIssues,
              "project issues canonical string sorts ascending and matches the "
              "cross-language vector");

        // Sign a real project event with the node identity and verify it.
        ProjectStore projectStore(QString(), QString(), &identity, "tester");
        ProjectEvent signedProject =
            projectStore.makeSignedEvent(1, openProject);
        check(verifyEd25519(signedProject.author, signedProject.sig,
                            ProjectStore::canonicalString(1, signedProject)),
              "project-event signature verifies against the public key");
        ProjectEvent tamperedProject = signedProject;
        tamperedProject.body = "Ship the mesh!";
        check(!verifyEd25519(tamperedProject.author, tamperedProject.sig,
                             ProjectStore::canonicalString(1, tamperedProject)),
              "tampered project-event signature is rejected");
    }

    // Functional round-trip (issue #384): create a project in a real temp git
    // repo, mutate its fields, and reload it — plus the issue "dates" event.
    {
        QTemporaryDir td;
        check(td.isValid(), "project store temp repo dir is valid");
        const QString dir = td.path();
        const auto git = [&](const QStringList &args) {
            QProcess p;
            p.start(QStringLiteral("git"),
                    QStringList{QStringLiteral("-C"), dir} + args);
            p.waitForFinished(10000);
        };
        git({QStringLiteral("init")});
        git({QStringLiteral("config"), QStringLiteral("user.email"),
             QStringLiteral("t@t")});
        git({QStringLiteral("config"), QStringLiteral("user.name"),
             QStringLiteral("T")});

        ProjectStore projects(dir, QString(), &identity, "tester");
        check(projects.canWrite(), "project store can write to the temp repo");
        QString err;
        const int number =
            projects.createProject("Roadmap", "Ship the mesh", 1700000000000LL,
                                   1702000000000LL, "v1.0", {385, 384}, &err);
        check(number == 1, "first project takes number 1");
        QList<Project> loaded = projects.loadAll(&err);
        check(loaded.size() == 1 && loaded.first().title == "Roadmap" &&
                  loaded.first().body == "Ship the mesh" &&
                  loaded.first().status == "open" &&
                  loaded.first().startDate == 1700000000000LL &&
                  loaded.first().endDate == 1702000000000LL &&
                  loaded.first().milestone == "v1.0" &&
                  loaded.first().issues == (QList<int>{384, 385}),
              "a created project round-trips through disk with sorted issues");
        check(projects.setStatus(number, "closed", &err) &&
                  projects.setIssues(number, {7}, &err) &&
                  projects.setDates(number, 0, 1702000000000LL, &err),
              "project status/issues/dates mutations succeed");
        loaded = projects.loadAll(&err);
        check(loaded.size() == 1 && loaded.first().status == "closed" &&
                  loaded.first().issues == (QList<int>{7}) &&
                  loaded.first().startDate == 0 &&
                  loaded.first().endDate == 1702000000000LL,
              "later project events fold over the earlier metadata");
        check(projects.tombstoneProject(number, &err),
              "tombstoning a project succeeds");
        check(projects.loadAll().isEmpty(),
              "a tombstoned project disappears from loadAll");

        IssueStore issues(dir, QString(), &identity, "tester");
        const int issueNumber =
            issues.createIssue("Test issue", "body", {}, QString(), 0, {}, {},
                               &err);
        check(issueNumber == 1, "first issue takes number 1");
        check(issues.setDates(issueNumber, 1700000000000LL, 0, &err),
              "issue setDates appends a signed dates event");
        const QList<Issue> reloaded = issues.loadAll();
        check(reloaded.size() == 1 &&
                  reloaded.first().startDate == 1700000000000LL &&
                  reloaded.first().endDate == 0,
              "issue dates fold into the reloaded metadata");

        // A public contributor's self-assignment reaches the host as a signed
        // "claim" (issue #570). It folds as a DELTA: it adds or drops only the
        // account it names, so a visitor can never clear the maintainer's own
        // assignment the way a whole-list "assignees" event would.
        check(issues.setAssignees(issueNumber, {"maintainer"}, &err),
              "issue setAssignees records the maintainer's own list");
        IssueEvent claim;
        claim.type = "claim";
        claim.id = "claim-web-1";
        claim.author = "WEBPUB";
        claim.authorName = "alice";
        claim.assignee = "alice";
        claim.claimState = "assigned";
        claim.ts = 1700000001000LL;
        check(issues.applyRemoteEvent(issueNumber, claim, QString(), &err),
              "a remote claim event merges into the issue log");
        check(issues.loadAll().first().assignees ==
                  (QStringList{"maintainer", "alice"}),
              "a claim adds only the claiming account to the assignees");
        IssueEvent again = claim;
        again.id = "claim-web-2";
        again.ts = 1700000002000LL;
        check(issues.applyRemoteEvent(issueNumber, again, QString(), &err) &&
                  issues.loadAll().first().assignees ==
                      (QStringList{"maintainer", "alice"}),
              "re-claiming an issue does not duplicate the assignee");
        IssueEvent release = claim;
        release.id = "claim-web-3";
        release.claimState = "released";
        release.ts = 1700000003000LL;
        check(issues.applyRemoteEvent(issueNumber, release, QString(), &err),
              "a remote release event merges into the issue log");
        check(issues.loadAll().first().assignees == (QStringList{"maintainer"}),
              "releasing a claim removes only the claiming account");

        // An owner-authored reward estimate, written into the issue record the
        // way the reward sync tool reads it. Paying it from the desktop wallet
        // has to land here too, not only on the relay.
        const QString issueFile =
            IssueStore::issueDirPath(dir, issueNumber) +
            QStringLiteral("/issue-%1.json").arg(issueNumber);
        QFile issueJson(issueFile);
        check(issueJson.open(QIODevice::ReadOnly),
              "the created issue has a record on disk");
        QJsonObject issueRecord =
            QJsonDocument::fromJson(issueJson.readAll()).object();
        issueJson.close();
        issueRecord.insert(QStringLiteral("rewardEstimateLamports"), 4000000);
        issueRecord.insert(QStringLiteral("rewardRecipient"),
                           QStringLiteral("atomic-gateway-7974"));
        issueRecord.insert(QStringLiteral("rewardId"),
                           QStringLiteral("reward-forkmesh-forkmesh-issue-90"));
        issueRecord.insert(QStringLiteral("rewardCondition"),
                           QStringLiteral("accepted_report"));
        issueRecord.insert(QStringLiteral("rewardStatus"),
                           QStringLiteral("ready"));
        check(writeTestFile(issueFile,
                            QJsonDocument(issueRecord).toJson(
                                QJsonDocument::Compact)),
              "the owner records a reward estimate on the issue");

        const QString issueReward =
            QStringLiteral("reward-forkmesh-forkmesh-issue-90");
        const QString issueSignature =
            QStringLiteral("3pQ7Lz9WwUu4cCn2Ab6XyR1sT8kM5vE3hJ4dF7gN9bQz");
        QString issueReceiptError;
        check(!issues.recordRewardPayment(issueNumber,
                                          QStringLiteral("reward-other"),
                                          issueSignature, &issueReceiptError) &&
                  !issueReceiptError.isEmpty(),
              "an issue refuses a receipt that belongs to another reward");
        check(issues.recordRewardPayment(issueNumber, issueReward,
                                         issueSignature, &issueReceiptError),
              "the settled transfer is recorded on the issue it paid for");
        const QList<Issue> paidIssues = issues.loadAll();
        check(paidIssues.size() == 1 &&
                  paidIssues.first().rewardStatus == QLatin1String("paid") &&
                  paidIssues.first().rewardTxSignature == issueSignature,
              "the issue now names the transaction that settled its reward");
        check(issues.recordRewardPayment(issueNumber, issueReward,
                                         issueSignature, &issueReceiptError),
              "a retried issue reconciliation of the same transfer is a no-op");
        check(!issues.recordRewardPayment(
                  issueNumber, issueReward,
                  QStringLiteral("8wR2replacementSignatureXXXXXXXXXXXXXXXXX"),
                  &issueReceiptError) &&
                  issues.loadAll().first().rewardTxSignature == issueSignature,
              "a second transfer cannot overwrite the issue's recorded receipt");

        // A mirror built before the reward fields were carried through intake
        // filed the report bare; the relay's paid estimate is adopted, but
        // only when it credits the person the issue is filed under.
        const int bareNumber =
            issues.createIssue("Bare report", "body", {}, QString(), 0, {}, {},
                               &err);
        const QString bareReward =
            QStringLiteral("reward-discord-209543620a6659929a2ed124");
        RemoteIssueMeta estimate;
        estimate.rewardRecipient = QStringLiteral("someone-else");
        estimate.rewardEstimateLamports = 2000000;
        estimate.rewardCondition = QStringLiteral("accepted_report");
        check(!issues.recordRewardPayment(bareNumber, bareReward,
                                          issueSignature, &issueReceiptError,
                                          &estimate),
              "a bare issue refuses an estimate crediting someone else");
        check(!issues.recordRewardPayment(bareNumber, bareReward,
                                          issueSignature, &issueReceiptError),
              "a bare issue refuses a receipt with no estimate to adopt");
        estimate.rewardRecipient = QStringLiteral("Tester");
        check(issues.recordRewardPayment(bareNumber, bareReward,
                                         issueSignature, &issueReceiptError,
                                         &estimate),
              "a bare issue adopts its reporter's paid estimate");
        Issue bare;
        for (const Issue &candidate : issues.loadAll())
            if (candidate.number == bareNumber)
                bare = candidate;
        check(bare.rewardId == bareReward &&
                  bare.rewardRecipient == QLatin1String("tester") &&
                  bare.rewardEstimateLamports == 2000000 &&
                  bare.rewardCondition == QLatin1String("accepted_report") &&
                  bare.rewardStatus == QLatin1String("paid") &&
                  bare.rewardTxSignature == issueSignature,
              "the adopted reward is recorded as paid on the bare issue");
    }

    // --- PullAiReview: prompt, findings JSON, quick-fix suggestions ------
    {
        const QString prompt = buildAiReviewPrompt(
            "Add feature", "Does things.",
            "diff --git a/f.c b/f.c\n--- a/f.c\n+++ b/f.c\n@@ -1 +1 @@\n+line\n");
        check(prompt.contains("Add feature") && prompt.contains("Does things.") &&
                  prompt.contains("+line"),
              "AI review prompt carries the title, description and diff");
        check(prompt.contains("JSON"), "AI review prompt demands JSON output");
        QString longDiff;
        for (int i = 0; i < 200; ++i)
            longDiff += QStringLiteral("+line number %1 padding\n").arg(i);
        const QString capped = buildAiReviewPrompt("T", QString(), longDiff, 500);
        check(capped.contains("truncated") && !capped.contains("line number 199"),
              "an oversized diff is capped with a truncation note");

        bool ok = false;
        QList<AiReviewFinding> findings = parseAiReviewFindings(
            QStringLiteral(
                "[{\"path\": \"src/a.cpp\", \"line_start\": 3, \"line_end\": 4,"
                "  \"severity\": \"bug\", \"comment\": \"Off by one\","
                "  \"original\": \"int i = 0;\\nint j = 1;\","
                "  \"fix\": \"int i = 1;\\nint j = 2;\"},"
                " {\"path\": \"src/b.cpp\", \"line\": 9, \"severity\": \"odd\","
                "  \"comment\": \"No fix here\", \"original\": null,"
                "  \"fix\": null},"
                " {\"path\": \"\", \"line_start\": 1, \"comment\": \"dropped\"}]"),
            &ok);
        check(ok && findings.size() == 2,
              "findings parse from a bare JSON array and invalid entries drop");
        check(!findings.isEmpty() && findings.at(0).path == "src/a.cpp" &&
                  findings.at(0).lineStart == 3 && findings.at(0).lineEnd == 4 &&
                  findings.at(0).severity == "bug",
              "a finding carries its anchor and severity");
        check(!findings.isEmpty() &&
                  findings.at(0).suggestionPatch.contains("-int i = 0;") &&
                  findings.at(0).suggestionPatch.contains("+int j = 2;"),
              "an original+fix pair becomes a committable suggestion patch");
        check(findings.size() == 2 && findings.at(1).severity == "warning" &&
                  findings.at(1).suggestionPatch.isEmpty() &&
                  findings.at(1).lineStart == 9 && findings.at(1).lineEnd == 9,
              "a fix-less finding normalizes its severity and line range");

        findings = parseAiReviewFindings(
            QStringLiteral("Sure!\n```json\n[{\"path\": \"x\", \"line_start\": 1,"
                           " \"comment\": \"c\"}]\n```"),
            &ok);
        check(ok && findings.size() == 1,
              "findings parse out of surrounding chatter and fences");
        findings = parseAiReviewFindings(
            QStringLiteral("user\n+int a[3] = {0};\n[ ] checklist\ncodex\n"
                           "[{\"path\": \"x\", \"line_start\": 4,"
                           " \"comment\": \"c\", \"original\": [\"a\"],"
                           " \"fix\": [\"b\"]}]\ntokens used: 12"),
            &ok);
        check(ok && findings.size() == 1 && findings.at(0).lineStart == 4,
              "findings parse after an echoed prompt that carries brackets");
        findings = parseAiReviewFindings(
            QStringLiteral("{\"findings\": [{\"path\": \"x\", \"line_start\": 2,"
                           " \"comment\": \"c\"}]}"),
            &ok);
        check(ok && findings.size() == 1,
              "findings parse from a {\"findings\": [...]} wrapper");
        check(parseAiReviewFindings(QStringLiteral("[]"), &ok).isEmpty() && ok,
              "an empty array parses as a clean review");
        check(parseAiReviewFindings(QStringLiteral("no json here"), &ok).isEmpty() &&
                  !ok,
              "an unparseable reply is distinguished from a clean review");

        const QString patch =
            buildSuggestionPatch({"foo(1);", "bar(2);"}, {"foo(2);"}, 5);
        check(patch.startsWith("@@ -5,2 +5,1 @@"),
              "suggestion patch header carries the line range");
        QStringList orig, repl;
        check(parseSuggestionPatch(patch, &orig, &repl) && orig.size() == 2 &&
                  repl.size() == 1 && orig.first() == "foo(1);" &&
                  repl.first() == "foo(2);",
              "suggestion patch round-trips original and replacement lines");

        QString exact = "1\n2\n3\n4\nfoo(1);\nbar(2);\n7\n";
        QString applyErr;
        check(applySuggestionToContent(&exact, 5, patch, &applyErr) &&
                  exact == "1\n2\n3\n4\nfoo(2);\n7\n",
              "a suggestion applies at its recorded line");
        QString drifted = "a\nfoo(1);\nbar(2);\nz\n";
        check(applySuggestionToContent(&drifted, 5, patch, &applyErr) &&
                  drifted == "a\nfoo(2);\nz\n",
              "a drifted suggestion relocates to the unique matching block");
        QString ambiguous = "foo(1);\nbar(2);\nfoo(1);\nbar(2);\n";
        check(!applySuggestionToContent(&ambiguous, 9, patch, &applyErr),
              "an ambiguous suggestion anchor is refused");
        QString missing = "nothing here\n";
        check(!applySuggestionToContent(&missing, 1, patch, &applyErr),
              "a suggestion whose lines are gone is refused");
        const QString delPatch = buildSuggestionPatch({"kill me"}, {}, 2);
        QString delContent = "keep\nkill me\nkeep2\n";
        check(applySuggestionToContent(&delContent, 2, delPatch, &applyErr) &&
                  delContent == "keep\nkeep2\n",
              "an empty replacement deletes the anchored lines");
        check(buildSuggestionPatch({}, {"new"}, 1).isEmpty(),
              "a pure insertion is not representable as a suggestion");
    }

    // --- Checklist tasks become linked sub-issues in one durable batch ----
    QTemporaryDir subIssueTmp;
    if (subIssueTmp.isValid()) {
        auto subGit = [&](const QStringList &args) {
            QProcess process;
            process.start("git", QStringList{"-C", subIssueTmp.path()} + args);
            process.waitForFinished(8000);
            return process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
        };
        auto subGitOutput = [&](const QStringList &args) {
            QProcess process;
            process.start("git", QStringList{"-C", subIssueTmp.path()} + args);
            process.waitForFinished(8000);
            return process.readAllStandardOutput().trimmed();
        };
        check(subGit({"init", "-q"}) &&
                  subGit({"config", "user.email", "subissues@forkmesh.local"}) &&
                  subGit({"config", "user.name", "subissue tester"}),
              "sub-issue test repository initializes");

        const QString checklist =
            "## Marketplace candidates\n"
            "- [ ] Desktop theme packs\n"
            "  - [ ] World outfit bundles\n"
            "- [x] Already packaged\n"
            "[ ] Dashboard calendar widgets\n"
            "- [ ] Existing relation (sub-issue #99)\n";
        check(IssueStore::subIssueTitles(checklist) ==
                  (QStringList{"Desktop theme packs", "World outfit bundles",
                               "Dashboard calendar widgets"}),
              "subIssueTitles accepts nested and compact unchecked tasks only");

        const QList<IssueChecklistTask> parsedTasks =
            IssueStore::checklistTasks(checklist);
        check(parsedTasks.size() == 5 && !parsedTasks.at(0).checked &&
                  parsedTasks.at(1).indent == 2 && parsedTasks.at(2).checked &&
                  parsedTasks.at(4).subIssueNumber == 99 &&
                  parsedTasks.at(4).label == "Existing relation",
              "checklistTasks exposes state, nesting, and durable relationships");
        const QString dependencyBody =
            "Intro #7 is only a mention\n"
            "- [ ] Task\n"
            "Parent issue: #12\n"
            "Depends on: #4, #12 and #0\n";
        const QString dependencyDisplay =
            IssueStore::bodyWithoutDependencyLines(dependencyBody);
        check(IssueStore::dependencyNumbers(dependencyBody) ==
                      (QList<int>{12, 4}) &&
                  dependencyDisplay == "Intro #7 is only a mention\n- [ ] Task\n\n\n" &&
                  dependencyDisplay.count('\n') == dependencyBody.count('\n') &&
                  IssueStore::checklistTasks(dependencyDisplay).at(0).line ==
                      IssueStore::checklistTasks(dependencyBody).at(0).line,
              "dependency lines parse in order and blank without shifting task lines");
        const QString bulletDependencies =
            "## Delivery and dependencies\n"
            "- Depends on: #481, #484\n"
            "* **Parent issue:** #7\n"
            "- Depends on: None\n";
        check(IssueStore::dependencyNumbers(bulletDependencies) ==
                      (QList<int>{481, 484, 7}) &&
                  IssueStore::bodyWithoutDependencyLines(bulletDependencies) ==
                      "## Delivery and dependencies\n\n\n- Depends on: None\n",
              "bulleted and bold relationship lines count; a line naming none stays");
        check(IssueStore::referencedIssueNumbers(
                  "See #5 and #6, again #5.\n"
                  "`#7` is code, foo#8 and &#9; are not references\n"
                  "```\n#10 inside a fence\n```\n"
                  "Depends on: #11\n") == (QList<int>{5, 6, 11}),
              "issue references skip code, glued words, and entities");

        auto relationIssue = [](int number, const QString &title,
                                const QString &body, const QString &who,
                                qint64 ts) {
            Issue issue;
            issue.number = number;
            issue.title = title;
            issue.authorName = who;
            IssueEvent open;
            open.type = QStringLiteral("open");
            open.id = QStringLiteral("open-%1").arg(number);
            open.author = who + QStringLiteral("-key");
            open.authorName = who;
            open.title = title;
            open.body = body;
            open.ts = ts;
            issue.events.append(open);
            return issue;
        };
        Issue relationParent = relationIssue(
            20, "Parent", "- [ ] Linked row (sub-issue #24)\n", "ann", 100);
        Issue relationChild = relationIssue(
            21, "Child", "Parent issue: #20\n", "bob", 200);
        Issue relationDependent = relationIssue(
            22, "Dependent", "Depends on: #23, #20\n", "cy", 300);
        Issue relationCommenter = relationIssue(23, "Commenter", "Nothing.", "dee", 50);
        IssueEvent mentionComment;
        mentionComment.type = QStringLiteral("comment");
        mentionComment.id = QStringLiteral("c1");
        mentionComment.author = QStringLiteral("eve-key");
        mentionComment.authorName = QStringLiteral("eve");
        mentionComment.body = QStringLiteral("Blocked by #20 upstream");
        mentionComment.ts = 400;
        relationCommenter.events.append(mentionComment);
        IssueEvent deletedComment = mentionComment;
        deletedComment.id = QStringLiteral("c2");
        deletedComment.authorName = QStringLiteral("fay");
        deletedComment.ts = 500;
        relationCommenter.events.append(deletedComment);
        IssueEvent deleteEvent;
        deleteEvent.type = QStringLiteral("delete");
        deleteEvent.id = QStringLiteral("d1");
        deleteEvent.target = QStringLiteral("c2");
        deleteEvent.ts = 600;
        relationCommenter.events.append(deleteEvent);
        const QList<Issue> relationIssues{relationParent, relationChild,
                                          relationDependent, relationCommenter};
        check(IssueStore::subIssueNumbers(relationIssues, 20) ==
                  (QList<int>{21, 22, 24}) &&
                  IssueStore::subIssueNumbers(relationIssues, 23) ==
                      QList<int>{22},
              "sub-issues come from relationship lines and linked checklist rows");
        const QList<IssueMention> parentMentions =
            IssueStore::mentionsOf(relationIssues, 20);
        check(parentMentions.size() == 3 &&
                  parentMentions.at(0).sourceNumber == 21 &&
                  parentMentions.at(0).authorName == "bob" &&
                  parentMentions.at(1).sourceNumber == 22 &&
                  parentMentions.at(2).sourceNumber == 23 &&
                  parentMentions.at(2).authorName == "eve" &&
                  parentMentions.at(2).sourceTitle == "Commenter" &&
                  parentMentions.at(2).ts == 400,
              "mentions credit the describing or commenting author, skip deleted comments");
        const QString fencedTask =
            "Before\n```md\n- [ ] This is an example\n```\n- [ ] This is a task\n";
        bool fencedChanged = true;
        check(IssueStore::checklistTasks(fencedTask).size() == 1 &&
                  IssueStore::subIssueTitles(fencedTask) ==
                      QStringList{"This is a task"} &&
                  IssueStore::setChecklistTaskChecked(
                      fencedTask, 2, true, &fencedChanged) == fencedTask &&
                  !fencedChanged,
              "task-shaped text inside a fenced code block stays inert");
        bool taskChanged = false;
        const QString checkedChecklist = IssueStore::setChecklistTaskChecked(
            checklist, 1, true, &taskChanged);
        check(taskChanged && checkedChecklist.contains("- [x] Desktop theme packs") &&
                  IssueStore::setChecklistTaskChecked(
                      checkedChecklist, 1, true, &taskChanged) == checkedChecklist &&
                  !taskChanged,
              "checking a task edits only its source state and repeated clicks are no-ops");

        const QString reorderSource =
            "- [ ] Parent task\n"
            "  wrapped detail\n"
            "  - [ ] Nested task\n"
            "- [x] Second task\n"
            "- [ ] Third task\n";
        const QString reordered = IssueStore::reorderChecklistTask(
            reorderSource, 0, 3, true, &taskChanged);
        check(taskChanged && reordered ==
                  "- [x] Second task\n"
                  "- [ ] Parent task\n"
                  "  wrapped detail\n"
                  "  - [ ] Nested task\n"
                  "- [ ] Third task\n",
              "reordering a checklist task carries its wrapped and nested block");
        check(IssueStore::reorderChecklistTask(
                  reorderSource, 0, 2, true, &taskChanged) == reorderSource &&
                  !taskChanged,
              "reordering refuses ambiguous cross-indent drops");

        IssueStore subIssues(subIssueTmp.path(), QString(), &identity, "tester");
        QString subError;
        const int parent = subIssues.createIssue(
            "Marketplace extraction candidates", checklist, {}, QString(), 0,
            {}, {}, &subError);
        QList<int> children;
        check(parent == 1 &&
                  subIssues.createSubIssues(parent, &children, &subError),
              "createSubIssues creates children and updates their parent");
        check(children == (QList<int>{2, 3, 4}),
              "sub-issues receive consecutive durable issue numbers");

        const QList<Issue> linkedIssues = subIssues.loadAll();
        check(linkedIssues.size() == 4,
              "one parent and three checklist children load from disk");
        QString linkedParentBody;
        QStringList childBodies;
        QStringList childTitles;
        for (const Issue &linked : linkedIssues) {
            if (linked.number == parent) {
                const QString openId = linked.events.first().id;
                for (const IssueEvent &event : linked.events)
                    if (event.type == "edit" && event.target == openId)
                        linkedParentBody = event.body;
            } else {
                childTitles.append(linked.title);
                childBodies.append(linked.events.first().body);
            }
        }
        check(linkedParentBody.contains(
                  "- [ ] Desktop theme packs (sub-issue #2)") &&
                  linkedParentBody.contains(
                      "  - [ ] World outfit bundles (sub-issue #3)") &&
                  linkedParentBody.contains(
                      "[ ] Dashboard calendar widgets (sub-issue #4)"),
              "parent checklist links every created sub-issue in place");
        check(childTitles ==
                  (QStringList{"Desktop theme packs", "World outfit bundles",
                               "Dashboard calendar widgets"}) &&
                  childBodies ==
                  (QStringList{"Parent issue: #1", "Parent issue: #1",
                               "Parent issue: #1"}),
              "each child preserves the task title and links back to its parent");
        check(IssueStore::subIssueTitles(linkedParentBody).isEmpty(),
              "linked checklist tasks are not offered a second time");
        QList<int> repeated;
        check(subIssues.createSubIssues(parent, &repeated, &subError) &&
                  repeated.isEmpty() && subIssues.loadAll().size() == 4,
              "re-running sub-issue creation is idempotent");
        check(subGitOutput({"rev-list", "--count", "HEAD"}) == "2",
              "the parent rewrite and all children share one batch commit");

        int checkedChild = 0;
        check(subIssues.createSubIssue(parent, 3, &checkedChild, &subError) &&
                  checkedChild == 5,
              "a checked row can be converted by itself from the desktop hover action");
        const QList<Issue> afterSingle = subIssues.loadAll();
        QString afterSingleParentBody;
        for (const Issue &linked : afterSingle) {
            if (linked.number != parent)
                continue;
            const QString openId = linked.events.first().id;
            for (const IssueEvent &event : linked.events)
                if (event.type == "edit" && event.target == openId)
                    afterSingleParentBody = event.body;
        }
        check(afterSingle.size() == 5 &&
                  afterSingleParentBody.contains(
                      "- [x] Already packaged (sub-issue #5)") &&
                  subGitOutput({"rev-list", "--count", "HEAD"}) == "3",
              "single-row conversion keeps check state and commits one linked child");
    }

    // --- Full IssueStore round-trip in a throwaway git repo --------------
    QTemporaryDir tmp;
    if (tmp.isValid()) {
        auto git = [&](const QStringList &args) {
            QProcess p;
            p.start("git", QStringList{"-C", tmp.path()} + args);
            p.waitForFinished(8000);
        };
        auto gitOutput = [&](const QStringList &args) {
            QProcess p;
            p.start("git", QStringList{"-C", tmp.path()} + args);
            p.waitForFinished(8000);
            return p.readAllStandardOutput();
        };
        git({"init", "-q"});
        git({"config", "user.email", "test@forkmesh.local"});
        git({"config", "user.name", "tester"});

        IssueStore repo(tmp.path(), QString(), &identity, "tester");
        check(repo.canWrite(), "store reports the temp work tree as writable");

        QString err;
        const int n = repo.createIssue("Round trip", "Hello **body**",
                                       {"bug"}, "v1", 7, {}, {}, &err);
        check(n == 1, "createIssue returns the first issue number");
        const QString issueJsonPath =
            QDir(tmp.path()).filePath(
                QStringLiteral(".forkmesh/issues/open/1/issue-1.json"));
        check(QFileInfo::exists(issueJsonPath),
              "createIssue writes .forkmesh/issues/open/1/issue-1.json");
        check(!QFileInfo::exists(
                  QDir(tmp.path()).filePath(QStringLiteral("issues/1/issue.md"))),
              "createIssue does not write legacy issue.md");
        QList<Issue> loaded = repo.loadAll();
        check(loaded.size() == 1 && loaded.first().title == "Round trip" &&
                  loaded.first().labels.contains("bug") &&
                  loaded.first().milestone == "v1" &&
                  loaded.first().priority == 7,
              "issue loads back with title, label, milestone and priority");
        check(!loaded.isEmpty() && loaded.first().events.first().body == "Hello **body**",
              "open-event body round-trips from issue JSON");
        const QString legacyIssueDir =
            QDir(tmp.path()).filePath(QStringLiteral("issues/99"));
        QDir().mkpath(legacyIssueDir);
        QFile legacyIssue(QDir(legacyIssueDir).filePath(QStringLiteral("issue.md")));
        if (legacyIssue.open(QIODevice::WriteOnly | QIODevice::Truncate))
            legacyIssue.write("---\ntitle: Old format\nstatus: open\n---\n\nignored\n");
        legacyIssue.close();
        loaded = repo.loadAll();
        check(loaded.size() == 1 && loaded.first().number == n,
              "legacy issues/<n>/issue.md folders are ignored");
        const int second = repo.createIssue("Second after legacy", "body", {},
                                            QString(), 0, {}, {}, &err);
        check(second == 2, "legacy issue folders do not affect new issue numbers");

        // A pre-split .forkmesh/issues/<n>/ folder is migrated into the
        // status-named subfolder (and committed) the next time issues load on
        // the owning node.
        const QString preSplitDir =
            QDir(tmp.path()).filePath(QStringLiteral(".forkmesh/issues/77"));
        QDir().mkpath(preSplitDir);
        QFile preSplitJson(
            QDir(preSplitDir).filePath(QStringLiteral("issue-77.json")));
        if (preSplitJson.open(QIODevice::WriteOnly | QIODevice::Truncate))
            preSplitJson.write(
                "{\"schema\":\"forkmesh-issue-v1\",\"number\":77,"
                "\"title\":\"Legacy layout\",\"status\":\"open\","
                "\"events\":[{\"type\":\"open\",\"id\":\"open-77\","
                "\"author\":\"a\",\"ts\":1,\"title\":\"Legacy layout\","
                "\"body\":\"\",\"attachments\":[],\"sig\":\"s\"},"
                "{\"type\":\"status\",\"id\":\"s1\",\"author\":\"a\","
                "\"ts\":2,\"status\":\"closed\",\"sig\":\"s\"}]}");
        preSplitJson.close();
        loaded = repo.loadAll();
        check(QFileInfo::exists(QDir(tmp.path()).filePath(QStringLiteral(
                  ".forkmesh/issues/closed/77/issue-77.json"))) &&
                  !QDir(preSplitDir).exists(),
              "loadAll migrates a pre-split issue folder into closed/");
        check(std::any_of(loaded.begin(), loaded.end(),
                          [](const Issue &i) {
                              return i.number == 77 &&
                                     i.status == QStringLiteral("closed");
                          }),
              "migrated legacy issue still loads with its folded status");

        // Folder-authoritative status (adhoc #16): an issue still sitting in
        // open/ whose record carries a stale status->closed event must load as
        // OPEN, so the Issues-tab tally matches the open/ folder listing and the
        // served/advertised open count instead of drifting below it (the
        // 11-vs-7 skew the file browser exposes).
        const QString skewDir =
            QDir(tmp.path()).filePath(QStringLiteral(".forkmesh/issues/open/55"));
        QDir().mkpath(skewDir);
        QFile skewJson(QDir(skewDir).filePath(QStringLiteral("issue-55.json")));
        if (skewJson.open(QIODevice::WriteOnly | QIODevice::Truncate))
            skewJson.write(
                "{\"schema\":\"forkmesh-issue-v1\",\"number\":55,"
                "\"title\":\"Stale status\",\"status\":\"closed\","
                "\"events\":[{\"type\":\"open\",\"id\":\"open-55\","
                "\"author\":\"a\",\"ts\":1,\"title\":\"Stale status\","
                "\"body\":\"\",\"attachments\":[],\"sig\":\"s\"},"
                "{\"type\":\"status\",\"id\":\"s55\",\"author\":\"a\","
                "\"ts\":2,\"status\":\"closed\",\"sig\":\"s\"}]}");
        skewJson.close();
        loaded = repo.loadAll();
        check(std::any_of(loaded.begin(), loaded.end(),
                          [](const Issue &i) {
                              return i.number == 55 &&
                                     i.status == QStringLiteral("open");
                          }),
              "an issue in open/ with a stale status->closed record counts as open");

        // Deletion is shown, not hidden (adhoc #16). A delete/self event only
        // deletes the issue when its own creator signed it; a delete from anyone
        // else is an unauthorized attempt that leaves the issue open and counted
        // but flagged. Every issue is returned by loadAll either way — the UI
        // badges it — and the record is never rewritten.
        auto writeIssueWithDelete = [&](int number, const QString &creator,
                                        const QString &deleter) {
            const QString dir = QDir(tmp.path()).filePath(
                QStringLiteral(".forkmesh/issues/open/%1").arg(number));
            QDir().mkpath(dir);
            QFile f(QDir(dir).filePath(
                QStringLiteral("issue-%1.json").arg(number)));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QStringLiteral(
                            "{\"schema\":\"forkmesh-issue-v1\",\"number\":%1,"
                            "\"title\":\"t%1\",\"status\":\"open\","
                            "\"events\":[{\"type\":\"open\",\"id\":\"open-%1\","
                            "\"author\":\"%2\",\"ts\":1,\"title\":\"t%1\","
                            "\"body\":\"\",\"attachments\":[],\"sig\":\"s\"},"
                            "{\"type\":\"delete\",\"id\":\"d%1\",\"author\":\"%3\","
                            "\"ts\":2,\"target\":\"self\",\"sig\":\"s\"}]}")
                            .arg(QString::number(number), creator, deleter)
                            .toUtf8());
        };
        writeIssueWithDelete(60, "creatorA", "stranger");  // unauthorized attempt
        writeIssueWithDelete(61, "creatorB", "creatorB");  // creator self-delete
        loaded = repo.loadAll();
        const auto find60 = std::find_if(loaded.begin(), loaded.end(),
            [](const Issue &i) { return i.number == 60; });
        const auto find61 = std::find_if(loaded.begin(), loaded.end(),
            [](const Issue &i) { return i.number == 61; });
        check(find60 != loaded.end() && !find60->isDeleted() &&
                  find60->hasUnauthorizedDeleteAttempt(),
              "a non-author's delete is shown, flagged, and does not delete the issue");
        check(find61 != loaded.end() && find61->isDeleted() &&
                  !find61->hasUnauthorizedDeleteAttempt(),
              "a creator's own self-delete marks the issue deleted but still shows it");
        {
            // The record is left intact — the delete event is not stripped.
            QFile keptFile(QDir(tmp.path()).filePath(QStringLiteral(
                ".forkmesh/issues/open/60/issue-60.json")));
            bool hasDelete = false;
            if (keptFile.open(QIODevice::ReadOnly)) {
                const QJsonArray evs =
                    QJsonDocument::fromJson(keptFile.readAll())
                        .object().value("events").toArray();
                hasDelete = std::any_of(
                    evs.begin(), evs.end(), [](const QJsonValue &v) {
                        return v.toObject().value("type").toString() == "delete";
                    });
            }
            check(hasDelete,
                  "the unauthorized delete event is preserved, not stripped");
        }

        check(repo.addComment(n, "a comment", {}, &err), "addComment succeeds");
        QFile issueJson(issueJsonPath);
        const bool commentJsonOk =
            issueJson.open(QIODevice::ReadOnly) &&
            QJsonDocument::fromJson(issueJson.readAll())
                    .object()
                    .value("events")
                    .toArray()
                    .at(1)
                    .toObject()
                    .value("body")
                    .toString() == "a comment";
        issueJson.close();
        check(commentJsonOk, "comment body is stored inside the issue JSON");
        check(QDir(QDir(tmp.path())
                       .filePath(QStringLiteral(".forkmesh/issues/open/1")))
                  .entryList(QStringList{QStringLiteral("*.md")}, QDir::Files)
                  .isEmpty(),
              "issue folder contains no markdown event files");

        // Reactions. A bot's "seen it" on the comment that mentioned it and a
        // person's chip are the same signed event, and one node signs for every
        // model bot it speaks for -- so two display names on one key have to
        // count separately, or the bots would toggle each other's
        // acknowledgements off.
        {
            QList<Issue> reacting = repo.loadAll();
            const QString commentEventId =
                reacting.isEmpty() || reacting.first().events.size() < 2
                    ? QString()
                    : reacting.first().events.at(1).id;
            check(!commentEventId.isEmpty(), "the comment event has an id to react to");
            check(repo.setReaction(n, commentEventId, "eyes", true, &err),
                  "setReaction adds a reaction to a comment");
            check(repo.setReaction(n, commentEventId, "eyes", true, &err),
                  "setReaction is a no-op when the reaction is already on");
            check(!repo.setReaction(n, QString(), "eyes", true, &err),
                  "setReaction refuses an empty target");
            IssueStore bot(tmp.path(), QString(), &identity,
                           QStringLiteral("Sonnet 5 [bot]"));
            check(bot.setReaction(n, commentEventId, "eyes", true, &err),
                  "a bot on the same signing key reacts under its own name");
            reacting = repo.loadAll();
            bool signedReaction = false;
            for (const IssueEvent &e : reacting.first().events)
                if (e.type == QStringLiteral("reaction") &&
                    e.reaction == QStringLiteral("eyes") &&
                    e.authorName == QStringLiteral("Sonnet 5 [bot]"))
                    signedReaction =
                        verifyEd25519(e.author, e.sig,
                                      IssueStore::canonicalString(n, e));
            check(signedReaction,
                  "reaction event round-trips from issue JSON with a valid signature");
            QList<IssueReaction> tally = IssueStore::reactionsForEvent(
                reacting.first(), commentEventId, identity.publicKey(), "tester");
            check(tally.size() == 1 && tally.first().value == "eyes" &&
                      tally.first().names.size() == 2 && tally.first().mine,
                  "both names count under one slug, and the caller's own is marked");
            check(repo.toggleReaction(n, commentEventId, "eyes", &err),
                  "toggleReaction takes the caller's own reaction back");
            reacting = repo.loadAll();
            tally = IssueStore::reactionsForEvent(
                reacting.first(), commentEventId, identity.publicKey(), "tester");
            check(tally.size() == 1 &&
                      tally.first().names ==
                          QStringList{QStringLiteral("Sonnet 5 [bot]")} &&
                      !tally.first().mine,
                  "un-reacting leaves the bot's reaction on the same comment standing");
            check(repo.toggleReaction(n, commentEventId, "eyes", &err),
                  "toggleReaction puts the caller's reaction back");
            reacting = repo.loadAll();
            check(IssueStore::reactionsForEvent(reacting.first(), commentEventId,
                                                identity.publicKey(), "tester")
                          .first()
                          .names.size() == 2,
                  "a reaction taken back can be added again");
            check(IssueStore::reactionsForEvent(reacting.first(),
                                                QStringLiteral("no-such-event"),
                                                identity.publicKey(), "tester")
                      .isEmpty(),
                  "reactions are scoped to the event they name");
        }

        check(repo.setStatus(n, "closed", &err), "setStatus succeeds");
        check(QFileInfo::exists(QDir(tmp.path()).filePath(QStringLiteral(
                  ".forkmesh/issues/closed/1/issue-1.json"))) &&
                  !QDir(QDir(tmp.path())
                            .filePath(QStringLiteral(".forkmesh/issues/open/1")))
                       .exists(),
              "closing an issue moves its folder from open/ to closed/");
        check(repo.setPriority(n, 8999, &err),
              "setPriority accepts the expanded lowest rank");
        check(!repo.setPriority(n, 9000, &err),
              "setPriority rejects ranks beyond 8999");
        check(repo.setPriority(n, 3, &err), "setPriority succeeds");
        check(repo.assignAgent(n, "codex", 42, true, "queued", &err),
              "assignAgent succeeds");
        check(repo.assignAgent(n, QString(), 0, false, "cleared", &err),
              "assignAgent clears the issue agent");
        loaded = repo.loadAll();
        check(!loaded.isEmpty() && loaded.first().priority == 3,
              "priority event folds into issue metadata");
        bool sawComment = false;
        bool sawAgentAssign = false;
        bool sawAgentClear = false;
        for (const IssueEvent &e : loaded.first().events)
            if (e.type == "comment" && e.body == "a comment")
                sawComment = true;
            else if (e.type == "agent" && e.agentProvider == "codex" &&
                     e.agentSessionId == 42 && e.agentCreatePr &&
                     e.agentStatus == "queued")
                sawAgentAssign = true;
            else if (e.type == "agent" && e.agentProvider.isEmpty() &&
                     e.agentSessionId == 0 && !e.agentCreatePr &&
                     e.agentStatus == "cleared")
                sawAgentClear = true;
        check(sawComment, "comment body round-trips from issue JSON");
        check(sawAgentAssign, "agent assignment event round-trips");
        check(sawAgentClear, "agent clear event round-trips");
        check(loaded.first().status == "closed", "status reflects close event");

        // Deleting a comment appends a signed "delete" event targeting the
        // comment's id; the comment event itself is left in place (folding it
        // out of the UI is a render-time concern) so history is preserved.
        const QString commentId = loaded.first().events.at(1).id;
        check(repo.deleteEvent(n, commentId, &err), "deleteEvent succeeds");
        const QList<Issue> afterCommentDelete = repo.loadAll();
        const Issue &withDeletedComment = afterCommentDelete.first();
        check(withDeletedComment.number == n,
              "issue with a deleted comment still loads");
        bool sawDeleteEvent = false;
        for (const IssueEvent &e : withDeletedComment.events)
            if (e.type == "delete" && e.target == commentId)
                sawDeleteEvent = true;
        check(sawDeleteEvent,
              "delete event targeting the comment id round-trips from issue JSON");
        check(!withDeletedComment.isDeleted(),
              "deleting a single comment does not tombstone the whole issue");

        // Re-crediting a report (the desktop's Reporter sidebar) appends a
        // signed "reporter" event. It moves the displayed credit only: the
        // opening event keeps the name and the key it was signed under, which
        // is what the reporter leaderboard reads for provenance.
        check(repo.setReporter(n, QStringLiteral("  sammy  "), &err),
              "setReporter succeeds and trims the name");
        check(!repo.setReporter(n, QStringLiteral("   "), &err),
              "setReporter refuses an empty name");
        const QList<Issue> recredited = repo.loadAll();
        const auto creditedIt =
            std::find_if(recredited.begin(), recredited.end(),
                         [&](const Issue &i) { return i.number == n; });
        check(creditedIt != recredited.end() &&
                  creditedIt->authorName == QStringLiteral("sammy"),
              "reporter event folds into the issue's displayed author name");
        check(creditedIt != recredited.end() && !creditedIt->events.isEmpty() &&
                  creditedIt->events.first().type == QStringLiteral("open") &&
                  creditedIt->events.first().authorName == QStringLiteral("tester"),
              "re-crediting leaves the signed open event's author name intact");
        bool sawSignedReporter = false;
        if (creditedIt != recredited.end())
            for (const IssueEvent &e : creditedIt->events)
                if (e.type == QStringLiteral("reporter") &&
                    e.reporter == QStringLiteral("sammy"))
                    sawSignedReporter =
                        verifyEd25519(e.author, e.sig,
                                      IssueStore::canonicalString(n, e));
        check(sawSignedReporter,
              "reporter event round-trips from issue JSON with a valid signature");

        // A creator's signed self-tombstone marks the issue deleted but loadAll
        // still returns it (adhoc #16 — shown with a Deleted badge, not hidden),
        // and its history stays intact in git.
        const int tomb = repo.createIssue("Tombstone me", "body", {}, QString(),
                                          0, {}, {}, &err);
        check(repo.tombstoneIssue(tomb, &err), "tombstoneIssue succeeds");
        const QList<Issue> afterTombstone = repo.loadAll();
        const auto tombIt =
            std::find_if(afterTombstone.begin(), afterTombstone.end(),
                         [&](const Issue &i) { return i.number == tomb; });
        check(tombIt != afterTombstone.end() && tombIt->isDeleted(),
              "a self-tombstoned issue still loads, flagged as deleted");
        check(!gitOutput({"log", "--all", "--",
                          QStringLiteral(".forkmesh/issues/open/%1").arg(tomb)})
                   .trimmed()
                   .isEmpty(),
              "tombstoned issue is preserved in git history");

        // The owning desktop's regular delete removes the issue from the
        // current tree instead of appending a creator-only tombstone. This must
        // work for issues opened by a browser or an older signing key while
        // keeping the prior record recoverable from git history.
        check(repo.removeIssue(60, &err),
              "owner regular delete accepts an issue from another signing key");
        const QList<Issue> afterOwnerDelete = repo.loadAll();
        check(std::none_of(afterOwnerDelete.begin(), afterOwnerDelete.end(),
                           [](const Issue &i) { return i.number == 60; }),
              "owner regular delete removes the issue from the current tree");
        check(!gitOutput({"log", "--all", "--",
                          QStringLiteral(".forkmesh/issues/open/60")})
                   .trimmed()
                   .isEmpty(),
              "owner regular delete preserves the issue in git history");

        check(repo.deleteIssue(n, &err), "deleteIssue succeeds");
        const QList<Issue> afterDelete = repo.loadAll();
        check(std::none_of(afterDelete.begin(), afterDelete.end(),
                           [&](const Issue &i) { return i.number == n; }),
              "deleted issue no longer loads");
        // The issue lived at open/<n> and then closed/<n>; the purge must strip
        // every location (including the pre-split legacy path) from history.
        check(gitOutput({"log", "--all", "--",
                         QStringLiteral(".forkmesh/issues/%1").arg(n),
                         QStringLiteral(".forkmesh/issues/open/%1").arg(n),
                         QStringLiteral(".forkmesh/issues/closed/%1").arg(n)})
                  .trimmed()
                  .isEmpty(),
              "deleted issue is purged from git history");

        // A mirror node files an issue: it arrives as a signed "open" event with
        // a proposed number that collides with an existing issue. applyRemoteEvent
        // must reassign a fresh number, keep the submitter as author, and apply
        // the vouched issue-level metadata that rode along with the submission.
        const int base = repo.createIssue("Base", "b", {}, QString(), 0, {}, {}, &err);
        IssueEvent remoteOpen;
        remoteOpen.type = "open";
        remoteOpen.id = QStringLiteral("open-%1").arg(base); // collides with base
        remoteOpen.title = "From a mirror";
        remoteOpen.body = "filed remotely";
        remoteOpen = repo.makeSignedEvent(base, remoteOpen);
        remoteOpen.authorName = "mirrornode";
        RemoteIssueMeta meta;
        meta.labels = QStringList{"enhancement"};
        meta.priority = 9;
        meta.reporter = "alice";
        meta.rewardEstimateLamports = 3000000;
        meta.rewardRecipient = "alice";
        meta.rewardId = "reward-discord-a1b2c3d4e5f6";
        meta.rewardCondition = "accepted_report";
        meta.rewardStatus = "pending_wallet";
        check(repo.applyRemoteEvent(base, remoteOpen, "From a mirror", &err, meta),
              "applyRemoteEvent accepts a mirror-authored open event");
        const QList<Issue> afterRemote = repo.loadAll();
        const Issue *mirrored = nullptr;
        for (const Issue &i : afterRemote)
            if (i.title == "From a mirror")
                mirrored = &i;
        check(mirrored && mirrored->number != base,
              "colliding remote issue is reassigned a fresh number");
        check(mirrored && mirrored->authorName == "alice",
              "remote issue credits its vouched human reporter");
        check(mirrored && !mirrored->events.isEmpty() &&
                  mirrored->events.first().authorName == "mirrornode",
              "reporter credit leaves the signed open-event author intact");
        check(mirrored && mirrored->labels.contains("enhancement") &&
                  mirrored->priority == 9,
              "remote issue applies vouched metadata (labels + priority)");
        check(mirrored && mirrored->rewardEstimateLamports == 3000000 &&
                  mirrored->rewardRecipient == "alice" &&
                  mirrored->rewardId == "reward-discord-a1b2c3d4e5f6" &&
                  mirrored->rewardCondition == "accepted_report" &&
                  mirrored->rewardStatus == "pending_wallet",
              "remote issue preserves a vouched reporting reward estimate");

        // Re-syncing the same submission (same signature) must be idempotent:
        // the inbox can redeliver before the owner acknowledges it.
        const int before = repo.loadAll().size();
        check(repo.applyRemoteEvent(base, remoteOpen, "From a mirror", &err, meta),
              "re-applying the same remote open event succeeds");
        check(repo.loadAll().size() == before,
              "re-syncing a merged issue does not duplicate it");

        const int unknownIssue = 7290;
        IssueEvent orphanClaim;
        orphanClaim.type = "claim";
        orphanClaim.id = "claim-web-orphan";
        orphanClaim.author = "WEBPUB";
        orphanClaim.authorName = "bob";
        orphanClaim.assignee = "bob";
        orphanClaim.claimState = "released";
        orphanClaim.ts = 1790617567277LL;
        QString orphanError;
        check(!repo.applyRemoteEvent(unknownIssue, orphanClaim, QString(),
                                     &orphanError) &&
                  !orphanError.isEmpty(),
              "an event for an issue this node never opened waits for its opening");
        check(!QDir(IssueStore::issueDirPath(tmp.path(), unknownIssue)).exists(),
              "an event for an unknown issue does not fabricate an opening event");

        for (const QString &fakeSig :
             {QString(), QStringLiteral("relay-placeholder")}) {
            const int slot = unknownIssue + (fakeSig.isEmpty() ? 1 : 2);
            IssueEvent fakeOpen;
            fakeOpen.type = "open";
            fakeOpen.id = QStringLiteral("open-%1").arg(slot);
            fakeOpen.author = "WEBPUB";
            fakeOpen.authorName = "bob";
            fakeOpen.sig = fakeSig;
            fakeOpen.ts = orphanClaim.ts;
            IssueEvent heldClaim = orphanClaim;
            heldClaim.id = QStringLiteral("claim-web-held-%1").arg(slot);
            heldClaim.claimState = "assigned";
            Issue legacy;
            legacy.number = slot;
            legacy.status = "open";
            legacy.createdAt = orphanClaim.ts;
            legacy.author = "WEBPUB";
            legacy.authorName = "bob";
            legacy.events = {fakeOpen, heldClaim};
            const QString legacyPath =
                IssueStore::issueDirPath(tmp.path(), slot) +
                QStringLiteral("/issue-%1.json").arg(slot);
            QDir().mkpath(QFileInfo(legacyPath).absolutePath());
            QFile legacyFile(legacyPath);
            check(legacyFile.open(QIODevice::WriteOnly) &&
                      legacyFile.write(QJsonDocument(legacy.toJson()).toJson()) > 0,
                  "a legacy placeholder issue file is written");
            legacyFile.close();
            const int countWithPlaceholder = repo.loadAll().size();
            IssueEvent realOpen;
            realOpen.type = "open";
            realOpen.id = QStringLiteral("open-%1").arg(slot);
            realOpen.title = "The real report";
            realOpen.body = "signed opening";
            realOpen = repo.makeSignedEvent(slot, realOpen);
            check(repo.applyRemoteEvent(slot, realOpen, realOpen.title, &err),
                  "the signed opening lands on an issue that only had a placeholder");
            const QList<Issue> healed = repo.loadAll();
            const auto found = std::find_if(
                healed.begin(), healed.end(),
                [slot](const Issue &i) { return i.number == slot; });
            check(healed.size() == countWithPlaceholder && found != healed.end() &&
                      found->title == "The real report" &&
                      found->events.size() == 2 &&
                      found->events.first().sig == realOpen.sig &&
                      found->assignees == QStringList{"bob"},
                  fakeSig.isEmpty()
                      ? "the signed opening replaces an unsigned placeholder in "
                        "place and keeps the later events"
                      : "the signed opening replaces a relay-placeholder in "
                        "place and keeps the later events");
        }

        // The web app's Delete issue button (issue #582) reaches the owner node
        // as a relay-authorized delete/self event. The relay only accepts one
        // from a verified repository manager, so it removes the issue from the
        // tree the way the desktop's own Delete does -- it is not a creator
        // tombstone, and it must work for an issue this key never opened.
        const int webDeleted =
            repo.createIssue("Delete me from the web", "body", {}, QString(), 0,
                             {}, {}, &err);
        IssueEvent remoteDelete;
        remoteDelete.type = "delete";
        remoteDelete.target = "self";
        remoteDelete = repo.makeSignedEvent(webDeleted, remoteDelete);
        remoteDelete.id = QStringLiteral("delete-web-%1").arg(webDeleted);
        remoteDelete.author = QStringLiteral("browser-key-that-never-opened-it");
        remoteDelete.authorName = "owner-in-a-browser";
        check(repo.applyRemoteEvent(webDeleted, remoteDelete, QString(), &err),
              "applyRemoteEvent accepts an owner's browser delete");
        const QList<Issue> afterWebDelete = repo.loadAll();
        check(std::none_of(afterWebDelete.begin(), afterWebDelete.end(),
                           [&](const Issue &i) { return i.number == webDeleted; }),
              "a web delete removes the issue instead of flagging an "
              "unauthorized tombstone");
        check(!gitOutput({"log", "--all", "--",
                          QStringLiteral(".forkmesh/issues/open/%1")
                              .arg(webDeleted)})
                   .trimmed()
                   .isEmpty(),
              "a web delete preserves the issue in git history");
        check(repo.applyRemoteEvent(webDeleted, remoteDelete, QString(), &err),
              "redelivering a web delete is idempotent so the inbox row drains");

        // A delete targeting a comment stays append-only even from the inbox:
        // only the whole-issue delete above removes anything.
        const int keptIssue = repo.createIssue("Keep me", "body", {}, QString(),
                                               0, {}, {}, &err);
        IssueEvent remoteCommentDelete;
        remoteCommentDelete.type = "delete";
        remoteCommentDelete.target = QStringLiteral("comment-web-1");
        remoteCommentDelete = repo.makeSignedEvent(keptIssue, remoteCommentDelete);
        remoteCommentDelete.id = QStringLiteral("delete-web-comment");
        check(repo.applyRemoteEvent(keptIssue, remoteCommentDelete, QString(), &err),
              "applyRemoteEvent accepts a remote comment delete");
        const QList<Issue> afterCommentTarget = repo.loadAll();
        check(std::any_of(afterCommentTarget.begin(), afterCommentTarget.end(),
                          [&](const Issue &i) { return i.number == keptIssue; }),
              "deleting a comment from the web leaves the issue in place");

        // Settle any untracked state the IssueStore section above left behind
        // (e.g. its issues/ folder) so the PullStore tests below start from a
        // clean tree — beginPullBranch (agent edit / file edit / conflict
        // resolve) requires one, and pulls/ commits no longer land in this
        // working tree at all (issue #399) to incidentally sweep it up.
        git({"add", "-A"});
        git({"commit", "-q", "-m", "test: settle pre-pull-tests state",
            "--allow-empty"});

        // Older repositories can still carry PR records on the code branch
        // after current activity moved to forkmesh/pulls. The ordinary load is
        // intentionally lightweight/current-only; All unions both locations,
        // and Restore makes an archived record a normal ledger record again.
        {
            QTemporaryDir archivedRoot;
            const QString archivedRepo = archivedRoot.path();
            const auto pullMarkdown = [](int number, const QString &title) {
                return QStringLiteral(
                           "---\nschema: forkmesh-pull-v1\nnumber: %1\n"
                           "title: %2\nbase: main\nhead: archived/%1\n"
                           "status: open\nts: 1\nauthor: archived-author\n"
                           "sig: archived-signature\n---\nbody\n")
                    .arg(number)
                    .arg(title)
                    .toUtf8();
            };
            bool archivedSetup =
                archivedRoot.isValid() &&
                runTestGit(archivedRepo,
                           {QStringLiteral("init"), QStringLiteral("-q"),
                            QStringLiteral("-b"), QStringLiteral("main")}) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("config"), QStringLiteral("user.name"),
                            QStringLiteral("Archived Pull Tester")}) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("config"), QStringLiteral("user.email"),
                            QStringLiteral("archived@example.test")}) &&
                writeTestFile(archivedRepo +
                                  QStringLiteral("/.forkmesh/pulls/70/pull.md"),
                              pullMarkdown(70, QStringLiteral("Archived pull"))) &&
                writeTestFile(
                    archivedRepo +
                        QStringLiteral("/.forkmesh/pulls/70/changes.patch"),
                    QByteArrayLiteral("diff --git a/old b/old\n")) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("add"), QStringLiteral(".forkmesh")}) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("commit"), QStringLiteral("-q"),
                            QStringLiteral("-m"),
                            QStringLiteral("archived pull on main")}) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("checkout"), QStringLiteral("-q"),
                            QStringLiteral("-b"),
                            QStringLiteral("forkmesh/pulls")}) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("rm"), QStringLiteral("-q"),
                            QStringLiteral("-r"),
                            QStringLiteral(".forkmesh/pulls/70")}) &&
                writeTestFile(archivedRepo +
                                  QStringLiteral("/.forkmesh/pulls/2/pull.md"),
                              pullMarkdown(2, QStringLiteral("Current pull"))) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("add"), QStringLiteral(".forkmesh")}) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("commit"), QStringLiteral("-q"),
                            QStringLiteral("-m"),
                            QStringLiteral("current pull ledger")}) &&
                runTestGit(archivedRepo,
                           {QStringLiteral("checkout"), QStringLiteral("-q"),
                            QStringLiteral("main")});
            check(archivedSetup, "set up code-branch pull archive fixture");
            if (archivedSetup) {
                PullStore archivedStore(archivedRepo, QString(), &identity,
                                        QStringLiteral("tester"));
                QString archivedError;
                const QList<PullRequest> current = archivedStore.loadAll();
                QList<PullRequest> all =
                    archivedStore.loadAllIncludingArchived(&archivedError);
                const auto archivedRecord = std::find_if(
                    all.cbegin(), all.cend(), [](const PullRequest &pull) {
                        return pull.number == 70 && pull.archivedOnCodeBranch;
                    });
                check(current.size() == 1 && current.first().number == 2 &&
                          all.size() == 2 && archivedRecord != all.cend() &&
                          archivedError.isEmpty(),
                      "All loads current and archived pull records without "
                      "duplicating the ledger");
                const int afterArchive = archivedStore.createPull(
                    QStringLiteral("After archived pull"), QStringLiteral("body"),
                    QStringLiteral("main"), QStringLiteral("after-archive"),
                    QStringLiteral("diff --git a/a b/a\n"), QString(),
                    /*branchBacked=*/false, &archivedError);
                check(afterArchive == 71,
                      "an archived PR reserves its number when the next PR opens");
                check(archivedStore.restoreArchivedPull(70, &archivedError),
                      "Restore copies a code-branch pull into the live ledger");
                const QList<PullRequest> restored = archivedStore.loadAll();
                const auto liveRecord = std::find_if(
                    restored.cbegin(), restored.cend(),
                    [](const PullRequest &pull) {
                        return pull.number == 70 &&
                               !pull.archivedOnCodeBranch;
                    });
                check(restored.size() == 3 && liveRecord != restored.cend(),
                      "a restored pull is manageable through the current ledger");
            }
        }

        {
            QTemporaryDir sourceRoot;
            QTemporaryDir mirrorRoot;
            const QString repo = sourceRoot.path();
            const QString mirror = mirrorRoot.path() + QStringLiteral("/mirror.git");
            const QString author = QStringLiteral("Merge Source Tester");
            const QString email = QStringLiteral("merge-source@example.test");
            const auto git = [&](const QStringList &args, QByteArray *out = nullptr) {
                return runTestGit(repo, args, out);
            };
            const auto tip = [&](const QString &ref) {
                QByteArray out;
                return git({QStringLiteral("rev-parse"), QStringLiteral("-q"),
                            QStringLiteral("--verify"), ref + QStringLiteral("^{commit}")},
                           &out)
                           ? QString::fromUtf8(out).trimmed()
                           : QString();
            };
            const auto commitFile = [&](const QString &file, const QByteArray &body,
                                        const QString &when) {
                return writeTestFile(repo + QLatin1Char('/') + file, body) &&
                       commitTestTree(repo, file, when, author, email);
            };
            bool setup =
                sourceRoot.isValid() && mirrorRoot.isValid() &&
                git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("main")}) &&
                git({QStringLiteral("config"), QStringLiteral("user.name"), author}) &&
                git({QStringLiteral("config"), QStringLiteral("user.email"), email}) &&
                commitFile(QStringLiteral("base.txt"), "base\n",
                           QStringLiteral("2026-09-01T10:00:00Z")) &&
                git({QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-b"),
                     QStringLiteral("ui/page-kit-buttons-favorit")}) &&
                commitFile(QStringLiteral("mirror.txt"), "mirror\n",
                           QStringLiteral("2026-09-01T11:00:00Z")) &&
                git({QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("main")});
            const QString mirrorTip = tip(QStringLiteral("ui/page-kit-buttons-favorit"));
            QProcess clone;
            clone.start(QStringLiteral("git"),
                        {QStringLiteral("clone"), QStringLiteral("--bare"), QStringLiteral("-q"),
                         repo, mirror});
            setup = setup && !mirrorTip.isEmpty() && clone.waitForFinished(30000) &&
                    clone.exitCode() == 0 &&
                    git({QStringLiteral("branch"), QStringLiteral("-D"), QStringLiteral("-q"),
                         QStringLiteral("ui/page-kit-buttons-favorit")});

            const QString pullHead = QStringLiteral("ui/landing-pricing-downloads");
            QByteArray patch;
            QByteArray mbox;
            setup = setup &&
                    git({QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-b"),
                         pullHead}) &&
                    commitFile(QStringLiteral("pricing.txt"), "pricing\n",
                               QStringLiteral("2026-09-01T12:00:00Z")) &&
                    git({QStringLiteral("diff"), QStringLiteral("--binary"),
                         QStringLiteral("main...") + pullHead},
                        &patch) &&
                    git({QStringLiteral("format-patch"), QStringLiteral("--binary"),
                         QStringLiteral("--stdout"), QStringLiteral("main..") + pullHead},
                        &mbox) &&
                    git({QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("main")}) &&
                    git({QStringLiteral("branch"), QStringLiteral("-D"), QStringLiteral("-q"),
                         pullHead});
            check(setup, "set up mirror-only and patch-only merge source fixtures");

            if (setup) {
                PullStore sources(repo, mirror, &identity, QStringLiteral("tester"));
                QString sourceError;
                const int pullNumber = sources.createPull(
                    QStringLiteral("Landing pricing"), QStringLiteral("body"),
                    QStringLiteral("main"), pullHead, QString::fromUtf8(patch),
                    QString::fromUtf8(mbox), false, &sourceError);
                const QString numberedRef = QStringLiteral("refs/heads/pr/%1").arg(pullNumber);
                const QString prHeadRef = QStringLiteral("refs/pr/%1/head").arg(pullNumber);
                check(pullNumber > 0 &&
                          git({QStringLiteral("update-ref"), QStringLiteral("-d"), numberedRef}) &&
                          (tip(prHeadRef).isEmpty() ||
                           git({QStringLiteral("update-ref"), QStringLiteral("-d"), prHeadRef})) &&
                          tip(QStringLiteral("refs/heads/") + pullHead).isEmpty(),
                      "a patch-only pull request leaves no branch named after its head");

                int fromPull = -1;
                sourceError.clear();
                check(sources.ensureLocalBranch(QStringLiteral("main"), &fromPull,
                                                &sourceError) ==
                              PullStore::BranchSource::Local &&
                          fromPull == 0 && sourceError.isEmpty(),
                      "a local branch is merged as it stands");

                check(sources.ensureLocalBranch(QStringLiteral("ui/page-kit-buttons-favorit"),
                                                &fromPull, &sourceError) ==
                              PullStore::BranchSource::Mirror &&
                          tip(QStringLiteral("refs/heads/ui/page-kit-buttons-favorit")) ==
                              mirrorTip,
                      "a branch that only exists on the mirror is fetched into a local "
                      "branch before the merge");

                check(sources.ensureLocalBranch(pullHead, &fromPull, &sourceError) ==
                              PullStore::BranchSource::Pull &&
                          fromPull == pullNumber &&
                          !tip(QStringLiteral("refs/heads/") + pullHead).isEmpty(),
                      "a patch-only pull request's head is rebuilt from its commits.mbox");
                QByteArray pricing;
                check(git({QStringLiteral("show"), pullHead + QStringLiteral(":pricing.txt")},
                          &pricing) &&
                          pricing == "pricing\n",
                      "the rebuilt branch carries the pull request's change");
                check(git({QStringLiteral("merge"), QStringLiteral("--no-ff"),
                           QStringLiteral("-q"), pullHead, QStringLiteral("-m"),
                           QStringLiteral("Merge landing pricing")}) &&
                          QFile::exists(repo + QStringLiteral("/pricing.txt")),
                      "merging the rebuilt branch by name no longer fails with "
                      "\"not something we can merge\"");

                sourceError.clear();
                check(sources.ensureLocalBranch(QStringLiteral("446-add-button-stetings-admin"),
                                                &fromPull, &sourceError) ==
                              PullStore::BranchSource::Missing &&
                          fromPull == 0 &&
                          sourceError.contains(QStringLiteral("446-add-button-stetings-admin")) &&
                          sourceError.contains(QStringLiteral("nothing to merge")),
                      "a branch found nowhere gets a clear message instead of Git's");

                const QString mergedHead = QStringLiteral("ui/merged-already");
                const int mergedNumber = sources.createPull(
                    QStringLiteral("Merged already"), QStringLiteral("body"),
                    QStringLiteral("main"), mergedHead, QString::fromUtf8(patch),
                    QString::fromUtf8(mbox), false, &sourceError);
                sourceError.clear();
                check(mergedNumber > 0 &&
                          sources.setStatus(mergedNumber, QStringLiteral("merged"),
                                            &sourceError) &&
                          sources.ensureLocalBranch(mergedHead, &fromPull, &sourceError) ==
                              PullStore::BranchSource::Missing &&
                          sourceError.contains(
                              QStringLiteral("#%1").arg(mergedNumber)) &&
                          sourceError.contains(QStringLiteral("already merged")) &&
                          tip(QStringLiteral("refs/heads/") + mergedHead).isEmpty(),
                      "a pull request that already merged is not replayed a second time");

                sourceError.clear();
                check(sources.ensureLocalBranch(QStringLiteral("-bad"), &fromPull,
                                                &sourceError) ==
                              PullStore::BranchSource::Missing &&
                          sourceError.contains(QStringLiteral("not a valid branch name")),
                      "an invalid branch name is refused before Git sees it");
            }
        }

        // --- PullStore materializes durable, visible per-PR branches ------
        // Every PR gets refs/heads/pr/<n> in addition to the historical
        // refs/pr/<n>/head compatibility ref. Re-materialization is idempotent,
        // while a branch whose tip diverged is never force-overwritten.
        {
            QTemporaryDir branchRoot;
            const QString branchRepo = branchRoot.path();
            bool branchSetup =
                branchRoot.isValid() &&
                runTestGit(branchRepo,
                           {QStringLiteral("init"), QStringLiteral("-q"),
                            QStringLiteral("-b"), QStringLiteral("main")}) &&
                runTestGit(branchRepo,
                           {QStringLiteral("config"), QStringLiteral("user.name"),
                            QStringLiteral("PR Branch Tester")}) &&
                runTestGit(branchRepo,
                           {QStringLiteral("config"), QStringLiteral("user.email"),
                            QStringLiteral("pr-branch@example.test")}) &&
                writeTestFile(branchRepo + QStringLiteral("/base.txt"),
                              QByteArrayLiteral("base\n")) &&
                commitTestTree(branchRepo, QStringLiteral("base"),
                               QStringLiteral("2026-08-01T10:00:00Z"),
                               QStringLiteral("PR Branch Tester"),
                               QStringLiteral("pr-branch@example.test")) &&
                runTestGit(branchRepo,
                           {QStringLiteral("checkout"), QStringLiteral("-q"),
                            QStringLiteral("-b"), QStringLiteral("feature-pr")}) &&
                writeTestFile(branchRepo + QStringLiteral("/feature.txt"),
                              QByteArrayLiteral("feature\n")) &&
                commitTestTree(branchRepo, QStringLiteral("feature"),
                               QStringLiteral("2026-08-01T11:00:00Z"),
                               QStringLiteral("PR Branch Tester"),
                               QStringLiteral("pr-branch@example.test"));
            const QString featureTip = testGitHead(branchRepo);
            branchSetup = branchSetup && !featureTip.isEmpty() &&
                          runTestGit(branchRepo,
                                     {QStringLiteral("branch"),
                                      QStringLiteral("feature-pr-idempotent"),
                                      featureTip}) &&
                          runTestGit(branchRepo,
                                     {QStringLiteral("branch"),
                                      QStringLiteral("feature-pr-divergent"),
                                      featureTip}) &&
                          runTestGit(branchRepo,
                                     {QStringLiteral("checkout"),
                                      QStringLiteral("-q"),
                                      QStringLiteral("main")}) &&
                          runTestGit(branchRepo,
                                     {QStringLiteral("checkout"),
                                      QStringLiteral("-q"),
                                      QStringLiteral("-b"),
                                      QStringLiteral("independent-pr-work")}) &&
                          writeTestFile(branchRepo +
                                            QStringLiteral("/independent.txt"),
                                        QByteArrayLiteral("independent\n")) &&
                          commitTestTree(
                              branchRepo, QStringLiteral("independent"),
                              QStringLiteral("2026-08-01T12:00:00Z"),
                              QStringLiteral("PR Branch Tester"),
                              QStringLiteral("pr-branch@example.test"));
            const QString divergentTip = testGitHead(branchRepo);
            branchSetup = branchSetup && !divergentTip.isEmpty() &&
                          runTestGit(branchRepo,
                                     {QStringLiteral("checkout"),
                                      QStringLiteral("-q"),
                                      QStringLiteral("main")});
            check(branchSetup, "set up durable per-PR branch fixtures");

            auto refTip = [&](const QString &ref) {
                QByteArray output;
                return runTestGit(branchRepo,
                                  {QStringLiteral("rev-parse"),
                                   QStringLiteral("--verify"), ref},
                                  &output)
                           ? QString::fromUtf8(output).trimmed()
                           : QString();
            };
            if (branchSetup) {
                PullStore branchPulls(branchRepo, QString(), &identity,
                                      QStringLiteral("tester"));
                QString branchError;
                const int created = branchPulls.createPull(
                    QStringLiteral("Visible branch"), QStringLiteral("body"),
                    QStringLiteral("main"), QStringLiteral("feature-pr"),
                    QString(), QString(), /*branchBacked=*/true, &branchError);
                check(created == 1 && branchError.isEmpty() &&
                          refTip(QStringLiteral("refs/heads/pr/1")) == featureTip &&
                          refTip(QStringLiteral("refs/pr/1/head")) == featureTip &&
                          !refTip(QStringLiteral("refs/pr/1/metadata")).isEmpty() &&
                          refTip(QStringLiteral("refs/heads/forkmesh/pull/1")) ==
                              refTip(QStringLiteral("refs/pr/1/metadata")),
                      "materializing a PR creates code and metadata refs");

                QByteArray isolatedTree;
                check(runTestGit(
                          branchRepo,
                          {QStringLiteral("ls-tree"), QStringLiteral("-r"),
                           QStringLiteral("--name-only"),
                           QStringLiteral("refs/heads/forkmesh/pull/1")},
                          &isolatedTree) &&
                          QString::fromUtf8(isolatedTree).contains(
                              QStringLiteral(".forkmesh/pulls/1/pull.md")) &&
                          !QString::fromUtf8(isolatedTree).contains(
                              QStringLiteral(".forkmesh/pulls/2/")),
                      "each PR metadata branch contains only its own record");

                const QString metadataBefore =
                    refTip(QStringLiteral("refs/pr/1/metadata"));
                check(branchPulls.addComment(
                          1, QStringLiteral("metadata belongs to this PR"),
                          &branchError) &&
                          refTip(QStringLiteral("refs/pr/1/metadata")) !=
                              metadataBefore &&
                          refTip(QStringLiteral("refs/pr/1/head")) == featureTip &&
                          refTip(QStringLiteral("refs/heads/pr/1")) == featureTip,
                      "PR conversation advances only its metadata ref");

                check(runTestGit(branchRepo,
                                 {QStringLiteral("update-ref"),
                                  QStringLiteral("refs/heads/pr/2"), featureTip}),
                      "pre-create an idempotent PR branch at the desired tip");
                const int idempotent = branchPulls.createPull(
                    QStringLiteral("Idempotent branch"), QStringLiteral("body"),
                    QStringLiteral("main"),
                    QStringLiteral("feature-pr-idempotent"), QString(), QString(),
                    /*branchBacked=*/true, &branchError);
                check(idempotent == 2 &&
                          refTip(QStringLiteral("refs/heads/pr/2")) == featureTip &&
                          refTip(QStringLiteral("refs/pr/2/head")) == featureTip &&
                          refTip(QStringLiteral("refs/heads/forkmesh/pull/2")) ==
                              refTip(QStringLiteral("refs/pr/2/metadata")),
                      "re-materializing the same PR tip is idempotent");

                const QString pullTwoMetadata =
                    refTip(QStringLiteral("refs/heads/forkmesh/pull/2"));
                check(branchPulls.addComment(
                          1, QStringLiteral("only pull one advances"),
                          &branchError) &&
                          refTip(QStringLiteral("refs/heads/forkmesh/pull/1")) !=
                              metadataBefore &&
                          refTip(QStringLiteral("refs/heads/forkmesh/pull/2")) ==
                              pullTwoMetadata,
                      "updating one PR cannot advance another PR metadata branch");

                check(runTestGit(branchRepo,
                                 {QStringLiteral("update-ref"),
                                  QStringLiteral("refs/heads/pr/3"), divergentTip}),
                      "pre-create a divergent visible PR branch");
                const int divergent = branchPulls.createPull(
                    QStringLiteral("Preserve divergence"),
                    QStringLiteral("body"), QStringLiteral("main"),
                    QStringLiteral("feature-pr-divergent"), QString(), QString(),
                    /*branchBacked=*/true, &branchError);
                PullRequest divergentRecord;
                for (const PullRequest &candidate : branchPulls.loadAll()) {
                    if (candidate.number == divergent)
                        divergentRecord = candidate;
                }
                check(divergent == 3 &&
                          refTip(QStringLiteral("refs/heads/pr/3")) == divergentTip &&
                          refTip(QStringLiteral("refs/pr/3/head")) == featureTip,
                      "materialization preserves a divergent pr/3 branch");
                check(divergentRecord.head ==
                              QStringLiteral("feature-pr-divergent") &&
                          verifyEd25519(divergentRecord.author,
                                        divergentRecord.sig,
                                        PullStore::canonicalString(
                                            divergentRecord)),
                      "canonical branch materialization preserves signed pr.head metadata");

                const QString addStackOne = QStringLiteral(
                    "diff --git a/stack-one.txt b/stack-one.txt\n"
                    "new file mode 100644\n--- /dev/null\n"
                    "+++ b/stack-one.txt\n@@ -0,0 +1 @@\n+one\n");
                const int stackOne = branchPulls.createPull(
                    QStringLiteral("Stack one"), QStringLiteral("body"),
                    QStringLiteral("main"), QStringLiteral("pr/stack-one"),
                    addStackOne, QString(), /*branchBacked=*/false,
                    &branchError);
                const QString addStackTwo = QStringLiteral(
                    "diff --git a/stack-two.txt b/stack-two.txt\n"
                    "new file mode 100644\n--- /dev/null\n"
                    "+++ b/stack-two.txt\n@@ -0,0 +1 @@\n+two\n");
                const int stackTwo = branchPulls.createPull(
                    QStringLiteral("Stack two"), QStringLiteral("body"),
                    QStringLiteral("pr/stack-one"),
                    QStringLiteral("pr/stack-two"), addStackTwo, QString(),
                    /*branchBacked=*/false, &branchError);
                check(stackOne == 4 && stackTwo == 5 &&
                          refTip(QStringLiteral("refs/heads/pr/stack-one")) ==
                              refTip(QStringLiteral("refs/heads/pr/4")) &&
                          refTip(QStringLiteral("refs/heads/pr/stack-two")) ==
                              refTip(QStringLiteral("refs/heads/pr/5")) &&
                          runTestGit(branchRepo,
                                     {QStringLiteral("merge-base"),
                                      QStringLiteral("--is-ancestor"),
                                      QStringLiteral("refs/heads/pr/4"),
                                      QStringLiteral("refs/heads/pr/5")}),
                      "stacked PRs retain safe signed head aliases for the next base");

                {
                    const QString metaDir = branchPulls.metaWorkTree();
                    const QString ledgerPath =
                        metaDir + QStringLiteral("/.forkmesh/pulls/%1/pull.md")
                                      .arg(stackOne);
                    QFile ledgerFile(ledgerPath);
                    QString ledgerText;
                    if (ledgerFile.open(QIODevice::ReadOnly)) {
                        ledgerText = QString::fromUtf8(ledgerFile.readAll());
                        ledgerFile.close();
                    }
                    const QString openLine = QStringLiteral("\nstatus: open\n");
                    const bool wasOpen = ledgerText.contains(openLine);
                    ledgerText.replace(
                        openLine,
                        QStringLiteral("\nstatus: merged\nmergeBase: %1\n"
                                       "mergeHead: %1\n")
                            .arg(featureTip));
                    const bool ledgerMerged =
                        wasOpen &&
                        writeTestFile(ledgerPath, ledgerText.toUtf8()) &&
                        runTestGit(metaDir,
                                   {QStringLiteral("commit"), QStringLiteral("-q"),
                                    QStringLiteral("-m"),
                                    QStringLiteral("pull #%1: merged").arg(stackOne),
                                    QStringLiteral("--"), ledgerPath});
                    PullRequest afterLedgerMerge;
                    for (const PullRequest &candidate : branchPulls.loadAll()) {
                        if (candidate.number == stackOne)
                            afterLedgerMerge = candidate;
                    }
                    check(ledgerMerged &&
                              !refTip(QStringLiteral("refs/heads/forkmesh/pull/%1")
                                          .arg(stackOne))
                                   .isEmpty() &&
                              afterLedgerMerge.status == QStringLiteral("merged") &&
                              afterLedgerMerge.mergeHead == featureTip,
                          "a merge recorded only on forkmesh/pulls is not undone "
                          "by a stale open forkmesh/pull/<n> snapshot");
                }

                // An interrupted teardown can commit the ledger removal and
                // stop before dropping the materialized refs. loadAll() reads
                // forkmesh/pull/<n> in preference to the aggregate index, so
                // the PR came back on every load -- and every retry took
                // deletePull()'s "already gone" bail-out, which reported the
                // miss without touching a single ref, so the desktop said it
                // had "finished cleaning up" while the PR stayed in the list
                // for good (adhoc #2091).
                const auto listed = [&](int number) {
                    for (const PullRequest &candidate : branchPulls.loadAll()) {
                        if (candidate.number == number)
                            return true;
                    }
                    return false;
                };
                const QString strandedMetadata =
                    refTip(QStringLiteral("refs/heads/forkmesh/pull/%1")
                               .arg(stackTwo));
                const QString strandedHead =
                    refTip(QStringLiteral("refs/pr/%1/head").arg(stackTwo));
                QString strandedError;
                check(branchPulls.deletePull(stackTwo, /*rewriteHistory=*/false,
                                             &strandedError) &&
                          !listed(stackTwo) && !strandedMetadata.isEmpty() &&
                          !strandedHead.isEmpty(),
                      "deleting a PR clears both its record and its refs");
                // Put the refs back without the ledger record: this is the
                // state a teardown interrupted after its ledger commit leaves
                // on disk, and the state PR #175 was found in.
                const auto restoreRef = [&](const QString &ref,
                                            const QString &tip) {
                    return runTestGit(branchRepo, {QStringLiteral("update-ref"),
                                                   ref, tip});
                };
                check(restoreRef(QStringLiteral("refs/heads/forkmesh/pull/%1")
                                     .arg(stackTwo),
                                 strandedMetadata) &&
                          restoreRef(QStringLiteral("refs/pr/%1/metadata")
                                         .arg(stackTwo),
                                     strandedMetadata) &&
                          restoreRef(QStringLiteral("refs/heads/pr/%1").arg(stackTwo),
                                     strandedHead) &&
                          restoreRef(QStringLiteral("refs/pr/%1/head").arg(stackTwo),
                                     strandedHead) &&
                          !listed(stackTwo),
                      "a stranded metadata branch older than its ledger "
                      "deletion stays out of the list");
                strandedError.clear();
                const bool strandedOk = branchPulls.deletePull(
                    stackTwo, /*rewriteHistory=*/false, &strandedError);
                check(!strandedOk &&
                          PullStore::missingPullError(strandedError, stackTwo),
                      "deleting an absent ledger record still reports the miss");
                check(!listed(stackTwo) &&
                          refTip(QStringLiteral("refs/heads/forkmesh/pull/%1")
                                     .arg(stackTwo))
                              .isEmpty() &&
                          refTip(QStringLiteral("refs/pr/%1/metadata")
                                     .arg(stackTwo))
                              .isEmpty() &&
                          refTip(QStringLiteral("refs/heads/pr/%1").arg(stackTwo))
                              .isEmpty() &&
                          refTip(QStringLiteral("refs/pr/%1/head").arg(stackTwo))
                              .isEmpty(),
                      "cleaning up after an already-gone PR drops it from the list");

                // The fleet's mirrors keep every forkmesh/pull/<n> branch and
                // the upstream sync copies them back, so a deleted pull's
                // metadata branch returns on its own. Only a snapshot written
                // after the ledger removal (a reused number) may list again.
                check(restoreRef(QStringLiteral("refs/heads/forkmesh/pull/%1")
                                     .arg(stackTwo),
                                 strandedMetadata) &&
                          !listed(stackTwo),
                      "a deleted pull's metadata branch copied back from a "
                      "mirror does not relist it");
                QByteArray snapshotTree;
                QByteArray reusedSnapshot;
                QProcessEnvironment futureCommit =
                    QProcessEnvironment::systemEnvironment();
                futureCommit.insert(QStringLiteral("GIT_COMMITTER_DATE"),
                                    QStringLiteral("2099-01-01T00:00:00Z"));
                futureCommit.insert(QStringLiteral("GIT_AUTHOR_DATE"),
                                    QStringLiteral("2099-01-01T00:00:00Z"));
                const bool reused =
                    runTestGit(branchRepo,
                               {QStringLiteral("rev-parse"),
                                strandedMetadata + QStringLiteral("^{tree}")},
                               &snapshotTree) &&
                    runTestGit(branchRepo,
                               {QStringLiteral("commit-tree"),
                                QString::fromUtf8(snapshotTree).trimmed(),
                                QStringLiteral("-m"),
                                QStringLiteral("pull #%1: metadata snapshot")
                                    .arg(stackTwo)},
                               &reusedSnapshot, &futureCommit) &&
                    restoreRef(QStringLiteral("refs/heads/forkmesh/pull/%1")
                                   .arg(stackTwo),
                               QString::fromUtf8(reusedSnapshot).trimmed());
                check(reused && listed(stackTwo),
                      "a metadata branch written after the ledger deletion "
                      "still lists");
                runTestGit(branchRepo,
                           {QStringLiteral("update-ref"), QStringLiteral("-d"),
                            QStringLiteral("refs/heads/forkmesh/pull/%1")
                                .arg(stackTwo)});
                check(!listed(stackTwo),
                      "dropping the reused snapshot leaves the pull removed");
            }
        }

        // --- A conflicting cross-node pull still gets a branch -------------
        // A contribution that arrived through the relay is a signed patch with
        // no branch until its commits are replayed, and the "Fix" button that
        // hands an agent that branch is only ever offered for a pull whose
        // patch no longer applies to its base branch's tip. Replaying onto
        // that moved tip therefore cannot work -- the conflict is exactly that
        // -- so the pull most in need of an agent was the one left with no
        // branch to give it, and Fix refused with "branch is not on this node"
        // (adhoc #2092). The base the author signed against still takes it.
        {
            QTemporaryDir conflictRoot;
            const QString conflictRepo = conflictRoot.path();
            bool conflictSetup =
                conflictRoot.isValid() &&
                runTestGit(conflictRepo,
                           {QStringLiteral("init"), QStringLiteral("-q"),
                            QStringLiteral("-b"), QStringLiteral("main")}) &&
                runTestGit(conflictRepo,
                           {QStringLiteral("config"), QStringLiteral("user.name"),
                            QStringLiteral("Conflict Tester")}) &&
                runTestGit(conflictRepo,
                           {QStringLiteral("config"), QStringLiteral("user.email"),
                            QStringLiteral("conflict@example.test")}) &&
                writeTestFile(conflictRepo + QStringLiteral("/shared.txt"),
                              QByteArrayLiteral("one\n")) &&
                commitTestTree(conflictRepo, QStringLiteral("base"),
                               QStringLiteral("2026-09-01T10:00:00Z"),
                               QStringLiteral("Conflict Tester"),
                               QStringLiteral("conflict@example.test"));
            const QString signedBase = testGitHead(conflictRepo);
            conflictSetup =
                conflictSetup && !signedBase.isEmpty() &&
                runTestGit(conflictRepo,
                           {QStringLiteral("checkout"), QStringLiteral("-q"),
                            QStringLiteral("-b"),
                            QStringLiteral("contributor-work")}) &&
                writeTestFile(conflictRepo + QStringLiteral("/shared.txt"),
                              QByteArrayLiteral("one\ncontributor\n")) &&
                commitTestTree(conflictRepo, QStringLiteral("contribution"),
                               QStringLiteral("2026-09-01T11:00:00Z"),
                               QStringLiteral("Conflict Tester"),
                               QStringLiteral("conflict@example.test"));
            QByteArray conflictPatchOut;
            QByteArray conflictCommitsOut;
            conflictSetup =
                conflictSetup &&
                runTestGit(conflictRepo,
                           {QStringLiteral("diff"), QStringLiteral("--binary"),
                            signedBase + QStringLiteral("...contributor-work")},
                           &conflictPatchOut) &&
                runTestGit(conflictRepo,
                           {QStringLiteral("format-patch"),
                            QStringLiteral("--binary"),
                            QStringLiteral("--no-signature"),
                            QStringLiteral("--stdout"),
                            signedBase + QStringLiteral("..contributor-work")},
                           &conflictCommitsOut) &&
                runTestGit(conflictRepo,
                           {QStringLiteral("checkout"), QStringLiteral("-q"),
                            QStringLiteral("main")}) &&
                // Only the signed patch crosses the relay, so the branch it was
                // cut from is not a name this node can resolve.
                runTestGit(conflictRepo,
                           {QStringLiteral("branch"), QStringLiteral("-D"),
                            QStringLiteral("contributor-work")}) &&
                writeTestFile(conflictRepo + QStringLiteral("/shared.txt"),
                              QByteArrayLiteral("one\nupstream\n")) &&
                commitTestTree(conflictRepo, QStringLiteral("upstream moves on"),
                               QStringLiteral("2026-09-01T12:00:00Z"),
                               QStringLiteral("Conflict Tester"),
                               QStringLiteral("conflict@example.test"));
            check(conflictSetup,
                  "set up a cross-node pull that conflicts with its base tip");
            if (conflictSetup) {
                PullStore conflictPulls(conflictRepo, QString(), &identity,
                                        QStringLiteral("tester"));
                PullRequest relayed;
                relayed.title = QStringLiteral("Conflicting contribution");
                relayed.base = QStringLiteral("main");
                relayed.head = QStringLiteral("othernode:agent/attribution-core");
                relayed.patch = QString::fromUtf8(conflictPatchOut);
                relayed.commits = QString::fromUtf8(conflictCommitsOut);
                relayed.creationBaseOid = signedBase;
                relayed = conflictPulls.makeSignedPull(relayed);
                QString conflictError;
                check(conflictPulls.applyRemotePull(relayed, &conflictError),
                      "a conflicting cross-node pull is stored");
                int relayedNumber = 0;
                for (const PullRequest &candidate : conflictPulls.loadAll())
                    if (candidate.title == relayed.title)
                        relayedNumber = candidate.number;
                bool cleanApply = true;
                QStringList conflictFiles;
                check(relayedNumber > 0 &&
                          conflictPulls.checkMergeable(relayedNumber, &cleanApply,
                                                       &conflictFiles, nullptr,
                                                       false) &&
                          !cleanApply,
                      "the fixture pull really does conflict with the base tip");
                QByteArray branchTree;
                check(relayedNumber > 0 &&
                          runTestGit(conflictRepo,
                                     {QStringLiteral("show"),
                                      QStringLiteral("pr/%1:shared.txt")
                                          .arg(relayedNumber)},
                                     &branchTree) &&
                          branchTree == QByteArrayLiteral("one\ncontributor\n"),
                      "a conflicting cross-node pull is materialized on its "
                      "numbered branch, carrying the contributor's tree");

                // The compatibility ref alone is not something git can check an
                // agent worktree out of, so a pull whose commit is only
                // recorded there still gets the numbered branch.
                PullRequest refOnly = relayed;
                refOnly.title = QStringLiteral("Ref-only contribution");
                refOnly.creationBaseOid.clear();
                refOnly.ts += 1;
                refOnly = conflictPulls.makeSignedPull(refOnly);
                check(conflictPulls.applyRemotePull(refOnly, &conflictError),
                      "a cross-node pull with no signed base is stored");
                PullRequest storedRefOnly;
                for (const PullRequest &candidate : conflictPulls.loadAll())
                    if (candidate.title == refOnly.title)
                        storedRefOnly = candidate;
                const int refOnlyNumber = storedRefOnly.number;
                QByteArray contributorTipOut;
                runTestGit(conflictRepo,
                           {QStringLiteral("rev-parse"),
                            QStringLiteral("refs/pr/%1/head").arg(relayedNumber)},
                           &contributorTipOut);
                const QString contributorTip =
                    QString::fromUtf8(contributorTipOut).trimmed();
                QByteArray refOnlyBranch;
                check(refOnlyNumber > 0 && !contributorTip.isEmpty() &&
                          runTestGit(conflictRepo,
                                     {QStringLiteral("update-ref"),
                                      QStringLiteral("refs/pr/%1/head")
                                          .arg(refOnlyNumber),
                                      contributorTip}) &&
                          conflictPulls.ensurePullHeadRef(storedRefOnly,
                                                          &conflictError) &&
                          runTestGit(conflictRepo,
                                     {QStringLiteral("rev-parse"),
                                      QStringLiteral("--verify"),
                                      QStringLiteral("refs/heads/pr/%1")
                                          .arg(refOnlyNumber)},
                                     &refOnlyBranch) &&
                          QString::fromUtf8(refOnlyBranch).trimmed() ==
                              contributorTip,
                      "a pull whose commit is only on the compatibility ref is "
                      "adopted onto its numbered branch");

                // The case that left PR #89 with no branch at all: a relayed
                // pull whose author signed no base oid, whose base branch has
                // moved on since the patch was cut, and whose commit no ref
                // here records. The patch's own pre-image blobs still name the
                // commit it fits, so history is asked for that one instead of
                // reporting code that is sitting right here as unreachable.
                PullRequest driftedBase = relayed;
                driftedBase.title = QStringLiteral("Unsigned-base contribution");
                driftedBase.creationBaseOid.clear();
                driftedBase.ts += 2;
                driftedBase = conflictPulls.makeSignedPull(driftedBase);
                check(conflictPulls.applyRemotePull(driftedBase, &conflictError),
                      "a cross-node pull with no signed base and a moved base "
                      "tip is stored");
                int driftedNumber = 0;
                for (const PullRequest &candidate : conflictPulls.loadAll())
                    if (candidate.title == driftedBase.title)
                        driftedNumber = candidate.number;
                QByteArray driftedTree;
                check(driftedNumber > 0 &&
                          runTestGit(conflictRepo,
                                     {QStringLiteral("show"),
                                      QStringLiteral("pr/%1:shared.txt")
                                          .arg(driftedNumber)},
                                     &driftedTree) &&
                          driftedTree == QByteArrayLiteral("one\ncontributor\n"),
                      "a pull with no signed base is replayed onto the commit "
                      "its patch was cut from, so it still gets a branch");
                // Cut from the same base as the pull request itself, the branch
                // has to conflict with the base tip exactly as the pull does -
                // that unresolved merge is the work "Fix" hands the agent.
                QByteArray driftedMergeBase;
                check(driftedNumber > 0 &&
                          runTestGit(conflictRepo,
                                     {QStringLiteral("merge-base"),
                                      QStringLiteral("main"),
                                      QStringLiteral("pr/%1").arg(driftedNumber)},
                                     &driftedMergeBase) &&
                          QString::fromUtf8(driftedMergeBase).trimmed() ==
                              signedBase &&
                          !runTestGit(conflictRepo,
                                      {QStringLiteral("merge-tree"),
                                       QStringLiteral("--write-tree"),
                                       QStringLiteral("main"),
                                       QStringLiteral("pr/%1").arg(driftedNumber)},
                                      nullptr),
                      "that branch forks from the base the patch was cut from "
                      "and still conflicts with the base tip");
            }
        }

        {
            QTemporaryDir vectorRoot;
            const QString vectorRepo = vectorRoot.path();
            const QByteArray vectorPatch = QByteArrayLiteral(
                "diff --git a/shared.txt b/shared.txt\n"
                "index 5626abf..e265e82 100644\n"
                "--- a/shared.txt\n"
                "+++ b/shared.txt\n"
                "@@ -1 +1,2 @@\n"
                " one\n"
                "+contributor\n");
            const QByteArray vectorMbox =
                QByteArrayLiteral(
                    "From 0000000000000000000000000000000000000000 Mon Sep 17 00:00:00 2001\n"
                    "From: Vector <vector@example.test>\n"
                    "Date: Tue, 1 Sep 2026 11:00:00 +0000\n"
                    "Subject: [PATCH] contribution\n"
                    "\n"
                    "---\n") +
                vectorPatch;
            const bool vectorSetup =
                vectorRoot.isValid() &&
                runTestGit(vectorRepo,
                           {QStringLiteral("init"), QStringLiteral("-q"),
                            QStringLiteral("-b"), QStringLiteral("main")}) &&
                runTestGit(vectorRepo,
                           {QStringLiteral("config"), QStringLiteral("commit.gpgSign"),
                            QStringLiteral("false")}) &&
                writeTestFile(vectorRepo + QStringLiteral("/shared.txt"),
                              QByteArrayLiteral("one\n")) &&
                commitTestTree(vectorRepo, QStringLiteral("base"),
                               QStringLiteral("2026-09-01T10:00:00Z"),
                               QStringLiteral("Vector"),
                               QStringLiteral("vector@example.test"));
            check(vectorSetup &&
                      testGitHead(vectorRepo) ==
                          QStringLiteral("1d1c53be6d9b05fa8a531653a4a940a6c19da46e"),
                  "the pull head vector base matches server/pull_head_test.go");
            if (vectorSetup) {
                PullStore vectorPulls(vectorRepo, QString(), &identity,
                                      QStringLiteral("tester"));
                PullRequest seriesPull;
                seriesPull.title = QStringLiteral("contribution series");
                seriesPull.base = QStringLiteral("main");
                seriesPull.head = QStringLiteral("vector:contribution");
                seriesPull.author = QStringLiteral("vectorkey");
                seriesPull.authorName = QStringLiteral("Vector");
                seriesPull.ts = 1788260400000;
                seriesPull.patch = QString::fromUtf8(vectorPatch);
                seriesPull.commits = QString::fromUtf8(vectorMbox);
                PullRequest patchPull = seriesPull;
                patchPull.title = QStringLiteral("contribution");
                patchPull.commits.clear();
                QString vectorError;
                const bool stored =
                    vectorPulls.applyRemotePull(seriesPull, &vectorError);
                const bool storedPatch =
                    vectorPulls.applyRemotePull(patchPull, &vectorError);
                int seriesNumber = 0;
                int patchNumber = 0;
                for (const PullRequest &candidate : vectorPulls.loadAll()) {
                    if (candidate.title == seriesPull.title)
                        seriesNumber = candidate.number;
                    if (candidate.title == patchPull.title)
                        patchNumber = candidate.number;
                }
                QByteArray seriesHead;
                QByteArray patchHead;
                QByteArray committer;
                check(stored && seriesNumber > 0 &&
                          runTestGit(vectorRepo,
                                     {QStringLiteral("rev-parse"),
                                      QStringLiteral("refs/pr/%1/head").arg(seriesNumber)},
                                     &seriesHead) &&
                          seriesHead.trimmed() ==
                              QByteArrayLiteral("0231062d32ad46550ba26e9ed7b812c49bb05aaf"),
                      "a relayed series becomes the same commit the mirror cores derive");
                check(storedPatch && patchNumber > 0 &&
                          runTestGit(vectorRepo,
                                     {QStringLiteral("rev-parse"),
                                      QStringLiteral("refs/pr/%1/head").arg(patchNumber)},
                                     &patchHead) &&
                          patchHead.trimmed() ==
                              QByteArrayLiteral("5f915f9b52b6a2cfa6fc6e68d01496ab24f3e6fd"),
                      "a relayed bare patch becomes the same commit the mirror cores derive");
                check(runTestGit(vectorRepo,
                                 {QStringLiteral("show"), QStringLiteral("-s"),
                                  QStringLiteral("--format=%cn <%ce>"),
                                  QString::fromUtf8(seriesHead.trimmed())},
                                 &committer) &&
                          committer.trimmed() ==
                              QByteArrayLiteral("ForkMesh <pulls@users.noreply.forkmesh.com>"),
                      "a replayed pull head carries the fixed pull committer");
                bool vectorClean = false;
                QStringList vectorConflicts;
                check(seriesNumber > 0 &&
                          vectorPulls.checkMergeable(seriesNumber, &vectorClean,
                                                     &vectorConflicts, nullptr, false) &&
                          vectorClean,
                      "a relayed pull is checked by merging its head commit");
            }
        }

        // --- PullStore conversation round-trip ---------------------------
        PullStore pulls(tmp.path(), QString(), &identity, "tester");
        const int pn = pulls.createPull(
            "A change", "Body", "main", "feature",
            "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n---flag\n+hi\n",
            QString(), /*branchBacked=*/false, &err);
        check(pn == 1, "createPull returns the first PR number");
        check(pulls.addComment(pn, "first comment", &err), "PR addComment succeeds");
        check(pulls.addReview(pn, "approved", "LGTM", &err), "PR addReview succeeds");
        check(pulls.addLineComment(pn, "x", "new", 1, "inline note", &err),
              "PR addLineComment succeeds");
        check(pulls.addThreadComment(pn, "x", "new", 1, 2, "thread note",
                                     "@@ -0,0 +1 @@\n+suggested\n", &err),
              "PR addThreadComment succeeds");
        check(pulls.addThreadReply(pn, "thread-1", "event-1", "reply note", &err),
              "PR addThreadReply succeeds");
        check(pulls.setThreadState(pn, "thread-1", "resolved", "fixed", &err),
              "PR setThreadState succeeds");
        check(pulls.setSuggestionState(pn, "thread-1", "applied", "abc123",
                                       "applied", &err),
              "PR setSuggestionState succeeds");
        PullAnchor anchor;
        anchor.patchset = "0123456789abcdef0123456789abcdef01234567";
        anchor.anchorLines = "first\nsecond";
        check(pulls.addThreadComment(pn, "x", "new", 1, 2, "anchored note",
                                     QString(), anchor, &err),
              "PR addThreadComment with a patchset anchor succeeds");
        QList<PullRequest> loadedPulls = pulls.loadAll();
        check(!loadedPulls.isEmpty() && loadedPulls.first().events.size() == 8,
              "PR conversation events round-trip from NNNN-*.md");
        bool sawLine = false, sawThread = false, sawReply = false;
        bool sawResolved = false, sawSuggestionApplied = false;
        bool sawAnchored = false;
        if (!loadedPulls.isEmpty()) {
            for (const PullEvent &e : loadedPulls.first().events) {
                if (e.type == "line-comment" && e.path == "x" && e.side == "new" &&
                    e.line == 1 && e.body == "inline note") {
                    sawLine = true;
                } else if (e.type == "thread-comment" && e.path == "x" &&
                           e.side == "new" && e.lineStart == 1 && e.lineEnd == 2 &&
                           e.body == "thread note" &&
                           e.suggestionPatch.contains("+suggested")) {
                    sawThread = true;
                } else if (e.type == "thread-reply" && e.threadId == "thread-1" &&
                           e.parentId == "event-1" && e.body == "reply note") {
                    sawReply = true;
                } else if (e.type == "thread-state" && e.threadId == "thread-1" &&
                           e.state == "resolved" && e.body == "fixed") {
                    sawResolved = true;
                } else if (e.type == "suggestion-state" &&
                           e.threadId == "thread-1" && e.state == "applied" &&
                           e.appliedCommit == "abc123") {
                    sawSuggestionApplied = true;
                } else if (e.type == "thread-comment" &&
                           e.body == "anchored note" &&
                           e.patchset == "0123456789abcdef0123456789abcdef01234567" &&
                           e.anchorLines == "first\nsecond" &&
                           !e.sig.isEmpty()) {
                    sawAnchored = true;
                }
            }
        }
        check(sawAnchored,
              "anchored thread-comment round-trips its patchset and multi-line "
              "anchor text through front matter");
        check(sawLine, "line-comment round-trips with path/side/line");
        check(sawThread, "thread-comment round-trips with anchor and suggestion");
        check(sawReply, "thread-reply round-trips with thread and parent");
        check(sawResolved, "thread-state round-trips with resolved state");
        check(sawSuggestionApplied,
              "suggestion-state round-trips with applied commit");
        check(!loadedPulls.isEmpty() &&
                  loadedPulls.first().reviewSummary() == "approved",
              "PR review summary folds to approved");
        check(!loadedPulls.isEmpty() && loadedPulls.first().additions == 1 &&
                  loadedPulls.first().deletions == 1,
              "PR stats count removed content beginning with two hyphens");

        PullAnchor branchAnchor;
        branchAnchor.patchset = "89abcdef0123456789abcdef0123456789abcdef";
        branchAnchor.anchorLines = "hi";
        check(pulls.addBranchThreadComment("feature/review", "x", "new", 1, 1,
                                           "branch note", branchAnchor, &err),
              "branch addBranchThreadComment succeeds");
        const QList<PullEvent> branchOpened =
            pulls.readBranchReviewEvents("feature/review");
        const QString branchThread =
            branchOpened.isEmpty() ? QString() : branchOpened.first().threadId;
        check(pulls.addBranchThreadReply("feature/review", branchThread,
                                         branchThread, "branch reply", &err) &&
                  pulls.setBranchThreadState("feature/review", branchThread,
                                             "resolved", &err),
              "branch reply and resolve succeed");
        check(pulls.addBranchThreadComment("feature/review", "x", "new", 0, 0,
                                           "file note", PullAnchor(), &err),
              "branch file-level comment succeeds");
        const QList<PullEvent> branchEvents =
            pulls.readBranchReviewEvents("feature/review");
        check(branchEvents.size() == 4 && branchEvents.at(0).body == "branch note" &&
                  branchEvents.at(0).lineStart == 1 &&
                  branchEvents.at(0).patchset == branchAnchor.patchset &&
                  branchEvents.at(0).anchorLines == "hi" &&
                  branchEvents.at(1).type == "thread-reply" &&
                  branchEvents.at(1).threadId == branchThread &&
                  branchEvents.at(2).state == "resolved" &&
                  branchEvents.at(3).lineStart == 0,
              "branch review events round-trip in order");
        check(!branchEvents.isEmpty() &&
                  verifyEd25519(branchEvents.first().author,
                                branchEvents.first().sig,
                                PullStore::branchCanonicalString(
                                    "feature/review", branchEvents.first())) &&
                  !verifyEd25519(branchEvents.first().author,
                                 branchEvents.first().sig,
                                 PullStore::branchCanonicalString(
                                     "other", branchEvents.first())),
              "branch review signatures bind the branch name");
        PullEvent phoneEvent;
        phoneEvent.type = "thread-comment";
        phoneEvent.path = "x";
        phoneEvent.side = "new";
        phoneEvent.lineStart = 1;
        phoneEvent.lineEnd = 1;
        phoneEvent.body = "from the phone";
        phoneEvent = pulls.makeSignedBranchEvent("feature/review", phoneEvent);
        check(pulls.applyRemoteBranchEvent("feature/review", phoneEvent, &err) &&
                  pulls.applyRemoteBranchEvent("feature/review", phoneEvent, &err) &&
                  pulls.readBranchReviewEvents("feature/review").size() == 5,
              "a drained branch event is stored once, and a replay is a no-op");
        PullEvent pullOnly = phoneEvent;
        pullOnly.type = "comment";
        pullOnly.id = "pull-only";
        check(!pulls.applyRemoteBranchEvent("feature/review", pullOnly, &err) &&
                  !pulls.applyRemoteBranchEvent("../escape", phoneEvent, &err) &&
                  !pulls.applyRemoteBranchEvent("bad name", phoneEvent, &err) &&
                  PullStore::validReviewBranchName("1319-diff-view-allow-click"),
              "drained branch events with other types or bad branch names are refused");
        check(pulls.readBranchReviewEvents("other").isEmpty(),
              "another branch has no review events");
        check(pulls.loadAll().size() == loadedPulls.size(),
              "branch reviews never appear as pull requests");
        QByteArray branchLedger;
        check(runTestGit(tmp.path(),
                         {QStringLiteral("ls-tree"), QStringLiteral("-r"),
                          QStringLiteral("--name-only"),
                          QStringLiteral("refs/heads/forkmesh/pulls"),
                          QStringLiteral(".forkmesh/branch-reviews/") +
                              PullStore::branchReviewKey("feature/review")},
                         &branchLedger) &&
                  branchLedger.contains("branch.md") &&
                  branchLedger.contains("-thread-comment.md"),
              "branch reviews are committed to the forkmesh/pulls ledger");

        // A browser-created pull carries portable, signed change bytes rather
        // than only mutable branch names. Exercise the exact Worker -> Qt wire
        // shape (including legacy `body`) and prove the drained record merges.
        const QString webBase = QString::fromUtf8(
            gitOutput({"rev-parse", "--abbrev-ref", "HEAD"})).trimmed();
        const QString webBaseOid = QString::fromUtf8(
            gitOutput({"rev-parse", "HEAD"})).trimmed();
        git({"checkout", "-q", "-b", "web-wire-feature"});
        check(writeTestFile(tmp.path() + "/web-wire.txt",
                            QByteArrayLiteral("portable browser change\n")),
              "write browser pull fixture");
        git({"add", "web-wire.txt"});
        git({"commit", "-q", "-m", "portable browser commit"});
        const QString webHeadOid = QString::fromUtf8(
            gitOutput({"rev-parse", "HEAD"})).trimmed();
        const QString webPatch = QString::fromUtf8(
            gitOutput({"diff", "--binary",
                       webBase + "...web-wire-feature"}));
        const QString webCommits = QString::fromUtf8(
            gitOutput({"format-patch", "--binary", "--no-signature", "--stdout",
                       webBase + "..web-wire-feature"}));
        git({"checkout", "-q", webBase});

        PullRequest browserPull;
        browserPull.title = "Portable browser pull";
        browserPull.description = "Browser description";
        browserPull.base = webBase;
        browserPull.head = "web-wire-feature";
        browserPull.patch = webPatch;
        browserPull.commits = webCommits;
        browserPull = pulls.makeSignedPull(browserPull);
        QJsonObject browserWire = browserPull.toJson();
        browserWire.remove("description");
        browserWire.insert("body", "Browser description");
        PullRequest drainedBrowserPull = PullRequest::fromJson(browserWire);
        drainedBrowserPull.number = 73; // relay-reserved web number
        check(drainedBrowserPull.description == "Browser description",
              "PullRequest wire reader preserves legacy browser body text");
        check(drainedBrowserPull.patch == webPatch &&
                  drainedBrowserPull.commits == webCommits &&
                  !drainedBrowserPull.patch.isEmpty() &&
                  !drainedBrowserPull.commits.isEmpty(),
              "PullRequest wire reader retains the signed portable change set");
        check(verifyEd25519(drainedBrowserPull.author,
                            drainedBrowserPull.sig,
                            PullStore::canonicalString(drainedBrowserPull)),
              "drained browser change bytes retain their valid signature");
        check(pulls.applyRemotePull(drainedBrowserPull, &err),
              "owner drain stores a portable browser pull");
        int browserPullNumber = 0;
        for (const PullRequest &candidate : pulls.loadAll()) {
            if (candidate.title == browserPull.title) {
                browserPullNumber = candidate.number;
                check(candidate.description == "Browser description" &&
                          candidate.patch == webPatch &&
                          candidate.commits == webCommits,
                      "stored browser pull keeps description, patch, and commits");
                break;
            }
        }
        check(browserPullNumber == 73,
              "owner drain honors the relay-reserved browser pull number");
        QString reviewGateError;
        check(browserPullNumber > 0 &&
                  !pulls.mergePull(browserPullNumber, &reviewGateError, true) &&
                  reviewGateError.contains("approval from a peer"),
              "required peer review still blocks a normal pull merge");
        check(pulls.mergePull(browserPullNumber, &err, true, true),
              "repository-owner override merges after explicit review waiver");
        QString mergedAgainError;
        check(!pulls.mergePull(browserPullNumber, &mergedAgainError, true, true)
                  && mergedAgainError
                         == forkmesh::pulldecision::decidedPullMessage(
                             QStringLiteral("merged"))
                  && forkmesh::pulldecision::decidedPullError(mergedAgainError),
              "merging an already merged pull is refused as a decided pull, "
              "which the merge dialog keeps out of the relay error log");
        check(forkmesh::pulldecision::decidedPullError(
                  QStringLiteral("This pull request is already closed."))
                  && !forkmesh::pulldecision::decidedPullError(
                      QStringLiteral("This pull request is already open."))
                  && !forkmesh::pulldecision::decidedPullError(reviewGateError)
                  && !forkmesh::pulldecision::decidedPullError(QStringLiteral(
                      "This pull request is already merged. git said no")),
              "only the exact merged/closed refusal counts as a decided pull");
        {
            const QByteArray heldBack(
                "{\"schema\":\"forkmesh-release-v2\",\"tag\":\"v1.2.4\"}");
            check(forkmesh::releasemanifest::stillNamesAnotherRelease(
                      heldBack, QStringLiteral("v1.2.5"))
                      && !forkmesh::releasemanifest::stillNamesAnotherRelease(
                          heldBack, QStringLiteral("v1.2.4"))
                      && !forkmesh::releasemanifest::stillNamesAnotherRelease(
                          QByteArray("not json"), QStringLiteral("v1.2.5")),
                  "a latest manifest that still names the previous release "
                  "means the new tag is held back, not that its manifest is "
                  "forged; a matching or unreadable manifest is still checked");
        }
        {
            const QString gitSaid = QStringLiteral(
                "error: Your local changes to the following files would be "
                "overwritten by merge:\n\tdocs/desktop/README.md\nPlease commit "
                "your changes or stash them before you merge.\nAborting");
            const QString blocked = forkmesh::pulldecision::localChangesBlockMessage(
                12, QStringLiteral("/work/src"), gitSaid);
            check(forkmesh::gitrefusal::overLocalChanges(gitSaid)
                      && forkmesh::pulldecision::localChangesBlockError(blocked)
                      && blocked.contains(QStringLiteral("docs/desktop/README.md"))
                      && !forkmesh::pulldecision::localChangesBlockError(
                          QStringLiteral("Could not merge the pull request's "
                                         "branch cleanly: CONFLICT (content)")),
                  "a pull merge refused over uncommitted edits in the checkout "
                  "names them and is told apart from a conflict");
        }
        QByteArray ownerWaiverLedgerSubject;
        check(runTestGit(pulls.metaWorkTree(),
                         {QStringLiteral("log"), QStringLiteral("-1"),
                          QStringLiteral("--format=%s")},
                         &ownerWaiverLedgerSubject) &&
                  ownerWaiverLedgerSubject.contains(
                      "review gate waived by owner"),
              "owner review waiver is recorded in the pull-request ledger");
        QFile mergedWebFile(tmp.path() + "/web-wire.txt");
        check(mergedWebFile.open(QIODevice::ReadOnly) &&
                  mergedWebFile.readAll() ==
                      QByteArrayLiteral("portable browser change\n"),
              "merged browser pull lands its signed repository change");
        PullRequest collidingBrowserPull = browserPull;
        collidingBrowserPull.title = "Colliding portable browser pull";
        collidingBrowserPull.ts += 1;
        collidingBrowserPull = pulls.makeSignedPull(collidingBrowserPull);
        collidingBrowserPull.number = 73;
        check(pulls.applyRemotePull(collidingBrowserPull, &err),
              "owner drain resolves an occupied relay-reserved pull number");
        int collisionNumber = 0;
        for (const PullRequest &candidate : pulls.loadAll())
            if (candidate.title == collidingBrowserPull.title)
                collisionNumber = candidate.number;
        check(collisionNumber > 73,
              "colliding browser pull is reassigned above the occupied slot");
        check(webBaseOid != webHeadOid,
              "browser pull fixture resolves distinct immutable commits");

        // Path lookup is on the hot read path: readPull() calls pullDir()
        // repeatedly for every PR. Once the metadata worktree exists, asking
        // for it must be a cached/file-only operation and must not spawn git.
        // Put a marker executable first on PATH to make that guarantee exact
        // rather than relying on a timing threshold.
        {
            QTemporaryDir fakeGitDir;
            const QString marker = fakeGitDir.filePath(QStringLiteral("called"));
            const QString fakeGit = fakeGitDir.filePath(QStringLiteral("git"));
            check(writeTestFile(
                      fakeGit,
                      QStringLiteral("#!/bin/sh\nprintf called >> '%1'\nexit 99\n")
                          .arg(marker)
                          .toUtf8()),
                  "write metadata lookup git-spawn sentinel");
            check(QFile::setPermissions(
                      fakeGit, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                   QFileDevice::ExeOwner),
                  "make metadata lookup git-spawn sentinel executable");
            const QByteArray originalPath = qgetenv("PATH");
            qputenv("PATH", fakeGitDir.path().toUtf8());
            const QString expectedMeta = pulls.metaWorkTree();
            bool stable = !expectedMeta.isEmpty();
            for (int i = 0; i < 100; ++i)
                stable = stable && pulls.metaWorkTree() == expectedMeta;
            qputenv("PATH", originalPath);
            check(stable, "repeated PR metadata path lookup stays stable");
            check(!QFileInfo::exists(marker),
                  "repeated PR metadata path lookup starts no git subprocess");
        }

        // The pull ledger lives on forkmesh/pulls, kept in a long-lived linked
        // checkout that every writable read walks as plain files. Anything that
        // moves that branch behind the checkout's back -- a forced fetch, a
        // `git branch -f`, a path migration commit -- leaves HEAD ahead of the
        // files on disk, and then the desktop lists no pull requests at all
        // while the branch still carries every one of them. Resolving the
        // metadata worktree has to restore what the branch has and the checkout
        // lacks.
        {
            QTemporaryDir staleTmp;
            const QString staleRepo = QDir(staleTmp.path()).absolutePath();
            const QString staleMeta =
                QStandardPaths::writableLocation(
                    QStandardPaths::AppDataLocation) +
                QStringLiteral("/pull-meta/") +
                QString::fromUtf8(
                    QCryptographicHash::hash(staleRepo.toUtf8(),
                                             QCryptographicHash::Sha256)
                        .toHex()
                        .left(16));
            bool staleSetup =
                staleTmp.isValid() &&
                runTestGit(staleRepo, {QStringLiteral("init"),
                                       QStringLiteral("-q"),
                                       QStringLiteral("-b"),
                                       QStringLiteral("main")}) &&
                runTestGit(staleRepo, {QStringLiteral("config"),
                                       QStringLiteral("user.name"),
                                       QStringLiteral("Owner Name")}) &&
                runTestGit(staleRepo, {QStringLiteral("config"),
                                       QStringLiteral("user.email"),
                                       QStringLiteral("owner@example.test")}) &&
                writeTestFile(staleRepo + "/README.md", "hello\n") &&
                commitTestTree(staleRepo, "seed", "2026-08-08T09:00:00Z",
                               "Owner Name", "owner@example.test") &&
                runTestGit(staleRepo, {QStringLiteral("branch"),
                                       QStringLiteral("forkmesh/pulls")}) &&
                runTestGit(staleRepo,
                           {QStringLiteral("worktree"), QStringLiteral("add"),
                            QStringLiteral("-q"), staleMeta,
                            QStringLiteral("forkmesh/pulls")});
            const QString ledgerBase = testGitHead(staleMeta);
            // Author the pull through the checkout itself, so the shared
            // pull-meta path is never resolved from the repository yet.
            PullStore ledger(staleMeta, QString(), &identity, "tester");
            const int stalePn = ledger.createPull(
                "Stale ledger", "Body", "main", "feature",
                "diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n",
                QString(), /*branchBacked=*/false, &err);
            staleSetup = staleSetup && stalePn == 1 && !ledgerBase.isEmpty();
            check(staleSetup, "stale pull-ledger checkout fixture is created");
            // Rewind the checkout to the pre-pull tree without moving HEAD --
            // exactly what a forced ref update leaves behind.
            check(runTestGit(staleMeta,
                             {QStringLiteral("read-tree"), ledgerBase}) &&
                      QDir(staleMeta + "/.forkmesh/pulls").removeRecursively(),
                  "pull ledger checkout falls behind its branch");

            const PullStore recovered(staleRepo, QString(), &identity, "tester");
            const QList<PullRequest> recoveredPulls = recovered.loadAll();
            check(recoveredPulls.size() == 1 &&
                      recoveredPulls.first().number == stalePn &&
                      recoveredPulls.first().title ==
                          QStringLiteral("Stale ledger"),
                  "a pull ledger checkout left behind by its branch is restored");
            check(recovered.metaWorkTree() == staleMeta,
                  "recovery reuses the existing pull metadata worktree");
            // A left-behind index reads as a pile of unrelated pending changes,
            // which is what makes closing or deleting a pull refuse to run.
            QByteArray recoveredStatus;
            check(runTestGit(staleMeta,
                             {QStringLiteral("status"),
                              QStringLiteral("--porcelain"),
                              QStringLiteral("--untracked-files=no")},
                             &recoveredStatus) &&
                      recoveredStatus.trimmed().isEmpty(),
                  "recovery leaves the pull ledger checkout with nothing pending");
            QDir(staleMeta).removeRecursively();
            runTestGit(staleRepo, {QStringLiteral("worktree"),
                                   QStringLiteral("prune")});
        }

        // A remote node files a review event via the inbox; it must append+commit.
        PullEvent remoteReview;
        remoteReview.type = "review";
        remoteReview.state = "changes_requested";
        remoteReview.body = "please fix";
        remoteReview = pulls.makeSignedEvent(pn, remoteReview);
        remoteReview.authorName = "reviewer-node";
        check(pulls.applyRemoteEvent(pn, remoteReview, &err),
              "applyRemoteEvent accepts a mirror-authored review");
        loadedPulls = pulls.loadAll();
        check(!loadedPulls.isEmpty() &&
                  loadedPulls.first().reviewSummary() == "changes_requested",
              "a later changes-requested review supersedes approval");
        const QString closedPullDir =
            pulls.metaWorkTree() + "/.forkmesh/pulls/" + QString::number(pn);
        const QString legacyRewriteSentinel =
            pulls.metaWorkTree() + "/.git-rewrite/keep";
        check(writeTestFile(legacyRewriteSentinel, "keep\n"),
              "write pull-ledger rewrite-folder sentinel");
        check(pulls.setStatus(pn, "closed", &err),
              "closing a pull request succeeds");
        loadedPulls = pulls.loadAll();
        bool foundClosedPull = false;
        for (const PullRequest &candidate : std::as_const(loadedPulls))
            if (candidate.number == pn)
                foundClosedPull = true;
        check(!foundClosedPull && !QFile::exists(closedPullDir),
              "closing removes the PR metadata and change payload");
        check(gitOutput({"log", "--format=%H", "forkmesh/pulls", "--",
                         QStringLiteral(".forkmesh/pulls/%1").arg(pn)}).trimmed().isEmpty(),
              "closing purges the PR from pull-ledger history");
        check(QFile::exists(legacyRewriteSentinel),
              "pull-history rewrite does not use or remove the checkout-local rewrite folder");

        // --- DiscussionStore round-trip ---------------------------------
        DiscussionStore discussions(tmp.path(), QString(), &identity, "tester");
        const int dn = discussions.createDiscussion(
            "Welcome", "Hello **discussion**", "Announcements", &err);
        check(dn == 1, "createDiscussion returns the first discussion number");
        check(discussions.addComment(dn, "first reply", &err),
              "discussion addComment succeeds");
        QList<Discussion> loadedDiscussions = discussions.loadAll();
        check(!loadedDiscussions.isEmpty() &&
                  loadedDiscussions.first().title == "Welcome" &&
                  loadedDiscussions.first().category == "Announcements" &&
                  loadedDiscussions.first().events.size() == 2,
              "discussion loads back with title, category and comments");
        DiscussionEvent remoteComment;
        remoteComment.type = "comment";
        remoteComment.body = "remote reply";
        remoteComment = discussions.makeSignedEvent(dn, remoteComment);
        check(discussions.applyRemoteEvent(dn, remoteComment, "Remote welcome", &err),
              "discussion remote comment applies");
        check(discussions.applyRemoteEvent(dn, remoteComment, "Remote welcome", &err),
              "discussion remote comment reapply is idempotent");
        check(discussions.setStatus(dn, "closed", &err),
              "discussion close succeeds");
        loadedDiscussions = discussions.loadAll();
        check(!loadedDiscussions.isEmpty() &&
                  loadedDiscussions.first().status == "closed",
              "closed discussion status round-trips");
        check(!discussions.addComment(dn, "should be rejected", &err),
              "closed discussion rejects new comments");
        check(discussions.setStatus(dn, "archived", &err),
              "discussion archive succeeds");
        check(discussions.setStatus(dn, "open", &err),
              "archived discussion can be reopened");
        const int qn = discussions.createDiscussion(
            "How?", "question", "Q&A", &err);
        check(discussions.addComment(qn, "an answer", &err),
              "Q&A discussion takes a reply");
        Discussion question;
        for (const Discussion &d : discussions.loadAll())
            if (d.number == qn)
                question = d;
        const QString replyId = question.events.value(1).id;
        check(question.answerable() && question.answerId().isEmpty() &&
                  discussions.mayMarkAnswer(question),
              "a Q&A discussion starts unanswered and its owner may mark one");
        check(discussions.markAnswer(qn, replyId, &err),
              "markAnswer records the chosen reply");
        QFile answerFile(tmp.path() + QStringLiteral("/.forkmesh/discussions/%1/answer.md").arg(qn));
        check(answerFile.open(QIODevice::ReadOnly) &&
                  QString::fromUtf8(answerFile.readAll())
                      .contains("\ntargetAuthor: " + identity.publicKey() + "\n"),
              "answer.md names the answering comment's author");
        for (const Discussion &d : discussions.loadAll())
            if (d.number == qn)
                question = d;
        check(question.answerId() == replyId && !question.answer.sig.isEmpty(),
              "the marked answer loads back signed");
        check(!discussions.markAnswer(qn, "missing-comment", &err),
              "an answer must be a reply in the discussion");
        check(!discussions.markAnswer(dn, discussions.loadAll().first().events.value(1).id, &err),
              "only Q&A discussions take an answer");
        DiscussionEvent stranger;
        stranger.type = "answer";
        stranger.target = replyId;
        stranger.author = "someone-else";
        stranger.ts = QDateTime::currentMSecsSinceEpoch() + 1000;
        check(!discussions.applyRemoteEvent(qn, stranger, QString(), &err),
              "a remote answer from anyone but the opener is refused");
        DiscussionEvent clear;
        clear.type = "answer";
        clear.ts = QDateTime::currentMSecsSinceEpoch() + 2000;
        clear = discussions.makeSignedEvent(qn, clear);
        check(discussions.applyRemoteEvent(qn, clear, QString(), &err),
              "the opener can clear the answer remotely");
        for (const Discussion &d : discussions.loadAll())
            if (d.number == qn)
                question = d;
        check(question.answerId().isEmpty(), "a cleared answer loads as none");
        check(discussions.deleteDiscussion(qn, &err), "Q&A discussion delete succeeds");
        check(discussions.deleteDiscussion(dn, &err),
              "discussion delete succeeds");
        const QList<Discussion> afterDiscussionDelete = discussions.loadAll();
        check(std::none_of(afterDiscussionDelete.begin(), afterDiscussionDelete.end(),
                           [&](const Discussion &d) { return d.number == dn; }),
              "deleted discussion no longer loads");

        PullRequest reviewPr;
        reviewPr.number = 99;
        reviewPr.author = "reviewer-a";
        PullEvent approvingReview;
        approvingReview.type = "review";
        approvingReview.author = "reviewer-a";
        approvingReview.state = "approved";
        approvingReview.ts = 100;
        PullEvent blockingReview;
        blockingReview.type = "review";
        blockingReview.author = "reviewer-b";
        blockingReview.state = "changes_requested";
        blockingReview.ts = 110;
        reviewPr.events = {approvingReview, blockingReview};
        check(!reviewPr.independentReviewGateSatisfied() &&
                  reviewPr.independentApprovalCount() == 0,
              "pull author approval is ignored and a peer change request blocks merge");
        check(reviewPr.independentChangesRequestedBy() ==
                  QStringList{"reviewer-b"},
              "the blocking reviewer is named for the merge report");
        reviewPr.events.last().authorName = "Ada";
        check(reviewPr.independentChangesRequestedBy() == QStringList{"Ada"},
              "a signed display name is preferred over the signer key");
        reviewPr.events.last().state = "approved";
        check(reviewPr.independentReviewGateSatisfied() &&
                  reviewPr.independentApprovalCount() == 1 &&
                  reviewPr.independentChangesRequestedBy().isEmpty(),
              "one distinct peer approval satisfies the merge gate");
        check(PullMergePolicy::repositoryOwnedByAccount(
                  QStringLiteral("repo-node"), QStringLiteral("jett"),
                  {QStringLiteral("other-node"), QStringLiteral("REPO-NODE")}) &&
                  PullMergePolicy::repositoryOwnedByAccount(
                      QStringLiteral("Jett"), QStringLiteral("jett")) &&
                  !PullMergePolicy::repositoryOwnedByAccount(
                      QStringLiteral("repo-node"), QStringLiteral("jett"),
                      {QStringLiteral("other-node")}),
              "repository ownership recognizes account and linked-node aliases");
        const QJsonArray organizations{
            QJsonObject{{QStringLiteral("name"), QStringLiteral("core-team")},
                        {QStringLiteral("role"), QStringLiteral("owner")}},
            QJsonObject{{QStringLiteral("name"), QStringLiteral("writers")},
                        {QStringLiteral("role"), QStringLiteral("admin")}},
        };
        check(PullMergePolicy::repositoryOwnedByOrganizationOwner(
                  QStringLiteral("CORE-TEAM"), organizations) &&
                  !PullMergePolicy::repositoryOwnedByOrganizationOwner(
                      QStringLiteral("writers"), organizations) &&
                  !PullMergePolicy::repositoryOwnedByOrganizationOwner(
                      QStringLiteral("elsewhere"), organizations),
              "only an organization owner may waive its repository review gate");
        check(PullMergePolicy::reviewGateBlocks(true, false, false) &&
                  !PullMergePolicy::reviewGateBlocks(true, false, true) &&
                  PullMergePolicy::ownerOverrideRequired(true, false, true) &&
                  !PullMergePolicy::ownerOverrideRequired(true, false, false) &&
                  !PullMergePolicy::ownerOverrideRequired(true, true, true),
              "only a repository owner may waive an unmet required-review gate");
        PullEvent openThread;
        openThread.type = "thread-comment";
        openThread.id = "event-open";
        openThread.threadId = "thread-open";
        openThread.path = "src/a.cpp";
        openThread.side = "new";
        openThread.lineStart = 10;
        openThread.lineEnd = 12;
        openThread.body = "please guard this";
        openThread.suggestionPatch = "@@ -10 +10 @@\n-old\n+new\n";
        openThread.ts = 120;
        PullEvent openReply;
        openReply.type = "thread-reply";
        openReply.threadId = "thread-open";
        openReply.parentId = "event-open";
        openReply.body = "reply";
        openReply.ts = 130;
        PullEvent appliedSuggestion;
        appliedSuggestion.type = "suggestion-state";
        appliedSuggestion.threadId = "thread-open";
        appliedSuggestion.state = "applied";
        appliedSuggestion.appliedCommit = "abc123";
        appliedSuggestion.ts = 140;
        PullEvent resolvedThread;
        resolvedThread.type = "thread-comment";
        resolvedThread.id = "event-resolved";
        resolvedThread.threadId = "thread-resolved";
        resolvedThread.path = "src/a.cpp";
        resolvedThread.side = "old";
        resolvedThread.lineStart = 20;
        resolvedThread.lineEnd = 20;
        resolvedThread.body = "remove this";
        resolvedThread.ts = 150;
        PullEvent resolvedState;
        resolvedState.type = "thread-state";
        resolvedState.threadId = "thread-resolved";
        resolvedState.state = "resolved";
        resolvedState.ts = 160;
        PullEvent legacyLine;
        legacyLine.type = "line-comment";
        legacyLine.path = "src/b.cpp";
        legacyLine.side = "new";
        legacyLine.line = 5;
        legacyLine.body = "legacy inline note";
        legacyLine.ts = 170;
        reviewPr.events = {approvingReview, blockingReview, openThread,
                           openReply, appliedSuggestion, resolvedThread,
                           resolvedState, legacyLine};
        const PullReviewSnapshot snapshot = buildPullReviewSnapshot(reviewPr);
        check(snapshot.reviewSummary == "changes_requested",
              "review model folds latest blocking review state");
        check(snapshot.totalThreads == 3 && snapshot.unresolvedThreads == 2 &&
                  snapshot.resolvedThreads == 1,
              "review model counts resolved and unresolved threads");
        check(snapshot.suggestions == 1,
              "review model counts suggested changes");
        check(snapshot.files.value("src/a.cpp").totalThreads == 2 &&
                  snapshot.files.value("src/a.cpp").unresolvedThreads == 1,
              "review model folds per-file thread counts");
        check(snapshot.files.value("src/b.cpp").totalThreads == 1 &&
                  snapshot.files.value("src/b.cpp").unresolvedThreads == 1,
              "review model treats legacy line-comments as unresolved threads");

        // --- Thread placement across patchsets (issue #483) ----------------
        // A thread carries the patchset it was written against and the text
        // it commented on. Against a re-cut diff it stays on that text where
        // it still is, follows it where it moved, and is explicitly outdated
        // where it is gone — never painted on a line it did not talk about.
        {
            const QString headA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
            const QString headB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
            const QString patchA =
                "diff --git a/src/a.cpp b/src/a.cpp\n"
                "index 000..111 100644\n"
                "--- a/src/a.cpp\n+++ b/src/a.cpp\n"
                "@@ -1,4 +1,5 @@\n"
                " int a;\n"
                "-int old;\n"
                "+int fresh;\n"
                "+int extra;\n"
                " int b;\n"
                " int c;\n";
            // The re-push inserts two lines above the change and drops
            // `int extra;` entirely.
            const QString patchB =
                "diff --git a/src/a.cpp b/src/a.cpp\n"
                "index 000..222 100644\n"
                "--- a/src/a.cpp\n+++ b/src/a.cpp\n"
                "@@ -1,4 +1,6 @@\n"
                "+#include <x>\n"
                "+\n"
                " int a;\n"
                "-int old;\n"
                "+int fresh;\n"
                " int b;\n"
                " int c;\n";

            PullRequest revA;
            revA.number = 9;
            revA.patch = patchA;
            revA.patchHeadOid = headA;
            check(pullPatchsetId(revA) == headA,
                  "pullPatchsetId names the head the patch was cut against");
            check(pullAnchorLines(patchA, "src/a.cpp", "new", 2, 3) ==
                      "int fresh;\nint extra;",
                  "pullAnchorLines captures the new-side text without prefixes");
            check(pullAnchorLines(patchA, "src/a.cpp", "old", 2, 2) == "int old;",
                  "pullAnchorLines reads the old side by old line numbers");
            check(pullAnchorLines(patchA, "src/a.cpp", "new", 40, 41).isEmpty(),
                  "pullAnchorLines is empty outside the diff");
            const PullAnchor anchor =
                pullAnchorFor(revA, "src/a.cpp", "new", 2, 2);
            check(anchor.patchset == headA && anchor.anchorLines == "int fresh;",
                  "pullAnchorFor combines patchset and commented text");

            PullEvent stays;
            stays.type = "thread-comment";
            stays.id = "t-stays";
            stays.threadId = "t-stays";
            stays.path = "src/a.cpp";
            stays.side = "new";
            stays.lineStart = 2;
            stays.lineEnd = 2;
            stays.body = "rename this";
            stays.patchset = headA;
            stays.anchorLines = "int fresh;";
            PullEvent gone = stays;
            gone.id = "t-gone";
            gone.threadId = "t-gone";
            gone.lineStart = 3;
            gone.lineEnd = 3;
            gone.anchorLines = "int extra;";
            PullEvent range = stays;
            range.id = "t-range";
            range.threadId = "t-range";
            range.lineStart = 4;
            range.lineEnd = 5;
            range.anchorLines = "int b;\nint c;";
            PullEvent legacy;
            legacy.type = "line-comment";
            legacy.path = "src/a.cpp";
            legacy.side = "new";
            legacy.line = 5;
            legacy.body = "no anchor";
            PullEvent legacyGone = legacy;
            legacyGone.line = 9;
            revA.events = {stays, gone, range, legacy};

            const PullReviewSnapshot same = buildPullReviewSnapshot(revA);
            bool allCurrent = same.threads.size() == 4;
            for (const PullReviewThread &t : same.threads) {
                if (t.events.first().type == "line-comment") {
                    allCurrent = allCurrent &&
                                 t.placement == PullThreadPlacement::Legacy &&
                                 t.currentLineStart == 5;
                } else {
                    allCurrent = allCurrent &&
                                 t.placement == PullThreadPlacement::Current &&
                                 t.currentLineStart == t.lineStart &&
                                 t.currentLineEnd == t.lineEnd;
                }
            }
            check(allCurrent && same.outdatedThreads == 0,
                  "threads on their own patchset are current at their lines");

            PullRequest revB = revA;
            revB.patch = patchB;
            revB.patchHeadOid = headB;
            revB.events.append(legacyGone);
            const PullReviewSnapshot moved = buildPullReviewSnapshot(revB);
            QHash<QString, PullReviewThread> byId;
            for (const PullReviewThread &t : moved.threads)
                byId.insert(t.id, t);
            const PullReviewThread s = byId.value("t-stays");
            check(s.placement == PullThreadPlacement::Moved &&
                      s.currentLineStart == 4 && s.currentLineEnd == 4,
                  "a thread follows its commented text to its new line");
            check(pullThreadPlacementLabel(s) == "Moved from line 2",
                  "moved threads say where they were written");
            const PullReviewThread g = byId.value("t-gone");
            check(g.placement == PullThreadPlacement::Outdated &&
                      g.currentLineStart == 0 && g.outdated(),
                  "a thread whose text left the diff is outdated, not "
                  "painted on the line that now has its number");
            check(pullThreadPlacementLabel(g) == "Outdated",
                  "outdated threads carry an explicit label");
            const PullReviewThread r = byId.value("t-range");
            check(r.placement == PullThreadPlacement::Moved &&
                      r.currentLineStart == 5 && r.currentLineEnd == 6,
                  "a multi-line thread moves as a block");
            int legacyKept = 0, legacyOutdated = 0;
            for (const PullReviewThread &t : moved.threads) {
                if (t.events.first().type != "line-comment")
                    continue;
                if (t.placement == PullThreadPlacement::Legacy &&
                    t.currentLineStart == t.lineStart)
                    ++legacyKept;
                if (t.placement == PullThreadPlacement::Outdated)
                    ++legacyOutdated;
            }
            check(legacyKept == 1 && legacyOutdated == 1,
                  "pre-anchor threads stay where they say while the line "
                  "exists and are outdated once it does not");
            check(moved.outdatedThreads == 2 &&
                      moved.files.value("src/a.cpp").outdatedThreads == 2,
                  "snapshot counts outdated threads in total and per file");

            // Same head, patch text re-derived: no placement work is needed.
            PullRequest sameHead = revA;
            sameHead.patch = patchB;
            const PullReviewSnapshot trusted = buildPullReviewSnapshot(sameHead);
            bool trustedCurrent = true;
            for (const PullReviewThread &t : trusted.threads)
                if (t.events.first().type == "thread-comment" &&
                    t.placement != PullThreadPlacement::Current)
                    trustedCurrent = false;
            check(trustedCurrent,
                  "a matching patchset id places threads without text search");

            // A patchset but no text against a different head: outdated,
            // because nothing proves the line still holds the same code.
            PullEvent blind = stays;
            blind.id = "t-blind";
            blind.threadId = "t-blind";
            blind.anchorLines.clear();
            PullRequest blindPr = revB;
            blindPr.events = {blind};
            const PullReviewSnapshot blindSnap = buildPullReviewSnapshot(blindPr);
            check(blindSnap.threads.size() == 1 &&
                      blindSnap.threads.first().outdated(),
                  "a different patchset with no anchored text is outdated");
            // ...unless the current diff has no named patchset at all (an
            // immutable patch-backed pull), where the line stands.
            blindPr.patchHeadOid.clear();
            blindPr.creationHeadOid.clear();
            const PullReviewSnapshot blindPatch = buildPullReviewSnapshot(blindPr);
            check(blindPatch.threads.size() == 1 &&
                      blindPatch.threads.first().placement ==
                          PullThreadPlacement::Current,
                  "an unnamed current patchset keeps a bare-patchset thread "
                  "on its existing line");
        }

        // --- Agent provenance from signed commit trailers (issue #365) --------
        // A PR whose commit series carries the ForkMesh-Agent trailer is
        // attributable to a tool/model without any local AgentSession; the
        // per-file map lets the review UI filter by authorship.
        {
            const QString agentMbox =
                "From 1111111 Mon Sep 17 00:00:00 2001\n"
                "From: Bot <bot@example.com>\n"
                "Subject: [PATCH 1/2] agent change\n\n"
                "Body line.\n\n"
                "ForkMesh-Agent: claude-code/claude-opus-4-8\n"
                "---\n"
                "diff --git a/src/agent.cpp b/src/agent.cpp\n"
                "index 000..111 100644\n"
                "--- a/src/agent.cpp\n+++ b/src/agent.cpp\n"
                "@@ -0,0 +1 @@\n+agent\n"
                "From 2222222 Mon Sep 17 00:00:00 2001\n"
                "From: Human <dev@example.com>\n"
                "Subject: [PATCH 2/2] human change\n\n"
                "Hand-written.\n"
                "---\n"
                "diff --git a/src/human.cpp b/src/human.cpp\n"
                "index 000..222 100644\n"
                "--- a/src/human.cpp\n+++ b/src/human.cpp\n"
                "@@ -0,0 +1 @@\n+human\n";
            PullRequest agentPr;
            agentPr.commits = agentMbox;
            const PullAgentProvenance prov = pullAgentProvenance(agentPr);
            check(prov.isAgent && prov.tool == "claude-code" &&
                      prov.model == "claude-opus-4-8",
                  "pullAgentProvenance reads the ForkMesh-Agent trailer");
            const QHash<QString, bool> authorship = pullFileAuthorship(agentPr);
            check(authorship.value("src/agent.cpp") == true &&
                      authorship.value("src/human.cpp") == false,
                  "pullFileAuthorship maps each file to its commit's authorship");

            PullRequest humanPr;
            humanPr.commits =
                "From 3333333 Mon Sep 17 00:00:00 2001\n"
                "From: Human <dev@example.com>\n"
                "Subject: [PATCH] plain change\n\n"
                "No trailer here.\n"
                "---\n"
                "diff --git a/src/x.cpp b/src/x.cpp\n";
            check(!pullAgentProvenance(humanPr).isAgent,
                  "pullAgentProvenance treats an un-trailered PR as human-authored");
        }

        // --- PullStore deletePullFile excises one file, keeps the rest -------
        // Regression for issue #258: deleting a file used to rebuild the PR by
        // replaying its commits onto the current base with `git am`, which drops
        // commits already present on the base and so could wipe most of the PR.
        // The delete now edits the stored diff/commits in place.
        {
            const QString baseBranch = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            auto writeFile = [&](const QString &rel, const QString &text) {
                check(writeTestFile(tmp.path() + "/" + rel, text.toUtf8()),
                      "write pull-delete fixture file");
            };
            git({"checkout", "-q", "-b", "feat-258"});
            writeFile("alpha.txt", "alpha-1\n");
            writeFile("beta.txt", "beta-1\n");
            git({"add", "alpha.txt", "beta.txt"});
            git({"commit", "-q", "-m", "add alpha and beta"});
            writeFile("beta.txt", "beta-2\n"); // a commit that touches only beta
            git({"add", "beta.txt"});
            git({"commit", "-q", "-m", "tweak beta"});
            writeFile("gamma.txt", "gamma-1\n");
            git({"add", "gamma.txt"});
            git({"commit", "-q", "-m", "add gamma"});
            const QString delPatch =
                QString::fromUtf8(gitOutput({"diff", baseBranch + "..feat-258"}));
            const QString delMbox = QString::fromUtf8(
                gitOutput({"format-patch", "--stdout", baseBranch + "..feat-258"}));
            git({"checkout", "-q", baseBranch});

            const int dn = pulls.createPull("Three files", "body", baseBranch,
                                            "feat-258", delPatch, delMbox,
                                            /*branchBacked=*/false, &err);
            check(dn > 0, "createPull stores a multi-file PR with a commit series");
            check(pulls.deletePullFile(dn, "beta.txt", &err),
                  "deletePullFile removes one file from the PR");

            PullRequest afterDel;
            for (const PullRequest &p : pulls.loadAll())
                if (p.number == dn)
                    afterDel = p;
            check(afterDel.patch.contains("alpha.txt") &&
                      afterDel.patch.contains("gamma.txt"),
                  "deleting one file leaves the other files in the patch (#258)");
            check(!afterDel.patch.contains("beta.txt"),
                  "the deleted file is gone from the patch");
            check(afterDel.filesChanged == 2,
                  "stats drop by exactly the one deleted file");
            check(afterDel.commits.contains("add alpha and beta") &&
                      afterDel.commits.contains("add gamma"),
                  "multi-file commits survive with their other changes intact");
            check(!afterDel.commits.contains("tweak beta") &&
                      !afterDel.commits.contains("beta.txt"),
                  "a commit touching only the deleted file is dropped from the series");

            const QString staleAm =
                tmp.path() + "/" +
                QString::fromUtf8(
                    gitOutput({"rev-parse", "--git-path", "rebase-apply"}).trimmed());
            check(QDir().mkpath(staleAm) &&
                      writeTestFile(staleAm + "/applying", QByteArray()) &&
                      writeTestFile(staleAm + "/next", QByteArray("1\n")) &&
                      writeTestFile(staleAm + "/last", QByteArray("1\n")),
                  "plant a stale rebase-apply left by an interrupted git am");
            check(!pulls.conflictMergeInProgress(),
                  "a stale am directory is not reported as a live conflict session");
            // The trimmed commit series must still be a valid, appliable patch.
            check(pulls.mergePull(dn, &err, false),
                  "the PR still merges cleanly after a file was deleted from it");
            check(!QFileInfo::exists(staleAm),
                  "merging clears the stale rebase-apply instead of failing on it");
            check(QFile::exists(tmp.path() + "/alpha.txt") &&
                      QFile::exists(tmp.path() + "/gamma.txt") &&
                      !QFile::exists(tmp.path() + "/beta.txt"),
                  "merging applies the kept files and not the deleted one");
            const QString mergedStoredDir =
                pulls.metaWorkTree() + "/.forkmesh/pulls/" + QString::number(dn);
            check(!QFile::exists(mergedStoredDir + "/changes.patch") &&
                      !QFile::exists(mergedStoredDir + "/commits.mbox"),
                  "merging removes a stored PR's patch and commit payload");
            PullRequest mergedStored;
            for (const PullRequest &p : pulls.loadAll())
                if (p.number == dn)
                    mergedStored = p;
            check(mergedStored.status == "merged" &&
                      mergedStored.patch.isEmpty() &&
                      mergedStored.commits.isEmpty(),
                  "a merged stored PR exposes no diff or commit payload");
            QProcess payloadHistory;
            payloadHistory.start(
                "git",
                {"-C", pulls.metaWorkTree(), "log", "--format=%H", "--all", "--",
                 QStringLiteral(".forkmesh/pulls/%1/changes.patch").arg(dn),
                 QStringLiteral(".forkmesh/pulls/%1/commits.mbox").arg(dn)});
            payloadHistory.waitForFinished(30000);
            check(payloadHistory.exitStatus() == QProcess::NormalExit &&
                      payloadHistory.exitCode() == 0 &&
                      payloadHistory.readAllStandardOutput().trimmed().isEmpty(),
                  "merging purges the stored payload from ledger and metadata-ref history");
        }

        // --- PullStore deletePull works with a dirty working tree -----------
        // Regression for adhoc #214: a plain delete commits path-scoped
        // (`commit -- pulls/<n>`), so unrelated tracked edits elsewhere (e.g.
        // uncommitted changes on main) never enter it and must not block the
        // deletion. Only the opt-in history rewrite needs a clean tree.
        {
            const QString baseBranch = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            check(writeTestFile(tmp.path() + "/del214.txt", "v1\n"),
                  "write pull-delete-dirty fixture file");
            git({"add", "del214.txt"});
            git({"commit", "-q", "-m", "add del214"});
            const QString patch214 =
                "diff --git a/del214.txt b/del214.txt\n"
                "index 000..111 100644\n--- a/del214.txt\n+++ b/del214.txt\n"
                "@@ -1 +1 @@\n-v1\n+v2\n";
            const int pn = pulls.createPull("Dirty tree", "body", baseBranch,
                                            "feat-214", patch214, QString(),
                                            /*branchBacked=*/false, &err);
            check(pn > 0, "createPull stores a PR to delete against a dirty tree");

            // Dirty the working tree with an unrelated tracked change.
            check(writeTestFile(tmp.path() + "/del214.txt", "dirty\n"),
                  "make an unrelated tracked change before deleting");

            QString delErr;
            check(pulls.deletePull(pn, /*rewriteHistory=*/false, &delErr),
                  "deletePull succeeds even with unrelated tracked changes");
            bool stillThere = false;
            for (const PullRequest &p : pulls.loadAll())
                if (p.number == pn)
                    stillThere = true;
            check(!stillThere, "the deleted PR is gone from the store");
            // Deleting it again is the same requested outcome, and callers that
            // tear down a PR with its branch recognize that reply so they finish
            // the rest of the cleanup instead of stopping on it (adhoc #2088).
            QString goneErr;
            check(!pulls.deletePull(pn, /*rewriteHistory=*/false, &goneErr) &&
                      PullStore::missingPullError(goneErr, pn),
                  "a second deletePull reports the record as already missing");
            check(!PullStore::missingPullError(
                      QStringLiteral("git commit failed: not found"), pn),
                  "missingPullError does not swallow unrelated delete failures");
            // Leave the tree clean for later blocks.
            git({"checkout", "-q", "--", "del214.txt"});
        }

        // --- the opt-in history rewrite cleans up after itself (adhoc #147) --
        // The rewrite used to hand "filter-branch -d" the QTemporaryDir it had
        // just created. filter-branch deletes and recreates that exact
        // directory, so the two cleanups raced and Qt reported the loss as
        // "QTemporaryDir: Unable to remove ... most likely due to the presence
        // of read-only files" while leaking the directory into the temp folder.
        {
            // A fresh QDir per listing: QDir caches its entry list.
            const auto scratchDirs = [] {
                return QDir(QDir::tempPath())
                    .entryList({QStringLiteral("forkmesh-filter-branch-*")},
                               QDir::Dirs | QDir::NoDotAndDotDot);
            };
            const QStringList scratchBefore = scratchDirs();
            const QString baseBranch = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            const QString patch147 =
                "diff --git a/scrub147.txt b/scrub147.txt\n"
                "index 000..111 100644\n--- a/scrub147.txt\n+++ b/scrub147.txt\n"
                "@@ -1 +1 @@\n-v1\n+v2\n";
            const int pn = pulls.createPull("Scrubbed", "body", baseBranch,
                                            "feat-147", patch147, QString(),
                                            /*branchBacked=*/false, &err);
            check(pn > 0, "createPull stores a PR to delete with a history rewrite");
            const QString relPath = QStringLiteral(".forkmesh/pulls/%1").arg(pn);

            QStringList captured;
            capturedMessages = &captured;
            QtMessageHandler previous = qInstallMessageHandler(captureMessages);
            QString scrubErr;
            const bool scrubbed =
                pulls.deletePull(pn, /*rewriteHistory=*/true, &scrubErr);
            qInstallMessageHandler(previous); // restore so PASS/FAIL output prints
            capturedMessages = nullptr;
            if (!scrubbed)
                std::fprintf(stderr, "history rewrite failed: %s\n",
                             scrubErr.toUtf8().constData());
            check(scrubbed, "deletePull rewrites history when asked to");

            QProcess scrubbedHistory;
            scrubbedHistory.start("git", {"-C", pulls.metaWorkTree(), "log",
                                          "--format=%H", "--all", "--", relPath});
            scrubbedHistory.waitForFinished(30000);
            check(scrubbedHistory.exitStatus() == QProcess::NormalExit &&
                      scrubbedHistory.exitCode() == 0 &&
                      scrubbedHistory.readAllStandardOutput().trimmed().isEmpty(),
                  "the rewrite purges the deleted pull from ledger history");

            QStringList leaked = scratchDirs();
            for (const QString &known : scratchBefore)
                leaked.removeAll(known);
            if (!leaked.isEmpty())
                std::fprintf(stderr, "leaked rewrite scratch: %s\n",
                             leaked.join(QLatin1Char(' ')).toUtf8().constData());
            check(leaked.isEmpty(),
                  "the history rewrite leaves no scratch directory behind");
            check(!captured.join(QLatin1Char('\n'))
                       .contains(QStringLiteral("Unable to remove")),
                  "the history rewrite reports no temp-directory removal warning");
        }

        // --- PullStore deletePulls removes a batch in one ledger commit -------
        // "Delete closed and merged" hands every finished pull to deletePulls:
        // one commit for the batch, absent records reported rather than fatal,
        // and every materialized ref gone afterwards.
        {
            const QString baseBranch = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            QList<int> batch;
            for (int i = 0; i < 3; ++i) {
                const QString patch =
                    QStringLiteral("diff --git a/bulk%1.txt b/bulk%1.txt\n"
                                   "new file mode 100644\n--- /dev/null\n"
                                   "+++ b/bulk%1.txt\n@@ -0,0 +1 @@\n+bulk %1\n")
                        .arg(i);
                const int pn = pulls.createPull(
                    QStringLiteral("Bulk %1").arg(i), "body", baseBranch,
                    QStringLiteral("feat-bulk-%1").arg(i), patch, QString(),
                    /*branchBacked=*/false, &err);
                check(pn > 0, "createPull stores a PR for the bulk delete");
                batch << pn;
            }
            // Count the ledger's own history: --all would shrink by the
            // per-pull metadata refs the delete tears down.
            const QString ledgerRef =
                gitOutput({"rev-parse", "--verify", "-q",
                           "refs/heads/forkmesh/pulls"}).trimmed().isEmpty()
                    ? QStringLiteral("HEAD")
                    : QStringLiteral("refs/heads/forkmesh/pulls");
            const int commitsBefore =
                gitOutput({"rev-list", "--count", ledgerRef}).trimmed().toInt();
            QList<int> deleted, missing;
            QString bulkErr;
            check(pulls.deletePulls(batch + QList<int>{batch.last() + 1000},
                                    /*rewriteHistory=*/false, &deleted, &missing,
                                    &bulkErr),
                  "deletePulls succeeds for a batch with one absent number");
            check(deleted == batch, "deletePulls reports every removed record");
            check(missing == QList<int>{batch.last() + 1000},
                  "deletePulls reports the absent record instead of failing");
            const int commitsAfter =
                gitOutput({"rev-list", "--count", ledgerRef}).trimmed().toInt();
            check(commitsAfter == commitsBefore + 1,
                  "the whole batch is one ledger commit");
            check(gitOutput({"log", "-1", "--format=%s", ledgerRef})
                      .contains(QStringLiteral("#%1").arg(batch.first()).toUtf8()),
                  "the batch commit names the deleted pulls");
            bool anyLeft = false;
            for (const PullRequest &p : pulls.loadAll())
                if (batch.contains(p.number))
                    anyLeft = true;
            check(!anyLeft, "deleted pulls are gone from the store");
            for (int pn : std::as_const(batch)) {
                check(gitOutput({"for-each-ref",
                                 QStringLiteral("refs/pr/%1/").arg(pn),
                                 QStringLiteral("refs/heads/pr/%1").arg(pn),
                                 QStringLiteral("refs/heads/forkmesh/pull/%1").arg(pn)})
                          .trimmed()
                          .isEmpty(),
                      "deletePulls drops the materialized refs of every pull");
            }
        }

        // --- PullStore branch-backed PRs keep the diff out of the repo -------
        // A PR whose head is a real branch stores only the signed pull.md
        // pointer; its diff and full commit series are reconstructed from
        // base..head, and the branch's own commits (with authorship) drive the
        // merge — nothing about the diff is committed into the repo.
        {
            const QString baseBranch = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            auto writeFile = [&](const QString &rel, const QString &text) {
                check(writeTestFile(tmp.path() + "/" + rel, text.toUtf8()),
                      "write branch-backed fixture file");
            };
            git({"checkout", "-q", "-b", "feat-bb"});
            writeFile("bb1.txt", "one\n");
            git({"add", "bb1.txt"});
            git({"commit", "-q", "-m", "bb: add one"});
            writeFile("bb2.txt", "two\n");
            git({"add", "bb2.txt"});
            git({"commit", "-q", "-m", "bb: add two"});
            git({"checkout", "-q", baseBranch});

            const int bn =
                pulls.createPull("Branch backed", "body", baseBranch, "feat-bb",
                                 QString(), QString(), /*branchBacked=*/true, &err);
            check(bn > 0, "createPull stores a branch-backed PR");
            // pulls/ metadata lives on its own linked worktree, not tmp.path()
            // (issue #399) - it never lands on whatever's checked out there.
            const QString metaDir = pulls.metaWorkTree();
            auto metaGitOutput = [&](const QStringList &args) {
                QProcess p;
                p.start("git", QStringList{"-C", metaDir} + args);
                p.waitForFinished(8000);
                return p.readAllStandardOutput();
            };
            const QString bdir = metaDir + "/.forkmesh/pulls/" + QString::number(bn);
            check(!QFile::exists(bdir + "/changes.patch"),
                  "a branch-backed PR writes no changes.patch into the repo");
            check(QFile::exists(bdir + "/pull.md"),
                  "a branch-backed PR still records its signed pull.md pointer");
            const QString tracked = QString::fromUtf8(metaGitOutput(
                {"ls-tree", "-r", "--name-only", "HEAD",
                 ".forkmesh/pulls/" + QString::number(bn)}));
            check(tracked.contains("pull.md") &&
                      !tracked.contains("changes.patch") &&
                      !tracked.contains("commits.mbox"),
                  "git tracks only pull.md for a branch-backed PR");

            PullRequest bb;
            for (const PullRequest &p : pulls.loadAll())
                if (p.number == bn)
                    bb = p;
            check(bb.patch.contains("bb1.txt") && bb.patch.contains("bb2.txt"),
                  "a branch-backed PR's diff is reconstructed from the refs");
            check(bb.commits.contains("bb: add one") &&
                      bb.commits.contains("bb: add two"),
                  "a branch-backed PR's full commit series is reconstructed");
            check(bb.filesChanged == 2,
                  "branch-backed stats come from the reconstructed diff");

            check(pulls.mergePull(bn, &err, false),
                  "a branch-backed PR merges by replaying its commits");
            check(QFile::exists(tmp.path() + "/bb1.txt") &&
                      QFile::exists(tmp.path() + "/bb2.txt"),
                  "merging a branch-backed PR applies its files");
            const QString log = QString::fromUtf8(gitOutput({"log", "--format=%s"}));
            check(log.contains("bb: add one") && log.contains("bb: add two"),
                  "merging a branch-backed PR preserves every authored commit");
            PullRequest merged;
            for (const PullRequest &p : pulls.loadAll())
                if (p.number == bn)
                    merged = p;
            check(merged.status == "merged",
                  "the branch-backed PR is marked merged");
            check(merged.patch.isEmpty() && merged.commits.isEmpty(),
                  "a merged branch-backed PR exposes no diff or commit payload");
            check(!QFile::exists(bdir + "/changes.patch") &&
                      !QFile::exists(bdir + "/commits.mbox"),
                  "a merged branch-backed PR stores metadata only");
        }

        // --- PullStore agent edit: multi-file changes on the PR's branch -----
        // The "Fix all with AI" flow (adhoc #82): check out the PR's branch
        // with the PR applied, let an agent edit any number of files, then
        // commit the lot on the branch and regenerate the PR's patch.
        {
            const QString baseBranch = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            auto writeFile = [&](const QString &rel, const QString &text) {
                check(writeTestFile(tmp.path() + "/" + rel, text.toUtf8()),
                      "write agent-edit fixture file");
            };
            git({"checkout", "-q", "-b", "feat-ai"});
            writeFile("ai1.txt", "first draft\n");
            git({"add", "ai1.txt"});
            git({"commit", "-q", "-m", "ai: add draft"});
            git({"checkout", "-q", baseBranch});

            const int an = pulls.createPull("Agent editable", "body", baseBranch,
                                            "feat-ai", QString(), QString(),
                                            /*branchBacked=*/true, &err);
            check(an > 0, "createPull stores the agent-editable PR");

            // The user's own checkout is dirty and sitting on the base branch —
            // neither must matter (adhoc #437): the edit runs in a scratch
            // worktree checked out at the PR's branch.
            writeFile("local-wip.txt", "uncommitted work\n");
            check(pulls.startPullAgentEdit(an, &err),
                  "startPullAgentEdit opens the PR's branch with a dirty base tree");
            const QString editDir = pulls.agentEditWorkTree();
            check(!editDir.isEmpty() && QFile::exists(editDir + "/ai1.txt"),
                  "the agent edits in a worktree holding the PR's branch content");
            check(QString::fromUtf8(
                      gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed()) ==
                      baseBranch,
                  "the user's checkout stays on its own branch during an agent edit");
            check(QFile::exists(tmp.path() + "/local-wip.txt"),
                  "the user's uncommitted work is untouched by an agent edit");
            check(writeTestFile(editDir + "/ai1.txt", QByteArray("fixed draft\n")) &&
                      writeTestFile(editDir + "/ai2.txt", QByteArray("brand new\n")),
                  "write the agent's edits into the edit worktree");
            check(pulls.finishPullAgentEdit(
                      an, QStringLiteral("pull #%1: apply AI review fixes").arg(an),
                      &err),
                  "finishPullAgentEdit commits the edits and finalizes");
            check(pulls.agentEditWorkTree().isEmpty() && !QFile::exists(editDir),
                  "the edit worktree is torn down once the fixes are committed");
            check(QString::fromUtf8(
                      gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed()) ==
                      baseBranch,
                  "finishing an agent edit leaves the user's checkout where it was");
            check(!pulls.conflictMergeInProgress(),
                  "no am session is left open after an agent edit");
            check(QString::fromUtf8(gitOutput({"log", "--format=%s", "-1", "feat-ai"})
                                        .trimmed()) ==
                      QStringLiteral("pull #%1: apply AI review fixes").arg(an),
                  "the fixes land on the PR's own branch");
            PullRequest edited;
            for (const PullRequest &p : pulls.loadAll())
                if (p.number == an)
                    edited = p;
            check(edited.status == "open",
                  "the PR stays open after an agent edit");
            check(edited.patch.contains("fixed draft") &&
                      edited.patch.contains("ai2.txt"),
                  "the PR's patch regenerates with every agent-edited file");
            check(!edited.patch.contains("local-wip.txt"),
                  "the base tree's uncommitted files stay out of the PR");

            // An agent run that changes nothing must refuse to commit, drop its
            // worktree, and leave the PR's branch exactly where it was.
            const QString beforeNoOp =
                QString::fromUtf8(gitOutput({"rev-parse", "feat-ai"}).trimmed());
            check(pulls.startPullAgentEdit(an, &err),
                  "a second agent-edit session opens on the same PR");
            const QString noOpDir = pulls.agentEditWorkTree();
            check(!pulls.finishPullAgentEdit(an, QStringLiteral("no-op"), &err),
                  "an agent session with no changes refuses to commit");
            check(!noOpDir.isEmpty() && !QFile::exists(noOpDir),
                  "a no-op agent edit tears its worktree down");
            check(QString::fromUtf8(gitOutput({"rev-parse", "feat-ai"}).trimmed()) ==
                      beforeNoOp,
                  "a no-op agent edit leaves the PR's branch untouched");
            // A stored-patch PR has no branch to edit at, so the scratch
            // worktree starts at the base and replays the patch there — again
            // without needing the user's checkout to be clean.
            check(writeTestFile(tmp.path() + "/ap1.txt", "v1\n"),
                  "write patch-PR fixture file");
            git({"add", "ap1.txt"});
            git({"commit", "-q", "-m", "add ap1"});
            const QString patchAp =
                "diff --git a/ap1.txt b/ap1.txt\n"
                "index 000..111 100644\n--- a/ap1.txt\n+++ b/ap1.txt\n"
                "@@ -1 +1 @@\n-v1\n+v2\n";
            const int pn = pulls.createPull("Patch backed", "body", baseBranch,
                                            "feat-ai-patch", patchAp, QString(),
                                            /*branchBacked=*/false, &err);
            check(pn > 0, "createPull stores the patch-backed PR");
            check(pulls.startPullAgentEdit(pn, &err),
                  "startPullAgentEdit replays a patch-backed PR into its worktree");
            const QString patchDir = pulls.agentEditWorkTree();
            QFile replayed(patchDir + "/ap1.txt");
            check(!patchDir.isEmpty() && replayed.open(QIODevice::ReadOnly) &&
                      replayed.readAll() == QByteArray("v2\n"),
                  "the replayed patch is what the agent edits");
            replayed.close();
            check(writeTestFile(patchDir + "/ap1.txt", QByteArray("v3\n")),
                  "write the agent's edit to the replayed patch");
            check(pulls.finishPullAgentEdit(pn, QStringLiteral("pull: fix"), &err),
                  "finishPullAgentEdit commits a patch-backed PR's edits");
            PullRequest patched;
            for (const PullRequest &p : pulls.loadAll())
                if (p.number == pn)
                    patched = p;
            check(patched.patch.contains("+v3"),
                  "the patch-backed PR's diff picks up the agent's edit");

            // Conflict fixing uses the same isolation guarantee. Build a PR
            // whose branch and current base changed the same line, then leave
            // both tracked and untracked work in the user's checkout.
            check(writeTestFile(tmp.path() + "/agent-conflict.txt",
                                QByteArray("shared\n")),
                  "write the conflict-agent base fixture");
            git({"add", "agent-conflict.txt"});
            git({"commit", "-q", "-m", "add conflict-agent fixture"});
            git({"checkout", "-q", "-b", "feat-agent-conflict"});
            check(writeTestFile(tmp.path() + "/agent-conflict.txt",
                                QByteArray("from pull\n")),
                  "write the conflict-agent pull side");
            git({"add", "agent-conflict.txt"});
            git({"commit", "-q", "-m", "change conflict-agent fixture in pull"});
            git({"checkout", "-q", baseBranch});
            check(writeTestFile(tmp.path() + "/agent-conflict.txt",
                                QByteArray("from base\n")),
                  "write the conflict-agent base side");
            git({"add", "agent-conflict.txt"});
            git({"commit", "-q", "-m", "change conflict-agent fixture on base"});
            const int cn = pulls.createPull(
                "Agent conflict", "body", baseBranch, "feat-agent-conflict",
                QString(), QString(), /*branchBacked=*/true, &err);
            check(cn > 0, "createPull stores the conflict-agent PR");
            check(writeTestFile(tmp.path() + "/ap1.txt",
                                QByteArray("uncommitted local edit\n")),
                  "leave a tracked user edit before conflict-agent work");
            QStringList conflictFiles;
            bool conflictClean = false;
            check(pulls.startConflictAgentEdit(
                      cn, &conflictFiles, &conflictClean, &err),
                  "conflict agent starts while the user's tree is dirty");
            const QString conflictDir = pulls.agentEditWorkTree();
            check(!conflictClean && !conflictDir.isEmpty() &&
                      conflictFiles.contains("agent-conflict.txt"),
                  "the agent receives conflict markers in its scratch worktree");
            QFile localDirty(tmp.path() + "/ap1.txt");
            check(localDirty.open(QIODevice::ReadOnly) &&
                      localDirty.readAll() == QByteArray("uncommitted local edit\n"),
                  "starting conflict resolution leaves tracked local work untouched");
            localDirty.close();
            check(writeTestFile(conflictDir + "/agent-conflict.txt",
                                QByteArray("resolved by agent\n")),
                  "resolve the conflict inside the scratch worktree");
            check(pulls.finishConflictMerge(cn, &err),
                  "finishConflictMerge commits the isolated agent resolution");
            check(pulls.agentEditWorkTree().isEmpty() &&
                      !QFile::exists(conflictDir),
                  "finishing conflict resolution removes its scratch worktree");
            check(localDirty.open(QIODevice::ReadOnly) &&
                      localDirty.readAll() == QByteArray("uncommitted local edit\n"),
                  "finishing conflict resolution leaves tracked local work untouched");
            localDirty.close();
            PullRequest conflictResolved;
            for (const PullRequest &p : pulls.loadAll())
                if (p.number == cn)
                    conflictResolved = p;
            check(conflictResolved.status == "open" &&
                      conflictResolved.head == QStringLiteral("feat-agent-conflict") &&
                      conflictResolved.patch.contains("resolved by agent") &&
                      !gitOutput({"rev-parse", "--verify", "--quiet",
                                  "refs/heads/feat-agent-conflict"})
                           .trimmed()
                           .isEmpty() &&
                      gitOutput({"rev-parse", "--verify", "--quiet",
                                 QStringLiteral("refs/heads/pull/%1-agent-fix")
                                     .arg(cn)})
                          .trimmed()
                          .isEmpty(),
                  "the resolved PR stays open on its own branch without an agent-fix branch");

            // Leave the tree clean again for the tests that follow.
            git({"checkout", "--", "ap1.txt"});
            QFile::remove(tmp.path() + "/local-wip.txt");

            // Cross-node PR heads are informational "<node>:<branch>" labels,
            // not local Git branch names.  The desktop conflict dialog must
            // replay one on pull/<n> instead of passing the colon to checkout.
            check(writeTestFile(tmp.path() + "/remote-conflict.txt",
                                QByteArray("shared\n")),
                  "write the cross-node conflict base fixture");
            git({"add", "remote-conflict.txt"});
            git({"commit", "-q", "-m", "add cross-node conflict fixture"});
            git({"checkout", "-q", "-b", "remote-conflict-source"});
            check(writeTestFile(tmp.path() + "/remote-conflict.txt",
                                QByteArray("from remote pull\n")),
                  "write the cross-node pull side");
            git({"add", "remote-conflict.txt"});
            git({"commit", "-q", "-m", "change cross-node pull side"});
            const QString remotePatch = QString::fromUtf8(
                gitOutput({"diff", baseBranch + "..remote-conflict-source"}));
            const QString remoteMbox = QString::fromUtf8(gitOutput(
                {"format-patch", "--stdout",
                 baseBranch + "..remote-conflict-source"}));
            git({"checkout", "-q", baseBranch});
            check(writeTestFile(tmp.path() + "/remote-conflict.txt",
                                QByteArray("from local base\n")),
                  "write the local side of the cross-node conflict");
            git({"add", "remote-conflict.txt"});
            git({"commit", "-q", "-m", "change cross-node base side"});
            git({"branch", "-D", "remote-conflict-source"});

            const int remoteConflictNumber = pulls.createPull(
                "Cross-node conflict", "body", baseBranch,
                "gaurav2631:fix/pr-delivery-error-detail", remotePatch,
                remoteMbox, /*branchBacked=*/false, &err);
            check(remoteConflictNumber > 0,
                  "createPull stores a cross-node conflict PR");
            QStringList remoteConflictFiles;
            bool remoteConflictClean = false;
            err.clear();
            check(pulls.startConflictMerge(remoteConflictNumber,
                                           &remoteConflictFiles,
                                           &remoteConflictClean, &err),
                  "the desktop conflict flow accepts a cross-node head label");
            const QString conflictBranch =
                QStringLiteral("pull/%1").arg(remoteConflictNumber);
            check(!remoteConflictClean &&
                      remoteConflictFiles.contains("remote-conflict.txt") &&
                      QString::fromUtf8(gitOutput(
                          {"rev-parse", "--abbrev-ref", "HEAD"}).trimmed()) ==
                          conflictBranch,
                  "cross-node conflicts open on the PR's canonical local branch");
            {
                PullStore other(tmp.path(), QString(), &identity, "tester");
                QString liveErr;
                check(other.conflictMergeInProgress() &&
                          !other.mergePull(remoteConflictNumber, &liveErr, false) &&
                          liveErr.contains(QLatin1String("already in progress")) &&
                          QString::fromUtf8(gitOutput(
                              {"rev-parse", "--abbrev-ref", "HEAD"}).trimmed()) ==
                              conflictBranch,
                      "a merge leaves another store's live conflict session alone");
            }
            pulls.abortConflictMerge();
            check(QString::fromUtf8(gitOutput(
                      {"rev-parse", "--abbrev-ref", "HEAD"}).trimmed()) ==
                      baseBranch &&
                      gitOutput({"rev-parse", "--verify", "--quiet",
                                 "refs/heads/" + conflictBranch})
                          .trimmed()
                          .isEmpty(),
                  "cancelling cross-node conflict resolution restores the base");
        }

        // --- A branch-backed PR survives a corrupt stored blob by rebuilding
        // --- its commits from the mirror's refs ------------------------------
        // Reproduces the "corrupt binary patch" failure: a PR pushed from
        // another node whose head branch has not landed in this working tree
        // falls back to the committed commits.mbox, which the inbox can deliver
        // truncated. Reconstructing the series from real git objects (the
        // mirror) sidesteps the damaged blob entirely.
        {
            auto writeBytes = [&](const QString &rel, const QByteArray &bytes) {
                check(writeTestFile(tmp.path() + "/" + rel, bytes),
                      "write binary-pull fixture file");
            };
            const QString cbBase = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            git({"checkout", "-q", "-b", "feat-bin"});
            QByteArray binary; // a genuine binary file → a "GIT binary patch"
            for (int i = 0; i < 256; ++i)
                binary.append(static_cast<char>(i));
            writeBytes("asset.bin", binary);
            git({"add", "asset.bin"});
            git({"commit", "-q", "-m", "bin: add asset"});
            git({"checkout", "-q", cbBase});

            PullStore plain(tmp.path(), QString(), &identity, "tester");
            const int pbn = plain.createPull("Binary change", "body", cbBase,
                                             "feat-bin", QString(), QString(),
                                             /*branchBacked=*/true, &err);
            check(pbn > 0,
                  "createPull stores a branch-backed PR carrying a binary file");
            PullRequest seen;
            for (const PullRequest &p : plain.loadAll())
                if (p.number == pbn)
                    seen = p;
            check(seen.commits.contains("GIT binary patch"),
                  "the binary file reconstructs as a real binary patch from refs");
            // The flat diff must also carry the literal binary delta (derived with
            // `git diff --binary`), not just "Binary files … differ" — otherwise
            // the `git apply` merge fallback and the checkMergeable dry-run can't
            // replay binary changes.
            check(seen.patch.contains("GIT binary patch"),
                  "the reconstructed flat patch embeds the binary delta, not just "
                  "a \"Binary files differ\" marker");

            // Mirror the repo, then make this node look like one that holds the
            // PR pointer but not its branch, with a truncated commits.mbox where
            // the inbox would have dropped one.
            QTemporaryDir mirrorRoot;
            const QString mirror = mirrorRoot.path() + "/mirror.git";
            {
                QProcess clone;
                clone.start("git",
                            {"clone", "--bare", "-q", tmp.path(), mirror});
                clone.waitForFinished(8000);
            }
            git({"branch", "-D", "feat-bin"});
            // Also drop the materialized PR ref (issue #399): deleting the
            // named branch alone no longer strands the commits (the ref keeps
            // them reachable independently), so simulating "no reachable refs
            // anywhere" needs both gone.
            git({"update-ref", "-d",
                QStringLiteral("refs/pr/%1/head").arg(pbn)});
            // pulls/ metadata lives on its own linked worktree, not tmp.path()
            // (issue #399).
            const QString metaDir = plain.metaWorkTree();
            auto writeMetaBytes = [&](const QString &rel, const QByteArray &bytes) {
                check(writeTestFile(metaDir + "/" + rel, bytes),
                      "write pull metadata fixture file");
            };
            writeMetaBytes(
                ".forkmesh/pulls/" + QString::number(pbn) + "/commits.mbox",
                QByteArray(
                    "From 0000000000000000000000000000000000000000 Mon Sep 17 "
                    "00:00:00 2001\nFrom: x <x@x>\nDate: Thu, 1 Jan 1970 "
                    "00:00:00 +0000\nSubject: [PATCH] bin\n\n---\n asset.bin | "
                    "Bin\n\ndiff --git a/asset.bin b/asset.bin\nnew file mode "
                    "100644\nindex 0000000000000000000000000000000000000000.."
                    "1111111111111111111111111111111111111111\nGIT binary "
                    "patch\nliteral 256\nzcmZ!!CORRUPT!!\n\nliteral 0\n\n-- "
                    "\n2.0.0\n"));

            // With no reachable refs, the corrupt blob is the only source: fail.
            PullStore noMirror(tmp.path(), QString(), &identity, "tester");
            QString blobErr;
            check(!noMirror.mergePull(pbn, &blobErr, false),
                  "a branch-backed PR with no reachable refs and a corrupt blob "
                  "fails to apply");

            // With the mirror, the commits come from real objects and apply.
            PullStore viaMirror(tmp.path(), mirror, &identity, "tester");
            check(viaMirror.mergePull(pbn, &err, false),
                  "the PR merges once its commits are rebuilt from the mirror");
            QFile applied(tmp.path() + "/asset.bin");
            check(applied.open(QIODevice::ReadOnly) &&
                      applied.readAll() == binary,
                  "merging restores the binary file's exact bytes from the "
                  "mirror's commits");
        }

        // --- checkMergeable previews a branch-backed PR via a real ref-merge --
        // The mergeability check runs `git merge-tree` on the actual commits, so
        // it reports a clean merge or names the conflicting files with no patch
        // (and none of a `git apply --check`'s missing-blob / context-drift false
        // conflicts).
        {
            auto writeFile = [&](const QString &rel, const QString &text) {
                check(writeTestFile(tmp.path() + "/" + rel, text.toUtf8()),
                      "write merge-tree fixture file");
            };
            const QString mtBase = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            writeFile("mt.txt", "orig-1\norig-2\norig-3\n");
            git({"add", "mt.txt"});
            git({"commit", "-q", "-m", "mt: seed"});

            // A branch-backed PR that only adds a new file → merges cleanly.
            git({"checkout", "-q", "-b", "feat-mt-clean"});
            writeFile("mt-new.txt", "added\n");
            git({"add", "mt-new.txt"});
            git({"commit", "-q", "-m", "mt: add a separate file"});
            git({"checkout", "-q", mtBase});
            const int cleanN =
                pulls.createPull("mt clean", "body", mtBase, "feat-mt-clean",
                                 QString(), QString(), /*branchBacked=*/true, &err);
            check(cleanN > 0, "createPull stores the non-overlapping branch PR");
            bool clean = false;
            QStringList cf;
            check(pulls.checkMergeable(cleanN, &clean, &cf, &err) && clean,
                  "checkMergeable reports a non-overlapping branch-backed PR as "
                  "clean (via merge-tree, no patch)");

            // A branch-backed PR that rewrites line 2, then the base rewrites the
            // same line differently → a genuine content conflict merge-tree finds.
            git({"checkout", "-q", "-b", "feat-mt-conflict", mtBase});
            writeFile("mt.txt", "orig-1\nFEATURE-2\norig-3\n");
            git({"add", "mt.txt"});
            git({"commit", "-q", "-m", "mt: feature rewrites line 2"});
            git({"checkout", "-q", mtBase});
            writeFile("mt.txt", "orig-1\nBASE-2\norig-3\n");
            git({"add", "mt.txt"});
            git({"commit", "-q", "-m", "mt: base rewrites line 2"});
            const int confN =
                pulls.createPull("mt conflict", "body", mtBase, "feat-mt-conflict",
                                 QString(), QString(), /*branchBacked=*/true, &err);
            check(confN > 0, "createPull stores the overlapping branch PR");
            bool clean2 = true;
            QStringList cf2;
            check(pulls.checkMergeable(confN, &clean2, &cf2, &err) && !clean2,
                  "checkMergeable reports an overlapping branch-backed PR as not "
                  "clean");
            check(cf2.contains(QStringLiteral("mt.txt")),
                  "checkMergeable names the conflicting file from the ref-merge");
        }

        // --- Merging settles the pull's owner-authored reward estimate -------
        // Regression for adhoc #86: pull #32 merged while its estimate stayed
        // "pending_merge". That status is exactly what holds a reward out of
        // the payout queue and keeps the public ledger printing "PENDING
        // MERGE" for work that has already landed.
        {
            const QString rewardBase = QString::fromUtf8(
                gitOutput({"rev-parse", "--abbrev-ref", "HEAD"}).trimmed());
            git({"checkout", "-q", "-b", "feat-86"});
            check(writeTestFile(tmp.path() + "/reward86.txt", "v1\n"),
                  "write reward fixture file");
            git({"add", "reward86.txt"});
            git({"commit", "-q", "-m", "add reward86"});
            const QString rewardPatch = QString::fromUtf8(
                gitOutput({"diff", rewardBase + "..feat-86"}));
            git({"checkout", "-q", rewardBase});
            const int rn =
                pulls.createPull("Rewarded change", "body", rewardBase,
                                 "feat-86", rewardPatch, QString(),
                                 /*branchBacked=*/false, &err);
            check(rn > 0, "createPull stores the pull that carries an estimate");

            // The estimate is authored by the repository owner straight into
            // pull.md, outside the contributor's signature — the same place
            // the reward sync tool reads it from.
            const QString pullMd =
                pulls.metaWorkTree() +
                QStringLiteral("/.forkmesh/pulls/%1/pull.md").arg(rn);
            auto recordEstimate = [&](const QString &status) {
                QFile file(pullMd);
                if (!file.open(QIODevice::ReadOnly))
                    return false;
                QStringList kept;
                for (const QString &line :
                     QString::fromUtf8(file.readAll()).split(QLatin1Char('\n')))
                    if (!line.startsWith(QLatin1String("reward")))
                        kept.append(line);
                file.close();
                QString text = kept.join(QLatin1Char('\n'));
                const int close = text.indexOf(QStringLiteral("\n---\n"));
                if (close < 0)
                    return false;
                text.insert(close,
                            QStringLiteral(
                                "\nrewardEstimateLamports: 4000000"
                                "\nrewardRecipient: atomic-gateway-7974"
                                "\nrewardId: reward-forkmesh-forkmesh-pull-86"
                                "\nrewardCondition: merged"
                                "\nrewardStatus: ") +
                                status);
                return writeTestFile(pullMd, text.toUtf8());
            };
            auto rewardPull = [&]() {
                PullRequest found;
                for (const PullRequest &p : pulls.loadAll())
                    if (p.number == rn)
                        found = p;
                return found;
            };
            check(recordEstimate(QStringLiteral("pending_merge")),
                  "the owner records a merge-conditional estimate on the pull");

            const PullRequest waiting = rewardPull();
            check(waiting.rewardStatus == QLatin1String("pending_merge") &&
                      waiting.rewardEstimateLamports == 4000000,
                  "the estimate round-trips through pull.md");
            check(PullReward::amountText(waiting.rewardEstimateLamports) ==
                          QLatin1String("0.004 SOL") &&
                      PullReward::statusText(waiting) ==
                          QLatin1String("pays on merge"),
                  "an open pull states its reward and what it is waiting for");

            check(pulls.mergePull(rn, &err, false),
                  "the pull carrying an estimate merges");
            const PullRequest settled = rewardPull();
            check(settled.status == QLatin1String("merged") &&
                      settled.rewardStatus == QLatin1String("pending_wallet"),
                  "merging moves the estimate out of pending_merge (adhoc #86)");
            check(settled.rewardEstimateLamports == 4000000 &&
                      settled.rewardId ==
                          QLatin1String("reward-forkmesh-forkmesh-pull-86") &&
                      settled.rewardRecipient ==
                          QLatin1String("atomic-gateway-7974") &&
                      settled.rewardCondition == QLatin1String("merged"),
                  "the rest of the estimate survives the merge rewrite");
            check(PullReward::statusText(settled) ==
                      QLatin1String("awaiting the recipient's wallet"),
                  "a merged pull says its reward is now waiting on payout");

            // The records an older desktop already wrote: merged, with the
            // estimate never moved. The ledger sweep heals them in place.
            check(recordEstimate(QStringLiteral("pending_merge")),
                  "restore a stale pending_merge estimate on a merged pull");
            QString sweepError;
            QList<int> rewarded;
            pulls.reconcileMergedPulls(&sweepError, &rewarded);
            check(rewarded.contains(rn),
                  "the ledger sweep reports the estimate it healed");
            check(rewardPull().rewardStatus ==
                      QLatin1String("pending_wallet"),
                  "a merged pull's stale estimate is healed to pending_wallet");
            QList<int> secondPass;
            pulls.reconcileMergedPulls(&sweepError, &secondPass);
            check(secondPass.isEmpty(),
                  "a settled estimate is not rewritten on every sweep");

            // Paying the estimate from the desktop wallet has to land on the
            // pull as well as on the relay, or the repository keeps saying
            // "ready to pay" for a transfer that already settled.
            const QString rewardName =
                QStringLiteral("reward-forkmesh-forkmesh-pull-86");
            const QString paidSignature = QStringLiteral(
                "4vJ9JU1bJJE96FbKq6oCw1L1nR3n8Q6XmB1jHnGkQZ2m");
            QString receiptError;
            check(!pulls.recordRewardPayment(rn, QStringLiteral("reward-other"),
                                             paidSignature, &receiptError) &&
                      !receiptError.isEmpty(),
                  "a receipt for another reward is refused, with a reason");
            check(pulls.recordRewardPayment(rn, rewardName, paidSignature,
                                            &receiptError),
                  "the settled transfer is recorded on the pull it paid for");
            const PullRequest paid = rewardPull();
            check(paid.rewardStatus == QLatin1String("paid") &&
                      paid.rewardTxSignature == paidSignature &&
                      PullReward::statusText(paid) == QLatin1String("paid"),
                  "the pull now names the transaction that settled its reward");
            check(pulls.recordRewardPayment(rn, rewardName, paidSignature,
                                            &receiptError),
                  "a retried reconciliation of the same transfer is a no-op");
            check(!pulls.recordRewardPayment(
                      rn, rewardName,
                      QStringLiteral("2zK8replacementSignatureXXXXXXXXXXXXXXXX"),
                      &receiptError),
                  "a second transfer cannot overwrite the recorded receipt");
            check(rewardPull().rewardTxSignature == paidSignature,
                  "the first recorded transaction stands");
        }

        // --- CoveStore round-trip ----------------------------------------
        // Create an encrypted cove, confirm the committed file is opaque, then
        // unlock it from a fresh envelope read and verify the documents.
        CoveStore coves(tmp.path(), QString(), &identity, "tester");
        check(coves.canWrite(), "cove store reports the temp work tree as writable");
        CoveDocument cdoc;
        cdoc.id = "doc-1";
        cdoc.name = "Prod secrets";
        cdoc.body = "DB_PASSWORD=hunter2";
        Cove created;
        check(coves.createCove("Launch plans", "team-shared-password", true,
                               {cdoc}, &created, &err),
              "createCove encrypts, writes and commits a .cove file");
        check(created.creator == identity.publicKey(),
              "the cove records its creator's public key");

        const QByteArray onDisk =
            gitOutput({"show", QStringLiteral("HEAD:") + created.relPath});
        check(!onDisk.isEmpty() && onDisk.contains("\"kind\""),
              "the committed cove is a JSON envelope");
        check(!onDisk.contains("hunter2") && !onDisk.contains("DB_PASSWORD"),
              "the committed cove never leaks the plaintext document body");
        check(!onDisk.contains("Launch plans"),
              "the committed cove never leaks the cove's name");
        check(!onDisk.contains(identity.publicKey().toUtf8()),
              "the committed cove never leaks the creator's public key");
        check(!onDisk.contains("\"creator\"") && !onDisk.contains("\"access\"") &&
                  !onDisk.contains("\"notifyOnOpen\"") &&
                  !onDisk.contains("\"createdAtMs\""),
              "the committed envelope carries no identifying metadata keys");
        check(onDisk.contains("\"grants\""),
              "the committed envelope carries uniform grant slots");
        const QByteArray coveLog = gitOutput(
            {"log", "-1", "--format=%an <%ae>", "--", created.relPath});
        check(coveLog.trimmed() == "forkmesh <coves@forkmesh.invalid>",
              "cove commits use a neutral git author, not the creator's identity");
        check(!created.slug.contains("launch", Qt::CaseInsensitive) &&
                  !created.relPath.contains("launch", Qt::CaseInsensitive),
              "the cove's repo filename is an obscure slug, not its name");

        Cove reloaded;
        check(coves.loadEnvelope(created.relPath, reloaded, &err),
              "the cove envelope reloads (metadata only)");
        check(!reloaded.unlocked && reloaded.documents.isEmpty(),
              "a reloaded cove starts locked with no decrypted documents");
        check(reloaded.name.isEmpty(),
              "a locked cove envelope exposes no name (it is encrypted)");
        check(reloaded.creator.isEmpty() && reloaded.creatorAccount.isEmpty() &&
                  reloaded.invitedAccounts.isEmpty() && reloaded.accessMode.isEmpty() &&
                  !reloaded.notifyOnOpen && reloaded.createdAtMs == 0,
              "a locked cove envelope exposes no creator, accounts, mode or times");
        check(!CoveStore::unlock(reloaded, "WRONG-password"),
              "unlock rejects the wrong cove password");
        check(CoveStore::unlock(reloaded, "team-shared-password"),
              "unlock accepts the correct cove password");
        check(reloaded.unlocked && reloaded.documents.size() == 1 &&
                  reloaded.documents.first().body == "DB_PASSWORD=hunter2",
              "the unlocked cove yields the original document");
        check(reloaded.name == "Launch plans",
              "unlock recovers the cove's name from the encrypted payload");
        check(reloaded.creator == identity.publicKey() && reloaded.notifyOnOpen &&
                  reloaded.accessMode == "password" && reloaded.createdAtMs > 0,
              "unlock recovers the creator, mode and notify flag from the payload");

        // Appending an access entry and saving carries the trail in the payload.
        CoveAccessEntry visit;
        visit.who = identity.publicKey();
        visit.name = "tester";
        visit.ts = 1700000000000LL;
        visit.action = "open";
        CoveStore::appendAccess(reloaded, visit);
        check(coves.save(reloaded, "team-shared-password", &err),
              "saving a cove with an appended access entry succeeds");
        Cove afterLog;
        check(coves.loadEnvelope(created.relPath, afterLog, &err) &&
                  CoveStore::unlock(afterLog, "team-shared-password") &&
                  afterLog.accessLog.size() == 1 &&
                  afterLog.accessLog.first().who == identity.publicKey(),
              "the encrypted access log round-trips through save + unlock");

        const QList<Cove> listed = coves.listCoves();
        check(listed.size() == 1 && listed.first().name.isEmpty(),
              "listCoves enumerates envelopes without leaking the cove name");

        // --- Account-scoped coves: anonymous v2 envelopes -----------------
        // The ACL lives inside the ciphertext; access is granted by unwrapping a
        // per-account key grant, and the envelope reveals no accounts, no mode
        // and no member count (grant slots are padded with decoys).
        Cove teamCove;
        check(coves.createAccountCove("Team plans", "Alice", {"bob"}, false,
                                      {cdoc}, &teamCove, &err),
              "createAccountCove encrypts, writes and commits a .cove file");

        const QByteArray teamDisk =
            gitOutput({"show", QStringLiteral("HEAD:") + teamCove.relPath});
        check(!teamDisk.contains("alice") && !teamDisk.contains("Alice") &&
                  !teamDisk.contains("bob"),
              "the committed account cove never leaks creator or invitee names");
        check(!teamDisk.contains("account") && !teamDisk.contains("\"access\"") &&
                  !teamDisk.contains(identity.publicKey().toUtf8()),
              "the committed account cove reveals neither its mode nor its creator");
        check(!teamDisk.contains("hunter2") && !teamDisk.contains("Team plans"),
              "the committed account cove keeps its body and name encrypted");

        Cove teamLoaded;
        check(coves.loadEnvelope(teamCove.relPath, teamLoaded, &err) &&
                  teamLoaded.grants.size() == 4,
              "account grant slots are padded with decoys to hide the member count");
        Cove asMallory = teamLoaded;
        check(!CoveStore::unlockForAccount(asMallory, "mallory"),
              "an uninvited account cannot unlock an account cove");
        Cove asBob = teamLoaded;
        check(CoveStore::unlockForAccount(asBob, "BOB"),
              "an invited account unlocks the cove (case-insensitively)");
        check(asBob.name == "Team plans" && asBob.accessMode == "account" &&
                  asBob.creatorAccount == "alice" &&
                  asBob.invitedAccounts.contains("bob") &&
                  asBob.documents.size() == 1 &&
                  asBob.documents.first().body == "DB_PASSWORD=hunter2",
              "unlockForAccount recovers the encrypted ACL, name and documents");
        check(CoveStore::accountCanAccess(asBob, "alice") &&
                  !CoveStore::accountCanAccess(asBob, "mallory"),
              "the decrypted ACL answers accountCanAccess after unlock");

        // A collaborator with only a bare mirror cannot use saveAccountCove(),
        // but must still be able to seal an edit for the signed PR workflow.
        Cove proposedByBob = asBob;
        proposedByBob.documents.first().body = "updated from a mirror";
        QByteArray proposedEnvelope;
        check(CoveStore::encryptedAccountEnvelope(
                  proposedByBob, &proposedEnvelope, &err) &&
                  !proposedEnvelope.contains("updated from a mirror") &&
                  !proposedEnvelope.contains("bob"),
              "a mirror collaborator can seal an opaque account-cove edit");
        const QString proposedRel =
            CoveStore::covesDirRel() + "/proposed-account-edit.cove";
        QFile proposedFile(tmp.path() + "/" + proposedRel);
        const bool proposedWritten =
            proposedFile.open(QIODevice::WriteOnly) &&
            proposedFile.write(proposedEnvelope) == proposedEnvelope.size();
        proposedFile.close();
        Cove proposedReloaded;
        check(proposedWritten &&
                  coves.loadEnvelope(proposedRel, proposedReloaded, &err) &&
                  CoveStore::unlockForAccount(proposedReloaded, "bob") &&
                  proposedReloaded.documents.first().body ==
                      "updated from a mirror",
              "the proposed account-cove envelope decrypts to the collaborator's edit");
        QFile::remove(tmp.path() + "/" + proposedRel);

        // Inviting another account re-wraps the content key; the newcomer can
        // then unlock while the envelope still names nobody.
        asBob.invitedAccounts << "carol";
        check(coves.saveAccountCove(asBob, &err),
              "saving an account cove with a new invitee succeeds");
        Cove asCarol;
        check(coves.loadEnvelope(teamCove.relPath, asCarol, &err) &&
                  CoveStore::unlockForAccount(asCarol, "carol"),
              "a newly invited account can unlock the re-sealed cove");
        check(!gitOutput({"show", QStringLiteral("HEAD:") + teamCove.relPath})
                   .contains("carol"),
              "the re-sealed envelope still never names the invited accounts");

        // --- Legacy v1 account coves still unlock (and upgrade on save) ---
        // v1 stored the ACL in plaintext and derived the secret from it; write
        // one by hand and confirm the modern reader still opens it.
        {
            const QString coveId = "11111111-2222-3333-4444-555555555555";
            const QByteArray salt = CoveCrypto::randomSalt();
            const int rounds = 2048; // keep the test fast; v1 honors stored rounds
            const QByteArray material =
                QStringLiteral("forkmesh-account-cove-v1\n%1\n%2\n%3\n%4")
                    .arg(coveId, identity.publicKey(), "alice", "alice\nbob")
                    .toUtf8();
            const QString secret = QString::fromLatin1(
                QCryptographicHash::hash(material, QCryptographicHash::Sha256)
                    .toBase64(QByteArray::Base64UrlEncoding |
                              QByteArray::OmitTrailingEquals));
            CoveCrypto legacyCrypto(secret, salt, rounds);
            const QJsonObject legacyCipher = legacyCrypto.encrypt(
                QJsonDocument(QJsonObject{{"name", "Old team cove"},
                                          {"documents", QJsonArray{}},
                                          {"accessLog", QJsonArray{}}})
                    .toJson(QJsonDocument::Compact));
            const QJsonObject legacyEnvelope{
                {"kind", "cove"},
                {"v", 1},
                {"id", coveId},
                {"creator", identity.publicKey()},
                {"access", QJsonObject{{"mode", "account"},
                                       {"creatorAccount", "alice"},
                                       {"invitedAccounts", QJsonArray{"alice", "bob"}}}},
                {"kdf", QJsonObject{{"algo", "pbkdf2-sha256"},
                                    {"rounds", rounds},
                                    {"salt", QString::fromLatin1(salt.toBase64())}}},
                {"cipher", legacyCipher}};
            const QString legacyRel = CoveStore::covesDirRel() + "/legacyv1.cove";
            QDir().mkpath(tmp.path() + "/" + CoveStore::covesDirRel());
            QFile legacyFile(tmp.path() + "/" + legacyRel);
            check(legacyFile.open(QIODevice::WriteOnly) &&
                      legacyFile.write(QJsonDocument(legacyEnvelope).toJson()) > 0,
                  "a legacy v1 account cove envelope writes to the coves dir");
            legacyFile.close();

            Cove legacy;
            check(coves.loadEnvelope(legacyRel, legacy, &err) &&
                      legacy.creatorAccount == "alice" &&
                      CoveStore::accountCanAccess(legacy, "bob"),
                  "a legacy v1 envelope still exposes its plaintext ACL on load");
            check(!CoveStore::unlockForAccount(legacy, "mallory"),
                  "a legacy v1 cove still rejects uninvited accounts");
            check(CoveStore::unlockForAccount(legacy, "bob") &&
                      legacy.name == "Old team cove",
                  "a legacy v1 cove still unlocks via the v1 derived secret");
            check(coves.saveAccountCove(legacy, &err),
                  "saving a legacy cove upgrades it to a v2 envelope");
            const QByteArray upgraded =
                gitOutput({"show", QStringLiteral("HEAD:") + legacyRel});
            check(!upgraded.contains("alice") && !upgraded.contains("bob") &&
                      !upgraded.contains("account") &&
                      !upgraded.contains(identity.publicKey().toUtf8()),
                  "the upgraded envelope no longer names accounts, mode or creator");
            Cove upgradedCove;
            check(coves.loadEnvelope(legacyRel, upgradedCove, &err) &&
                      CoveStore::unlockForAccount(upgradedCove, "bob") &&
                      upgradedCove.name == "Old team cove",
                  "the upgraded cove still unlocks for its invited accounts");
        }
    }

    // AgentJail (adhoc #236): jailed launches get a memory cap prepended to the
    // shell command and a private scratch environment; a zero/negative cap
    // leaves the command untouched so unjailed runs are byte-identical.
    {
        const QString cmd = QStringLiteral("exec claude --model 'opus'");
        check(AgentJail::wrapCommand(cmd, 0) == cmd,
              "jail off leaves the launch command unchanged");
        check(AgentJail::wrapCommand(cmd, -5) == cmd,
              "a negative memory cap means no jail");
        check(AgentJail::wrapCommand(QString(), 1024).isEmpty(),
              "an empty command stays empty even with a cap");
        check(AgentJail::wrapCommand(cmd, 2048) ==
                  QStringLiteral(
                      "ulimit -d 2097152 2>/dev/null; exec claude --model 'opus'"),
              "the cap is applied in KB via ulimit ahead of the exec");
        check(AgentJail::wrapCommand(QStringLiteral("run %1 %2"), 1) ==
                  QStringLiteral("ulimit -d 1024 2>/dev/null; run %1 %2"),
              "percent placeholders in the command survive wrapping");

        QTemporaryDir jailTmp;
        check(jailTmp.isValid(), "agent jail temp dir is valid");
        const QString jailDir = jailTmp.path() + QStringLiteral("/jail");
        const QStringList env = AgentJail::envEntries(jailDir);
        check(env.contains(QStringLiteral("TMPDIR=") + jailDir +
                           QStringLiteral("/tmp")),
              "jail env redirects TMPDIR into the jail");
        check(env.contains(QStringLiteral("XDG_CACHE_HOME=") + jailDir +
                           QStringLiteral("/cache")),
              "jail env redirects the cache into the jail");
        check(QDir(jailDir + QStringLiteral("/tmp")).exists() &&
                  QDir(jailDir + QStringLiteral("/cache")).exists(),
              "jail scratch directories are created up front");
        check(AgentJail::sessionJailDir(7).endsWith(
                  QStringLiteral("/forkmesh-agent-jails/s7")),
              "stream sessions get a per-session jail dir under temp");
    }

    // Agent worktrees live in the project now (adhoc #1624): a run works in
    // <checkout>/.worktrees/agent-<id>-<desc> instead of a /tmp path, the
    // directory hides itself from the project's git, and a bare mirror — which
    // has no working tree to nest one in — still falls back to temp.
    {
        using namespace forkmesh::agentwt;

        check(dirName(1624, QStringLiteral(
                                "lets have agents start worktrees in the "
                                "project's folder")) ==
                  QStringLiteral("agent-1624-agents-start-worktrees-project"),
              "a worktree is named agent-<id>-<short description of the task>");
        check(dirName(9, QStringLiteral("  ")) == QStringLiteral("agent-9"),
              "a session with no usable title still gets a unique directory");
        check(dirName(9, QStringLiteral("Fix the ../etc/passwd loader!")) ==
                  QStringLiteral("agent-9-fix-etc-passwd-loader"),
              "punctuation in a title cannot escape the worktree directory");

        QTemporaryDir worktreeRepo;
        check(worktreeRepo.isValid(), "agent worktree test repository is valid");
        const QString repo = worktreeRepo.path();
        check(root(repo).endsWith(QStringLiteral("/forkmesh-worktrees")),
              "a directory that is not a checkout keeps the temp worktree root");
        bool ready =
            runTestGit(repo, {QStringLiteral("init"), QStringLiteral("-q"),
                              QStringLiteral("-b"), QStringLiteral("main")}) &&
            runTestGit(repo, {QStringLiteral("config"), QStringLiteral("user.name"),
                              QStringLiteral("Agent Runner")}) &&
            runTestGit(repo, {QStringLiteral("config"), QStringLiteral("user.email"),
                              QStringLiteral("agent@example.test")}) &&
            writeTestFile(repo + QStringLiteral("/README.md"), "hello") &&
            commitTestTree(repo, QStringLiteral("first"),
                           QStringLiteral("2026-08-07T10:00:00Z"),
                           QStringLiteral("Agent Runner"),
                           QStringLiteral("agent@example.test"));
        check(ready, "agent worktree test repository has a commit");
        check(root(repo) == repo + QStringLiteral("/.worktrees"),
              "a checkout hosts its agents' worktrees inside the project");

        const QString wtRoot = root(repo);
        ensureRoot(wtRoot);
        const QString wtPath = QDir(wtRoot).filePath(
            dirName(3, QStringLiteral("teach the relay to back off")));
        check(runTestGit(repo, {QStringLiteral("worktree"), QStringLiteral("add"),
                                QStringLiteral("-q"), QStringLiteral("-B"),
                                QStringLiteral("agent/adhoc-3-teach-relay-back-off"),
                                wtPath, QStringLiteral("HEAD")}) &&
                  QFileInfo::exists(wtPath + QStringLiteral("/README.md")),
              "git creates the agent worktree inside the project");
        QByteArray status;
        check(runTestGit(repo,
                         {QStringLiteral("status"), QStringLiteral("--porcelain")},
                         &status) &&
                  QString::fromUtf8(status).trimmed().isEmpty(),
              "a full worktree checked out in the project leaves it clean");
        check(writeTestFile(wtPath + QStringLiteral("/agent-note.md"), "wip"),
              "the agent can write inside its own worktree");
        QByteArray inner;
        check(runTestGit(wtPath,
                         {QStringLiteral("status"), QStringLiteral("--porcelain")},
                         &inner) &&
                  QString::fromUtf8(inner).contains(
                      QStringLiteral("agent-note.md")),
              "the root's ignore rule does not reach into the worktree itself");

        check(shellQuote(QStringLiteral("/home/dev/o'brien/repo")) ==
                  QStringLiteral("'/home/dev/o'\\''brien/repo'"),
              "a quote in the project path cannot break out of the git command");

        // adhoc #1680: merging with "delete the agent" started two removals for
        // one directory — the session's own teardown (a background thread) and
        // the merge's `git worktree remove` — and whichever lost the race
        // reported "Could not remove the worktree at <path>" for a worktree the
        // winner had already removed. Ordering them is the fix; these are the
        // two rules it rests on.
        check(samePath(wtPath, wtPath + QStringLiteral("/")) &&
                  samePath(wtPath, wtPath + QStringLiteral("/.")),
              "the same directory compares equal however the path was spelled");
        check(!samePath(QString(), QString()) && !samePath(wtPath, QString()) &&
                  !samePath(QStringLiteral("  "), wtPath),
              "an empty path names no worktree, so it claims nothing");

        check(runTestGit(repo, {QStringLiteral("worktree"), QStringLiteral("remove"),
                                QStringLiteral("--force"), wtPath}),
              "the first removal takes the worktree away");
        check(!runTestGit(repo, {QStringLiteral("worktree"), QStringLiteral("remove"),
                                 QStringLiteral("--force"), wtPath}),
              "a second removal is refused — git no longer knows the path");
        check(!refusalWorthReporting(/*gitSucceeded=*/false,
                                     QDir(wtPath).exists()),
              "that refusal is not shown: the worktree is gone, which is what "
              "the click asked for");
        check(refusalWorthReporting(/*gitSucceeded=*/false,
                                    /*directoryStillExists=*/true),
              "a refusal with the directory still standing is a real failure");

        // Removing a *registered* worktree whose directory was deleted still
        // succeeds, so reaching the refusal at all means another pass got there
        // first — and until its admin entry is pruned the branch stays hostage.
        const QString gonePath = QDir(wtRoot).filePath(QStringLiteral("agent-1680"));
        const QString goneBranch = QStringLiteral("agent/adhoc-1680-gone");
        check(runTestGit(repo, {QStringLiteral("worktree"), QStringLiteral("add"),
                                QStringLiteral("-q"), QStringLiteral("-B"),
                                goneBranch, gonePath, QStringLiteral("HEAD")}) &&
                  QDir(gonePath).removeRecursively(),
              "a worktree whose directory was wiped out from under git");
        check(!runTestGit(repo, {QStringLiteral("branch"), QStringLiteral("-D"),
                                 goneBranch}),
              "its branch is held hostage by the admin entry left behind");
        check(runTestGit(repo,
                         {QStringLiteral("worktree"), QStringLiteral("prune")}) &&
                  runTestGit(repo, {QStringLiteral("branch"), QStringLiteral("-D"),
                                    goneBranch}),
              "pruning before the branch step is what frees it");
    }

    // AgentStore persists a Claude Code session's stream-json transcript so it
    // survives an app restart (issue #41): events append one per line, reload in
    // order, and clearEvents starts a fresh run.
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "agent store temp dir is valid");
        AgentStore store(tmp.path());
        AgentSession session;
        session.owner = "octo";
        session.name = "demo";
        session = store.createSession(session);
        session.branchName = QStringLiteral("575-clean-cloudflare-prompts");
        session.worktreeName = session.branchName;
        session.skillsUsed = {QStringLiteral("Imagegen"),
                              QStringLiteral("Three.js Fundamentals")};
        session.teammateIds = {QStringLiteral("quality"),
                               QStringLiteral("steward")};
        session.teammateName = QStringLiteral("Team (2)");
        session.chatConversation = QStringLiteral("#team");
        session.chatThreadRootId = QStringLiteral("root-message-288");
        session.originInstanceId = QStringLiteral("relay-instance-work");
        session.originInstanceUrl = QStringLiteral(
            "wss://work.example.test/api/repo/mainnode/forkmesh/rooms/"
            "world-general/ws");
        session.taskGrade = 4;
        session.priority = 7;
        check(store.saveSession(session),
              "an AI-named branch/worktree session saves successfully");
        {
            QStringList metaFiles;
            QString metaPath;
            QDirIterator walk(tmp.path(), {QStringLiteral("meta.json*")},
                              QDir::Files | QDir::Hidden,
                              QDirIterator::Subdirectories);
            while (walk.hasNext()) {
                const QString found = walk.next();
                metaFiles << QFileInfo(found).fileName();
                if (found.endsWith(QLatin1String("/meta.json")))
                    metaPath = found;
            }
            QFile meta(metaPath);
            check(metaFiles == QStringList{QStringLiteral("meta.json")}
                      && meta.open(QIODevice::ReadOnly)
                      && AgentSession::fromJson(
                             QJsonDocument::fromJson(meta.readAll()).object())
                                 .priority
                             == 7,
                  "a session record replaces meta.json whole and leaves no "
                  "temporary file behind");
#ifdef Q_OS_UNIX
            check(!(QFileInfo(meta).permissions()
                    & (QFileDevice::ReadOther | QFileDevice::WriteOther)),
                  "a session record is not readable by other users");
#endif
        }

        check(store.loadEvents(session).isEmpty(),
              "a fresh agent session has no persisted transcript events");

        store.appendEvent(session, QJsonObject{{"type", "_local_user"},
                                               {"text", "do the thing"}});
        store.appendEvent(session,
                          QJsonObject{{"type", "assistant"}, {"seq", 2}});
        const QList<QJsonObject> events = store.loadEvents(session);
        check(events.size() == 2, "appended transcript events reload from disk");
        check(!events.isEmpty() &&
                  events.first().value("type").toString() == "_local_user" &&
                  events.first().value("text").toString() == "do the thing",
              "the first reloaded event is the initial user prompt, intact");
        check(events.size() == 2 && events.at(1).value("seq").toInt() == 2,
              "transcript events reload in append order");

        // Reopening the store mimics an app restart: the transcript is still there.
        AgentStore reopened(tmp.path());
        const QList<AgentSession> sessions = reopened.loadAllSessions();
        check(sessions.size() == 1 &&
                  reopened.loadEvents(sessions.first()).size() == 2 &&
                  sessions.first().branchName ==
                      QStringLiteral("575-clean-cloudflare-prompts") &&
                  sessions.first().worktreeName ==
                      QStringLiteral("575-clean-cloudflare-prompts") &&
                  sessions.first().teammateIds == session.teammateIds &&
                  sessions.first().chatConversation == QLatin1String("#team") &&
                  sessions.first().chatThreadRootId ==
                      QLatin1String("root-message-288") &&
                  sessions.first().originInstanceId ==
                      QLatin1String("relay-instance-work") &&
                  sessions.first().originInstanceUrl ==
                      session.originInstanceUrl &&
                  sessions.first().hasTeammateAttribution() &&
                  sessions.first().skillsUsed == session.skillsUsed &&
                  sessions.first().taskGrade == 4 &&
                  sessions.first().priority == 7,
              "transcript, branch/worktree name, team, skills, task grade, and priority survive an app restart");

        QJsonObject legacySession = session.toJson();
        legacySession.remove(QStringLiteral("worktreeName"));
        legacySession.remove(QStringLiteral("skillsUsed"));
        legacySession.remove(QStringLiteral("teammateIds"));
        legacySession.remove(QStringLiteral("chatConversation"));
        legacySession.remove(QStringLiteral("chatThreadRootId"));
        legacySession.remove(QStringLiteral("originInstanceId"));
        legacySession.remove(QStringLiteral("originInstanceUrl"));
        legacySession.remove(QStringLiteral("taskGrade"));
        legacySession.remove(QStringLiteral("priority"));
        check(AgentSession::fromJson(legacySession).worktreeName.isEmpty(),
              "legacy agent sessions keep deriving their worktree name");
        check(AgentSession::fromJson(legacySession).skillsUsed.isEmpty(),
              "legacy agent sessions load without an invented skill history");
        check(AgentSession::fromJson(legacySession).teammateIds.isEmpty(),
              "legacy agent sessions load without invented team members");
        check(AgentSession::fromJson(legacySession).chatConversation.isEmpty() &&
                  AgentSession::fromJson(legacySession).chatThreadRootId.isEmpty() &&
                  AgentSession::fromJson(legacySession).originInstanceId.isEmpty() &&
                  AgentSession::fromJson(legacySession).originInstanceUrl.isEmpty(),
              "legacy agent sessions load without an invented chat thread or relay");
        check(AgentSession::fromJson(legacySession).taskGrade == 0,
              "legacy agent sessions load as ungraded");
        check(AgentSession::fromJson(legacySession).priority == 50,
              "legacy agent sessions receive the neutral priority");
        legacySession.insert(QStringLiteral("priority"), -10);
        check(AgentSession::fromJson(legacySession).priority == 1,
              "agent priority is clamped at the high-priority endpoint");
        legacySession.insert(QStringLiteral("priority"), 1000);
        check(AgentSession::fromJson(legacySession).priority == 99,
              "agent priority is clamped at the low-priority endpoint");

        store.clearEvents(session);
        check(store.loadEvents(session).isEmpty(),
              "clearEvents starts the next run with a clean transcript");
    }

    // Live transcript search: the search bar filters the session list by what a
    // run actually said, so the store has to find a query across both the raw log
    // and the stream-json events, count the hits, and hand back the surrounding
    // text for the matching row's tooltip.
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "transcript search temp dir is valid");
        AgentStore store(tmp.path());
        AgentSession session;
        session.owner = "octo";
        session.name = "demo";
        session = store.createSession(session);

        check(store.searchTranscript(session, "recovery") == 0,
              "an empty transcript matches nothing");

        store.appendLog(session, "only seeing RECOVERY pings here");
        store.appendEvent(session,
                          QJsonObject{{"type", "assistant"},
                                      {"text", "failure pings need recovery"}});

        QString snippet;
        check(store.searchTranscript(session, "recovery", &snippet) == 2,
              "hits are counted across the raw log and the event stream");
        check(snippet.contains("seeing RECOVERY pings"),
              "the snippet carries the text around the first hit");
        check(store.searchTranscript(session, "Recovery") == 2,
              "transcript search is case-insensitive");
        check(store.searchTranscript(session, "nowhere in here") == 0,
              "a query the run never said matches nothing");
        check(store.searchTranscript(session, "   ") == 0,
              "a blank query never claims a match");

        // Only the tail of a long-running session is read, so scanning every
        // session on each keystroke stays cheap.
        AgentSession chatty;
        chatty.owner = "octo";
        chatty.name = "demo";
        chatty = store.createSession(chatty);
        store.appendLog(chatty, "needle at the very start");
        store.appendLog(chatty,
                        QString(AgentStore::kTranscriptSearchTailBytes + 4096,
                                QLatin1Char('x')));
        check(store.searchTranscript(chatty, "needle") == 0,
              "text older than the search tail is out of scope");
        store.appendLog(chatty, "needle again at the end");
        check(store.searchTranscript(chatty, "needle") == 1,
              "the tail of a long transcript is still searched");
    }

    // Attachment scan behind the sessions list's little square (adhoc #222): the
    // pictures a run was started from — or steered with — are named as "Attached
    // image: <path>" lines in the prompt and in the transcripts, and the list has
    // to find them in both without reading a whole multi-megabyte transcript.
    {
        check(AgentStore::attachmentPathsIn("nothing attached here").isEmpty(),
              "a prompt with no attachment line yields no images");
        const QStringList plain = AgentStore::attachmentPathsIn(
            "look at this\nAttached image: /tmp/shot.png\nand fix it");
        check(plain == QStringList{"/tmp/shot.png"},
              "an attachment path is lifted out of plain prompt text");
        // Transcript events store the same line JSON-encoded, so the path ends at
        // the escape or the closing quote rather than at a real newline.
        const QStringList encoded = AgentStore::attachmentPathsIn(
            "{\"type\":\"_local_user\",\"text\":\"fix this\\nAttached image: "
            "/tmp/a.png\\nthanks\"}");
        check(encoded == QStringList{"/tmp/a.png"},
              "an attachment path is lifted out of a JSON-encoded event line");
        const QStringList many = AgentStore::attachmentPathsIn(
            "Attached image: /tmp/a.png\nAttached image: /tmp/b.png\n"
            "Attached image: /tmp/a.png\n");
        check(many == (QStringList{"/tmp/a.png", "/tmp/b.png"}),
              "attachments keep their order and are de-duplicated");

        QTemporaryDir tmp;
        check(tmp.isValid(), "attachment scan temp dir is valid");
        AgentStore store(tmp.path());
        AgentSession session;
        session.owner = "octo";
        session.name = "demo";
        session.prompt = "make it blue\nAttached image: /tmp/launch.png";
        session = store.createSession(session);

        const QString firstStamp = store.transcriptStamp(session);
        check(store.attachmentPaths(session) == QStringList{"/tmp/launch.png"},
              "an ad-hoc session's launch attachment is found in its prompt");

        // A follow-up only ever reaches disk as a transcript turn.
        store.appendEvent(session,
                          QJsonObject{{"type", "_local_user"},
                                      {"text", "and this one\nAttached image: "
                                               "/tmp/followup.png"}});
        check(store.attachmentPaths(session) ==
                  (QStringList{"/tmp/launch.png", "/tmp/followup.png"}),
              "an attachment steered into a running session is found too");
        check(store.transcriptStamp(session) != firstStamp,
              "the transcript stamp moves when a session is appended to, so the "
              "scan knows to re-read it");

        // The middle of a long run is skipped, but both of its ends are read: an
        // attachment can only be named in a prompt, and those sit at the opening
        // turn or at the newest follow-up.
        AgentSession chatty;
        chatty.owner = "octo";
        chatty.name = "demo";
        chatty = store.createSession(chatty);
        store.appendLog(chatty, "Attached image: /tmp/opening.png");
        store.appendLog(chatty, QString(3 * AgentStore::kAttachmentScanBytes,
                                        QLatin1Char('x')));
        store.appendLog(chatty, "Attached image: /tmp/latest.png");
        check(store.attachmentPaths(chatty) ==
                  (QStringList{"/tmp/opening.png", "/tmp/latest.png"}),
              "both ends of a long transcript are scanned for attachments");
    }

    // The Claude Code run summary the CLI reports on finish ("done · N turns ·
    // Ms · $X") is stored on the session and survives a restart (issue #296).
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "run-summary store temp dir is valid");
        AgentStore store(tmp.path());
        AgentSession session;
        session.owner = "octo";
        session.name = "demo";
        session = store.createSession(session);
        session.numTurns = 71;
        session.durationMs = 828000;
        session.costUsd = 4.59;
        session.handoffFromProvider = QStringLiteral("claude-code");
        check(store.saveSession(session), "saving a session with a run summary succeeds");

        AgentStore reopened(tmp.path());
        const QList<AgentSession> sessions = reopened.loadAllSessions();
        check(sessions.size() == 1 && sessions.first().numTurns == 71 &&
                  sessions.first().durationMs == 828000 &&
                  sessions.first().handoffFromProvider ==
                      QStringLiteral("claude-code") &&
                  qAbs(sessions.first().costUsd - 4.59) < 1e-9,
              "run summary and agent-handoff metadata reload after restart");
    }

    // Website-created organization jobs keep their exact Worker lease alongside
    // the local session until the terminal result is acknowledged.  The binding
    // and FIFO restart order must both survive the app process going away.
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "organization-agent store temp dir is valid");
        AgentStore store(tmp.path());
        AgentSession session;
        session.owner = "octo";
        session.name = "demo";
        session = store.createSession(session);
        session.orgAgentJob = QJsonObject{
            {"kind", "start"},
            {"jobId", 314},
            {"leaseId", "lease-after-restart"},
            {"sessionId", "remote-session"},
        };
        check(store.saveSession(session),
              "saving an organization-agent lease binding succeeds");

        AgentStore reopened(tmp.path());
        const QList<AgentSession> sessions = reopened.loadAllSessions();
        check(sessions.size() == 1 &&
                  sessions.first().orgAgentJob.value("jobId").toInt() == 314 &&
                  sessions.first().orgAgentJob.value("leaseId").toString() ==
                      "lease-after-restart",
              "organization-agent job id and lease survive an app restart");

        QJsonObject legacy = session.toJson();
        legacy.remove("orgAgentJob");
        check(AgentSession::fromJson(legacy).orgAgentJob.isEmpty(),
              "legacy agent sessions default to no organization-job binding");

        AgentSession newest;
        newest.id = 30;
        newest.createdAtMs = 300;
        newest.priority = 20;
        newest.status = AgentStatus::Queued;
        AgentSession oldest;
        oldest.id = 10;
        oldest.createdAtMs = 100;
        oldest.priority = 20;
        oldest.status = AgentStatus::Queued;
        AgentSession middle;
        middle.id = 20;
        middle.createdAtMs = 200;
        middle.priority = 2;
        middle.status = AgentStatus::Queued;
        AgentSession completed;
        completed.id = 5;
        completed.createdAtMs = 50;
        completed.status = AgentStatus::Success;
        check(AgentStore::queuedSessionIdsOldestFirst(
                  {newest, completed, oldest, middle}) ==
                  QList<int>({20, 10, 30}),
              "restart rebuilds the queued agent backlog by priority, then oldest-first");
    }

    // The composer ranks models by merged work, so that score must outlive the
    // sessions it was earned by: deleting a landed branch and its agent used to
    // take the model's whole record with it ("0 merged" for every model).
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "model-outcome store temp dir is valid");
        AgentStore store(tmp.path());
        check(store.retiredModelOutcomes().isEmpty(),
              "a fresh store has retired no model outcomes");

        const QString key =
            AgentStore::modelOutcomeKey("claude-code", "claude-opus-5");
        check(key == AgentStore::modelOutcomeKey("claude-code", "  Claude-Opus-5 "),
              "outcome keys ignore model case and surrounding space");
        check(AgentStore::modelOutcomeKey("claude-code", "  ").isEmpty(),
              "a session with no model has no outcome key");

        auto landed = [&store](const QString &model, bool merged) {
            AgentSession session;
            session.owner = "octo";
            session.name = "demo";
            session.provider = "claude-code";
            session.model = model;
            session = store.createSession(session);
            session.merged = merged;
            store.saveSession(session);
            return session;
        };
        const AgentSession first = landed("claude-opus-5", true);
        const AgentSession second = landed("claude-opus-5", false);
        const AgentSession other = landed("claude-sonnet-5", true);
        const AgentSession modelless = landed(QString(), true);
        check(store.retiredModelOutcomes().isEmpty(),
              "living sessions are not counted as retired outcomes");

        check(store.deleteSession(first) && store.deleteSession(second) &&
                  store.deleteSession(other) && store.deleteSession(modelless),
              "deleting the sessions succeeds");
        AgentStore reopened(tmp.path());
        const QHash<QString, AgentModelOutcome> outcomes =
            reopened.retiredModelOutcomes();
        check(outcomes.value(key).merged == 1 && outcomes.value(key).runs == 2,
              "a deleted session's merge is still credited to its model after a "
              "restart");
        check(outcomes.value(AgentStore::modelOutcomeKey("claude-code",
                                                         "claude-sonnet-5"))
                      .merged == 1,
              "each model keeps its own retired tally");
        check(outcomes.size() == 2,
              "a session with no model is credited to nothing");

        // nextId() hands a deleted id straight back out, so the tally must never
        // be keyed by session id: re-deleting the same record must not double it.
        check(store.deleteSession(first),
              "deleting an already-deleted session still succeeds");
        check(reopened.retiredModelOutcomes().value(key).runs == 2,
              "a repeated delete does not double-count the model");
    }

    // The quick-add "YOLO" toggle is stamped onto the session at launch (adhoc
    // #12), so the auto-merge decision survives a restart and never depends on
    // where the checkbox happens to sit when the run finishes.
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "yolo store temp dir is valid");
        AgentStore store(tmp.path());
        AgentSession session;
        session.owner = "octo";
        session.name = "demo";
        session = store.createSession(session);
        check(!session.yolo, "a session defaults to no auto-merge");
        session.yolo = true;
        session.associationOnly = true;
        session.branchName = "agent/adhoc-1-yolo";
        check(store.saveSession(session), "saving a YOLO session succeeds");

        AgentStore reopened(tmp.path());
        const QList<AgentSession> sessions = reopened.loadAllSessions();
        check(sessions.size() == 1 && sessions.first().yolo &&
                  sessions.first().associationOnly &&
                  !sessions.first().merged,
              "session behavior flags reload intact after a restart");

        // Sessions written before the flag existed must read back as opt-out —
        // an absent "yolo" key can never turn into an unattended merge.
        QJsonObject legacy = session.toJson();
        legacy.remove("yolo");
        check(!AgentSession::fromJson(legacy).yolo,
              "a session JSON without the yolo key never auto-merges");
        check(!session.vibe && !legacy.contains("vibe") &&
                  !AgentSession::fromJson(legacy).vibe,
              "a session JSON without the vibe key never merges, cleans up or pushes");
        AgentSession vibeSession = session;
        vibeSession.vibe = true;
        check(AgentSession::fromJson(vibeSession.toJson()).vibe,
              "a vibe session keeps its flag through JSON");
        legacy.remove("associationOnly");
        check(!AgentSession::fromJson(legacy).associationOnly,
              "legacy session JSON never becomes a provenance-only record");

        // Genie (adhoc #38) is stamped the same way: the launch attaches the MCP
        // connector because the run was started as a genie, so the flag has to
        // survive a restart (a resumed genie must get its tools back) and an
        // older session file must not read as one.
        check(!AgentSession::fromJson(legacy).genie,
              "a session JSON without the genie key is not a genie run");
        session.genie = true;
        check(store.saveSession(session), "saving a genie session succeeds");
        AgentStore genieReopened(tmp.path());
        const QList<AgentSession> genieSessions = genieReopened.loadAllSessions();
        check(genieSessions.size() == 1 && genieSessions.first().genie,
              "the genie flag reloads intact after a restart");
        // The sparkle glyph marks a genie only while it is still in flight; a
        // finished or merged one reads exactly like every other run.
        AgentSession live = genieSessions.first();
        live.status = AgentStatus::Running;
        check(live.genieInFlight(), "a running genie is drawn with the genie glyph");
        live.status = AgentStatus::Success;
        check(!live.genieInFlight(),
              "a finished genie falls back to the ordinary status glyph");
        live.status = AgentStatus::Running;
        live.merged = true;
        check(!live.genieInFlight(),
              "a merged genie shows the merge glyph, not the genie one");
        AgentSession ordinary = live;
        ordinary.genie = false;
        ordinary.merged = false;
        check(!ordinary.genieInFlight(),
              "an ordinary run never shows the genie glyph");
    }

    {
        // Workflow variable substitution must expand the explicit
        // ${{ vars.NAME }} context form and any declared ${NAME} variables, but
        // must NOT touch a workflow's own shell variables — otherwise a release
        // step building "releases/${channel}/forkmesh-${os}-${arch}" from shell
        // variables collapses to "releases//forkmesh--" (the reported bug).
        QMap<QString, QString> vars;
        vars.insert(QStringLiteral("TOKEN"), QStringLiteral("s3cr3t"));

        const QString shellScript =
            QStringLiteral("channel=\"${RELEASE_CHANNEL:-latest}\"\n"
                           "os=linux; arch=x86_64\n"
                           "dest=\"releases/${channel}/forkmesh-${os}-${arch}\"");
        const QString out = ActionFile::substitute(shellScript, vars);
        check(out == shellScript,
              "substitute leaves unknown ${NAME} shell variables untouched");

        check(ActionFile::substitute(
                  QStringLiteral("auth ${{ vars.TOKEN }} and ${TOKEN}"), vars) ==
                  QStringLiteral("auth s3cr3t and s3cr3t"),
              "substitute expands declared vars via both ${{ vars.X }} and ${X}");

        check(ActionFile::substitute(
                  QStringLiteral("x=${{ vars.MISSING }}-end"), vars) ==
                  QStringLiteral("x=-end"),
              "substitute blanks unknown ${{ vars.X }} context references");
    }

    {
        // Workflow `on:` triggers parse into the predicates the runner gates on:
        // a release workflow fires on a published release, not on every push.
        const ActionWorkflow rel = ActionFile::parse(
            QStringLiteral(".forkmesh/release.yml"),
            QStringLiteral("name: Release\non: [release, workflow_dispatch]\n"
                           "jobs:\n  release:\n    steps:\n"
                           "      - run: echo hi\n"));
        check(rel.valid, "release workflow parses");
        check(rel.triggersOnRelease(), "on: release sets triggersOnRelease()");
        check(!rel.triggersOnPush(), "on: release does not trigger on push");
        check(rel.allowsManualRun(), "workflow_dispatch stays a manual trigger");

        const ActionWorkflow push = ActionFile::parse(
            QStringLiteral(".forkmesh/ci.yml"),
            QStringLiteral("name: CI\non: [push]\n"
                           "jobs:\n  test:\n    steps:\n"
                           "      - run: echo hi\n"));
        check(push.triggersOnPush(), "on: push sets triggersOnPush()");
        check(!push.triggersOnRelease(), "on: push does not trigger on release");
        // Pull requests are their own event: a repository's deploy workflow says
        // `on: push` and must not run a contributor's proposed head, so the
        // automatic per-PR pass matches only workflows that opt in.
        check(!push.triggersOnPullRequest(),
              "on: push alone does not trigger on a pull request");

        const ActionWorkflow checks = ActionFile::parse(
            QStringLiteral(".forkmesh/ci.yml"),
            QStringLiteral("name: CI\non: [push, pull_request]\n"
                           "jobs:\n  test:\n    steps:\n"
                           "      - run: echo hi\n"));
        check(checks.triggersOnPullRequest() && checks.triggersOnPush(),
              "on: [push, pull_request] triggers on both events");

        // GitHub's mapping form carries per-event filters, so the events are the
        // keys. Reading only scalars there left `on` empty and the workflow
        // triggered on nothing at all.
        const ActionWorkflow filtered = ActionFile::parse(
            QStringLiteral(".forkmesh/ci.yml"),
            QStringLiteral("name: CI\non:\n  pull_request:\n"
                           "    branches: [main]\n  push:\n"
                           "jobs:\n  test:\n    steps:\n"
                           "      - run: echo hi\n"));
        check(filtered.valid, "a mapping-form on: still parses");
        check(filtered.triggersOnPullRequest(),
              "on: with a pull_request key triggers on pull requests");
        check(filtered.triggersOnPush(),
              "on: with a push key still triggers on push");

        const ActionWorkflow pathFiltered = ActionFile::parse(
            QStringLiteral(".forkmesh/android.yml"),
            QStringLiteral("name: Android\n"
                           "on:\n  push:\n    paths:\n      - mobile/**\n"
                           "jobs:\n  build:\n    steps:\n      - run: echo hi\n"));
        check(pathFiltered.valid, "a push.paths-form on: parses");
        check(pathFiltered.triggersOnPush(),
              "on: push.paths-form still triggers on push");
        check(pathFiltered.matchesPushChangedPaths(
                  {QStringLiteral("mobile/main.dart")}),
              "matching push paths include mobile/ changes");
        check(!pathFiltered.matchesPushChangedPaths(
                  {QStringLiteral("app/main.dart")}),
              "non-mobile changes do not match mobile/ push path filter");
        QStringList manyPaths;
        for (int i = 0; i < 20000; ++i)
            manyPaths.append(QStringLiteral("app/src/file%1.py").arg(i));
        check(!pathFiltered.matchesPushChangedPaths(manyPaths),
              "a large push outside mobile/ does not match");
        manyPaths.append(QStringLiteral("mobile/late.dart"));
        check(pathFiltered.matchesPushChangedPaths(manyPaths),
              "the last changed path of a large push still matches");
        const ActionWorkflow blankPattern = ActionFile::parse(
            QStringLiteral(".forkmesh/blank.yml"),
            QStringLiteral("name: Blank\n"
                           "on:\n  push:\n    paths:\n      - \"  \"\n"
                           "      - docs/*.md\n"
                           "jobs:\n  build:\n    steps:\n      - run: echo hi\n"));
        check(blankPattern.matchesPushChangedPaths({QStringLiteral("docs/a.md")}) &&
                  !blankPattern.matchesPushChangedPaths(
                      {QStringLiteral("docs/sub/a.md")}),
              "a blank path pattern is skipped and * stays inside one folder");
    }

    {
        using forkmesh::actions::isKnownPushBase;
        using forkmesh::actions::isMetadataOnlyChange;
        using forkmesh::actions::pushRangeChangedPaths;
        const ActionWorkflow deployNodes = ActionFile::parse(
            QStringLiteral(".forkmesh/deploy-nodes.yml"),
            QStringLiteral("name: Deploy nodes\n"
                           "on:\n  push:\n    branches: [main]\n"
                           "    paths:\n"
                           "      - \"world/*\"\n"
                           "      - \"world/*/*\"\n"
                           "      - \"world/*/*/*\"\n"
                           "      - \"world/*/*/*/*\"\n"
                           "jobs:\n  deploy:\n    steps:\n"
                           "      - run: echo deploy\n"));
        check(deployNodes.valid && deployNodes.triggersOnPush(),
              "push range: the world/* deploy workflow parses");
        const QString zeros(40, QLatin1Char('0'));
        check(!isKnownPushBase(QString()) && !isKnownPushBase(zeros) &&
                  !isKnownPushBase(QStringLiteral("--output=x")) &&
                  isKnownPushBase(QStringLiteral(
                      "0123456789ABCDEF0123456789abcdef01234567")),
              "push range: only a real object id is a known old value");

        QTemporaryDir rangeRoot;
        const QString repo = rangeRoot.path() + QStringLiteral("/range");
        const QString who = QStringLiteral("Range Tester");
        const QString mail = QStringLiteral("range@example.test");
        QProcessEnvironment gitEnv = QProcessEnvironment::systemEnvironment();
        gitEnv.insert(QStringLiteral("GIT_AUTHOR_NAME"), who);
        gitEnv.insert(QStringLiteral("GIT_AUTHOR_EMAIL"), mail);
        gitEnv.insert(QStringLiteral("GIT_COMMITTER_NAME"), who);
        gitEnv.insert(QStringLiteral("GIT_COMMITTER_EMAIL"), mail);
        int stamp = 0;
        const auto commitFile = [&](const QString &path) {
            ++stamp;
            const QString when =
                QStringLiteral("2026-09-01T10:%1:00Z")
                    .arg(stamp, 2, 10, QLatin1Char('0'));
            if (!writeTestFile(repo + QLatin1Char('/') + path,
                               path.toUtf8() + QByteArrayLiteral("\n")) ||
                !commitTestTree(repo, path, when, who, mail))
                return QString();
            return testGitHead(repo);
        };
        const auto runGit = [&](const QStringList &args) {
            return runTestGit(repo, args, nullptr, &gitEnv);
        };
        const auto has = [](const QStringList &paths, const char *path) {
            return paths.contains(QString::fromLatin1(path));
        };

        bool ok = rangeRoot.isValid() && QDir().mkpath(repo) &&
                  runGit({QStringLiteral("init"), QStringLiteral("-q"),
                          QStringLiteral("-b"), QStringLiteral("main")});
        const QString rootCommit = ok ? commitFile(QStringLiteral("README.md"))
                                      : QString();
        const QString mainDesktop =
            commitFile(QStringLiteral("desktop/src/Main.cpp"));
        ok = ok && !rootCommit.isEmpty() && !mainDesktop.isEmpty() &&
             runGit({QStringLiteral("checkout"), QStringLiteral("-q"),
                     QStringLiteral("-b"), QStringLiteral("integrate/world3"),
                     rootCommit});
        const QString worldTip =
            ok ? commitFile(QStringLiteral("world/public/world/scene.js"))
               : QString();
        ok = ok && !worldTip.isEmpty() &&
             runGit({QStringLiteral("merge"), QStringLiteral("-q"),
                     QStringLiteral("--no-ff"), QStringLiteral("--no-edit"),
                     QStringLiteral("-m"),
                     QStringLiteral("Merge main into integrate/world3"),
                     QStringLiteral("main")});
        const QString mergeTip = ok ? testGitHead(repo) : QString();
        check(ok && !mergeTip.isEmpty(),
              "push range: the merge-into-integration history is built");

        QByteArray firstParentOnly;
        runTestGit(repo,
                   {QStringLiteral("diff-tree"), QStringLiteral("--no-commit-id"),
                    QStringLiteral("--name-only"), QStringLiteral("-r"),
                    mergeTip + QStringLiteral("^1"), mergeTip},
                   &firstParentOnly);
        check(!QString::fromUtf8(firstParentOnly).contains(
                  QStringLiteral("world/")),
              "push range: the tip's first-parent diff misses world/ (the "
              "old bug's input)");
        const QStringList mergeRange =
            pushRangeChangedPaths(repo, mainDesktop, mergeTip);
        check(has(mergeRange, "world/public/world/scene.js") &&
                  !has(mergeRange, "desktop/src/Main.cpp"),
              "push range: a merge whose second parent brings world/ changes "
              "reports them over the pushed range");
        check(deployNodes.matchesPushChangedPaths(mergeRange),
              "push range: that merge push matches the world/* filter");

        ok = runGit({QStringLiteral("checkout"), QStringLiteral("-q"),
                     QStringLiteral("main")});
        const QString linearBase = mainDesktop;
        const QString linearOne =
            ok ? commitFile(QStringLiteral("world/public/world/avatar.js"))
               : QString();
        const QString linearTwo =
            commitFile(QStringLiteral("desktop/src/Two.cpp"));
        const QString linearThree =
            commitFile(QStringLiteral("desktop/src/Three.cpp"));
        check(!linearOne.isEmpty() && !linearTwo.isEmpty() &&
                  !linearThree.isEmpty(),
              "push range: three linear commits are built");
        const QStringList linearRange =
            pushRangeChangedPaths(repo, linearBase, linearThree);
        check(has(linearRange, "world/public/world/avatar.js") &&
                  has(linearRange, "desktop/src/Two.cpp") &&
                  has(linearRange, "desktop/src/Three.cpp"),
              "push range: three linear commits report every commit's files");
        check(deployNodes.matchesPushChangedPaths(linearRange),
              "push range: only the first of three commits touching world/ "
              "still matches the filter");
        check(!deployNodes.matchesPushChangedPaths(
                  pushRangeChangedPaths(repo, linearTwo, linearThree)),
              "push range: a push of only the last commit does not match");

        const QStringList newBranch =
            pushRangeChangedPaths(repo, zeros, mergeTip);
        check(has(newBranch, "world/public/world/scene.js") &&
                  has(newBranch, "desktop/src/Main.cpp"),
              "push range: a new-branch push falls back to the union of the "
              "tip's diffs against every parent");
        check(pushRangeChangedPaths(repo, QString(), mergeTip) == newBranch,
              "push range: an unknown old value takes the same fallback");
        check(pushRangeChangedPaths(repo, linearThree, mergeTip) == newBranch,
              "push range: a force push (old value not an ancestor) falls "
              "back to the union of the tip's parents");
        check(pushRangeChangedPaths(repo, zeros, rootCommit) ==
                  QStringList{QStringLiteral("README.md")},
              "push range: a root commit lists its whole tree");

        const QString issueCommit =
            commitFile(QStringLiteral(".forkmesh/issues/729.json"));
        const QString pullCommit =
            commitFile(QStringLiteral(".forkmesh/pulls/12.json"));
        check(!issueCommit.isEmpty() && !pullCommit.isEmpty(),
              "push range: metadata commits are built");
        const QStringList metadataRange =
            pushRangeChangedPaths(repo, linearThree, pullCommit);
        check(metadataRange.size() == 2 &&
                  isMetadataOnlyChange(metadataRange),
              "push range: a range of issue and pull commits is metadata-only");
        check(!isMetadataOnlyChange(
                  pushRangeChangedPaths(repo, linearTwo, pullCommit)),
              "push range: a range that also carries code is not metadata-only");
        check(!isMetadataOnlyChange({}),
              "push range: an empty change list is not metadata-only");
    }

    {
        // Who may run code on this node. A contributor with no merged work here
        // is a first-time author: every one of their pull requests waits for the
        // maintainer, and approving one must not arm the next.
        const QString stranger = QStringLiteral("strangerKeyBase64Url");
        const QString regular = QStringLiteral("regularKeyBase64Url");
        const QStringList trusted{QStringLiteral("myOwnNodeKey"),
                                  QStringLiteral("owner")};

        PullRequest merged;
        merged.number = 1;
        merged.status = QStringLiteral("merged");
        merged.author = regular;
        merged.authorName = QStringLiteral("regular");

        PullRequest open;
        open.number = 2;
        open.status = QStringLiteral("open");
        open.author = regular;
        open.authorName = QStringLiteral("regular");

        PullRequest first;
        first.number = 3;
        first.status = QStringLiteral("open");
        first.author = stranger;
        first.authorName = QStringLiteral("atomic-gateway-7974");

        const QList<PullRequest> history{merged, open, first};
        check(PullAuthorTrust::isEstablished(open, history, trusted),
              "an author with a merged pull request here is established");
        check(!PullAuthorTrust::isEstablished(first, history, trusted),
              "an author with no merged work here is a first-time author");

        // A second open pull request from the same stranger is still first-time:
        // nothing of theirs has been merged, so each one is gated on its own.
        PullRequest second = first;
        second.number = 4;
        const QList<PullRequest> withSecond{merged, open, first, second};
        check(!PullAuthorTrust::isEstablished(second, withSecond, trusted),
              "a second pull request from an unmerged author is still gated");
        check(PullAuthorTrust::pullOrdinal(first, withSecond) == 1,
              "the stranger's first pull request counts as their 1st");
        check(PullAuthorTrust::pullOrdinal(second, withSecond) == 2,
              "a later pull request counts the author's earlier ones");
        check(PullAuthorTrust::pullOrdinal(open, withSecond) == 2,
              "merged pull requests count toward the author's number too");
        check(PullAuthorTrust::ordinal(1) == QStringLiteral("1st") &&
                  PullAuthorTrust::ordinal(2) == QStringLiteral("2nd") &&
                  PullAuthorTrust::ordinal(3) == QStringLiteral("3rd") &&
                  PullAuthorTrust::ordinal(4) == QStringLiteral("4th") &&
                  PullAuthorTrust::ordinal(11) == QStringLiteral("11th") &&
                  PullAuthorTrust::ordinal(12) == QStringLiteral("12th") &&
                  PullAuthorTrust::ordinal(13) == QStringLiteral("13th") &&
                  PullAuthorTrust::ordinal(21) == QStringLiteral("21st") &&
                  PullAuthorTrust::ordinal(112) == QStringLiteral("112th"),
              "ordinals read as English");

        // authorName is chosen by whoever submitted the pull, so claiming the
        // owner's name while signing with your own key must not buy any trust.
        PullRequest impostor = first;
        impostor.number = 5;
        impostor.authorName = QStringLiteral("owner");
        check(!PullAuthorTrust::isEstablished(impostor, history, trusted),
              "a signed pull cannot claim trust by using the owner's name");

        PullRequest mine;
        mine.number = 6;
        mine.status = QStringLiteral("open");
        mine.author = QStringLiteral("myOwnNodeKey");
        check(PullAuthorTrust::isEstablished(mine, history, trusted),
              "this node's own signing key is established by definition");

        PullRequest local; // authored here from a patch: no signer key at all
        local.number = 7;
        local.status = QStringLiteral("open");
        local.authorName = QStringLiteral("Owner");
        check(PullAuthorTrust::isEstablished(local, history, trusted),
              "an unsigned local pull matches a trusted name");

        PullRequest anonymous;
        anonymous.number = 8;
        anonymous.status = QStringLiteral("open");
        check(!PullAuthorTrust::isEstablished(anonymous, history, trusted),
              "a pull with neither key nor name is never established");
    }

    {
        // The pull-request context rides with the run, so a restart still knows
        // the run is holding for a first-time contributor.
        ActionRun run;
        run.id = 7;
        run.owner = QStringLiteral("owner");
        run.name = QStringLiteral("repo");
        run.workflowPath = QStringLiteral(".forkmesh/ci.yml");
        run.commit = QStringLiteral("abc123");
        run.status = ActionStatus::AwaitingApproval;
        run.pullNumber = 32;
        run.pullAuthor = QStringLiteral("atomic-gateway-7974");
        run.firstTimePullAuthor = true;
        const ActionRun restored = ActionRun::fromJson(run.toJson());
        check(restored.pullNumber == 32 &&
                  restored.pullAuthor == run.pullAuthor &&
                  restored.firstTimePullAuthor,
              "an action run keeps its pull request approval gate across a restart");

        ActionRun pushRun;
        pushRun.commit = QStringLiteral("def456");
        check(!ActionRun::fromJson(pushRun.toJson()).firstTimePullAuthor,
              "a push run carries no pull request gate");
    }

    {
        // Dedicated nodes: `runs-on:` pins a workflow to named machines, so the
        // mesh can send tests to one node, Cloudflare deploys to a mirror, and
        // the iOS build to a Mac. Every other node must skip it entirely.
        const ActionWorkflow anywhere = ActionFile::parse(
            QStringLiteral(".forkmesh/ci.yml"),
            QStringLiteral("name: CI\non: [push]\n"
                           "jobs:\n  test:\n    steps:\n      - run: echo hi\n"));
        check(anywhere.runsOn.isEmpty(), "no runs-on leaves the workflow undedicated");
        check(anywhere.runsOnNode({QStringLiteral("mirror2")}),
              "an undedicated workflow runs on any node");

        const ActionWorkflow tests = ActionFile::parse(
            QStringLiteral(".forkmesh/tests.yml"),
            QStringLiteral("name: Tests\non: [push]\nruns-on: forkmesh\n"
                           "jobs:\n  test:\n    steps:\n      - run: ctest\n"));
        check(tests.runsOn == QStringList{QStringLiteral("forkmesh")},
              "top-level runs-on parses into one label");
        check(tests.runsOnNode({QStringLiteral("forkmesh"), QStringLiteral("linux")}),
              "the named node runs its dedicated workflow");
        check(!tests.runsOnNode({QStringLiteral("mirror2"), QStringLiteral("linux")}),
              "another node skips a workflow dedicated elsewhere");
        check(tests.runsOnNode({QStringLiteral("ForkMesh")}),
              "runs-on matching is case-insensitive");

        const ActionWorkflow deploy = ActionFile::parse(
            QStringLiteral(".forkmesh/deploy.yml"),
            QStringLiteral("name: Deploy\non: [push]\n"
                           "runs-on: [mirror2, mirror3]\n"
                           "jobs:\n  deploy:\n    steps:\n      - run: wrangler deploy\n"));
        check(deploy.runsOn.size() == 2 && deploy.runsOnNode({QStringLiteral("mirror3")}),
              "a runs-on list lets any listed node take the workflow");
        check(!deploy.runsOnNode({QStringLiteral("mac1")}),
              "a node outside the runs-on list stays out");

        const ActionWorkflow ios = ActionFile::parse(
            QStringLiteral(".forkmesh/ios.yml"),
            QStringLiteral("name: iOS\non: [workflow_dispatch]\n"
                           "jobs:\n  build:\n    runs-on:\n      - mac1\n"
                           "    steps:\n      - run: flutter build ios\n"));
        check(ios.runsOn == QStringList{QStringLiteral("mac1")},
              "job-level runs-on block lists parse too");
        check(!ios.runsOnNode({QStringLiteral("forkmesh")}),
              "a Linux node never picks up the Mac's iOS build");

        const ActionWorkflow any = ActionFile::parse(
            QStringLiteral(".forkmesh/any.yml"),
            QStringLiteral("name: Any\non: [push]\nruns-on: any\n"
                           "jobs:\n  j:\n    steps:\n      - run: echo hi\n"));
        check(any.runsOnNode({QStringLiteral("mirror2")}),
              "the reserved \"any\" label matches every node");

        // The Actions tab's "Run on" dropdown pins a node without editing the
        // YAML: the pin is stored on the repository record and stands in for the
        // workflow's own runs-on labels.
        QStringList pins;
        check(ActionFile::pinnedNode(pins, QStringLiteral(".forkmesh/ci.yml"))
                  .isEmpty(),
              "no pin means the workflow file decides where it runs");
        pins = ActionFile::setPinnedNode(pins, {QStringLiteral(".forkmesh/ci.yml")},
                                         QStringLiteral("Mac1"));
        check(ActionFile::pinnedNode(pins, QStringLiteral(".forkmesh/ci.yml")) ==
                  QStringLiteral("mac1"),
              "a pinned node is stored lower-cased and read back by path");
        check(ActionFile::pinnedNode(pins, QStringLiteral(".forkmesh/deploy.yml"))
                  .isEmpty(),
              "a pin only applies to the workflow it names");
        pins = ActionFile::setPinnedNode(
            pins,
            {QStringLiteral(".forkmesh/ci.yml"), QStringLiteral(".forkmesh/deploy.yml")},
            QStringLiteral("mirror2"));
        check(pins.size() == 2 &&
                  ActionFile::pinnedNode(pins, QStringLiteral(".forkmesh/ci.yml")) ==
                      QStringLiteral("mirror2"),
              "pinning every workflow at once repins the ones already pinned");
        pins = ActionFile::setPinnedNode(pins, {QStringLiteral(".forkmesh/ci.yml")},
                                         QString());
        check(pins.size() == 1 &&
                  ActionFile::pinnedNode(pins, QStringLiteral(".forkmesh/ci.yml"))
                      .isEmpty() &&
                  ActionFile::pinnedNode(pins, QStringLiteral(".forkmesh/deploy.yml")) ==
                      QStringLiteral("mirror2"),
              "clearing one pin leaves the others alone");

        // A pin decides on its own: the dedication check runs against the pinned
        // label, so a workflow the file sends elsewhere still runs here.
        ActionWorkflow pinned;
        pinned.runsOn = QStringList{
            ActionFile::pinnedNode(pins, QStringLiteral(".forkmesh/deploy.yml"))};
        check(pinned.runsOnNode({QStringLiteral("mirror2")}) &&
                  !pinned.runsOnNode({QStringLiteral("mac1")}),
              "the pinned node is the only one that runs the workflow");

        // This node's own labels: node name, mirror-executor name, platform, and
        // whatever capability tags the operator typed in Settings.
        const QStringList labels = ActionFile::nodeLabels(
            QStringLiteral("Mac1"), QString(),
            QStringLiteral("ios, xcode  flutter,ios"));
        check(labels.contains(QStringLiteral("mac1")),
              "the machine node name is always a label, lower-cased");
        check(labels.contains(QStringLiteral("ios")) &&
                  labels.contains(QStringLiteral("xcode")) &&
                  labels.contains(QStringLiteral("flutter")),
              "configured labels split on commas and whitespace");
        check(labels.count(QStringLiteral("ios")) == 1,
              "duplicate labels are collapsed");
        check(ActionFile::nodeLabels(QString(), QStringLiteral("mirror2"),
                                     QString())
                  .contains(QStringLiteral("mirror2")),
              "a headless node answers to its mirror-executor node name");
        check(ActionFile::parseLabelList(QStringLiteral("  ")).isEmpty(),
              "an empty label list normalizes to nothing");
    }

    {
        // `needs:` orders one workflow behind another for the same commit, so a
        // deploy can trust the CI run instead of repeating its test suite.
        const ActionWorkflow deploy = ActionFile::parse(
            QStringLiteral(".forkmesh/deploy.yml"),
            QStringLiteral("name: Deploy\non: [push]\n"
                           "needs: [.forkmesh/ci.yml]\n"
                           "jobs:\n  deploy:\n    steps:\n"
                           "      - run: ./deploy.sh\n"));
        check(deploy.valid &&
                  deploy.needs == QStringList{QStringLiteral(".forkmesh/ci.yml")},
              "top-level needs parses into one dependency");

        const ActionWorkflow jobLevel = ActionFile::parse(
            QStringLiteral(".forkmesh/publish.yml"),
            QStringLiteral("name: Publish\non: [push]\n"
                           "jobs:\n  publish:\n    needs:\n      - CI tests\n"
                           "    steps:\n      - run: echo hi\n"));
        check(jobLevel.needs == QStringList{QStringLiteral("CI tests")},
              "job-level needs block lists parse too");

        // Jobs inside one file are flattened into a single step list, so a
        // GitHub-style dependency between sibling jobs must not be read as a
        // dependency on another workflow (which would never resolve).
        const ActionWorkflow siblings = ActionFile::parse(
            QStringLiteral(".forkmesh/build.yml"),
            QStringLiteral("name: Build\non: [push]\n"
                           "jobs:\n  compile:\n    steps:\n      - run: make\n"
                           "  test:\n    needs: compile\n    steps:\n"
                           "      - run: make test\n"));
        check(siblings.needs.isEmpty(),
              "needs naming a job in the same file is not a workflow dependency");

        check(ActionNeeds::matches(QStringLiteral("ci"), QStringLiteral("CI tests"),
                                   QStringLiteral(".forkmesh/ci.yml")) &&
                  ActionNeeds::matches(QStringLiteral("CI TESTS"),
                                       QStringLiteral("CI tests"),
                                       QStringLiteral(".forkmesh/ci.yml")) &&
                  ActionNeeds::matches(QStringLiteral(".forkmesh/ci.yml"),
                                       QStringLiteral("CI tests"),
                                       QStringLiteral(".forkmesh/ci.yml")),
              "a needs entry matches by name, path, or file name, case-insensitively");
        check(!ActionNeeds::matches(QStringLiteral("ci-qt"),
                                    QStringLiteral("CI tests"),
                                    QStringLiteral(".forkmesh/ci.yml")),
              "a needs entry does not match an unrelated workflow");

        ActionRun dependency;
        dependency.id = 1;
        dependency.owner = QStringLiteral("forkmesh");
        dependency.name = QStringLiteral("forkmesh");
        dependency.workflowPath = QStringLiteral(".forkmesh/ci.yml");
        dependency.workflowName = QStringLiteral("CI tests");
        dependency.commit = QStringLiteral("abc123");
        ActionRun dependent = dependency;
        dependent.id = 2;
        dependent.workflowPath = QStringLiteral(".forkmesh/deploy.yml");
        dependent.workflowName = QStringLiteral("Deploy Cloudflare Worker");
        const QStringList needs{QStringLiteral(".forkmesh/ci.yml")};

        dependency.status = ActionStatus::Running;
        check(ActionNeeds::resolve(dependent, needs, {dependency}) ==
                  ActionNeeds::State::Waiting,
              "a dependent run waits while its dependency is still running");
        dependency.status = ActionStatus::AwaitingApproval;
        check(ActionNeeds::resolve(dependent, needs, {dependency}) ==
                  ActionNeeds::State::Waiting,
              "an unapproved dependency keeps the dependent queued");
        dependency.status = ActionStatus::Success;
        check(ActionNeeds::resolve(dependent, needs, {dependency}) ==
                  ActionNeeds::State::Ready,
              "a succeeded dependency releases the dependent run");
        dependency.status = ActionStatus::Failed;
        QString detail;
        check(ActionNeeds::resolve(dependent, needs, {dependency}, &detail) ==
                  ActionNeeds::State::Blocked &&
                  detail.contains(ActionStatus::Failed),
              "a failed dependency blocks the dependent run and reports why");

        // A dependency at a different commit is a different result entirely; the
        // gate is per-commit, and a missing record must not wedge the queue.
        dependency.status = ActionStatus::Success;
        ActionRun otherCommit = dependency;
        otherCommit.commit = QStringLiteral("def456");
        otherCommit.status = ActionStatus::Failed;
        check(ActionNeeds::resolve(dependent, needs, {otherCommit}) ==
                  ActionNeeds::State::Ready,
              "runs of the dependency at other commits do not gate this one");
        check(ActionNeeds::resolve(dependent, needs, {}) ==
                  ActionNeeds::State::Ready,
              "a dependency that never ran on this node is not a barrier");
        check(ActionNeeds::resolve(dependent, {}, {dependency}) ==
                  ActionNeeds::State::Ready,
              "a workflow with no needs is always ready");

        // Re-running a workflow prepends a fresh record; the newest one wins.
        ActionRun rerun = dependency;
        rerun.id = 3;
        rerun.status = ActionStatus::Running;
        ActionRun superseded = dependency;
        superseded.status = ActionStatus::Cancelled;
        check(ActionNeeds::resolve(dependent, needs, {rerun, superseded}) ==
                  ActionNeeds::State::Waiting,
              "the newest run of a dependency decides the gate");

        // The desktop owns the runner process, so a persisted `running` row
        // after relaunch means the controller disappeared, not that CI
        // failed. Requeue that exact approved run and keep its deploy waiting
        // rather than skipping it as a failed prerequisite.
        ActionRun interrupted = dependency;
        interrupted.status = ActionStatus::Running;
        interrupted.startedAtMs = 1234;
        interrupted.finishedAtMs = 5678;
        check(interrupted.requeueAfterDesktopInterruption() &&
                  interrupted.status == ActionStatus::Queued &&
                  interrupted.startedAtMs == 0 &&
                  interrupted.finishedAtMs == 0 &&
                  ActionNeeds::resolve(dependent, needs, {interrupted}) ==
                      ActionNeeds::State::Waiting,
              "a desktop-interrupted prerequisite restarts and keeps dependents queued");
        ActionRun completed = dependency;
        completed.status = ActionStatus::Success;
        check(!completed.requeueAfterDesktopInterruption() &&
                  completed.status == ActionStatus::Success,
              "startup recovery never requeues a terminal action run");
    }

    // --- Secret scanning -------------------------------------------------------
    {
        // Helper: init a fresh git repo, commit fileContent, then call
        // findSecretsInPush with no upstream ref (full tracked-file scan).
        const auto runSecretTest = [](const QByteArray &fileContent,
                                      const QString &expectedTitle) -> bool {
            QTemporaryDir td;
            if (!td.isValid())
                return false;
            const QString dir = td.path();
            const auto git = [&](const QStringList &args) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        QStringList{QStringLiteral("-C"), dir} + args);
                p.waitForFinished(10000);
            };
            git({QStringLiteral("init")});
            git({QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t")});
            git({QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("T")});
            check(writeTestFile(dir + QStringLiteral("/secret.env"), fileContent),
                  "write secret-scan fixture file");
            git({QStringLiteral("add"), QStringLiteral("secret.env")});
            git({QStringLiteral("commit"), QStringLiteral("-m"),
                 QStringLiteral("add")});
            const QList<RepoSecurityFinding> findings =
                RepoSecurity::findSecretsInPush(dir, {});
            for (const RepoSecurityFinding &f : findings)
                if (f.title == expectedTitle)
                    return true;
            return false;
        };

        // Per-provider checks (file-scan path)
        check(runSecretTest("GITHUB_TOKEN=ghp_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("GitHub token")),
              "secret scan detects GitHub PAT (ghp_)");
        check(runSecretTest("GITHUB_TOKEN=github_pat_AAAAAAAAAAAAAAAAAAAAAA\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("GitHub fine-grained PAT")),
              "secret scan detects GitHub fine-grained PAT (github_pat_)");
        check(runSecretTest("AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("AWS access key")),
              "secret scan detects AWS long-term access key (AKIA)");
        check(runSecretTest("KEY=ASIAQFI2EXAMPLE123456789012345\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("AWS temporary access key")),
              "secret scan detects AWS temporary STS key (ASIA)");
        check(runSecretTest(
                  "SLACK_TOKEN=xoxb-123456789012-123456789012-abcdefghijklmnopqrstuvwx\n",  // forkmesh-secret-scan:ignore-line
                  QStringLiteral("Slack token")),
              "secret scan detects Slack token (xoxb-)");
        check(runSecretTest(
                  "OPENAI_KEY=sk-proj-ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefgh\n",  // forkmesh-secret-scan:ignore-line
                  QStringLiteral("OpenAI API key")),
              "secret scan detects OpenAI project key (sk-proj-)");
        check(runSecretTest(
                  "OPENAI_KEY=sk-ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuv\n",  // forkmesh-secret-scan:ignore-line
                  QStringLiteral("OpenAI API key")),
              "secret scan detects OpenAI legacy key (sk- + 48 chars)");
        check(runSecretTest(
                  "ANTHROPIC_KEY=sk-ant-api03-ABCDEFGHIJKLMNOPQRSTUVWXYZabcde\n",  // forkmesh-secret-scan:ignore-line
                  QStringLiteral("Anthropic API key")),
              "secret scan detects Anthropic API key (sk-ant-)");
        check(runSecretTest("STRIPE_KEY=sk_live_ABCDEFGHIJKLMNOPQRSTUVWXyz\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("Stripe secret key")),
              "secret scan detects Stripe secret key (sk_live_)");
        check(runSecretTest("STRIPE_KEY=rk_test_ABCDEFGHIJKLMNOPQRSTUVWXyz\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("Stripe restricted key")),
              "secret scan detects Stripe restricted key (rk_test_)");
        check(runSecretTest("GOOGLE_KEY=AIzaSyDOCAbC123dEf456GhI789jKl012-MnOAB\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("Google API key")),
              "secret scan detects Google API key (AIza)");
        check(runSecretTest("GOOGLE_OAUTH=ya29.A0ARrda1ABCDEFGHIJKLMNOPQRSTUVWXYZ\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("Google OAuth token")),
              "secret scan detects Google OAuth token (ya29.)");
        check(runSecretTest("GOOGLE_SECRET=GOCSPX-ABCDEFGHIJKLMNOPQRSTUVWXabcde\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("Google OAuth client secret")),
              "secret scan detects Google OAuth client secret (GOCSPX-)");
        check(runSecretTest(
                  "SG=SG.AAAAAAAAAAAAAAAAAAAAAA.BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n",  // forkmesh-secret-scan:ignore-line
                  QStringLiteral("SendGrid API key")),
              "secret scan detects SendGrid API key (SG.)");
        check(runSecretTest("TWILIO=SKabcdef1234567890abcdef1234567890\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("Twilio auth token")),
              "secret scan detects Twilio auth token (SK + 32 hex chars)");
        check(runSecretTest("NPM=npm_AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("npm access token")),
              "secret scan detects npm access token (npm_)");
        check(runSecretTest("VAULT=hvs.CAESIABC123defGHI456jklMNO789pqr\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("HashiCorp Vault token")),
              "secret scan detects HashiCorp Vault service token (hvs.)");
        check(runSecretTest(
                  "CLOUDFLARE_API_TOKEN=aBcDeFgHiJkLmNoPqRsTuVwXyZaBcDeFgHiJkLMN\n",  // forkmesh-secret-scan:ignore-line
                  QStringLiteral("Cloudflare API token")),
              "secret scan detects Cloudflare API token (env-var anchored)");
        check(runSecretTest("-----BEGIN PRIVATE KEY-----\nMIIBIjANBg...\n"  // forkmesh-secret-scan:ignore-line
                            "-----END PRIVATE KEY-----\n",
                            QStringLiteral("PEM private key")),
              "secret scan detects PKCS#8 PEM private key block");
        check(runSecretTest("-----BEGIN RSA PRIVATE KEY-----\nMIIE...\n"  // forkmesh-secret-scan:ignore-line
                            "-----END RSA PRIVATE KEY-----\n",
                            QStringLiteral("PEM private key")),
              "secret scan detects RSA PEM private key block");
        check(runSecretTest(
                  "DATABASE_PASSWORD=\"s3cr3tPasswordThatIsLongEnough\"\n",  // forkmesh-secret-scan:ignore-line
                  QStringLiteral("Secret/token assignment")),
              "secret scan detects generic quoted secret assignment");

        // The generic assignment rule has no provider prefix to anchor it, so
        // recognisable placeholders (test fixtures, docs samples, template
        // holes) must not block a push.
        check(!runSecretTest(
                  "password: \"correct-horse-battery-staple\"\n",
                  QStringLiteral("Secret/token assignment")),
              "secret scan ignores the XKCD example password");
        check(!runSecretTest("API_TOKEN=\"your-token-goes-here-abcdef\"\n",
                             QStringLiteral("Secret/token assignment")),
              "secret scan ignores your-… placeholder token values");
        check(!runSecretTest("API_KEY=\"${FORKMESH_API_KEY_FROM_ENV}\"\n",
                             QStringLiteral("Secret/token assignment")),
              "secret scan ignores ${VAR} template holes");
        check(!runSecretTest("API_TOKEN=\"$CLOUDFLARE_API_TOKEN\"\n",
                             QStringLiteral("Secret/token assignment")),
              "secret scan ignores bare shell-variable indirection");
        check(!runSecretTest(
                  "API_TOKEN=\"$(load-forkmesh-token-from-keyring)\"\n",
                  QStringLiteral("Secret/token assignment")),
              "secret scan ignores shell command-substitution indirection");
        check(!runSecretTest("API_TOKEN=\"%CLOUDFLARE_API_TOKEN%\"\n",
                             QStringLiteral("Secret/token assignment")),
              "secret scan ignores Windows environment-variable indirection");
        check(!runSecretTest("password=\"xxxxxxxxxxxxxxxxxxxxxxxx\"\n",
                             QStringLiteral("Secret/token assignment")),
              "secret scan ignores single-character filler runs");
        // A provider-prefixed hit stays high-confidence even next to
        // placeholder-ish wording.
        check(runSecretTest("EXAMPLE_TOKEN=ghp_BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\n",  // forkmesh-secret-scan:ignore-line
                            QStringLiteral("GitHub token")),
              "secret scan still flags prefixed tokens in example wording");

        // Benign content must not trigger a false positive
        {
            QTemporaryDir td;
            const QString dir = td.path();
            const auto git = [&](const QStringList &args) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        QStringList{QStringLiteral("-C"), dir} + args);
                p.waitForFinished(10000);
            };
            git({QStringLiteral("init")});
            git({QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t")});
            git({QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("T")});
            check(writeTestFile(
                      dir + QStringLiteral("/readme.txt"),
                      "This is a benign file with no secrets.\nname: ForkMesh\n"),
                  "write benign secret-scan fixture file");
            git({QStringLiteral("add"), QStringLiteral("readme.txt")});
            git({QStringLiteral("commit"), QStringLiteral("-m"),
                 QStringLiteral("add")});
            check(RepoSecurity::findSecretsInPush(dir, {}).isEmpty(),
                  "secret scan produces no false positives on benign content");
        }

        // A line carrying the "forkmesh-secret-scan:ignore-line" marker is
        // skipped even though it matches a pattern — this is how the
        // scanner's own per-provider test fixtures (above) avoid flagging
        // themselves when this file is scanned as part of a tracked repo.
        {
            QTemporaryDir td;
            const QString dir = td.path();
            const auto git = [&](const QStringList &args) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        QStringList{QStringLiteral("-C"), dir} + args);
                p.waitForFinished(10000);
            };
            git({QStringLiteral("init")});
            git({QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t")});
            git({QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("T")});
            check(writeTestFile(dir + QStringLiteral("/fixtures.cpp"),
                                "AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE // forkmesh-secret-scan:ignore-line\n"),  // forkmesh-secret-scan:ignore-line
                  "write ignored secret-scan fixture file");
            git({QStringLiteral("add"), QStringLiteral("fixtures.cpp")});
            git({QStringLiteral("commit"), QStringLiteral("-m"),
                 QStringLiteral("add")});
            check(RepoSecurity::findSecretsInPush(dir, {}).isEmpty(),
                  "secret scan skips lines carrying the ignore-line marker");
        }

        // Diff-scan path: secrets introduced in HEAD commit are detected
        {
            QTemporaryDir td;
            const QString dir = td.path();
            const auto git = [&](const QStringList &args) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        QStringList{QStringLiteral("-C"), dir} + args);
                p.waitForFinished(10000);
            };
            git({QStringLiteral("init")});
            git({QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t")});
            git({QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("T")});
            // Base commit — clean
            check(writeTestFile(dir + QStringLiteral("/readme.txt"),
                                "placeholder\n"),
                  "write diff-scan base fixture file");
            git({QStringLiteral("add"), QStringLiteral("readme.txt")});
            git({QStringLiteral("commit"), QStringLiteral("-m"),
                 QStringLiteral("base")});
            // HEAD commit introduces a secret
            check(writeTestFile(dir + QStringLiteral("/creds.env"),
                                "AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE\n"),  // forkmesh-secret-scan:ignore-line
                  "write diff-scan secret fixture file");
            git({QStringLiteral("add"), QStringLiteral("creds.env")});
            git({QStringLiteral("commit"), QStringLiteral("-m"),
                 QStringLiteral("add creds")});
            const QList<RepoSecurityFinding> diffFindings =
                RepoSecurity::findSecretsInPush(dir, QStringLiteral("HEAD~1"));
            bool found = false;
            for (const RepoSecurityFinding &f : diffFindings)
                if (f.title == QStringLiteral("AWS access key"))
                    found = true;
            check(found, "diff-scan detects secrets added in pushed commits");
            check(RepoSecurity::findSecretsInPush(dir, QStringLiteral("HEAD")).isEmpty(),
                  "diff-scan reports no findings when HEAD has no new commits to push");
        }

        // Branch-diff health scans added lines on the GUI path. A large benign
        // diff must still find a credential near its end; this covers the
        // conservative candidate sieve that avoids running every credential
        // regular expression on each ordinary source line.
        {
            QString largeDiff = QStringLiteral(
                "diff --git a/src/example.cpp b/src/example.cpp\n"
                "--- a/src/example.cpp\n"
                "+++ b/src/example.cpp\n"
                "@@ -0,0 +1,6001 @@\n");
            const QString benignLine =
                QStringLiteral("+const QString label = QStringLiteral(\"ordinary UI text\");\n");
            for (int i = 0; i < 6000; ++i)
                largeDiff += benignLine;
            largeDiff += QStringLiteral(
                "+GITHUB_TOKEN=ghp_CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC\n"); // forkmesh-secret-scan:ignore-line
            const QList<RepoSecurityFinding> findings =
                RepoSecurity::findSecretsInDiff(largeDiff);
            const bool foundGitHubToken = std::any_of(
                findings.cbegin(), findings.cend(),
                [](const RepoSecurityFinding &finding) {
                    return finding.title == QStringLiteral("GitHub token");
                });
            check(foundGitHubToken,
                  "diff health scans a credential after a large benign diff");
        }

        // Diff-scan path: secrets removed in a commit must NOT trigger
        {
            QTemporaryDir td;
            const QString dir = td.path();
            const auto git = [&](const QStringList &args) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        QStringList{QStringLiteral("-C"), dir} + args);
                p.waitForFinished(10000);
            };
            git({QStringLiteral("init")});
            git({QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t")});
            git({QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("T")});
            // Base commit WITH a secret (already in history)
            check(writeTestFile(dir + QStringLiteral("/creds.env"),
                                "AWS_ACCESS_KEY_ID=AKIAIOSFODNN7EXAMPLE\n"),  // forkmesh-secret-scan:ignore-line
                  "write remediation base fixture file");
            git({QStringLiteral("add"), QStringLiteral("creds.env")});
            git({QStringLiteral("commit"), QStringLiteral("-m"),
                 QStringLiteral("base")});
            // HEAD commit removes the secret (remediation commit)
            check(writeTestFile(dir + QStringLiteral("/creds.env"),
                                "# credentials removed\n"),
                  "write remediation head fixture file");
            git({QStringLiteral("add"), QStringLiteral("creds.env")});
            git({QStringLiteral("commit"), QStringLiteral("-m"),
                 QStringLiteral("remove creds")});
            check(RepoSecurity::findSecretsInPush(dir, QStringLiteral("HEAD~1")).isEmpty(),
                  "diff-scan ignores secrets removed (not added) in commits");
        }

        // Mark-safe regression: QString regex offsets are UTF-16 offsets, not
        // UTF-8 byte offsets. Non-ASCII text before a finding used to make its
        // reported line drift and append the marker to unrelated code. A shell
        // continuation made that especially damaging: appending after the '\\'
        // broke the command. The marker now belongs at the end of the complete
        // logical line, and both full-file and outgoing-diff scans honor it.
        {
            QTemporaryDir td;
            const QString dir = td.path();
            const auto git = [&](const QStringList &args) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        QStringList{QStringLiteral("-C"), dir} + args);
                p.waitForFinished(10000);
                return p.exitCode() == 0;
            };
            check(git({QStringLiteral("init")}),
                  "initialize mark-safe regression repo");
            git({QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t")});
            git({QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("T")});
            check(writeTestFile(dir + QStringLiteral("/install.sh"),
                                QByteArray("#!/bin/sh\n"
                                           "# Running bootstrap\342\200\246\n"
                                           "CLOUDFLARE_API_TOKEN=\"real-looking-token-value\" \\\n"
                                           "HOME=\"$HOME\" \\\n"
                                           "  python3 bootstrap.py\n")),
                  "write UTF-8 continued-line mark-safe fixture");
            check(git({QStringLiteral("add"), QStringLiteral("install.sh")}) &&
                      git({QStringLiteral("commit"), QStringLiteral("-m"),
                           QStringLiteral("add bootstrap")}),
                  "commit mark-safe regression fixture");

            const QList<RepoSecurityFinding> before =
                RepoSecurity::findSecretsInPush(dir, {});
            const auto findingIt = std::find_if(
                before.cbegin(), before.cend(), [](const RepoSecurityFinding &f) {
                    return f.title == QStringLiteral("Secret/token assignment");
                });
            check(findingIt != before.cend() && findingIt->line == 3,
                  "secret scan reports UTF-8 finding on the correct source line");
            RepoSecurityFinding staleFinding;
            staleFinding.path = QStringLiteral("install.sh");
            staleFinding.line = 2;
            check(!RepoSecurity::markFindingSafe(dir, staleFinding),
                  "mark safe refuses a stale finding on unrelated source");
            check(findingIt != before.cend() &&
                      RepoSecurity::markFindingSafe(dir, *findingIt),
                  "mark safe annotates a continued shell statement");

            QFile markedFile(dir + QStringLiteral("/install.sh"));
            check(markedFile.open(QIODevice::ReadOnly),
                  "open marked-safe fixture");
            const QByteArray marked = markedFile.readAll();
            check(marked.contains(
                      "HOME=\"$HOME\" \\\n  python3 bootstrap.py  # "
                      "forkmesh-secret-scan:ignore-line\n"),
                  "mark safe preserves continuations and annotates their final line");
            check(!marked.contains("\\  # forkmesh-secret-scan:ignore-line"),
                  "mark safe never appends a comment after a continuation backslash");
            check(RepoSecurity::findSecretsInPush(dir, {}).isEmpty(),
                  "full-file rescan honors a logical-line safe marker");

            check(git({QStringLiteral("add"), QStringLiteral("install.sh")}) &&
                      git({QStringLiteral("commit"), QStringLiteral("-m"),
                           QStringLiteral("mark safe")}),
                  "commit mark-safe annotation fixture");
            check(RepoSecurity::findSecretsInPush(
                      dir, QStringLiteral("HEAD~2")).isEmpty(),
                  "outgoing-diff rescan honors a logical-line safe marker");
        }

        // Annotation uses syntax valid for the file type and refuses formats
        // (such as JSON) where adding any comment would corrupt the document.
        {
            QTemporaryDir td;
            const QString dir = td.path();
            check(writeTestFile(dir + QStringLiteral("/fixture.html"),
                                "<div data-api-token=\"real-looking-token-value\"></div>\n"),
                  "write markup mark-safe fixture");
            RepoSecurityFinding finding;
            finding.path = QStringLiteral("fixture.html");
            finding.line = 1;
            check(RepoSecurity::markFindingSafe(dir, finding),
                  "mark safe supports markup comments");
            QFile html(dir + QStringLiteral("/fixture.html"));
            check(html.open(QIODevice::ReadOnly) &&
                      html.readAll().contains(
                          "<!-- forkmesh-secret-scan:ignore-line -->"),
                  "mark safe writes a complete markup comment");

            check(writeTestFile(dir + QStringLiteral("/fixture.json"),
                                "{\"api_token\":\"real-looking-token-value\"}\n"),
                  "write JSON mark-safe fixture");
            finding.path = QStringLiteral("fixture.json");
            check(!RepoSecurity::markFindingSafe(dir, finding),
                  "mark safe refuses formats without comments");
            QFile json(dir + QStringLiteral("/fixture.json"));
            check(json.open(QIODevice::ReadOnly) &&
                      json.readAll() ==
                          QByteArray("{\"api_token\":\"real-looking-token-value\"}\n"),
                  "failed JSON annotation leaves the file unchanged");
        }

        {
            QTemporaryDir td;
            const QString dir = td.path();
            const auto git = [&](const QStringList &args) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        QStringList{QStringLiteral("-C"), dir} + args);
                p.waitForFinished(10000);
                return p.exitCode() == 0;
            };
            git({QStringLiteral("init")});
            git({QStringLiteral("config"), QStringLiteral("user.email"),
                 QStringLiteral("t@t")});
            git({QStringLiteral("config"), QStringLiteral("user.name"),
                 QStringLiteral("T")});
            const QByteArray wrapped(
                "void main() {\n"
                "  final credentials = AgentCredentials(\n"
                "    apiKey:\n"
                "        'wrapped-real-looking-value-1234567890',\n"
                "    model: 'opus',\n"
                "  );\n"
                "}\n");
            check(writeTestFile(dir + QStringLiteral("/wrapped.dart"), wrapped)
                      && git({QStringLiteral("add"), QStringLiteral(".")})
                      && git({QStringLiteral("commit"), QStringLiteral("-m"),
                              QStringLiteral("wrapped")}),
                  "commit a wrapped-assignment mark-safe fixture");
            const QList<RepoSecurityFinding> wrappedFindings =
                RepoSecurity::findSecretsInPush(dir, {});
            check(wrappedFindings.size() == 1
                      && wrappedFindings.first().line == 3,
                  "an assignment whose value wraps to the next line is flagged "
                  "on the key's line");
            check(!wrappedFindings.isEmpty()
                      && RepoSecurity::markFindingSafe(dir,
                                                       wrappedFindings.first()),
                  "mark safe annotates an assignment whose value wraps to the "
                  "next line");
            QFile wrappedFile(dir + QStringLiteral("/wrapped.dart"));
            check(wrappedFile.open(QIODevice::ReadOnly)
                      && wrappedFile.readAll().contains(
                          "'wrapped-real-looking-value-1234567890',  // "
                          "forkmesh-secret-scan:ignore-line\n"),
                  "the marker lands on the line that ends the wrapped match");
            check(RepoSecurity::findSecretsInPush(dir, {}).isEmpty(),
                  "a marker on the value line of a wrapped assignment "
                  "suppresses the finding");
            check(RepoSecurity::markFindingSafe(dir, wrappedFindings.first()),
                  "marking an already marked wrapped assignment succeeds");
        }
    }

    // --- Per-manifest dependency counts (adhoc #86) -----------------------
    {
        QTemporaryDir td;
        check(td.isValid(), "dependency-count temp repo created");
        const QString dir = td.path();
        const auto git = [&](const QStringList &args) {
            QProcess p;
            p.start(QStringLiteral("git"),
                    QStringList{QStringLiteral("-C"), dir} + args);
            p.waitForFinished(10000);
        };
        git({QStringLiteral("init")});
        git({QStringLiteral("config"), QStringLiteral("user.email"),
             QStringLiteral("t@t")});
        git({QStringLiteral("config"), QStringLiteral("user.name"),
             QStringLiteral("T")});
        // The scanner reads line-by-line (one "name": "version" entry per
        // line), so the fixture must be pretty-printed, not minified.
        check(writeTestFile(dir + QStringLiteral("/package.json"), R"JSON({
  "dependencies": {
    "left-pad": "1.0.0",
    "chalk": "*"
  },
  "devDependencies": {
    "jest": "^29.0.0"
  }
}
)JSON"),
              "write package manifest fixture file");
        check(writeTestFile(dir + QStringLiteral("/requirements.txt"),
                            "requests==2.31.0\nflask>=2.0\n# a comment\n\nclick\n"),
              "write requirements manifest fixture file");
        git({QStringLiteral("add"), QStringLiteral("package.json"),
             QStringLiteral("requirements.txt")});

        RepoSecurityInput input;
        input.owner = QStringLiteral("alice");
        input.name = QStringLiteral("project");
        input.localPath = dir;

        const RepoSecuritySnapshot snapshot = RepoSecurity::scan(input);
        check(snapshot.dependencyCounts.value(QStringLiteral("package.json")) == 3,
              "full scan counts every declared package.json dependency (prod + dev)");
        check(snapshot.dependencyCounts.value(QStringLiteral("requirements.txt")) == 3,
              "full scan counts requirements.txt entries, skipping blanks/comments");

        const RepoSecurityManifestScan pkgRescan =
            RepoSecurity::scanManifest(input, QStringLiteral("package.json"));
        check(pkgRescan.dependencyCount == 3,
              "single-manifest rescan matches the full scan's package.json count");
        bool flaggedChalk = false;
        for (const RepoSecurityFinding &f : pkgRescan.findings)
            if (f.title == QStringLiteral("chalk") && f.detail.contains(QLatin1Char('*')))
                flaggedChalk = true;
        check(flaggedChalk,
              "single-manifest rescan on package.json surfaces chalk's floating \"*\" version");

        const RepoSecurityManifestScan reqRescan =
            RepoSecurity::scanManifest(input, QStringLiteral("requirements.txt"));
        check(reqRescan.dependencyCount == 3,
              "single-manifest rescan matches the full scan's requirements.txt count");

        const RepoSecurityManifestScan missingRescan =
            RepoSecurity::scanManifest(input, QStringLiteral("does-not-exist.json"));
        check(missingRescan.dependencyCount == 0 && missingRescan.findings.isEmpty(),
              "single-manifest rescan on a missing/untracked path is a safe no-op");
    }

    {
        // Agent metadata is the only unsealed local session state. Provider
        // credentials are launch-time config and can never become workspace
        // metadata or a shared owner-sealed session payload by field alias.
        AgentSession session;
        session.id = 42;
        session.owner = QStringLiteral("alice");
        session.name = QStringLiteral("project");
        session.prompt = QStringLiteral("review the owner-sealed task");
        session.provider = QStringLiteral("claude-code");
        const QJsonObject serialized = session.toJson();
        for (const QString &forbidden :
             {QStringLiteral("accessToken"),
              QStringLiteral("refreshToken"),
              QStringLiteral("oauth"),
              QStringLiteral("apiKey"),
              QStringLiteral("anthropicApiKey"),
              QStringLiteral("openAiApiKey"),
              QStringLiteral("credentials")}) {
            check(!serialized.contains(forbidden),
                  qPrintable(QStringLiteral(
                      "agent metadata excludes provider credential field %1")
                                 .arg(forbidden)));
        }
    }

    {
        // Background-activity bus (adhoc #421): the footer strip is driven purely
        // by these tickets, so a lost or double-retired one leaves a spinner
        // running forever (or hides work that is still going).
        QStringList seen;
        forkmesh::LogSource lastSource;
        forkmesh::BackgroundTicket lastTicket;
        forkmesh::BackgroundActivity::setListener(nullptr);
        const quint64 startup = forkmesh::BackgroundActivity::begin(
            QStringLiteral("startup"), QStringLiteral("already running"),
            forkmesh::ActionTelemetry::Execution::Worker);
        forkmesh::BackgroundActivity::setListener(
            [&seen, &lastSource, &lastTicket](
                const forkmesh::BackgroundTicket &ticket, bool started) {
                if (started)
                    lastSource = ticket.source;
                lastTicket = ticket;
                const forkmesh::ActionTelemetry::Execution execution =
                    ticket.execution;
                const QString lane =
                    execution == forkmesh::ActionTelemetry::Execution::Async
                        ? QStringLiteral("async")
                        : execution ==
                                  forkmesh::ActionTelemetry::Execution::Worker
                              ? QStringLiteral("worker")
                              : execution == forkmesh::ActionTelemetry::Execution::UiDeferred
                                    ? QStringLiteral("ui-deferred")
                                    : QStringLiteral("ui");
                seen.append(QStringLiteral("%1:%2:%3:%4:%5")
                                .arg(started ? QStringLiteral("+")
                                             : QStringLiteral("-"))
                                .arg(ticket.id)
                                .arg(ticket.kind, ticket.detail, lane));
            });
        check(seen == QStringList({
                  QStringLiteral("+:%1:startup:already running:worker")
                      .arg(startup)}),
              "attaching a listener replays work that started before the footer");
        const quint64 first =
            forkmesh::BackgroundActivity::begin(QStringLiteral("git"),
                                                QStringLiteral("git log"));
        const quint64 second =
            forkmesh::BackgroundActivity::begin(QStringLiteral("net"));
        check(first != second && first != 0 && second != 0,
              "each background ticket gets its own non-zero id");
        forkmesh::BackgroundActivity::end(first);
        forkmesh::BackgroundActivity::end(second);
        forkmesh::BackgroundActivity::end(startup);
        forkmesh::BackgroundActivity::end(0); // no-op guard for untracked work
        // adhoc #186: a retired ticket still says what it was, so a listener
        // can tell a failed `git status` from a failed fetch without keeping
        // its own id table.
        check(seen == QStringList({
                  QStringLiteral("+:%1:startup:already running:worker")
                      .arg(startup),
                  QStringLiteral("+:%1:git:git log:async").arg(first),
                  QStringLiteral("+:%1:net::async").arg(second),
                  QStringLiteral("-:%1:git:git log:async").arg(first),
                  QStringLiteral("-:%1:net::async").arg(second),
                  QStringLiteral("-:%1:startup:already running:worker")
                      .arg(startup)}),
              "every begin/end pair reaches the listener exactly once, in order, "
              "and the retire announcement still names the work");

        {
            const forkmesh::BackgroundScope scope(QStringLiteral("scan"));
            check(seen.size() == 7 && seen.last().startsWith(QLatin1Char('+')),
                  "a background scope opens its ticket on construction");
        }
        check(seen.size() == 8 && seen.last().startsWith(QLatin1Char('-')),
              "a background scope retires its ticket when it unwinds");

        // adhoc #36: every outcome line used to point at the one flush loop in
        // MainWindowChat.cpp, which says nothing about which flow is spinning.
        // A ticket records the line that opened it, and a chokepoint every
        // caller shares (waitForGit, runGitCapture) forwards the site its own
        // caller captured so the link lands on the code that asked for the work.
        {
            const int here = __LINE__ + 1;
            const quint64 own = forkmesh::BackgroundActivity::begin(
                QStringLiteral("scan"), QStringLiteral("owned"));
            check(lastSource.isValid() && lastSource.line == here &&
                      QString::fromUtf8(lastSource.file)
                          .endsWith(QStringLiteral("test_crypto.cpp")),
                  qPrintable(QStringLiteral(
                      "a ticket records the call site that opened it "
                      "(adhoc #36, saw %1:%2, wanted line %3)")
                                 .arg(QString::fromUtf8(lastSource.file))
                                 .arg(lastSource.line)
                                 .arg(here)));
            forkmesh::BackgroundActivity::end(own);

            const auto chokepoint = [](const forkmesh::LogSource &source =
                                           forkmesh::logSourceHere()) {
                return forkmesh::BackgroundActivity::begin(
                    QStringLiteral("git"), QStringLiteral("git status"),
                    forkmesh::ActionTelemetry::Execution::Worker, source);
            };
            const int callerLine = __LINE__ + 1;
            forkmesh::BackgroundActivity::end(chokepoint());
            check(lastSource.line == callerLine,
                  qPrintable(QStringLiteral(
                      "a forwarded source reports the caller, not the helper "
                      "(adhoc #36, saw line %1, wanted %2)")
                                 .arg(lastSource.line)
                                 .arg(callerLine)));
            // adhoc #186: the source also names the function it was captured
            // in, and the forwarded one keeps the caller's. A lambda's
            // "operator()" names every closure in the program, so it reads as
            // nothing rather than as a caller.
            const QString callingFunction = QString::fromUtf8(__FUNCTION__);
            check(forkmesh::logSourceFunctionName(lastSource) == callingFunction,
                  qPrintable(QStringLiteral(
                      "a forwarded source names the calling function "
                      "(adhoc #186, saw '%1', wanted '%2')")
                                 .arg(forkmesh::logSourceFunctionName(lastSource),
                                      callingFunction)));
            const auto inLambda = [] {
                return forkmesh::BackgroundActivity::begin(
                    QStringLiteral("scan"));
            };
            forkmesh::BackgroundActivity::end(inLambda());
            check(QString::fromUtf8(lastSource.function) ==
                          QStringLiteral("operator()") &&
                      forkmesh::logSourceFunctionName(lastSource).isEmpty(),
                  "a ticket opened inside a lambda names no function instead "
                  "of 'operator()' (adhoc #186)");
        }

        // adhoc #186: the routine named on this thread rides on every ticket
        // opened under it; the innermost one wins; an empty name installs
        // nothing; and it is gone once the scope unwinds. Work that names its
        // own routine (a request carried to a worker) keeps that one.
        {
            const forkmesh::BackgroundRoutine outer(
                QStringLiteral("Git rail status timer (every 10s)"));
            forkmesh::BackgroundActivity::end(forkmesh::BackgroundActivity::begin(
                QStringLiteral("git"), QStringLiteral("git status")));
            check(lastTicket.routine ==
                      QStringLiteral("Git rail status timer (every 10s)"),
                  qPrintable(QStringLiteral(
                      "a ticket carries the routine named on its thread "
                      "(adhoc #186, saw '%1')").arg(lastTicket.routine)));
            {
                const forkmesh::BackgroundRoutine inner(
                    QStringLiteral("opening repo me/r"));
                const forkmesh::BackgroundRoutine blank(QString());
                forkmesh::BackgroundActivity::end(
                    forkmesh::BackgroundActivity::begin(QStringLiteral("git")));
                check(lastTicket.routine == QStringLiteral("opening repo me/r"),
                      "the innermost routine wins, and an empty name installs "
                      "nothing (adhoc #186)");
                check(forkmesh::BackgroundRoutine::unlessNamed(
                          QStringLiteral("generic refresh")).isEmpty(),
                      "unlessNamed() yields nothing while a routine is named "
                      "(adhoc #186)");
            }
            forkmesh::BackgroundWork carried;
            carried.kind = QStringLiteral("git");
            carried.detail = QStringLiteral("git status --porcelain=v1 -z");
            carried.source = forkmesh::logSourceHere();
            carried.routine = QStringLiteral("agent files diff poll · agent #7");
            carried.workingDirectory = QStringLiteral("/tmp/wt");
            carried.queuedMs = 1200;
            forkmesh::BackgroundActivity::end(
                forkmesh::BackgroundActivity::begin(carried),
                QStringLiteral("failed (exit 128)"));
            check(lastTicket.routine ==
                          QStringLiteral("agent files diff poll · agent #7") &&
                      lastTicket.workingDirectory == QStringLiteral("/tmp/wt") &&
                      lastTicket.queuedMs == 1200 &&
                      lastTicket.outcome == QStringLiteral("failed (exit 128)") &&
                      lastTicket.kind == QStringLiteral("git"),
                  "work that names its own routine keeps it over the thread's, "
                  "and its checkout, queue wait and outcome reach the listener "
                  "(adhoc #186)");
        }
        forkmesh::BackgroundActivity::end(
            forkmesh::BackgroundActivity::begin(QStringLiteral("git")));
        check(lastTicket.routine.isEmpty() &&
                  forkmesh::BackgroundRoutine::current().isEmpty() &&
                  forkmesh::BackgroundRoutine::unlessNamed(
                      QStringLiteral("generic refresh")) ==
                      QStringLiteral("generic refresh"),
              "a routine is gone once its scope unwinds (adhoc #186)");

        const int announced = seen.size();
        forkmesh::BackgroundActivity::setListener(nullptr);
        forkmesh::BackgroundActivity::end(
            forkmesh::BackgroundActivity::begin(QStringLiteral("git")));
        check(seen.size() == announced,
              "a detached bus drops announcements instead of calling a dead "
              "listener");
    }

    {
        // Log outcome lines (adhoc #419/#421): the ✓ / ✕ marker reflects the
        // declared execution lane, never how long the operation happened to run.
        const QString ok = forkmesh::backgroundOkGlyph();
        const QString no = forkmesh::backgroundNotGlyph();
        check(forkmesh::backgroundOutcomeLine(QStringLiteral("git"), 1, 1400,
                                              QStringLiteral("git log"), true) ==
                  QStringLiteral("Background %1 git backgrounded (1.4s) - git log")
                      .arg(ok),
              "async work logs a checkmark, its duration and the caller's note");
        check(forkmesh::backgroundOutcomeLine(QStringLiteral("git"), 24, 61,
                                              QString(), true) ==
                  QStringLiteral("Background %1 git %2%3 backgrounded "
                                 "(longest 61ms)")
                      .arg(ok)
                      .arg(QChar(0x00D7))
                      .arg(24),
              "a fast async burst remains correctly marked as backgrounded");
        check(forkmesh::backgroundOutcomeLine(
                  QString(), 1, 1, QString(), false)
                  .startsWith(QStringLiteral("Background %1 work").arg(no)),
              "declared GUI-thread work logs a red x even when it is fast, and "
              "an unnamed kind still reads as something");
        // adhoc #1620/#36: a note is free-form, and the whole command line is
        // the point — a repeating ticket is only actionable if you can paste it
        // into a shell. So a realistic diff over dozens of files survives
        // intact, and the cap is only a backstop for a pathological caller.
        {
            QStringList realistic;
            for (int i = 0; i < 30; ++i)
                realistic << QStringLiteral(":(literal)desktop/src/File%1.cpp")
                                 .arg(i);
            const QString whole = QStringLiteral("git diff main -- ") +
                                  realistic.join(QLatin1Char(' '));
            check(whole.size() > 900 &&
                      forkmesh::backgroundOutcomeLine(QStringLiteral("git"), 2,
                                                      224, whole, true)
                          .endsWith(whole),
                  "a full command line reaches the log line unshortened "
                  "(adhoc #36)");

            QStringList pathspecs;
            for (int i = 0; i < 300; ++i)
                pathspecs << QStringLiteral(":(literal)a/b/c%1.py").arg(i);
            const QString flood = QStringLiteral("git diff main -- ") +
                                  pathspecs.join(QLatin1Char(' '));
            const QString capped = forkmesh::backgroundOutcomeLine(
                QStringLiteral("git"), 2, 224, flood, true);
            check(capped.size() < forkmesh::kBackgroundDetailMaxChars + 100 &&
                      capped.endsWith(QString::fromUtf8("\xE2\x80\xA6")),
                  "a pathological note is still elided instead of pasted whole "
                  "(adhoc #1620)");
            check(forkmesh::backgroundDetailNote(
                      QStringLiteral("  git   log\n--numstat ")) ==
                      QStringLiteral("git log --numstat"),
                  "a note is flattened to one line before it reaches the log");
        }
        check(forkmesh::backgroundElapsedText(-5) == QStringLiteral("0ms") &&
                  forkmesh::backgroundElapsedText(999) ==
                      QStringLiteral("999ms") &&
                  forkmesh::backgroundElapsedText(1000) ==
                      QStringLiteral("1.0s"),
              "elapsed text stays short and never renders a negative clock skew");

        // adhoc #186: the line also says who asked for the work and where it
        // ran — every routine the tally folded in, the function that called,
        // the repository and the agent worktree — and a run that failed says
        // so with its reason. The command line stays last, so it can still be
        // pasted from the end of the line.
        {
            forkmesh::BackgroundOutcomeContext context;
            context.routines = {QStringLiteral("Git rail status timer (every 10s)"),
                                QStringLiteral("window activation refresh")};
            context.function = QStringLiteral("refreshRepoChangeBadge");
            context.repo = QStringLiteral("me/r");
            context.agent = QStringLiteral("#186 running");
            context.path = QStringLiteral("~/ignored-when-a-repo-matched");
            context.failures = 1;
            context.lastFailure = QStringLiteral("exit 128");
            context.longestQueuedMs = 1200;
            const QString full = forkmesh::backgroundOutcomeLine(
                QStringLiteral("git"), 3, 34,
                QStringLiteral("git status --porcelain=v1 -z"), true, context);
            const QString wanted =
                QStringLiteral("Background ") + ok + QStringLiteral(" git ") +
                QChar(0x00D7) +
                QStringLiteral("3 backgrounded (longest 34ms, 1 failed: exit 128)"
                               " · routine: Git rail status timer (every 10s), "
                               "window activation refresh"
                               " · caller: refreshRepoChangeBadge"
                               " · repo: me/r · agent: #186 running"
                               " · longest queued: 1.2s"
                               " - git status --porcelain=v1 -z");
            check(full == wanted,
                  qPrintable(QStringLiteral(
                      "an attributed outcome line names routine, caller, repo, "
                      "agent, failure and queue wait before the command "
                      "(adhoc #186)\n  saw:    %1\n  wanted: %2")
                                 .arg(full, wanted)));

            forkmesh::BackgroundOutcomeContext single;
            single.failures = 1;
            single.lastFailure = QStringLiteral("timed out");
            single.path = QStringLiteral("~/scratch");
            single.longestQueuedMs = 50; // the normal worker hand-off: unsaid
            check(forkmesh::backgroundOutcomeLine(QStringLiteral("git"), 1, 31,
                                                  QStringLiteral("git fetch"),
                                                  true, single) ==
                      QStringLiteral("Background %1 git backgrounded "
                                     "(31ms, failed: timed out) · dir: ~/scratch"
                                     " - git fetch")
                          .arg(ok),
                  "a single failed run says why, a directory outside every "
                  "repository is shown as-is, and a sub-100ms queue wait is "
                  "not worth a word (adhoc #186)");

            check(forkmesh::backgroundOutcomeLine(
                      QStringLiteral("git"), 1, 1400, QStringLiteral("git log"),
                      true, forkmesh::BackgroundOutcomeContext()) ==
                      forkmesh::backgroundOutcomeLine(QStringLiteral("git"), 1,
                                                      1400,
                                                      QStringLiteral("git log"),
                                                      true),
                  "an empty context renders exactly the plain line (adhoc #186)");

            check(forkmesh::backgroundFailureText(QStringLiteral("succeeded"))
                          .isEmpty() &&
                      forkmesh::backgroundFailureText(QStringLiteral("completed"))
                          .isEmpty() &&
                      forkmesh::backgroundFailureText(QStringLiteral("failed"))
                          .isEmpty() &&
                      forkmesh::backgroundFailureText(
                          QStringLiteral("failed (exit 128)")) ==
                          QStringLiteral("exit 128") &&
                      forkmesh::backgroundFailureText(QStringLiteral("timed out")) ==
                          QStringLiteral("timed out"),
                  "a failure reason is the outcome minus the word 'failed' "
                  "(adhoc #186)");

            const QString git = QStringLiteral("git");
            const auto answer = [&git](const QStringList &args, int code) {
                return forkmesh::gitProbeAnswerOutcome(git, args, code);
            };
            const QString dir = QStringLiteral("/tmp/repo");
            check(answer({QStringLiteral("-C"), dir, QStringLiteral("merge-base"),
                          QStringLiteral("--is-ancestor"), QStringLiteral("a"),
                          QStringLiteral("main")}, 1) ==
                          QStringLiteral("answered (exit 1)") &&
                      !answer({QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                               QStringLiteral("--quiet"),
                               QStringLiteral("refs/heads/x")}, 1).isEmpty() &&
                      !answer({QStringLiteral("merge-tree"),
                               QStringLiteral("--write-tree"), QStringLiteral("a"),
                               QStringLiteral("main")}, 1).isEmpty() &&
                      forkmesh::gitProbeAnswerOutcome(
                          QStringLiteral("/usr/bin/git"),
                          {QStringLiteral("diff"), QStringLiteral("--quiet")}, 1) ==
                          QStringLiteral("answered (exit 1)"),
                  "a git question whose exit 1 means 'no' retires as an answer");
            check(answer({QStringLiteral("merge-base"),
                          QStringLiteral("--is-ancestor"), QStringLiteral("a"),
                          QStringLiteral("b")}, 128).isEmpty() &&
                      answer({QStringLiteral("rev-parse"), QStringLiteral("HEAD")}, 1)
                          .isEmpty() &&
                      answer({QStringLiteral("fetch"), QStringLiteral("--quiet")}, 1)
                          .isEmpty() &&
                      forkmesh::gitProbeAnswerOutcome(
                          QStringLiteral("sh"),
                          {QStringLiteral("merge-base"),
                           QStringLiteral("--is-ancestor")}, 1)
                          .isEmpty(),
                  "a real git failure, a non-probe exit 1 and a non-git program "
                  "stay failures");
            check(forkmesh::backgroundFailureText(
                      QStringLiteral("answered (exit 1)")).isEmpty(),
                  "an answered probe adds no 'failed' to the outcome line, so the "
                  "log classifier never badges it ERROR");
        }

        // Both log views paint the message body in one colour, so the marker is
        // recoloured after escaping — green for ✓, red for ✕, and only the first
        // of each so a note that happens to contain one can't smear the line.
        const QString painted = forkmesh::colorizeBackgroundMarker(
            QStringLiteral("Background %1 net %2").arg(no, ok));
        check(painted.contains(QStringLiteral("#f85149")) &&
                  painted.contains(QStringLiteral("#3fb950")),
              "the outcome marker is lifted into its own coloured span");
        check(forkmesh::colorizeBackgroundMarker(QStringLiteral("no marker")) ==
                  QStringLiteral("no marker"),
              "a line without a marker is passed through untouched");
    }

    // --- Hourly local backups of the live database ----------------------
    // The Settings -> Data backup panel restores whatever these helpers list,
    // so the naming round-trip, the newest-first order, the retention cut and
    // the tar argument order are the contract that makes recovery work.
    {
        QTemporaryDir backupRoot;
        const QDateTime taken =
            QDateTime::fromString(QStringLiteral("2026-07-28T09:15:00"),
                                  Qt::ISODate);
        const QString name = forkmesh::backupFileName(taken);
        check(name == QStringLiteral("forkmesh-backup-20260728-091500.tar.gz"),
              "a snapshot is named after the moment it was taken");
        check(forkmesh::backupTimestampFromName(name) == taken,
              "the timestamp round-trips out of the file name");
        // Only our own archives may be listed: anything else in the folder is a
        // file the user put there, and restoring or pruning it would be wrong.
        check(!forkmesh::backupTimestampFromName(
                   QStringLiteral("forkmesh-backup-20260728-091500-copy.tar.gz"))
                   .isValid() &&
                  !forkmesh::backupTimestampFromName(
                       QStringLiteral("holiday-photos.tar.gz"))
                       .isValid(),
              "a look-alike or unrelated archive is not treated as a snapshot");

        auto write = [&backupRoot](const QString &fileName, int bytes) {
            QFile f(QDir(backupRoot.path()).filePath(fileName));
            f.open(QIODevice::WriteOnly);
            f.write(QByteArray(bytes, 'z'));
            f.close();
        };
        write(QStringLiteral("forkmesh-backup-20260728-070000.tar.gz"), 16);
        write(QStringLiteral("forkmesh-backup-20260728-090000.tar.gz"), 32);
        write(QStringLiteral("forkmesh-backup-20260728-080000.tar.gz"), 64);
        write(QStringLiteral("forkmesh-backup-20260728-100000.tar.gz.part"), 8);
        write(QStringLiteral("notes.txt"), 4);

        const QList<forkmesh::BackupSnapshot> listed =
            forkmesh::listBackups(backupRoot.path());
        check(listed.size() == 3 &&
                  listed.at(0).fileName ==
                      QStringLiteral("forkmesh-backup-20260728-090000.tar.gz") &&
                  listed.at(2).fileName ==
                      QStringLiteral("forkmesh-backup-20260728-070000.tar.gz"),
              "snapshots list newest first, ignoring partial and foreign files");
        check(listed.at(0).bytes == 32 &&
                  QFileInfo::exists(listed.at(0).path),
              "each listed snapshot carries its real size and absolute path");

        const QList<forkmesh::BackupSnapshot> pruned =
            forkmesh::backupsToPrune(listed, 2);
        check(pruned.size() == 1 &&
                  pruned.at(0).fileName ==
                      QStringLiteral("forkmesh-backup-20260728-070000.tar.gz"),
              "retention drops the oldest snapshot beyond the keep count");
        check(forkmesh::backupsToPrune(listed, 3).isEmpty() &&
                  forkmesh::backupsToPrune(listed, 0).size() == 2,
              "nothing is pruned under the limit, and keep=0 still spares the "
              "newest snapshot");

        // Credentials never opt a machine into recurring multi-gigabyte disk
        // writes. Every node starts off until its operator explicitly opts in.
        check(!forkmesh::autoBackupDefault(QStringLiteral("cf-token")) &&
                  !forkmesh::autoBackupDefault(QString()) &&
                  !forkmesh::autoBackupDefault(QStringLiteral("   ")),
              "hourly backups require an explicit opt-in on every node");

        check(forkmesh::backupLeavesFreeReserve(forkmesh::kBackupFreeReserveBytes) &&
                  !forkmesh::backupLeavesFreeReserve(forkmesh::kBackupFreeReserveBytes - 1) &&
                  !forkmesh::backupLeavesFreeReserve(0) &&
                  forkmesh::backupLeavesFreeReserve(-1),
              "a backup needs the free-space reserve and runs when free space is unknown");

        const QDateTime now =
            QDateTime::fromString(QStringLiteral("2026-07-28T10:00:00"),
                                  Qt::ISODate);
        check(forkmesh::backupIsDue(QDateTime(), now),
              "the very first backup is always due");
        check(forkmesh::backupIsDue(now.addSecs(-3600), now) &&
                  !forkmesh::backupIsDue(now.addSecs(-3599), now),
              "the next snapshot is due exactly one hour after the last");
        check(!forkmesh::backupIsDue(now.addSecs(3600), now),
              "a snapshot stamped in the future (clock skew) doesn't fire a "
              "burst of backups");

        // The archive must hold the app data + settings file and must never
        // recurse into the mirrors, the browse cache or the backup folder
        // itself; tar only honours --exclude when it precedes the members.
        QTemporaryDir dataRoot;
        const QString appData = QDir(dataRoot.path()).filePath("ForkMesh");
        QDir().mkpath(QDir(appData).filePath("backups"));
        QDir().mkpath(QDir(appData).filePath("mirrors"));
        const QString configFile = QDir(dataRoot.path()).filePath("forkmesh.conf");
        write(QStringLiteral("unused"), 1);
        QFile conf(configFile);
        conf.open(QIODevice::WriteOnly);
        conf.write("[General]\n");
        conf.close();

        const QStringList args = forkmesh::configArchiveTarArgs(
            QStringLiteral("/tmp/out.tar.gz"), appData, configFile,
            {QDir(appData).filePath("mirrors"),
             QDir(appData).filePath("backups"),
             QStringLiteral("/elsewhere/preview")});
        check(args.value(0) == QStringLiteral("-czf") &&
                  args.value(1) == QStringLiteral("/tmp/out.tar.gz"),
              "the archive path is the first thing tar is told to write");
        check(args.contains(QStringLiteral("--exclude=ForkMesh/backups")) &&
                  args.contains(QStringLiteral("--exclude=ForkMesh/mirrors")),
              "nested backups and mirrors are excluded, so a snapshot never "
              "packs the previous snapshots");
        check(!args.contains(QStringLiteral("--exclude=/elsewhere/preview")),
              "a root outside the app-data tree needs no exclusion");
        check(args.indexOf(QStringLiteral("--exclude=ForkMesh/backups")) <
                  args.indexOf(QStringLiteral("-C")),
              "every exclusion precedes the members, as tar requires");
        check(args.contains(QStringLiteral("ForkMesh")) &&
                  args.contains(QStringLiteral("forkmesh.conf")),
              "the app-data dir and the settings file are both archived");
        check(forkmesh::configArchiveTarArgs(
                  QStringLiteral("/tmp/out.tar.gz"),
                  QDir(dataRoot.path()).filePath("missing"),
                  QDir(dataRoot.path()).filePath("missing.conf"), {})
                  .isEmpty(),
              "a fresh install with nothing on disk yet produces no tar run");
    }

    // QFontDatabase logs "OpenType support missing for \"<family>\", script N"
    // once per installed family every time it walks the fallback list for a
    // codepoint the system fonts can't shape, burying the console. The log
    // filter (installed for every launch, not just headless) must swallow
    // exactly those lines and forward everything else.
    {
        QStringList captured;
        capturedMessages = &captured;
        QtMessageHandler previous = qInstallMessageHandler(captureMessages);
        forkmesh::installPlatformLogFilter(); // chains to captureMessages
        qWarning("OpenType support missing for \"DejaVu Sans Mono\", script 9");
        qWarning("OpenType support missing for \"\", script 9");
        qWarning("forkmesh-419-control-line");
        qInstallMessageHandler(previous); // restore so PASS/FAIL output prints
        capturedMessages = nullptr;

        const QString joined = captured.join(QLatin1Char('\n'));
        check(!joined.contains(QStringLiteral("OpenType support missing")),
              "the log filter drops QFontDatabase OpenType fallback warnings");
        check(joined.contains(QStringLiteral("forkmesh-419-control-line")),
              "the log filter still forwards unrelated warnings");
        check(forkmesh::isFontDatabaseNoise(QStringLiteral(
                  "OpenType support missing for \"Noto Mono\", script 9")) &&
                  !forkmesh::isFontDatabaseNoise(
                      QStringLiteral("forkmesh-419-control-line")),
              "isFontDatabaseNoise matches only the font-database warning");
    }

    // The app's progress lines and Qt's own warnings belong in the Log view, not
    // in the terminal the desktop was launched from. The filter's sink is what
    // moves them: it takes the message *instead of* the console, except headless
    // (keepConsoleEcho), where the operator only has the console.
    {
        QStringList captured;
        capturedMessages = &captured;
        QtMessageHandler previous = qInstallMessageHandler(captureMessages);
        forkmesh::installPlatformLogFilter(); // chains to captureMessages

        QList<QPair<QtMsgType, QString>> sunk;
        // The sink also receives the emitting file and line (adhoc #1587); this
        // suite only cares which messages reach it.
        const auto record = [&sunk](QtMsgType type, const QString &message,
                                    const QString &, int) {
            sunk.append({type, message});
        };

        forkmesh::setAppLogSink(record, /*keepConsoleEcho=*/false);
        qWarning("QProcess: Destroyed while process (\"git\") is still running.");
        qInfo("Catalog publish response: jett/forkmesh 201 0");
        // Noise stays noise: the sink must not be handed what the console was
        // already spared.
        qWarning("OpenType support missing for \"Noto Mono\", script 9");

        forkmesh::setAppLogSink(record, /*keepConsoleEcho=*/true);
        qWarning("forkmesh-headless-echo-line");

        // A sink that logs would otherwise re-enter itself forever.
        forkmesh::setAppLogSink(
            [&sunk](QtMsgType type, const QString &message, const QString &,
                    int) {
                sunk.append({type, message});
                if (!message.startsWith(QLatin1String("re-entrant")))
                    qWarning("re-entrant sink line");
            },
            /*keepConsoleEcho=*/false);
        qWarning("forkmesh-reentrant-trigger");

        forkmesh::clearAppLogSink();
        qWarning("forkmesh-after-clear-line");
        qInstallMessageHandler(previous); // restore so PASS/FAIL output prints
        capturedMessages = nullptr;

        const QString console = captured.join(QLatin1Char('\n'));
        QStringList sunkText;
        for (const auto &entry : sunk)
            sunkText << entry.second;
        const QString logged = sunkText.join(QLatin1Char('\n'));

        check(logged.contains(QStringLiteral(
                  "QProcess: Destroyed while process (\"git\") is still "
                  "running.")) &&
                  !console.contains(QStringLiteral("QProcess: Destroyed")),
              "Qt's QProcess teardown warning goes to the log, not the console");
        check(logged.contains(QStringLiteral("Catalog publish response")) &&
                  !console.contains(QStringLiteral("Catalog publish response")),
              "the app's own qInfo progress lines go to the log only");
        check(!logged.contains(QStringLiteral("OpenType support missing")),
              "known platform noise is still dropped before the sink");
        check(sunk.first().first == QtWarningMsg &&
                  sunk.at(1).first == QtInfoMsg,
              "the sink is told each message's severity");
        check(logged.contains(QStringLiteral("forkmesh-headless-echo-line")) &&
                  console.contains(QStringLiteral("forkmesh-headless-echo-line")),
              "keepConsoleEcho (headless) logs and still prints to the console");
        // Qt refuses to re-enter an installed handler at all — the nested line
        // goes straight to stderr, past captureMessages — so the observable
        // guarantee is that it never loops back into the sink.
        check(sunkText.count(QStringLiteral("re-entrant sink line")) == 0,
              "a sink that logs re-entrantly does not feed itself");
        check(!logged.contains(QStringLiteral("forkmesh-after-clear-line")) &&
                  console.contains(QStringLiteral("forkmesh-after-clear-line")),
              "clearAppLogSink sends messages back to the console");
    }

    // MCP connector (adhoc #16): the token is a bearer credential that lets an
    // external agent write signed entries as this node, so minting, masking,
    // persistence permissions and revocation all have to hold.
    {
        using namespace forkmesh::mcp;
        QTemporaryDir appData;
        check(appData.isValid(), "MCP connector test dir created");

        check(!loadConnector(appData.path()).isValid(),
              "a node with no connector file grants no token");

        const QString token = generateToken();
        check(isWellFormedToken(token) && token.startsWith("fmcp_") &&
                  token.size() >= 40,
              "a generated token is well formed and long enough to be a secret");
        check(generateToken() != generateToken(),
              "each generated token is distinct");
        check(!isWellFormedToken(QStringLiteral("fmcp_short")) &&
                  !isWellFormedToken(QStringLiteral("hunter2")) &&
                  !isWellFormedToken(QString()),
              "malformed tokens are rejected");

        Connector minted;
        minted.token = token;
        minted.node = QStringLiteral("nodepub");
        minted.label = QStringLiteral("laptop");
        minted.createdMs = 1700000000000LL;
        QString error;
        check(saveConnector(appData.path(), minted, &error), "connector saved");

        const Connector loaded = loadConnector(appData.path());
        check(loaded.token == token && loaded.node == QStringLiteral("nodepub")
                  && loaded.label == QStringLiteral("laptop")
                  && loaded.createdMs == 1700000000000LL,
              "the saved connector round-trips");
        // Qt reports the same POSIX bits as both the Owner and User flags, so
        // assert on what actually matters: the owner can read/write it and
        // nobody else can see it at all.
        const QFile::Permissions perms =
            QFile::permissions(connectorPath(appData.path()));
        check(perms.testFlag(QFile::ReadOwner) &&
                  perms.testFlag(QFile::WriteOwner) &&
                  !(perms & (QFile::ReadGroup | QFile::WriteGroup |
                             QFile::ReadOther | QFile::WriteOther)),
              "connector.json is owner-only, like the identity key beside it");
        check(connectorPath(appData.path())
                  .endsWith(QStringLiteral("/mcp/connector.json")),
              "the connector lands where forkmesh_mcp_server.py reads it");

        Connector bogus;
        bogus.token = QStringLiteral("not-a-token");
        check(!saveConnector(appData.path(), bogus, &error) &&
                  loadConnector(appData.path()).token == token,
              "a malformed token is refused without clobbering the live one");

        // A hand-edited/truncated file must read as "no connector" rather than
        // as a connector whose token nothing can ever match.
        QFile broken(connectorPath(appData.path()));
        check(broken.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "connector file reopened");
        broken.write("{\"token\":\"\"}");
        broken.close();
        check(!loadConnector(appData.path()).isValid(),
              "a corrupt connector file reads as absent");

        check(saveConnector(appData.path(), minted, &error) &&
                  revokeConnector(appData.path(), &error) &&
                  !loadConnector(appData.path()).isValid(),
              "revoking removes the token");
        check(revokeConnector(appData.path(), &error),
              "revoking an already-revoked connector succeeds");

        const QString config = configJson(QStringLiteral("/usr/bin/python3"),
                                          QStringLiteral("/opt/fm/mcp.py"),
                                          QStringLiteral("/home/u/mirrors"),
                                          token);
        const QJsonObject server =
            QJsonDocument::fromJson(config.toUtf8()).object()
                .value("mcpServers").toObject().value("forkmesh").toObject();
        check(server.value("command").toString() ==
                      QStringLiteral("/usr/bin/python3") &&
                  server.value("args").toArray().at(0).toString() ==
                      QStringLiteral("/opt/fm/mcp.py"),
              "the generated config launches the resolved interpreter and script");
        check(server.value("env").toObject().value("FORKMESH_MCP_TOKEN")
                      .toString() == token &&
                  server.value("env").toObject().value("FORKMESH_REPOS_DIR")
                      .toString() == QStringLiteral("/home/u/mirrors"),
              "the generated config carries the token and repo root");
        check(!configJson(QString(), QStringLiteral("/opt/fm/mcp.py"),
                          QString(), QString())
                   .contains(QStringLiteral("env")),
              "with no token and no repo root the config has no env block");

        const QString cli = cliCommand(QStringLiteral("python3"),
                                       QStringLiteral("/home/a b/mcp.py"),
                                       QString(), token);
        check(cli.startsWith(QStringLiteral("claude mcp add forkmesh")) &&
                  cli.contains(QStringLiteral("'/home/a b/mcp.py'")) &&
                  cli.contains(QStringLiteral("FORKMESH_MCP_TOKEN=") + token),
              "the CLI one-liner quotes paths with spaces and passes the token");

        check(maskToken(token).startsWith(QStringLiteral("fmcp_")) &&
                  !maskToken(token).contains(token.mid(12, 8)) &&
                  maskToken(QString()).isEmpty(),
              "masking hides the middle of the token");
    }

    {
        // Size map over a folder the user cannot fully read (adhoc #76): the
        // mount table has to yield the pseudo filesystems Qt's QStorageInfo
        // hides (or /proc/kcore's fictional terabytes swallow a scan of "/"),
        // unreadable directories have to be counted rather than silently
        // skipped, and the tree has to survive the trip back from the elevated
        // helper process.
        using namespace forkmesh;

        const QByteArray mountInfo =
            "23 28 0:22 / /proc rw,nosuid,relatime shared:12 - proc proc rw\n"
            "24 28 0:23 / /sys rw,nosuid shared:2 - sysfs sysfs rw\n"
            "31 28 0:29 / /media/My\\040Disk rw,relatime shared:5 - ext4 "
            "/dev/sdb1 rw\n";
        const QSet<QString> mounts = mountPointsFromMountTable(mountInfo);
        check(mounts.contains(QStringLiteral("/proc")) &&
                  mounts.contains(QStringLiteral("/sys")) &&
                  mounts.contains(QStringLiteral("/media/My Disk")),
              "mountinfo yields the pseudo mounts QStorageInfo hides, "
              "unescaped");
        const QSet<QString> mtab = mountPointsFromMountTable(
            "proc /proc proc rw,nosuid 0 0\ntmpfs /run tmpfs rw 0 0\n");
        check(mtab.contains(QStringLiteral("/proc")) &&
                  mtab.contains(QStringLiteral("/run")) && mtab.size() == 2,
              "the /proc/mounts layout parses to the same mount points");

        QTemporaryDir tree;
        check(tree.isValid(), "the size-map scan gets a scratch tree");
        const QDir root(tree.path());
        check(root.mkpath(QStringLiteral("keep")) &&
                  root.mkpath(QStringLiteral("pruned")) &&
                  root.mkpath(QStringLiteral("locked")),
              "the scratch tree has readable, pruned and locked folders");
        const auto writeBytes = [&root](const QString &relative, int size) {
            QFile file(root.absoluteFilePath(relative));
            return file.open(QIODevice::WriteOnly) &&
                   file.write(QByteArray(size, 'x')) == size;
        };
        check(writeBytes(QStringLiteral("keep/a.bin"), 4096) &&
                  writeBytes(QStringLiteral("pruned/b.bin"), 8192) &&
                  writeBytes(QStringLiteral("locked/c.bin"), 2048),
              "the scratch tree has one file per folder");
        QFile::setPermissions(root.absoluteFilePath(QStringLiteral("locked")),
                              QFileDevice::WriteOwner);

        DirectorySizeScanOptions options;
        options.pruned.insert(root.absoluteFilePath(QStringLiteral("pruned")));
        const DirectorySizeScanResult scan =
            scanDirectorySizes(tree.path(), options);
        check(scan.root.size == 4096 &&
                  (runningAsRoot() || scan.root.fileCount == 1),
              "pruned paths contribute no bytes to the totals");
        if (!runningAsRoot()) {
            check(scan.unreadableDirs == 1 &&
                      scan.unreadableSample.size() == 1 &&
                      scan.unreadableSample.first().endsWith(
                          QStringLiteral("/locked")),
                  "an unlistable directory is reported, not silently dropped");
            // What decides whether selecting a folder asks for the root password
            // before scanning it at all (adhoc #112).
            check(scanNeedsElevation(tree.path(), {}),
                  "a folder holding an unlistable directory wants root up front");
            check(!scanNeedsElevation(
                      tree.path(),
                      {root.absoluteFilePath(QStringLiteral("locked"))}),
                  "a folder whose only locked child is pruned does not");
            check(!scanNeedsElevation(
                      root.absoluteFilePath(QStringLiteral("keep")), {}),
                  "a fully readable folder never raises a password prompt");
        } else {
            check(!scanNeedsElevation(tree.path(), {}),
                  "already running as root, there is nothing left to ask for");
        }
        // Restore, or QTemporaryDir cannot clean up after itself.
        QFile::setPermissions(root.absoluteFilePath(QStringLiteral("locked")),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                  QFileDevice::ExeOwner);

        QString requestPath;
        DirectorySizeScanOptions decodedOptions;
        check(decodeScanRequest(encodeScanRequest(tree.path(), options),
                                &requestPath, &decodedOptions) &&
                  requestPath == tree.path() &&
                  decodedOptions.pruned == options.pruned &&
                  decodedOptions.maxDepth == options.maxDepth,
              "the elevated helper's request survives the round trip");
        check(!decodeScanRequest(QByteArray("{}"), &requestPath, &decodedOptions),
              "a request without a folder is rejected");

        DirectorySizeScanResult decoded;
        check(decodeScanResult(encodeScanResult(scan), &decoded) &&
                  decoded.root.size == scan.root.size &&
                  decoded.root.children.size() == scan.root.children.size() &&
                  decoded.unreadableDirs == scan.unreadableDirs &&
                  decoded.unreadableSample == scan.unreadableSample,
              "the elevated helper's tree survives the round trip");
        check(!decodeScanResult(QByteArray("not a scan"), &decoded) &&
                  !decodeScanResult(QByteArray(), &decoded),
              "a truncated or foreign payload is never read as a tree");

        // Live progress (adhoc #112): the walk names the folder it is inside so
        // the tab can show it, and the same updates survive the trip out of the
        // elevated helper on stderr.
        QStringList visited;
        qint64 lastBytes = -1;
        int firstWorker = -1;
        scanDirectorySizes(tree.path(), options,
                           [&](const DirectorySizeScanProgressUpdate &update) {
                               // Serialized by the scan, so no lock is needed
                               // here however many threads are walking.
                               if (visited.isEmpty())
                                   firstWorker = update.worker;
                               visited.append(update.path);
                               lastBytes = update.bytes;
                           });
        check(visited.contains(QDir::cleanPath(tree.path())) && lastBytes >= 0,
              "a scan reports the folder it is walking, root first");
        check(firstWorker == 0,
              "worker 0 is the thread that asked for the scan");

        // Per-thread lines (adhoc #95): a wide tree is what the walk actually
        // spreads across the pool, and every update it produces has to name the
        // thread behind it. The slot a thread holds must be stable — a thread
        // that finishes one top-level tree and picks up the next keeps its line
        // rather than opening a second one — and no slot may be shared.
        {
            const QDir wide(tree.path());
            bool built = true;
            for (int i = 0; i < 24; ++i) {
                const QString branch = QStringLiteral("wide/b%1").arg(i);
                built = built && wide.mkpath(branch) &&
                        writeBytes(branch + QStringLiteral("/f.bin"), 512);
            }
            check(built, "the wide tree has a file in each of its 24 branches");
            QHash<int, quintptr> threadOfSlot;
            QHash<quintptr, int> slotOfThread;
            QSet<int> wentIdle;
            bool stable = true;
            bool inRange = true;
            int slotCount = 0; // not "slots": Qt's own keyword macro
            scanDirectorySizes(
                wide.absoluteFilePath(QStringLiteral("wide")), {},
                [&](const DirectorySizeScanProgressUpdate &update) {
                    // The scan serializes its callback, so these plain maps need
                    // no lock however many threads are walking.
                    const auto thread =
                        reinterpret_cast<quintptr>(QThread::currentThread());
                    stable = stable &&
                             threadOfSlot.value(update.worker, thread) == thread &&
                             slotOfThread.value(thread, update.worker) ==
                                 update.worker;
                    threadOfSlot.insert(update.worker, thread);
                    slotOfThread.insert(thread, update.worker);
                    inRange = inRange && update.worker >= 0 &&
                              update.worker < update.workers;
                    if (update.idle)
                        wentIdle.insert(update.worker);
                    slotCount = qMax(slotCount, update.workers);
                });
            check(stable,
                  "one thread holds one worker slot for a whole scan, and no "
                  "slot is shared");
            check(inRange && slotCount >= threadOfSlot.size() &&
                      !threadOfSlot.isEmpty(),
                  "every update names a slot inside the worker count it reports");
            // Without this a thread that finished its branch early would leave
            // its line frozen on a folder it left, reading as a stalled scan.
            check(wentIdle.size() == threadOfSlot.size(),
                  "every worker reports going idle when its tree is counted");
        }

        DirectorySizeScanProgressUpdate progress;
        // A newline in a filename would otherwise split one update into two, and
        // a space would be read as another counter.
        DirectorySizeScanProgressUpdate sent;
        sent.path = tree.path() + QStringLiteral("/od d\nname 100%");
        sent.bytes = 4096;
        sent.files = 7;
        sent.worker = 3;
        sent.workers = 5;
        sent.workerBytes = 1024;
        sent.workerFiles = 2;
        sent.idle = true;
        check(decodeScanProgress(encodeScanProgress(sent), &progress) &&
                  progress.path == sent.path && progress.bytes == 4096 &&
                  progress.files == 7 && progress.worker == 3 &&
                  progress.workers == 5 && progress.workerBytes == 1024 &&
                  progress.workerFiles == 2 && progress.idle,
              "a progress line round-trips its worker and a path with a newline");
        check(encodeScanProgress(sent).count('\n') == 1,
              "one progress update is exactly one line");
        check(!decodeScanProgress(QByteArray("sudo: a password is required"),
                                  &progress) &&
                  !decodeScanProgress(QByteArray("FMSZ-PROGRESS 12"), &progress),
              "sudo's own chatter and a truncated line are not progress");
        // An older helper left on disk by a half-applied update still sends the
        // two-counter line; it reads as a single worker rather than vanishing.
        check(decodeScanProgress(QByteArray("FMSZ-PROGRESS 4096 7 /tmp/x"),
                                 &progress) &&
                  progress.worker == 0 && progress.workers == 1 &&
                  progress.workerBytes == 4096 && progress.workerFiles == 7 &&
                  !progress.idle && progress.path == QStringLiteral("/tmp/x"),
              "a pre-worker progress line still reads as one thread's progress");

        // Stop button: a canceled poll must unwind before the walk descends
        // into anything, rather than finishing the tree and throwing it away.
        int cancelChecks = 0;
        const DirectorySizeScanResult stopped = scanDirectorySizes(
            tree.path(), options, {},
            [&] { ++cancelChecks; return true; });
        check(stopped.root.size == 0 && stopped.root.fileCount == 0 &&
                  stopped.root.children.isEmpty() && cancelChecks > 0,
              "a scan canceled up front produces an empty tree, not a partial one");
    }

#if defined(Q_OS_LINUX)
    {
        QProcess victim;
        victim.start(QStringLiteral("sleep"), {QStringLiteral("30")});
        check(victim.waitForStarted(5000), "a process to end is running");
        const qint64 victimPid = victim.processId();
        QString failure;
        check(SystemStats::requestTermination(victimPid, &failure)
                  == SystemStats::Termination::Requested,
              "ending a running process is requested");
        check(victim.waitForFinished(5000), "the ended process exits");
        failure.clear();
        check(SystemStats::requestTermination(victimPid, &failure)
                      == SystemStats::Termination::AlreadyExited
                  && failure.isEmpty(),
              "a process that already exited is the outcome asked for, not a "
              "failure (Could not kill process: No such process)");
    }
    {
        QTemporaryDir sandbox;
        const QString repo = sandbox.path() + QStringLiteral("/repo");
        QDir().mkpath(repo);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), sandbox.path());
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), sandbox.path() + QStringLiteral("/config"));
        env.insert(QStringLiteral("XDG_DATA_HOME"), sandbox.path() + QStringLiteral("/data"));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), sandbox.path() + QStringLiteral("/cache"));
        env.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
        env.remove(QStringLiteral("FORKMESH_HOME"));
        env.remove(QStringLiteral("FORKMESH_PROJECTS_DIR"));
        const QStringList identity{QStringLiteral("-c"), QStringLiteral("user.name=t"),
                                   QStringLiteral("-c"), QStringLiteral("user.email=t@example.com")};
        const auto git = [&](const QStringList &args) {
            QProcess process;
            process.setProcessEnvironment(env);
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), repo} + identity + args);
            return process.waitForFinished(15000) && process.exitCode() == 0;
        };
        const auto writeTracked = [&](const QByteArray &text) {
            QFile file(repo + QStringLiteral("/tracked.txt"));
            return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
        };
        const bool ready = git({QStringLiteral("init"), QStringLiteral("-q")}) &&
                           writeTracked("one\n") &&
                           git({QStringLiteral("add"), QStringLiteral("tracked.txt")}) &&
                           git({QStringLiteral("commit"), QStringLiteral("-q"),
                                QStringLiteral("-m"), QStringLiteral("one")}) &&
                           writeTracked("two\n");
        check(ready, "a repository with an uncommitted edit exists");
        const QString lock = repo + QStringLiteral("/.git/index.lock");
        const auto holdLock = [&](QProcess &process) {
            QProcessEnvironment editing = env;
            editing.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral("sleep 8 #"));
            process.setProcessEnvironment(editing);
            process.start(QStringLiteral("git"),
                          QStringList{QStringLiteral("-C"), repo} + identity +
                              QStringList{QStringLiteral("commit"), QStringLiteral("-a")});
            QElapsedTimer waited;
            waited.start();
            while (!QFile::exists(lock) && waited.elapsed() < 5000)
                QThread::msleep(20);
            return QFile::exists(lock);
        };
        QProcess graceful;
        check(ready && holdLock(graceful),
              "a git commit waiting on its editor holds index.lock");
        forkmesh::stopProcessGracefully(graceful);
        check(graceful.state() == QProcess::NotRunning && !QFile::exists(lock),
              "stopping a git that overran its deadline lets it remove "
              "index.lock (fatal: Unable to write index)");
        QProcess killed;
        check(ready && holdLock(killed), "a second commit holds index.lock again");
        killed.kill();
        killed.waitForFinished(2000);
        check(QFile::exists(lock),
              "SIGKILL strands index.lock, which is why a timed-out git is "
              "sent SIGTERM first");
        QFile::remove(lock);
    }
    {
        // Counting an agent's own compilers (adhoc #57): the /proc walk has to
        // find a grandchild by command name and ignore everything outside the
        // tree it was asked about.
        using SystemStats::descendantsNamed;
        using SystemStats::descendantProcesses;
        check(descendantProcesses(0).isEmpty() &&
                  descendantsNamed(0, QStringLiteral("sleep")).count == 0 &&
                  descendantsNamed(QCoreApplication::applicationPid(), QString())
                          .count == 0,
              "an invalid root PID or empty name reports nothing");

        QProcess child;
        // `sh` execs the sleep, so the match is a grandchild of this process —
        // the same shape as claude → bash → cc1plus.
        child.start(QStringLiteral("/bin/sh"),
                    {QStringLiteral("-c"), QStringLiteral("sleep 30")});
        if (child.waitForStarted(5000)) {
            SystemStats::DescendantLoad load;
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < 5000 && load.count == 0) {
                load = descendantsNamed(QCoreApplication::applicationPid(),
                                        QStringLiteral("sleep"));
                if (load.count == 0)
                    QThread::msleep(50); // sh hasn't exec'd the sleep yet
            }
            check(load.count >= 1 && load.residentBytes > 0,
                  "a descendant process is counted with its resident memory");
            bool foundSleep = false;
            for (const SystemStats::DescendantProcess &process :
                 descendantProcesses(QCoreApplication::applicationPid())) {
                // /bin/sh may exec sleep or fork it before waiting, so the
                // observed child's PID is not guaranteed to be QProcess's PID.
                if (process.pid > 0 && process.parentPid > 0 &&
                    process.command == QLatin1String("sleep")) {
                    foundSleep = true;
                    break;
                }
            }
            check(foundSleep,
                  "a descendant process snapshot identifies the child PID and command");
            check(descendantsNamed(child.processId(),
                                   QStringLiteral("forkmesh-tests"))
                          .count == 0,
                  "processes outside the subtree are not counted");
            child.kill();
            child.waitForFinished(5000);
        }
    }
#endif


    {
        // Node self-diagnostics (adhoc #27): the checks a node runs on itself
        // and pushes to every node list. The gauges already show CPU/RAM/disk;
        // these rules exist for what a point sample cannot show, so each one is
        // pinned here against the shape of input that must (and must not) trip it.
        using namespace NodeDiagnostics;
        const qint64 hour = 60 * 60 * 1000;
        const qint64 gb = 1024LL * 1024 * 1024;
        auto findingFor = [](const QList<Finding> &findings, const char *id) {
            for (const Finding &f : findings) {
                if (f.id == QLatin1String(id))
                    return f;
            }
            return Finding{};
        };
        auto has = [&](const QList<Finding> &findings, const char *id) {
            return !findingFor(findings, id).id.isEmpty();
        };
        auto diskInputs = [&](qint64 spanMs, qint64 startFree, qint64 endFree) {
            Inputs in;
            in.nowMs = 1000 * hour;
            in.windowMs = kWindowMs;
            in.diskHistory = {DiskSample{in.nowMs - spanMs, startFree, 100 * gb},
                              DiskSample{in.nowMs, endFree, 100 * gb}};
            return in;
        };

        // A volume losing 1 GB/hour with 10 GB left is hours from wedging, and
        // its disk bar still reads a comfortable 90%.
        const Finding trend =
            findingFor(evaluate(diskInputs(4 * hour, 14 * gb, 10 * gb)),
                       "disk-trend");
        check(trend.severity == Critical && trend.message.contains("Disk filling") &&
                  trend.message.contains("hours"),
              "a fast-filling disk is critical with a time-to-full horizon");
        check(findingFor(evaluate(diskInputs(24 * hour, 100 * gb, 90 * gb)),
                         "disk-trend")
                      .severity == Warning,
              "10 GB/day against 90 GB free is a warning, not a crisis");
        check(findingFor(evaluate(diskInputs(24 * hour, 92 * gb, 90 * gb)),
                         "disk-trend")
                      .severity == Info,
              "a month and a half of headroom is a note");
        check(!has(evaluate(diskInputs(24 * hour, 91 * gb, 90 * gb)), "disk-trend"),
              "three months out is not worth telling the fleet about");
        check(!has(evaluate(diskInputs(2 * 60 * 1000, 14 * gb, 10 * gb)),
                   "disk-trend"),
              "a trend measured over two minutes is noise, not a trend");
        check(!has(evaluate(diskInputs(4 * hour, 10 * gb, 14 * gb)), "disk-trend"),
              "a disk that is freeing space raises nothing");
        check(!has(evaluate(diskInputs(4 * hour, 10 * gb, 10 * gb - 1024)),
                   "disk-trend"),
              "a kilobyte of churn an hour is not a leak");

        Inputs base;
        base.nowMs = 1000 * hour;
        base.windowMs = kWindowMs;

        // Inodes and file descriptors: both fail writes/connections while every
        // gauge on the row still looks fine.
        Inputs inodes = base;
        inodes.inodesTotal = 1000000;
        inodes.inodesFree = 100000; // 90% used
        check(findingFor(evaluate(inodes), "disk-inodes").severity == Warning,
              "90% of inodes used warns before the disk does");
        inodes.inodesFree = 20000; // 98% used
        check(findingFor(evaluate(inodes), "disk-inodes").severity == Critical,
              "a nearly full inode table is critical");
        check(!has(evaluate(base), "disk-inodes"),
              "a host that cannot report inodes reports nothing about them");

        Inputs fds = base;
        fds.openFileDescriptors = 800;
        fds.fileDescriptorLimit = 1024;
        check(findingFor(evaluate(fds), "file-descriptors").severity == Warning,
              "descriptors at 78% of the limit warn");
        fds.openFileDescriptors = 1000;
        check(findingFor(evaluate(fds), "file-descriptors").severity == Critical,
              "descriptors about to hit the limit are critical");
        fds.openFileDescriptors = -1;
        check(!has(evaluate(fds), "file-descriptors"),
              "an unknown descriptor count raises nothing");

        Inputs readonly = base;
        readonly.dataDirWritable = false;
        check(findingFor(evaluate(readonly), "data-dir").severity == Critical,
              "a read-only data directory is critical");

        Inputs zombies = base;
        zombies.zombieProcesses = 30;
        check(findingFor(evaluate(zombies), "zombie-processes").severity == Warning,
              "a pile of defunct children warns");
        zombies.zombieProcesses = 4;
        check(!has(evaluate(zombies), "zombie-processes"),
              "a handful of defunct children is a race, not a leak");

        // Log errors: the whole point of pushing them is that nobody reads a
        // headless node's terminal, so the newest line travels with the count.
        Inputs logs = base;
        logs.logErrors = 12;
        logs.logErrorSample = QStringLiteral("mirror push failed: exit 128");
        const Finding logFinding = findingFor(evaluate(logs), "log-errors");
        check(logFinding.severity == Warning &&
                  logFinding.message.contains("12 errors") &&
                  logFinding.message.contains("exit 128"),
              "a run of log errors warns and carries the newest line");
        logs.logErrors = 2;
        check(findingFor(evaluate(logs), "log-errors").severity == Info,
              "a couple of log errors is a note, not a warning");
        check(!has(evaluate(base), "log-errors"),
              "a quiet log raises nothing");

        Inputs link = base;
        link.relayDrops = 6;
        link.backpressureDrops = 40;
        check(findingFor(evaluate(link), "relay-flap").severity == Warning &&
                  findingFor(evaluate(link), "relay-backpressure").severity ==
                      Warning,
              "a flapping relay link and dropped frames are both reported");

        Inputs skew = base;
        skew.clockSkewKnown = true;
        skew.clockSkewMs = 6 * 60 * 1000;
        check(findingFor(evaluate(skew), "clock-skew").severity == Critical &&
                  findingFor(evaluate(skew), "clock-skew").message.contains("ahead of"),
              "a badly drifted clock is critical and says which way it drifted");
        skew.clockSkewMs = -3 * 60 * 1000;
        check(findingFor(evaluate(skew), "clock-skew").message.contains("behind"),
              "a clock running slow is described as behind");
        skew.clockSkewMs = 20 * 1000;
        check(!has(evaluate(skew), "clock-skew"),
              "twenty seconds of drift is not worth a row");

        // Ordering and the node-list column text.
        Inputs many = base;
        many.dataDirWritable = false;
        many.relayDrops = 6;
        many.logErrors = 1;
        const QList<Finding> ranked = evaluate(many);
        check(ranked.size() == 3 && ranked.first().severity == Critical &&
                  ranked.last().severity == Info,
              "findings come back worst first");
        check(summaryLabel(ranked, true) ==
                  QString::fromUtf8("1 critical \xC2\xB7 1 warning"),
              "the column counts criticals and warnings, notes only when alone");
        check(summaryLabel({}, true) == QStringLiteral("OK"),
              "a node that checked and found nothing reads OK");
        check(summaryLabel({}, false) == QString::fromUtf8("\xE2\x80\x94"),
              "a node that never reported is unknown, not healthy");
        check(worstSeverity({}) == Ok && worstSeverity(ranked) == Critical,
              "the worst severity drives the row's colour");
        const QString verboseDetail = detailText({findingFor(evaluate(link),
                                                            "relay-flap")});
        check(verboseDetail.contains(QStringLiteral("Warning [relay-flap]")) &&
                  verboseDetail.contains(QStringLiteral("Correlate disconnect times")) &&
                  verboseDetail.contains(QStringLiteral("verify heartbeats stay connected")),
              "diagnostic detail expands a compact finding into actionable checks");

        // Wire form: bounded in both directions, so neither our heartbeat nor a
        // hostile peer's can inflate a frame or a tooltip.
        QList<Finding> wide;
        for (int i = 0; i < 12; ++i)
            wide.append(Finding{QStringLiteral("check-%1").arg(i), Warning,
                                QString(400, QLatin1Char('x'))});
        const QJsonArray encoded = toJson(wide);
        check(encoded.size() == kMaxWireFindings &&
                  encoded.first().toObject().value("m").toString().size() ==
                      kMaxMessageChars,
              "the wire form caps how many findings and how long each one is");
        check(fromJson(encoded).size() == kMaxWireFindings,
              "a peer cannot push more findings than the wire allows");
        const QList<Finding> hostile = fromJson(QJsonArray{
            QJsonObject{{"i", QStringLiteral("evil")},
                        {"s", 99},
                        {"m", QString(400, QLatin1Char('y'))}},
            QJsonObject{{"i", QStringLiteral("low")}, {"s", -5}, {"m", QStringLiteral("hm")}},
            QJsonObject{{"i", QString()}, {"s", 2}, {"m", QString()}}});
        check(hostile.size() == 2 && hostile.first().severity == Critical &&
                  hostile.first().message.size() == kMaxMessageChars &&
                  hostile.last().severity == Ok,
              "a peer's severities are clamped, its text truncated and its "
              "empty rows dropped");
        const QList<Finding> roundTripped = fromJson(toJson(ranked));
        check(roundTripped.size() == ranked.size() &&
                  roundTripped.first().id == ranked.first().id &&
                  roundTripped.first().severity == Critical,
              "a decoded set keeps its worst-first order");

        // The collector: what it counts, and what it refuses to count.
        QTemporaryDir logDir;
        check(logDir.isValid(), "the diagnostics test has a temporary log dir");
        const QString logPath = logDir.filePath(QStringLiteral("node.log"));
        {
            QFile seed(logPath);
            check(seed.open(QIODevice::WriteOnly),
                  "the test log file can be created");
            seed.write("2026-01-01 00:00:00  older error nobody should re-report\n");
            seed.close();
        }
        Collector collector;
        collector.setLogPaths({logPath});
        const qint64 t0 = base.nowMs;
        collector.run(t0); // first pass baselines the existing log
        check(!has(collector.findings(), "log-errors"),
              "a log file's existing history is not replayed as fresh errors");
        {
            QFile append(logPath);
            check(append.open(QIODevice::WriteOnly | QIODevice::Append),
                  "the test log file can be appended to");
            append.write("2026-01-01 00:01:00  mirror sync failed: exit 128\n");
            append.write("2026-01-01 00:01:01  everything is fine here\n");
            append.write("2026-01-01 00:01:02    UI stalled ~900 ms\n");
            append.write(
                "2026-01-01 00:01:03  Recovered; last failure was timeout  "
                "[desktop/src/MainWindowActions.cpp:12|INFO]\n");
            append.write(
                "2026-01-01 00:01:04  Installer needs attention  "
                "[desktop/src/MainWindowActions.cpp:13|ERROR]\n");
            append.close();
        }
        const QList<Finding> scanned = collector.run(t0 + Collector::kRunIntervalMs);
        check(findingFor(scanned, "log-errors").message.contains("2 errors"),
              "typed durable log severity overrides prose while ordinary "
              "failures still count");
        check(!has(scanned, "ui-stalls"),
              "one stall report is below the threshold that would report it");

        Collector counters;
        counters.noteLogLine(QStringLiteral("push failed: remote hung up"), t0);
        counters.noteLogLine(QStringLiteral("published 3 repositories"), t0);
        counters.noteLogLine(QStringLiteral(
            "Recovered; last failure was timeout  "
            "[desktop/src/MainWindowActions.cpp:12|INFO]"), t0);
        counters.noteLogLine(QStringLiteral(
            "Installer needs attention  "
            "[desktop/src/MainWindowActions.cpp:13|ERROR]"), t0);
        counters.noteLogLine(
            QStringLiteral("Self-diagnostics: 1 error in the log"), t0);
        const QList<Finding> fromApp = counters.run(t0);
        check(findingFor(fromApp, "log-errors").message.contains("2 errors"),
              "the app log honors typed severity and excludes its own summary");
        // A restart resets the node's counters; that must not read as a burst.
        counters.noteBackpressureDrops(500);
        check(!has(counters.run(t0 + Collector::kRunIntervalMs),
                   "relay-backpressure"),
              "the first backpressure reading is a baseline, not 500 drops");
        counters.noteBackpressureDrops(560);
        check(findingFor(counters.run(t0 + 2 * Collector::kRunIntervalMs),
                         "relay-backpressure")
                  .message.contains("60"),
              "only frames dropped since the baseline are reported");

        Collector throttled;
        const qint64 gap = Collector::kRelayDropCoalesceMs;
        throttled.noteRelayDrop(t0 - 4 * gap);
        throttled.noteRelayDrop(t0 - 3 * gap);
        check(findingFor(throttled.run(t0), "relay-flap").severity == Info,
              "two link drops in the window are a note");
        throttled.noteRelayDrop(t0 - 2 * gap);
        throttled.noteRelayDrop(t0 - gap);
        throttled.noteRelayDrop(t0);
        check(findingFor(throttled.run(t0 + 1000), "relay-flap").severity == Info,
              "a re-run inside the interval hands back the cached findings");
        check(findingFor(throttled.run(t0 + Collector::kRunIntervalMs),
                         "relay-flap")
                      .severity == Warning,
              "once the interval passes the extra link drops are evaluated");

        Collector shared;
        for (int i = 0; i < 12; ++i)
            shared.noteRelayDrop(t0 + i * 500);
        check(!has(shared.run(t0 + 6000), "relay-flap"),
              "sockets that fall together on one relay restart are one drop");
        shared.noteRelayDrop(t0 + 6000 + Collector::kRelayDropCoalesceMs);
        check(findingFor(shared.run(t0 + 6000 + Collector::kRunIntervalMs),
                         "relay-flap")
                      .message.contains("2 times"),
              "a later separate drop still counts");
    }

    {
        using forkmesh::PlannedRestarts;
        const qint64 t0 = 1'700'000'000'000LL;
        const QString refused = QStringLiteral(
            "Node n3 core caddy err: error http.log.error.log0: dial tcp "
            "127.0.0.1:8080: connect: connection refused");
        const QString gateway = QStringLiteral(
            "Node n3 core caddy err: error http.log.error.log2: dial tcp "
            "127.0.0.1:8790: connect: connection refused");
        const QString local502 = QString::fromUtf8(
            "Node n3 core caddy err: GET app.forkmesh.com/api/nodes/events -> "
            "502 \xC2\xB7 127.0.0.1 (local) \xC2\xB7 ForkMesh-Mirror-Node");
        const QString local500 = QString::fromUtf8(
            "Node n3 core caddy err: GET app.forkmesh.com/api/nodes/events -> "
            "500 \xC2\xB7 127.0.0.1 (local) \xC2\xB7 ForkMesh-Mirror-Node");
        const QString peer503 = QString::fromUtf8(
            "Node n3 core caddy err: GET n3.next.forkmesh.com/forkmesh/"
            "forkmesh.git/info/refs -> 503 \xC2\xB7 203.0.113.21 (r1) \xC2\xB7 "
            "git/2.47.3");
        const QString visitor500 = QString::fromUtf8(
            "Node n3 core caddy err: GET app.forkmesh.com/api/x -> 500 \xC2\xB7 "
            "98.0.1.7 (Charter Communications Inc) \xC2\xB7 Mozilla/5.0");
        const QString preflight502 = QString::fromUtf8(
            "Node n3 core caddy err: OPTIONS app.forkmesh.com/api/world/ticket "
            "-> 502 \xC2\xB7 98.0.1.7 (Charter Communications Inc) \xC2\xB7 "
            "Mozilla/5.0");
        const QString upgrade = QStringLiteral(
            "Node n3 core forkmesh-mirror-node err: node event channel: upgrade "
            "returned HTTP 500: Internal Server Error");
        const QString sealed = QStringLiteral(
            "Node n3 core forkmesh-app err: vault: sealed; unseal socket "
            "/run/forkmesh-next/unseal.sock; asking peers n1,n2,n3");
        const QString idsAlert = QStringLiteral(
            "Node n3 core fmnext-ids err: alert listener_unexpected: dial tcp "
            "127.0.0.1:8080: connect: connection refused");
        const QString appTraceback = QStringLiteral(
            "Node n3 core forkmesh-app err: Traceback: KeyError: 'x' at "
            "entry.py:10 in f");
        const QString otherNode = QStringLiteral(
            "Node n1 core caddy err: error http.log.error.log0: dial tcp "
            "127.0.0.1:8080: connect: connection refused");
        const QStringList noise = {refused, gateway,      local502, local500,
                                   peer503, preflight502, upgrade,  sealed};

        for (const QString &line : noise)
            check(PlannedRestarts::isRestartNoise(line),
                  "a restart symptom is recognised as restart noise");
        check(!PlannedRestarts::isRestartNoise(visitor500),
              "a 500 answered to an outside visitor is not restart noise");
        check(!PlannedRestarts::isRestartNoise(idsAlert),
              "an IDS line is never restart noise");
        check(!PlannedRestarts::isRestartNoise(appTraceback),
              "an app traceback is not restart noise");
        check(!PlannedRestarts::isRestartNoise(
                  QStringLiteral("Node n3 core forkmesh-app err: vault: "
                                 "unsealed (21 secrets)")),
              "an unsealed vault is not the sealed symptom");

        PlannedRestarts idle;
        check(!idle.quiets(refused, t0),
              "restart noise without a planned restart still pings");

        PlannedRestarts byIds;
        const PlannedRestarts::Change fromIds = byIds.observe(
            QStringLiteral("Node n3 core fmnext-ids info: expectation accepted "
                           "allow=listeners files=1 reason=next_deploy "
                           "ec88ec69c76e rescan=/etc/forkmesh-next/caddy"),
            t0);
        check(fromIds.type == PlannedRestarts::Change::Opened &&
                  fromIds.node == QStringLiteral("n3") &&
                  fromIds.kind == PlannedRestarts::Kind::Deploy &&
                  byIds.isOpen(QStringLiteral("n3")),
              "the node's own expectation accepted line opens a deploy window");
        PlannedRestarts byIdsRoll;
        check(byIdsRoll.observe(
                  QStringLiteral("Node n2 core fmnext-ids info: expectation "
                                 "accepted allow=listeners, processes files=0 "
                                 "reason=next_fleet start rescan=/etc/systemd"),
                  t0)
                      .kind == PlannedRestarts::Kind::Roll &&
                  byIdsRoll.isOpen(QStringLiteral("n2")),
              "a next_fleet expectation opens a roll window");

        PlannedRestarts byDeploy;
        const PlannedRestarts::Change fromDeploy = byDeploy.observe(
            QStringLiteral("Deploy next_fleet roll: [execute] n2: tell the IDS "
                           "to expect next_fleet daemon (rescan /etc/forkmesh; "
                           "allow listeners, signed, 5 min)"),
            t0);
        check(fromDeploy.type == PlannedRestarts::Change::Opened &&
                  fromDeploy.node == QStringLiteral("n2") &&
                  fromDeploy.kind == PlannedRestarts::Kind::Roll &&
                  byDeploy.nextDeadlineMs() == t0 + 5 * 60 * 1000,
              "the desktop's tell-the-IDS execute line opens a roll window for "
              "its stated duration");
        check(byDeploy.observe(
                  QStringLiteral("Deploy agent: [execute] n1: tell the IDS to "
                                 "expect next_deploy abc (1 exact files; "
                                 "signed, 90 min)"),
                  t0)
                      .type == PlannedRestarts::Change::Opened &&
                  byDeploy.takeExpired(t0 + PlannedRestarts::kMaxWindowMs)
                          .size() == 2,
              "a stated duration is capped at 15 minutes");

        PlannedRestarts housekeeping;
        check(housekeeping.observe(
                  QStringLiteral("Deploy next_fleet delete-refs: [execute] n1: "
                                 "tell the IDS to expect next_fleet delete-refs "
                                 "(allow processes, signed, 5 min)"),
                  t0)
                      .type == PlannedRestarts::Change::None &&
                  housekeeping.observe(
                      QStringLiteral("Node n1 core fmnext-ids info: expectation "
                                     "accepted allow=processes files=0 "
                                     "reason=next_fleet delete-refs"),
                      t0)
                          .type == PlannedRestarts::Change::None,
              "an expectation that restarts nothing opens no window");
        check(housekeeping.observe(
                  QStringLiteral("Node n1 core fmnext-ids info: expectation "
                                 "accepted reason=next_deploy abc (logged "
                                 "2026-09-27 13:12:44, replayed)"),
                  t0)
                      .type == PlannedRestarts::Change::None,
              "a replayed expectation from the backlog opens no window");

        PlannedRestarts windows;
        windows.observe(QStringLiteral("Deploy agent: [execute] n3: tell the "
                                       "IDS to expect next_deploy abc (1 exact "
                                       "files; signed, 15 min)"),
                        t0);
        for (const QString &line : noise) {
            QString quietNode;
            PlannedRestarts::Kind quietKind = PlannedRestarts::Kind::Roll;
            check(windows.quiets(line, t0 + 1000, &quietNode, &quietKind) &&
                      quietNode == QStringLiteral("n3") &&
                      quietKind == PlannedRestarts::Kind::Deploy,
                  "restart noise from the restarting node is quiet");
        }
        check(!windows.quiets(idsAlert, t0 + 1000),
              "an IDS alert from the restarting node still pings");
        check(!windows.quiets(appTraceback, t0 + 1000) &&
                  !windows.quiets(visitor500, t0 + 1000),
              "other errors from the restarting node still ping");
        check(!windows.quiets(otherNode, t0 + 1000),
              "the same symptom on another node still pings");
        check(windows.claimAnnouncement(QStringLiteral("n3")) &&
                  !windows.claimAnnouncement(QStringLiteral("n3")),
              "one info ping per window");
        check(PlannedRestarts::announcementTitle(QStringLiteral("n3"),
                                                 PlannedRestarts::Kind::Roll) ==
                  QString::fromUtf8("n3 restarting for roll \xE2\x80\x94 "
                                    "errors during the restart are in the Log"),
              "the info ping names the node and the kind of restart");
        const PlannedRestarts::Change closed = windows.observe(
            QStringLiteral("Deploy agent: n3: current abc, running abc, app "
                           "active"),
            t0 + 8000);
        check(closed.type == PlannedRestarts::Change::Closed &&
                  !windows.isOpen(QStringLiteral("n3")) &&
                  !windows.quiets(refused, t0 + 9000),
              "app active closes a deploy window and later noise pings again");

        PlannedRestarts roll;
        roll.observe(QStringLiteral("Deploy next_fleet roll: [execute] n1: "
                                    "tell the IDS to expect next_fleet start "
                                    "(rescan /x; allow listeners, signed, 15 "
                                    "min)"),
                     t0);
        check(roll.observe(QStringLiteral("Deploy agent: [execute] n1: tell the "
                                          "IDS to expect next_deploy abc "
                                          "(signed, 15 min)"),
                           t0 + 30000)
                      .type == PlannedRestarts::Change::Extended &&
                  roll.observe(QStringLiteral("Deploy agent: n1: current abc, "
                                              "running abc, app active"),
                               t0 + 40000)
                          .type == PlannedRestarts::Change::None &&
                  roll.isOpen(QStringLiteral("n1")),
              "a deploy landing mid-roll does not end the roll window");
        check(roll.observe(QStringLiteral("Deploy next_fleet roll: n1: rolled "
                                          "and back in DNS in 180 s"),
                           t0 + 180000)
                      .type == PlannedRestarts::Change::Closed,
              "rolled and back in DNS closes a roll window");
        roll.observe(QStringLiteral("Deploy next_fleet roll: [execute] n2: "
                                    "tell the IDS to expect next_fleet files "
                                    "(signed, 15 min)"),
                     t0);
        check(roll.observe(QStringLiteral("Node n2 core app info: n2: rolled and "
                                          "back in DNS"),
                           t0)
                      .type == PlannedRestarts::Change::None &&
                  roll.isOpen(QStringLiteral("n2")),
              "a node cannot close its window from its own journal");
        check(roll.observe(QStringLiteral("Deploy next_dns undrain: n2 passed 3 "
                                          "health probes and is back in DNS"),
                           t0 + 60000)
                      .type == PlannedRestarts::Change::Closed,
              "the health loop's back in DNS closes a window");

        PlannedRestarts stuck;
        stuck.observe(QStringLiteral("Deploy next_fleet roll: [execute] n3: "
                                     "tell the IDS to expect next_fleet start "
                                     "(signed, 15 min)"),
                      t0);
        check(stuck.takeExpired(t0 + PlannedRestarts::kMaxWindowMs - 1).isEmpty(),
              "a window still inside its duration has not expired");
        const QList<PlannedRestarts::Expired> gone =
            stuck.takeExpired(t0 + PlannedRestarts::kMaxWindowMs);
        check(gone.size() == 1 && gone.first().node == QStringLiteral("n3") &&
                  gone.first().kind == PlannedRestarts::Kind::Roll &&
                  PlannedRestarts::failureLine(gone.first())
                      .startsWith(QStringLiteral(
                          "n3 did not come back after its roll")) &&
                  !stuck.isOpen(QStringLiteral("n3")) &&
                  !stuck.quiets(refused, t0 + PlannedRestarts::kMaxWindowMs),
              "a node that never comes back ends its window with a failure");
    }

    {
        // --- Error reports the relay turns into a ping (adhoc #1538) --------
        // A warning dialog or an error notice used to be the *whole* record of a
        // failure on this machine. Each one now becomes one signed POST, so what
        // leaves the machine and how often it leaves are both pinned here.
        using forkmesh::ClientErrorReports;
        const qint64 t0 = 1'700'000'000'000LL;

        const auto groupKey = &ClientErrorReports::failureGroupKey;
        check(groupKey(QString::fromUtf8(
                  "Node n2 core caddy err: GET app.forkmesh.com/forkmesh/forkmesh/"
                  "info/refs -> 503 \xC2\xB7 \xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 "
                  "104.23.187.0")) ==
                  groupKey(QString::fromUtf8(
                      "Node n2 core caddy err: GET app.forkmesh.com/forkmesh/"
                      "forkmesh/info/refs -> 503 \xC2\xB7 "
                      "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA 162.158.154.0 "
                      "(Cloudflare)")),
              "one host/path/status from different clients is one failure group");
        check(groupKey(QStringLiteral(
                  "Node n2 core caddy err: GET app.forkmesh.com/x/info/refs -> 503")) !=
                  groupKey(QStringLiteral(
                      "Node n2 core caddy err: GET app.forkmesh.com/x/info/refs -> 502")),
              "a different status is a different failure group");
        check(groupKey(QStringLiteral(
                  "Node n2 core caddy err: GET app.forkmesh.com/x/info/refs -> 503")) !=
                  groupKey(QStringLiteral(
                      "Node n2 core caddy err: GET app.forkmesh.com/y/info/refs -> 503")),
              "a different path is a different failure group");
        check(groupKey(QStringLiteral(
                  "Deploy publisher: gateway cores: FAILED, 0 ref(s) updated, 7 "
                  "rejected")) ==
                  groupKey(QStringLiteral(
                      "Deploy publisher: gateway cores: FAILED, 0 ref(s) "
                      "updated, 6 rejected")),
              "a repeated failure whose counts drift is still one group");
        check(groupKey(QStringLiteral(
                  "Actions: could not publish pull request #310's head - "
                  "da9b3717e0b21df724fe21e4c91eb1a1fe6138b1 -> pr/310")) ==
                  groupKey(QStringLiteral(
                      "Actions: could not publish pull request #311's head - "
                      "79f5d92afd1df3517bd6cd4c379a8d9a92c3db75 -> pr/311")),
              "object ids and numbers do not split one failure into many groups");
        forkmesh::ErrorPingGroups groups;
        check(groups.note(QStringLiteral("k"), t0).count == 1,
              "the first occurrence opens a failure group");
        groups.bind(QStringLiteral("k"), 42, QStringLiteral("title"));
        const forkmesh::ErrorPingGroups::Group again =
            groups.note(QStringLiteral("k"), t0 + 1000);
        check(again.count == 2 && again.pingId == 42 && again.firstMs == t0,
              "a repeat folds into the bound ping with a count");
        check(groups.note(QStringLiteral("k"),
                          t0 + 1000 + forkmesh::ErrorPingGroups::kQuietMs)
                      .count == 1,
              "a failure that went quiet opens a fresh group");

        const QString redacted = ClientErrorReports::redact(
            QStringLiteral(
                "Could not push to https://forkmesh.com/alice/private-repo "
                "as alice@example.com from /home/alice/work/private-repo "
                "(token=abcdef-super-secret)"),
            ClientErrorReports::kMaxMessageChars);
        check(!redacted.contains(QStringLiteral("forkmesh.com"))
                  && redacted.contains(QStringLiteral("<url>")),
              "a report carries no URL off the machine");
        check(!redacted.contains(QStringLiteral("alice@example.com"))
                  && redacted.contains(QStringLiteral("<email>")),
              "a report carries no address off the machine");
        check(!redacted.contains(QStringLiteral("/home/alice"))
                  && redacted.contains(QStringLiteral("<path>")),
              "a report carries no local path off the machine");
        check(!redacted.contains(QStringLiteral("super-secret")),
              "a report carries no credential off the machine");
        check(redacted.startsWith(QStringLiteral("Could not push to")),
              "the shape of the failure survives the redaction");

        // The failure from the dialog this was built for: nothing in it is
        // private, so all of it survives and stays one stable group.
        const QString relayText = QStringLiteral(
            "Could not load the pending inbox (HTTP 0): ForkMesh relay is "
            "rate-limited (HTTP 429); backing off");
        check(ClientErrorReports::redact(
                  relayText, ClientErrorReports::kMaxMessageChars)
                  == relayText,
              "a relay rate-limit failure is reported verbatim");

        ClientErrorReports reports;
        const ClientErrorReports::Report first = ClientErrorReports::build(
            QStringLiteral("dialog"), QString(), QStringLiteral("Sync inbox"),
            relayText, t0);
        check(first.surface == QStringLiteral("app"),
              "a report with no named area defaults to the desktop surface");
        check(reports.accept(first), "the first sighting is reported");
        check(!reports.accept(ClientErrorReports::build(
                  QStringLiteral("dialog"), QString(),
                  QStringLiteral("Sync inbox"), relayText,
                  t0 + 30 * 60 * 1000)),
              "the same failure half an hour later is not reported again");
        check(reports.accept(ClientErrorReports::build(
                  QStringLiteral("dialog"), QString(),
                  QStringLiteral("Sync inbox"), relayText,
                  t0 + ClientErrorReports::kWindowMs + 1)),
              "once the dedupe window passes the failure reports again");
        check(!reports.accept(ClientErrorReports::build(
                  QStringLiteral("dialog"), QString(), QStringLiteral("Empty"),
                  QStringLiteral("   "), t0)),
              "a report with no message left is never sent");

        // A storm of distinct failures is capped, so a broken node cannot spend
        // its whole hour reporting.
        ClientErrorReports storm;
        int sent = 0;
        for (int i = 0; i < ClientErrorReports::kMaxPerWindow * 3; ++i) {
            if (storm.accept(ClientErrorReports::build(
                    QStringLiteral("notice"), QStringLiteral("headless"),
                    QString(),
                    QStringLiteral("distinct failure %1").arg(i), t0)))
                ++sent;
        }
        check(sent == ClientErrorReports::kMaxPerWindow,
              "no node sends more than the hourly cap of distinct reports");
        check(storm.accept(ClientErrorReports::build(
                  QStringLiteral("notice"), QStringLiteral("headless"),
                  QString(), QStringLiteral("a later failure"),
                  t0 + ClientErrorReports::kWindowMs + 1)),
              "the cap is a rolling window, not a lifetime budget");

        // The relay refusing the report is the failure most worth reporting:
        // park it, hand it back once, and give up rather than retry forever.
        ClientErrorReports parked;
        check(parked.defer(first) && parked.deferredCount() == 1,
              "a report the relay cannot take yet is parked");
        QList<ClientErrorReports::Report> ready = parked.takeDeferred(t0 + 1000);
        check(ready.size() == 1 && parked.deferredCount() == 0,
              "the parked report is handed back once the relay is reachable");
        ClientErrorReports::Report retried = ready.first();
        check(retried.attempts == 1, "a parked report counts its attempts");
        for (int i = 0; i < ClientErrorReports::kMaxAttempts + 2; ++i) {
            if (!parked.defer(retried))
                break;
            retried = parked.takeDeferred(t0 + 1000).value(0);
        }
        check(!parked.defer(retried) && parked.deferredCount() == 0,
              "a report is dropped rather than retried forever");
        check(parked.defer(first)
                  && parked.takeDeferred(
                                t0 + ClientErrorReports::kMaxDeferralMs + 1)
                         .isEmpty(),
              "a report nobody could deliver all day is dropped, not sent stale");

        ClientErrorReports overflow;
        for (int i = 0; i < ClientErrorReports::kMaxDeferred + 4; ++i)
            overflow.defer(ClientErrorReports::build(
                QStringLiteral("notice"), QStringLiteral("app"), QString(),
                QStringLiteral("parked failure %1").arg(i), t0));
        check(overflow.deferredCount() == ClientErrorReports::kMaxDeferred,
              "the parked queue is bounded");
        check(overflow.takeDeferred(t0).last().message.contains(
                  QStringLiteral("%1").arg(
                      ClientErrorReports::kMaxDeferred + 3)),
              "an overfull queue keeps the newest reports");

        const QJsonObject body = ClientErrorReports::payload(first);
        check(body.value(QStringLiteral("kind")).toString()
                      == QStringLiteral("dialog")
                  && body.value(QStringLiteral("surface")).toString()
                         == QStringLiteral("app")
                  && body.value(QStringLiteral("title")).toString()
                         == QStringLiteral("Sync inbox")
                  && body.value(QStringLiteral("message")).toString()
                         == relayText,
              "the posted body carries exactly the four fields the relay reads");

        // adhoc #1654: the relay pings the account when a new error group
        // appears, and the desktop shows that ping as a warning card. Reporting
        // the card back stored an error whose message quoted the group it came
        // from — a message the relay had never grouped before, so it opened a
        // new group, which pinged again, each round quoting the last. These
        // never leave the machine.
        check(ClientErrorReports::echoesRelayAlert(
                  QStringLiteral("error_group"),
                  QStringLiteral("New app error group (521) — APP "
                                 "<path> — desktop toast [app] Action failed")),
              "a new-error-group ping is not reported back as a new error");
        check(ClientErrorReports::echoesRelayAlert(
                  QStringLiteral("operational_alert"),
                  QStringLiteral("Worker errors needs attention")),
              "a relay status alert is not reported back as a desktop error");
        check(ClientErrorReports::echoesRelayAlert(
                  QStringLiteral("desktop"),
                  QStringLiteral("Worker errors needs attention - 521 on "
                                 "<path>: desktop toast [headless] Sync inbox")),
              "text still carrying the relay's own report stamp is held back "
              "however the card was labelled");
        check(ClientErrorReports::echoesRelayAlert(
                  QStringLiteral("desktop"),
                  QStringLiteral("New app error group (521) — APP "
                                 "<path> — desktop notice [app] Action failed")),
              "the stamp an inline notice's report wears is held back too");
        check(!ClientErrorReports::echoesRelayAlert(QStringLiteral("action"),
                                                    relayText)
                  && !ClientErrorReports::echoesRelayAlert(QString(),
                                                           relayText),
              "an ordinary failure this machine saw first is still reported");

        check(ClientErrorReports::relayAccepts(first),
              "an ordinary dialog report is one the relay stores");
        check(ClientErrorReports::relayAccepts(ClientErrorReports::build(
                  QStringLiteral("notice"), QStringLiteral("app"), QString(),
                  QStringLiteral("Push failed"), t0))
                  && ClientErrorReports::relayAccepts(ClientErrorReports::build(
                      QStringLiteral("toast"), QStringLiteral("app"), QString(),
                      QStringLiteral("Push failed"), t0)),
              "an inline notice's report is one the relay stores, as is the "
              "retired toast kind");
        const ClientErrorReports::Report quoting = ClientErrorReports::build(
            QStringLiteral("dialog"), QStringLiteral("app"),
            QStringLiteral("Agent network access"),
            QStringLiteral("Agent #684 on forkmesh could not resolve "
                           "<b>cdn.jsdelivr.net&amp;quot</b>. <code>desktop "
                           "dialog [app] Agent network access — Agent #459</code>"),
            t0);
        check(!ClientErrorReports::relayAccepts(quoting),
              "a report quoting the relay's own stamp after markup is never "
              "posted: the relay answers it 400 invalid_desktop_error");
        check(ClientErrorReports::echoesRelayAlert(QStringLiteral("dialog"),
                                                   quoting.message),
              "the echo rule matches the stamp wherever the relay does, "
              "without a word boundary");
        {
            ClientErrorReports::Report wrapped = first;
            wrapped.message = QStringLiteral("see desktop\n toast  [APP] x");
            check(!ClientErrorReports::relayAccepts(wrapped),
                  "a stamp split by a newline or double space is caught the "
                  "way the relay collapses whitespace");
            wrapped.message = QStringLiteral("see desktop\x01toast [app] x");
            check(!ClientErrorReports::relayAccepts(wrapped),
                  "a stamp joined by a control character is caught");
            wrapped.message = QStringLiteral("see desktop notice [app] x");
            check(!ClientErrorReports::relayAccepts(wrapped),
                  "a notice report's own stamp is caught");
            wrapped.message = QStringLiteral("\x01\x02 \x7f");
            check(!ClientErrorReports::relayAccepts(wrapped),
                  "a message of control characters is empty to the relay");
            check(ClientErrorReports::build(QStringLiteral("notice"),
                                            QStringLiteral("app"), QString(),
                                            QStringLiteral("\x01\x02 \x7f"), t0)
                      .message.isEmpty(),
                  "redaction turns control characters into spaces the way the "
                  "relay does, so an empty report is never built");
        }
        check(!ClientErrorReports::relayAccepts(ClientErrorReports::build(
                  QStringLiteral("local"), QStringLiteral("app"), QString(),
                  QStringLiteral("worktree busy"), t0)),
              "a kind the relay does not know is never posted");
        check(!ClientErrorReports::relayAccepts(ClientErrorReports::build(
                  QStringLiteral("notice"), QStringLiteral("phone"), QString(),
                  QStringLiteral("worktree busy"), t0)),
              "a surface the relay does not know is never posted");
        check(ClientErrorReports::permanentRefusal(400)
                  && ClientErrorReports::permanentRefusal(413)
                  && !ClientErrorReports::permanentRefusal(429)
                  && !ClientErrorReports::permanentRefusal(408)
                  && !ClientErrorReports::permanentRefusal(503)
                  && !ClientErrorReports::permanentRefusal(0),
              "only a 4xx other than a timeout or rate limit ends a report's "
              "retries");

        using DialogButton = ClientErrorReports::DialogButton;
        check(ClientErrorReports::dialogAsksQuestion(
                  {DialogButton::Decision, DialogButton::Choice}),
              "Task deleted's Delete/Keep is a question, not an error report");
        check(ClientErrorReports::dialogAsksQuestion(
                  {DialogButton::Choice, DialogButton::Choice}),
              "two custom choices are a question");
        check(!ClientErrorReports::dialogAsksQuestion({DialogButton::Acknowledge})
                  && !ClientErrorReports::dialogAsksQuestion(
                      {DialogButton::Acknowledge, DialogButton::Aside})
                  && !ClientErrorReports::dialogAsksQuestion({DialogButton::Choice})
                  && !ClientErrorReports::dialogAsksQuestion({}),
              "an OK-only warning, with or without a Show Details button, is "
              "still an error that reports");

        {
            namespace gr = forkmesh::gitrefusal;
            check(gr::overLocalChanges(QStringLiteral(
                      "error: Your local changes to the following files would be "
                      "overwritten by merge:\n\tdocs/a.md\nPlease commit your "
                      "changes or stash them before you merge."))
                      && !gr::overLocalChanges(
                          QStringLiteral("fatal: Unable to write index.")),
                  "a merge refused over local edits is recognised, a lock "
                  "failure is not");
            const gr::MergeSource local = gr::mergeSourceFromRefs(
                QStringLiteral("feature"),
                QStringLiteral("refs/remotes/origin/feature aaa\n"
                               "refs/heads/feature bbb\n"));
            check(local.kind == gr::MergeSourceKind::Local
                      && local.ref == QStringLiteral("refs/heads/feature"),
                  "a local branch is merged as it is");
            const gr::MergeSource remote = gr::mergeSourceFromRefs(
                QStringLiteral("446-add-button"),
                QStringLiteral("refs/remotes/forkmesh-mesh/446-add-button c1\n"
                               "refs/remotes/origin/446-add-button c1\n"));
            check(remote.kind == gr::MergeSourceKind::RemoteTracking
                      && remote.ref
                             == QStringLiteral(
                                 "refs/remotes/forkmesh-mesh/446-add-button")
                      && remote.candidates.size() == 2,
                  "a branch only on remotes that agree merges from the "
                  "remote-tracking ref");
            check(gr::mergeSourceFromRefs(
                      QStringLiteral("x"),
                      QStringLiteral("refs/remotes/a/x c1\nrefs/remotes/b/x c2\n"))
                          .kind
                      == gr::MergeSourceKind::Ambiguous,
                  "remotes that disagree are not merged blindly");
            check(gr::mergeSourceFromRefs(
                      QStringLiteral("landing"),
                      QStringLiteral("refs/remotes/origin/ui/landing c1\n"))
                          .kind
                      == gr::MergeSourceKind::Missing,
                  "ui/landing on origin is not the branch landing");
            check(gr::mergeSourceFromRefs(QStringLiteral("gone"), QString()).kind
                      == gr::MergeSourceKind::Missing,
                  "a branch with no ref anywhere is missing");
            check(gr::firstFailureLine(QStringLiteral(
                      "Fetching origin\nremote: busy\nfatal: unable to access "
                      "'https://app.forkmesh.com/x/': The requested URL returned "
                      "error: 503\n"))
                      .startsWith(QStringLiteral("fatal: unable to access")),
                  "the fatal line is what a failed fetch reports");
            check(gr::remoteUnavailable(QStringLiteral(
                      "fatal: unable to access 'https://h/': The requested URL "
                      "returned error: 503"))
                      && !gr::remoteUnavailable(QStringLiteral(
                          "fatal: Not possible to fast-forward, aborting.")),
                  "a 5xx from the remote is an outage, a diverged branch is not");
            check(gr::notFastForward(QStringLiteral(
                      "fatal: Not possible to fast-forward, aborting."))
                      && !gr::notFastForward(
                          QStringLiteral("fatal: couldn't find remote ref main")),
                  "only git's fast-forward refusal says diverged");
            const QString framing = QStringLiteral(
                "fatal: unable to access 'https://app.forkmesh.com/forkmesh/"
                "forkmesh/': Error in the HTTP2 framing layer\n");
            check(gr::remoteUnavailable(framing)
                      && gr::explainFailure(framing).startsWith(
                          QStringLiteral("The HTTPS connection broke")),
                  "an HTTP/2 framing error is explained as a broken connection");
            check(gr::explainFailure(QStringLiteral(
                      "error: RPC failed; curl 18 transfer closed with outstanding "
                      "read data remaining\nfatal: early EOF\n"))
                      .startsWith(QStringLiteral("The connection dropped")),
                  "a dropped download is explained");
            check(gr::explainFailure(QStringLiteral(
                      "remote: ForkMesh could not reach a mirror of this "
                      "repository just now. Try again in a minute.\nremote: "
                      "Help: https://forkmesh.com/docs/git-errors/\nfatal: "
                      "unable to access 'https://h/': The requested URL returned "
                      "error: 503\n"))
                      == QStringLiteral("ForkMesh could not reach a mirror of "
                                        "this repository just now. Try again "
                                        "in a minute."),
                  "ForkMesh's own remote: message wins");
            check(gr::explainFailure(QStringLiteral(
                      "remote: Enumerating objects: 5, done.\nfatal: couldn't "
                      "find remote ref topic\n"))
                      .isEmpty(),
                  "other remote: lines and git's own refusals stay unexplained");

            QTemporaryDir td;
            const QString origin = td.path() + QStringLiteral("/origin");
            const QString clone = td.path() + QStringLiteral("/clone");
            const auto git = [](const QString &dir, const QStringList &args) {
                QProcess p;
                p.start(QStringLiteral("git"),
                        QStringList{QStringLiteral("-C"), dir} + args);
                p.waitForFinished(10000);
                return QString::fromUtf8(p.readAllStandardOutput());
            };
            QDir().mkpath(origin);
            git(origin, {QStringLiteral("init"), QStringLiteral("-q"),
                         QStringLiteral("-b"), QStringLiteral("main")});
            git(origin, {QStringLiteral("-c"), QStringLiteral("user.email=t@t"),
                         QStringLiteral("-c"), QStringLiteral("user.name=T"),
                         QStringLiteral("commit"), QStringLiteral("-q"),
                         QStringLiteral("--allow-empty"), QStringLiteral("-m"),
                         QStringLiteral("root")});
            git(origin, {QStringLiteral("branch"), QStringLiteral("ui/landing")});
            git(td.path(), {QStringLiteral("clone"), QStringLiteral("-q"), origin,
                            clone});
            const QString refs = git(
                clone,
                QStringList{QStringLiteral("for-each-ref"),
                            QStringLiteral("--format=%(refname) %(objectname)")}
                    + gr::mergeSourceRefPatterns(QStringLiteral("ui/landing")));
            const gr::MergeSource cloned =
                gr::mergeSourceFromRefs(QStringLiteral("ui/landing"), refs);
            check(cloned.kind == gr::MergeSourceKind::RemoteTracking
                      && cloned.ref
                             == QStringLiteral("refs/remotes/origin/ui/landing"),
                  "git's own for-each-ref finds a slashed branch that exists "
                  "only on the remote");

            git(clone, {QStringLiteral("worktree"), QStringLiteral("add"),
                        QStringLiteral("-q"), QStringLiteral("-b"),
                        QStringLiteral("480-fresh-agent"),
                        td.path() + QStringLiteral("/wt"), QStringLiteral("main")});
            const QSet<QString> checkedOut = gr::branchesCheckedOutInWorktrees(
                git(clone, {QStringLiteral("worktree"), QStringLiteral("list"),
                            QStringLiteral("--porcelain")}));
            check(checkedOut.contains(QStringLiteral("480-fresh-agent"))
                      && checkedOut.contains(QStringLiteral("main"))
                      && checkedOut.size() == 2,
                  "a branch checked out in a linked worktree is never offered "
                  "as a merged branch to delete, since git refuses to delete it");
        }

        // adhoc #1659: "Could not remove the worktree at <path>" is git
        // refusing the operator's own click, on their own machine, while they
        // watch. redact() turns every such path into the same "<path>", so the
        // fleet's worktree refusals pile into one relay error group that pings
        // on every occurrence — noise no release can fix. Held back by kind,
        // so a genuine fault that happens to name a path still reports.
        check(ClientErrorReports::describesLocalOperation(
                  forkmesh::kLocalOperationNoticeKind),
              "a local operation's refusal is not reported to the relay");
        check(ClientErrorReports::describesLocalOperation(
                  QStringLiteral(" Local ")),
              "the local-operation kind is matched however it was cased");
        check(!ClientErrorReports::describesLocalOperation(QString())
                  && !ClientErrorReports::describesLocalOperation(
                      QStringLiteral("desktop"))
                  && !ClientErrorReports::describesLocalOperation(
                      QStringLiteral("action")),
              "every other notice kind still reaches the error log");
        check(!ClientErrorReports::echoesRelayAlert(
                  forkmesh::kLocalOperationNoticeKind,
                  QStringLiteral("Could not remove the worktree at <path>.")),
              "the local-operation rule is the only thing holding these back, "
              "so the relay-echo rule stays free of message-text guesswork");

        // The Pings row a report came from rides along so its Status column can
        // follow the report's fate, but it is local bookkeeping only: it never
        // goes on the wire, and two sightings of one failure still dedupe to a
        // single report (adhoc #1629).
        ClientErrorReports::Report filed = ClientErrorReports::build(
            QStringLiteral("dialog"), QString(), QStringLiteral("Sync inbox"),
            relayText, t0);
        filed.pingId = 42;
        check(!ClientErrorReports::payload(filed).contains(
                  QStringLiteral("pingId")),
              "the ping row id stays on the machine");
        ClientErrorReports rows;
        check(rows.accept(filed), "the first sighting is reported");
        ClientErrorReports::Report second = filed;
        second.pingId = 43;
        check(!rows.accept(second),
              "a second sighting filed as its own ping is still one report");

        // A report parked past the deferral window is never sent, so the page
        // row waiting on it has to be told — takeDeferred hands the expired ones
        // back instead of dropping them silently.
        ClientErrorReports abandoned;
        abandoned.defer(filed);
        QList<ClientErrorReports::Report> expired;
        check(abandoned
                  .takeDeferred(t0 + ClientErrorReports::kMaxDeferralMs + 1,
                                &expired)
                  .isEmpty()
                  && expired.size() == 1 && expired.first().pingId == 42,
              "a report dropped for age is handed back with the row it belongs to");
    }

    {
        // --- Where a filed ping stands with the cloud (adhoc #1629) ---------
        // Every alert the desktop raises is filed on the Pings page, and the
        // page has to be honest about which of those rows exist anywhere else:
        // one raised while the node was offline never left the machine, and one
        // that should have synced and didn't must not read like one that did.
        using forkmesh::PingSync;

        check(forkmesh::pingSyncLabel(PingSync::Offline)
                      == QStringLiteral("Offline")
                  && forkmesh::pingSyncLabel(PingSync::Failed)
                         == QStringLiteral("Not synced")
                  && forkmesh::pingSyncLabel(PingSync::Synced)
                         == QStringLiteral("Synced"),
              "the Status column names each state the operator can act on");
        check(forkmesh::pingSyncLabel(PingSync::Offline)
                  != forkmesh::pingSyncLabel(PingSync::Failed),
              "\"nothing was sent\" and \"it was refused\" are different rows");

        for (PingSync state :
             {PingSync::LocalOnly, PingSync::Offline, PingSync::Pending,
              PingSync::Synced, PingSync::Failed}) {
            check(forkmesh::pingSyncFromToken(forkmesh::pingSyncToken(state))
                      == state,
                  "a ping's cloud state survives the on-disk journal");
            check(!forkmesh::pingSyncDescription(state).isEmpty(),
                  "every state explains itself on hover");
        }
        check(forkmesh::pingSyncFromToken(QStringLiteral("nonsense"))
                      == PingSync::LocalOnly
                  && forkmesh::pingSyncFromToken(QString())
                         == PingSync::LocalOnly,
              "an unreadable journal entry claims the least, not the most");

        check(forkmesh::pingSyncIsUnsynced(PingSync::Offline)
                  && forkmesh::pingSyncIsUnsynced(PingSync::Failed)
                  && forkmesh::pingSyncIsUnsynced(PingSync::Pending)
                  && !forkmesh::pingSyncIsUnsynced(PingSync::Synced),
              "the rows the mesh does not have are the unsynced ones");

        // Nothing re-queues a report that only ever existed in memory, so a row
        // still in flight at shutdown is resolved on load rather than left
        // counting down forever.
        check(forkmesh::pingSyncAfterRestart(PingSync::Pending)
                      == PingSync::Failed
                  && !forkmesh::pingSyncRestartReason().isEmpty(),
              "a ping still syncing when the app closed comes back \"not synced\"");
        check(forkmesh::pingSyncAfterRestart(PingSync::Offline)
                      == PingSync::Offline
                  && forkmesh::pingSyncAfterRestart(PingSync::Synced)
                         == PingSync::Synced,
              "a settled ping is not re-judged by a restart");
    }
#if defined(Q_OS_LINUX)
    {
        // Descriptor accounting and the startup limit raise. The app dies with
        // a SIGTRAP inside glib (g_wakeup_new -> g_error) when a thread start
        // finds no descriptors left, so the 1024 soft cap most distributions
        // ship has to be lifted before Qt starts any thread.
        const int hard = SystemStats::openFileHardLimit();
        check(hard > 0 && SystemStats::openFileSoftLimit() > 0,
              "the descriptor caps are readable");
        check(SystemStats::threadCount() >= 1, "this process has threads");

        const int before = SystemStats::openFileCount();
        check(before > 0 && before <= SystemStats::openFileSoftLimit(),
              "open descriptors are counted and fit under the soft cap");
        {
            QFile held(QStringLiteral("/proc/self/status"));
            check(held.open(QIODevice::ReadOnly), "opened a probe descriptor");
            check(SystemStats::openFileCount() > before,
                  "an extra open descriptor shows up in the count");
        }

        // Drop the soft cap the way a stock login session does, then confirm
        // the raise takes it back up to the hard cap (bounded by the 64k
        // target) without ever exceeding what the kernel allows.
        rlimit narrowed{};
        check(::getrlimit(RLIMIT_NOFILE, &narrowed) == 0, "read RLIMIT_NOFILE");
        const rlim_t restore = narrowed.rlim_cur;
        narrowed.rlim_cur = 256;
        if (::setrlimit(RLIMIT_NOFILE, &narrowed) == 0) {
            check(SystemStats::openFileSoftLimit() == 256,
                  "the lowered soft cap is reported");
            const int raised = SystemStats::raiseOpenFileLimit();
            check(raised == qMin(hard, 65536),
                  "raiseOpenFileLimit lifts the soft cap to the hard cap, "
                  "capped at 64k");
            check(raised <= hard && raised == SystemStats::openFileSoftLimit(),
                  "the raise never exceeds the hard cap and is the live value");
            // Idempotent: calling it again on an already-raised process is a
            // no-op rather than a downgrade.
            check(SystemStats::raiseOpenFileLimit() == raised,
                  "a second raise leaves the soft cap alone");
            narrowed.rlim_cur = restore;
            ::setrlimit(RLIMIT_NOFILE, &narrowed);
        }
    }
#endif

    // Merge queue: the ordered list of pull requests the queue runner drains,
    // covering membership, explicit reordering, persistence round-trips, and
    // the states the runner records between passes.
    {
        MergeQueue queue;
        check(queue.isEmpty() && queue.summary() == QStringLiteral("Empty"),
              "a new merge queue is empty");
        check(queue.enqueue(7, 1000) && queue.enqueue(3, 2000) &&
                  queue.enqueue(12, 3000),
              "pull requests join the merge queue");
        check(!queue.enqueue(7, 4000),
              "a pull request cannot join the merge queue twice");
        check(!queue.enqueue(0, 4000) && !queue.enqueue(-2, 4000),
              "invalid pull numbers never join the merge queue");
        check(queue.numbers() == (QList<int>{7, 3, 12}),
              "the merge queue preserves arrival order");
        check(queue.front().number == 7,
              "the front of the merge queue is the first entry");

        check(queue.move(12, -1) && queue.numbers() == (QList<int>{7, 12, 3}),
              "an entry moves one place towards the front");
        check(queue.move(7, 5) && queue.numbers() == (QList<int>{12, 3, 7}),
              "a move past the back clamps to the back");
        check(!queue.move(12, -1),
              "the front entry cannot move further forward");
        check(!queue.move(99, 1), "moving an absent entry is refused");

        check(queue.setState(3, MergeQueueState::Blocked,
                             QStringLiteral("Waiting for peer\napproval"), 5000),
              "an entry records the runner's verdict");
        check(queue.at(3).state == MergeQueueState::Blocked &&
                  queue.at(3).detail ==
                      QStringLiteral("Waiting for peer approval"),
              "state details are flattened to one line");
        check(!queue.setState(99, MergeQueueState::Failed, QString(), 5000),
              "a verdict for a departed entry is dropped");
        check(queue.summary().contains(QStringLiteral("3 queued")) &&
                  queue.summary().contains(QStringLiteral("1 blocked")),
              "the summary counts queued and blocked entries");

        // Persistence round-trip: order, states and details survive; the
        // in-flight states deliberately do not (a merge interrupted by an app
        // exit must come back as plain "queued", not "merging" forever).
        check(queue.setState(12, MergeQueueState::Merging, QString(), 6000),
              "the front entry can be marked merging");
        const MergeQueue restored = MergeQueue::fromRows(queue.toRows());
        check(restored.numbers() == queue.numbers(),
              "the merge queue round-trips its order through rows");
        check(restored.at(3).state == MergeQueueState::Blocked &&
                  restored.at(3).detail ==
                      QStringLiteral("Waiting for peer approval"),
              "settled states round-trip through rows");
        check(restored.at(12).state == MergeQueueState::Queued,
              "an interrupted in-flight state reloads as queued");

        // Tolerant loading: a bare number is a valid row (hand-edited or from
        // an older build), garbage and duplicates are dropped.
        const MergeQueue loose = MergeQueue::fromRows(
            {QStringLiteral("5"), QStringLiteral("not-a-number"),
             QStringLiteral("5\t123\tqueued\t"), QStringLiteral("8\tx\tbogus")});
        check(loose.numbers() == (QList<int>{5, 8}),
              "row loading keeps bare numbers, drops garbage and duplicates");
        check(loose.at(8).state == MergeQueueState::Queued,
              "an unknown persisted state degrades to queued");

        check(queue.remove(3) && queue.numbers() == (QList<int>{12, 7}),
              "an entry can be taken back out of the merge queue");
        check(!queue.remove(3), "removing a departed entry is refused");
        const QString longDetail(500, QLatin1Char('x'));
        check(MergeQueue::sanitizeDetail(longDetail).size() <= 200,
              "oversized state details are bounded");
        queue.clear();
        check(queue.isEmpty(), "clearing the merge queue empties it");
    }

    {
        // Window view modes: persisted by name, unknown names fall back, the
        // shortcut cycles every mode, and only Standard keeps the full shell.
        using namespace forkmesh::ui;
        bool roundTrips = true;
        QSet<QString> keys;
        for (AppViewMode mode : kAppViewModes) {
            roundTrips = roundTrips && appViewModeFromKey(appViewModeKey(mode)) == mode;
            keys.insert(appViewModeKey(mode));
        }
        check(roundTrips && keys.size() == kAppViewModeCount,
              "every view mode round-trips through a distinct settings key");
        check(appViewModeFromKey(QStringLiteral(" Workspace ")) ==
                  AppViewMode::Workspace,
              "a saved view key is matched case- and whitespace-insensitively");
        check(appViewModeFromKey(QStringLiteral("zen")) == AppViewMode::Standard &&
                  appViewModeFromKey(QString(), AppViewMode::Focus) ==
                      AppViewMode::Focus,
              "an unknown or missing view key falls back");
        AppViewMode walked = AppViewMode::Standard;
        QSet<int> visited;
        for (int i = 0; i < kAppViewModeCount; ++i) {
            visited.insert(static_cast<int>(walked));
            walked = nextAppViewMode(walked);
        }
        check(walked == AppViewMode::Standard && visited.size() == kAppViewModeCount,
              "cycling the view visits every mode and returns to Standard");
        check(!appViewModeHidesShellChrome(AppViewMode::Standard) &&
                  appViewModeHidesShellChrome(AppViewMode::Focus) &&
                  appViewModeHidesShellChrome(AppViewMode::Workspace),
              "only Standard keeps the navigation rail and status strip");
        check(appViewModeShowsWorkspaceTerminal(AppViewMode::Workspace) &&
                  appViewModeOpensAgents(AppViewMode::Workspace) &&
                  !appViewModeShowsWorkspaceTerminal(AppViewMode::Focus) &&
                  !appViewModeOpensAgents(AppViewMode::Standard),
              "only Workspace opens Agents with its terminal pane");
    }

    {
        // Chat sidebar groups: a presentation view over the flat channel
        // list. Provider channels (Discord) auto-group under their server;
        // user groups, drags, collapse state, and tombstones persist in one
        // JSON document; removing the provider purges its traces.
        using namespace ChannelGroups;
        const auto sl = [](std::initializer_list<const char *> parts) {
            QStringList out;
            for (const char *part : parts)
                out.append(QString::fromUtf8(part));
            return out;
        };
        const auto flatten = [](const QList<Row> &rows) {
            QStringList out;
            for (const Row &row : rows) {
                if (row.header)
                    out.append(QStringLiteral("[%1]").arg(row.group));
                else if (row.group.isEmpty())
                    out.append(row.channel);
                else
                    out.append(row.group + QStringLiteral(">") + row.channel);
            }
            return out;
        };
        const QString discordGroup = QString::fromUtf8("Discord \xC2\xB7 Forkmesh");
        const auto autoGroup = [discordGroup](const QString &channel) {
            return channel.startsWith(QStringLiteral("#discord/")) ? discordGroup
                                                                   : QString();
        };
        const QStringList channels =
            sl({"#general", "#welcome", "#discord/1", "#discord/2"});
        const QString team = QStringLiteral("Team");

        State state;
        QList<Row> rows = layout(state, channels, autoGroup);
        check(flatten(rows) ==
                  sl({"#general", "#welcome", "[Discord \xC2\xB7 Forkmesh]",
                      "Discord \xC2\xB7 Forkmesh>#discord/1",
                      "Discord \xC2\xB7 Forkmesh>#discord/2"}),
              "provider channels auto-group under their server after the ungrouped rooms");
        check(rows.at(2).automatic && toJson(state).isEmpty(),
              "an automatic group needs no persisted state");

        check(createGroup(state, QStringLiteral("  Team  ")) &&
                  !createGroup(state, team) && !createGroup(state, QStringLiteral("   ")),
              "group names are normalized, unique, and never blank");
        rows = layout(state, channels, autoGroup);
        check(rows.size() == 6 && rows.at(2).header && rows.at(2).group == team &&
                  !rows.at(2).automatic,
              "an empty user group still renders so it can receive a drop");

        check(assign(state, QStringLiteral("#welcome"), team, -1,
                     renderedMembersOf(rows, team), false),
              "a channel can be dropped on a group header");
        rows = layout(state, channels, autoGroup);
        check(assign(state, QStringLiteral("#discord/1"), team, 1,
                     renderedMembersOf(rows, team), false),
              "a provider channel can be dropped into a user group");
        rows = layout(state, channels, autoGroup);
        check(flatten(rows) ==
                  sl({"#general", "[Team]", "Team>#welcome", "Team>#discord/1",
                      "[Discord \xC2\xB7 Forkmesh]",
                      "Discord \xC2\xB7 Forkmesh>#discord/2"}),
              "channels dragged into a group leave their automatic group");

        check(assign(state, QStringLiteral("#discord/1"), team, 0,
                     renderedMembersOf(rows, team), false),
              "a member can be dropped above a sibling");
        rows = layout(state, channels, autoGroup);
        check(renderedMembersOf(rows, team) == sl({"#discord/1", "#welcome"}),
              "reordering inside a group keeps the drop index");
        check(assign(state, QStringLiteral("#discord/1"), team, 2,
                     renderedMembersOf(rows, team), false),
              "a member can be dropped below a later sibling");
        rows = layout(state, channels, autoGroup);
        check(renderedMembersOf(rows, team) == sl({"#welcome", "#discord/1"}),
              "dropping below a later sibling accounts for the vacated row");

        check(assign(state, QStringLiteral("#discord/1"), QString(), -1,
                     QStringList(), false),
              "a channel can be dropped back on the top level");
        rows = layout(state, channels, autoGroup);
        check(flatten(rows) ==
                  sl({"#general", "#discord/1", "[Team]", "Team>#welcome",
                      "[Discord \xC2\xB7 Forkmesh]",
                      "Discord \xC2\xB7 Forkmesh>#discord/2"}),
              "a channel moved to the top level is pinned out of its automatic group");

        setCollapsed(state, discordGroup, true, true);
        rows = layout(state, channels, autoGroup);
        check(rows.at(4).header && rows.at(4).group == discordGroup &&
                  rows.at(4).collapsed && rows.at(4).automatic,
              "collapse state persists for automatic groups");
        rows = layout(state, sl({"#general"}), autoGroup);
        check(renderedGroupNames(rows) == sl({"Team"}),
              "an automatic group with no channel present disappears");

        const State restored = fromJson(toJson(state));
        check(!toJson(state).isEmpty() && toJson(restored) == toJson(state) &&
                  restored.groups.size() == 2 &&
                  restored.ungrouped == sl({"#discord/1"}),
              "groups round-trip through one JSON document");
        check(fromJson(QStringLiteral("garbage")).isEmpty() &&
                  fromJson(QStringLiteral(
                               "{\"groups\":[{\"name\":\"\"},"
                               "{\"name\":\"A\",\"channels\":[\"#x\",\"#x\"]},"
                               "{\"name\":\"A\"}],\"ungrouped\":[\"#x\"]}"))
                          .groups.size() == 1 &&
                  fromJson(QStringLiteral(
                               "{\"groups\":[{\"name\":\"A\",\"channels\":[\"#x\",\"#x\"]}],"
                               "\"ungrouped\":[\"#x\"]}"))
                          .ungrouped.isEmpty(),
              "malformed group state degrades to what is valid");

        State renamed = state;
        rows = layout(renamed, channels, autoGroup);
        check(renameGroup(renamed, discordGroup, QStringLiteral("Community"),
                          renderedMembersOf(rows, discordGroup)) &&
                  !renameGroup(renamed, team, QStringLiteral("Community"),
                               QStringList()),
              "renaming keeps group names unique");
        rows = layout(renamed, channels, autoGroup);
        check(renderedGroupOf(rows, QStringLiteral("#discord/2")) ==
                      QStringLiteral("Community") &&
                  renderedGroupNames(rows) == sl({"Team", "Community"}),
              "renaming an automatic group keeps its channels under the new name");

        State removed = state;
        rows = layout(removed, channels, autoGroup);
        removeGroup(removed, team, renderedMembersOf(rows, team));
        rows = layout(removed, channels, autoGroup);
        check(renderedGroupOf(rows, QStringLiteral("#welcome")).isEmpty() &&
                  !renderedGroupNames(rows).contains(team),
              "deleting a group returns its channels to the top level");

        check(purge(
                  state,
                  [](const QString &channel) {
                      return channel.startsWith(QStringLiteral("#discord/"));
                  },
                  [](const QString &group) {
                      return group.startsWith(QString::fromUtf8("Discord \xC2\xB7 "));
                  }) &&
                  state.ungrouped.isEmpty() && indexOfGroup(state, discordGroup) < 0 &&
                  indexOfGroup(state, team) >= 0 &&
                  !purge(state, [](const QString &) { return false; },
                         [](const QString &) { return false; }),
              "removing a provider purges its channels and automatic group but keeps user groups");
    }

    {
        RailFolders::State state;
        const QString tools = RailFolders::create(state, QStringLiteral("  Tools  "));
        check(tools == QStringLiteral("1") && state.first().name == QStringLiteral("Tools"),
              "rail folders get a numeric id and a trimmed name");
        check(RailFolders::create(state, QStringLiteral("   ")).isEmpty(),
              "a rail folder needs a name");
        const QString work = RailFolders::create(state, QStringLiteral("Work"));
        check(RailFolders::move(state, QStringLiteral("notes"), tools) &&
                  RailFolders::move(state, QStringLiteral("calendar"), tools),
              "rail items move into a folder");
        check(!RailFolders::move(state, QStringLiteral("notes"), tools),
              "moving an item into its own folder is a no-op");
        check(RailFolders::move(state, QStringLiteral("notes"), work) &&
                  RailFolders::folderOf(state, QStringLiteral("notes")) == work &&
                  !state.at(0).members.contains(QStringLiteral("notes")),
              "an item lives in one rail folder at a time");
        check(!RailFolders::move(state, RailFolders::railKey(work), tools),
              "a folder cannot go inside another folder");
        check(!RailFolders::move(state, QStringLiteral("chat"), QStringLiteral("99")),
              "moving into a missing folder is refused");
        const RailFolders::State reread =
            RailFolders::parse(RailFolders::serialize(state));
        check(reread.size() == 2 && reread.at(0).members == QStringList{QStringLiteral("calendar")} &&
                  reread.at(1).name == QStringLiteral("Work"),
              "rail folders round-trip through their settings JSON");
        const RailFolders::State messy = RailFolders::parse(QStringLiteral(
            R"([{"id":"a","name":"","members":["x","folder:b","x"]},)"
            R"({"id":"a","name":"dup"},{"id":"b","name":"B","members":["x","y"]},{"name":"noid"}])"));
        check(messy.size() == 2 && messy.at(0).name == QStringLiteral("Folder") &&
                  messy.at(0).members == QStringList{QStringLiteral("x")} &&
                  messy.at(1).members == QStringList{QStringLiteral("y")},
              "stored rail folders drop duplicates, nesting and id-less entries");
        check(RailFolders::move(state, QStringLiteral("calendar"), QString()) &&
                  RailFolders::folderOf(state, QStringLiteral("calendar")).isEmpty(),
              "an item can be taken back out onto the rail");
        check(RailFolders::remove(state, tools) && state.size() == 1 &&
                  RailFolders::create(state, QStringLiteral("Again")) == tools,
              "removing a folder frees its id");
    }

    {
        using RailGroups::SortMode;
        check(RailGroups::parseSortMode(RailGroups::sortModeKey(SortMode::Count)) ==
                      SortMode::Count &&
                  RailGroups::parseSortMode(QStringLiteral("bogus")) == SortMode::Custom,
              "rail sort modes round-trip and an unknown one means custom order");
        RailGroups::State state = RailGroups::parse(QString());
        check(RailGroups::isDefault(state) &&
                  state.groups.size() == RailGroups::builtInGroups().size() &&
                  RailGroups::groupOf(state, QStringLiteral("chat")) == QStringLiteral("people") &&
                  RailGroups::groupOf(state, QStringLiteral("package:crm")) ==
                      QStringLiteral("packages") &&
                  RailGroups::groupOf(state, QStringLiteral("repoTab:Issues")) ==
                      QStringLiteral("work") &&
                  RailGroups::groupOf(state, QStringLiteral("nope")) == QStringLiteral("more"),
              "empty rail group state holds the built-in groups in order");
        const QString daily = RailGroups::create(state, QStringLiteral("  Daily  "));
        check(daily == QStringLiteral("custom-1") && state.groups.first().id == daily &&
                  state.groups.first().name == QStringLiteral("Daily") &&
                  RailGroups::create(state, QStringLiteral("  ")).isEmpty(),
              "a new rail group goes first, trimmed, and needs a name");
        check(RailGroups::assign(state, QStringLiteral("chat"), daily) &&
                  RailGroups::groupOf(state, QStringLiteral("chat")) == daily &&
                  !RailGroups::assign(state, QStringLiteral("chat"), daily) &&
                  !RailGroups::assign(state, QStringLiteral("chat"), QStringLiteral("missing")),
              "rail items can be assigned to a custom group");
        check(RailGroups::assign(state, QStringLiteral("stats"), QStringLiteral("work")) &&
                  RailGroups::assign(state, QStringLiteral("stats"), QStringLiteral("insights")) &&
                  !state.assigned.contains(QStringLiteral("stats")),
              "assigning an item back to its own built-in group drops the override");
        check(!RailGroups::rename(state, QStringLiteral("work"), QStringLiteral("Mine")) &&
                  !RailGroups::remove(state, QStringLiteral("work")) &&
                  RailGroups::rename(state, daily, QStringLiteral("Every day")),
              "only custom rail groups can be renamed or removed");
        check(RailGroups::moveGroup(state, daily, 1) &&
                  state.groups.at(1).id == daily &&
                  !RailGroups::moveGroup(state, QStringLiteral("more"), 1),
              "rail groups move up and down");
        check(RailGroups::moveGroup(state, daily, 1,
                                    {daily, QStringLiteral("insights")}) &&
                  RailGroups::indexOf(state, daily) ==
                      RailGroups::indexOf(state, QStringLiteral("insights")) + 1,
              "moving a rail group skips groups that are not shown");
        const RailGroups::State reread =
            RailGroups::parse(RailGroups::serialize(state));
        check(reread.groups.size() == state.groups.size() &&
                  RailGroups::groupOf(reread, QStringLiteral("chat")) == daily &&
                  reread.groups.at(RailGroups::indexOf(reread, daily)).name ==
                      QStringLiteral("Every day") &&
                  RailGroups::indexOf(reread, daily) == RailGroups::indexOf(state, daily),
              "rail groups round-trip through their settings JSON");
        const RailGroups::State messy = RailGroups::parse(QStringLiteral(
            R"({"groups":[{"id":"tools"},{"id":"custom-9","name":""},{"id":"tools"},)"
            R"({"id":"x","name":"X"}],"assigned":{"chat":"gone","notes":"work","files":"custom-9"}})"));
        check(messy.groups.first().id == QStringLiteral("tools") &&
                  messy.groups.at(1).name == QStringLiteral("Group") &&
                  RailGroups::indexOf(messy, QStringLiteral("x")) < 0 &&
                  messy.groups.size() == RailGroups::builtInGroups().size() + 1 &&
                  messy.assigned.size() == 1 &&
                  messy.assigned.value(QStringLiteral("files")) == QStringLiteral("custom-9"),
              "stored rail groups drop duplicates, foreign ids and dead assignments");
        check(RailGroups::remove(state, daily) &&
                  RailGroups::groupOf(state, QStringLiteral("chat")) == QStringLiteral("people") &&
                  RailGroups::isDefault(RailGroups::parse(RailGroups::serialize(state))),
              "removing a rail group sends its items back to their own groups");
    }

    {
        RailShortcuts::State state;
        RailShortcuts::Shortcut closed;
        closed.path = QStringLiteral("n:central/@0/c:QPushButton#3");
        closed.label = QStringLiteral("&Closed (12)");
        closed.icon = QStringLiteral("issue-closed");
        closed.section = 0;
        closed.repoTab = 2;
        const QString first = RailShortcuts::add(state, closed);
        check(first == QStringLiteral("1") &&
                  state.first().label == QStringLiteral("Closed") &&
                  RailShortcuts::railKey(first) == QStringLiteral("shortcut:1"),
              "a rail shortcut gets a numeric id and a label without its count");
        check(RailShortcuts::add(state, closed) == first && state.size() == 1,
              "dropping the same button twice keeps one shortcut");
        RailShortcuts::Shortcut bare;
        bare.path = QStringLiteral("n:settingsButton");
        bare.icon = QStringLiteral(":/icons/evil.svg");
        const QString second = RailShortcuts::add(state, bare);
        check(second == QStringLiteral("2") &&
                  state.at(1).label == QStringLiteral("Page") &&
                  state.at(1).icon == RailShortcuts::kDefaultIcon,
              "a shortcut without a label or octicon gets defaults");
        check(RailShortcuts::add(state, RailShortcuts::Shortcut{}).isEmpty(),
              "a shortcut needs the button's path");
        check(RailShortcuts::isShortcutKey(QStringLiteral("shortcut:2")) &&
                  RailShortcuts::idOf(QStringLiteral("shortcut:2")) == second &&
                  RailShortcuts::idOf(QStringLiteral("notes")).isEmpty(),
              "shortcut rail keys carry their id");
        const RailShortcuts::State reread =
            RailShortcuts::parse(RailShortcuts::serialize(state));
        check(reread.size() == 2 && reread.at(0).path == closed.path &&
                  reread.at(0).section == 0 && reread.at(0).repoTab == 2 &&
                  reread.at(0).icon == QStringLiteral("issue-closed"),
              "rail shortcuts round-trip through their settings JSON");
        const RailShortcuts::State messy = RailShortcuts::parse(QStringLiteral(
            R"([{"id":"a","path":"p"},{"id":"a","path":"q"},{"id":"b","path":"p"},)"
            R"({"id":"c"},{"path":"r"},{"id":"d","path":"s","label":"Stats"}])"));
        check(messy.size() == 2 && messy.at(0).label == QStringLiteral("Page") &&
                  messy.at(0).section == -1 && messy.at(1).label == QStringLiteral("Stats"),
              "stored rail shortcuts drop duplicate ids, duplicate paths and incomplete entries");
        check(RailShortcuts::remove(state, first) && state.size() == 1 &&
                  RailShortcuts::add(state, closed) == first,
              "removing a shortcut frees its id");
        QStringList known;
        check(RailShortcuts::rememberPageButton(known, QStringLiteral("a")) &&
                  !RailShortcuts::rememberPageButton(known, QStringLiteral("a")) &&
                  !RailShortcuts::rememberPageButton(known, QString()),
              "a page button is remembered once");
        for (int i = 0; i < RailShortcuts::kMaxKnownPageButtons; ++i)
            RailShortcuts::rememberPageButton(known, QString::number(i));
        check(known.size() == RailShortcuts::kMaxKnownPageButtons &&
                  !known.contains(QStringLiteral("a")),
              "the remembered page buttons drop the oldest past the cap");
    }

    if (failures) {
        qCritical("TESTS FAILED");
        return 1;
    }
    qInfo("ALL TESTS PASSED");
    return 0;
}

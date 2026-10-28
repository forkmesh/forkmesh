
#include "MainWindow.h"
#include "ReleaseDraft.h"
#include "SyncStatusModel.h"
#include "MainWindowInternal.h"
#include "KebabHeaderView.h"
#include "NetworkReplyError.h"
#include "MirrorSyncVerdict.h"
#include "MirrorNodeColumns.h"
#include "SafeDelete.h"

#include <QFileInfo>
#include <QTemporaryDir>
#include <QVersionNumber>

using namespace forkmesh::ui;

namespace {

// The source's heads+tags digest the Mirror nodes panel last compared against,
// per source repository. Mirror observation fetches stamp each result with the
// value current when they were requested (forkmesh::mirrorsync).
QHash<QString, QString> &mirrorObservationSourceRefs()
{
    static QHash<QString, QString> refs;
    return refs;
}

enum class MirrorCommitRelation {
    Unknown,
    Same,
    BehindSource,
    AheadOfSource,
    Diverged,
};

struct MirrorSyncBacklog {
    int issues = 0;
    int commits = 0;
    int branches = 0;
    int pulls = 0;
    int discussions = 0;
};

bool isVisibleMirrorBranch(const QString &value)
{
    QString name = value.trimmed();
    const QString headsPrefix = QStringLiteral("refs/heads/");
    if (name.startsWith(headsPrefix))
        name.remove(0, headsPrefix.size());
    // These refs persist pull records and their compatibility index. They are
    // collaboration storage, not branches a user can check out or work on.
    return name != QLatin1String("forkmesh/pulls") &&
           !name.startsWith(QLatin1String("forkmesh/pull/"));
}

QHash<QString, QString> branchHeadsFromGit(const QString &gitDir)
{
    QHash<QString, QString> heads;
    QByteArray output;
    if (gitDir.trimmed().isEmpty() ||
        !runGitCapture(
            gitDir,
            {QStringLiteral("for-each-ref"),
             QStringLiteral("--format=%(refname:strip=2) %(objectname)"),
             QStringLiteral("refs/heads")},
            &output, nullptr)) {
        return heads;
    }
    const QStringList lines = QString::fromUtf8(output).split(
        QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const int separator = line.lastIndexOf(QLatin1Char(' '));
        if (separator <= 0)
            continue;
        const QString name = line.left(separator).trimmed();
        const QString oid = line.mid(separator + 1).trimmed().toLower();
        if (!name.isEmpty() && isVisibleMirrorBranch(name) &&
            (oid.size() == 40 || oid.size() == 64))
            heads.insert(name, oid);
    }
    return heads;
}

QHash<QString, QString> branchHeadsFromJson(const QJsonObject &object)
{
    QHash<QString, QString> heads;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString name = it.key().trimmed();
        const QString oid = it.value().toString().trimmed().toLower();
        if (!name.isEmpty() && isVisibleMirrorBranch(name) &&
            (oid.size() == 40 || oid.size() == 64))
            heads.insert(name, oid);
    }
    return heads;
}

const QString kDirectMirrorCachePrefix =
    QStringLiteral("mirrors/directDiscovery/");
const QString kPendingReleaseRolloutsSetting =
    QStringLiteral("releases/pendingMirrorRollouts");
const QString kPendingReleasePublicationsSetting =
    QStringLiteral("releases/pendingMirrorPublications");
const QString kPendingReleaseArtifactPrunesSetting =
    QStringLiteral("releases/pendingArtifactPrunes");

QString releaseRolloutToken(const QString &owner, const QString &name,
                            const QString &tag)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonArray{owner.trimmed().toLower(),
                                 name.trimmed().toLower(), tag.trimmed()})
            .toJson(QJsonDocument::Compact));
}

void rememberReleaseIntent(const QString &setting, const QString &owner,
                           const QString &name, const QString &tag)
{
    const QString token = releaseRolloutToken(owner, name, tag);
    if (token.isEmpty())
        return;
    QSettings settings;
    QStringList pending = settings.value(setting).toStringList();
    const QString wantedOwner = owner.trimmed().toLower();
    const QString wantedName = name.trimmed().toLower();
    for (qsizetype i = pending.size() - 1; i >= 0; --i) {
        const QJsonArray parts =
            QJsonDocument::fromJson(pending.at(i).toUtf8()).array();
        if (parts.size() >= 2 && parts.at(0).toString() == wantedOwner &&
            parts.at(1).toString() == wantedName) {
            pending.removeAt(i);
        }
    }
    pending.append(token);
    while (pending.size() > 32)
        pending.removeFirst();
    settings.setValue(setting, pending);
    settings.sync();
}

bool releaseIntentPending(const QString &setting, const QString &owner,
                          const QString &name, const QString &tag)
{
    return QSettings()
        .value(setting)
        .toStringList()
        .contains(releaseRolloutToken(owner, name, tag));
}

bool releaseIntentPendingForRepository(const QString &setting,
                                       const QString &owner,
                                       const QString &name)
{
    const QString wantedOwner = owner.trimmed().toLower();
    const QString wantedName = name.trimmed().toLower();
    if (wantedOwner.isEmpty() || wantedName.isEmpty())
        return false;
    const QStringList pending = QSettings().value(setting).toStringList();
    for (const QString &token : pending) {
        const QJsonArray parts =
            QJsonDocument::fromJson(token.toUtf8()).array();
        if (parts.size() >= 3 && parts.at(0).toString() == wantedOwner &&
            parts.at(1).toString() == wantedName &&
            !parts.at(2).toString().isEmpty()) {
            return true;
        }
    }
    return false;
}

void clearReleaseIntent(const QString &setting, const QString &owner,
                        const QString &name, const QString &tag)
{
    QSettings settings;
    QStringList pending = settings.value(setting).toStringList();
    if (!pending.removeAll(releaseRolloutToken(owner, name, tag)))
        return;
    if (pending.isEmpty())
        settings.remove(setting);
    else
        settings.setValue(setting, pending);
    settings.sync();
}

QString directMirrorCacheKey(const QString &source)
{
    return kDirectMirrorCachePrefix +
           QString::fromLatin1(QCryptographicHash::hash(
                                  source.trimmed().toLower().toUtf8(),
                                  QCryptographicHash::Sha256)
                                  .toHex());
}

QJsonArray loadDirectMirrorCache(const QString &source)
{
    const QJsonObject envelope = QJsonDocument::fromJson(
        QSettings().value(directMirrorCacheKey(source)).toByteArray()).object();
    if (envelope.value(QStringLiteral("source")).toString() != source)
        return {};
    return envelope.value(QStringLiteral("mirrors")).toArray();
}

void saveDirectMirrorCache(const QString &source, const QJsonArray &mirrors)
{
    const QJsonObject envelope{
        {QStringLiteral("source"), source},
        {QStringLiteral("savedAt"), double(QDateTime::currentMSecsSinceEpoch())},
        {QStringLiteral("mirrors"), mirrors},
    };
    QSettings().setValue(
        directMirrorCacheKey(source),
        QJsonDocument(envelope).toJson(QJsonDocument::Compact));
}

QUrl normalizedDirectMirrorOrigin(const QString &value)
{
    QUrl origin(value.trimmed());
    if (!origin.isValid() || origin.scheme() != QLatin1String("https") ||
        origin.host().isEmpty() || !origin.userInfo().isEmpty())
        return {};
    origin.setPath(QString());
    origin.setQuery(QString());
    origin.setFragment(QString());
    return origin;
}

bool savedHostMayBeActive(const QJsonObject &host)
{
    return forkmesh::control::savedHostMayBeActive(host);
}

QByteArray sshMirrorMetadataProgram()
{
    // This program is transported as base64 and executed by the host's system
    // Python.  It is read-only, receives only normalized repository names, and
    // prints one bounded JSON object.  Keeping the collector self-contained
    // also lets older managed mirrors report their facts before their ForkMesh
    // binary has been updated with any newer inspection command.
    return QByteArrayLiteral(R"PY(
import base64,hashlib,json,os,platform,shutil,stat,subprocess,sys,time

owner=base64.b64decode(sys.argv[1]).decode('utf-8')
repo_name=base64.b64decode(sys.argv[2]).decode('utf-8')

def read_json(path,limit=4*1024*1024):
    try:
        st=os.lstat(path)
        if not os.path.isfile(path) or os.path.islink(path) or st.st_size>limit:
            return {}
        with open(path,'r',encoding='utf-8') as handle:
            value=json.load(handle)
        return value if isinstance(value,dict) else {}
    except Exception:
        return {}

def run(args,timeout=5):
    try:
        result=subprocess.run(args,stdin=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                              stdout=subprocess.PIPE,timeout=timeout,check=False)
        if result.returncode==0:
            return result.stdout.decode('utf-8','replace').strip()
    except Exception:
        pass
    return ''

main=read_json('/etc/forkmesh/mirror-node.json',256*1024)
gateway=read_json(str(main.get('gatewayConfig') or ''),4*1024*1024)
legacy={}
if not gateway:
    legacy=read_json('/var/lib/forkmesh-mirror/gateway/mirror-refresh.json',4*1024*1024)
    gateway=read_json(str(legacy.get('gatewayConfigPath') or ''),4*1024*1024)

git_dir=''
release_store=''
public_repo_dirs=set()
for candidate in gateway.get('repositories',[]) if isinstance(gateway.get('repositories'),list) else []:
    if not isinstance(candidate,dict):
        continue
    candidate_dir=str(candidate.get('gitDir') or '')
    if (candidate.get('enabled',True) and
            str(candidate.get('visibility') or 'public')=='public' and
            candidate_dir):
        public_repo_dirs.add(os.path.realpath(candidate_dir))
    if (str(candidate.get('owner') or '').casefold()==owner.casefold() and
            str(candidate.get('name') or '').casefold()==repo_name.casefold()):
        git_dir=candidate_dir
        release_store=str(candidate.get('releaseStore') or '')
if not git_dir and legacy:
    legacy_name=str(legacy.get('repositoryName') or '')
    aliases=legacy.get('ownerAliases') if isinstance(legacy.get('ownerAliases'),list) else []
    aliases=[str(value).casefold() for value in aliases]
    if owner.casefold() in aliases and legacy_name.casefold()==repo_name.casefold():
        git_dir=str(legacy.get('sourceRepository') or '')
        if git_dir:
            public_repo_dirs.add(os.path.realpath(git_dir))
if not git_dir or not os.path.isdir(git_dir):
    raise SystemExit(3)
if not release_store:
    release_store=os.path.join(git_dir,'forkmesh-releases')

node=str(main.get('publishOwner') or '')
gateway_node=gateway.get('node') if isinstance(gateway.get('node'),dict) else {}
node=node or str(gateway_node.get('name') or legacy.get('nodeOwner') or '')
node_id=str(gateway_node.get('publicKey') or '')
catalog=legacy.get('catalog') if isinstance(legacy.get('catalog'),dict) else {}
version=str(main.get('version') or catalog.get('version') or '')

git=lambda *args: run(['git','--git-dir='+git_dir,*args])
branch=git('symbolic-ref','--short','HEAD')
commit=git('rev-parse','--verify','HEAD')
subject=git('show','-s','--format=%s','HEAD') if commit else ''
author=git('show','-s','--format=%an','HEAD') if commit else ''
committed_at=git('show','-s','--format=%ct','HEAD') if commit else ''
commit_count=git('rev-list','--count','HEAD') if commit else ''
branch_rows=git('for-each-ref','--format=%(refname:strip=2) %(objectname)','refs/heads')
branch_heads={}
for row in branch_rows.splitlines()[:2048]:
    try:
        name,oid=row.rsplit(' ',1)
        is_internal=(name=='forkmesh/pulls' or name.startswith('forkmesh/pull/'))
        if name and not is_internal and len(oid) in (40,64):
            branch_heads[name]=oid.lower()
    except Exception:
        pass
all_ref_rows=git('for-each-ref','--format=%(objectname) %(refname)',
                 'refs/heads','refs/tags').splitlines()
all_ref_rows=sorted((row.strip() for row in all_ref_rows if row.strip()),
                    key=lambda row:row.rsplit(' ',1)[-1])
all_refs_sha256=hashlib.sha256('\n'.join(all_ref_rows).encode('utf-8')).hexdigest()
paths=git('ls-tree','-r','--name-only','HEAD','--','.forkmesh').splitlines() if commit else []
pull_ref=git('rev-parse','--verify','--quiet','refs/heads/forkmesh/pulls')
if pull_ref:
    paths+=git('ls-tree','-r','--name-only','refs/heads/forkmesh/pulls','--',
               '.forkmesh/pulls').splitlines()
paths=list(dict.fromkeys(paths))

issues=sum(1 for p in paths if p.startswith('.forkmesh/issues/open/') and p.endswith('.json'))
pulls=0
discussions=0
for path in paths:
    parts=path.split('/')
    if ((len(parts)==4 and parts[:2]==['.forkmesh','pulls'] and
         parts[2].isdigit() and parts[3]=='pull.md') or
        (path.startswith('.forkmesh/pulls/open/') and path.endswith('.json'))):
        pulls+=1
    if (path.startswith('.forkmesh/discussions/') and
            (path.endswith('/discussion.md') or path.endswith('/discussion.json'))):
        discussions+=1

# Artifacts are the physical content-addressed objects this host can serve,
# not Git metadata records.  Keep this collector aligned with the native node
# catalog so a directly inspected mirror never replaces a valid CAS count with
# zero merely because release bytes intentionally live outside Git.
artifacts=-1
release_root=os.path.join(release_store,'sha256')
try:
    if not os.path.exists(release_root):
        artifacts=0
    else:
        root_info=os.lstat(release_root)
        if not stat.S_ISDIR(root_info.st_mode) or stat.S_ISLNK(root_info.st_mode):
            raise ValueError('unsafe release store')
        count=0
        visited=0
        hexdigits=set('0123456789abcdef')
        with os.scandir(release_root) as shards:
            for shard in shards:
                visited+=1
                if visited>4096:
                    raise ValueError('release store is too large')
                if (len(shard.name)!=2 or not set(shard.name)<=hexdigits or
                        not shard.is_dir(follow_symlinks=False)):
                    continue
                with os.scandir(shard.path) as blobs:
                    for blob in blobs:
                        visited+=1
                        if visited>4096:
                            raise ValueError('release store is too large')
                        if (len(blob.name)!=64 or
                                not set(blob.name)<=hexdigits or
                                not blob.name.startswith(shard.name) or
                                not blob.is_dir(follow_symlinks=False)):
                            continue
                        try:
                            data_info=os.lstat(os.path.join(blob.path,'data'))
                            if (stat.S_ISREG(data_info.st_mode) and
                                    not stat.S_ISLNK(data_info.st_mode)):
                                count+=1
                        except OSError:
                            pass
        artifacts=count
except Exception:
    pass

size_text=run(['du','-sb','--',git_dir])
try:
    size_bytes=int(size_text.split()[0])
except Exception:
    size_bytes=-1

cpu=-1
def cpu_sample():
    with open('/proc/stat','r',encoding='ascii') as handle:
        fields=[int(v) for v in handle.readline().split()[1:]]
    return sum(fields),fields[3]+(fields[4] if len(fields)>4 else 0)
try:
    total1,idle1=cpu_sample(); time.sleep(.08); total2,idle2=cpu_sample()
    cpu=round(100*(1-(idle2-idle1)/max(1,total2-total1)))
    cpu=max(0,min(100,cpu))
except Exception:
    pass

mem_used=mem_total=-1
try:
    memory={}
    with open('/proc/meminfo','r',encoding='ascii') as handle:
        for line in handle:
            key,value=line.split(':',1)
            memory[key]=int(value.strip().split()[0])*1024
    mem_total=memory.get('MemTotal',-1)
    mem_used=mem_total-memory.get('MemAvailable',memory.get('MemFree',0))
except Exception:
    pass
try:
    disk=shutil.disk_usage(git_dir)
    disk_used,disk_total=disk.used,disk.total
except Exception:
    disk_used=disk_total=-1

last_sync=0
for stamp in ('FETCH_HEAD','packed-refs'):
    try:
        last_sync=max(last_sync,round(os.path.getmtime(os.path.join(git_dir,stamp))*1000))
    except Exception:
        pass

clones=website=-1
gateway_config=str(main.get('gatewayConfig') or legacy.get('gatewayConfigPath') or '')
counters=read_json(os.path.join(os.path.dirname(gateway_config),
                                'service-counters.json'),1024*1024)
rows=counters.get('repositories') if isinstance(counters.get('repositories'),dict) else {}
counter=rows.get(owner+'/'+repo_name) if isinstance(rows,dict) else None
if isinstance(counter,dict):
    try: clones=int(counter.get('clonesServed',-1))
    except Exception: pass
    try: website=int(counter.get('websiteServed',-1))
    except Exception: pass

def integer(value,scale=1):
    try: return int(value)*scale
    except Exception: return -1

out={
    'node':node,'id':node_id,'status':'ssh','sshHealthy':True,
    'metadataSource':'ssh','owner':owner,'repository':repo_name,
    'repositoryCount':len(public_repo_dirs),
    'commit':commit,'branch':branch,'lastCommitMessage':subject,
    'lastCommitAuthorName':author,'lastCommitAt':integer(committed_at,1000),
    'lastSync':last_sync,'sizeBytes':size_bytes,'issueCount':issues,
    'commitCount':integer(commit_count),'branchCount':len(branch_heads),
    'branchHeads':branch_heads,'allRefsSha256':all_refs_sha256,
    'pullCount':pulls,'discussionCount':discussions,'artifactCount':artifacts,
    'cpuPercent':cpu,'memUsedBytes':mem_used,'memTotalBytes':mem_total,
    'diskUsedBytes':disk_used,'diskTotalBytes':disk_total,
    'platform':platform.system().lower() or sys.platform,'version':version,
    'clonesServed':clones,'websiteServed':website,
    'sshFetchedAt':round(time.time()*1000)
}
print(json.dumps(out,separators=(',',':'),ensure_ascii=True))
)PY");
}

bool verifiedRepositoryHealth(const QJsonObject &health,
                              const QString &expectedNode,
                              const QString &owner,
                              const QString &repo,
                              const QString &nonce,
                              qint64 issuedAt,
                              QJsonObject *verifiedProof = nullptr)
{
    const QJsonObject proof =
        health.value(QStringLiteral("repositoryProof")).toObject();
    const QJsonObject challenge =
        health.value(QStringLiteral("challenge")).toObject();
    const QString node =
        health.value(QStringLiteral("node")).toString().trimmed().toLower();
    const QByteArray signedMessage =
        QByteArrayLiteral("forkmesh-https-health-repository-v1\n") +
        node.toUtf8() + '\n' + nonce.toUtf8() + '\n' +
        QByteArray::number(issuedAt) + '\n' +
        owner.trimmed().toLower().toUtf8() + '\n' +
        repo.trimmed().toUtf8() + QByteArrayLiteral("\n1\n") +
        proof.value(QStringLiteral("integrity")).toString().toUtf8() + '\n' +
        proof.value(QStringLiteral("refsSha256")).toString().toUtf8() + '\n' +
        proof.value(QStringLiteral("operationsSha256")).toString().toUtf8();
    const QString messageSha256 = QString::fromLatin1(
        QCryptographicHash::hash(signedMessage, QCryptographicHash::Sha256)
            .toHex());
    const bool valid =
        node == expectedNode.trimmed().toLower() &&
        proof.value(QStringLiteral("owner")).toString().compare(
            owner, Qt::CaseInsensitive) == 0 &&
        proof.value(QStringLiteral("repository")).toString().compare(
            repo, Qt::CaseInsensitive) == 0 &&
        proof.value(QStringLiteral("available")).toBool() &&
        proof.value(QStringLiteral("integrity")).toString() ==
            QLatin1String("ok") &&
        challenge.value(QStringLiteral("messageType")).toString() ==
            QLatin1String("forkmesh-https-health-repository-v1") &&
        challenge.value(QStringLiteral("nonce")).toString() == nonce &&
        qint64(challenge.value(QStringLiteral("issuedAt")).toDouble()) ==
            issuedAt &&
        challenge.value(QStringLiteral("messageSha256")).toString() ==
            messageSha256 &&
        ForkMeshIdentity::verifySignature(
            health.value(QStringLiteral("publicKey")).toString(),
            challenge.value(QStringLiteral("signature")).toString(),
            signedMessage);
    if (valid && verifiedProof)
        *verifiedProof = proof;
    return valid;
}

QString mirrorDiagnosticPrompt(const QString &source, const QString &nodeName,
                               const QString &nodeId, const QString &owner,
                               const QString &platform, const QString &version,
                               const QList<NodeDiagnostics::Finding> &findings,
                               qint64 diagnosticsMs)
{
    QStringList identity;
    identity << QStringLiteral("Node: %1").arg(
        nodeName.trimmed().isEmpty() ? QStringLiteral("(unnamed)") : nodeName);
    if (!nodeId.trimmed().isEmpty())
        identity << QStringLiteral("Node id: %1").arg(nodeId.trimmed());
    if (!owner.trimmed().isEmpty())
        identity << QStringLiteral("Owner: %1").arg(owner.trimmed());
    if (!platform.trimmed().isEmpty())
        identity << QStringLiteral("Platform: %1").arg(platform.trimmed());
    if (!version.trimmed().isEmpty())
        identity << QStringLiteral("ForkMesh version: %1").arg(version.trimmed());

    QString report;
    if (diagnosticsMs <= 0) {
        report = QStringLiteral(
            "No self-diagnostic report has been received. Determine whether this "
            "node is on an older build, has self-diagnostics disabled, or cannot "
            "deliver heartbeat diagnostics, and restore reporting before judging "
            "the node healthy.");
    } else {
        report = QStringLiteral("Reported at: %1\n")
                     .arg(QDateTime::fromMSecsSinceEpoch(diagnosticsMs)
                              .toLocalTime()
                              .toString(Qt::ISODate));
        report += findings.isEmpty()
                      ? QStringLiteral("The self-check reported no findings. Verify "
                                       "the suspected failure independently and add "
                                       "diagnostic coverage if it was missed.")
                      : NodeDiagnostics::detailText(findings);
    }

    const QString repoName = source.trimmed().isEmpty()
                                 ? QStringLiteral("the open repository")
                                 : source.trimmed();
    return QStringLiteral(
               "Investigate and fix the mirror-node diagnostics below for %1. "
               "Find the root cause rather than suppressing the check. If the "
               "cause is in ForkMesh, implement the fix and add or update tests; "
               "if it is host configuration, give exact safe remediation steps. "
               "Afterward, rerun or wait for the self-check and verify the finding "
               "clears without breaking mirror sync, serving, or relay connectivity.\n\n"
               "%2\nRepository mirrored: %1\n\nDiagnostic report:\n%3")
        .arg(repoName, identity.join(QLatin1Char('\n')), report);
}

// One version stamp inside a file: capture group 2 of `pattern` is the version
// text in place today, and `rewrite` turns the release version into whatever
// replaces it.
struct ReleaseVersionStamp {
    QRegularExpression pattern;
    std::function<QString(const QString &current, const QString &version)> rewrite;
};

struct ReleaseVersionFile {
    QString relativePath;
    QList<ReleaseVersionStamp> stamps;
};

// Android versionCode for a release, using the same scheme
// tools/forkmesh-release-host.py stamps into the published APK:
// MAJOR*10000 + MINOR*100 + PATCH. Returns 0 when the version does not fit it.
int releaseAndroidVersionCode(const QString &version)
{
    const QStringList parts = version.split(QLatin1Char('.'));
    if (parts.size() != 3)
        return 0;
    const int minor = parts.at(1).toInt();
    const int patch = parts.at(2).toInt();
    if (minor >= 100 || patch >= 100)
        return 0;
    return parts.at(0).toInt() * 10000 + minor * 100 + patch;
}

// Every file that carries the ForkMesh release version: the desktop project
// version and the native Android app.
QList<ReleaseVersionFile> releaseVersionFiles()
{
    const auto releaseVersion = [](const QString &, const QString &version) {
        return version;
    };
    return {
        {QStringLiteral("desktop/CMakeLists.txt"),
         {{QRegularExpression(QStringLiteral(
                                  "^(project\\(ForkMesh VERSION )([0-9]+\\.[0-9]+\\.[0-9]+)"),
                              QRegularExpression::MultilineOption),
           releaseVersion}}},
        {QStringLiteral("native/android/app/build.gradle.kts"),
         {{QRegularExpression(QStringLiteral(
                                  "^(\\s*versionName = versionNameOverride\\.ifEmpty \\x7B \")([0-9]+\\.[0-9]+\\.[0-9]+)\""),
                              QRegularExpression::MultilineOption),
           releaseVersion},
          {QRegularExpression(QStringLiteral("^(\\s*versionCode = versionCodeOverride \\?: )([0-9]+)"),
                              QRegularExpression::MultilineOption),
           [](const QString &current, const QString &version) {
               const int code = releaseAndroidVersionCode(version);
               return code > 0 ? QString::number(code) : current;
           }}}},
    };
}

// The release version a tag stamps: "v1.2.3" becomes "1.2.3". Empty for a tag
// that is not a plain MAJOR.MINOR.PATCH release, which stamps nothing.
QString releaseStampVersion(const QString &tag)
{
    QString version = tag.trimmed();
    if (version.startsWith(QLatin1Char('v')))
        version = version.mid(1);
    static const QRegularExpression semver(
        QStringLiteral("\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z"));
    return semver.match(version).hasMatch() ? version : QString();
}

// Rewrite every stamp of `file` inside `text`. Returns whether anything
// changed; `missed` gets a note when a stamp's pattern no longer matches.
bool stampReleaseVersionText(const ReleaseVersionFile &file,
                             const QString &version, QString *text,
                             QStringList *missed)
{
    bool changed = false;
    for (const ReleaseVersionStamp &stamp : file.stamps) {
        // Matched against the running text, so an earlier stamp's
        // replacement cannot leave this one with stale offsets.
        const QRegularExpressionMatch m = stamp.pattern.match(*text);
        if (!m.hasMatch()) {
            if (missed)
                *missed << QStringLiteral("%1 (version line not found)")
                               .arg(file.relativePath);
            continue; // the source-text test guards this
        }
        const QString current = m.captured(2);
        const QString next = stamp.rewrite(current, version);
        if (next.isEmpty() || next == current)
            continue;
        text->replace(m.capturedStart(2), m.capturedLength(2), next);
        changed = true;
    }
    return changed;
}

QString releaseVersionCommitMessage(const QString &version)
{
    return QStringLiteral("release: bump version to %1").arg(version);
}

const QStringList kReleaseCommitIdentity{
    QStringLiteral("-c"), QStringLiteral("user.email=actions@forkmesh.local"),
    QStringLiteral("-c"), QStringLiteral("user.name=ForkMesh Actions")};

// Stamp the release version into every file that carries one — desktop and
// Android alike — and commit them together, ahead of the release tag. Returns
// the paths that changed; a file is skipped when it already carries the
// release version, is absent from this checkout, or has uncommitted edits that
// must not ride along in the release commit. Every skip that leaves a stale
// version behind is named in `skipped`.
QStringList stampVersionForRelease(const QString &workTree, const QString &tag,
                                   QStringList *skipped)
{
    const QString version = releaseStampVersion(tag);
    if (workTree.isEmpty() || version.isEmpty())
        return {};

    QStringList stamped;
    QList<std::pair<QString, QString>> rewrites;
    const QList<ReleaseVersionFile> files = releaseVersionFiles();
    for (const ReleaseVersionFile &file : files) {
        QByteArray pending;
        if (!runGitCapture(workTree,
                           {"status", "--porcelain", "--", file.relativePath},
                           &pending, nullptr)) {
            *skipped << QStringLiteral("%1 (git status failed)")
                            .arg(file.relativePath);
            continue;
        }
        if (!QString::fromUtf8(pending).trimmed().isEmpty()) {
            *skipped << QStringLiteral("%1 (uncommitted edits)")
                            .arg(file.relativePath);
            continue;
        }

        QFile handle(workTree + QLatin1Char('/') + file.relativePath);
        if (!handle.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QString text = QString::fromUtf8(handle.readAll());
        handle.close();

        if (!stampReleaseVersionText(file, version, &text, skipped))
            continue;
        if (!handle.open(QIODevice::WriteOnly | QIODevice::Truncate |
                         QIODevice::Text)) {
            *skipped << QStringLiteral("%1 (not writable)").arg(file.relativePath);
            continue;
        }
        const bool wrote = handle.write(text.toUtf8()) >= 0;
        handle.close();
        if (wrote)
            stamped << file.relativePath;
    }
    if (stamped.isEmpty())
        return {};

    QStringList args = kReleaseCommitIdentity;
    args << "commit" << "-m" << releaseVersionCommitMessage(version)
         << "--";
    args += stamped;
    if (!runGitCapture(workTree, args, nullptr, nullptr)) {
        *skipped << QStringLiteral("the version commit (git commit failed)");
        return {};
    }
    return stamped;
}

// The worktree of `repoDir` that has `branch` checked out, or empty.
QString worktreeForBranch(const QString &repoDir, const QString &branch)
{
    QByteArray out;
    if (!runGitCapture(repoDir, {"worktree", "list", "--porcelain"}, &out,
                       nullptr))
        return {};
    const QString wanted = QStringLiteral("branch refs/heads/") + branch;
    QString path;
    for (const QByteArray &raw : out.split('\n')) {
        const QString line = QString::fromUtf8(raw);
        if (line.startsWith(QStringLiteral("worktree ")))
            path = line.mid(9);
        else if (line == wanted)
            return path;
    }
    return {};
}

// The same stamp for a local branch no worktree has checked out: the files
// are read from the branch tip and the version commit is built in a throwaway
// no-checkout worktree (only its index is used; no file is checked out), then
// the branch is advanced with a compare-and-swap so a concurrent push is never
// overwritten.
QStringList stampVersionOnBranch(const QString &repoDir, const QString &branch,
                                 const QString &tag, QStringList *skipped)
{
    const QString version = releaseStampVersion(tag);
    if (repoDir.isEmpty() || version.isEmpty())
        return {};
    const QString ref = QStringLiteral("refs/heads/") + branch;
    QByteArray tipOut;
    if (!runGitCapture(repoDir, {"rev-parse", "--verify", ref + "^{commit}"},
                       &tipOut, nullptr)) {
        *skipped << QStringLiteral("every file (%1 is not a local branch)")
                        .arg(branch);
        return {};
    }
    const QString tip = QString::fromUtf8(tipOut).trimmed();

    QStringList stamped;
    QStringList cacheInfo;
    const QList<ReleaseVersionFile> files = releaseVersionFiles();
    for (const ReleaseVersionFile &file : files) {
        QByteArray entry;
        if (!runGitCapture(repoDir, {"ls-tree", tip, "--", file.relativePath},
                           &entry, nullptr) ||
            entry.trimmed().isEmpty())
            continue; // absent from this branch
        const QString mode =
            QString::fromUtf8(entry).section(QLatin1Char(' '), 0, 0);
        QByteArray blob;
        if (!runGitCapture(repoDir,
                           {"cat-file", "blob", tip + ":" + file.relativePath},
                           &blob, nullptr))
            continue;
        QString text = QString::fromUtf8(blob);
        if (!stampReleaseVersionText(file, version, &text, skipped))
            continue;
        QByteArray hash;
        if (!runGitCaptureInput(repoDir,
                                {"hash-object", "-w", "--no-filters", "--stdin"},
                                text.toUtf8(), &hash, nullptr)) {
            *skipped << QStringLiteral("%1 (could not write the blob)")
                            .arg(file.relativePath);
            continue;
        }
        cacheInfo << QStringLiteral("%1,%2,%3")
                         .arg(mode, QString::fromUtf8(hash).trimmed(),
                              file.relativePath);
        stamped << file.relativePath;
    }
    if (stamped.isEmpty())
        return {};

    QTemporaryDir scratch;
    if (!scratch.isValid()) {
        *skipped << QStringLiteral("every file (no scratch directory)");
        return {};
    }
    const QString scratchTree = scratch.path() + QStringLiteral("/stamp");
    QString commit;
    if (runGitCapture(repoDir, {"worktree", "add", "--detach", "--no-checkout",
                                scratchTree, tip},
                      nullptr, nullptr)) {
        bool ok =
            runGitCapture(scratchTree, {"read-tree", tip}, nullptr, nullptr);
        for (const QString &info : std::as_const(cacheInfo))
            ok = ok && runGitCapture(scratchTree,
                                     {"update-index", "--cacheinfo", info},
                                     nullptr, nullptr);
        QByteArray tree;
        ok = ok && runGitCapture(scratchTree, {"write-tree"}, &tree, nullptr);
        QByteArray made;
        QStringList args = kReleaseCommitIdentity;
        args << "commit-tree" << QString::fromUtf8(tree).trimmed() << "-p" << tip
             << "-m" << releaseVersionCommitMessage(version);
        if (ok && runGitCapture(scratchTree, args, &made, nullptr))
            commit = QString::fromUtf8(made).trimmed();
        runGitCapture(repoDir, {"worktree", "remove", "--force", scratchTree},
                      nullptr, nullptr);
    }
    QString err;
    if (commit.isEmpty() ||
        !runGitCapture(repoDir, {"update-ref", "-m",
                                 QStringLiteral("release: stamp %1").arg(tag),
                                 ref, commit, tip},
                       nullptr, &err)) {
        *skipped << QStringLiteral("every file (%1)").arg(
            err.isEmpty() ? QStringLiteral("could not commit to %1").arg(branch)
                          : err);
        return {};
    }
    return stamped;
}

// Stamp the release version wherever `branch` lives: its own worktree when one
// has it checked out (a ref moved under a checkout would show the stamp as a
// revert there), otherwise straight onto the branch.
QStringList stampReleaseTarget(const QString &repoDir, const QString &branch,
                               const QString &tag, QStringList *skipped)
{
    const QString checkout = worktreeForBranch(repoDir, branch);
    if (!checkout.isEmpty())
        return stampVersionForRelease(checkout, tag, skipped);
    return stampVersionOnBranch(repoDir, branch, tag, skipped);
}

// A relay 404 for a release blob is an answer, not a fault: no hosting node
// has those bytes (candidate-1.2.2's uploads never landed anywhere). The
// in-memory backoff forgot that on every restart, so each launch and each
// repository open asked again. The miss is remembered as a marker file whose
// mtime is the last 404 -- outside the served CAS, which the gateway and the
// mirror push walk -- and the blob is skipped until a day has passed.
constexpr qint64 kReleaseBlobMissRetryMs = 24LL * 60 * 60 * 1000;

QString releaseBlobMissMarkerPath(const QString &hash)
{
    return QDir(forkmesh::paths::cacheRoot())
        .filePath(QStringLiteral("release-blob-misses/") + hash);
}

bool releaseBlobRecentlyMissing(const QString &hash, qint64 nowMs)
{
    const QFileInfo marker(releaseBlobMissMarkerPath(hash));
    return marker.exists() &&
           nowMs - marker.lastModified().toMSecsSinceEpoch() <
               kReleaseBlobMissRetryMs;
}

void noteReleaseBlobMissing(const QString &hash)
{
    const QString path = releaseBlobMissMarkerPath(hash);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile marker(path);
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
        marker.write(QByteArray::number(QDateTime::currentMSecsSinceEpoch()));
}

struct ArtifactsPanelRead {
    QList<MirrorReleaseBlob> blobs;
    QHash<QString, QString> nameByHash;
    QHash<QString, QString> tagByHash;
};

ArtifactsPanelRead readArtifactsPanel(const QString &mirrorPath)
{
    ArtifactsPanelRead read;
    read.blobs = mirrorReleaseBlobs(mirrorPath);
    if (read.blobs.isEmpty())
        return read;
    const QString branch = mirrorHeadBranch(mirrorPath);
    QByteArray channelsOut;
    if (branch.isEmpty() ||
        !runGitCapture(mirrorPath,
                       {QStringLiteral("ls-tree"), QStringLiteral("-z"),
                        branch + QStringLiteral(":.forkmesh/releases")},
                       &channelsOut, nullptr))
        return read;
    for (const QByteArray &raw : channelsOut.split('\0')) {
        const int tab = raw.indexOf('\t');
        if (tab < 0 || !raw.left(tab).contains(" tree "))
            continue;
        const QString channel = QString::fromUtf8(raw.mid(tab + 1)).trimmed();
        if (channel.isEmpty())
            continue;
        QByteArray manifestOut;
        if (!runGitCapture(mirrorPath,
                           {QStringLiteral("show"),
                            branch + QStringLiteral(":.forkmesh/releases/") + channel +
                                QStringLiteral("/release.json")},
                           &manifestOut, nullptr))
            continue;
        const QJsonObject obj = QJsonDocument::fromJson(manifestOut).object();
        const QString tag = obj.value(QStringLiteral("tag")).toString().trimmed();
        const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue &asset : assets) {
            const QJsonObject a = asset.toObject();
            const QString hash = a.value(QStringLiteral("blob_sha256"))
                                     .toString()
                                     .trimmed()
                                     .toLower();
            if (hash.isEmpty())
                continue;
            const QString name = a.value(QStringLiteral("name")).toString();
            if (!name.isEmpty() && !read.nameByHash.contains(hash))
                read.nameByHash.insert(hash, name);
            if (!tag.isEmpty() && !read.tagByHash.contains(hash))
                read.tagByHash.insert(hash, tag);
        }
    }
    return read;
}

struct ReleaseTagsRead {
    bool ok = false;
    QByteArray out;
};

ReleaseTagsRead readReleaseTags(const QString &dir)
{
    ReleaseTagsRead read;
    if (dir.isEmpty())
        return read;
    read.ok = runGitCapture(dir,
                            {"for-each-ref", "--sort=-creatordate", "refs/tags",
                             "--format=%(refname:short)%1f"
                             "%(creatordate:format:%Y-%m-%d %H:%M)%1f"
                             "%(contents:subject)%1f"
                             "%(*objectname:short)%1f"
                             "%(objectname:short)%1f"
                             "%(taggername)%1f"
                             "%(contents:body)%1e"},
                            &read.out, nullptr);
    return read;
}

struct ReleaseTagPrune {
    bool listed = false;
    int stale = 0;
    int deleted = 0;
    int failed = 0;
};

ReleaseTagPrune pruneReleaseTagsIn(const QString &dir, const QString &keepTag)
{
    ReleaseTagPrune prune;
    QByteArray out;
    if (!runGitCapture(dir,
                       {"for-each-ref", "--format=%(refname:short)", "refs/tags"},
                       &out, nullptr))
        return prune;
    prune.listed = true;
    QStringList staleTags;
    for (const QByteArray &line : out.split('\n')) {
        const QString name = QString::fromUtf8(line).trimmed();
        if (!name.isEmpty() && name != keepTag)
            staleTags << name;
    }
    prune.stale = staleTags.size();
    for (const QString &name : std::as_const(staleTags)) {
        if (runGitCapture(dir, {"tag", "-d", name}, nullptr, nullptr))
            ++prune.deleted;
        else
            ++prune.failed;
    }
    return prune;
}

struct NewReleasePublish {
    bool tagged = false;
    QString err;
    QStringList stamped;
    QStringList skipped;
    QString tip;
};

struct ReleaseDetailRead {
    bool exists = false;
    QString when;
    QString tagger;
    QString subject;
    QString body;
    QString prevTag;
    QString diffHtml;
};

ReleaseDetailRead readReleaseDetail(const QString &dir, const QString &tag,
                                    bool split,
                                    const DiffRenderAppearance &appearance)
{
    ReleaseDetailRead read;
    if (!runGitCapture(dir, {"rev-parse", "--verify", "--quiet",
                             QStringLiteral("refs/tags/") + tag},
                       nullptr, nullptr))
        return read;
    read.exists = true;

    QByteArray meta;
    runGitCapture(dir,
                  {"for-each-ref",
                   "--format=%(creatordate:format:%Y-%m-%d %H:%M)%1f%(taggername)"
                   "%1f%(contents:subject)%1f%(contents:body)",
                   QStringLiteral("refs/tags/") + tag},
                  &meta, nullptr);
    const QStringList mf = QString::fromUtf8(meta).split(QLatin1Char('\x1f'));
    read.when = mf.value(0).trimmed();
    read.tagger = mf.value(1).trimmed();
    read.subject = mf.value(2).trimmed();
    read.body = mf.value(3).trimmed();

    QByteArray tagsOut;
    if (runGitCapture(dir,
                      {"for-each-ref", "--sort=-creatordate",
                       "--format=%(refname:short)", "refs/tags"},
                      &tagsOut, nullptr)) {
        const QStringList all = QString::fromUtf8(tagsOut).split(QLatin1Char('\n'));
        int idx = -1;
        for (int i = 0; i < all.size(); ++i)
            if (all.at(i).trimmed() == tag) {
                idx = i;
                break;
            }
        for (int j = idx + 1; idx >= 0 && j < all.size(); ++j) {
            const QString cand = all.at(j).trimmed();
            if (!cand.isEmpty()) {
                read.prevTag = cand;
                break;
            }
        }
    }

    if (!read.prevTag.isEmpty()) {
        QByteArray patchRaw;
        runGitCapture(dir, {"diff", "-M", read.prevTag, tag}, &patchRaw, nullptr);
        QList<DiffFileEntry> files;
        read.diffHtml = renderDiffHtmlSplit(split, QString::fromUtf8(patchRaw),
                                            files, dir, read.prevTag, tag,
                                            QString(), {}, {}, {}, appearance);
    }
    return read;
}

} // namespace


QWidget *MainWindow::buildReleasesTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(10);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel("Releases");
    heading->setObjectName("channelTitle");
    m_releasesSummary = new QLabel;
    m_releasesSummary->setObjectName("statusLine");
    auto *newButton = new QPushButton("Draft a release");
    newButton->setObjectName("primaryButton");
    newButton->setCursor(Qt::PointingHandCursor);
    setOcticon(newButton, "tag", 16);
    connect(newButton, &QPushButton::clicked, this, &MainWindow::promptNewRelease);
    auto *refreshButton = new QPushButton("Refresh");
    refreshButton->setObjectName("ghostButton");
    refreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(refreshButton, "sync", 16);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::loadReleasesPanel);
    addRefreshSpin(refreshButton);
    headerRow->addWidget(heading);
    headerRow->addWidget(m_releasesSummary);
    headerRow->addStretch();
    headerRow->addWidget(refreshButton);
    headerRow->addWidget(newButton);
    layout->addLayout(headerRow);

    m_releasesTable = new QTableWidget(0, 11);
    installColumnHeaderMenu(m_releasesTable);
    m_releasesTable->setObjectName("issueTable");
    // Compare and artifact cells are link labels; one rule here instead of a
    // sheet per row every time the panel reloads.
    m_releasesTable->setStyleSheet(Theme::remapSheet(
        QStringLiteral("QLabel#releaseCellLink { background:transparent; }")));
    enableHoverRowHighlight(m_releasesTable);
    m_releasesTable->setHorizontalHeaderLabels(
        {"Tag", "Commit", "Released", "Release notes", "Compare", "Artifacts",
         "Size", "SHA-256", "Downloads", "Mirrors", ""});
    m_releasesTable->verticalHeader()->setVisible(false);
    m_releasesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_releasesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_releasesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_releasesTable->setShowGrid(false);
    m_releasesTable->setWordWrap(false);
    QHeaderView *rh = m_releasesTable->horizontalHeader();
    rh->setHighlightSections(false);
    rh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(3, QHeaderView::Stretch);
    rh->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(5, QHeaderView::Stretch);
    rh->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(8, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(9, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(10, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_releasesTable);
    connect(m_releasesTable, &QTableWidget::itemActivated, this,
            [this](QTableWidgetItem *item) {
                QTableWidgetItem *it =
                    item ? m_releasesTable->item(item->row(), 0) : nullptr;
                if (it && !it->text().trimmed().isEmpty())
                    showReleaseDetail(it->text().trimmed());
            });
    layout->addWidget(m_releasesTable, 1);
    return page;
}


QWidget *MainWindow::buildArtifactsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(10);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    auto *heading = new QLabel("Artifacts");
    heading->setObjectName("channelTitle");
    m_artifactsSummary = new QLabel;
    m_artifactsSummary->setObjectName("statusLine");
    auto *refreshButton = new QPushButton("Refresh");
    refreshButton->setObjectName("ghostButton");
    refreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(refreshButton, "sync", 16);
    connect(refreshButton, &QPushButton::clicked, this,
            &MainWindow::loadArtifactsPanel);
    addRefreshSpin(refreshButton);
    headerRow->addWidget(heading);
    headerRow->addWidget(m_artifactsSummary);
    headerRow->addStretch();
    headerRow->addWidget(refreshButton);
    layout->addLayout(headerRow);

    auto *hint = new QLabel(
        "Release binaries this node hosts for download, stored out of git in its "
        "content-addressed release store. Deleting one frees its disk space; a peer "
        "that still holds it can re-seed this node on the next sync.");
    hint->setObjectName("statusLine");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_artifactsTable = new QTableWidget(0, 5);
    installColumnHeaderMenu(m_artifactsTable);
    m_artifactsTable->setObjectName("issueTable");
    enableHoverRowHighlight(m_artifactsTable);
    m_artifactsTable->setHorizontalHeaderLabels(
        {"Artifact", "Release", "Size", "Checksum", ""});
    m_artifactsTable->verticalHeader()->setVisible(false);
    m_artifactsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_artifactsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_artifactsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_artifactsTable->setShowGrid(false);
    m_artifactsTable->setWordWrap(false);
    QHeaderView *rh = m_artifactsTable->horizontalHeader();
    rh->setHighlightSections(false);
    rh->setSectionResizeMode(0, QHeaderView::Stretch);
    rh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    rh->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_artifactsTable);
    layout->addWidget(m_artifactsTable, 1);
    return page;
}

QString MainWindow::artifactsMirrorRequest() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    return QString::number(m_repoDetailIndex) + QLatin1Char('|') +
           repo.mirrorPath + QLatin1Char('|') +
           (repo.previewOnly ? QLatin1Char('p') : QLatin1Char('w'));
}

void MainWindow::loadArtifactsPanel()
{
    if (!m_artifactsTable)
        return;
    const QString request = artifactsMirrorRequest();
    if (request.isEmpty()) {
        ++m_artifactsLoadGen;
        m_artifactsLoading = false;
        m_artifactsReloadPending = false;
        m_artifactsShownRequest.clear();
        TableRepaintGuard repaintGuard(m_artifactsTable);
        m_artifactsTable->setRowCount(0);
        if (m_artifactsSummary)
            m_artifactsSummary->clear();
        return;
    }
    if (m_artifactsLoading && request == m_artifactsLoadRequest) {
        m_artifactsReloadPending = true;
        return;
    }
    if (request != m_artifactsShownRequest) {
        m_artifactsShownRequest.clear();
        TableRepaintGuard repaintGuard(m_artifactsTable);
        m_artifactsTable->setRowCount(0);
        if (m_artifactsSummary)
            m_artifactsSummary->clear();
    }
    const QString mirrorPath = m_repositories.at(m_repoDetailIndex).mirrorPath;
    const bool previewOnly = m_repositories.at(m_repoDetailIndex).previewOnly;
    const quint64 generation = ++m_artifactsLoadGen;
    m_artifactsLoading = true;
    m_artifactsLoadRequest = request;
    m_artifactsReloadPending = false;
    struct ArtifactsLoad {
        ArtifactsPanelRead read;
        bool mirrorExists = false;
    };
    runOffThread<ArtifactsLoad>(
        [mirrorPath] {
            ArtifactsLoad load;
            load.mirrorExists = !mirrorPath.isEmpty() && QDir(mirrorPath).exists();
            load.read = readArtifactsPanel(mirrorPath);
            return load;
        },
        [this, generation, request, previewOnly](ArtifactsLoad load) {
            if (generation != m_artifactsLoadGen)
                return;
            m_artifactsLoading = false;
            const bool reloadPending = m_artifactsReloadPending;
            m_artifactsReloadPending = false;
            if (!m_artifactsTable)
                return;
            if (request != artifactsMirrorRequest()) {
                loadArtifactsPanel();
                return;
            }
            m_artifactsShownRequest = request;
            const bool writable = !previewOnly && load.mirrorExists;
            fillArtifactsTable(load.read.blobs, load.read.nameByHash,
                               load.read.tagByHash, writable);
            if (reloadPending)
                loadArtifactsPanel();
        });
}

void MainWindow::fillArtifactsTable(const QList<MirrorReleaseBlob> &blobs,
                                    const QHash<QString, QString> &nameByHash,
                                    const QHash<QString, QString> &tagByHash,
                                    bool writable)
{
    TableRepaintGuard repaintGuard(m_artifactsTable);
    m_artifactsTable->setRowCount(0);
    if (m_artifactsSummary)
        m_artifactsSummary->clear();

    qint64 totalBytes = 0;
    for (const MirrorReleaseBlob &blob : blobs) {
        const int row = m_artifactsTable->rowCount();
        m_artifactsTable->insertRow(row);
        totalBytes += blob.size;

        const QString name = nameByHash.value(blob.hash);
        const QString tag = tagByHash.value(blob.hash);
        auto *nameItem = new QTableWidgetItem(
            name.isEmpty() ? QStringLiteral("(unreferenced blob)") : name);
        nameItem->setIcon(themedOcticon("package", Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")), 14));
        if (name.isEmpty())
            nameItem->setToolTip(QStringLiteral(
                "No release manifest references this blob — safe to delete to "
                "reclaim its space."));
        m_artifactsTable->setItem(row, 0, nameItem);
        m_artifactsTable->setItem(
            row, 1,
            new QTableWidgetItem(tag.isEmpty() ? QStringLiteral("—") : tag));

        auto *sizeItem =
            new QTableWidgetItem(QLocale().formattedDataSize(blob.size));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_artifactsTable->setItem(row, 2, sizeItem);

        auto *hashItem = new QTableWidgetItem(
            QStringLiteral("sha256:%1").arg(blob.hash.left(12)));
        hashItem->setToolTip(QStringLiteral("sha256:%1").arg(blob.hash));
        m_artifactsTable->setItem(row, 3, hashItem);

        auto *del = new QPushButton;
        del->setObjectName("issueIconButton");
        del->setFlat(true);
        del->setCursor(Qt::PointingHandCursor);
        del->setIcon(themedOcticon("trash", QColor("#f85149"), 15));
        del->setIconSize(QSize(15, 15));
        del->setToolTip(QStringLiteral("Delete this artifact from disk"));
        del->setEnabled(writable);
        const QString hash = blob.hash;
        const QString label = name.isEmpty() ? blob.hash.left(12) : name;
        connect(del, &QPushButton::clicked, this,
                [this, hash, label] { deleteArtifact(hash, label); });
        m_artifactsTable->setCellWidget(row, 4, del);
    }

    if (m_artifactsSummary) {
        if (blobs.isEmpty())
            m_artifactsSummary->setText(QStringLiteral("(none)"));
        else
            m_artifactsSummary->setText(
                QStringLiteral("(%1 · %2)")
                    .arg(blobs.size())
                    .arg(QLocale().formattedDataSize(totalBytes)));
    }
}

void MainWindow::deleteArtifact(const QString &hash, const QString &label)
{
    if (hash.isEmpty() || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const QString mirrorPath = m_repositories.at(m_repoDetailIndex).mirrorPath;
    if (QMessageBox::question(
            this, "Delete artifact",
            QStringLiteral("Delete artifact \"%1\" from this node's release store? "
                           "This frees its disk space and cannot be undone, but a "
                           "peer that still holds it can re-seed this node.")
                .arg(label),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    const QFileInfo info(mirrorReleaseBlobPath(mirrorPath, hash));
    QDir hashDir = info.absoluteDir();
    if (mirrorPath.trimmed().isEmpty() ||
        !forkmesh::safedelete::removeTree(hashDir.absolutePath(),
                                          mirrorReleaseCasRoot(mirrorPath))) {
        setRepoDetailNotice(
            QStringLiteral("Could not delete artifact %1.").arg(label), true);
        return;
    }
    QDir shardDir = hashDir;
    if (shardDir.cdUp() && shardDir.isEmpty())
        shardDir.rmdir(QStringLiteral("."));
    logSystem(
        QStringLiteral("Artifacts: deleted %1 (sha256:%2) from the release store.")
            .arg(label, hash.left(12)));
    setRepoDetailNotice(QStringLiteral("Deleted artifact %1.").arg(label));
    loadArtifactsPanel();
    if (mirrorNodesOnScreen())
        loadMirrorNodesPanel();
}

void MainWindow::pruneReleaseArtifacts(
    int index, const QString &releaseTag, const QStringList &keepHashes)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    const QString mirrorPath = repo.mirrorPath;
    if (mirrorPath.trimmed().isEmpty() || !QDir(mirrorPath).exists()) {
        if (index == m_repoDetailIndex) {
            setRepoDetailNotice(
                QStringLiteral("Release %1 published, but no local artifact store was found to prune.")
                    .arg(releaseTag),
                true);
        }
        return;
    }

    const QList<MirrorReleaseBlob> blobs = mirrorReleaseBlobs(mirrorPath);
    if (blobs.isEmpty()) {
        logSystem(QStringLiteral("Artifacts: no previous release artifacts to delete for %1.")
                      .arg(releaseTag));
        return;
    }

    QSet<QString> retained;
    for (const QString &hash : keepHashes)
        retained.insert(hash.trimmed().toLower());

    int deleted = 0;
    int failed = 0;
    qint64 bytesDeleted = 0;
    // code-health: allow(dir-walk-in-loop) each hash dir holds one data blob; only superseded artifacts are pruned, once per publish
    for (const MirrorReleaseBlob &blob : blobs) {
        if (retained.contains(blob.hash.toLower()))
            continue;
        const QFileInfo info(blob.path);
        QDir hashDir = info.absoluteDir();
        if (forkmesh::safedelete::removeTree(hashDir.absolutePath(),
                                             mirrorReleaseCasRoot(mirrorPath))) {
            ++deleted;
            bytesDeleted += blob.size;
            QDir shardDir = hashDir;
            if (shardDir.cdUp() && shardDir.isEmpty())
                shardDir.rmdir(QStringLiteral("."));
        } else {
            ++failed;
        }
    }

    if (deleted > 0) {
        logSystem(
            QStringLiteral("Artifacts: deleted %1 previous release artifact%2 (%3) after publishing %4.")
                .arg(deleted)
                .arg(deleted == 1 ? QString() : QStringLiteral("s"))
                .arg(QLocale().formattedDataSize(bytesDeleted), releaseTag));
    }
    if (failed > 0) {
        if (index == m_repoDetailIndex) {
            setRepoDetailNotice(
                QStringLiteral("Published release %1, but %2 previous artifact%3 could not be deleted.")
                    .arg(releaseTag)
                    .arg(failed)
                    .arg(failed == 1 ? QString() : QStringLiteral("s")),
                true);
        }
    } else if (deleted > 0) {
        if (index == m_repoDetailIndex) {
            setRepoDetailNotice(
                QStringLiteral("Published release %1 and deleted %2 previous artifact%3 (%4).")
                    .arg(releaseTag)
                    .arg(deleted)
                    .arg(deleted == 1 ? QString() : QStringLiteral("s"))
                    .arg(QLocale().formattedDataSize(bytesDeleted)));
        }
    }

    if (index == m_repoDetailIndex && m_repoDetailStack &&
        m_artifactsTabIndex >= 0 &&
        m_repoDetailStack->currentIndex() == m_artifactsTabIndex)
        loadArtifactsPanel();
    if (index == mirrorNodesRepoIndex() && mirrorNodesOnScreen())
        loadMirrorNodesPanel();
    m_mirrorAdvertSig.clear();
    refreshRepositoryList();
}

void MainWindow::pruneReleaseTagsForRepo(int repoIndex, const QString &dir,
                                         const QString &keepTag)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size() || dir.isEmpty())
        return;
    const QString key = dir + QLatin1Char('|') + keepTag;
    if (m_releaseTagPrunes.contains(key))
        return;
    m_releaseTagPrunes.insert(key);
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    runOffThread<ReleaseTagPrune>(
        [dir, keepTag] { return pruneReleaseTagsIn(dir, keepTag); },
        [this, key, dir, keepTag, owner = repo.owner,
         name = repo.name](ReleaseTagPrune prune) {
            m_releaseTagPrunes.remove(key);
            if (!prune.listed)
                return;
            if (prune.stale == 0) {
                logSystem(QStringLiteral("Releases: no previous release tags to delete for %1.")
                               .arg(keepTag));
                return;
            }
            const int index = repoIndexFor(owner, name);
            const bool current = index >= 0 && index == m_repoDetailIndex &&
                                 repoGitDir() == dir;
            finishReleaseTagPrune(keepTag, prune.deleted, prune.failed,
                                  current, index);
        });
}

void MainWindow::finishReleaseTagPrune(const QString &keepTag, int deleted,
                                       int failed, bool current, int index)
{
    if (deleted > 0)
        logSystem(
            QStringLiteral("Git: deleted %1 previous release tag%2 before publishing %3.")
                .arg(deleted)
                .arg(deleted == 1 ? QString() : QStringLiteral("s"))
                .arg(keepTag));
    if (failed > 0) {
        setRepoDetailNotice(
            QStringLiteral("Published release %1, but %2 previous tag%3 could not be deleted.")
                .arg(keepTag)
                .arg(failed)
                .arg(failed == 1 ? QString() : QStringLiteral("s")),
            true);
    } else if (deleted > 0) {
        setRepoDetailNotice(
            QStringLiteral("Published release %1 and deleted %2 previous tag%3.")
                .arg(keepTag)
                .arg(deleted)
                .arg(deleted == 1 ? QString() : QStringLiteral("s")));
    }

    if (current) {
        loadBranchesAndTags();
        if (m_repoDetailStack && m_releasesTabIndex >= 0 &&
            m_repoDetailStack->currentIndex() == m_releasesTabIndex)
            loadReleasesPanel();
    }

    if (index >= 0)
        propagateRepoUpdate(index);
}

QString MainWindow::releasesLoadRequest() const
{
    return QString::number(m_repoDetailIndex) + QLatin1Char('|') + repoGitDir();
}

void MainWindow::loadReleasesPanel()
{
    if (!m_releasesTable)
        return;
    const QString request = releasesLoadRequest();
    if (m_releasesLoading && request == m_releasesLoadRequest) {
        m_releasesReloadPending = true;
        return;
    }
    if (request != m_releasesShownRequest) {
        m_releasesShownRequest.clear();
        TableRepaintGuard repaintGuard(m_releasesTable);
        m_releasesTable->setRowCount(0);
    }
    const QString dir = repoGitDir();
    const quint64 generation = ++m_releasesLoadGen;
    m_releasesLoading = true;
    m_releasesLoadRequest = request;
    m_releasesReloadPending = false;
    runOffThread<ReleaseTagsRead>(
        [dir] { return readReleaseTags(dir); },
        [this, generation, request, dir](ReleaseTagsRead tags) {
            if (generation != m_releasesLoadGen)
                return;
            m_releasesLoading = false;
            const bool reloadPending = m_releasesReloadPending;
            m_releasesReloadPending = false;
            if (!m_releasesTable)
                return;
            if (request != releasesLoadRequest()) {
                loadReleasesPanel();
                return;
            }
            applyReleasesPanel(dir, tags.ok, tags.out);
            m_releasesShownRequest = request;
            if (reloadPending)
                loadReleasesPanel();
        });
}

void MainWindow::applyReleasesPanel(const QString &dir, bool tagsRead,
                                    const QByteArray &tagsOut)
{
    TableRepaintGuard repaintGuard(m_releasesTable);
    m_releasesTable->setRowCount(0);
    const bool writable = repoHasWorkingTree();
    bool canPushToMirrors = false;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        canPushToMirrors = writable && !repo.previewOnly && !repo.isPrivate &&
                           !repo.localPath.trimmed().isEmpty();
    }

    QString relayHost =
        serverHost(QSettings().value(kServerUrlSetting).toString().trimmed());
    if (relayHost.isEmpty())
        relayHost = serverHost(kDefaultServerUrl);
    const bool localRelay = relayHost.startsWith(QStringLiteral("127.0.0.1")) ||
                            relayHost.startsWith(QStringLiteral("localhost"));
    const QString relayBase =
        relayHost.isEmpty()
            ? QString()
            : QStringLiteral("%1://%2").arg(
                  localRelay ? QStringLiteral("http") : QStringLiteral("https"),
                  relayHost);
    static const QRegularExpression sha256Re(QStringLiteral("\\A[0-9a-f]{64}\\z"));

    QString dlOwner;
    QString dlRepo;
    QString dlSource;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &openRepo = m_repositories.at(m_repoDetailIndex);
        dlOwner = repoSegment(openRepo.owner, QStringLiteral("owner"));
        dlRepo = repoSegment(openRepo.name, QStringLiteral("repository"));
        if (!dlOwner.isEmpty() && !dlRepo.isEmpty())
            dlSource = dlOwner + QLatin1Char('/') + dlRepo;
    }
    const bool haveDownloadCounts = !dlSource.isEmpty() &&
                                    m_releaseDownloadsSource == dlSource;

    QHash<QString, QString> artifactsByTag;
    QHash<QString, QString> sizeByTag;
    QHash<QString, QString> shaByTag;
    QHash<QString, QString> shaTooltipByTag;
    QHash<QString, QString> downloadsByTag;
    QString latestChannelHtml;
    QString latestChannelTag;
    QString latestChannelSize;
    QString latestChannelSha;
    QString latestChannelShaTooltip;
    QString latestChannelDownloads;
    if (!dir.isEmpty()) {
        const QDir releasesDir(dir + QStringLiteral("/.forkmesh/releases"));
        const QStringList channels =
            releasesDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &channel : channels) {
            QFile releaseFile(
                releasesDir.filePath(channel + QStringLiteral("/release.json")));
            if (!releaseFile.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(releaseFile.readAll());
            releaseFile.close();
            if (!doc.isObject())
                continue;
            const QJsonObject obj = doc.object();
            const QString manifestTag =
                obj.value(QStringLiteral("tag")).toString().trimmed();
            if (manifestTag.isEmpty())
                continue;
            const QString manifestRepo =
                obj.value(QStringLiteral("repo")).toString().trimmed();
            QStringList assetLinks;
            QStringList sizeParts;
            QStringList shaParts;
            QStringList shaTooltipParts;
            QStringList downloadParts;
            const QJsonArray assets = obj.value(QStringLiteral("assets")).toArray();
            for (const QJsonValue &asset : assets) {
                const QJsonObject a = asset.toObject();
                const QString name = a.value(QStringLiteral("name")).toString();
                if (name.isEmpty())
                    continue;
                const QString escaped = name.toHtmlEscaped();
                const QString hash =
                    a.value(QStringLiteral("blob_sha256")).toString().trimmed();
                const bool hashValid = sha256Re.match(hash).hasMatch();
                QString entry;
                if (!relayBase.isEmpty() && !manifestRepo.isEmpty() &&
                    hashValid) {
                    const QString url =
                        QStringLiteral(
                            "%1/api/repo/%2/releases/blob/sha256/%3?filename=%4")
                            .arg(relayBase, manifestRepo, hash,
                                 QString::fromUtf8(QUrl::toPercentEncoding(name)));
                    entry =
                        QStringLiteral(
                            "<a href=\"%1\" style=\"color:%3;"
                            "text-decoration:none\">%2</a>")
                            .arg(url.toHtmlEscaped(), escaped,
                                 Theme::nextOrHex(Theme::Role::AccentFg,
                                                  QStringLiteral("#58a6ff")));
                } else {
                    entry = escaped;
                }
                assetLinks.append(entry);

                const qint64 size =
                    static_cast<qint64>(a.value(QStringLiteral("size")).toDouble());
                sizeParts.append(size > 0 ? QLocale().formattedDataSize(size)
                                          : QStringLiteral("—"));

                shaParts.append(hashValid ? hash.left(12) : QStringLiteral("—"));
                shaTooltipParts.append(hashValid ? QStringLiteral("sha256:%1").arg(hash)
                                                 : QString());

                if (hashValid && haveDownloadCounts)
                    downloadParts.append(
                        QString::number(m_releaseDownloadsCache.value(hash, 0)));
                else
                    downloadParts.append(QStringLiteral("—"));
            }
            if (!assetLinks.isEmpty()) {
                const QString joined = assetLinks.join(QStringLiteral(", "));
                artifactsByTag.insert(manifestTag, joined);
                sizeByTag.insert(manifestTag, sizeParts.join(QStringLiteral(", ")));
                shaByTag.insert(manifestTag, shaParts.join(QStringLiteral(", ")));
                shaTooltipByTag.insert(
                    manifestTag,
                    shaTooltipParts.join(QStringLiteral("\n")).trimmed());
                downloadsByTag.insert(manifestTag,
                                      downloadParts.join(QStringLiteral(", ")));
                if (channel.compare(QStringLiteral("latest"),
                                    Qt::CaseInsensitive) == 0) {
                    latestChannelHtml = joined;
                    latestChannelTag = manifestTag;
                    latestChannelSize = sizeByTag.value(manifestTag);
                    latestChannelSha = shaByTag.value(manifestTag);
                    latestChannelShaTooltip = shaTooltipByTag.value(manifestTag);
                    latestChannelDownloads = downloadsByTag.value(manifestTag);
                }
            }
        }
    }

    int count = 0;
    QString currentTag;
    if (!dir.isEmpty() && tagsRead) {
        QStringList tagLines;
        for (const QString &record :
             QString::fromUtf8(tagsOut).split(QLatin1Char('\x1e'))) {
            if (!record.trimmed().isEmpty())
                tagLines << record;
        }
        for (int i = 0; i < tagLines.size(); ++i) {
            const QStringList f = tagLines.at(i).split(QLatin1Char('\x1f'));
            if (f.isEmpty())
                continue;
            const QString tag = f.value(0).trimmed();
            if (tag.isEmpty())
                continue;
            if (currentTag.isEmpty())
                currentTag = tag;

            QString prevTag;
            for (int j = i + 1; j < tagLines.size(); ++j) {
                const QString cand =
                    tagLines.at(j).split(QLatin1Char('\x1f')).value(0).trimmed();
                if (!cand.isEmpty()) {
                    prevTag = cand;
                    break;
                }
            }

            const int row = m_releasesTable->rowCount();
            m_releasesTable->insertRow(row);
            auto *tagItem = new QTableWidgetItem(tag);
            tagItem->setIcon(themedOcticon("tag", Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")), 14));
            m_releasesTable->setItem(row, 0, tagItem);

            QString commitSha = f.value(3).trimmed();
            if (commitSha.isEmpty())
                commitSha = f.value(4).trimmed();
            auto *commitItem = new QTableWidgetItem(commitSha);
            commitItem->setFont(QFont(QStringLiteral("monospace")));
            commitItem->setForeground(Theme::remapColor(QColor("#8b949e")));
            m_releasesTable->setItem(row, 1, commitItem);

            const QString dateTimeStr = f.value(1).trimmed();
            QString relativeTime = dateTimeStr;
            if (!dateTimeStr.isEmpty()) {
                QDateTime dt = QDateTime::fromString(dateTimeStr, "yyyy-MM-dd hh:mm");
                if (dt.isValid()) {
                    relativeTime = formatIssueRelativeTime(dt.toMSecsSinceEpoch());
                }
            }
            auto *whenItem = new QTableWidgetItem(relativeTime);
            const QString tagger = f.value(5).trimmed();
            QString toolTip = dateTimeStr;
            if (!tagger.isEmpty()) {
                if (!toolTip.isEmpty())
                    toolTip += QStringLiteral(" · ");
                toolTip += QStringLiteral("Tagged by %1").arg(tagger);
            }
            if (!toolTip.isEmpty())
                whenItem->setToolTip(toolTip);
            m_releasesTable->setItem(row, 2, whenItem);

            const QString subject = f.value(2).trimmed();
            const QString body = f.value(6).trimmed();
            QString notesPreview = subject;
            if (notesPreview.isEmpty() ||
                notesPreview.compare(tag, Qt::CaseInsensitive) == 0) {
                for (const QString &rawLine : body.split(QLatin1Char('\n'))) {
                    QString line = rawLine.trimmed();
                    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                        continue;
                    if (line.startsWith(QStringLiteral("* ")))
                        line = line.mid(2).trimmed();
                    notesPreview = line;
                    break;
                }
            }
            if (notesPreview.isEmpty())
                notesPreview = QStringLiteral("—");
            auto *notesItem = new QTableWidgetItem(notesPreview);
            const QString notesTooltip =
                body.isEmpty() ? subject : subject + QStringLiteral("\n\n") + body;
            if (!notesTooltip.trimmed().isEmpty())
                notesItem->setToolTip(notesTooltip);
            m_releasesTable->setItem(row, 3, notesItem);

            if (prevTag.isEmpty()) {
                auto *initial = new QTableWidgetItem("Initial release");
                initial->setForeground(Theme::remapColor(QColor("#8b949e")));
                m_releasesTable->setItem(row, 4, initial);
            } else {
                auto *compare = new QLabel(
                    QStringLiteral("<a href=\"#\" style=\"color:%3;"
                                   "text-decoration:none\">Compare %1...%2</a>")
                        .arg(prevTag.toHtmlEscaped(), tag.toHtmlEscaped(),
                             Theme::nextOrHex(Theme::Role::AccentFg,
                                              QStringLiteral("#58a6ff"))));
                compare->setTextFormat(Qt::RichText);
                compare->setContentsMargins(6, 0, 6, 0);
                compare->setObjectName(QStringLiteral("releaseCellLink"));
                compare->setCursor(Qt::PointingHandCursor);
                connect(compare, &QLabel::linkActivated, this,
                        [this, tag] { showReleaseDetail(tag); });
                m_releasesTable->setCellWidget(row, 4, compare);
            }

            QString artifactsHtml = artifactsByTag.value(tag);
            QString sizeText = sizeByTag.value(tag);
            QString shaText = shaByTag.value(tag);
            QString shaTooltip = shaTooltipByTag.value(tag);
            QString downloadsText = downloadsByTag.value(tag);
            if (artifactsHtml.isEmpty() && count == 0 &&
                !latestChannelHtml.isEmpty()) {
                artifactsHtml =
                    latestChannelHtml +
                    QStringLiteral(" <span style=\"color:#8b949e;"
                                   "font-size:11px\">latest &middot; %1</span>")
                        .arg(latestChannelTag.toHtmlEscaped());
                sizeText = latestChannelSize;
                shaText = latestChannelSha;
                shaTooltip = latestChannelShaTooltip;
                downloadsText = latestChannelDownloads;
            }
            if (artifactsHtml.isEmpty()) {
                m_releasesTable->setItem(row, 5, new QTableWidgetItem(QString()));
            } else {
                auto *artifacts = new QLabel(artifactsHtml);
                artifacts->setTextFormat(Qt::RichText);
                artifacts->setOpenExternalLinks(true);
                artifacts->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
                artifacts->setContentsMargins(6, 0, 6, 0);
                artifacts->setObjectName(QStringLiteral("releaseCellLink"));
                artifacts->setCursor(Qt::PointingHandCursor);
                m_releasesTable->setCellWidget(row, 5, artifacts);
            }

            auto *sizeItem = new QTableWidgetItem(
                sizeText.isEmpty() ? QStringLiteral("—") : sizeText);
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            sizeItem->setForeground(Theme::remapColor(QColor("#8b949e")));
            m_releasesTable->setItem(row, 6, sizeItem);

            auto *shaItem = new QTableWidgetItem(
                shaText.isEmpty() ? QStringLiteral("—") : shaText);
            shaItem->setFont(QFont(QStringLiteral("monospace")));
            shaItem->setForeground(Theme::remapColor(QColor("#8b949e")));
            if (!shaTooltip.isEmpty())
                shaItem->setToolTip(shaTooltip);
            m_releasesTable->setItem(row, 7, shaItem);

            auto *downloadsItem = new QTableWidgetItem(
                downloadsText.isEmpty() ? QStringLiteral("—") : downloadsText);
            downloadsItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            downloadsItem->setForeground(Theme::remapColor(QColor("#8b949e")));
            m_releasesTable->setItem(row, 8, downloadsItem);

            auto *pushMirrors = new QPushButton("Push to mirrors");
            pushMirrors->setObjectName("ghostButton");
            pushMirrors->setCursor(Qt::PointingHandCursor);
            setOcticon(pushMirrors, "broadcast", 14);
            pushMirrors->setToolTip(
                QStringLiteral(
                    "Push %1 directly to installed saved mirror hosts and "
                    "configured SSH gateways, then notify online peer "
                    "mirrors. Release artifacts are transferred through the "
                    "existing SHA-256-verified mirror path.")
                    .arg(tag));
            pushMirrors->setEnabled(canPushToMirrors);
            connect(pushMirrors, &QPushButton::clicked, this,
                    [this, tag] { pushReleaseToMirrors(tag); });
            m_releasesTable->setCellWidget(row, 9, pushMirrors);

            auto *del = new QPushButton;
            del->setObjectName("issueIconButton");
            del->setFlat(true);
            del->setCursor(Qt::PointingHandCursor);
            del->setIcon(themedOcticon("trash", QColor("#f85149"), 15));
            del->setIconSize(QSize(15, 15));
            del->setToolTip(QStringLiteral("Delete tag %1").arg(tag));
            del->setEnabled(writable);
            connect(del, &QPushButton::clicked, this, [this, tag] { deleteTag(tag); });
            m_releasesTable->setCellWidget(row, 10, del);
            ++count;
        }
    }
    if (m_releasesSummary) {
        if (count == 0)
            m_releasesSummary->setText(QString());
        else if (currentTag.isEmpty())
            m_releasesSummary->setText(QStringLiteral("(%1)").arg(count));
        else
            m_releasesSummary->setText(
                QStringLiteral("%1 (%2)").arg(currentTag).arg(count));
    }
    if (auto *b = dynamic_cast<VerticalIconButton *>(m_repoReleasesTab))
        b->setBadgeCount(count);
    if (count == 0) {
        m_releasesTable->insertRow(0);
        auto *empty = new QTableWidgetItem(
            "No releases yet. Draft one to tag a commit in the repository.");
        empty->setForeground(Theme::remapColor(QColor("#8b949e")));
        m_releasesTable->setItem(0, 0, empty);
    }
    if (!dlSource.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_releaseDownloadsFetchSource != dlSource ||
            nowMs - m_releaseDownloadsFetchedMs > 15000) {
            m_releaseDownloadsFetchSource = dlSource;
            m_releaseDownloadsFetchedMs = nowMs;
            fetchReleaseDownloadCounts(dlOwner, dlRepo, dlSource);
        }
    }
}

QWidget *MainWindow::buildMirrorNodesSection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("mirrorNodesSection"));
    const bool next = Theme::isNext();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    if (next) {
        page->setAttribute(Qt::WA_StyledBackground, true);
        layout->setContentsMargins(16, 8, 12, 8);
    }

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    if (next) {
        headerRow->setObjectName(QStringLiteral("mirrorNodesHeaderRow"));
        headerRow->setSpacing(12);
    }
    auto *heading = new QLabel(QStringLiteral("Repository mirrors"));
    heading->setObjectName("sectionLabel");
    m_mirrorNodesRepoCombo = new QComboBox;
    m_mirrorNodesRepoCombo->setObjectName(QStringLiteral("mirrorNodesRepoCombo"));
    m_mirrorNodesRepoCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_mirrorNodesRepoCombo->setToolTip(QStringLiteral(
        "Repository whose copies these rows compare with its source of truth. "
        "Follows the repository you open until you pick another here."));
    connect(m_mirrorNodesRepoCombo, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
                m_mirrorNodesRepoKey =
                    m_mirrorNodesRepoCombo->itemData(index).toString();
                m_catalogMirrorsFetchedMs = 0;
                loadMirrorNodesPanel();
            });
    m_mirrorNodesSummary = new QLabel;
    m_mirrorNodesSummary->setObjectName("statusLine");
    m_mirrorResetPinButton = new QPushButton("Reset integrity pin");
    m_mirrorResetPinButton->setObjectName("ghostButton");
    m_mirrorResetPinButton->setCursor(Qt::PointingHandCursor);
    m_mirrorResetPinButton->setToolTip(QStringLiteral(
        "Re-sign the refs this node serves and overwrite the relay's integrity "
        "pin, so clones work again after the served refs have moved on."));
    setOcticon(m_mirrorResetPinButton, "shield-check", 16);
    m_mirrorResetPinButton->hide();
    connect(m_mirrorResetPinButton, &QPushButton::clicked, this, [this] {
        if (mirrorNodesRepoIndex() == m_repoDetailIndex)
            resetRepoPin();
        else
            requestMirrorNodesRefresh();
    });
    m_mirrorNodesOnlineOnlyCheck =
        next ? new kit::Toggle(QStringLiteral("Online only"))
             : new QCheckBox(QStringLiteral("Online only"));
    m_mirrorNodesOnlineOnlyCheck->setChecked(true);
    m_mirrorNodesOnlineOnlyCheck->setToolTip(QStringLiteral(
        "Show the chosen repository's copy only for nodes that are online "
        "right now; every node stays listed"));
    connect(m_mirrorNodesOnlineOnlyCheck, &QCheckBox::toggled, this,
            &MainWindow::loadMirrorNodesPanel);
    auto *refreshNodesButton = new QPushButton;
    refreshNodesButton->setObjectName("ghostButton");
    refreshNodesButton->setFixedSize(next ? QSize(28, 28) : QSize(32, 30));
    refreshNodesButton->setAccessibleName(QStringLiteral("Refresh nodes"));
    refreshNodesButton->setCursor(Qt::PointingHandCursor);
    refreshNodesButton->setToolTip(QStringLiteral(
        "Ask online mirror nodes to immediately report their latest commit and "
        "mirror metadata"));
    setOcticon(refreshNodesButton, "broadcast", 16);
    connect(refreshNodesButton, &QPushButton::clicked, this,
            &MainWindow::requestMirrorNodesRefresh);
    headerRow->addWidget(heading);
    headerRow->addWidget(m_mirrorNodesRepoCombo);
    headerRow->addWidget(m_mirrorNodesSummary, 1);
    headerRow->addWidget(m_mirrorNodesOnlineOnlyCheck);
    headerRow->addWidget(m_mirrorResetPinButton);
    headerRow->addWidget(refreshNodesButton);
    layout->addLayout(headerRow);

    m_mirrorNodesTable = new QTableWidget(0, MirrorNodeColumnCount);
    installColumnHeaderMenu(m_mirrorNodesTable);
    m_mirrorNodesTable->setObjectName("issueTable");
    m_mirrorNodesTable->setProperty("mirrorNodesTable", true);
    enableHoverRowHighlight(m_mirrorNodesTable);
    struct MirrorHeaderIcon {
        int column;
        const char *icon;
        const char *label; // tooltip: the full name of the column
        const char *text;  // header caption: kept terse, the icon carries the rest
    };
    static constexpr MirrorHeaderIcon kMirrorHeaderIcons[] = {
        {MirrorNodeColNode, "device-desktop", "Node", "Node"},
        {MirrorNodeColOwner, "person", "Owner", "Owner"},
        {MirrorNodeColRepos, "repo", "Repositories this node mirrors", "Repos"},
        {MirrorNodeColCommit, "git-commit", "Latest commit", "Commit"},
        {MirrorNodeColMessage, "note", "Message", "Message"},
        {MirrorNodeColAuthor, "pencil", "Author", "Author"},
        {MirrorNodeColSynced, "history", "Synced", "Synced"},
        {MirrorNodeColSize, "pie-chart", "Size", "Size"},
        {MirrorNodeColGitDatabase, "database", "Physical Git object database", "Git DB"},
        {MirrorNodeColIssues, "issue-opened", "Issues", "Issues"},
        {MirrorNodeColIssuesToSource, "upload", "Issues waiting to sync back to the source of truth", "Issues \xE2\x86\x91"},
        {MirrorNodeColCommits, "list-unordered", "Commits", "Commits"},
        {MirrorNodeColCommitsToSource, "upload", "Commits waiting to sync back to the source of truth", "Commits \xE2\x86\x91"},
        {MirrorNodeColBranches, "git-branch", "Branches", "Branches"},
        {MirrorNodeColBranchesToSource, "upload", "Branches waiting to sync back to the source of truth", "Branches \xE2\x86\x91"},
        {MirrorNodeColPulls, "git-pull-request", "Pulls", "Pulls"},
        {MirrorNodeColPullsToSource, "upload", "Pull requests waiting to sync back to the source of truth", "Pulls \xE2\x86\x91"},
        {MirrorNodeColDiscussions, "comment", "Discussions", "Discuss"},
        {MirrorNodeColDiscussionsToSource, "upload", "Discussions waiting to sync back to the source of truth", "Discuss \xE2\x86\x91"},
        {MirrorNodeColHealth, "shield-check", "Health", "Health"},
        {MirrorNodeColCpu, "graph", "CPU", "CPU"},
        {MirrorNodeColRam, "server", "RAM", "RAM"},
        {MirrorNodeColDisk, "file-directory", "Disk", "Disk"},
        {MirrorNodeColPlatform, "terminal", "Platform", "Platform"},
        {MirrorNodeColVersion, "tag", "Version", "Version"},
        {MirrorNodeColId, "key", "Node id", "Node id"},
        {MirrorNodeColRoute, "link-external", "Direct HTTPS route", "Route"},
        {MirrorNodeColClones, "download", "Clones", "Clones"},
        {MirrorNodeColWebsite, "eye", "Website", "Website"},
        {MirrorNodeColArtifacts, "package", "Artifacts", "Artifacts"},
        {MirrorNodeColReachability, "link", "Reachability", "Reach"},
        {MirrorNodeColRole, "server", "Role: core, runner, mirror or desktop node", "Role"},
    };
    static_assert(sizeof(kMirrorHeaderIcons) / sizeof(kMirrorHeaderIcons[0]) ==
                      MirrorNodeColumnCount,
                  "every mirror-node column needs a header icon");
    for (const MirrorHeaderIcon &h : kMirrorHeaderIcons) {
        auto *headerItem = new QTableWidgetItem;
        headerItem->setIcon(
            themedOcticon(QLatin1String(h.icon), Theme::remapColor(QColor("#8b949e")), 14));
        headerItem->setText(QString::fromUtf8(h.text));
        headerItem->setToolTip(QString::fromUtf8(h.label));
        if (h.column == MirrorNodeColRole)
            headerItem->setData(Qt::UserRole, QStringLiteral("role"));
        m_mirrorNodesTable->setHorizontalHeaderItem(h.column, headerItem);
    }
    m_mirrorNodesTable->verticalHeader()->setVisible(false);
    m_mirrorNodesTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_mirrorNodesTable->verticalHeader()->setDefaultSectionSize(28);
    m_mirrorNodesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mirrorNodesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_mirrorNodesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mirrorNodesTable->setShowGrid(false);
    m_mirrorNodesTable->setWordWrap(false);
    m_mirrorNodesTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_mirrorNodesTable->setSortingEnabled(true);
    m_mirrorNodesTable->sortByColumn(MirrorNodeColNode,
                                     Qt::AscendingOrder); // source of truth first
    QHeaderView *mh = m_mirrorNodesTable->horizontalHeader();
    mh->setHighlightSections(false);
    mh->setMinimumSectionSize(22);
    // Keep the native icon-and-caption paint path. The stacked custom painter
    // can receive a header rectangle shorter than its two-line size hint after
    // a deferred table layout; on light themes that left the Mirror nodes
    // header as a row of apparently empty white cells. The native path honors
    // the active QSS foreground and clips one compact line predictably.
    mh->setStyleSheet(Theme::remapSheet("QHeaderView::section { font-size: 10px; padding: 2px; }"));
    if (auto *compactHeader = qobject_cast<KebabHeaderView *>(mh)) {
        compactHeader->setCompactSections(true);
        compactHeader->setStackedIconLabels(false);
    }
    mh->setSectionResizeMode(MirrorNodeColNode, QHeaderView::Stretch);
    mh->setSectionResizeMode(MirrorNodeColOwner, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColRepos, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColCommit, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColMessage, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColAuthor, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColSynced, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColSize, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColGitDatabase,
                             QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColIssues, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColIssuesToSource,
                             QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColCommits, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColCommitsToSource,
                             QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColBranches, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColBranchesToSource,
                             QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColPulls, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColPullsToSource,
                             QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColDiscussions, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColDiscussionsToSource,
                             QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColHealth, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColCpu, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColRam, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColDisk, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColPlatform, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColVersion, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColId, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColRoute, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColClones, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColWebsite, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColArtifacts, QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColReachability,
                             QHeaderView::ResizeToContents);
    mh->setSectionResizeMode(MirrorNodeColRole, QHeaderView::ResizeToContents);
    mh->moveSection(mh->visualIndex(MirrorNodeColRole), 1);
    makeColumnsResizable(m_mirrorNodesTable);
    m_mirrorNodesTable->setItemDelegateForColumn(
        MirrorNodeColSynced, new MirrorSyncDelegate(m_mirrorNodesTable));
    auto *resourceBars = new ResourceBarDelegate(m_mirrorNodesTable);
    for (int col : {MirrorNodeColCpu, MirrorNodeColRam, MirrorNodeColDisk})
        m_mirrorNodesTable->setItemDelegateForColumn(col, resourceBars);
    m_mirrorNodesTable->hide();
    layout->addWidget(m_mirrorNodesTable);
    return page;
}

int MainWindow::mirrorNodesRepoIndex() const
{
    if (!m_mirrorNodesRepoKey.isEmpty()) {
        for (int i = 0; i < m_repositories.size(); ++i) {
            const RepositoryRecord &candidate = m_repositories.at(i);
            if ((candidate.owner + QLatin1Char('/') + candidate.name)
                    .compare(m_mirrorNodesRepoKey, Qt::CaseInsensitive) == 0)
                return i;
        }
    }
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size())
        return m_repoDetailIndex;
    for (int i = 0; i < m_repositories.size(); ++i)
        if (!m_repositories.at(i).previewOnly)
            return i;
    return m_repositories.isEmpty() ? -1 : 0;
}

void MainWindow::refreshMirrorNodesRepoChooser()
{
    if (!m_mirrorNodesRepoCombo)
        return;
    QStringList keys;
    for (const RepositoryRecord &candidate : std::as_const(m_repositories)) {
        const QString key = candidate.owner + QLatin1Char('/') + candidate.name;
        if (!candidate.owner.isEmpty() && !candidate.name.isEmpty() &&
            !keys.contains(key, Qt::CaseInsensitive))
            keys.append(key);
    }
    const int repoIndex = mirrorNodesRepoIndex();
    const QString current =
        repoIndex >= 0 ? m_repositories.at(repoIndex).owner + QLatin1Char('/') +
                             m_repositories.at(repoIndex).name
                       : QString();
    QSignalBlocker block(m_mirrorNodesRepoCombo);
    QStringList shown;
    for (int i = 0; i < m_mirrorNodesRepoCombo->count(); ++i)
        shown.append(m_mirrorNodesRepoCombo->itemData(i).toString());
    if (shown != keys) {
        m_mirrorNodesRepoCombo->clear();
        for (const QString &key : std::as_const(keys))
            m_mirrorNodesRepoCombo->addItem(key, key);
    }
    const int currentRow = keys.indexOf(current);
    if (currentRow >= 0 && m_mirrorNodesRepoCombo->currentIndex() != currentRow)
        m_mirrorNodesRepoCombo->setCurrentIndex(currentRow);
    m_mirrorNodesRepoCombo->setEnabled(!keys.isEmpty());
}

void MainWindow::showMirrorNodesOnNetwork(int repoIndex)
{
    if (repoIndex >= 0 && repoIndex < m_repositories.size())
        m_mirrorNodesRepoKey = m_repositories.at(repoIndex).owner +
                               QLatin1Char('/') +
                               m_repositories.at(repoIndex).name;
    showNetworkTab(kNetworkNodesTab);
    loadMirrorNodesPanel();
    if (mirrorNodesOnScreen() && m_nodesTable->isEnabled())
        m_nodesTable->setFocus(Qt::OtherFocusReason);
}

void MainWindow::fetchMirrorSectionCatalog(const QString &owner,
                                           const QString &repo)
{
    if (!m_networkAccess || owner.isEmpty() || repo.isEmpty())
        return;
    constexpr qint64 kMirrorSectionCatalogMs = 5 * 60 * 1000;
    const QString cacheKey = owner.toLower() + QLatin1Char('/') + repo.toLower();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 fetchedMs = m_networkRepoMirrorsFetchedMs.value(cacheKey, 0);
    if (fetchedMs > 0 && nowMs - fetchedMs < kMirrorSectionCatalogMs)
        return;
    const QString backoffKey = QStringLiteral("mirror-section-catalog|") + cacheKey;
    if (!m_pollBackoff.ready(backoffKey, nowMs))
        return;
    m_networkRepoMirrorsFetchedMs.insert(cacheKey, nowMs);
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/mirrors")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(owner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(repo))));
    url.setQuery(QString());
    QNetworkRequest request(url);
    request.setRawHeader("accept", "application/json");
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, cacheKey, backoffKey] {
                const QByteArray body = reply->readAll();
                const bool ok = reply->error() == QNetworkReply::NoError &&
                                QJsonDocument::fromJson(body)
                                    .object()
                                    .value(QStringLiteral("ok"))
                                    .toBool();
                const bool expectedDegraded =
                    forkmesh::networkReplyIsExpectedDegraded(reply);
                reply->deleteLater();
                if (!ok) {
                    m_networkRepoMirrorsFetchedMs.remove(cacheKey);
                    if (!expectedDegraded)
                        m_pollBackoff.noteFailure(
                            backoffKey, QDateTime::currentMSecsSinceEpoch());
                    return;
                }
                m_pollBackoff.noteSuccess(backoffKey);
                m_networkRepoMirrorsCache.insert(cacheKey, body);
                if (mirrorNodesOnScreen())
                    loadMirrorNodesPanel();
            });
}

void MainWindow::draftMirrorNodeDiagnosticsPrompt(QTableWidgetItem *healthItem)
{
    if (!healthItem || !m_issueQuickAdd)
        return;
    const QString prompt =
        healthItem->data(kMirrorDiagnosticPromptRole).toString().trimmed();
    if (prompt.isEmpty())
        return;
    const int repoIndex = mirrorNodesRepoIndex();
    if (repoIndex >= 0 && repoIndex != m_repoDetailIndex)
        openRepoDetail(repoIndex);
    if (m_sectionStack && m_sectionStack->currentIndex() != 0)
        showSection(0);
    recordQuickAddHistory(m_issueQuickAdd->toPlainText());
    m_issueQuickAdd->setPlainText(prompt);
    m_issueQuickAdd->moveCursor(QTextCursor::End);
    m_issueQuickAdd->setFocus();
    setRepoDetailNotice(QStringLiteral(
        "Drafted the mirror-node diagnostic report in the composer — review it, "
        "then send it to an agent."));
    logSystem(QStringLiteral("Drafted a mirror-node diagnostic troubleshooting prompt."));
}

void MainWindow::requestMirrorNodesRefresh()
{
    const int repoIndex = mirrorNodesRepoIndex();
    if (repoIndex < 0)
        return;

    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    const QString name = repoSegment(repo.name, QStringLiteral("repository"));
    const QString source =
        repoSegment(repo.owner, QStringLiteral("owner")) + "/" + name;
    if (source.section('/', 0, 0).isEmpty() || name.isEmpty())
        return;

    // Saved Hosts are authoritative and each signed health request goes to its
    // mirror origin. Refreshing this view must not publish desktop state or
    // require the app Worker.
    m_catalogMirrorsFetchedMs = 0;
    const QString sshPrefix = source + QLatin1Char('|');
    for (auto it = m_sshMirrorMetadataCache.begin();
         it != m_sshMirrorMetadataCache.end();) {
        if (it.key().startsWith(sshPrefix))
            it = m_sshMirrorMetadataCache.erase(it);
        else
            ++it;
    }
    logSystem(QStringLiteral("Mirror nodes: refreshing active Hosts directly for %1.")
                  .arg(source));
    flashMessage(QStringLiteral("Refreshing active mirror hosts directly."));
    loadMirrorNodesPanel();
}

static QString mirrorRelationKey(const QString &gitDir, const QString &sourceOid,
                                 const QString &peerOid)
{
    return gitDir + QLatin1Char('\x1f') + sourceOid + QLatin1Char('\x1f') + peerOid;
}

static QString mirrorCountKey(const QString &gitDir, const QString &stamp,
                              const QStringList &args)
{
    return gitDir + QLatin1Char('\x1f') + stamp + QLatin1Char('\x1f') +
           args.join(QLatin1Char('\x1f'));
}

static QString mirrorPanelRefsStamp(const QString &gitDir)
{
    const QString common = gitCommonDirPath(gitDir);
    if (common.isEmpty())
        return gitRefsDirSignature(gitDir);
    const QString own = resolveGitDirPath(gitDir);
    QString stamp = gitRefsDirSignature(common);
    if (!own.isEmpty() && QDir(own).absolutePath() != QDir(common).absolutePath())
        stamp += QLatin1Char('|') + gitRefsDirSignature(own);
    return stamp;
}

static MirrorSelfSnapshot gatherMirrorSelfSnapshot(const RepositoryRecord &repo,
                                                   const QString &key,
                                                   bool hasWorkingTree)
{
    MirrorSelfSnapshot snapshot;
    snapshot.key = key;
    snapshot.gatheredMs = QDateTime::currentMSecsSinceEpoch();
    const QString mirror = repo.mirrorPath;
    MirrorAdvert &advert = snapshot.advert;
    const MirrorBranchTip tip = mirrorPrimaryBranchTip(mirror, repo.localPath);
    advert.branch = tip.branch;
    advert.commit = tip.commit;
    advert.commitIdentity = mirrorCommitIdentity(mirror, repo.localPath, tip.commit);
    advert.updatedMs = repo.lastSyncMs;
    const QString countsDir =
        (!mirror.trimmed().isEmpty() && QDir(mirror).exists())
            ? mirror
            : repo.localPath.trimmed();
    advert.sizeBytes = mirrorRepoSizeBytes(countsDir);
    advert.issueCount = mirrorIssueCount(countsDir, advert.branch);
    advert.commitCount = mirrorCommitCount(countsDir, advert.branch);
    advert.branchCount = mirrorBranchCount(countsDir);
    advert.pullCount = mirrorPullCount(countsDir, advert.branch);
    advert.discussionCount = mirrorDiscussionCount(countsDir, advert.branch);
    advert.worktreeCount = mirrorWorktreeCount(repo.localPath);
    advert.artifactCount = mirrorArtifactCount(mirror);
    if (advert.artifactCount < 0)
        advert.artifactCount = checkoutArtifactCount(repo.localPath);
    snapshot.servedCommit = mirrorBranchCommit(countsDir, advert.branch);
    if (hasWorkingTree && !repo.localPath.trimmed().isEmpty() &&
        !snapshot.servedCommit.isEmpty()) {
        QByteArray out;
        const QString pushTarget =
            advert.branch.isEmpty() ? QStringLiteral("HEAD") : advert.branch;
        if (runGitCapture(repo.localPath,
                          {QStringLiteral("rev-list"), QStringLiteral("--count"),
                           snapshot.servedCommit + QStringLiteral("..") + pushTarget},
                          &out, nullptr))
            snapshot.pendingPush = QString::fromUtf8(out).trimmed().toInt();
    }
    return snapshot;
}

QString MainWindow::mirrorSelfSnapshotKey(const RepositoryRecord &repo) const
{
    // Filesystem-only, like refreshMirrorAdverts()'s input signature: asking git
    // whether git has anything new would cost exactly what we are trying to skip.
    auto stamp = [](const QString &base, const QString &leaf) {
        return QString::number(QFileInfo(QDir(base).filePath(leaf))
                                   .lastModified()
                                   .toMSecsSinceEpoch());
    };
    // lastSyncMs is deliberately not part of the key: autoSyncMirrors()
    // rewrites it every minute whether or not anything moved, and the only
    // field it fed (advert.updatedMs) is overwritten by the caller anyway —
    // so it re-ran the whole ten-command snapshot once a minute for nothing.
    // The mirror's refs stamp is recursive (a nested branch, agent/x, only
    // bumps its own directory, which the flat refs/ mtime never saw). The
    // checkout contributes only what the snapshot reads from it: its HEAD
    // and reflog (the pending-push count) and its worktrees — not every
    // agent branch, which move constantly on a busy node.
    const QString gitDir = resolveGitDirPath(repo.localPath);
    return repo.mirrorPath + QLatin1Char('|') + repo.localPath + QLatin1Char('|') +
           gitRefsDirSignature(repo.mirrorPath) + QLatin1Char('|') +
           stamp(gitDir, QStringLiteral("HEAD")) + QLatin1Char('|') +
           stamp(gitDir, QStringLiteral("logs/HEAD")) + QLatin1Char('|') +
           stamp(gitDir, QStringLiteral("worktrees"));
}

void MainWindow::refreshMirrorSelfSnapshot(const RepositoryRecord &repo,
                                           const QString &key)
{
    if (m_mirrorSelfSnapshotsInFlight.contains(repo.mirrorPath))
        return; // a gather is already running; its result carries the newer key
    m_mirrorSelfSnapshotsInFlight.insert(repo.mirrorPath);
    const RepositoryRecord snapshotRepo = repo; // by value: the worker outlives it
    const bool hasWorkingTree = !repo.previewOnly && !repo.localPath.isEmpty() &&
                                QDir(repo.localPath).exists(QStringLiteral(".git"));
    auto result = std::make_shared<MirrorSelfSnapshot>();
    QThread *worker = QThread::create([snapshotRepo, key, hasWorkingTree, result] {
        const forkmesh::BackgroundScope activity(
            QStringLiteral("mirrors"),
            QStringLiteral("read mirror row for %1").arg(snapshotRepo.name),
            forkmesh::ActionTelemetry::Execution::Worker);
        *result = gatherMirrorSelfSnapshot(snapshotRepo, key, hasWorkingTree);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &QThread::finished, this,
            [this, mirror = repo.mirrorPath, result] {
                m_mirrorSelfSnapshotsInFlight.remove(mirror);
                m_mirrorSelfSnapshots.insert(mirror, *result);
                loadMirrorNodesPanel(); // one rebuild with the fresh figures
            });
    worker->start();
}

// code-health: allow(git-in-loop) runs on a runOffThread worker; one cat-file/merge-base pair per commit pair and one rev-list per backlog the panel has not seen, each answer memoised by the caller
MainWindow::MirrorPanelGitFacts
MainWindow::readMirrorPanelGitFacts(const MirrorPanelGitRequest &request)
{
    MirrorPanelGitFacts facts;
    facts.gitDir = request.gitDir;
    facts.stamp = request.stamp;
    const QString &gitDir = request.gitDir;
    if (request.wantRefs) {
        facts.refsRead = true;
        facts.refs.stamp = request.stamp;
        facts.refs.routingHash = mirrorRoutingStateHash(gitDir);
        facts.refs.allRefsHash = mirrorStateHash(gitDir);
        facts.refs.heads = branchHeadsFromGit(gitDir);
    }
    for (const QPair<QString, QString> &pair : request.relations) {
        const QString &sourceOid = pair.first;
        const QString &peerOid = pair.second;
        MirrorCommitRelation relation = MirrorCommitRelation::Unknown;
        if (mirrorHasCommit(gitDir, sourceOid) && mirrorHasCommit(gitDir, peerOid)) {
            if (runGitCapture(gitDir,
                              {QStringLiteral("merge-base"),
                               QStringLiteral("--is-ancestor"), peerOid, sourceOid},
                              nullptr, nullptr))
                relation = MirrorCommitRelation::BehindSource;
            else if (runGitCapture(gitDir,
                                   {QStringLiteral("merge-base"),
                                    QStringLiteral("--is-ancestor"), sourceOid,
                                    peerOid},
                                   nullptr, nullptr))
                relation = MirrorCommitRelation::AheadOfSource;
            else
                relation = MirrorCommitRelation::Diverged;
        }
        facts.relations.insert(mirrorRelationKey(gitDir, sourceOid, peerOid),
                               static_cast<int>(relation));
    }
    for (const QStringList &args : request.counts) {
        QByteArray count;
        facts.counts.insert(mirrorCountKey(gitDir, request.stamp, args),
                            runGitCapture(gitDir, args, &count, nullptr)
                                ? QString::fromUtf8(count).trimmed().toInt()
                                : -1);
    }
    for (const QString &commit : request.identities)
        facts.identities.insert(commit,
                                gitCommitIdentity(request.identityDir, commit));
    return facts;
}

void MainWindow::requestMirrorPanelGitFacts(const MirrorPanelGitRequest &request)
{
    if (!request.wantRefs && request.relations.isEmpty() &&
        request.counts.isEmpty() && request.identities.isEmpty())
        return;
    if (m_mirrorPanelGitInFlight) {
        m_mirrorPanelGitPending = true;
        return;
    }
    m_mirrorPanelGitInFlight = true;
    m_mirrorPanelGitPending = false;
    runOffThread<MirrorPanelGitFacts>(
        [request] { return readMirrorPanelGitFacts(request); },
        [this](MirrorPanelGitFacts facts) { applyMirrorPanelGitFacts(facts); });
}

void MainWindow::applyMirrorPanelGitFacts(const MirrorPanelGitFacts &facts)
{
    m_mirrorPanelGitInFlight = false;
    bool changed = false;
    if (facts.refsRead) {
        const auto previous = m_mirrorPanelRefs.constFind(facts.gitDir);
        changed = previous == m_mirrorPanelRefs.constEnd() ||
                  previous->routingHash != facts.refs.routingHash ||
                  previous->allRefsHash != facts.refs.allRefsHash ||
                  previous->heads != facts.refs.heads;
        m_mirrorPanelRefs.insert(facts.gitDir, facts.refs);
    }
    if (m_mirrorRelationCache.size() > 4096)
        m_mirrorRelationCache.clear();
    if (m_mirrorRelationUnknownAt.size() > 4096)
        m_mirrorRelationUnknownAt.clear();
    for (auto it = facts.relations.constBegin(); it != facts.relations.constEnd();
         ++it) {
        if (it.value() == static_cast<int>(MirrorCommitRelation::Unknown))
            m_mirrorRelationUnknownAt.insert(it.key(), facts.stamp);
        else
            m_mirrorRelationCache.insert(it.key(), it.value());
        changed = true;
    }
    if (m_mirrorPanelCounts.size() > 2048)
        m_mirrorPanelCounts.clear();
    for (auto it = facts.counts.constBegin(); it != facts.counts.constEnd(); ++it) {
        m_mirrorPanelCounts.insert(it.key(), it.value());
        changed = true;
    }
    if (m_commitIdentityCache.size() > 5000)
        m_commitIdentityCache.clear();
    for (auto it = facts.identities.constBegin();
         it != facts.identities.constEnd(); ++it) {
        m_commitIdentityCache.insert(it.key(), it.value());
        changed = true;
    }
    const bool pending = m_mirrorPanelGitPending;
    m_mirrorPanelGitPending = false;
    if (pending || (changed && facts.gitDir == m_mirrorPanelGitDir))
        loadMirrorNodesPanel();
}

void MainWindow::loadMirrorNodesPanel()
{
    if (!m_mirrorNodesTable)
        return;
    // Re-entrancy guard: every git read below — our own advert's
    // head/counts, and the `git show` that names each row's commit — pumps the
    // event loop on the GUI thread, so a queued rebuild (a roster heartbeat, a
    // /mirrors reply) can land mid-build. That nested pass clears and refills
    // the table, then this one resumes and appends its remaining rows on top of
    // it — the table ends up listing every node twice, the stragglers with only
    // the cells built after the pump. Drop the re-entrant call: the in-flight
    // build finishes a consistent table, and anything it missed lands on the
    // next rebuild (the roster re-keys this panel constantly).
    // (Mirrors the m_branchesPanelLoading guard in loadBranchesPanel.)
    if (m_mirrorNodesPanelLoading)
        return;
    QScopedValueRollback<bool> loadingGuard(m_mirrorNodesPanelLoading, true);
    const auto mergeIntoNodes = qScopeGuard([this] {
        if (mirrorNodesOnScreen())
            refreshNodesTable();
        else
            m_nodesTableStale = true;
    });
    refreshMirrorNodesRepoChooser();
    TableRepaintGuard repaintGuard(m_mirrorNodesTable);
    m_mirrorNodesTable->setSortingEnabled(false);
    m_mirrorNodesTable->setRowCount(0);
    const bool onlineOnly =
        !m_mirrorNodesOnlineOnlyCheck || m_mirrorNodesOnlineOnlyCheck->isChecked();

    const int repoIndex = mirrorNodesRepoIndex();
    if (repoIndex < 0) {
        if (m_mirrorNodesSummary)
            m_mirrorNodesSummary->setText(QStringLiteral(
                "No repository to compare yet. Add or open one to see which "
                "nodes keep a copy of it."));
        if (m_mirrorResetPinButton)
            m_mirrorResetPinButton->hide();
        setNodeDotRepoStates({});
        m_mirrorNodesTable->insertRow(0);
        auto *empty = new QTableWidgetItem(QStringLiteral(
            "No repository to compare yet. Add or open one to see which nodes "
            "keep a copy of it."));
        empty->setForeground(Theme::remapColor(QColor("#8b949e")));
        m_mirrorNodesTable->setItem(0, MirrorNodeColNode, empty);
        m_mirrorNodesTable->setSortingEnabled(true);
        updateMirrorNodeLightTimer();
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    const bool repoWorkingTree =
        !repo.previewOnly && !repo.localPath.isEmpty() &&
        QDir(repo.localPath).exists(QStringLiteral(".git"));
    const QString canonical =
        catalogOwner(repo) + "/" +
        repoSegment(repo.name, QStringLiteral("repository"));
    const QString source = repoSegment(repo.owner, QStringLiteral("owner")) + "/" +
                           repoSegment(repo.name, QStringLiteral("repository"));
    const QString sourceOwner = source.section('/', 0, 0);
    QSet<QString> activeSavedNodes;
    QSet<QString> savedNodes;
    {
        QSettings settings;
        const QJsonArray hosts = forkmesh::control::loadSavedHosts(
            settings, kHostsSetting, nullptr);
        for (const QJsonValue &value : hosts) {
            const QJsonObject host = value.toObject();
            const QString name =
                host.value(QStringLiteral("name")).toString().trimmed().toLower();
            if (name.isEmpty())
                continue;
            savedNodes.insert(name);
            if (savedHostMayBeActive(host))
                activeSavedNodes.insert(name);
        }
    }
    // Direct HTTPS health is deliberately sparse.  Overlay the latest
    // authenticated SSH snapshot for saved Hosts so every fact the machine can
    // report remains visible without weakening the direct proof fields.
    QJsonArray mirrorMetadata;
    if (m_catalogMirrorsSource == source) {
        for (const QJsonValue &value : std::as_const(m_catalogMirrorsCache)) {
            const QString node = value.toObject()
                                     .value(QStringLiteral("node"))
                                     .toString().trimmed().toLower();
            if (savedNodes.contains(node))
                mirrorMetadata.append(value);
        }
    }
    const QString sshCachePrefix = source + QLatin1Char('|');
    for (auto it = m_sshMirrorMetadataCache.constBegin();
         it != m_sshMirrorMetadataCache.constEnd(); ++it) {
        if (!it.key().startsWith(sshCachePrefix))
            continue;
        const QJsonObject ssh = it.value();
        const QString sshNode = ssh.value(QStringLiteral("node"))
                                    .toString().trimmed().toLower();
        if (sshNode.isEmpty() || !activeSavedNodes.contains(sshNode))
            continue;
        int match = -1;
        for (int i = 0; i < mirrorMetadata.size(); ++i) {
            if (mirrorMetadata.at(i).toObject()
                    .value(QStringLiteral("node")).toString().trimmed()
                    .compare(sshNode, Qt::CaseInsensitive) == 0) {
                match = i;
                break;
            }
        }
        QJsonObject merged = match >= 0 ? mirrorMetadata.at(match).toObject()
                                         : QJsonObject();
        const bool sshRepositoryState =
            forkmesh::mirrorsync::sshSnapshotProvidesRepositoryState(ssh, merged);
        for (auto field = ssh.constBegin(); field != ssh.constEnd(); ++field) {
            if (!sshRepositoryState &&
                forkmesh::mirrorsync::isRepositoryStateField(field.key()))
                continue;
            merged.insert(field.key(), field.value());
        }
        if (match >= 0)
            mirrorMetadata.replace(match, merged);
        else
            mirrorMetadata.append(merged);
    }
    {
        const QJsonArray published = QJsonDocument::fromJson(
            m_networkRepoMirrorsCache.value(source.toLower()))
                                         .object()
                                         .value(QStringLiteral("mirrors"))
                                         .toArray();
        for (const QJsonValue &value : published) {
            const QJsonObject record = value.toObject();
            const QString node = record.value(QStringLiteral("node"))
                                     .toString().trimmed().toLower();
            if (node.isEmpty())
                continue;
            forkmesh::noderole::Facts facts;
            facts.name = node;
            facts.declaredRole = record.value(QStringLiteral("role")).toString();
            facts.runtimeMode =
                record.value(QStringLiteral("runtimeMode")).toString();
            const forkmesh::noderole::Role role = nodeRoleFor(facts);
            if (role != forkmesh::noderole::Role::Core &&
                role != forkmesh::noderole::Role::Runner)
                continue;
            bool present = false;
            for (const QJsonValue &known : std::as_const(mirrorMetadata)) {
                if (known.toObject().value(QStringLiteral("node")).toString()
                        .trimmed().compare(node, Qt::CaseInsensitive) == 0) {
                    present = true;
                    break;
                }
            }
            if (!present)
                mirrorMetadata.append(record);
        }
    }
    const QString legacy = repo.owner + "/" + repo.name;
    const QString localMirror = repo.mirrorPath;
    const QString referenceGitDir =
        !localMirror.trimmed().isEmpty() ? localMirror : repo.localPath;
    const QString referenceStamp = mirrorPanelRefsStamp(referenceGitDir);
    m_mirrorPanelGitDir = referenceGitDir;
    MirrorPanelGitRequest gitRequest;
    gitRequest.gitDir = referenceGitDir;
    gitRequest.stamp = referenceStamp;
    gitRequest.identityDir = localMirror;

    const QString selfKey = mirrorSelfSnapshotKey(repo);
    const auto cached = m_mirrorSelfSnapshots.constFind(repo.mirrorPath);
    const auto cachedRefs = m_mirrorPanelRefs.constFind(referenceGitDir);
    if (cached == m_mirrorSelfSnapshots.constEnd() ||
        cachedRefs == m_mirrorPanelRefs.constEnd()) {
        if (cached == m_mirrorSelfSnapshots.constEnd())
            refreshMirrorSelfSnapshot(repo, selfKey);
        if (cachedRefs == m_mirrorPanelRefs.constEnd()) {
            gitRequest.wantRefs = true;
            requestMirrorPanelGitFacts(gitRequest);
        }
        if (m_mirrorNodesSummary)
            m_mirrorNodesSummary->setText(
                QStringLiteral("Reading this repository's mirror state…"));
        m_mirrorNodesTable->insertRow(0);
        auto *loading = new QTableWidgetItem(
            QStringLiteral("Reading this repository's mirror state…"));
        loading->setForeground(Theme::remapColor(QColor("#8b949e")));
        m_mirrorNodesTable->setItem(0, MirrorNodeColNode, loading);
        m_mirrorNodesTable->setSortingEnabled(true);
        updateMirrorNodeLightTimer();
        return;
    }
    const MirrorPanelRefs referenceRefs = cachedRefs.value();
    gitRequest.wantRefs = referenceRefs.stamp != referenceStamp;
    MirrorSelfSnapshot snapshot = *cached;
    {
        constexpr qint64 kSelfSnapshotFloorMs = 2000;
        if (snapshot.key != selfKey &&
            QDateTime::currentMSecsSinceEpoch() - snapshot.gatheredMs >
                kSelfSnapshotFloorMs)
            refreshMirrorSelfSnapshot(repo, selfKey);
    }
    MirrorAdvert selfAdvert = snapshot.advert;
    selfAdvert.ownerName = canonical;
    selfAdvert.source = source;
    selfAdvert.updatedMs = repo.lastSyncMs;
    const QString servedBranch = selfAdvert.branch;
    const QString servedCommit = snapshot.servedCommit;
    const int pendingPush = snapshot.pendingPush;

    auto matchAdvert = [&](const MemberInfo &node,
                           bool &namedOnly) -> const MirrorAdvert * {
        const MirrorAdvert *advert = nullptr;
        for (const MirrorAdvert &m : node.mirrorDetails) {
            if (m.source == source || m.ownerName == canonical ||
                m.ownerName == legacy) {
                advert = &m;
                break;
            }
        }
        namedOnly = !advert && (node.mirrors.contains(canonical) ||
                                node.mirrors.contains(legacy));
        if (node.self && (advert || namedOnly || !repo.previewOnly))
            advert = &selfAdvert; // always prefer our live local HEAD for our row
        return advert;
    };

    auto advertisedNodeName = [](const MirrorAdvert *advert) {
        if (!advert || advert->ownerName.trimmed().isEmpty())
            return QString();
        return advert->ownerName.section(QLatin1Char('/'), 0, 0).trimmed();
    };
    auto displayNodeName = [&](const MemberInfo &node,
                               const MirrorAdvert *advert) {
        QString name = advertisedNodeName(advert);
        if (name.isEmpty() && !node.nodeName.trimmed().isEmpty())
            name = node.nodeName.trimmed();
        if (name.isEmpty() && node.self)
            name = accountOwner().trimmed();
        if (name.isEmpty())
            name = node.name.trimmed();
        return name;
    };
    auto displayNodeLabel = [&](const MemberInfo &node,
                                const MirrorAdvert *advert) {
        QString label = node.nodeName.trimmed();
        if (label.isEmpty() && node.self)
            label = machineNodeName().trimmed();
        if (label.isEmpty())
            label = displayNodeName(node, advert);
        return label;
    };
    QHash<QString, QString> catalogOwnerUserByNode;
    if (!mirrorMetadata.isEmpty()) {
        for (const QJsonValue &value : std::as_const(mirrorMetadata)) {
            const QJsonObject m = value.toObject();
            const QString n = m.value("node").toString().trimmed().toLower();
            const QString ownerUser =
                m.value(QStringLiteral("ownerUser")).toString().trimmed();
            if (!n.isEmpty() && !ownerUser.isEmpty())
                catalogOwnerUserByNode.insert(n, ownerUser);
        }
    }
    auto displayOwnerName = [&](const MemberInfo &node,
                                const MirrorAdvert *advert) {
        QString owner = node.ownerUser.trimmed();
        if (owner.isEmpty())
            owner = catalogOwnerUserByNode.value(
                displayNodeName(node, advert).trimmed().toLower());
        if (owner.isEmpty())
            owner = catalogOwnerUserByNode.value(node.nodeName.trimmed().toLower());
        if (owner.isEmpty() &&
            (node.self ||
             (!node.nodeName.trimmed().isEmpty() &&
              m_profileLinkedNodes.contains(node.nodeName.trimmed(),
                                            Qt::CaseInsensitive))))
            owner = topBarUserName().trimmed();
        return owner;
    };
    auto makeOwnerCell = [](const QString &owner) {
        auto *item = new QTableWidgetItem(
            owner.trimmed().isEmpty() ? QString::fromUtf8("\xE2\x80\x94")
                                      : owner.trimmed());
        item->setForeground(Theme::remapColor(QColor("#8b949e")));
        if (!owner.trimmed().isEmpty())
            item->setToolTip(QStringLiteral("Owned by user %1").arg(owner.trimmed()));
        return item;
    };

    QList<MemberInfo> rosterNodes;
    QHash<QString, int> rosterIndexByName;
    for (const MemberInfo &node : std::as_const(m_homeRoster)) {
        bool namedOnly = false;
        const MirrorAdvert *advert = matchAdvert(node, namedOnly);
        if (!advert && !namedOnly)
            continue; // not mirroring this repo; the row loop skips these anyway
        const QString nameKey = displayNodeName(node, advert).trimmed().toLower();
        const QString labelKey = displayNodeLabel(node, advert).trimmed().toLower();
        if (!node.self && !activeSavedNodes.contains(nameKey) &&
            !activeSavedNodes.contains(labelKey))
            continue;
        if (nameKey.isEmpty()) {
            rosterNodes.append(node);
            continue;
        }
        const auto it = rosterIndexByName.constFind(nameKey);
        if (it == rosterIndexByName.constEnd()) {
            rosterIndexByName.insert(nameKey, rosterNodes.size());
            rosterNodes.append(node);
            continue;
        }
        const MemberInfo &kept = rosterNodes.at(it.value());
        bool keptNamedOnly = false;
        const MirrorAdvert *keptAdvert = matchAdvert(kept, keptNamedOnly);
        bool replace;
        if (node.self != kept.self)
            replace = node.self;
        else if (node.online != kept.online)
            replace = node.online;
        else if ((advert != nullptr) != (keptAdvert != nullptr))
            replace = advert != nullptr;
        else if (advert && keptAdvert && advert->updatedMs != keptAdvert->updatedMs)
            replace = advert->updatedMs > keptAdvert->updatedMs;
        else
            replace = QVersionNumber::fromString(node.version) >
                      QVersionNumber::fromString(kept.version);
        if (replace)
            rosterNodes[it.value()] = node;
    }

    QString sourceCommit;
    QString newestCommit;
    qint64 newestMs = -1;
    const MirrorAdvert *sourceAdvert = nullptr;
    const MirrorAdvert *newestAdvert = nullptr;
    for (const MemberInfo &node : std::as_const(rosterNodes)) {
        bool namedOnly = false;
        const MirrorAdvert *advert = matchAdvert(node, namedOnly);
        if (!advert || advert->commit.isEmpty())
            continue;
        if (advert->ownerName == source ||
            displayNodeName(node, advert).compare(sourceOwner, Qt::CaseInsensitive) == 0 ||
            (node.self && repoWorkingTree)) {
            sourceCommit = advert->commit;
            sourceAdvert = advert;
        }
        if (advert->updatedMs > newestMs) {
            newestMs = advert->updatedMs;
            newestCommit = advert->commit;
            newestAdvert = advert;
        }
    }
	QString referenceCommit =
		!sourceCommit.isEmpty() ? sourceCommit : newestCommit;
	// Repository health signs the stable routing generation (default branch +
	// tags). Compare it with the same local generation; mirrorStateHash() also
	// includes transient agent/pull heads and is the wrong hash domain here.
	QString referenceRefsSha256 = referenceRefs.routingHash;
    const QString referenceAllRefsSha256 = referenceRefs.allRefsHash;
    // Observation fetches started by this pass stamp their results with these
    // refs; a row observed against older refs is re-requested, not flagged.
    mirrorObservationSourceRefs().insert(source, referenceAllRefsSha256);
    bool staleMirrorObservations = false;
    bool mirrorObservedBehind = false;
	// If this desktop has no local repository, consensus among independently
	// signed direct proofs is still enough to identify a divergent node. It is
	// deliberately a majority, not merely the first response to finish.
	QHash<QString, int> directCommitCounts;
	QHash<QString, int> directRefsCounts;
	if (!mirrorMetadata.isEmpty()) {
		for (const QJsonValue &value : std::as_const(mirrorMetadata)) {
			const QJsonObject mirror = value.toObject();
			if (!mirror.value(QStringLiteral("endpointHealthy")).toBool() ||
				!mirror.value(QStringLiteral("endpointFresh")).toBool())
				continue;
			const QString commit =
				mirror.value(QStringLiteral("commit")).toString().trimmed();
			const QString refs = mirror.value(QStringLiteral("refsSha256"))
								 .toString().trimmed().toLower();
			if (!commit.isEmpty())
				++directCommitCounts[commit];
			if (refs.size() == 64)
				++directRefsCounts[refs];
		}
	}
	auto majorityValue = [](const QHash<QString, int> &counts) {
		QString value;
		int best = 0;
		for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
			if (it.value() > best ||
				(it.value() == best && (value.isEmpty() || it.key() < value))) {
				value = it.key();
				best = it.value();
			}
		}
		return value;
	};
	if (referenceCommit.isEmpty())
		referenceCommit = majorityValue(directCommitCounts);
	if (referenceRefsSha256.isEmpty())
		referenceRefsSha256 = majorityValue(directRefsCounts);
    const MirrorAdvert *referenceAdvert =
        sourceAdvert ? sourceAdvert : newestAdvert;
    const int refIssues = referenceAdvert ? referenceAdvert->issueCount : -1;
    const int refCommits = referenceAdvert ? referenceAdvert->commitCount : -1;
    const int refBranches = referenceAdvert ? referenceAdvert->branchCount : -1;
    const int refPulls = referenceAdvert ? referenceAdvert->pullCount : -1;
    const int refDiscussions =
        referenceAdvert ? referenceAdvert->discussionCount : -1;
    const int refArtifacts = referenceAdvert ? referenceAdvert->artifactCount : -1;
    const qint64 refSizeBytes = referenceAdvert ? referenceAdvert->sizeBytes : 0;
    const QHash<QString, QString> referenceHeads = referenceRefs.heads;
    const QString referencePrimary =
        !referenceHeads.value(servedBranch).isEmpty()
            ? referenceHeads.value(servedBranch)
            : (!referenceHeads.value(QStringLiteral("main")).isEmpty()
                   ? referenceHeads.value(QStringLiteral("main"))
                   : referenceCommit);
    // Ancestry between two commit object names in one repository is fixed for
    // all time, so this memo outlives the rebuild that filled it. It used to be
    // a local, and every one of the ~15 callers of loadMirrorNodesPanel() paid
    // the full set of `merge-base --is-ancestor` spawns again: 22 UI stalls
    // over 2026-09-01..10 (stalls.log). Keyed by repository too, because a
    // mirror that lacks the objects answers differently from one that has them.
    auto commitRelation =
        [this, &referenceGitDir, &referenceStamp,
         &gitRequest](const QString &sourceTip,
                      const QString &peerTip) -> MirrorCommitRelation {
        const QString sourceOid = sourceTip.trimmed().toLower();
        const QString peerOid = peerTip.trimmed().toLower();
        if (sourceOid.isEmpty() || peerOid.isEmpty())
            return MirrorCommitRelation::Unknown;
        if (sourceOid == peerOid)
            return MirrorCommitRelation::Same;
        const QString key = mirrorRelationKey(referenceGitDir, sourceOid, peerOid);
        const auto cached = m_mirrorRelationCache.constFind(key);
        if (cached != m_mirrorRelationCache.constEnd())
            return static_cast<MirrorCommitRelation>(cached.value());
        const auto unknown = m_mirrorRelationUnknownAt.constFind(key);
        if (unknown != m_mirrorRelationUnknownAt.constEnd() &&
            unknown.value() == referenceStamp)
            return MirrorCommitRelation::Unknown;
        const QPair<QString, QString> pair(sourceOid, peerOid);
        if (!gitRequest.relations.contains(pair))
            gitRequest.relations.append(pair);
        return MirrorCommitRelation::Unknown;
    };
    auto syncBacklog =
        [this, &commitRelation, &referenceHeads, &referencePrimary,
         &referenceGitDir, &referenceStamp, &gitRequest, refIssues,
         refCommits, refBranches, refPulls, refDiscussions](const QHash<QString, QString> &peerHeads,
                         const QString &peerPrimary, int peerIssues,
                         int peerCommits, int peerBranches, int peerPulls,
                         int peerDiscussions) {
        MirrorSyncBacklog backlog;
        const MirrorCommitRelation primaryRelation =
            commitRelation(referencePrimary, peerPrimary);
        const bool primaryNeedsSource =
            primaryRelation == MirrorCommitRelation::AheadOfSource ||
            primaryRelation == MirrorCommitRelation::Diverged;
        if (primaryNeedsSource) {
            backlog.issues =
                refIssues >= 0 && peerIssues > refIssues
                    ? peerIssues - refIssues
                    : 0;
            backlog.discussions =
                refDiscussions >= 0 && peerDiscussions > refDiscussions
                    ? peerDiscussions - refDiscussions
                    : 0;
        }

        QList<QString> uniqueTips;
        if (!peerHeads.isEmpty() && !referenceHeads.isEmpty()) {
            for (auto it = peerHeads.constBegin(); it != peerHeads.constEnd();
                 ++it) {
                const QString sourceTip = referenceHeads.value(it.key());
                MirrorCommitRelation relation =
                    commitRelation(sourceTip, it.value());
                if (sourceTip.isEmpty()) {
                    // A branch absent at the source is stale when its entire
                    // history is already in main; otherwise it contains work
                    // that the reverse reconciliation must preserve.
                    relation = commitRelation(referencePrimary, it.value());
                    if (relation == MirrorCommitRelation::BehindSource ||
                        relation == MirrorCommitRelation::Same)
                        continue;
                }
                if (relation != MirrorCommitRelation::AheadOfSource &&
                    relation != MirrorCommitRelation::Diverged &&
                    !(sourceTip.isEmpty() &&
                      relation == MirrorCommitRelation::Unknown))
                    continue;
                ++backlog.branches;
                if (it.key() == QLatin1String("forkmesh/pulls") ||
                    it.key().startsWith(QLatin1String("forkmesh/pull/")))
                    ++backlog.pulls;
                if (!uniqueTips.contains(it.value()))
                    uniqueTips.append(it.value());
            }
        } else if (primaryNeedsSource) {
            backlog.branches =
                refBranches >= 0 && peerBranches > refBranches
                    ? peerBranches - refBranches
                    : 0;
            backlog.pulls =
                refPulls >= 0 && peerPulls > refPulls
                    ? peerPulls - refPulls
                    : 0;
        }

        if (!uniqueTips.isEmpty()) {
            // Count unique commits reachable only from the peer tips. By the
            // time the source has run its automatic reverse fetch these
            // objects are local, so this is exact and does not contact a node.
            QStringList args{QStringLiteral("rev-list"),
                             QStringLiteral("--count")};
            args.append(uniqueTips);
            if (!referenceHeads.isEmpty()) {
                args.append(QStringLiteral("--not"));
                args.append(referenceHeads.values());
            }
            const auto known = m_mirrorPanelCounts.constFind(
                mirrorCountKey(referenceGitDir, referenceStamp, args));
            if (known == m_mirrorPanelCounts.constEnd()) {
                if (!gitRequest.counts.contains(args))
                    gitRequest.counts.append(args);
            } else if (known.value() >= 0) {
                backlog.commits = qMax(0, known.value());
            }
        }
        if (backlog.commits == 0 && primaryNeedsSource) {
            backlog.commits =
                refCommits >= 0 && peerCommits > refCommits
                    ? peerCommits - refCommits
                    : 1;
        }
        if ((primaryNeedsSource || backlog.pulls > 0) && refPulls >= 0 &&
            peerPulls > refPulls)
            backlog.pulls = peerPulls - refPulls;
        return backlog;
    };
    auto makeSyncBackCell = [](int count, const QString &noun) {
        auto *item = new SortTableWidgetItem;
        item->setData(kTableSortRole, double(count));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (count <= 0) {
            item->setText(QString());
            return item;
        }
        item->setText(QString::fromUtf8("\xE2\x86\x91%1").arg(count));
        item->setForeground(QColor("#d29922"));
        item->setToolTip(
            QStringLiteral("%1 %2%3 on this mirror still need%4 to sync back "
                           "to the source of truth. Automatic reconciliation "
                           "keeps retrying without discarding divergent work.")
                .arg(count)
                .arg(noun)
                .arg(count == 1 ? QString() : QStringLiteral("s"))
                .arg(count == 1 ? QStringLiteral("s") : QString()));
        return item;
    };
    auto markMismatch = [](QTableWidgetItem *item, bool mismatch,
                           const QString &refText) {
        if (!item || !mismatch)
            return;
        QFont f = item->font();
        f.setUnderline(true);
        item->setFont(f);
        const QString note =
            QStringLiteral("Doesn't match the source of truth (%1)").arg(refText);
        item->setToolTip(item->toolTip().isEmpty()
                             ? note
                             : item->toolTip() + QStringLiteral("\n") + note);
    };
    auto countMismatch = [](int value, int ref) {
        return ref >= 0 && value >= 0 && value != ref;
    };
    QHash<QString, QPair<int, int>> serveCounts;
    QHash<QString, QString> integrityByNode;
    // Per-node direct HTTPS state from /mirrors. The strongest verdict is the
    // repository-specific signed proof; endpoint health remains a useful
    // fallback for older nodes during an upgrade.
    struct RouteInfo {
        bool registered = false;
        bool healthy = false;
        bool fresh = false;
        bool directVerified = false;
        bool directReachable = false;
        bool abuseBlocked = false;
        int latencyMs = 0;
        QString endpoint;
        QString integrity;
        QString reason;
    };
    QHash<QString, RouteInfo> routeByNode;
    QHash<QString, QJsonObject> metadataByNode;
    if (!mirrorMetadata.isEmpty()) {
        for (const QJsonValue &value : std::as_const(mirrorMetadata)) {
            const QJsonObject m = value.toObject();
            const QString n = m.value("node").toString().trimmed().toLower();
            if (!n.isEmpty()) {
                metadataByNode.insert(n, m);
                serveCounts.insert(n, {m.value("clonesServed").toInt(-1),
                                       m.value("websiteServed").toInt(-1)});
                integrityByNode.insert(n, m.value("integrity").toString());
                RouteInfo route;
                route.endpoint = m.value("endpoint").toString().trimmed();
                route.registered = !route.endpoint.isEmpty();
                route.healthy = m.value("endpointHealthy").toBool();
                route.fresh = m.value("endpointFresh").toBool();
                route.directVerified = m.value("directVerified").toBool();
                route.directReachable = m.value("directReachable").toBool();
                route.abuseBlocked = m.value("abuseBlocked").toBool();
                route.latencyMs = m.value("directLatencyMs").toInt(
                    m.value("latencyMs").toInt(0));
                route.integrity = m.value("directIntegrity").toString(
                    m.value("endpointIntegrity").toString());
                route.reason = m.value("directReason").toString();
                routeByNode.insert(n, route);
            }
        }
    }
    auto makeRouteCell = [](const RouteInfo &route) -> SortTableWidgetItem * {
        QString text = QString::fromUtf8("\xE2\x80\x94");
        double sortValue = 0;
        QString tip = QStringLiteral(
            "No independent direct-HTTPS route is registered for this node.");
        if (route.abuseBlocked) {
            text = QStringLiteral("blocked");
            sortValue = 1;
            tip = QStringLiteral("Endpoint blocked for abuse.");
        } else if (route.directVerified) {
            text = route.latencyMs > 0
                       ? QStringLiteral("direct · %1 ms").arg(route.latencyMs)
                       : QStringLiteral("direct verified");
            sortValue = 5;
            tip = QStringLiteral(
                      "Fresh signed repository proof from the independent "
                      "HTTPS origin (%1).")
                      .arg(route.endpoint);
        } else if (route.registered) {
            if (route.healthy && route.fresh) {
                text = route.latencyMs > 0
                           ? QStringLiteral("HTTPS · %1 ms").arg(route.latencyMs)
                           : QStringLiteral("HTTPS up");
                sortValue = 4;
                tip = QStringLiteral("Direct HTTPS endpoint healthy (%1)")
                          .arg(route.endpoint);
            } else if (route.healthy || route.directReachable) {
                text = QStringLiteral("HTTPS · stale");
                sortValue = 3;
                tip = QStringLiteral(
                          "The direct endpoint answered, but the "
                          "node has not been seen recently (%1)")
                          .arg(route.endpoint);
            } else {
                text = QStringLiteral("HTTPS down");
                sortValue = 2;
                tip = QStringLiteral("Direct HTTPS endpoint is registered but "
                                     "failing its health proof (%1)")
                          .arg(route.endpoint);
            }
            if (!route.integrity.isEmpty() &&
                route.integrity != QLatin1String("ok"))
                tip += QStringLiteral("\nIntegrity: %1").arg(route.integrity);
            if (!route.reason.isEmpty() && route.reason != QLatin1String("verified"))
                tip += QStringLiteral("\nReason: %1").arg(route.reason);
        }
        auto *item = new SortTableWidgetItem(text);
        item->setData(kTableSortRole, sortValue);
        item->setToolTip(tip);
        return item;
    };
    auto reachabilityReasonHint = [](const QString &reason) -> QString {
        if (reason == QLatin1String("endpoint_not_registered"))
            return QString::fromUtf8(
                "This node has not registered a direct-HTTPS endpoint, so the "
                "relay has no way to fetch README.md from it. Configure its "
                "independent HTTPS origin and the check goes live.");
        if (reason == QLatin1String("endpoint_blocked"))
            return QStringLiteral("Its endpoint is blocked for abuse.");
        if (reason == QLatin1String("state_pin_not_admitted"))
            return QStringLiteral(
                "It is outside the source's signed state-pin window, so reads "
                "are not routed to it until it finishes syncing.");
        if (reason == QLatin1String("repository_proof_failed"))
            return QStringLiteral(
                "Its endpoint answered, but it does not advertise a matching "
                "signed refs digest for this repository.");
        if (reason == QLatin1String("endpoint_stale"))
            return QString::fromUtf8(
                "Its endpoint lease is stale \xE2\x80\x94 the node has not "
                "passed a signed health probe recently.");
        if (reason == QLatin1String("router_unavailable"))
            return QStringLiteral("The relay could not sign the probe request.");
        if (reason == QLatin1String("request_failed"))
            return QStringLiteral("The request to its endpoint failed outright.");
        if (reason == QLatin1String("probe_endpoint_unavailable"))
            return QStringLiteral(
                "The relay did not answer the reachability probe for this node.");
        return QString();
    };
    auto makeReachabilityCell = [this, &source, &repo, &reachabilityReasonHint](
                                    const QString &nodeName, bool selfNode)
        -> SortTableWidgetItem * {
        const QString key = source + QLatin1Char('|') +
                            nodeName.trimmed().toLower();
        const QJsonObject probe = m_mirrorReachabilityCache.value(key);
        const bool pending = probe.isEmpty();
        const bool loaded = probe.value(QStringLiteral("readmeLoaded")).toBool();
        const QString reason = probe.value(QStringLiteral("reason")).toString();
        QString text;
        QString tip;
        double sortValue = 0;
        const bool selfLocalReadme =
            selfNode && !loaded && !repo.localPath.isEmpty() &&
            QFileInfo::exists(repo.localPath + QStringLiteral("/README.md"));
        if (selfLocalReadme) {
            text = QStringLiteral("README \u00b7 local");
            tip = QStringLiteral(
                      "README.md is present in this machine's working copy "
                      "(%1).")
                      .arg(repo.localPath);
            sortValue = 3;
        } else if (selfNode) {
            // This machine is the source, not one of its own mirrors, so the
            // relay has no mirror row to probe: the reachability endpoint
            // answered 404 every single time it was asked (38/38 in a 21.5h
            // network-log window, once every two minutes, forever). The row is
            // now answered from local knowledge instead of from a request that
            // cannot succeed - see loadMirrorNodesPanel, which no longer
            // enqueues the self node for probing.
            text = QStringLiteral("Source \u00b7 this machine");
            tip = repo.localPath.isEmpty()
                      ? QStringLiteral(
                            "This machine is the source for this repository, "
                            "not a mirror of it, so there is no mirror endpoint "
                            "to probe. No working copy is registered here.")
                      : QStringLiteral(
                            "This machine is the source for this repository, "
                            "not a mirror of it, so there is no mirror endpoint "
                            "to probe. Working copy: %1")
                            .arg(repo.localPath);
            sortValue = 3;
        } else if (pending) {
            text = QStringLiteral("Checking\u2026");
            tip = QStringLiteral(
                "Requesting README.md from this exact mirror; failover is disabled.");
            sortValue = 1;
        } else if (probe.value(QStringLiteral("repositoryVerified")).toBool()) {
            const int latency = probe.value(QStringLiteral("latencyMs")).toInt();
            text = latency > 0
                       ? QStringLiteral("Repository \u00b7 %1 ms").arg(latency)
                       : QStringLiteral("Repository verified");
            tip = QStringLiteral(
                "This mirror answered its signed repository-specific health "
                "challenge directly; the relay Worker was not used.");
            sortValue = 3;
        } else if (loaded) {
            const int latency = probe.value(QStringLiteral("latencyMs")).toInt();
            text = latency > 0
                       ? QStringLiteral("README \u00b7 %1 ms").arg(latency)
                       : QStringLiteral("README loaded");
            tip = QStringLiteral(
                "This exact mirror returned README.md successfully (HTTP %1).")
                      .arg(probe.value(QStringLiteral("status")).toInt(200));
            sortValue = 3;
        } else if (!probe.value(QStringLiteral("endpointRegistered"))
                        .toBool(true)) {
            text = QStringLiteral("Relay only");
            tip = QStringLiteral(
                      "No direct-HTTPS endpoint is registered for this node, so "
                      "README.md cannot be fetched from it directly; it serves "
                      "through relay sync.\n%1")
                      .arg(reachabilityReasonHint(
                          QStringLiteral("endpoint_not_registered")));
            sortValue = 1.5;
        } else {
            text = QStringLiteral("Unavailable");
            const int status = probe.value(QStringLiteral("status")).toInt();
            tip = QStringLiteral(
                "This exact mirror did not return README.md; no other mirror was used.");
            const QString hint = reachabilityReasonHint(reason);
            if (!hint.isEmpty())
                tip += QStringLiteral("\n%1").arg(hint);
            if (status > 0)
                tip += QStringLiteral("\nHTTP %1").arg(status);
            if (!reason.isEmpty())
                tip += QStringLiteral("\n(%1)").arg(reason);
            sortValue = 2;
        }
        auto *item = new SortTableWidgetItem(text);
        item->setData(kTableSortRole, sortValue);
        item->setToolTip(tip);
        return item;
    };
    auto makeServeCountCell = [](int value, const QString &tip) -> SortTableWidgetItem * {
        auto *item = new SortTableWidgetItem(
            value >= 0 ? formatCount(value) : QString::fromUtf8("\xE2\x80\x94"));
        item->setData(kTableSortRole, double(value));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (value >= 0)
            item->setToolTip(tip);
        return item;
    };
    auto clonesTip = [](int n) {
        return QString::fromUtf8("Provided %1 clone%2").arg(n).arg(n == 1 ? "" : "s");
    };
    auto websiteTip = [](int n) {
        return QString::fromUtf8("Served the website %1 time%2")
            .arg(n)
            .arg(n == 1 ? "" : "s");
    };
    int count = 0;
    qint64 totalBytes = 0;     // data mirrored across every node in this group
    qint64 maxRepoBytes = 0;   // best (largest, == most complete) copy seen
    bool weAreSource = false;  // this node holds the source-of-truth copy
    int outOfSyncPeers = 0;    // other nodes whose served state != the source
	int directVerifiedPeers = 0; // signed direct proof, no Worker/catalog hop
    QSet<QString> shownNames;
    QSet<QString> shownIds;
    QVector<MirrorNodeDot> activityDots;
    // How many repositories each node mirrors in total. Unlike every other
    // count in this table, this one is a fact about the *node*, not about this
    // repo: it tells a full fleet mirror apart from a node that happens to
    // carry this one repository. Nodes advertise the list in their presence
    // frames, so it is known for anyone in the roster — including nodes this
    // panel does not give a row of their own.
    QHash<QString, int> reposByNode;
    QSet<QString> localPublicRepositories;
    for (const RepositoryRecord &candidate : std::as_const(m_repositories)) {
        if (candidate.previewOnly || candidate.isPrivate ||
            !candidate.publishToNetwork)
            continue;
        localPublicRepositories.insert(
            (catalogOwner(candidate) + QLatin1Char('/') +
             repoSegment(candidate.name, QStringLiteral("repository")))
                .toLower());
    }
    const int localRepositoryCount = localPublicRepositories.size();
    // Presence may not have completed yet (and a source-only desktop does not
    // need to advertise itself as a separate mirror). The application still
    // knows its own enabled public repositories exactly, so never leave the
    // source row as an em dash while waiting for roster metadata.
    for (const QString &key : {machineNodeName().trimmed().toLower(),
                               accountOwner().trimmed().toLower(),
                               topBarUserName().trimmed().toLower()}) {
        if (!key.isEmpty())
            reposByNode.insert(key, localRepositoryCount);
    }
    for (const MemberInfo &member : std::as_const(m_homeRoster)) {
        const int held = qMax(member.mirrors.size(), member.mirrorDetails.size());
        if (held <= 0)
            continue; // never advertised a list; -1/em-dash rather than a fake 0
        for (const QString &key : {member.nodeName.trimmed().toLower(),
                                   member.name.trimmed().toLower()}) {
            if (!key.isEmpty())
                reposByNode.insert(key, qMax(reposByNode.value(key, 0), held));
        }
    }
    for (const QJsonValue &value : std::as_const(mirrorMetadata)) {
        const QJsonObject mirror = value.toObject();
        const QString key =
            mirror.value(QStringLiteral("node")).toString().trimmed().toLower();
        const int reported = mirror.contains(QStringLiteral("repositoryCount"))
                                 ? mirror.value(QStringLiteral("repositoryCount"))
                                       .toInt(-1)
                                 : mirror.value(
                                             QStringLiteral("publicRepositoryCount"))
                                       .toInt(-1);
        if (!key.isEmpty() && reported >= 0)
            reposByNode.insert(key, qMax(reposByNode.value(key, 0), reported));
    }
    auto makeReposCell = [](int n) -> SortTableWidgetItem * {
        auto *item = new SortTableWidgetItem(
            n >= 0 ? QString::number(n) : QString::fromUtf8("\xE2\x80\x94"));
        item->setData(kTableSortRole, double(n));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(
            n >= 0 ? QStringLiteral("Mirroring %1 repositor%2 in total")
                         .arg(n)
                         .arg(n == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
                   : QStringLiteral(
                         "This node has not advertised the list of repositories "
                         "it mirrors."));
        return item;
    };
    auto makeCountCell = [](int n, const QString &singular,
                            const QString &plural) -> SortTableWidgetItem * {
        auto *item = new SortTableWidgetItem(
            n >= 0 ? QString::number(n) : QString::fromUtf8("\xE2\x80\x94"));
        item->setData(kTableSortRole, double(n));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (n >= 0)
            item->setToolTip(
                QStringLiteral("%1 %2").arg(n).arg(n == 1 ? singular : plural));
        return item;
    };
    auto makeGitDatabaseCell = [](qint64 bytes) -> SortTableWidgetItem * {
        auto *item = new SortTableWidgetItem(
            bytes > 0 ? formatByteSize(bytes)
                      : QString::fromUtf8("\xE2\x80\x94"));
        item->setData(kTableSortRole, double(bytes));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(
            bytes > 0
                ? QStringLiteral(
                      "Physical bare Git object database on this node: %1. "
                      "Packfiles, unreachable objects, and compression can make "
                      "this differ from canonical repository content size.")
                      .arg(formatByteSize(bytes))
                : QStringLiteral(
                      "This node has not reported its physical Git object "
                      "database size."));
        return item;
    };
    auto resolveCommitIdentity = [this, &gitRequest](
                                     const QString &commit,
                                     const CommitIdentity &advertised) {
        if (!advertised.subject.isEmpty() || !advertised.author.isEmpty())
            return advertised;
        if (commit.isEmpty())
            return CommitIdentity{};
        const auto cached = m_commitIdentityCache.constFind(commit);
        if (cached != m_commitIdentityCache.constEnd())
            return cached.value();
        if (!gitRequest.identities.contains(commit))
            gitRequest.identities.append(commit);
        return CommitIdentity{};
    };
    auto setCommitIdentityCells = [this](int row,
                                         const CommitIdentity &identity) {
        const QString dash = QString::fromUtf8("\xE2\x80\x94");
        const QString subject = identity.subject.trimmed();
        auto *messageItem = new QTableWidgetItem(
            subject.isEmpty()
                ? dash
                : (subject.size() > 72
                       ? subject.left(71) + QString::fromUtf8("\xE2\x80\xA6")
                       : subject));
        if (!subject.isEmpty()) {
            messageItem->setToolTip(
                identity.committedAtMs > 0
                    ? subject + "\n" +
                          QDateTime::fromMSecsSinceEpoch(identity.committedAtMs)
                              .toString(Qt::ISODate)
                    : subject);
        }
        m_mirrorNodesTable->setItem(row, MirrorNodeColMessage, messageItem);
        const QString author = identity.author.trimmed();
        auto *authorItem = new QTableWidgetItem(author.isEmpty() ? dash : author);
        if (!author.isEmpty())
            authorItem->setToolTip(
                QStringLiteral("Authored the node's latest commit"));
        m_mirrorNodesTable->setItem(row, MirrorNodeColAuthor, authorItem);
    };
    int pinRejectedNodes = 0;
    auto markPinRejected = [&pinRejectedNodes](QTableWidgetItem *item, bool isSelf) {
        ++pinRejectedNodes;
        item->setText(item->text() +
                      QString::fromUtf8("  \xE2\x9A\xA0 failing integrity pin"));
        item->setForeground(QColor("#f85149"));
        const QString note =
            isSelf
                ? QString::fromUtf8(
                      "Clones of this repo are being rejected: the relay's pinned "
                      "hash no longer matches the refs this node serves. As the "
                      "source of truth this node re-signs its current refs "
                      "automatically within seconds; \xE2\x80\x9CReset integrity "
                      "pin\xE2\x80\x9D above forces it now.")
                : QString::fromUtf8(
                      "Clones from this node are being rejected: the refs it serves "
                      "match no state the source of truth attested (integrity pin). "
                      "This clears once the node syncs \xE2\x80\x94 or, if the node "
                      "is already up to date, when the owner resets the pin.");
        item->setToolTip(item->toolTip().isEmpty()
                             ? note
                             : item->toolTip() + QStringLiteral("\n\n") + note);
    };
    for (const MemberInfo &node : std::as_const(rosterNodes)) {
        bool namedOnly = false;
        const MirrorAdvert *advert = matchAdvert(node, namedOnly);
        if (!advert && !namedOnly)
            continue;

        const bool online = node.self ? (m_backend != nullptr) : node.online;
        const bool integrityFailing =
            integrityByNode.value(displayNodeName(node, advert).trimmed().toLower()) ==
                QLatin1String("rejected");
        if (onlineOnly && !online)
            continue;
        const QString nodeDisplay = displayNodeName(node, advert);
        const QString nodeLabel = displayNodeLabel(node, advert);
        QJsonObject nodeMetadata =
            metadataByNode.value(nodeDisplay.trimmed().toLower());
        if (nodeMetadata.isEmpty())
            nodeMetadata =
                metadataByNode.value(nodeLabel.trimmed().toLower());
        // Metadata observed against older source refs says nothing about
        // whether this node has followed the source since then.
        const bool metadataCurrent =
            forkmesh::mirrorsync::observationMatchesSourceRefs(
                nodeMetadata, referenceAllRefsSha256);
        const bool metadataRefreshable =
            nodeMetadata.value(QStringLiteral("endpointHealthy")).toBool() ||
            nodeMetadata.value(QStringLiteral("sshHealthy")).toBool();
        if (!metadataCurrent && metadataRefreshable && !node.self)
            staleMirrorObservations = true;
        QString nodeAllRefs = advert ? advert->refsFingerprint.trimmed().toLower()
                                     : QString();
        if (nodeAllRefs.isEmpty() && metadataCurrent)
            nodeAllRefs = nodeMetadata
                              .value(QStringLiteral("allRefsSha256"))
                              .toString().trimmed().toLower();
        const QHash<QString, QString> nodeHeads = branchHeadsFromJson(
            nodeMetadata.value(QStringLiteral("branchHeads")).toObject());
        const bool allRefsDiffer =
            nodeAllRefs.size() == 64 && referenceAllRefsSha256.size() == 64 &&
            nodeAllRefs != referenceAllRefsSha256;
        const bool headsDiffer =
            metadataCurrent && !nodeHeads.isEmpty() && !referenceHeads.isEmpty() &&
            nodeHeads != referenceHeads;
        const bool behind =
            online && advert &&
            ((!advert->commit.isEmpty() && !referenceCommit.isEmpty() &&
              advert->commit != referenceCommit) ||
             allRefsDiffer || headsDiffer);
        // Only a mismatch in probed metadata can clear on a re-probe; a peer's
        // own stale presence advert does not warrant polling the fleet.
        if (!node.self && metadataCurrent && metadataRefreshable &&
            (allRefsDiffer || headsDiffer))
            mirrorObservedBehind = true;
        // A host deleted from the Hosts page keeps its relay presence and its
        // last signed advert until their leases lapse. We know better: this
        // desktop is what deleted it. Hide it again the moment it publishes
        // something newer than the deletion.
        if (!node.self &&
            (mirrorNodeWasDecommissioned(nodeDisplay,
                                         advert ? advert->updatedMs : 0) ||
             mirrorNodeWasDecommissioned(nodeLabel,
                                         advert ? advert->updatedMs : 0)))
            continue;
        const QString ownerDisplay = displayOwnerName(node, advert);
        shownNames.insert(nodeDisplay.trimmed().toLower());
        shownNames.insert(nodeLabel.trimmed().toLower());
        if (!node.id.isEmpty())
            shownIds.insert(node.id);

        const bool isSource =
            (advert && advert->ownerName == source) ||
            nodeDisplay.compare(sourceOwner, Qt::CaseInsensitive) == 0 ||
            (node.self && repoWorkingTree);

        const int row = m_mirrorNodesTable->rowCount();
        m_mirrorNodesTable->insertRow(row);

        if (online || integrityFailing)
            activityDots.append(
                {node.id, nodeLabel, online, node.self, behind, integrityFailing});
        auto *nameItem = new SortTableWidgetItem(
            nodeLabel + (node.self ? QStringLiteral("  (you)") : QString()) +
            (isSource ? QString::fromUtf8("  \xE2\x98\x85 source of truth")
                      : QString()));
        const int light = integrityFailing ? 2 : behind ? 1 : 0;
        const QColor lightColor = integrityFailing ? QColor("#f85149")
                                  : !online          ? Theme::remapColor(QColor("#8b949e"))
                                  : behind           ? QColor("#d29922")
                                                     : QColor("#3fb950");
        nameItem->setIcon(
            QIcon(nodeStatusLightPixmap(lightColor, 14, 0.0, light != 0)));
        nameItem->setData(kNodeLightRole, light);
        nameItem->setData(Qt::UserRole, node.id);
        {
            QStringList keys;
            for (const QString &candidate :
                 {nodeLabel, nodeDisplay, node.name, node.nodeName}) {
                const QString key = candidate.trimmed().toLower();
                if (!key.isEmpty() && !keys.contains(key))
                    keys.append(key);
            }
            nameItem->setData(kMirrorNodeKeysRole, keys);
            nameItem->setData(kMirrorNodeSelfRole, node.self);
            nameItem->setData(kMirrorNodeIdRole, node.id);
            nameItem->setData(kMirrorNodeOnlineRole, online);
        }
        nameItem->setData(kTableSortRole,
                          (isSource ? QStringLiteral("0") : QStringLiteral("1")) +
                              nodeLabel.toLower());
        nameItem->setToolTip(
            isSource ? QString::fromUtf8("Source of truth \xC2\xB7 %1")
                           .arg(online ? "online" : "offline")
                     : (online ? (behind ? QString::fromUtf8(
                                               "Online \xC2\xB7 out of sync")
                                         : QStringLiteral("Online now"))
                               : QStringLiteral("Offline")));
        if (!node.name.trimmed().isEmpty() &&
            node.name.compare(nodeLabel, Qt::CaseInsensitive) != 0)
            nameItem->setToolTip(nameItem->toolTip() + QStringLiteral("\nChat: ") +
                                 node.name.trimmed());
        if (integrityFailing)
            markPinRejected(nameItem, node.self);
        m_mirrorNodesTable->setItem(row, MirrorNodeColNode, nameItem);
        {
            forkmesh::noderole::Facts facts;
            facts.name = nodeDisplay;
            facts.self = node.self;
            facts.savedHost =
                activeSavedNodes.contains(nodeDisplay.trimmed().toLower()) ||
                activeSavedNodes.contains(nodeLabel.trimmed().toLower());
            facts.declaredRole =
                nodeMetadata.value(QStringLiteral("role")).toString();
            facts.runtimeMode =
                nodeMetadata.value(QStringLiteral("runtimeMode")).toString();
            m_mirrorNodesTable->setItem(row, MirrorNodeColRole,
                                        makeNodeRoleCell(nodeRoleFor(facts)));
        }
        m_mirrorNodesTable->setItem(row, MirrorNodeColOwner,
                                    makeOwnerCell(ownerDisplay));
        int nodeRepos = qMax(node.mirrors.size(), node.mirrorDetails.size());
        if (node.self)
            nodeRepos = qMax(nodeRepos, localRepositoryCount);
        if (nodeRepos <= 0 && !node.self)
            nodeRepos = reposByNode.value(nodeDisplay.trimmed().toLower(), -1);
        m_mirrorNodesTable->setItem(row, MirrorNodeColRepos,
                                    makeReposCell(nodeRepos));

        QString commitText = QString::fromUtf8("\xE2\x80\x94");
        QString commitTip;
        CommitIdentity commitIdentity;
        if (advert && !advert->commit.isEmpty()) {
            commitText = advert->commit.left(10);
            if (!advert->branch.isEmpty())
                commitText += "  (" + advert->branch + ")";
            commitTip = advert->commit;
            commitIdentity =
                resolveCommitIdentity(advert->commit, advert->commitIdentity);
            if (commitIdentity.committedAtMs > 0)
                commitText += QString::fromUtf8(" \xC2\xB7 ") +
                              formatShortRelativeTime(
                                  commitIdentity.committedAtMs / 1000) +
                              QStringLiteral(" ago");
            if (!commitIdentity.subject.isEmpty())
                commitTip = commitIdentity.subject + "\n" + advert->commit;
        }
        setCommitIdentityCells(row, commitIdentity);
        auto *commitItem = new QTableWidgetItem(commitText);
        commitItem->setToolTip(commitTip);
        markMismatch(commitItem,
                     advert && !advert->commit.isEmpty() &&
                         !referenceCommit.isEmpty() &&
                         advert->commit != referenceCommit,
                     referenceCommit.left(10));
        m_mirrorNodesTable->setItem(row, MirrorNodeColCommit, commitItem);

        const qint64 syncedSecs = advert ? advert->updatedMs / 1000 : 0;
        auto *syncedItem = new SortTableWidgetItem(
            syncedSecs > 0 ? formatShortRelativeTime(syncedSecs) + " ago"
                           : QString::fromUtf8("\xE2\x80\x94"));
        syncedItem->setData(kTableSortRole, double(syncedSecs));
        if (syncedSecs > 0)
            syncedItem->setToolTip(
                QDateTime::fromSecsSinceEpoch(syncedSecs).toString(Qt::ISODate));
        // Behind-but-online node: tag the cell so MirrorSyncDelegate draws a
        // pac-man counting down to its next heartbeat/re-sync. In-sync and
        // offline rows carry no anchor and render as plain text. (`behind` is
        // computed above so the Node dot can also go amber for it.) The source
        // broadcasts an instant mirror-update signal over the websocket when it
        // advances, so a behind node normally converges in seconds; the
        // heartbeat re-sync the countdown shows is the fallback for a node that
        // missed the ephemeral signal.
        if (behind) {
            syncedItem->setData(kPacmanAnchorRole,
                                static_cast<qlonglong>(advert->updatedMs));
            syncedItem->setData(
                kPacmanIntervalRole,
                static_cast<qlonglong>(
                    m_mirrorSyncTimer ? m_mirrorSyncTimer->interval()
                                      : mirrorSyncIntervalMs()));
            syncedItem->setToolTip(QString::fromUtf8(
                "Behind the source \xC2\xB7 signalled to sync now; its next "
                "heartbeat is the fallback"));
        }
        if (node.self && pendingPush > 0) {
            syncedItem->setText(QString::fromUtf8("\xE2\x86\x91 %1 to push")
                                    .arg(pendingPush));
            syncedItem->setToolTip(
                QString::fromUtf8("%1 local commit%2 not yet copied to this "
                                  "node's served mirror")
                    .arg(pendingPush)
                    .arg(pendingPush == 1 ? "" : "s"));
        }
        m_mirrorNodesTable->setItem(row, MirrorNodeColSynced, syncedItem);

        if (node.self && isSource)
            weAreSource = true;
        if (!node.self && behind)
            ++outOfSyncPeers;

        const qint64 publishedNodeBytes = advert ? advert->sizeBytes : 0;
        const bool usesCanonicalSize =
            refSizeBytes > 0 && advert && !referenceCommit.isEmpty() &&
            advert->commit == referenceCommit;
        const qint64 nodeBytes =
            usesCanonicalSize ? refSizeBytes : publishedNodeBytes;
        auto *sizeItem = new SortTableWidgetItem(
            nodeBytes > 0 ? formatByteSize(nodeBytes)
                          : QString::fromUtf8("\xE2\x80\x94"));
        sizeItem->setData(kTableSortRole, double(nodeBytes));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (usesCanonicalSize)
            sizeItem->setToolTip(QStringLiteral(
                "Canonical repository content size from the source of truth. "
                "Physical Git pack size can differ without changing content."));
        m_mirrorNodesTable->setItem(row, MirrorNodeColSize, sizeItem);
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColGitDatabase,
            makeGitDatabaseCell(publishedNodeBytes));
        if (nodeBytes > 0) {
            totalBytes += nodeBytes;
            maxRepoBytes = qMax(maxRepoBytes, nodeBytes);
        }

        const int nodeIssues = advert ? advert->issueCount : -1;
        auto *issuesItem = new SortTableWidgetItem(
            nodeIssues >= 0 ? QString::number(nodeIssues)
                            : QString::fromUtf8("\xE2\x80\x94"));
        issuesItem->setData(kTableSortRole, double(nodeIssues));
        issuesItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (nodeIssues >= 0)
            issuesItem->setToolTip(
                QString::fromUtf8("Mirroring %1 issue%2")
                    .arg(nodeIssues)
                    .arg(nodeIssues == 1 ? "" : "s"));
        markMismatch(issuesItem, countMismatch(nodeIssues, refIssues),
                     QString::number(refIssues));
        m_mirrorNodesTable->setItem(row, MirrorNodeColIssues, issuesItem);

        const int nodeCommits = advert ? advert->commitCount : -1;
        const int nodeBranches = !nodeHeads.isEmpty()
                                     ? nodeHeads.size()
                                     : (advert ? advert->branchCount : -1);
        const int nodePulls = advert ? advert->pullCount : -1;
        const int nodeDiscussions = advert ? advert->discussionCount : -1;
        auto *commitsItem = makeCountCell(nodeCommits, "commit", "commits");
        markMismatch(commitsItem, countMismatch(nodeCommits, refCommits),
                     QString::number(refCommits));
        m_mirrorNodesTable->setItem(row, MirrorNodeColCommits, commitsItem);
        auto *branchesItem = makeCountCell(nodeBranches, "branch", "branches");
        markMismatch(branchesItem, countMismatch(nodeBranches, refBranches),
                     QString::number(refBranches));
        m_mirrorNodesTable->setItem(row, MirrorNodeColBranches, branchesItem);
        auto *pullsItem =
            makeCountCell(nodePulls, "pull request", "pull requests");
        markMismatch(pullsItem, countMismatch(nodePulls, refPulls),
                     QString::number(refPulls));
        m_mirrorNodesTable->setItem(row, MirrorNodeColPulls, pullsItem);
        auto *discussionsItem =
            makeCountCell(nodeDiscussions, "discussion", "discussions");
        markMismatch(discussionsItem,
                     countMismatch(nodeDiscussions, refDiscussions),
                     QString::number(refDiscussions));
        m_mirrorNodesTable->setItem(row, MirrorNodeColDiscussions,
                                    discussionsItem);

        MirrorSyncBacklog backlog;
        if (!isSource && advert) {
            backlog = syncBacklog(
                nodeHeads,
                advert->commit, nodeIssues, nodeCommits, nodeBranches,
                nodePulls, nodeDiscussions);
        }
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColIssuesToSource,
            makeSyncBackCell(backlog.issues, QStringLiteral("issue")));
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColCommitsToSource,
            makeSyncBackCell(backlog.commits, QStringLiteral("commit")));
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColBranchesToSource,
            makeSyncBackCell(backlog.branches, QStringLiteral("branch")));
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColPullsToSource,
            makeSyncBackCell(backlog.pulls, QStringLiteral("pull request")));
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColDiscussionsToSource,
            makeSyncBackCell(backlog.discussions,
                             QStringLiteral("discussion")));

        auto *healthItem =
            makeNodeHealthCell(node.diagnostics, node.diagnosticsMs,
                               QDateTime::currentMSecsSinceEpoch());
        healthItem->setData(
            kMirrorDiagnosticPromptRole,
            mirrorDiagnosticPrompt(source, nodeLabel, node.id, ownerDisplay,
                                   node.platform, node.version, node.diagnostics,
                                   node.diagnosticsMs));
        healthItem->setToolTip(
            healthItem->toolTip() +
            QStringLiteral("\n\nDouble-click or press Enter to draft a troubleshooting prompt."));
        m_mirrorNodesTable->setItem(row, MirrorNodeColHealth, healthItem);

        m_mirrorNodesTable->setItem(row, MirrorNodeColCpu,
                                    makeCpuUsageCell(node.cpuPercent));
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColRam,
            makeByteUsageCell("RAM", node.memUsedBytes, node.memTotalBytes));
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColDisk,
            makeByteUsageCell("Disk", node.diskUsedBytes, node.diskTotalBytes));

        m_mirrorNodesTable->setItem(
            row, MirrorNodeColPlatform,
            new QTableWidgetItem(node.platform.isEmpty()
                                     ? QString::fromUtf8("\xE2\x80\x94")
                                     : node.platform));
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColVersion,
            new QTableWidgetItem(node.version.isEmpty()
                                     ? QString::fromUtf8("\xE2\x80\x94")
                                     : node.version));
        auto *idItem = new QTableWidgetItem(
            node.id.left(12) + (node.id.size() > 12 ? QString::fromUtf8("\xE2\x80\xA6")
                                                    : QString()));
        idItem->setToolTip(node.id);
        m_mirrorNodesTable->setItem(row, MirrorNodeColId, idItem);

        m_mirrorNodesTable->setItem(
            row, MirrorNodeColRoute,
            makeRouteCell(routeByNode.value(nodeDisplay.trimmed().toLower())));

        int nodeClones = -1, nodeWebsite = -1;
        {
            const QPair<int, int> s =
                serveCounts.value(nodeDisplay.trimmed().toLower(), {-1, -1});
            nodeClones = s.first;
            nodeWebsite = s.second;
        }
        m_mirrorNodesTable->setItem(row, MirrorNodeColClones,
                                    makeServeCountCell(nodeClones, clonesTip(nodeClones)));
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColWebsite,
            makeServeCountCell(nodeWebsite, websiteTip(nodeWebsite)));
        const int nodeArtifacts = advert ? advert->artifactCount : -1;
        auto *artifactsItem =
            makeCountCell(nodeArtifacts, "artifact", "artifacts");
        markMismatch(artifactsItem, countMismatch(nodeArtifacts, refArtifacts),
                     QString::number(refArtifacts));
        m_mirrorNodesTable->setItem(row, MirrorNodeColArtifacts, artifactsItem);
        const QString reachabilityNode = nodeDisplay.trimmed().toLower();
        m_mirrorNodesTable->setItem(
            row, MirrorNodeColReachability,
            makeReachabilityCell(reachabilityNode, node.self));
        ++count;
    }

    QVector<MirrorNodeDot> catalogOnlyDots;
    if (!mirrorMetadata.isEmpty()) {
        for (const QJsonValue &value : std::as_const(mirrorMetadata)) {
            const QJsonObject m = value.toObject();
            const QString nodeName = m.value("node").toString().trimmed();
            if (nodeName.isEmpty() || shownNames.contains(nodeName.toLower()))
                continue;
            const QString catalogId = m.value("id").toString().trimmed();
            if (!catalogId.isEmpty() && shownIds.contains(catalogId))
                continue;
            // Same tombstone check as the roster rows above: /mirrors reports a
            // destroyed server as online and healthy for the whole of its
            // endpoint freshness lease.
            if (mirrorNodeWasDecommissioned(
                    nodeName, qint64(m.value("lastSync").toDouble())))
                continue;
            shownNames.insert(nodeName.toLower());
            const bool isSource =
                nodeName.compare(sourceOwner, Qt::CaseInsensitive) == 0;
            const bool roomOnline =
                m.value("status").toString() == QLatin1String("online");
            // A managed/headless mirror serves entirely through its own signed
            // direct-HTTPS origin and can lose its relay-room presence (a
            // reconnect, a long gap between heartbeats) while that origin keeps
            // answering a fresh, signed health probe the whole time. Room
            // presence alone then reported a healthy, actively-serving fleet as
            // offline and the "Online only" filter (checked by default) hid
            // every one of those rows from the Mirror nodes list. Trust the
            // endpoint's own signed-fresh health probe as a second, independent
            // "is this node actually up" signal.
            const bool endpointAlive = m.value("endpointHealthy").toBool() &&
                                       m.value("endpointFresh").toBool();
            const bool sshAlive = m.value(QStringLiteral("sshHealthy")).toBool();
            const forkmesh::mirrorsync::MirrorLiveness liveness =
                forkmesh::mirrorsync::catalogMirrorLiveness(
                    m, m_relayOnlineNodes.contains(nodeName.toLower()));
            const bool relayServing =
                liveness == forkmesh::mirrorsync::MirrorLiveness::RelayServing;
            const bool online =
                liveness != forkmesh::mirrorsync::MirrorLiveness::Offline;
            const bool integrityFailing =
                m.value("integrity").toString() == QLatin1String("rejected");
            if (onlineOnly && !online)
                continue;
            const QString catCommit = m.value("commit").toString();
			// An observation taken against older source refs cannot show this
			// mirror behind the current source; it is re-requested instead.
			const bool observationCurrent =
				forkmesh::mirrorsync::observationMatchesSourceRefs(
					m, referenceAllRefsSha256);
			if (!observationCurrent && (endpointAlive || sshAlive))
				staleMirrorObservations = true;
			const QString catRefsSha256 =
				m.value(QStringLiteral("refsSha256"))
					.toString().trimmed().toLower();
			const bool directlyVerified =
				endpointAlive && catRefsSha256.size() == 64;
			const bool refsBehind =
				observationCurrent && directlyVerified &&
				referenceRefsSha256.size() == 64 &&
				catRefsSha256 != referenceRefsSha256;
			const QString catAllRefsSha256 =
				m.value(QStringLiteral("allRefsSha256"))
					.toString().trimmed().toLower();
			const bool allRefsBehind =
				observationCurrent && catAllRefsSha256.size() == 64 &&
				referenceAllRefsSha256.size() == 64 &&
				catAllRefsSha256 != referenceAllRefsSha256;
			const QHash<QString, QString> catHeads = branchHeadsFromJson(
				m.value(QStringLiteral("branchHeads")).toObject());
			const bool headsBehind =
				observationCurrent && !catHeads.isEmpty() &&
				!referenceHeads.isEmpty() && catHeads != referenceHeads;
			const bool commitBehind =
				observationCurrent && online && !catCommit.isEmpty() &&
				!referenceCommit.isEmpty() && catCommit != referenceCommit;
			const bool behind =
				refsBehind || allRefsBehind || headsBehind || commitBehind;
			if (behind)
				mirrorObservedBehind = true;
			if (directlyVerified) {
				++directVerifiedPeers;
				if (behind)
					++outOfSyncPeers;
			}
            if (online || integrityFailing)
                catalogOnlyDots.append({m.value("id").toString(), nodeName,
                                        online, false, behind, integrityFailing});
            const int row = m_mirrorNodesTable->rowCount();
            m_mirrorNodesTable->insertRow(row);
            QString ownerUser = m.value(QStringLiteral("ownerUser"))
                                    .toString()
                                    .trimmed();
            if (ownerUser.isEmpty() &&
                m.value(QStringLiteral("metadataSource")).toString() ==
                    QLatin1String("ssh"))
                ownerUser = topBarUserName().trimmed();
            if (ownerUser.isEmpty() &&
                m_profileLinkedNodes.contains(nodeName, Qt::CaseInsensitive))
                ownerUser = topBarUserName().trimmed();
            auto *nameItem = new SortTableWidgetItem(
                nodeName + (isSource
                                ? QString::fromUtf8("  \xE2\x98\x85 source of truth")
                                : QString()));
            const int light = integrityFailing ? 2 : behind ? 1 : 0;
            const QColor lightColor = integrityFailing ? QColor("#f85149")
                                      : !online          ? Theme::remapColor(QColor("#8b949e"))
                                      : behind           ? QColor("#d29922")
                                                         : QColor("#3fb950");
            nameItem->setIcon(
                QIcon(nodeStatusLightPixmap(lightColor, 14, 0.0, light != 0)));
            nameItem->setData(kNodeLightRole, light);
            nameItem->setData(kMirrorNodeKeysRole,
                              QStringList{nodeName.trimmed().toLower()});
            nameItem->setData(kMirrorNodeIdRole, m.value("id").toString());
            nameItem->setData(kMirrorNodeOnlineRole, online);
            nameItem->setData(kTableSortRole,
                              (isSource ? QStringLiteral("0") : QStringLiteral("1")) +
                                  nodeName.toLower());
            nameItem->setToolTip(
                roomOnline
                    ? (behind ? QString::fromUtf8("Online \xC2\xB7 out of sync")
                              : QStringLiteral("Online now"))
                    : endpointAlive
                          ? QString::fromUtf8(
                                "Not in the live relay room, but its direct-HTTPS "
                                "origin just answered a fresh signed health probe "
                                "\xE2\x80\x94 the mirror is up and serving.")
                          : sshAlive
                                ? QStringLiteral(
                                      "Online through the saved Host's authenticated "
                                      "SSH connection; repository metadata was read "
                                      "directly from that machine.")
                          : relayServing
                                ? QStringLiteral(
                                      "The relay reports this node live, but its "
                                      "signed direct-HTTPS lease is not current: its "
                                      "refs are not the fleet's accepted state, so "
                                      "clones are not routed to it until it syncs.")
                          : QString::fromUtf8(
                                "Published mirror \xC2\xB7 not in the live room"));
            if (integrityFailing)
                markPinRejected(nameItem, false);
            m_mirrorNodesTable->setItem(row, MirrorNodeColNode, nameItem);
            {
                forkmesh::noderole::Facts facts;
                facts.name = nodeName;
                facts.savedHost =
                    activeSavedNodes.contains(nodeName.toLower()) ||
                    m.value(QStringLiteral("metadataSource")).toString() ==
                        QLatin1String("ssh");
                facts.declaredRole = m.value(QStringLiteral("role")).toString();
                facts.runtimeMode =
                    m.value(QStringLiteral("runtimeMode")).toString();
                m_mirrorNodesTable->setItem(
                    row, MirrorNodeColRole,
                    makeNodeRoleCell(nodeRoleFor(facts)));
            }
            m_mirrorNodesTable->setItem(row, MirrorNodeColOwner,
                                        makeOwnerCell(ownerUser));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColRepos,
                makeReposCell(reposByNode.value(nodeName.toLower(), -1)));
            QString catCommitText = QString::fromUtf8("\xE2\x80\x94");
            if (!catCommit.isEmpty()) {
                catCommitText = catCommit.left(10);
                const QString catBranch = m.value("branch").toString();
                if (!catBranch.isEmpty())
                    catCommitText += "  (" + catBranch + ")";
            }
            // The subject/author the publishing node signed into its catalog
            // record, so an offline node still names its latest commit.
            CommitIdentity catIdentity;
            catIdentity.subject =
                m.value(QStringLiteral("lastCommitMessage")).toString().trimmed();
            catIdentity.author = m.value(QStringLiteral("lastCommitAuthorName"))
                                     .toString()
                                     .trimmed();
            catIdentity.committedAtMs =
                qMax(qint64(0), qint64(m.value(QStringLiteral("lastCommitAt"))
                                           .toDouble()));
            catIdentity = resolveCommitIdentity(catCommit, catIdentity);
            if (!catCommit.isEmpty() && catIdentity.committedAtMs > 0)
                catCommitText += QString::fromUtf8(" \xC2\xB7 ") +
                                 formatShortRelativeTime(
                                     catIdentity.committedAtMs / 1000) +
                                 QStringLiteral(" ago");
            setCommitIdentityCells(row, catIdentity);
            auto *catCommitItem = new QTableWidgetItem(catCommitText);
            if (!catCommit.isEmpty())
                catCommitItem->setToolTip(
                    catIdentity.subject.isEmpty()
                        ? catCommit
                        : catIdentity.subject + "\n" + catCommit);
            markMismatch(catCommitItem,
                         !catCommit.isEmpty() && !referenceCommit.isEmpty() &&
                             catCommit != referenceCommit,
                         referenceCommit.left(10));
            m_mirrorNodesTable->setItem(row, MirrorNodeColCommit, catCommitItem);
            const qint64 syncedSecs = qint64(m.value("lastSync").toDouble()) / 1000;
            auto *syncedItem = new SortTableWidgetItem(
                syncedSecs > 0 ? formatShortRelativeTime(syncedSecs) + " ago"
                               : QString::fromUtf8("\xE2\x80\x94"));
            syncedItem->setData(kTableSortRole, double(syncedSecs));
            if (syncedSecs > 0)
                syncedItem->setToolTip(
                    QDateTime::fromSecsSinceEpoch(syncedSecs).toString(Qt::ISODate));
            m_mirrorNodesTable->setItem(row, MirrorNodeColSynced, syncedItem);
            const qint64 publishedNodeBytes =
                qint64(m.value("sizeBytes").toDouble());
            const bool usesCanonicalSize =
                refSizeBytes > 0 && !referenceCommit.isEmpty() &&
                catCommit == referenceCommit;
            const qint64 nodeBytes =
                usesCanonicalSize ? refSizeBytes : publishedNodeBytes;
            auto *sizeItem = new SortTableWidgetItem(
                nodeBytes > 0 ? formatByteSize(nodeBytes)
                              : QString::fromUtf8("\xE2\x80\x94"));
            sizeItem->setData(kTableSortRole, double(nodeBytes));
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (usesCanonicalSize)
                sizeItem->setToolTip(QStringLiteral(
                    "Canonical repository content size from the source of truth. "
                    "Physical Git pack size can differ without changing content."));
            m_mirrorNodesTable->setItem(row, MirrorNodeColSize, sizeItem);
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColGitDatabase,
                makeGitDatabaseCell(publishedNodeBytes));
            if (nodeBytes > 0) {
                totalBytes += nodeBytes;
                maxRepoBytes = qMax(maxRepoBytes, nodeBytes);
            }
            const int catIssues = m.value("issueCount").toInt(-1);
            auto *catIssuesItem = new SortTableWidgetItem(
                catIssues >= 0 ? QString::number(catIssues)
                               : QString::fromUtf8("\xE2\x80\x94"));
            catIssuesItem->setData(kTableSortRole, double(catIssues));
            catIssuesItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (catIssues >= 0)
                catIssuesItem->setToolTip(QString::fromUtf8("Mirroring %1 issue%2")
                                              .arg(catIssues)
                                              .arg(catIssues == 1 ? "" : "s"));
            markMismatch(catIssuesItem, countMismatch(catIssues, refIssues),
                         QString::number(refIssues));
            m_mirrorNodesTable->setItem(row, MirrorNodeColIssues, catIssuesItem);
            const int catCommits = m.value("commitCount").toInt(-1);
            const int catBranches = !catHeads.isEmpty()
                                        ? catHeads.size()
                                        : m.value("branchCount").toInt(-1);
            const int catPulls = m.value("pullCount").toInt(-1);
            const int catDiscussions = m.value("discussionCount").toInt(-1);
            auto *catCommitsItem = makeCountCell(catCommits, "commit", "commits");
            markMismatch(catCommitsItem, countMismatch(catCommits, refCommits),
                         QString::number(refCommits));
            m_mirrorNodesTable->setItem(row, MirrorNodeColCommits, catCommitsItem);
            auto *catBranchesItem =
                makeCountCell(catBranches, "branch", "branches");
            markMismatch(catBranchesItem, countMismatch(catBranches, refBranches),
                         QString::number(refBranches));
            m_mirrorNodesTable->setItem(row, MirrorNodeColBranches,
                                        catBranchesItem);
            auto *catPullsItem =
                makeCountCell(catPulls, "pull request", "pull requests");
            markMismatch(catPullsItem, countMismatch(catPulls, refPulls),
                         QString::number(refPulls));
            m_mirrorNodesTable->setItem(row, MirrorNodeColPulls, catPullsItem);
            auto *catDiscussionsItem =
                makeCountCell(catDiscussions, "discussion", "discussions");
            markMismatch(catDiscussionsItem,
                         countMismatch(catDiscussions, refDiscussions),
                         QString::number(refDiscussions));
            m_mirrorNodesTable->setItem(row, MirrorNodeColDiscussions,
                                        catDiscussionsItem);
            MirrorSyncBacklog backlog;
            if (!isSource) {
                backlog = syncBacklog(
                    catHeads,
                    catCommit, catIssues, catCommits, catBranches, catPulls,
                    catDiscussions);
            }
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColIssuesToSource,
                makeSyncBackCell(backlog.issues, QStringLiteral("issue")));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColCommitsToSource,
                makeSyncBackCell(backlog.commits, QStringLiteral("commit")));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColBranchesToSource,
                makeSyncBackCell(backlog.branches, QStringLiteral("branch")));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColPullsToSource,
                makeSyncBackCell(backlog.pulls,
                                 QStringLiteral("pull request")));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColDiscussionsToSource,
                makeSyncBackCell(backlog.discussions,
                                 QStringLiteral("discussion")));
            const QString catPlatform = m.value("platform").toString();
            const QString catVersion = m.value("version").toString();
            const QString catId = m.value("id").toString();
            auto *healthItem =
                directlyVerified
                    ? new SortTableWidgetItem(QStringLiteral("Verified"))
                    : sshAlive
                          ? new SortTableWidgetItem(
                                QStringLiteral("SSH verified"))
                          : makeNodeHealthCell({}, 0, 0);
            if (directlyVerified) {
                healthItem->setData(kTableSortRole, 4.0);
                healthItem->setForeground(QColor("#3fb950"));
                healthItem->setToolTip(
                    QStringLiteral(
                        "Fresh Ed25519-signed direct repository health proof.\n"
                        "Refs SHA-256: %1\n"
                        "This proves availability and repository integrity; host "
                        "CPU/RAM/disk diagnostics are not part of this proof.")
                        .arg(catRefsSha256));
            } else if (sshAlive) {
                healthItem->setData(kTableSortRole, 3.0);
                healthItem->setForeground(QColor("#3fb950"));
                healthItem->setToolTip(QStringLiteral(
                    "The saved Host answered over its pinned authenticated SSH "
                    "connection. Repository and machine facts in this row were "
                    "read directly from that host; direct-HTTPS integrity proof "
                    "was unavailable."));
            }
            healthItem->setData(
                kMirrorDiagnosticPromptRole,
                mirrorDiagnosticPrompt(source, nodeName, catId, ownerUser,
                                       catPlatform, catVersion, {}, 0));
            healthItem->setToolTip(
                healthItem->toolTip() +
                QStringLiteral("\n\nDouble-click or press Enter to draft a "
                               "troubleshooting prompt."));
            m_mirrorNodesTable->setItem(row, MirrorNodeColHealth, healthItem);

            m_mirrorNodesTable->setItem(
                row, MirrorNodeColCpu,
                makeCpuUsageCell(m.value(QStringLiteral("cpuPercent"))
                                     .toDouble(-1.0)));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColRam,
                makeByteUsageCell(
                    QStringLiteral("RAM"),
                    qint64(m.value(QStringLiteral("memUsedBytes")).toDouble()),
                    qint64(m.value(QStringLiteral("memTotalBytes")).toDouble())));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColDisk,
                makeByteUsageCell(
                    QStringLiteral("Disk"),
                    qint64(m.value(QStringLiteral("diskUsedBytes")).toDouble()),
                    qint64(m.value(QStringLiteral("diskTotalBytes")).toDouble())));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColPlatform,
                new QTableWidgetItem(catPlatform.isEmpty()
                                         ? QString::fromUtf8("\xE2\x80\x94")
                                         : catPlatform));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColVersion,
                new QTableWidgetItem(catVersion.isEmpty()
                                         ? QString::fromUtf8("\xE2\x80\x94")
                                         : catVersion));
            auto *catIdItem = new QTableWidgetItem(
                catId.isEmpty()
                    ? QString::fromUtf8("\xE2\x80\x94")
                    : catId.left(12) + (catId.size() > 12
                                            ? QString::fromUtf8("\xE2\x80\xA6")
                                            : QString()));
            if (!catId.isEmpty())
                catIdItem->setToolTip(catId);
            m_mirrorNodesTable->setItem(row, MirrorNodeColId, catIdItem);
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColRoute,
                makeRouteCell(routeByNode.value(
                    nodeName.trimmed().toLower())));
            const int catClones = m.value("clonesServed").toInt(-1);
            const int catWebsite = m.value("websiteServed").toInt(-1);
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColClones,
                makeServeCountCell(catClones, clonesTip(catClones)));
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColWebsite,
                makeServeCountCell(catWebsite, websiteTip(catWebsite)));
            const int catArtifacts = m.value("artifactCount").toInt(-1);
            auto *catArtifactsItem =
                makeCountCell(catArtifacts, "artifact", "artifacts");
            markMismatch(catArtifactsItem,
                         countMismatch(catArtifacts, refArtifacts),
                         QString::number(refArtifacts));
            m_mirrorNodesTable->setItem(row, MirrorNodeColArtifacts,
                                        catArtifactsItem);
            const QString reachabilityNode = nodeName.trimmed().toLower();
            m_mirrorNodesTable->setItem(
                row, MirrorNodeColReachability,
                makeReachabilityCell(reachabilityNode, false));
            ++count;
        }
    }
    activityDots.append(catalogOnlyDots);
    requestMirrorPanelGitFacts(gitRequest);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 observationRefreshMs = forkmesh::mirrorsync::refreshIntervalMs(
        staleMirrorObservations, mirrorObservedBehind);
    // Roster frames rebuild this panel constantly, on any page. Only a panel
    // that is on screen may turn a rebuild into relay and mirror reads (one
    // /mirrors plus /health and /branches per mirror); a hidden one keeps its
    // cached rows and reads when its tab is opened.
    if (mirrorNodesOnScreen() &&
        (m_catalogMirrorsFetchSource != source ||
         nowMs - m_catalogMirrorsFetchedMs > observationRefreshMs)) {
        // Ahead of the settled cadence, re-read the SSH snapshots in the same
        // pass so both observation sources describe the current source refs.
        // The cached values stay on screen until the new answers land.
        if (m_catalogMirrorsFetchSource == source &&
            nowMs - m_catalogMirrorsFetchedMs <=
                forkmesh::mirrorsync::kSettledRefreshMs) {
            const QString sshPrefix = source + QLatin1Char('|');
            for (auto it = m_sshMirrorMetadataCache.begin();
                 it != m_sshMirrorMetadataCache.end(); ++it) {
                if (it.key().startsWith(sshPrefix))
                    it.value().insert(QStringLiteral("sshFetchedAt"), 0.0);
            }
        }
        m_catalogMirrorsFetchSource = source;
        m_catalogMirrorsFetchedMs = nowMs;
        fetchCatalogMirrors(sourceOwner,
                            repoSegment(repo.name, QStringLiteral("repository")),
                            source);
    }
    fetchSshMirrorMetadata(sourceOwner,
                           repoSegment(repo.name, QStringLiteral("repository")),
                           source);
    if (mirrorNodesOnScreen())
        fetchMirrorSectionCatalog(
            sourceOwner, repoSegment(repo.name, QStringLiteral("repository")));

    m_mirrorNodesTable->setSortingEnabled(true);

    QHash<QString, NodeDotRepoState> repoStates;
    repoStates.reserve(activityDots.size());
    for (const MirrorNodeDot &d : activityDots) {
        const QString key = d.name.trimmed().toLower();
        if (!key.isEmpty())
            repoStates.insert(key, NodeDotRepoState{d.online, d.behind,
                                                    d.integrityFailing});
    }
    setNodeDotRepoStates(repoStates);

    if (m_mirrorNodesSummary) {
        QString text = QString::fromUtf8("\xC2\xB7 %1 node%2 mirroring %3")
                           .arg(count)
                           .arg(count == 1 ? "" : "s")
                           .arg(source);
        if (maxRepoBytes > 0)
            text += QString::fromUtf8(" \xC2\xB7 %1 each \xC2\xB7 %2 total")
                        .arg(formatByteSize(maxRepoBytes),
                             formatByteSize(totalBytes));
		if (directVerifiedPeers > 0)
			text += QString::fromUtf8(" \xC2\xB7 %1 direct verified")
						.arg(directVerifiedPeers);
		if (outOfSyncPeers > 0)
            text += QString::fromUtf8(
                        " \xC2\xB7 <span style='color:#f85149'>\xE2\x9A\xA0 %1 "
                        "mirror node%2 out of sync</span>")
                        .arg(outOfSyncPeers)
                        .arg(outOfSyncPeers == 1 ? "" : "s");
        if (pinRejectedNodes > 0)
            text += QString::fromUtf8(
                        " \xC2\xB7 <span style='color:#f85149'>\xE2\x9A\xA0 %1 "
                        "node%2 failing the integrity pin</span>")
                        .arg(pinRejectedNodes)
                        .arg(pinRejectedNodes == 1 ? "" : "s");
        if (pendingPush > 0)
            text += QString::fromUtf8(
                        " \xC2\xB7 <span style='color:#d29922'>\xE2\x86\x91 %1 "
                        "to push</span>")
                        .arg(pendingPush);
        // The fleet's standing in the shared forkmesh-sync-status-v1 model,
        // with the plain-language explanation on hover.
        if (m_catalogMirrorsSource == source && !m_catalogMirrorsCache.isEmpty()) {
            const QJsonObject sync = forkmesh::syncstatus::mirrorRowsStatus(
                m_catalogMirrorsCache, nowMs);
            const QString severity =
                sync.value(QStringLiteral("severity")).toString();
            const QString label =
                sync.value(QStringLiteral("label")).toString().toHtmlEscaped();
            const QString color = severity == QLatin1String("error") ? QStringLiteral("#f85149")
                                  : severity == QLatin1String("warning") ? QStringLiteral("#d29922")
                                                                         : QString();
            text += QString::fromUtf8(" \xC2\xB7 ") +
                    (color.isEmpty() ? label
                                     : QStringLiteral("<span style='color:%1'>%2</span>")
                                           .arg(color, label));
            m_mirrorNodesSummary->setToolTip(
                sync.value(QStringLiteral("summary")).toString());
        } else {
            m_mirrorNodesSummary->setToolTip(QString());
        }
        if (count == 0)
            text += QString::fromUtf8(" \xC2\xB7 ") +
                    (onlineOnly ? QStringLiteral("no online node is advertising "
                                                 "a copy right now")
                                : QStringLiteral("no other node is advertising "
                                                 "a copy yet"));
        m_mirrorNodesSummary->setTextFormat(Qt::RichText);
        m_mirrorNodesSummary->setText(text);
    }
    if (m_mirrorResetPinButton)
        m_mirrorResetPinButton->setVisible(weAreSource && repoWorkingTree);

    if (count == 0) {
        m_mirrorNodesTable->insertRow(0);
        auto *empty = new QTableWidgetItem(
            onlineOnly
                ? "No online nodes are advertising a mirror of this repository right now."
                : "No other nodes are advertising a mirror of this repository yet.");
        empty->setForeground(Theme::remapColor(QColor("#8b949e")));
        m_mirrorNodesTable->setItem(0, MirrorNodeColNode, empty);
    } else {
        // Widen the columns the roster has outgrown. The rebuild that froze
        // this grid saw only the local row, with "Source", "Verified" and em
        // dashes in it; the fleet's own "Out of date", "1 critical - 2
        // warnings", "up - 87 ms" and "Repository - 142 ms" land in later
        // rebuilds and are clipped to nonsense without this. Skipped for the
        // empty state, whose one row is a full sentence in the Node column.
        growColumnsToContents(m_mirrorNodesTable);
    }
    applyNodeRoleFilter();
    updateMirrorNodeLightTimer();
}

void MainWindow::animateMirrorNodeLights()
{
    if (!m_mirrorNodesTable)
        return;
    // Two icon renders + a setIcon per flagged row, eight times a second,
    // are wasted while the Mirror nodes tab is off-screen. Idle the timer
    // instead (it keeps polling slowly, so the beacons resume on their own).
    if (!mirrorNodesOnScreen()) {
        if (m_nodeLightTimer && m_nodeLightTimer->interval() != kAgentSpinHiddenTickMs)
            m_nodeLightTimer->setInterval(kAgentSpinHiddenTickMs);
        return;
    }
    if (m_nodeLightTimer && m_nodeLightTimer->interval() != 120)
        m_nodeLightTimer->setInterval(120);
    m_nodeLightFrame = (m_nodeLightFrame + 1) % 10;
    const qreal angle = m_nodeLightFrame * 36.0;
    const QIcon caution(nodeStatusLightPixmap(QColor("#d29922"), 14, angle, true));
    const QIcon error(nodeStatusLightPixmap(QColor("#f85149"), 14, angle, true));
    for (QTableWidget *table : {m_mirrorNodesTable, m_nodesTable}) {
        if (!table)
            continue;
        QSignalBlocker block(table);
        for (int r = 0; r < table->rowCount(); ++r) {
            QTableWidgetItem *item = table->item(r, MirrorNodeColNode);
            if (!item)
                continue;
            const int light = item->data(kNodeLightRole).toInt();
            if (light == 1)
                item->setIcon(caution);
            else if (light == 2)
                item->setIcon(error);
        }
        table->viewport()->update();
    }
}

void MainWindow::updateMirrorNodeLightTimer()
{
    bool spinning = false;
    if (m_mirrorNodesTable) {
        for (int r = 0; r < m_mirrorNodesTable->rowCount() && !spinning; ++r) {
            QTableWidgetItem *item = m_mirrorNodesTable->item(r, MirrorNodeColNode);
            spinning = item && item->data(kNodeLightRole).toInt() != 0;
        }
    }
    if (spinning) {
        if (!m_nodeLightTimer) {
            m_nodeLightTimer = new QTimer(this);
            connect(m_nodeLightTimer, &QTimer::timeout, this,
                    &MainWindow::animateMirrorNodeLights);
        }
        if (!m_nodeLightTimer->isActive())
            m_nodeLightTimer->start(120);
    } else if (m_nodeLightTimer) {
        m_nodeLightTimer->stop();
    }
}

void MainWindow::fetchSshMirrorMetadata(const QString &owner,
                                        const QString &repo,
                                        const QString &source)
{
    if (owner.isEmpty() || repo.isEmpty() || source.isEmpty())
        return;

    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kSshMetadataCacheMs = 5 * 60 * 1000;
    QList<QJsonObject> pendingHosts;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (!savedHostMayBeActive(host))
            continue;
        const QString node =
            host.value(QStringLiteral("name")).toString().trimmed().toLower();
        const QString savedName =
            host.value(QStringLiteral("name")).toString().trimmed();
        const QString ip =
            host.value(QStringLiteral("ip")).toString().trimmed();
        const QString user =
            host.value(QStringLiteral("user")).toString().trimmed();
        if (node.isEmpty() || ip.isEmpty() || user.isEmpty())
            continue;
        const QString cacheKey = source + QLatin1Char('|') + node;
        const QJsonObject cached = m_sshMirrorMetadataCache.value(cacheKey);
        const qint64 fetchedAt = qint64(
            cached.value(QStringLiteral("sshFetchedAt")).toDouble());
        if ((fetchedAt > 0 && nowMs - fetchedAt < kSshMetadataCacheMs) ||
            m_sshMirrorMetadataInFlight.contains(cacheKey)) {
            continue;
        }
        QJsonObject checking = cached;
        checking.insert(QStringLiteral("node"), node);
        checking.insert(QStringLiteral("status"), QStringLiteral("offline"));
        checking.insert(QStringLiteral("metadataSource"), QStringLiteral("ssh"));
        checking.insert(QStringLiteral("sshHealthy"), false);
        // This is also the failed-attempt stamp: an unreachable saved host is
        // retried on the normal five-minute cadence instead of immediately
        // recursing from the completion-triggered table rebuild.
        checking.insert(QStringLiteral("sshFetchedAt"), double(nowMs));
        m_sshMirrorMetadataCache.insert(cacheKey, checking);
        QJsonObject request = host;
        request.insert(QStringLiteral("_cacheKey"), cacheKey);
        request.insert(QStringLiteral("_node"), node);
        request.insert(QStringLiteral("_savedName"), savedName);
        pendingHosts.append(request);
    }
    if (pendingHosts.isEmpty())
        return;

    struct SshMetadataBatch {
        int remaining = 0;
        QString source;
    };
    const auto batch = std::make_shared<SshMetadataBatch>();
    batch->remaining = pendingHosts.size();
    batch->source = source;
    const QString observedSourceRefs =
        mirrorObservationSourceRefs().value(source);
    const QByteArray encodedProgram = sshMirrorMetadataProgram().toBase64();
    const QString encodedOwner = QString::fromLatin1(owner.toUtf8().toBase64());
    const QString encodedRepo = QString::fromLatin1(repo.toUtf8().toBase64());
    const QString remoteCommand = QStringLiteral(
        "python3 -c 'import base64;exec(base64.b64decode(\"%1\"))' %2 %3")
                                      .arg(QString::fromLatin1(encodedProgram),
                                           encodedOwner, encodedRepo);

    for (const QJsonObject &host : std::as_const(pendingHosts)) {
        const QString cacheKey = host.value(QStringLiteral("_cacheKey")).toString();
        const QString node = host.value(QStringLiteral("_node")).toString();
        const QString savedName =
            host.value(QStringLiteral("_savedName")).toString();
        const QString ip = host.value(QStringLiteral("ip")).toString().trimmed();
        const QString user = host.value(QStringLiteral("user")).toString().trimmed();
        const QString credentialKey =
            forkmesh::control::savedHostCredentialKey(savedName, ip, user);
        const QString password = m_hostSessionPasswords.value(credentialKey);
        const QString identityFile = savedHostIdentityFile(savedName, ip, user);
        QString error;
        const forkmesh::control::HostSshCommand ssh =
            forkmesh::control::buildHostSshCommand(
                ip, user, password, remoteCommand, &error, identityFile);
        if (ssh.program.isEmpty()) {
            if (--batch->remaining == 0)
                QTimer::singleShot(0, this, [this] { loadMirrorNodesPanel(); });
            continue;
        }

        m_sshMirrorMetadataInFlight.insert(cacheKey);
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        auto *watcher = new QFutureWatcher<WorkerProcessResult>(this);
        connect(watcher, &QObject::destroyed, [cancelled](QObject *) {
            cancelled->store(true, std::memory_order_relaxed);
        });
        connect(watcher, &QFutureWatcher<WorkerProcessResult>::finished, this,
                [this, watcher, cacheKey, node, owner, repo, batch,
                 observedSourceRefs] {
                    const WorkerProcessResult result = watcher->result();
                    watcher->deleteLater();
                    m_sshMirrorMetadataInFlight.remove(cacheKey);
                    if (result.ok) {
                        QJsonParseError parseError;
                        QJsonObject metadata =
                            QJsonDocument::fromJson(result.output, &parseError)
                                .object();
                        if (parseError.error == QJsonParseError::NoError &&
                            metadata.value(QStringLiteral("node"))
                                    .toString().trimmed()
                                    .compare(node, Qt::CaseInsensitive) == 0 &&
                            metadata.value(QStringLiteral("owner"))
                                    .toString().compare(owner,
                                                        Qt::CaseInsensitive) == 0 &&
                            metadata.value(QStringLiteral("repository"))
                                    .toString().compare(repo,
                                                        Qt::CaseInsensitive) == 0) {
                            // Use the desktop's clock for cache expiry. The
                            // remote clock can be skewed far enough into the
                            // future to otherwise suppress later refreshes.
                            metadata.insert(
                                QStringLiteral("sshFetchedAt"),
                                double(QDateTime::currentMSecsSinceEpoch()));
                            if (!observedSourceRefs.isEmpty())
                                metadata.insert(
                                    forkmesh::mirrorsync::observedSourceRefsKey(),
                                    observedSourceRefs);
                            m_sshMirrorMetadataCache.insert(cacheKey, metadata);
                        }
                    }
                    if (--batch->remaining > 0)
                        return;
                    const int currentIndex = mirrorNodesRepoIndex();
                    if (currentIndex < 0)
                        return;
                    const RepositoryRecord &current =
                        m_repositories.at(currentIndex);
                    const QString currentSource =
                        repoSegment(current.owner, QStringLiteral("owner")) +
                        QLatin1Char('/') +
                        repoSegment(current.name,
                                    QStringLiteral("repository"));
                    if (currentSource == batch->source)
                        loadMirrorNodesPanel();
                });

        WorkerProcessRequest request;
        request.program = ssh.program;
        request.arguments = ssh.arguments;
        request.environment = ssh.environment;
        request.channelMode = QProcess::SeparateChannels;
        request.timeoutMs = 20000;
        request.maxOutputBytes = 64 * 1024;
        watcher->setFuture(QtConcurrent::run(
            [request = std::move(request), cancelled] {
                return runWorkerProcess(request, cancelled);
            }));
    }
}

void MainWindow::fetchCatalogMirrors(const QString &owner, const QString &repo,
                                     const QString &source)
{
    if (!m_networkAccess || owner.isEmpty() || repo.isEmpty())
        return;
    const QString observedSourceRefs =
        mirrorObservationSourceRefs().value(source);

    struct DirectEndpoint {
        QString node;
        QUrl origin;
    };
    QList<DirectEndpoint> endpoints;
    QSet<QString> endpointKeys;
    auto appendEndpoint = [&endpoints, &endpointKeys](const QString &nodeValue,
                                                       const QUrl &originValue) {
        const QString node = nodeValue.trimmed().toLower();
        const QUrl origin = normalizedDirectMirrorOrigin(originValue.toString());
        const QString key = node + QLatin1Char('|') + origin.toString();
        if (node.isEmpty() || origin.isEmpty() || endpointKeys.contains(key))
            return;
        endpointKeys.insert(key);
        endpoints.append({node, origin});
    };

    QJsonArray known = m_catalogMirrorsSource == source
                           ? m_catalogMirrorsCache
                           : loadDirectMirrorCache(source);

    // Hosts is the desktop's durable fleet inventory. Probe those managed
    // origins directly; no catalog/Worker request is needed to establish that
    // a node is alive and actually contains this repository.
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, nullptr);
    QSet<QString> savedNodes;
    const QString catalogHost = catalogApiUrl().host().trimmed().toLower();
    const QStringList catalogLabels = catalogHost.split(
        QLatin1Char('.'), Qt::SkipEmptyParts);
    const QString defaultZone = catalogLabels.size() >= 3
                                    ? catalogLabels.mid(1).join(QLatin1Char('.'))
                                    : catalogHost;
    const qint64 probeNowMs = QDateTime::currentMSecsSinceEpoch();
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        // A stale "install failed"/"installing" label is probed as well, so a
        // healthy answer can correct it; until then the panel still hides the
        // row (activeSavedNodes in loadMirrorNodesPanel).
        if (!savedHostMayBeActive(host) &&
            !forkmesh::control::savedHostInstallStatusMayBeStale(host,
                                                                 probeNowMs))
            continue;
        const QString node =
            host.value(QStringLiteral("name")).toString().trimmed().toLower();
        if (node.isEmpty())
            continue;
        QUrl origin = normalizedDirectMirrorOrigin(
            host.value(QStringLiteral("publicOrigin")).toString());
        if (origin.isEmpty()) {
            const QString dns =
                host.value(QStringLiteral("dnsHostname")).toString().trimmed();
            if (!dns.isEmpty())
                origin = normalizedDirectMirrorOrigin(
                    QStringLiteral("https://") + dns);
        }
        if (origin.isEmpty() && !defaultZone.isEmpty()) {
            origin = normalizedDirectMirrorOrigin(
                QStringLiteral("https://%1.%2").arg(node, defaultZone));
        }
        if (origin.isEmpty())
            continue;
        savedNodes.insert(node);
        appendEndpoint(node, origin);
    }

    // Hosts is authoritative. A catalog row for a removed machine must never
    // resurrect it in Status, and a relay outage must not hide a saved node.
    QJsonArray activeKnown;
    for (const QJsonValue &value : std::as_const(known)) {
        const QString node = value.toObject()
                                 .value(QStringLiteral("node"))
                                 .toString().trimmed().toLower();
        if (savedNodes.contains(node))
            activeKnown.append(value);
    }
    known = activeKnown;

    if (endpoints.isEmpty()) {
        m_catalogMirrorsSource = source;
        m_catalogMirrorsCache = {};
        saveDirectMirrorCache(source, {});
        QTimer::singleShot(0, this, [this] { loadMirrorNodesPanel(); });
        return;
    }
    if (m_directMirrorDiscoveryInFlight.contains(source))
        return;
    m_directMirrorDiscoveryInFlight.insert(source);

    // Do not render yesterday's endpoint health while today's direct probes
    // are in flight. Rows become online only after their own origin answers.
    for (const DirectEndpoint &endpoint : std::as_const(endpoints)) {
        bool found = false;
        for (int i = 0; i < known.size(); ++i) {
            QJsonObject mirror = known.at(i).toObject();
            if (mirror.value(QStringLiteral("node")).toString().compare(
                    endpoint.node, Qt::CaseInsensitive) != 0)
                continue;
            mirror.insert(QStringLiteral("endpoint"), endpoint.origin.toString());
            mirror.insert(QStringLiteral("endpointHealthy"), false);
            mirror.insert(QStringLiteral("endpointFresh"), false);
			mirror.remove(QStringLiteral("commit"));
			mirror.remove(QStringLiteral("branch"));
			mirror.remove(QStringLiteral("branchCount"));
			mirror.remove(QStringLiteral("branchHeads"));
			mirror.remove(QStringLiteral("lastCommitAt"));
			mirror.remove(QStringLiteral("refsSha256"));
			mirror.remove(QStringLiteral("allRefsSha256"));
            known.replace(i, mirror);
            found = true;
            break;
        }
        if (!found) {
            known.append(QJsonObject{
                {QStringLiteral("node"), endpoint.node},
                {QStringLiteral("endpoint"), endpoint.origin.toString()},
                {QStringLiteral("endpointHealthy"), false},
                {QStringLiteral("endpointFresh"), false},
            });
        }
    }
    m_catalogMirrorsSource = source;
    m_catalogMirrorsCache = known;

    struct ProbeBatch {
        int remaining = 0;
        QJsonArray mirrors;
    };
    const auto batch = std::make_shared<ProbeBatch>();
    batch->remaining = endpoints.size();
    batch->mirrors = known;
    for (const DirectEndpoint &endpoint : std::as_const(endpoints)) {
        QUrl health = endpoint.origin;
        health.setPath(QStringLiteral("/health"));
        const qint64 issuedAt = QDateTime::currentMSecsSinceEpoch();
        const QString nonce =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("nonce"), nonce);
        query.addQueryItem(QStringLiteral("issuedAt"), QString::number(issuedAt));
        query.addQueryItem(QStringLiteral("owner"), owner);
        query.addQueryItem(QStringLiteral("repo"), repo);
        health.setQuery(query);
        QNetworkRequest request(health);
        request.setRawHeader(QByteArrayLiteral("Accept"),
                             QByteArrayLiteral("application/json"));
        request.setTransferTimeout(5000);
        QNetworkReply *reply = m_networkAccess->get(request);
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, endpoint, owner, repo, source, issuedAt, nonce,
                 batch, observedSourceRefs] {
                    const int status = reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QJsonObject health =
                        QJsonDocument::fromJson(reply->readAll()).object();
                    reply->deleteLater();
                    QJsonObject proof;
                    const bool available = verifiedRepositoryHealth(
                        health, endpoint.node, owner, repo, nonce, issuedAt,
                        &proof);
                    const qint64 checkedAt =
                        qint64(health.value(QStringLiteral("checkedAt")).toDouble(
                            QDateTime::currentMSecsSinceEpoch()));
                    const int latencyMs = int(qBound<qint64>(
                        qint64(0),
                        QDateTime::currentMSecsSinceEpoch() - issuedAt,
                        qint64(60000)));

                    for (int i = 0; i < batch->mirrors.size(); ++i) {
                        QJsonObject mirror = batch->mirrors.at(i).toObject();
                        if (mirror.value(QStringLiteral("node")).toString().compare(
                                endpoint.node, Qt::CaseInsensitive) != 0)
                            continue;
                        mirror.insert(QStringLiteral("endpoint"),
                                      endpoint.origin.toString());
                        mirror.insert(QStringLiteral("endpointHealthy"), available);
                        mirror.insert(QStringLiteral("endpointFresh"), available);
                        mirror.insert(QStringLiteral("endpointIntegrity"),
                                      proof.value(QStringLiteral("integrity")));
                        if (available) {
                            mirror.insert(QStringLiteral("status"),
                                          QStringLiteral("online"));
                            mirror.insert(QStringLiteral("integrity"),
                                          QStringLiteral("ok"));
                            mirror.insert(QStringLiteral("lastSync"), double(checkedAt));
                            mirror.insert(QStringLiteral("latencyMs"), latencyMs);
                            mirror.insert(QStringLiteral("id"),
                                          health.value(QStringLiteral("publicKey")));
                            mirror.insert(
                                QStringLiteral("publicRepositoryCount"),
                                health.value(
                                    QStringLiteral("publicRepositoryCount")));
                            mirror.insert(
                                QStringLiteral("refsSha256"),
                                proof.value(QStringLiteral("refsSha256")));
                            if (!observedSourceRefs.isEmpty())
                                mirror.insert(
                                    forkmesh::mirrorsync::observedSourceRefsKey(),
                                    observedSourceRefs);
                        }
                        batch->mirrors.replace(i, mirror);
                        break;
                    }
                    const QString reachabilityKey =
                        source + QLatin1Char('|') + endpoint.node;
                    m_mirrorReachabilityCache.insert(
                        reachabilityKey,
                        QJsonObject{
                            {QStringLiteral("ok"), true},
                            {QStringLiteral("node"), endpoint.node},
                            {QStringLiteral("reachable"), available},
                            {QStringLiteral("repositoryVerified"), available},
                            {QStringLiteral("directChecked"), true},
                            {QStringLiteral("readmeLoaded"), false},
                            {QStringLiteral("status"), status},
                            {QStringLiteral("latencyMs"), latencyMs},
                            {QStringLiteral("reason"),
                             available
                                 ? QStringLiteral("repository_proof_ok")
                                 : QStringLiteral("direct_health_unavailable")},
                            {QStringLiteral("clientCheckedAt"),
                             double(QDateTime::currentMSecsSinceEpoch())},
                        });
					auto finishEndpoint = [this, source, batch] {
						if (--batch->remaining > 0)
							return;
						m_directMirrorDiscoveryInFlight.remove(source);
						saveDirectMirrorCache(source, batch->mirrors);
						if (m_catalogMirrorsSource == source)
							m_catalogMirrorsCache = batch->mirrors;
						promoteSavedHostsProvenHealthy(
							batch->mirrors,
							QStringLiteral("a signed repository health proof"));
						const int currentIndex = mirrorNodesRepoIndex();
						if (currentIndex < 0)
							return;
						const RepositoryRecord &current =
							m_repositories.at(currentIndex);
						const QString currentSource =
							repoSegment(current.owner, QStringLiteral("owner")) +
							QLatin1Char('/') +
							repoSegment(current.name,
										QStringLiteral("repository"));
						if (currentSource == source)
							loadMirrorNodesPanel();
					};
					if (!available) {
						finishEndpoint();
						return;
					}

					// One cheap, direct for-each-ref operation turns the signed refs
					// proof into the actual primary commit and branch count operators
					// expect in this table. This request goes to the mirror origin,
					// never the app Worker, and shares the five-minute probe cache.
					QUrl branches = endpoint.origin;
					branches.setPath(
					QStringLiteral("/v1/public/repositories/%1/%2/branches")
							.arg(QString::fromUtf8(QUrl::toPercentEncoding(owner)),
								 QString::fromUtf8(QUrl::toPercentEncoding(repo))));
					QNetworkRequest branchRequest(branches);
					branchRequest.setRawHeader(QByteArrayLiteral("Accept"),
										  QByteArrayLiteral("application/json"));
					branchRequest.setTransferTimeout(5000);
					QNetworkReply *branchReply =
						m_networkAccess->get(branchRequest);
					connect(branchReply, &QNetworkReply::finished, this,
							[this, branchReply, endpoint, batch, finishEndpoint] {
								const QJsonObject payload = QJsonDocument::fromJson(
									branchReply->readAll()).object();
								const bool ok =
									branchReply->error() == QNetworkReply::NoError &&
									payload.value(QStringLiteral("ok")).toBool();
								branchReply->deleteLater();
								if (ok) {
									const QJsonArray rows = payload.value(
										QStringLiteral("branches")).toArray();
									QJsonObject primary;
									QJsonObject branchHeads;
									for (const QJsonValue &value : rows) {
										const QJsonObject row = value.toObject();
										const QString name = row.value(
											QStringLiteral("name")).toString();
										const QString commit = row.value(
											QStringLiteral("commit")).toString().trimmed();
										if (!name.isEmpty() &&
											(commit.size() == 40 || commit.size() == 64))
											branchHeads.insert(name, commit.toLower());
										if (primary.isEmpty() ||
											name == QLatin1String("main") ||
											(name == QLatin1String("master") &&
											 primary.value(QStringLiteral("name")) !=
												 QLatin1String("main")))
											primary = row;
									}
									for (int i = 0; i < batch->mirrors.size(); ++i) {
										QJsonObject mirror =
											batch->mirrors.at(i).toObject();
										if (mirror.value(QStringLiteral("node"))
												.toString().compare(endpoint.node,
																	Qt::CaseInsensitive) != 0)
											continue;
										mirror.insert(QStringLiteral("branchCount"),
													  rows.size());
										mirror.insert(QStringLiteral("branchHeads"),
													  branchHeads);
										mirror.insert(QStringLiteral("branch"),
														  primary.value(QStringLiteral("name")));
										mirror.insert(QStringLiteral("commit"),
														  primary.value(QStringLiteral("commit")));
										mirror.insert(
											QStringLiteral("lastCommitMessage"),
											primary.value(QStringLiteral("message")));
										mirror.insert(
											QStringLiteral("lastCommitAuthorName"),
											primary.value(QStringLiteral("author")));
										const QDateTime updated = QDateTime::fromString(
											primary.value(QStringLiteral("updatedAt"))
												.toString(), Qt::ISODate);
										if (updated.isValid())
											mirror.insert(QStringLiteral("lastCommitAt"),
														  double(updated.toMSecsSinceEpoch()));
										batch->mirrors.replace(i, mirror);
										break;
									}
								}
								finishEndpoint();
							});
                });
    }
}

void MainWindow::fetchReleaseDownloadCounts(const QString &owner, const QString &repo,
                                            const QString &source)
{
    if (!m_networkAccess || owner.isEmpty() || repo.isEmpty())
        return;
    QUrl url = catalogApiUrl(); // same host/scheme as the catalog
    url.setPath(QStringLiteral("/api/repo/%1/%2/releases/downloads")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(owner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(repo))));
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, source]() {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        const QJsonObject resp = QJsonDocument::fromJson(body).object();
        if (!resp.value("ok").toBool())
            return;
        m_releaseDownloadsSource = source;
        m_releaseDownloadsCache.clear();
        const QJsonObject counts = resp.value("counts").toObject();
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
            m_releaseDownloadsCache.insert(it.key(), it.value().toInt());
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
            const RepositoryRecord &r = m_repositories.at(m_repoDetailIndex);
            const QString cur =
                repoSegment(r.owner, QStringLiteral("owner")) + "/" +
                repoSegment(r.name, QStringLiteral("repository"));
            if (cur == source)
                loadReleasesPanel();
        }
    });
}

void MainWindow::replicateReleaseArtifacts(int index)
{
    if (!m_networkAccess || index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(index);
    if (repo.previewOnly)
        return;
    const QString mirrorPath = repo.mirrorPath;
    if (mirrorPath.trimmed().isEmpty() || !QDir(mirrorPath).exists())
        return;

    const QString fallbackRepo =
        repoSegment(repo.owner, QStringLiteral("owner")) + "/" +
        repoSegment(repo.name, QStringLiteral("repository"));
    // One `git show` per release channel, after the listing: a dozen blocking
    // waits back to back on the GUI thread every time a repository opened or a
    // sync landed. The manifests are read on a worker; only the backoff gate
    // and the downloads, which are GUI state, come back here.
    static QSet<QString> inFlight; // GUI thread only
    if (inFlight.contains(mirrorPath))
        return;
    inFlight.insert(mirrorPath);
    using MissingBlobs = QList<QPair<QString, QString>>; // sha256, "owner/name"
    runOffThread<MissingBlobs>(
        [mirrorPath, fallbackRepo]() {
            MissingBlobs missing;
            const QString branch = mirrorHeadBranch(mirrorPath);
            if (branch.isEmpty())
                return missing;
            const GitCaptureResult channels = runGitCaptureDirect(
                mirrorPath, {QStringLiteral("ls-tree"), QStringLiteral("-z"),
                             branch + QStringLiteral(":.forkmesh/releases")});
            if (!channels.ok)
                return missing; // no .forkmesh/releases/ tree — nothing to mirror
            static const QRegularExpression sha256Re(
                QStringLiteral("\\A[0-9a-f]{64}\\z"));
            for (const QByteArray &raw : channels.output.split('\0')) {
                const int tab = raw.indexOf('\t');
                if (tab < 0 || !raw.left(tab).contains(" tree "))
                    continue;
                const QString channel =
                    QString::fromUtf8(raw.mid(tab + 1)).trimmed();
                if (channel.isEmpty())
                    continue;
                const GitCaptureResult manifest = runGitCaptureDirect(
                    mirrorPath,
                    {QStringLiteral("show"),
                     branch + QStringLiteral(":.forkmesh/releases/") + channel +
                         QStringLiteral("/release.json")});
                if (!manifest.ok)
                    continue;
                const QJsonObject obj =
                    QJsonDocument::fromJson(manifest.output).object();
                const QString manifestRepo =
                    obj.value(QStringLiteral("repo")).toString().trimmed();
                const QString downloadRepo =
                    manifestRepo.isEmpty() ? fallbackRepo : manifestRepo;
                const QJsonArray assets =
                    obj.value(QStringLiteral("assets")).toArray();
                for (const QJsonValue &asset : assets) {
                    const QString hash = asset.toObject()
                                             .value(QStringLiteral("blob_sha256"))
                                             .toString()
                                             .trimmed()
                                             .toLower();
                    if (!sha256Re.match(hash).hasMatch())
                        continue;
                    if (QFile::exists(mirrorReleaseBlobPath(mirrorPath, hash)))
                        continue; // already hosting this artifact
                    if (releaseBlobRecentlyMissing(
                            hash, QDateTime::currentMSecsSinceEpoch()))
                        continue; // no node hosts it; asked within the day
                    missing.append({hash, downloadRepo});
                }
            }
            return missing;
        },
        [this, mirrorPath](MissingBlobs missing) {
            inFlight.remove(mirrorPath);
            // The repository list can change while the worker reads; the
            // mirror path is what the downloads are keyed by.
            int index = -1;
            for (int i = 0; i < m_repositories.size() && index < 0; ++i)
                if (m_repositories.at(i).mirrorPath == mirrorPath)
                    index = i;
            if (index < 0 || !m_networkAccess)
                return;
            QMap<QString, QString> pending; // blob sha256 -> "owner/name"
            for (const auto &blob : std::as_const(missing)) {
                if (pending.contains(blob.first))
                    continue;
                if (!m_pollBackoff.ready(QStringLiteral("releaseBlob:") + blob.first,
                                         QDateTime::currentMSecsSinceEpoch()))
                    continue;
                pending.insert(blob.first, blob.second);
            }
            if (pending.isEmpty())
                return;
            const RepositoryRecord &repo = m_repositories.at(index);
            logSystem(QStringLiteral("Mirror: fetching %1 release artifact%2 for "
                                     "%3/%4 so this node can serve them.")
                          .arg(pending.size())
                          .arg(pending.size() == 1 ? "" : "s")
                          .arg(repo.owner, repo.name));
            downloadNextReleaseBlob(index, mirrorPath, pending);
        });
}

void MainWindow::downloadNextReleaseBlob(int index, const QString &mirrorPath,
                                         QMap<QString, QString> pending)
{
    if (pending.isEmpty() || !m_networkAccess)
        return;
    auto it = pending.begin();
    const QString hash = it.key();
    const QString downloadRepo = it.value();
    pending.erase(it);

    const int slash = downloadRepo.indexOf('/');
    if (slash <= 0) {
        downloadNextReleaseBlob(index, mirrorPath, pending);
        return;
    }
    const QString owner = downloadRepo.left(slash);
    const QString name = downloadRepo.mid(slash + 1);

    // Stream the bytes straight to a temp file in the target CAS shard, hashing as
    // we go, so even a large binary never sits fully in memory. On a verified match
    // we atomically rename it into place; otherwise the partial is discarded.
    const QString blobPath = mirrorReleaseBlobPath(mirrorPath, hash);
    QDir().mkpath(QFileInfo(blobPath).absolutePath());
    auto tmp = std::make_shared<QFile>(blobPath + QStringLiteral(".part"));
    if (!tmp->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        downloadNextReleaseBlob(index, mirrorPath, pending);
        return;
    }
    auto hasher = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);

    QUrl url = catalogApiUrl(); // relay host/scheme; content-addressed route is public
    url.setPath(QStringLiteral("/api/repo/%1/%2/releases/blob/sha256/%3")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(owner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(name)), hash));
    QNetworkRequest request(url);
    forkmesh::markExpectedHttpStatus(request, 404); // not hosted: an answer
    QNetworkReply *reply = m_networkAccess->get(request);
    auto errorBody = std::make_shared<QByteArray>();
    auto consume = [reply, tmp, hasher, errorBody](const QByteArray &chunk) {
        if (chunk.isEmpty())
            return;
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status >= 400) {
            const int room = 512 - int(errorBody->size());
            if (room > 0)
                errorBody->append(chunk.left(room));
            return;
        }
        tmp->write(chunk);
        hasher->addData(chunk);
    };
    connect(reply, &QNetworkReply::readyRead, this, [reply, consume]() {
        consume(reply->readAll());
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, tmp, hasher, consume, errorBody, index, mirrorPath,
             hash, blobPath, pending]() {
                consume(reply->readAll());
                const bool ok = reply->error() == QNetworkReply::NoError;
                const bool notHosted =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt() == 404;
                QString netError;
                if (!ok) {
                    netError = forkmesh::networkFailureText(reply);
                    const QString served =
                        forkmesh::networkResponseSnippet(*errorBody);
                    if (!served.isEmpty())
                        netError += QStringLiteral(": ") + served;
                }
                reply->deleteLater();
                tmp->close();
                const QString partPath = tmp->fileName();
                const QString actual =
                    QString::fromLatin1(hasher->result().toHex());
                if (ok && actual == hash) {
                    m_pollBackoff.noteSuccess(QStringLiteral("releaseBlob:") + hash);
                    QFile::remove(releaseBlobMissMarkerPath(hash));
                    QFile::remove(blobPath); // replace any stale/empty leftover
                    if (!QFile::rename(partPath, blobPath)) {
                        QFile::remove(partPath);
                    } else if (index >= 0 && index < m_repositories.size() &&
                               m_repositories.at(index).mirrorPath == mirrorPath) {
                        m_mirrorAdvertSig.clear();
                        refreshRepositoryList();
                    }
                } else {
                    QFile::remove(partPath);
                    // Only the shard directory mkpath made for this download;
                    // rmdir leaves it alone once it holds anything.
                    QDir().rmdir(QFileInfo(blobPath).absolutePath());
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    if (notHosted) {
                        noteReleaseBlobMissing(hash);
                        m_pollBackoff.noteFailure(
                            QStringLiteral("releaseBlob:") + hash, nowMs,
                            kReleaseBlobMissRetryMs, kReleaseBlobMissRetryMs);
                        logSystem(QStringLiteral(
                                      "Mirror: release artifact %1 is not hosted "
                                      "by any node; checking again in a day.")
                                      .arg(hash.left(12)));
                        downloadNextReleaseBlob(index, mirrorPath, pending);
                        return;
                    }
                    m_pollBackoff.noteFailure(
                        QStringLiteral("releaseBlob:") + hash, nowMs,
                        5LL * 60 * 1000, 6LL * 60 * 60 * 1000);
                    if (!ok)
                        logSystem(QStringLiteral(
                                      "Mirror: release artifact %1 download failed "
                                      "(%2); will retry after a backoff.")
                                      .arg(hash.left(12), netError));
                    else
                        logSystem(QStringLiteral(
                                      "Mirror: release artifact %1 failed checksum, "
                                      "discarded.")
                                      .arg(hash.left(12)));
                }
                downloadNextReleaseBlob(index, mirrorPath, pending);
            });
}

void MainWindow::pushReleaseToMirrors(const QString &tag)
{
    pushReleaseToMirrors(m_repoDetailIndex, tag);
}

void MainWindow::pushReleaseToMirrors(int index, const QString &tag)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.previewOnly || repo.localPath.trimmed().isEmpty()) {
        setRepoDetailNotice(
            "Only the repository's source working copy can push releases.", true);
        return;
    }
    if (repo.isPrivate) {
        setRepoDetailNotice(
            "Private repositories use sealed replica synchronization, not SSH "
            "mirror pushes.",
            true);
        return;
    }

    const QString key = repo.owner + QLatin1Char('/') + repo.name +
                        QLatin1Char('|') + tag;
    if (m_releaseMirrorPushResolving.contains(key))
        return;
    m_releaseMirrorPushResolving.insert(key);
    const QString tagRef = QStringLiteral("refs/tags/") + tag;
    runOffThread<QString>(
        [localPath = repo.localPath, tagRef] {
            QByteArray commitOut;
            if (!runGitCapture(localPath,
                               {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                tagRef + QStringLiteral("^{commit}")},
                               &commitOut, nullptr))
                return QString();
            return QString::fromUtf8(commitOut).trimmed() + QLatin1Char('\n');
        },
        [this, key, tag, owner = repo.owner, name = repo.name,
         localPath = repo.localPath](QString resolved) {
            m_releaseMirrorPushResolving.remove(key);
            if (resolved.isEmpty()) {
                setRepoDetailNotice(
                    QStringLiteral("Release %1 no longer resolves to a commit.").arg(tag),
                    true);
                return;
            }
            continueReleasePushToMirrors(tag, owner, name, localPath,
                                         resolved.trimmed());
        });
}

void MainWindow::continueReleasePushToMirrors(const QString &tag,
                                              const QString &owner,
                                              const QString &name,
                                              const QString &localPath,
                                              const QString &commit)
{
    const int index = repoIndexFor(owner, name);
    if (index < 0)
        return;
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.localPath != localPath || repo.previewOnly || repo.isPrivate)
        return;
    if (commit.isEmpty()) {
        setRepoDetailNotice(
            QStringLiteral("Release %1 has no commit to push.").arg(tag), true);
        return;
    }

    const auto pushConfigured =
        [this, owner = repo.owner, name = repo.name,
         tag](int managedHosts) -> bool {
            const int currentIndex = repoIndexFor(owner, name);
            if (currentIndex < 0)
                return false;
            // Suppress the managed half of the ordinary propagation pass: the
            // verified direct fan-out has already completed.
            propagateRepoUpdate(currentIndex,
                                /*includeManagedHosts=*/false);
            QString idleReason;
            const int sshRemotes = pushToSshMirrorRemotes(
                currentIndex, /*userInitiated=*/true, tag, &idleReason);
            const int targets = managedHosts + sshRemotes;
            if (targets == 0) {
                if (idleReason.trimmed().isEmpty())
                    idleReason =
                        QStringLiteral("no SSH mirror gateway took it");
                if (currentIndex == m_repoDetailIndex) {
                    setRepoDetailNotice(
                        QStringLiteral(
                            "Notified online mirrors about %1, but no refs "
                            "were pushed: %2.")
                            .arg(tag, idleReason),
                        true);
                }
                // The relay broadcast above only wakes desktop peers in the
                // live room; the SSH-fed mirrors advance on this push alone.
                logSystem(
                    QStringLiteral("Release: %1 reached no SSH mirror — %2.")
                        .arg(tag, idleReason));
                return true;
            }
            logSystem(
                QStringLiteral("Release: fanning %1 out to %2 SSH mirror%3.")
                    .arg(tag)
                    .arg(targets)
                    .arg(targets == 1 ? QString() : QStringLiteral("s")));
            return true;
        };

    const bool managedProduct =
        repo.owner.trimmed().compare(QLatin1String("forkmesh"),
                                     Qt::CaseInsensitive) == 0 &&
        repo.name.trimmed().compare(QLatin1String("forkmesh"),
                                    Qt::CaseInsensitive) == 0;
    if (!managedProduct) {
        pushConfigured(0);
        return;
    }

    PublishedReleaseDeployment publishedRelease;
    QString releaseError;
    if (!publishedReleaseDeployment(index, tag, commit, &publishedRelease,
                                    &releaseError)) {
        setRepoDetailNotice(
            QStringLiteral("Release %1 refs were not pushed: %2")
                .arg(tag, releaseError),
            true);
        logSystem(QStringLiteral(
                      "Release: %1 refs were held back because its signed "
                      "artifact set could not be authenticated: %2")
                      .arg(tag, releaseError));
        return;
    }
    if (releasePublicationPendingForRepository(repo.owner, repo.name) &&
        !releasePublicationPending(repo.owner, repo.name, tag)) {
        setRepoDetailNotice(
            QStringLiteral(
                "Release %1 cannot be pushed while a newer release is still "
                "publishing its artifacts.")
                .arg(tag),
            true);
        return;
    }
    rememberReleasePublication(repo.owner, repo.name, tag);

    const int managedTargets = pushReleaseToManagedHosts(
        index, tag, publishedRelease.artifactSha256s,
        [this, owner = repo.owner, name = repo.name, tag,
         pushConfigured](int succeeded, int failed) {
            if (failed > 0) {
                const int currentIndex = repoIndexFor(owner, name);
                if (currentIndex >= 0 && currentIndex == m_repoDetailIndex) {
                    setRepoDetailNotice(
                        QStringLiteral(
                            "Release %1 remains pending: %2 managed mirror%3 "
                            "could not accept its signed artifacts and refs.")
                            .arg(tag)
                            .arg(failed)
                            .arg(failed == 1 ? QString()
                                             : QStringLiteral("s")),
                        true);
                }
                return;
            }
            if (!releasePublicationPending(owner, name, tag))
                return;
            if (!pushConfigured(succeeded))
                return;
            if (releasePublicationPending(owner, name, tag))
                clearReleasePublication(owner, name, tag);
        });
    logSystem(QStringLiteral(
                  "Release: seeding %1 signed artifact%2 for %3 to %4 managed "
                  "mirror target%5 before refs advance.")
                  .arg(publishedRelease.artifactSha256s.size())
                  .arg(publishedRelease.artifactSha256s.size() == 1
                           ? QString()
                           : QStringLiteral("s"))
                  .arg(tag)
                  .arg(managedTargets)
                  .arg(managedTargets == 1 ? QString()
                                            : QStringLiteral("s")));
}

void MainWindow::rememberReleaseRollout(const QString &owner,
                                        const QString &name,
                                        const QString &tag)
{
    rememberReleaseIntent(kPendingReleaseRolloutsSetting, owner, name, tag);
}

void MainWindow::rememberReleasePublication(const QString &owner,
                                            const QString &name,
                                            const QString &tag)
{
    // A source commit or the periodic safety sync can otherwise outrun the
    // release callback and push the metadata refs while CAS seeding is still
    // in flight. This separate intent ends as soon as refs are coherent; the
    // rollout intent remains until installation succeeds.
    rememberReleaseIntent(kPendingReleasePublicationsSetting, owner, name,
                          tag);
}

void MainWindow::discardSupersededReleaseIntents(const QString &owner,
                                                 const QString &name,
                                                 const QString &tag)
{
    const QString wantedOwner = owner.trimmed().toLower();
    const QString wantedName = name.trimmed().toLower();
    const QString wantedTag = tag.trimmed();
    if (wantedOwner.isEmpty() || wantedName.isEmpty() || wantedTag.isEmpty())
        return;

    QSettings settings;
    bool changed = false;
    const QStringList intentSettings{
        kPendingReleaseRolloutsSetting,
        kPendingReleasePublicationsSetting,
        kPendingReleaseArtifactPrunesSetting};
    for (const QString &setting : intentSettings) {
        QStringList pending = settings.value(setting).toStringList();
        const qsizetype before = pending.size();
        for (qsizetype i = pending.size() - 1; i >= 0; --i) {
            const QJsonArray parts =
                QJsonDocument::fromJson(pending.at(i).toUtf8()).array();
            if (parts.size() >= 3 &&
                parts.at(0).toString() == wantedOwner &&
                parts.at(1).toString() == wantedName &&
                parts.at(2).toString() != wantedTag) {
                pending.removeAt(i);
            }
        }
        if (pending.size() == before)
            continue;
        changed = true;
        if (pending.isEmpty())
            settings.remove(setting);
        else
            settings.setValue(setting, pending);
    }
    if (changed)
        settings.sync();
}

bool MainWindow::releasePublicationPending(const QString &owner,
                                           const QString &name,
                                           const QString &tag) const
{
    return releaseIntentPending(kPendingReleasePublicationsSetting, owner,
                                name, tag);
}

bool MainWindow::releaseRolloutPending(const QString &owner,
                                       const QString &name,
                                       const QString &tag) const
{
    return releaseIntentPending(kPendingReleaseRolloutsSetting, owner, name,
                                tag);
}

void MainWindow::clearReleaseRollout(const QString &owner,
                                     const QString &name,
                                     const QString &tag)
{
    clearReleaseIntent(kPendingReleaseRolloutsSetting, owner, name, tag);
}

bool MainWindow::releasePublicationPendingForRepository(
    const QString &owner, const QString &name) const
{
    return releaseIntentPendingForRepository(
        kPendingReleasePublicationsSetting, owner, name);
}

void MainWindow::clearReleasePublication(const QString &owner,
                                         const QString &name,
                                         const QString &tag)
{
    clearReleaseIntent(kPendingReleasePublicationsSetting, owner, name, tag);
}

void MainWindow::rememberReleaseArtifactPrune(const QString &owner,
                                              const QString &name,
                                              const QString &tag)
{
    rememberReleaseIntent(kPendingReleaseArtifactPrunesSetting, owner, name,
                          tag);
}

bool MainWindow::releaseArtifactPrunePending(const QString &owner,
                                             const QString &name,
                                             const QString &tag) const
{
    return releaseIntentPending(kPendingReleaseArtifactPrunesSetting, owner,
                                name, tag);
}

void MainWindow::clearReleaseArtifactPrune(const QString &owner,
                                           const QString &name,
                                           const QString &tag)
{
    clearReleaseIntent(kPendingReleaseArtifactPrunesSetting, owner, name, tag);
}

bool MainWindow::publishedReleaseDeployment(
    int index, const QString &tag, const QString &buildCommit,
    PublishedReleaseDeployment *deployment, QString *error) const
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!deployment || index < 0 || index >= m_repositories.size())
        return fail(QStringLiteral("The release repository is no longer open."));
    const RepositoryRecord repo = m_repositories.at(index);
    const QString path =
        QDir(repo.localPath)
            .filePath(QStringLiteral(".forkmesh/releases/latest/release.json"));
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 ||
        info.size() > 1024 * 1024) {
        return fail(QStringLiteral(
            "The landed release manifest is missing or unsafe."));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("The landed release manifest cannot be read."));
    const QByteArray bytes = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("The landed release manifest is not valid JSON."));
    const QJsonObject manifest = document.object();
    const QString manifestTag =
        manifest.value(QStringLiteral("tag")).toString().trimmed();
    const QString commit =
        manifest.value(QStringLiteral("build_commit"))
            .toString().trimmed().toLower();
    const QString tagCommit =
        manifest.value(QStringLiteral("tag_commit"))
            .toString().trimmed().toLower();
    const QString wantedCommit = buildCommit.trimmed().toLower();
    const QString checksums =
        manifest.value(QStringLiteral("checksums_sha256"))
            .toString().trimmed().toLower();
    static const QRegularExpression exactCommit(
        QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
    static const QRegularExpression exactDigest(
        QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression exactVersion(
        QStringLiteral("^[0-9]+\\.[0-9]+\\.[0-9]+$"));
    QString version = manifestTag;
    if (version.startsWith(QLatin1Char('v')))
        version.remove(0, 1);
    if (manifest.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("forkmesh-release-v2") ||
        manifestTag != tag ||
        !exactCommit.match(wantedCommit).hasMatch() ||
        !exactCommit.match(commit).hasMatch() ||
        tagCommit != wantedCommit ||
        !exactDigest.match(checksums).hasMatch() ||
        !exactVersion.match(version).hasMatch()) {
        return fail(QStringLiteral(
            "The signed release manifest does not match the requested tag and "
            "source commit."));
    }
    const QString expectedRepo =
        repoSegment(repo.owner, QStringLiteral("owner")) + QLatin1Char('/') +
        repoSegment(repo.name, QStringLiteral("repository"));
    if (manifest.value(QStringLiteral("repo")).toString().trimmed()
            .compare(expectedRepo, Qt::CaseInsensitive) != 0) {
        return fail(QStringLiteral(
            "The signed release manifest names a different repository."));
    }
    QString linuxX86_64Sha256;
    QString linuxArm64Sha256;
    QStringList artifactSha256s;
    const QJsonArray assets = manifest.value(QStringLiteral("assets")).toArray();
    if (assets.isEmpty())
        return fail(QStringLiteral("The release manifest has no signed assets."));
    QHash<QString, QString> assetHashesByName;
    for (const QJsonValue &value : assets) {
        if (!value.isObject())
            return fail(QStringLiteral(
                "The release manifest contains an invalid asset entry."));
        const QJsonObject asset = value.toObject();
        const QString assetName =
            asset.value(QStringLiteral("name")).toString().trimmed();
        const QString assetHash =
            asset.value(QStringLiteral("blob_sha256"))
                .toString().trimmed().toLower();
        if (assetName.isEmpty() ||
            !exactDigest.match(assetHash).hasMatch()) {
            return fail(QStringLiteral(
                "The release manifest contains an invalid signed asset."));
        }
        if (assetHashesByName.contains(assetName) &&
            assetHashesByName.value(assetName) != assetHash) {
            return fail(QStringLiteral(
                "The release manifest assigns conflicting blobs to one "
                "asset name."));
        }
        assetHashesByName.insert(assetName, assetHash);
        if (!artifactSha256s.contains(assetHash))
            artifactSha256s.append(assetHash);
        if (assetName ==
            QLatin1String("forkmesh-mirror-node-linux-x86_64")) {
            if (!linuxX86_64Sha256.isEmpty() &&
                linuxX86_64Sha256 != assetHash) {
                return fail(QStringLiteral(
                    "The release contains conflicting x86_64 mirror-node "
                    "assets."));
            }
            linuxX86_64Sha256 = assetHash;
        } else if (assetName ==
                   QLatin1String("forkmesh-mirror-node-linux-arm64")) {
            if (!linuxArm64Sha256.isEmpty() &&
                linuxArm64Sha256 != assetHash) {
                return fail(QStringLiteral(
                    "The release contains conflicting arm64 mirror-node "
                    "assets."));
            }
            linuxArm64Sha256 = assetHash;
        }
    }
    if (linuxX86_64Sha256.isEmpty() && linuxArm64Sha256.isEmpty()) {
        return fail(QStringLiteral(
            "The release has no verified Linux mirror-node artifact."));
    }
    deployment->tag = tag;
    deployment->version = version;
    deployment->buildCommit = commit;
    deployment->manifestSha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    deployment->linuxX86_64Sha256 = linuxX86_64Sha256;
    deployment->linuxArm64Sha256 = linuxArm64Sha256;
    deployment->artifactSha256s = artifactSha256s;
    if (error)
        error->clear();
    return true;
}

void MainWindow::promptNewRelease()
{
    promptNewReleaseThen({});
}

void MainWindow::promptNewReleaseThen(std::function<void(bool)> done)
{
    if (!repoHasWorkingTree()) {
        setRepoDetailNotice("This is a read-only mirror; releases can't be created here.",
                            true);
        if (done)
            done(false);
        return;
    }
    if (m_newReleaseRunning) {
        if (done)
            done(false);
        return;
    }
    const QString dir = repoGitDir();
    const QStringList branches = repoBranches();
    const QString target = m_repoBranch.isEmpty() ? repoDefaultBranch(branches)
                                                  : m_repoBranch;
    const QString commandTag = m_desktopCommandReleaseTag;
    m_desktopCommandReleaseTag.clear();
    const QString request = releasesLoadRequest();
    m_newReleaseRunning = true;
    runOffThread<QByteArray>(
        [dir] {
            QByteArray out;
            if (dir.isEmpty() ||
                !runGitCapture(dir,
                               {"for-each-ref", "--sort=-creatordate", "--count=1",
                                "--format=%(refname:short)%09%(contents:subject)",
                                "refs/tags"},
                               &out, nullptr))
                return QByteArray();
            return out;
        },
        [this, request, dir, branches, target, commandTag,
         done](QByteArray latestTag) {
            if (request != releasesLoadRequest() || !repoHasWorkingTree()) {
                m_newReleaseRunning = false;
                if (done)
                    done(false);
                return;
            }
            draftNewRelease(dir, branches, target, commandTag, latestTag, done);
        });
}

void MainWindow::draftNewRelease(const QString &dir, const QStringList &branches,
                                 const QString &target,
                                 const QString &commandTag,
                                 const QByteArray &latestTag,
                                 std::function<void(bool)> done)
{
    const auto finish = [this, done](bool created) {
        m_newReleaseRunning = false;
        if (done)
            done(created);
    };
    const forkmesh::releasedraft::Latest latest =
        forkmesh::releasedraft::parseLatest(latestTag);
    const QString prevTag = latest.tag;
    QString suggestedTag = forkmesh::releasedraft::nextTag(prevTag);
    if (!commandTag.isEmpty())
        suggestedTag = commandTag;
    const QString suggestedTitle =
        forkmesh::releasedraft::nextTitle(prevTag, latest.title, suggestedTag);
    QDialog dialog(this);
    dialog.setWindowTitle("Draft a new release");
    auto *form = new QFormLayout(&dialog);
    auto *tagEdit = new QLineEdit;
    tagEdit->setPlaceholderText("v1.0.0");
    if (!suggestedTag.isEmpty())
        tagEdit->setText(suggestedTag);
    auto *targetEdit = new QComboBox;
    targetEdit->addItems(branches);
    const int targetIdx = targetEdit->findText(target);
    if (targetIdx >= 0)
        targetEdit->setCurrentIndex(targetIdx);
    auto *titleEdit = new QLineEdit;
    titleEdit->setPlaceholderText("Release title (optional)");
    if (!suggestedTitle.isEmpty())
        titleEdit->setText(suggestedTitle);
    auto *notesEdit = new QPlainTextEdit;
    notesEdit->setPlaceholderText("Describe this release...");
    notesEdit->setMinimumHeight(120);
    auto *pruneArtifactsCheck = new QCheckBox(
        QStringLiteral("Delete previous release artifacts from this node"));
    pruneArtifactsCheck->setToolTip(QStringLiteral(
        "After the release tag is created, delete every artifact currently "
        "stored under this repo's local forkmesh-releases/sha256 store."));
    auto *pruneTagsCheck = new QCheckBox(
        QStringLiteral("Delete previous release tags"));
    pruneTagsCheck->setToolTip(QStringLiteral(
        "After the new release tag is created, delete every other release tag "
        "in this repository's history, keeping only the one just published. "
        "This cannot be undone, and links to those older releases will stop "
        "working."));
    auto *rolloutCheck = new QCheckBox(
        QStringLiteral("Roll out the release to the mirror nodes"));
    const bool canRollOut =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size() &&
        releaseRolloutAllowed(m_repositories.at(m_repoDetailIndex));
    rolloutCheck->setEnabled(canRollOut);
    if (canRollOut) {
        pruneArtifactsCheck->setToolTip(QStringLiteral(
            "Keep the currently published artifacts available while the "
            "release workflow builds their replacements, then delete the "
            "older blobs only after the new signed artifacts have landed in "
            "the local forkmesh-releases/sha256 store."));
    }
    rolloutCheck->setToolTip(
        canRollOut
            ? QStringLiteral(
                  "After the release workflow publishes every signed artifact, "
                  "seed those bytes and refs to each installed saved host, then "
                  "install the exact mirror-node artifact.")
            : QStringLiteral(
                  "The canonical forkmesh/forkmesh source working copy with "
                  "Actions enabled is required for a mirror-node rollout."));
    auto *mastodonCheck = new QCheckBox(
        QStringLiteral("Post the release to Mastodon"));
    mastodonCheck->setToolTip(QStringLiteral(
        "After the release tag is created, open a Mastodon post with the "
        "release title, notes and link filled in, ready to publish from your "
        "own account. Uses the instance from your profile's Mastodon handle "
        "when one is set."));
    auto *genNotesButton = new QPushButton("Generate release notes");
    genNotesButton->setObjectName("ghostButton");
    genNotesButton->setCursor(Qt::PointingHandCursor);
    setOcticon(genNotesButton, "list-unordered", 14);
    connect(genNotesButton, &QPushButton::clicked, &dialog,
            [this, dir, prevTag, tagEdit, targetEdit, notesEdit, genNotesButton] {
                const QString targetRef = targetEdit->currentText().trimmed();
                if (targetRef.isEmpty())
                    return;
                const QStringList args =
                    forkmesh::releasedraft::logArgs(prevTag, targetRef);
                genNotesButton->setEnabled(false);
                const QPointer<QLineEdit> tagGuard(tagEdit);
                const QPointer<QPlainTextEdit> notesGuard(notesEdit);
                const QPointer<QPushButton> buttonGuard(genNotesButton);
                runOffThread<QByteArray>(
                    [dir, args] {
                        QByteArray log;
                        runGitCapture(dir, args, &log, nullptr);
                        return log;
                    },
                    [prevTag, targetRef, tagGuard, notesGuard,
                     buttonGuard](QByteArray log) {
                        if (buttonGuard)
                            buttonGuard->setEnabled(true);
                        if (!notesGuard || !tagGuard)
                            return;
                        notesGuard->setPlainText(forkmesh::releasedraft::notesFromLog(
                            log, prevTag, tagGuard->text().trimmed(), targetRef));
                    });
            });
    auto *agentNotesButton = new QPushButton("Generate release notes with agent");
    agentNotesButton->setObjectName("ghostButton");
    agentNotesButton->setCursor(Qt::PointingHandCursor);
    setOcticon(agentNotesButton, "rocket", 14);
    auto *agentNotesCombo = new QComboBox;
    agentNotesCombo->addItem(QStringLiteral("Codex"), kCodexProvider);
    agentNotesCombo->addItem(QStringLiteral("OpenAI API"),
                             QStringLiteral("openai"));
    agentNotesCombo->addItem(QStringLiteral("Claude API"),
                             QStringLiteral("claude-api"));
    agentNotesCombo->addItem(QStringLiteral("Claude Code"),
                             QStringLiteral("claude-code"));
    selectDefaultAgentProvider(agentNotesCombo);
    agentNotesCombo->setToolTip("Which agent writes the release notes");
    auto *agentNotesModelCombo = new QComboBox;
    agentNotesModelCombo->setToolTip("Which model the agent uses");
    auto populateModels = [agentNotesModelCombo](const QString &provider) {
        const QSignalBlocker block(agentNotesModelCombo);
        agentNotesModelCombo->clear();
        const bool claude = agentIsClaudeProvider(provider);
        if (claude) {
            agentNotesModelCombo->addItem(QStringLiteral("Haiku"),
                                          QStringLiteral("claude-haiku-4-5"));
            agentNotesModelCombo->addItem(QStringLiteral("Sonnet"),
                                          QStringLiteral("claude-sonnet-5"));
            agentNotesModelCombo->addItem(QStringLiteral("Opus"),
                                          QStringLiteral("claude-opus-5"));
        } else {
            agentNotesModelCombo->addItem(QStringLiteral("GPT nano"),
                                          QStringLiteral("gpt-4.1-nano"));
            agentNotesModelCombo->addItem(QStringLiteral("GPT mini"),
                                          QStringLiteral("gpt-4.1-mini"));
            agentNotesModelCombo->addItem(QStringLiteral("GPT"),
                                          QStringLiteral("gpt-4.1"));
        }
        selectModelComboValue(
            agentNotesModelCombo,
            QSettings()
                .value(claude ? kReleaseNotesClaudeModelSetting
                              : kReleaseNotesGptModelSetting)
                .toString());
    };
    populateModels(agentNotesCombo->currentData().toString());
    connect(agentNotesCombo, &QComboBox::currentTextChanged, &dialog,
            [agentNotesCombo, populateModels](const QString &) {
                populateModels(agentNotesCombo->currentData().toString());
            });
    auto persistReleaseNotesModel = [agentNotesCombo, agentNotesModelCombo] {
        const bool claude =
            agentIsClaudeProvider(agentNotesCombo->currentData().toString());
        QSettings().setValue(claude ? kReleaseNotesClaudeModelSetting
                                     : kReleaseNotesGptModelSetting,
                             selectedModelComboValue(agentNotesModelCombo));
    };
    connect(agentNotesModelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            &dialog, [persistReleaseNotesModel](int) { persistReleaseNotesModel(); });
    auto *agentNotesRow = new QWidget;
    auto *agentNotesRowLayout = new QHBoxLayout(agentNotesRow);
    agentNotesRowLayout->setContentsMargins(0, 0, 0, 0);
    agentNotesRowLayout->setSpacing(6);
    agentNotesRowLayout->addWidget(agentNotesButton);
    agentNotesRowLayout->addWidget(agentNotesCombo);
    agentNotesRowLayout->addWidget(agentNotesModelCombo);
    agentNotesRowLayout->addStretch(1);
    connect(agentNotesButton, &QPushButton::clicked, &dialog,
            [this, dir, prevTag, tagEdit, targetEdit, notesEdit, agentNotesButton,
             agentNotesCombo, agentNotesModelCombo] {
                const QString targetRef = targetEdit->currentText().trimmed();
                if (targetRef.isEmpty())
                    return;
                generateReleaseNotesWithAgent(
                    dir, prevTag, tagEdit->text().trimmed(), targetRef,
                    agentNotesCombo->currentData().toString(),
                    agentNotesModelCombo->currentData().toString(), notesEdit,
                    agentNotesButton);
            });
    form->addRow("Tag", tagEdit);
    form->addRow("Target", targetEdit);
    form->addRow("Title", titleEdit);
    form->addRow(QString(), genNotesButton);
    form->addRow(QString(), agentNotesRow);
    form->addRow("Notes", notesEdit);
    form->addRow(QString(), pruneArtifactsCheck);
    form->addRow(QString(), pruneTagsCheck);
    form->addRow(QString(), rolloutCheck);
    form->addRow(QString(), mastodonCheck);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("Publish release");
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    tagEdit->setFocus();
    tagEdit->selectAll();
    if (dialog.exec() != QDialog::Accepted || repoGitDir() != dir ||
        m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        finish(false);
        return;
    }

    const QString tag = tagEdit->text().trimmed();
    if (tag.isEmpty()) {
        setRepoDetailNotice("A release needs a tag name.", true);
        finish(false);
        return;
    }
    const RepositoryRecord &draftRepo = m_repositories.at(m_repoDetailIndex);
    forkmesh::releasedraft::Cut cut;
    cut.owner = draftRepo.owner;
    cut.name = draftRepo.name;
    cut.dir = dir;
    cut.tag = tag;
    cut.targetRef = targetEdit->currentText().trimmed();
    cut.title = titleEdit->text().trimmed();
    cut.notes = notesEdit->toPlainText().trimmed();
    cut.canRollOut = canRollOut;
    cut.rollout = rolloutCheck->isChecked() && rolloutCheck->isEnabled();
    cut.pruneArtifacts = pruneArtifactsCheck->isChecked();
    cut.pruneTags = pruneTagsCheck->isChecked();
    cut.postToMastodon = mastodonCheck->isChecked();
    cutRelease(cut, [this, finish](bool created, const QString &detail) {
        if (!created)
            setRepoDetailNotice(detail, true);
        finish(created);
    });
}

bool MainWindow::releaseRolloutAllowed(const RepositoryRecord &repo)
{
    return !repo.previewOnly && !repo.isPrivate && !repo.localPath.trimmed().isEmpty() &&
           repo.actionsEnabled &&
           repo.owner.trimmed().compare(QLatin1String("forkmesh"), Qt::CaseInsensitive) ==
               0 &&
           repo.name.trimmed().compare(QLatin1String("forkmesh"), Qt::CaseInsensitive) == 0;
}

void MainWindow::cutRelease(const forkmesh::releasedraft::Cut &cut,
                            std::function<void(bool, const QString &)> done)
{
    const auto fail = [done](const QString &detail) {
        if (done)
            done(false, detail);
    };
    const int startIndex = repoIndexFor(cut.owner, cut.name);
    if (startIndex < 0 || cut.dir.isEmpty()) {
        fail(QStringLiteral("The repository is not checked out on this desktop."));
        return;
    }
    if (!forkmesh::releasedraft::tagNameValid(cut.tag)) {
        fail(QStringLiteral("%1 is not a valid release tag.").arg(cut.tag));
        return;
    }
    if (cut.targetRef.isEmpty()) {
        fail(QStringLiteral("A release needs a target branch."));
        return;
    }
    if (cut.rollout && !forkmesh::releasedraft::deployableTag(cut.tag)) {
        fail(QStringLiteral("A mirror-node rollout requires a release tag such as v1.2.3."));
        return;
    }
    const RepositoryRecord &startRepo = m_repositories.at(startIndex);
    const QString dir = cut.dir;
    const QString targetRef = cut.targetRef;
    const QString tag = cut.tag;
    const QString message = forkmesh::releasedraft::tagMessage(cut.title, cut.notes, tag);
    const QString releaseTitle = cut.title.trimmed().isEmpty() ? tag : cut.title.trimmed();
    const QString notes = cut.notes.trimmed();
    const bool actionsEnabled = startRepo.actionsEnabled;
    const QString mirrorPath = startRepo.mirrorPath;
    const QString owner = cut.owner;
    const QString name = cut.name;
    const bool canRollOut = cut.canRollOut;
    const bool rollout = cut.rollout;
    const bool postToMastodon = cut.postToMastodon;
    const bool pruneArtifacts = cut.pruneArtifacts;
    const bool pruneTags = cut.pruneTags;

    runOffThread<NewReleasePublish>(
        [dir, targetRef, tag, message, actionsEnabled, mirrorPath] {
            NewReleasePublish publish;
            if (runGitCapture(dir, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                    QStringLiteral("--quiet"),
                                    QStringLiteral("refs/tags/") + tag},
                              nullptr, nullptr)) {
                publish.err = QStringLiteral("Release %1 already exists.").arg(tag);
                return publish;
            }
            if (!runGitCapture(dir, {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                                     QStringLiteral("--quiet"),
                                     targetRef + QStringLiteral("^{commit}")},
                               nullptr, nullptr)) {
                publish.err = QStringLiteral("%1 is not a branch or commit in this "
                                             "repository.")
                                  .arg(targetRef);
                return publish;
            }
            if (!releaseStampVersion(tag).isEmpty())
                publish.stamped =
                    stampReleaseTarget(dir, targetRef, tag, &publish.skipped);

            if (!runGitCapture(dir,
                               {"-c", QStringLiteral("user.email=actions@forkmesh.local"),
                                "-c", QStringLiteral("user.name=ForkMesh Actions"), "tag",
                                "-a", tag, targetRef, "-m", message},
                               nullptr, &publish.err))
                return publish;
            publish.tagged = true;
            if (actionsEnabled) {
                if (!mirrorPath.trimmed().isEmpty() && dir != mirrorPath &&
                    QDir(mirrorPath).exists())
                    runGitCapture(mirrorPath,
                                  {QStringLiteral("fetch"), dir,
                                   QStringLiteral("+refs/heads/*:refs/heads/*"),
                                   QStringLiteral("+refs/tags/*:refs/tags/*")},
                                  nullptr, nullptr);
                QByteArray tip;
                if (runGitCapture(dir, {"rev-parse", "--verify", tag + "^{commit}"},
                                  &tip, nullptr))
                    publish.tip = QString::fromUtf8(tip).trimmed();
            }
            return publish;
        },
        [this, done, dir, tag, targetRef, releaseTitle, notes, owner, name,
         canRollOut, postToMastodon, pruneArtifacts, pruneTags,
         rollout](NewReleasePublish publish) {
            QString stampWarning;
            if (!publish.stamped.isEmpty())
                logSystem(QStringLiteral("Bumped the version to match %1 in %2.")
                              .arg(tag, publish.stamped.join(QStringLiteral(", "))));
            if (!publish.skipped.isEmpty()) {
                stampWarning = QStringLiteral("The version was not bumped in %1.")
                                   .arg(publish.skipped.join(QStringLiteral(", ")));
                logSystem(QStringLiteral("Release %1: %2").arg(tag, stampWarning));
            }
            if (!publish.tagged) {
                const QString detail = publish.err.trimmed().isEmpty()
                                           ? QStringLiteral("Could not create the "
                                                            "release tag.")
                                           : publish.err.trimmed();
                if (done)
                    done(false, detail);
                return;
            }
            const int index = repoIndexFor(owner, name);
            const bool current = index >= 0 && index == m_repoDetailIndex &&
                                 repoGitDir() == dir;
            logSystem(QStringLiteral("Git: tagged release %1 at %2.").arg(tag, targetRef));
            if (stampWarning.isEmpty())
                setRepoDetailNotice(QStringLiteral("Published release %1.").arg(tag));
            else
                setRepoDetailNotice(
                    QStringLiteral("Published release %1. %2").arg(tag, stampWarning),
                    true);
            if (index >= 0) {
                announceReleaseOnFediverse(index, tag, releaseTitle, notes);
                if (postToMastodon)
                    postReleaseToMastodon(index, tag, releaseTitle, notes);
            }
            if (current)
                loadBranchesAndTags();
            if (pruneArtifacts && index >= 0) {
                const RepositoryRecord releaseRepo = m_repositories.at(index);
                if (canRollOut) {
                    rememberReleaseArtifactPrune(releaseRepo.owner, releaseRepo.name,
                                                 tag);
                    logSystem(QStringLiteral(
                                  "Artifacts: keeping the current release available "
                                  "until %1's signed replacements land.")
                                  .arg(tag));
                } else {
                    pruneReleaseArtifacts(index, tag);
                }
            }
            if (pruneTags && index >= 0)
                pruneReleaseTagsForRepo(index, dir, tag);
            if (rollout && index >= 0) {
                const RepositoryRecord rolloutRepo = m_repositories.at(index);
                rememberReleaseRollout(rolloutRepo.owner, rolloutRepo.name, tag);
                logSystem(QStringLiteral(
                              "Release: %1 will publish refs and deploy to installed "
                              "mirror hosts after all signed artifacts land.")
                              .arg(tag));
            }
            bool queued = false;
            if (index >= 0 && !publish.tip.isEmpty()) {
                const RepositoryRecord &repo = m_repositories.at(index);
                if (repo.actionsEnabled) {
                    queueWorkflowsForCommit(index, repo.owner, repo.name,
                                            publish.tip,
                                            QStringLiteral("refs/tags/") + tag,
                                            WorkflowTrigger::Release);
                    queued = true;
                }
            }
            if (done) {
                QStringList detail{QStringLiteral("Tagged %1 at %2.").arg(tag, targetRef)};
                if (!publish.stamped.isEmpty())
                    detail << QStringLiteral("Version bumped in %1.")
                                  .arg(publish.stamped.join(QStringLiteral(", ")));
                if (!stampWarning.isEmpty())
                    detail << stampWarning;
                if (queued)
                    detail << QStringLiteral("Release workflows queued.");
                if (rollout && index >= 0)
                    detail << QStringLiteral("Mirror rollout follows once every signed "
                                             "artifact lands.");
                done(true, detail.join(QLatin1Char(' ')));
            }
        });
}

void MainWindow::generateReleaseNotesWithAgent(
    const QString &dir, const QString &prevTag, const QString &newTag,
    const QString &targetRef, const QString &provider, const QString &modelChoice,
    QPlainTextEdit *notesEdit, QPushButton *button)
{
    if (!m_networkAccess || !notesEdit || targetRef.isEmpty())
        return;

    QStringList args{"log", "--no-merges", "--date-order",
                     "--format=%s%x1f%h%x1f%an"};
    if (prevTag.isEmpty())
        args << QStringLiteral("--max-count=250") << targetRef;
    else
        args << QStringLiteral("%1..%2").arg(prevTag, targetRef);
    if (button && !button->isEnabled())
        return;
    if (button)
        button->setEnabled(false);
    const QPointer<QPlainTextEdit> notesGuard(notesEdit);
    const QPointer<QPushButton> buttonGuard(button);
    runOffThread<QByteArray>(
        [dir, args] {
            QByteArray log;
            runGitCapture(dir, args, &log, nullptr);
            return log;
        },
        [this, prevTag, newTag, targetRef, provider, modelChoice, notesGuard,
         buttonGuard](QByteArray log) {
            if (buttonGuard)
                buttonGuard->setEnabled(true);
            if (!notesGuard)
                return;
            sendReleaseNotesRequest(log, prevTag, newTag, targetRef, provider,
                                    modelChoice, notesGuard.data(),
                                    buttonGuard.data());
        });
}

void MainWindow::sendReleaseNotesRequest(
    const QByteArray &log, const QString &prevTag, const QString &newTag,
    const QString &targetRef, const QString &provider, const QString &modelChoice,
    QPlainTextEdit *notesEdit, QPushButton *button)
{
    if (!m_networkAccess)
        return;
    QStringList commits;
    for (const QByteArray &line : log.split('\n')) {
        const QString entry = QString::fromUtf8(line).trimmed();
        if (entry.isEmpty())
            continue;
        const QStringList f = entry.split(QLatin1Char('\x1f'));
        const QString subject = f.value(0).trimmed();
        const QString sha = f.value(1).trimmed();
        const QString author = f.value(2).trimmed();
        if (subject.isEmpty())
            continue;
        QString c = QStringLiteral("- %1 (%2)").arg(subject, sha);
        if (!author.isEmpty())
            c += QStringLiteral(" by %1").arg(author);
        commits << c;
    }
    if (commits.isEmpty()) {
        setRepoDetailNotice(
            prevTag.isEmpty()
                ? QStringLiteral("No commits to write release notes from.")
                : QStringLiteral("No changes since %1 to write notes from.")
                      .arg(prevTag),
            true);
        return;
    }
    if (commits.size() > 250)
        commits = commits.mid(0, 250);

    const bool claude = agentIsClaudeProvider(provider);
    const bool claudeCode = provider == QLatin1String("claude-code");
    const QString oauthToken = claudeCode ? claudeCodeOAuthToken() : QString();
    const QString apiKey =
        claudeCode ? QString()
                   : (claude ? QSettings().value(kClaudeApiKeySetting)
                             : QSettings().value(kCodexApiKeySetting))
                         .toString()
                         .trimmed();
    if (apiKey.isEmpty() && oauthToken.isEmpty()) {
        setRepoDetailNotice(
            claudeCode
                ? QStringLiteral(
                      "Sign in to Claude Code first (run `claude` and log in).")
                : claude ? QStringLiteral("Add a Claude API key in Settings first.")
                         : QStringLiteral(
                               "Add an OpenAI API key in Settings first."),
            true, forkmesh::kLocalOperationNoticeKind);
        return;
    }

    const QString version = newTag.isEmpty() ? targetRef : newTag;
    const QString task =
        QStringLiteral(
            "Write the release notes for version %1 as a single friendly, "
            "conversational paragraph — like a nicely written changelog blurb a "
            "human would post. Highlight the most important changes in flowing "
            "prose, skip noise like version bumps, and don't use headings, bullet "
            "points, or Markdown lists. Keep it under 500 characters. Output only "
            "the paragraph, no preamble.\n\n----- COMMITS -----\n%2")
            .arg(version, commits.join(QLatin1Char('\n')));
    const QString model =
        !modelChoice.isEmpty()
            ? modelChoice
            : (claude ? QStringLiteral("claude-haiku-4-5") : kIssueAskAiModel);

    QNetworkReply *reply = nullptr;
    if (claude) {
        QJsonObject payload;
        payload.insert("model", model);
        payload.insert("max_tokens", 2000);
        QJsonArray messages;
        QJsonObject um;
        um.insert("role", "user");
        um.insert("content", task);
        messages.append(um);
        payload.insert("messages", messages);
        QNetworkRequest req(
            QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
        if (!oauthToken.isEmpty()) {
            req.setRawHeader("Authorization", "Bearer " + oauthToken.toUtf8());
            req.setRawHeader("anthropic-beta", "oauth-2025-04-20");
            payload.insert("system", kClaudeCodeOAuthSystem);
        } else {
            req.setRawHeader("x-api-key", apiKey.toUtf8());
        }
        req.setRawHeader("anthropic-version", "2023-06-01");
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    } else {
        QJsonObject payload;
        payload.insert("model", model);
        payload.insert("input", task);
        payload.insert("max_output_tokens", 2000);
        QNetworkRequest req = openAiRequest(
            QUrl(QStringLiteral("https://api.openai.com/v1/responses")), apiKey);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

    QPointer<QPlainTextEdit> notesGuard(notesEdit);
    QPointer<QPushButton> buttonGuard(button);
    if (button) {
        button->setEnabled(false);
        button->setText(QString::fromUtf8("Generating\xE2\x80\xA6"));
    }
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, claude, notesGuard, buttonGuard] {
                const QByteArray body = reply->readAll();
                reply->deleteLater();
                if (buttonGuard) {
                    buttonGuard->setEnabled(true);
                    buttonGuard->setText("Generate release notes with agent");
                }
                if (reply->error() != QNetworkReply::NoError) {
                    const int status = reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    setRepoDetailNotice(
                        "Release-notes request failed: " +
                            apiErrorSummary(reply, body),
                        true,
                        status == 401 || status == 403
                            ? forkmesh::kLocalOperationNoticeKind
                            : QString());
                    return;
                }
                const QJsonObject obj = QJsonDocument::fromJson(body).object();
                QString text;
                if (claude) {
                    for (const QJsonValue &v : obj.value("content").toArray()) {
                        const QJsonObject o = v.toObject();
                        if (o.value("type").toString() == QLatin1String("text"))
                            text += o.value("text").toString();
                    }
                } else {
                    text = openAiResponseText(obj);
                }
                text = text.trimmed();
                if (text.isEmpty()) {
                    setRepoDetailNotice(
                        "The agent returned no release notes.", true);
                    return;
                }
                if (notesGuard)
                    notesGuard->setPlainText(text);
                else
                    setRepoDetailNotice(
                        "Release notes are ready, but the draft dialog was "
                        "closed.",
                        true);
            });
}

// Announce a freshly published release to the repo's fediverse followers.
// Releases never pass through the relay's signed inboxes (they are canonical
// on this node), so the relay can only federate them when the owner node
// pushes the announcement itself: POST /api/repo/<o>/<r>/ap-publish, gated by
// the same forkmesh-issues-pull-v1 signed token as the inbox drains.
// Best-effort fire-and-forget — a relay hiccup must never affect the release.
void MainWindow::announceReleaseOnFediverse(int repoIndex, const QString &tag,
                                            const QString &title,
                                            const QString &notes)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    if (!repo.publishToNetwork)
        return; // unpublished repos have no public fediverse actor
    const QString owner = repoSegment(repo.owner, QStringLiteral("owner"));
    if (!hasOwnerSigningCapability(owner))
        return;
    QUrl url = catalogApiUrl();
    url.setPath("/api/repo/" + owner + "/" +
                repoSegment(repo.name, QStringLiteral("repository")) +
                "/ap-publish");
    url.setQuery(signedInboxQuery(owner));
    QJsonObject body;
    body.insert(QStringLiteral("kind"), QStringLiteral("release"));
    body.insert(QStringLiteral("eventType"), QStringLiteral("publish"));
    body.insert(QStringLiteral("tag"), tag);
    body.insert(QStringLiteral("title"), title);
    body.insert(QStringLiteral("body"), notes.left(4000));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, tag] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
            logSystem(
                QStringLiteral("Fediverse: announced release %1 to followers.")
                    .arg(tag));
        else
            logSystem(
                QStringLiteral("Fediverse: could not announce release %1 (%2).")
                    .arg(tag, reply->errorString()));
    });
}

// Open a Mastodon composer prefilled with the release announcement so the user
// can post it from their own account. The composer lives on the instance named
// by the account profile's Mastodon handle (@you@example.social -> /share);
// without one, share.joinmastodon.org asks which instance to use.
void MainWindow::postReleaseToMastodon(int repoIndex, const QString &tag,
                                       const QString &title, const QString &notes)
{
    constexpr int kMastodonPostLimit = 500;
    QString link;
    if (repoIndex >= 0 && repoIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(repoIndex);
        if (repo.publishToNetwork && !repo.previewOnly && !repo.isPrivate &&
            !repo.owner.trimmed().isEmpty() && !repo.name.trimmed().isEmpty())
            link = QStringLiteral("https://forkmesh.com/%1/%2/releases")
                       .arg(QString::fromUtf8(QUrl::toPercentEncoding(
                                repo.owner.trimmed())),
                            QString::fromUtf8(QUrl::toPercentEncoding(
                                repo.name.trimmed())));
    }
    QString heading = title.trimmed().isEmpty() ? tag : title.trimmed();
    if (!heading.contains(tag))
        heading = QStringLiteral("%1 (%2)").arg(heading, tag);
    QString text = heading;
    const QString tail = link.isEmpty() ? QString() : QStringLiteral("\n\n") + link;
    const int room = kMastodonPostLimit - text.size() - tail.size() - 2;
    const QString body = notes.trimmed();
    if (!body.isEmpty() && room > 1) {
        text += QStringLiteral("\n\n");
        text += body.size() <= room ? body
                                    : body.left(room - 1).trimmed() +
                                          QChar(0x2026);
    }
    text += tail;

    QString instance;
    const QString handle = cachedAccountRecord(m_accountName)
                               .value(QStringLiteral("mastodon"))
                               .toString()
                               .trimmed();
    static const QRegularExpression handlePattern(QStringLiteral(
        "^@?[A-Za-z0-9_.-]+@([A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)+)$"));
    const QRegularExpressionMatch match = handlePattern.match(handle);
    if (match.hasMatch())
        instance = match.captured(1).toLower();
    QUrl composer(instance.isEmpty()
                      ? QStringLiteral("https://share.joinmastodon.org/")
                      : QStringLiteral("https://%1/share").arg(instance));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("text"), text);
    composer.setQuery(query);
    if (QDesktopServices::openUrl(composer))
        logSystem(QStringLiteral("Mastodon: opened a post for release %1.")
                      .arg(tag));
    else
        setRepoDetailNotice(
            QStringLiteral("Published release %1, but the Mastodon composer "
                           "could not be opened.")
                .arg(tag),
            true);
}

void MainWindow::showReleaseDetail(const QString &tag)
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || tag.isEmpty())
        return;
    const QString request = dir + QLatin1Char('|') + tag;
    if (m_releaseDetailLoading == request)
        return;
    m_releaseDetailLoading = request;
    const quint64 generation = ++m_releaseDetailGen;
    const bool split = diffSplitPref();
    const DiffRenderAppearance appearance = currentDiffRenderAppearance();
    runOffThread<ReleaseDetailRead>(
        [dir, tag, split, appearance] {
            return readReleaseDetail(dir, tag, split, appearance);
        },
        [this, generation, dir, tag](ReleaseDetailRead read) {
            if (generation != m_releaseDetailGen)
                return;
            m_releaseDetailLoading.clear();
            if (!read.exists || repoGitDir() != dir)
                return;
            openReleaseDetailDialog(tag, read.when, read.tagger, read.subject,
                                    read.body, read.prevTag, read.diffHtml);
        });
}

void MainWindow::openReleaseDetailDialog(const QString &tag, const QString &when,
                                         const QString &tagger,
                                         const QString &subject,
                                         const QString &body,
                                         const QString &prevTag,
                                         const QString &diffHtml)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Release %1").arg(tag));
    dialog.resize(900, 640);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setSpacing(10);

    auto *title = new QLabel(
        QStringLiteral("<b style='font-size:16px'>%1</b>")
            .arg((subject.isEmpty() ? tag : subject).toHtmlEscaped()));
    title->setTextFormat(Qt::RichText);
    title->setWordWrap(true);
    layout->addWidget(title);

    QStringList metaBits;
    if (!when.isEmpty())
        metaBits << when;
    if (!tagger.isEmpty())
        metaBits << tagger;
    if (!metaBits.isEmpty()) {
        auto *metaLabel = new QLabel(metaBits.join(QString::fromUtf8(" \xC2\xB7 ")));
        metaLabel->setObjectName("statusLine");
        layout->addWidget(metaLabel);
    }

    if (!body.isEmpty()) {
        auto *notes = new QLabel(
            QStringLiteral("<span style='white-space:pre-wrap'>%1</span>")
                .arg(body.toHtmlEscaped()));
        notes->setTextFormat(Qt::RichText);
        notes->setWordWrap(true);
        notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(notes);
    }

    auto *diffHeading = new QLabel(
        prevTag.isEmpty()
            ? QStringLiteral("Changes")
            : QStringLiteral("Changes since %1").arg(prevTag.toHtmlEscaped()));
    diffHeading->setObjectName("channelTitle");
    layout->addWidget(diffHeading);

    auto *diff = new QTextBrowser;
    diff->setObjectName("diffView");
    diff->setOpenLinks(false); // read-only diff; don't navigate on anchor clicks
    registerDiffView(diff);
    if (diffHtml.trimmed().isEmpty())
        setDiffHtml(diff,
            prevTag.isEmpty()
                ? QString::fromUtf8(
                      "<p style='color:#8b949e'>This is the earliest release "
                      "\xE2\x80\x94 no previous release to diff against.</p>")
                : QStringLiteral(
                      "<p style='color:#8b949e'>No file changes between %1 and "
                      "%2.</p>")
                      .arg(prevTag.toHtmlEscaped(), tag.toHtmlEscaped()));
    else
        setDiffHtml(diff, diffHtml);
    layout->addWidget(diff, 1);

    auto *buttons = new QDialogButtonBox;
    auto *browseBtn = buttons->addButton(QStringLiteral("Browse repo at this tag"),
                                         QDialogButtonBox::ActionRole);
    auto *fediBtn = buttons->addButton(QStringLiteral("Announce on fediverse"),
                                       QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(browseBtn, &QPushButton::clicked, &dialog, [this, &dialog, tag] {
        dialog.accept();
        setRepoBranch(tag); // browse the repo's files at this tag
    });
    connect(fediBtn, &QPushButton::clicked, &dialog,
            [this, tag, subject, body, fediBtn, repoIndex = m_repoDetailIndex] {
        fediBtn->setEnabled(false);
        announceReleaseOnFediverse(repoIndex, tag, subject, body);
    });
    layout->addWidget(buttons);

    dialog.exec();
}

void MainWindow::deleteTag(const QString &tag)
{
    if (tag.isEmpty() || !repoHasWorkingTree())
        return;
    const QString dir = repoGitDir();
    const QString key = dir + QLatin1Char('|') + tag;
    if (m_releaseTagDeletes.contains(key))
        return;
    if (QMessageBox::question(
            this, "Delete release",
            QStringLiteral("Delete release tag \"%1\"? This cannot be undone.").arg(tag),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes ||
        m_releaseTagDeletes.contains(key) || repoGitDir() != dir)
        return;
    m_releaseTagDeletes.insert(key);
    runOffThread<QString>(
        [dir, tag] {
            QString err;
            if (runGitCapture(dir, {"tag", "-d", tag}, nullptr, &err))
                return QString();
            return err.isEmpty() ? QStringLiteral("Could not delete the tag.") : err;
        },
        [this, key, dir, tag](QString err) {
            m_releaseTagDeletes.remove(key);
            if (!err.isEmpty()) {
                setRepoDetailNotice(err, true);
                return;
            }
            logSystem(QStringLiteral("Git: deleted tag %1.").arg(tag));
            setRepoDetailNotice(QStringLiteral("Deleted release %1.").arg(tag));
            if (repoGitDir() == dir)
                loadBranchesAndTags();
        });
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testSettleMirrorNodesPanel()
{
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 15000 &&
           (m_mirrorPanelGitInFlight || !m_mirrorSelfSnapshotsInFlight.isEmpty())) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(2);
    }
}

QStringList MainWindow::testMirrorNodeHeaderCaptions() const
{
    QStringList captions;
    if (!m_mirrorNodesTable)
        return captions;
    for (int c = 0; c < m_mirrorNodesTable->columnCount(); ++c) {
        const QTableWidgetItem *header = m_mirrorNodesTable->horizontalHeaderItem(c);
        if (header)
            captions.append(header->text());
    }
    return captions;
}

QString MainWindow::testMirrorNodeCellByCaption(const QString &nodeName,
                                                const QString &caption) const
{
    if (!m_mirrorNodesTable)
        return QString();
    for (int c = 0; c < m_mirrorNodesTable->columnCount(); ++c) {
        const QTableWidgetItem *header = m_mirrorNodesTable->horizontalHeaderItem(c);
        if (header && header->text() == caption)
            return testMirrorNodeCellText(nodeName, c);
    }
    return QString();
}

bool MainWindow::testMirrorSectionOnNodesPage() const
{
    if (!m_mirrorNodesTable || !m_networkTabs)
        return false;
    QWidget *nodesPage = m_networkTabs->widget(kNetworkNodesTab);
    return nodesPage && nodesPage->isAncestorOf(m_mirrorNodesTable) &&
           m_nodesTable && nodesPage->isAncestorOf(m_nodesTable);
}

int MainWindow::testVisibleMirrorNodeRows() const
{
    int visible = 0;
    if (!m_mirrorNodesTable)
        return visible;
    for (int row = 0; row < m_mirrorNodesTable->rowCount(); ++row)
        if (!m_mirrorNodesTable->isRowHidden(row))
            ++visible;
    return visible;
}

void MainWindow::testSetMirrorSectionCatalog(const QString &source,
                                             const QByteArray &body)
{
    const QString key = source.trimmed().toLower();
    m_networkRepoMirrorsCache.insert(key, body);
    m_networkRepoMirrorsFetchedMs.insert(key, QDateTime::currentMSecsSinceEpoch());
    loadMirrorNodesPanel();
    testSettleMirrorNodesPanel();
}

QString MainWindow::testMirrorNodesRepoSource() const
{
    const int repoIndex = mirrorNodesRepoIndex();
    return repoIndex >= 0 ? m_repositories.at(repoIndex).owner +
                                QLatin1Char('/') + m_repositories.at(repoIndex).name
                          : QString();
}

QStringList MainWindow::testMirrorNodeRows() const
{
    QStringList rows;
    if (!m_mirrorNodesTable)
        return rows;
    for (int row = 0; row < m_mirrorNodesTable->rowCount(); ++row) {
        const QTableWidgetItem *item =
            m_mirrorNodesTable->item(row, MirrorNodeColNode);
        if (!item)
            continue;
        rows.append(item->text() + QLatin1Char('|') +
                    item->data(Qt::UserRole).toString());
    }
    return rows;
}

bool MainWindow::testMirrorNodesOnlineOnlyChecked() const
{
    return m_mirrorNodesOnlineOnlyCheck && m_mirrorNodesOnlineOnlyCheck->isChecked();
}

void MainWindow::testSetMirrorNodesOnlineOnly(bool checked)
{
    if (m_mirrorNodesOnlineOnlyCheck)
        m_mirrorNodesOnlineOnlyCheck->setChecked(checked);
}

bool MainWindow::testMirrorNodesAreSingleLineRows() const
{
    if (!m_nodesTable || m_nodesTable->horizontalHeader()->isHidden())
        return false;
    int actionsColumn = -1;
    for (int col = 0; col < m_nodesTable->columnCount(); ++col) {
        const QTableWidgetItem *header = m_nodesTable->horizontalHeaderItem(col);
        if (!header || header->icon().isNull() || header->toolTip().isEmpty() ||
            header->text().isEmpty())
            return false;
        if (header->text() == QLatin1String("Actions")) {
            actionsColumn = col;
            continue;
        }
        if (m_nodesTable->isColumnHidden(col))
            return false;
    }
    bool foundNode = false;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        const QTableWidgetItem *name = m_nodesTable->item(row, 0);
        if (!name || name->data(kMirrorNodeIdRole).isNull())
            continue;
        foundNode = true;
        for (int col = 0; col < m_nodesTable->columnCount(); ++col)
            if (col != actionsColumn && m_nodesTable->cellWidget(row, col))
                return false;
        if (m_nodesTable->rowHeight(row) > 30)
            return false;
    }
    return foundNode;
}

bool MainWindow::testMirrorNodeColumnsDraggable() const
{
    if (!m_nodesTable)
        return false;
    QHeaderView *header = m_nodesTable->horizontalHeader();
    if (!header || !header->sectionsMovable())
        return false;
    for (int column = 0; column < m_nodesTable->columnCount(); ++column)
        if (header->sectionResizeMode(column) != QHeaderView::Interactive)
            return false;
    return true;
}

bool MainWindow::testMirrorNodeHeaderHasOptionsButton() const
{
    const auto *header = m_nodesTable
                             ? qobject_cast<const KebabHeaderView *>(
                                   m_nodesTable->horizontalHeader())
                             : nullptr;
    return header && header->optionsButtonWidth() > 0;
}

bool MainWindow::testMirrorNodeHeaderIsCompactAndVisible() const
{
    const auto *header = m_nodesTable
                             ? qobject_cast<const KebabHeaderView *>(
                                   m_nodesTable->horizontalHeader())
                             : nullptr;
    if (!header || !header->compactSections() ||
        header->stackedIconLabels() || !header->isVisible() ||
        header->height() < header->fontMetrics().height())
        return false;
    QStringList captions;
    for (int column = 0; column < m_nodesTable->columnCount(); ++column) {
        const QTableWidgetItem *item = m_nodesTable->horizontalHeaderItem(column);
        if (!item || item->text().trimmed().isEmpty() || item->icon().isNull())
            return false;
        captions.append(item->text());
    }
    return captions.value(0) == QLatin1String("Node") &&
           captions.contains(QLatin1String("Owner"));
}

void MainWindow::testSetSshMirrorMetadata(const QString &source,
                                          const QJsonObject &metadata)
{
    const QString node =
        metadata.value(QStringLiteral("node")).toString().trimmed().toLower();
    if (source.trimmed().isEmpty() || node.isEmpty())
        return;
    m_sshMirrorMetadataCache.insert(source + QLatin1Char('|') + node, metadata);
    loadMirrorNodesPanel();
    testSettleMirrorNodesPanel();
}

QString MainWindow::testMirrorNodeCellText(const QString &nodeName, int column) const
{
    if (!m_mirrorNodesTable)
        return QString();
    for (int row = 0; row < m_mirrorNodesTable->rowCount(); ++row) {
        const QTableWidgetItem *name =
            m_mirrorNodesTable->item(row, MirrorNodeColNode);
        if (!name || !name->text().startsWith(nodeName))
            continue;
        const QTableWidgetItem *item = m_mirrorNodesTable->item(row, column);
        return item ? item->text() : QString();
    }
    return QString();
}

QString MainWindow::testMirrorNodeCellToolTip(const QString &nodeName, int column) const
{
    if (!m_mirrorNodesTable)
        return QString();
    for (int row = 0; row < m_mirrorNodesTable->rowCount(); ++row) {
        const QTableWidgetItem *name =
            m_mirrorNodesTable->item(row, MirrorNodeColNode);
        if (!name || !name->text().startsWith(nodeName))
            continue;
        const QTableWidgetItem *item = m_mirrorNodesTable->item(row, column);
        return item ? item->toolTip() : QString();
    }
    return QString();
}

bool MainWindow::testDraftMirrorNodeDiagnostics(const QString &nodeName)
{
    if (!m_mirrorNodesTable)
        return false;
    for (int row = 0; row < m_mirrorNodesTable->rowCount(); ++row) {
        const QTableWidgetItem *name =
            m_mirrorNodesTable->item(row, MirrorNodeColNode);
        if (!name || !name->text().startsWith(nodeName))
            continue;
        QTableWidgetItem *health =
            m_mirrorNodesTable->item(row, MirrorNodeColHealth);
        if (!health)
            return false;
        const QString expected =
            health->data(kMirrorDiagnosticPromptRole).toString().trimmed();
        draftMirrorNodeDiagnosticsPrompt(health);
        return m_issueQuickAdd && !expected.isEmpty() &&
               m_issueQuickAdd->toPlainText() == expected;
    }
    return false;
}
#endif

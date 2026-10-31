#include "MainWindow.h"

#include "AppPaths.h"
#include "GenieLandmarks.h"
#include "GenieSpiderWidget.h"
#include "GenieVitals.h"
#include "MainWindowInternal.h"
#include "NativePackageExtension.h"
#include "PetHostFeed.h"
#include "SettingsWrite.h"
#include "ForkMeshPackageRegistry.h"
#include "TeammateStore.h"

#include <QCheckBox>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace {

const QString kGenieVitalsSetting = QStringLiteral("genie/vitals");
const QString kGenieActivitySetting = QStringLiteral("genie/activity");
const QString kGeniePlaybookSetting = QStringLiteral("genie/playbook");
const QString kGenieSpotSetting = QStringLiteral("genie/spot");
const QString kGenieWandersSetting = QStringLiteral("genie/wanders");
const QString kGenieParkedSetting = QStringLiteral("genie/parked");
const QString kGenieDesktopSetting = QStringLiteral("genie/desktop");
const QString kGenieDesktopSpotSetting = QStringLiteral("genie/desktopSpot");
const QString kGenieWatchSetting = QStringLiteral("genie/watch");
const QString kGenieLessonsSetting = QStringLiteral("genie/lessons");
const QString kGenieRepliesSetting = QStringLiteral("genie/replies");
const QString kGenieShownSetting = QStringLiteral("genie/shown");
constexpr qint64 kGenieMeasureEveryMs = 30LL * 60 * 1000;
constexpr int kGenieLineMs = 5000;
constexpr int kGenieRepoScan = 12;

QString genieOrganization(const GenieMarketingFacts &facts)
{
    return facts.marketing.organization.isEmpty() ? facts.crm.organization : facts.marketing.organization;
}

QSettings &genieSettings()
{
    static QSettings settings;
    return settings;
}

struct GenieLocalScan {
    int commits = 0;
    bool hasReadme = false;
    bool readmeHasPicture = false;
    QList<GenieShippedChange> shipped;
};

struct GenieRepositoryScan {
    QString path;
    QString name;
};

struct GenieMarketingReads {
    int pending = 2;
    bool overviewOk = false;
    QString error;
    QJsonObject overview;
    QJsonObject performance;
};

void forgetGenieCrm(GenieVitalSigns *signs)
{
    signs->emailsSent = 0;
    signs->contactsTouched = 0;
    signs->replies = 0;
    signs->bounced = 0;
}

bool genieHasNoAudience()
{
#ifdef FORKMESH_WINDOW_TESTS
    return true;
#else
    const QString platform = QGuiApplication::platformName();
    return platform.contains(QStringLiteral("offscreen"), Qt::CaseInsensitive) ||
           platform.contains(QStringLiteral("minimal"), Qt::CaseInsensitive);
#endif
}

QString preferredCrmOrganization()
{
    QFile file(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
               QStringLiteral("/crm/organizations.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
        return QString();
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("organization")).toString();
}

}

void MainWindow::setupGenieSpider()
{
    if (m_genieSpider || !m_globalOverlayHost || genieHasNoAudience() || !genieSpiderShown())
        return;
    QSettings &settings = genieSettings();
    GenieVitalSigns saved;
    if (GenieVitalSigns::decode(settings.value(kGenieVitalsSetting).toStringList(), &saved))
        m_genieSigns = saved;

    m_genieSpider = new GenieSpiderWidget(m_globalOverlayHost);
    m_genieSpider->setHomeFraction(settings.value(kGenieSpotSetting, 0.82).toDouble());
    m_genieSpider->setWanders(settings.value(kGenieWandersSetting, true).toBool());
    m_genieSpider->setParked(settings.value(kGenieParkedSetting, false).toBool());
    m_genieSpider->setEscapes(true);
    m_genieCoach = new GenieCoachPanel(m_globalOverlayHost);
    m_genieTip = new GenieTipBubble(m_globalOverlayHost);
    m_genieSpider->setScene([this] {
        GenieScene scene;
        QWidget *host = m_globalOverlayHost;
        if (m_genieOnDesktop || !host || m_genieSpider->parentWidget() != host || !host->isVisible())
            return scene;
        scene = GenieLandmarks::scan(host, {m_genieSpider.data(), m_genieTip.data(), m_genieCoach.data()});
        scene.quiet = m_genieTip->isVisible() || m_genieCoach->isVisible();
        return scene;
    });

    connect(m_genieSpider, &GenieSpiderWidget::clicked, this, [this] {
        if (m_genieSpider->parked()) {
            m_genieSpider->setParked(false);
            genieSettings().setValue(kGenieParkedSetting, false);
            return;
        }
        showNextGenieTip();
    });
    connect(m_genieSpider, &GenieSpiderWidget::menuRequested, this, &MainWindow::showGenieMenu);
    connect(m_genieSpider, &GenieSpiderWidget::escaped, this, &MainWindow::releaseGenieToDesktop);
    connect(m_genieSpider, &GenieSpiderWidget::moved, this, [this] {
        if (m_genieCoach->isVisible())
            m_genieCoach->place(m_genieSpider->bodyRect());
    });
    connect(m_genieSpider, &GenieSpiderWidget::placed, this, [this] {
        if (!m_genieSpider->parked())
            forkmesh::settings::put(genieSettings(),
                                    m_genieOnDesktop ? kGenieDesktopSpotSetting : kGenieSpotSetting,
                                    m_genieSpider->homeFraction());
    });
    connect(m_genieTip, &GenieTipBubble::clicked, this, &MainWindow::showNextGenieTip);
    connect(m_genieTip, &GenieTipBubble::addToAgent, this, &MainWindow::addGenieTipToAgent);
    connect(m_genieTip, &GenieTipBubble::reply, this, &MainWindow::replyToGenieSpider);
    m_genieTip->setReplies(genieSpiderTakesReplies());
    connect(m_genieCoach, &GenieCoachPanel::closeRequested, this, [this] { m_genieCoach->hide(); });
    connect(m_genieCoach, &GenieCoachPanel::navigate, this, &MainWindow::openGeniePage);
    connect(m_genieCoach, &GenieCoachPanel::refreshRequested, this, [this] { measureGenieVitals(true); });
    connect(m_genieCoach, &GenieCoachPanel::stepTicked, this, [this](const QString &id, bool done) {
        QStringList ticked = genieSettings().value(kGeniePlaybookSetting).toStringList();
        ticked.removeAll(id);
        if (done)
            ticked << id;
        genieSettings().setValue(kGeniePlaybookSetting, ticked);
        if (done && m_genieSpider)
            m_genieSpider->play(GenieReaction::Cheer);
        QTimer::singleShot(0, this, [this] { applyGenieVitals(); });
    });

    m_genieTimer = new QTimer(this);
    m_genieTimer->setInterval(60 * 1000);
    connect(m_genieTimer, &QTimer::timeout, this, [this] { measureGenieVitals(false); });
    m_genieTimer->start();

    installGeniePetHost();
    m_genieSpider->show();
    positionGenieSpider();
    if (settings.value(kGenieDesktopSetting, false).toBool())
        moveGenieToDesktop(false, QPointF(), QPointF());
    applyGenieVitals();
    QTimer::singleShot(8000, this, [this] { measureGenieVitals(true); });
}

void MainWindow::installGeniePetHost()
{
    using namespace forkmesh::petfeed;
    Host host;
    host.sprite = [this](QWidget *in, Sprite *out) {
        GenieSpiderWidget *spider = m_genieSpider;
        if (!spider || !in || m_genieOnDesktop || !spider->isVisible() || spider->window() != in->window())
            return false;
        QWidget *window = in->window();
        const QPoint origin = in->mapFrom(window, spider->mapTo(window, QPoint()));
        out->frame = QRect(origin, spider->size());
        out->body = QRect(origin + QPoint(GenieSpiderWidget::kPad, GenieSpiderWidget::kPad),
                          QSize(GenieSpiderWidget::kSide, GenieSpiderWidget::kSide));
        out->image = spider->frameImage();
        out->walking = spider->walking();
        out->airborne = spider->airborne();
        return true;
    };
    host.setSubmerged = [this](bool on) {
        if (m_genieSpider)
            m_genieSpider->setSubmerged(on);
    };
    host.react = [this](Reaction reaction) {
        if (!m_genieSpider)
            return;
        switch (reaction) {
        case Reaction::Wave:
            m_genieSpider->play(GenieReaction::Wave);
            break;
        case Reaction::Cheer:
            m_genieSpider->play(GenieReaction::Cheer);
            break;
        case Reaction::Nod:
            m_genieSpider->play(GenieReaction::Nod);
            break;
        case Reaction::Eureka:
            m_genieSpider->play(GenieReaction::Eureka);
            break;
        case Reaction::Giggle:
            m_genieSpider->play(GenieReaction::Giggle);
            break;
        case Reaction::Surprise:
            m_genieSpider->play(GenieReaction::Surprise);
            break;
        case Reaction::Dizzy:
            m_genieSpider->play(GenieReaction::Dizzy);
            break;
        case Reaction::Dance:
            m_genieSpider->dance();
            break;
        }
    };
    host.walkTo = [this](QWidget *in, double x) {
        GenieSpiderWidget *spider = m_genieSpider;
        if (!spider || !in || m_genieOnDesktop || spider->parked() || !spider->parentWidget() ||
            spider->window() != in->window())
            return;
        QWidget *window = in->window();
        const QPoint at = spider->parentWidget()->mapFrom(window, in->mapTo(window, QPoint(int(x), 0)));
        spider->walkTo(at.x());
    };
    host.hop = [this](double vx, double vy) {
        if (m_genieSpider && !m_genieOnDesktop && !m_genieSpider->parked())
            m_genieSpider->hop(vx, vy);
    };
    host.lookAt = [this](QWidget *in, const QPointF &at) {
        GenieSpiderWidget *spider = m_genieSpider;
        if (!spider || !in || !spider->parentWidget() || spider->window() != in->window())
            return;
        QWidget *window = in->window();
        const QPoint point =
            spider->parentWidget()->mapFrom(window, in->mapTo(window, at.toPoint()));
        spider->lookAt(point);
    };
    setHost(this, std::move(host));
}

bool MainWindow::genieSpiderShown() const
{
    return genieSettings().value(kGenieShownSetting, true).toBool();
}

void MainWindow::setGenieSpiderShown(bool shown)
{
    genieSettings().setValue(kGenieShownSetting, shown);
    if (m_genieShownCheck) {
        const QSignalBlocker block(m_genieShownCheck);
        m_genieShownCheck->setChecked(shown);
    }
    if (shown) {
        setupGenieSpider();
        return;
    }
    if (m_genieTimer) {
        m_genieTimer->stop();
        m_genieTimer->deleteLater();
        m_genieTimer = nullptr;
    }
    const QList<QPointer<QWidget>> parts = {m_genieTip.data(), m_genieCoach.data(), m_genieSpider.data(),
                                            m_genieStage.data()};
    for (const QPointer<QWidget> &part : parts) {
        if (!part)
            continue;
        part->hide();
        part->deleteLater();
    }
    m_genieTip = nullptr;
    m_genieCoach = nullptr;
    m_genieSpider = nullptr;
    m_genieStage = nullptr;
    m_genieOnDesktop = false;
}

void MainWindow::positionGenieSpider()
{
    if (!m_genieSpider || !m_globalOverlayHost)
        return;
    if (m_genieOnDesktop) {
        if (m_genieStage && m_genieSpider->parentWidget() == m_genieStage)
            m_genieSpider->setArena(m_genieStage->rect().adjusted(4, 4, -4, -2), {});
        if (m_genieCoach && m_genieCoach->isVisible())
            m_genieCoach->place(m_genieSpider->bodyRect());
        return;
    }
    QWidget *host = m_globalOverlayHost;
    QList<QRect> avoid;
    if (m_footerDock && m_footerDock->isVisible() && m_footerDock->parentWidget() == host) {
        const QRegion mask = m_footerDock->mask();
        if (mask.isEmpty())
            avoid << m_footerDock->geometry();
        for (const QRect &rect : mask)
            avoid << rect.translated(m_footerDock->pos());
    }
    if (m_promptOverlayHost && m_promptOverlayHost->isVisible() && m_promptOverlayFloating &&
        host->isAncestorOf(m_promptOverlayHost))
        avoid << QRect(m_promptOverlayHost->mapTo(host, QPoint()), m_promptOverlayHost->size());
    m_genieSpider->setArena(host->rect().adjusted(8, 8, -8, -4), avoid);
    m_genieSpider->rescanSoon();
    m_genieSpider->raise();
    if (m_genieCoach && m_genieCoach->isVisible()) {
        m_genieCoach->place(m_genieSpider->bodyRect());
        m_genieCoach->raise();
    }
    if (m_genieTip && m_genieTip->isVisible())
        m_genieTip->raise();
}

GenieMarketingFacts MainWindow::genieFacts() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QStringList log = genieSettings().value(kGenieActivitySetting).toStringList();
    const GenieWatch watch = genieWatch();
    GenieMarketingFacts facts;
    facts.nowMs = now;
    facts.vitals = GenieVitals::measure(m_genieSigns, now, watch);
    if (m_genieCrmKnown && watch.has(QStringLiteral("crm"))) {
        facts.contacts = m_genieCrm.contacts;
        facts.emailsSentEver = m_genieCrm.emailsSentEver;
    }
    if (watch.has(QStringLiteral("crm")))
        facts.crm = m_genieCrmDigest;
    if (watch.has(QStringLiteral("marketing"))) {
        facts.marketing = m_genieMarketingDigest;
        facts.shippedKnown = m_genieShippedKnown;
        facts.shipped = m_genieShipped;
    }
    facts.analyticsVisits =
        GenieActivityLog::visits(log, now - GenieVitals::windowMs, QStringLiteral("analytics"));
    if (watch.has(QStringLiteral("readme")) || watch.has(QStringLiteral("playbook"))) {
        facts.hasReadme = m_genieReadme;
        facts.readmeHasPicture = m_genieReadmePicture;
        facts.readmeRepository = m_genieReadmeRepository;
    }
    const QStringList ticked = genieSettings().value(kGeniePlaybookSetting).toStringList();
    facts.ticked = QSet<QString>(ticked.cbegin(), ticked.cend());
    return facts;
}

void MainWindow::applyGenieVitals()
{
    if (!m_genieSpider)
        return;
    const GenieMarketingFacts facts = genieFacts();
    const GenieVitals &vitals = facts.vitals;
    m_genieSpider->setVitals(vitals);
    m_genieSpider->setMood(m_genieMeasuring > 0 && m_genieCoach->isVisible() ? GenieMood::Thinking
                                                                             : GenieMood::Idle);
    m_genieSpider->setToolTip(vitals.summary());
    const int level = int(vitals.level());
    if (m_genieLevel >= 0 && level != m_genieLevel)
        m_genieSpider->play(level < m_genieLevel ? GenieReaction::Cheer : GenieReaction::Surprise);
    m_genieLevel = level;
    if (m_genieCoach->isVisible())
        m_genieCoach->setCoaching(vitals, GenieMarketingCoach::coach(facts), m_genieMeasuring > 0);
}

void MainWindow::measureGenieVitals(bool force)
{
    if (!m_genieSpider || m_genieMeasuring > 0)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - m_genieMeasuredMs < kGenieMeasureEveryMs)
        return;
    m_genieMeasuredMs = now;
    m_genieRemeasure = false;
    const qint64 since = now - GenieVitals::windowMs;
    const GenieWatch watch = genieWatch();
    const bool wantCommits = watch.has(QStringLiteral("code"));
    const bool wantReadme = watch.has(QStringLiteral("readme")) || watch.has(QStringLiteral("playbook"));
    const bool wantShipped = watch.has(QStringLiteral("marketing"));

    GenieVitalSigns signs = m_genieSigns;
    signs.measuredAtMs = now;
    signs.agentRuns = 0;
    signs.merged = 0;
    for (const AgentSession &session : std::as_const(m_agentSessions)) {
        if (session.createdAtMs < since)
            continue;
        ++signs.agentRuns;
        if (session.merged)
            ++signs.merged;
    }
    signs.marketingVisits =
        GenieActivityLog::visits(genieSettings().value(kGenieActivitySetting).toStringList(), since);
    m_genieSigns = signs;

    QList<GenieRepositoryScan> repos;
    QString readmeDir;
    QString readmeName;
    for (int i = 0; i < m_repositories.size() && repos.size() < kGenieRepoScan; ++i) {
        const QString path = writableRecordFor(m_repositories.at(i)).localPath;
        if (path.isEmpty() || !QFileInfo::exists(path + QStringLiteral("/.git")))
            continue;
        const QString name = m_repositories.at(i).name.isEmpty() ? QFileInfo(path).fileName()
                                                                 : m_repositories.at(i).name;
        repos.append({path, name});
        if (i == m_repoDetailIndex || readmeDir.isEmpty()) {
            readmeDir = path;
            readmeName = name;
        }
    }
    if (!wantReadme)
        readmeDir.clear();
    ++m_genieMeasuring;
    runOffThread<GenieLocalScan>(
        [repos, readmeDir, wantCommits, wantShipped] {
            GenieLocalScan scan;
            for (const GenieRepositoryScan &repo : repos) {
                QByteArray out;
                QString err;
                const QStringList args = {QStringLiteral("rev-list"), QStringLiteral("--count"),
                                          QStringLiteral("--no-merges"), QStringLiteral("--since=7.days"),
                                          QStringLiteral("HEAD")};
                if (wantCommits && forkmesh::ui::runGitCapture(repo.path, args, &out, &err))
                    scan.commits += out.trimmed().toInt();
                const QStringList log = {QStringLiteral("log"), QStringLiteral("--no-merges"),
                                         QStringLiteral("--since=14.days"), QStringLiteral("--format=%ct%x1f%s"),
                                         QStringLiteral("-n"), QStringLiteral("300"), QStringLiteral("HEAD")};
                out.clear();
                if (wantShipped && forkmesh::ui::runGitCapture(repo.path, log, &out, &err))
                    scan.shipped += genieShippedChanges(out, repo.name);
            }
            std::stable_sort(scan.shipped.begin(), scan.shipped.end(),
                             [](const GenieShippedChange &a, const GenieShippedChange &b) { return a.atMs > b.atMs; });
            if (!readmeDir.isEmpty()) {
                const QStringList names = QDir(readmeDir).entryList(
                    {QStringLiteral("README*"), QStringLiteral("readme*"), QStringLiteral("Readme*")}, QDir::Files);
                if (!names.isEmpty()) {
                    QFile file(readmeDir + QLatin1Char('/') + names.constFirst());
                    if (file.open(QIODevice::ReadOnly)) {
                        scan.hasReadme = true;
                        scan.readmeHasPicture = genieReadmeHasPicture(QString::fromUtf8(file.read(512 * 1024)));
                    }
                }
            }
            return scan;
        },
        [this, readmeName, wantShipped](GenieLocalScan scan) {
            m_genieSigns.commits = scan.commits;
            m_genieReadme = scan.hasReadme;
            m_genieReadmePicture = scan.readmeHasPicture;
            m_genieReadmeRepository = scan.hasReadme ? readmeName : QString();
            m_genieShipped = scan.shipped;
            m_genieShippedKnown = wantShipped;
            finishGenieMeasure();
        });
    if (watch.has(QStringLiteral("crm")) || !genieSettings().contains(kGenieWatchSetting))
        fetchGenieCrm(since);
    if (watch.has(QStringLiteral("marketing")))
        fetchGenieMarketing(since);
    applyGenieVitals();
}

void MainWindow::fetchGenieCrm(qint64 since)
{
    const NativePackagePage *crm = nullptr;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        if (page.id == QLatin1String("crm") && page.widget)
            crm = &page;
    const forkmesh::market::PackageManifest *manifest =
        crm && m_packageRegistry ? m_packageRegistry->package(crm->id) : nullptr;
    const forkmesh::market::NativePackageContribution *found =
        manifest ? forkmesh::market::nativePackageContribution(crm->entrypoint, manifest->type) : nullptr;
    if (!found) {
        m_genieCrmDigest = GenieCrmDigest();
        m_genieCrmDigest.source = GenieSource::Missing;
        m_genieCrmKnown = false;
        forgetGenieCrm(&m_genieSigns);
        return;
    }
    const forkmesh::market::NativePackageContribution contribution = *found;
    ++m_genieMeasuring;
    requestNativePackage(
        QStringLiteral("crm"), contribution, QByteArrayLiteral("GET"), QStringLiteral("/api/crm"), {},
        [this, contribution, since](bool ok, const QJsonObject &payload, const QString &error) {
            QString organization;
            if (!ok) {
                m_genieCrmDigest = GenieCrmDigest();
                m_genieCrmDigest.source = GenieSource::Failed;
                m_genieCrmDigest.error = error;
            } else {
                noteGenieOrganizations(payload.value(QStringLiteral("organizations")).toArray());
                const QString preferred = preferredCrmOrganization();
                for (const QJsonValue &value : payload.value(QStringLiteral("organizations")).toArray()) {
                    const QJsonObject item = value.toObject();
                    const QString name = item.value(QStringLiteral("name")).toString();
                    if (!item.value(QStringLiteral("installed")).toBool() || name.isEmpty())
                        continue;
                    if (organization.isEmpty() || name == preferred)
                        organization = name;
                }
            }
            if (organization.isEmpty()) {
                if (ok) {
                    m_genieCrmDigest = GenieCrmDigest();
                    m_genieCrmDigest.source = GenieSource::Missing;
                    m_genieCrmKnown = false;
                    forgetGenieCrm(&m_genieSigns);
                }
                finishGenieMeasure();
                return;
            }
            const QString path = QStringLiteral("/api/crm/%1?tz=%2")
                                     .arg(organization)
                                     .arg(QDateTime::currentDateTime().offsetFromUtc() / 60);
            requestNativePackage(
                QStringLiteral("crm"), contribution, QByteArrayLiteral("GET"), path, {},
                [this, since](bool loaded, const QJsonObject &summary, const QString &failure) {
                    if (loaded) {
                        m_genieCrm = GenieCrmCounts::read(summary, since);
                        m_genieCrmKnown = true;
                        m_genieCrmDigest = GenieCrmDigest::read(summary, QDateTime::currentMSecsSinceEpoch(),
                                                                genieSpiderLessons());
                        m_genieSigns.emailsSent = m_genieCrm.emailsSent;
                        m_genieSigns.contactsTouched = m_genieCrm.contactsTouched;
                        m_genieSigns.replies = m_genieCrm.replies;
                        m_genieSigns.bounced = m_genieCrm.bounced;
                    } else {
                        m_genieCrmDigest = GenieCrmDigest();
                        m_genieCrmDigest.source = GenieSource::Failed;
                        m_genieCrmDigest.error = failure;
                    }
                    finishGenieMeasure();
                });
        });
}

void MainWindow::fetchGenieMarketing(qint64 since)
{
    const NativePackagePage *marketing = nullptr;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        if (page.id == QLatin1String("marketing") && page.widget)
            marketing = &page;
    const forkmesh::market::PackageManifest *manifest =
        marketing && m_packageRegistry ? m_packageRegistry->package(marketing->id) : nullptr;
    const forkmesh::market::NativePackageContribution *found =
        manifest ? forkmesh::market::nativePackageContribution(marketing->entrypoint, manifest->type) : nullptr;
    if (!found) {
        m_genieMarketingDigest = GenieMarketingDigest();
        m_genieMarketingDigest.source = GenieSource::Missing;
        m_genieSigns.published = 0;
        return;
    }
    const forkmesh::market::NativePackageContribution contribution = *found;
    const auto failed = [this](const QString &error) {
        m_genieMarketingDigest = GenieMarketingDigest();
        m_genieMarketingDigest.source = GenieSource::Failed;
        m_genieMarketingDigest.error = error;
    };
    ++m_genieMeasuring;
    requestNativePackage(
        QStringLiteral("marketing"), contribution, QByteArrayLiteral("GET"), QStringLiteral("/api/marketing"), {},
        [this, contribution, since, failed](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok) {
                failed(error);
                finishGenieMeasure();
                return;
            }
            const QJsonArray organizations = payload.value(QStringLiteral("organizations")).toArray();
            noteGenieOrganizations(organizations);
            const QString preferred = m_genieCrmDigest.organization.isEmpty() ? preferredCrmOrganization()
                                                                               : m_genieCrmDigest.organization;
            QString organization;
            for (const QJsonValue &value : organizations) {
                const QJsonObject item = value.toObject();
                const QString name = item.value(QStringLiteral("name")).toString();
                if (!item.value(QStringLiteral("installed")).toBool() || name.isEmpty())
                    continue;
                if (organization.isEmpty() || name == preferred)
                    organization = name;
            }
            if (organization.isEmpty()) {
                m_genieMarketingDigest = GenieMarketingDigest();
                m_genieMarketingDigest.source = GenieSource::Missing;
                m_genieSigns.published = 0;
                finishGenieMeasure();
                return;
            }
            auto reads = std::make_shared<GenieMarketingReads>();
            const auto done = [this, reads, since, failed] {
                if (--reads->pending > 0)
                    return;
                if (reads->overviewOk) {
                    m_genieMarketingDigest = GenieMarketingDigest::read(
                        reads->overview, reads->performance, QDateTime::currentMSecsSinceEpoch(), since);
                    m_genieSigns.published = m_genieMarketingDigest.published;
                } else {
                    failed(reads->error);
                }
                finishGenieMeasure();
            };
            const QString base = QStringLiteral("/api/marketing/") + organization;
            requestNativePackage(QStringLiteral("marketing"), contribution, QByteArrayLiteral("GET"), base, {},
                                 [reads, done](bool loaded, const QJsonObject &overview, const QString &failure) {
                                     reads->overviewOk = loaded;
                                     reads->overview = overview;
                                     reads->error = failure;
                                     done();
                                 });
            requestNativePackage(QStringLiteral("marketing"), contribution, QByteArrayLiteral("GET"),
                                 base + QStringLiteral("/performance"), {},
                                 [reads, done](bool loaded, const QJsonObject &performance, const QString &) {
                                     if (loaded)
                                         reads->performance = performance;
                                     done();
                                 });
        });
}

GenieWatch MainWindow::genieWatch() const
{
    QSettings &settings = genieSettings();
    if (!settings.contains(kGenieWatchSetting))
        return GenieWatch();
    return GenieWatch::decode(settings.value(kGenieWatchSetting).toString());
}

void MainWindow::setGenieWatch(const GenieWatch &watch)
{
    genieSettings().setValue(kGenieWatchSetting, watch.encoded());
    for (const QPointer<QCheckBox> &box : std::as_const(m_genieWatchChecks)) {
        if (!box)
            continue;
        const QSignalBlocker block(box);
        box->setChecked(watch.has(box->property("genieTopic").toString()));
    }
    if (!m_genieSpider)
        return;
    applyGenieVitals();
    if (m_genieMeasuring > 0)
        m_genieRemeasure = true;
    else
        measureGenieVitals(true);
}

void MainWindow::noteGenieOrganizations(const QJsonArray &organizations)
{
    if (genieSettings().contains(kGenieWatchSetting))
        return;
    QStringList names;
    for (const QJsonValue &value : organizations)
        names << value.toObject().value(QStringLiteral("name")).toString();
    const GenieWatch preset = GenieWatch::forOrganizations(names);
    if (!(preset == GenieWatch()))
        setGenieWatch(preset);
}

QWidget *MainWindow::buildGenieWatchSettings()
{
    auto *box = new QWidget;
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto *shown = new QCheckBox(QStringLiteral("Show the Genie spider"));
    shown->setObjectName(QStringLiteral("genieShown"));
    shown->setToolTip(QStringLiteral(
        "Untick to make the spider disappear completely. Hide the spider in its right-click or long-press menu "
        "does the same."));
    shown->setChecked(genieSpiderShown());
    connect(shown, &QCheckBox::toggled, this, &MainWindow::setGenieSpiderShown);
    m_genieShownCheck = shown;
    layout->addWidget(shown);
    auto *hint = new QLabel(QStringLiteral(
        "What the Genie spider watches. Its health, its tips and its coach only use what is ticked here, "
        "and every tip comes from something it read; when it cannot read a source it says so instead of "
        "guessing. Members of the ForkMesh organization start with only CRM and Marketing ticked."));
    hint->setObjectName(QStringLiteral("statusLine"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    const GenieWatch watch = genieWatch();
    m_genieWatchChecks.removeAll(nullptr);
    for (const QString &topic : GenieWatch::all()) {
        auto *check = new QCheckBox(GenieWatch::label(topic));
        check->setObjectName(QStringLiteral("genieWatch_") + topic);
        check->setProperty("genieTopic", topic);
        check->setToolTip(GenieWatch::detail(topic));
        check->setChecked(watch.has(topic));
        connect(check, &QCheckBox::toggled, this, [this, topic](bool on) {
            GenieWatch next = genieWatch();
            if (on)
                next.topics.insert(topic);
            else
                next.topics.remove(topic);
            setGenieWatch(next);
        });
        m_genieWatchChecks.append(check);
        layout->addWidget(check);
    }
    auto *replies = new QCheckBox(QStringLiteral("Let me reply to the spider to teach it"));
    replies->setObjectName(QStringLiteral("genieReplies"));
    replies->setToolTip(QStringLiteral(
        "Adds Reply to the spider's tips. A reply goes to the Genie special agent, which runs on the low-cost "
        "model of your agent (Haiku 4.5 or GPT-5.6 Luna), checks the CRM and teaches the spider whom to leave out."));
    replies->setChecked(genieSpiderTakesReplies());
    connect(replies, &QCheckBox::toggled, this, [this](bool on) {
        genieSettings().setValue(kGenieRepliesSetting, on);
        if (m_genieTip)
            m_genieTip->setReplies(on);
    });
    layout->addWidget(replies);
    return box;
}

void MainWindow::finishGenieMeasure()
{
    m_genieMeasuring = qMax(0, m_genieMeasuring - 1);
    if (m_genieMeasuring == 0)
        genieSettings().setValue(kGenieVitalsSetting, m_genieSigns.encoded());
    applyGenieVitals();
    if (m_genieMeasuring == 0 && m_genieRemeasure)
        QTimer::singleShot(0, this, [this] { measureGenieVitals(true); });
}

void MainWindow::noteGeniePage()
{
    if (!m_genieSpider || !m_sectionStack || !m_sectionStack->currentWidget())
        return;
    const QString id = m_sectionStack->currentWidget()->property("forkmeshPackageId").toString();
    m_genieSpider->calm();
    m_genieSpider->rescanSoon();
    if (GenieVitals::isMarketingPage(id))
        recordGenieVisit(id);
    else
        m_genieSpider->play(GenieReaction::Nod);
}

void MainWindow::recordGenieVisit(const QString &page)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSettings &settings = genieSettings();
    const QStringList before = settings.value(kGenieActivitySetting).toStringList();
    const QStringList after = GenieActivityLog::record(before, page, now);
    if (after == before)
        return;
    settings.setValue(kGenieActivitySetting, after);
    if (GenieVitals::isMarketingPage(page)) {
        m_genieSigns.marketingVisits = GenieActivityLog::visits(after, now - GenieVitals::windowMs);
        if (m_genieSpider)
            m_genieSpider->play(GenieReaction::Eureka);
    }
    applyGenieVitals();
}

void MainWindow::openGeniePage(const QString &page)
{
    const QString target = page == QLatin1String("ads") ? QStringLiteral("marketing") : page;
    if (m_genieOnDesktop && target != QLatin1String("analytics") && target != QLatin1String("competitors"))
        raiseForGenie();
    if (target == QLatin1String("crm") || target == QLatin1String("marketing")) {
        bool installed = false;
        for (const NativePackagePage &entry : std::as_const(m_nativePackagePages))
            installed = installed || (entry.id == target && entry.widget);
        if (!installed) {
            showSection(kMarketSectionIndex);
            flashMessage(QStringLiteral("Install the %1 plugin from the Market and the Genie can coach you there.")
                             .arg(target == QLatin1String("crm") ? QStringLiteral("CRM") : QStringLiteral("Marketing")));
            return;
        }
        showNativePackage(target);
        recordGenieVisit(target);
    } else if (target == QLatin1String("analytics") || target == QLatin1String("competitors")) {
        QUrl url = catalogApiUrl();
        url.setUserName(QString());
        url.setPassword(QString());
        url.setPath(QStringLiteral("/dashboard/") + target);
        url.setQuery(QString());
        QDesktopServices::openUrl(url);
        recordGenieVisit(target);
    } else if (target == QLatin1String("agents")) {
        openRepoAgentsList();
        if (m_issueQuickAdd)
            QTimer::singleShot(0, m_issueQuickAdd, [edit = m_issueQuickAdd] { edit->setFocus(); });
    } else if (target == QLatin1String("settings")) {
        if (m_genieOnDesktop)
            raiseForGenie();
        showSection(1);
    } else if (target == QLatin1String("repositories")) {
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size())
            openRepoReadme();
        else
            showSection(0);
    }
}

void MainWindow::toggleGenieCoach()
{
    if (!m_genieCoach || !m_genieSpider)
        return;
    if (m_genieCoach->isVisible()) {
        m_genieCoach->hide();
        return;
    }
    if (m_genieTip)
        m_genieTip->hide();
    m_genieSpider->calm();
    const GenieMarketingFacts facts = genieFacts();
    m_genieCoach->setCoaching(facts.vitals, GenieMarketingCoach::coach(facts), m_genieMeasuring > 0);
    m_genieCoach->show();
    m_genieCoach->place(m_genieSpider->bodyRect());
    measureGenieVitals(false);
}

void MainWindow::showGenieMenu(const QPoint &globalPos)
{
    if (!m_genieSpider)
        return;
    QMenu menu(m_genieSpider->window());
    menu.addAction(QStringLiteral("How are you feeling?"), this, [this] {
        const GenieMarketingFacts facts = genieFacts();
        const GenieCoaching coaching = GenieMarketingCoach::coach(facts);
        m_genieSpider->calm();
        m_genieTip->showTip(facts.vitals.summary(genieOrganization(facts)), m_genieSpider->bodyRect(), 10000,
                            coaching.prompt, coaching.task);
        recordGenieSaid(QLatin1String(GenieSaidLog::kindFeeling), facts.vitals.summary(genieOrganization(facts)),
                        coaching.prompt.isEmpty() ? QString() : coaching.task, genieBasis(facts));
    });
    menu.addAction(QStringLiteral("Marketing coach"), this, [this] {
        if (!m_genieCoach->isVisible())
            toggleGenieCoach();
    });
    menu.addAction(QStringLiteral("Measure again"), this, [this] { measureGenieVitals(true); });
    if (genieSpiderTakesReplies())
        menu.addAction(QStringLiteral("Reply to the spider\u2026"), this,
                       [this] { replyToGenieSpider(m_genieLastTip); });
    QMenu *watching = menu.addMenu(QStringLiteral("Watch"));
    const GenieWatch watch = genieWatch();
    for (const QString &topic : GenieWatch::all()) {
        QAction *item = watching->addAction(GenieWatch::label(topic));
        item->setCheckable(true);
        item->setChecked(watch.has(topic));
        item->setToolTip(GenieWatch::detail(topic));
        connect(item, &QAction::toggled, this, [this, topic](bool on) {
            GenieWatch next = genieWatch();
            if (on)
                next.topics.insert(topic);
            else
                next.topics.remove(topic);
            setGenieWatch(next);
        });
    }
    watching->setToolTipsVisible(true);
    menu.addSeparator();
    if (m_genieOnDesktop)
        menu.addAction(QStringLiteral("Come back inside"), this, [this] { bringGenieInside(); });
    else
        menu.addAction(QStringLiteral("Let out onto the desktop"), this,
                       [this] { moveGenieToDesktop(false, QPointF(), QPointF()); });
    QAction *still = menu.addAction(QStringLiteral("Stay put"));
    still->setCheckable(true);
    still->setChecked(!m_genieSpider->wanders());
    connect(still, &QAction::toggled, this, [this](bool on) {
        m_genieSpider->setWanders(!on);
        genieSettings().setValue(kGenieWandersSetting, !on);
    });
    const bool parked = m_genieSpider->parked();
    menu.addAction(parked ? QStringLiteral("Come back") : QStringLiteral("Tuck away"), this, [this, parked] {
        m_genieCoach->hide();
        m_genieTip->hide();
        m_genieSpider->setParked(!parked);
        genieSettings().setValue(kGenieParkedSetting, !parked);
    });
    menu.addSeparator();
    QAction *hide = menu.addAction(QStringLiteral("Hide the spider"));
    hide->setToolTip(QStringLiteral("Bring it back in Settings, General, Genie spider."));
    menu.setToolTipsVisible(true);
    if (menu.exec(globalPos) == hide)
        setGenieSpiderShown(false);
}

void MainWindow::danceGenie()
{
    if (m_genieSpider)
        m_genieSpider->dance();
}

void MainWindow::showNextGenieTip()
{
    if (!m_genieSpider || !m_genieTip)
        return;
    const GenieMarketingFacts facts = genieFacts();
    const QStringList lines = GenieMarketingCoach::coach(facts).lines();
    if (lines.isEmpty())
        return;
    const int index = m_genieTipIndex % lines.size();
    m_genieTipIndex = index + 1;
    if (m_genieCoach)
        m_genieCoach->hide();
    const QString &line = lines.at(index);
    m_genieLastTip = line;
    m_genieSpider->calm();
    m_genieTip->say(line, m_genieSpider->bodyRect(), kGenieLineMs);
    recordGenieSaid(QLatin1String(GenieSaidLog::kindClick), line, QString(), genieBasis(facts));
    measureGenieVitals(false);
}

void MainWindow::cheerGenieFor(const QJsonObject &alert)
{
    const QString line = GeniePings::badgeLine(alert);
    if (line.isEmpty() || !m_genieSpider || !m_genieTip || m_genieSpider->parked())
        return;
    if (m_genieCoach)
        m_genieCoach->hide();
    m_genieSpider->play(GenieReaction::Cheer);
    m_genieTip->say(line, m_genieSpider->bodyRect(), kGenieLineMs);
    recordGenieSaid(QLatin1String(GenieSaidLog::kindUnasked), line, QString(),
                    alert.value(QStringLiteral("title")).toString().trimmed());
}

GenieDesktopStage *MainWindow::genieStage()
{
    if (!m_genieStage) {
        m_genieStage = new GenieDesktopStage;
        connect(m_genieStage, &GenieDesktopStage::arenaChanged, this, &MainWindow::positionGenieSpider);
    }
    return m_genieStage;
}

void MainWindow::releaseGenieToDesktop(const QPoint &grab)
{
    if (!m_genieSpider || m_genieOnDesktop)
        return;
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    GenieDesktopStage *stage = genieStage();
    stage->setCatching(true);
    stage->open(screen ? screen : this->screen());
    m_genieTip->hide();
    m_genieCoach->hide();
    auto *mime = new QMimeData;
    mime->setData(QLatin1String(GenieDesktopStage::kMime), QByteArrayLiteral("genie"));
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(m_genieSpider->snapshot(devicePixelRatioF()));
    drag->setHotSpot(grab);
    m_genieSpider->hide();
    drag->exec(Qt::MoveAction);
    if (!m_genieSpider || !m_genieStage)
        return;
    m_genieStage->setCatching(false);
    QPointF at;
    QPointF velocity;
    if (m_genieStage->takeCatch(&at, &velocity)) {
        const QPointF foot = at - QPointF(grab) +
                             QPointF(GenieSpiderWidget::kSide / 2.0, GenieSpiderWidget::kSide - 3);
        moveGenieToDesktop(true, foot, velocity);
        return;
    }
    m_genieStage->hide();
    m_genieSpider->show();
    positionGenieSpider();
}

void MainWindow::moveGenieToDesktop(bool thrown, const QPointF &foot, const QPointF &velocity)
{
    if (!m_genieSpider)
        return;
    GenieDesktopStage *stage = genieStage();
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    stage->open(screen ? screen : this->screen());
    m_genieOnDesktop = true;
    genieSettings().setValue(kGenieDesktopSetting, true);
    m_genieTip->hide();
    m_genieCoach->hide();
    m_genieSpider->setParent(stage);
    m_genieTip->setParent(stage);
    m_genieCoach->setParent(stage);
    m_genieSpider->setEscapes(false);
    m_genieSpider->setRoams(true);
    positionGenieSpider();
    if (thrown) {
        m_genieSpider->throwFrom(foot, velocity);
    } else {
        const QRect arena = stage->rect();
        const double fraction = genieSettings().value(kGenieDesktopSpotSetting, 0.82).toDouble();
        m_genieSpider->setHomeFraction(fraction);
        m_genieSpider->throwFrom(
            QPointF(arena.left() + fraction * arena.width(), arena.top() + GenieSpiderWidget::kSide), QPointF());
    }
    m_genieSpider->show();
    m_genieSpider->play(GenieReaction::Cheer);
}

void MainWindow::bringGenieInside()
{
    if (!m_genieSpider || !m_genieOnDesktop || !m_globalOverlayHost)
        return;
    m_genieOnDesktop = false;
    genieSettings().setValue(kGenieDesktopSetting, false);
    m_genieTip->hide();
    m_genieCoach->hide();
    m_genieSpider->setParent(m_globalOverlayHost);
    m_genieTip->setParent(m_globalOverlayHost);
    m_genieCoach->setParent(m_globalOverlayHost);
    m_genieSpider->setRoams(false);
    m_genieSpider->setEscapes(true);
    positionGenieSpider();
    const QRect host = m_globalOverlayHost->rect();
    const double fraction = genieSettings().value(kGenieSpotSetting, 0.82).toDouble();
    m_genieSpider->setHomeFraction(fraction);
    m_genieSpider->throwFrom(QPointF(host.left() + fraction * host.width(), host.top() + GenieSpiderWidget::kSide),
                             QPointF());
    m_genieSpider->show();
    positionGenieSpider();
    if (m_genieStage)
        m_genieStage->hide();
    raiseForGenie();
    m_genieSpider->play(GenieReaction::Wave);
}

void MainWindow::raiseForGenie()
{
    if (isMinimized())
        showNormal();
    else if (!isVisible())
        show();
    raise();
    activateWindow();
}

void MainWindow::addGenieTipToAgent(const QString &prompt)
{
    const QString text = prompt.trimmed();
    if (!m_issueQuickAdd || text.isEmpty())
        return;
    if (m_genieOnDesktop)
        raiseForGenie();
    if (m_promptOverlayCollapsed)
        setPromptOverlayCollapsed(false);
    recordQuickAddHistory(m_issueQuickAdd->toPlainText());
    m_issueQuickAdd->setPlainText(QStringLiteral("%1\n\n").arg(text));
    m_issueQuickAdd->moveCursor(QTextCursor::End);
    m_issueQuickAdd->setFocus();
}

GenieLessons MainWindow::genieSpiderLessons() const
{
    return GenieLessons::decode(genieSettings().value(kGenieLessonsSetting).toString());
}

bool MainWindow::genieSpiderTakesReplies() const
{
    return genieSettings().value(kGenieRepliesSetting, true).toBool();
}

int MainWindow::genieSpiderTeammateRepo() const
{
    const QString id = QLatin1String(GenieLessons::teammateId);
    const auto hasGenie = [this, &id](int repoIndex) {
        const QString directory = teammatesDirPath(repoIndex);
        return !directory.isEmpty() && QFileInfo::exists(forkmesh::teammates::Store(directory).pathFor(id));
    };
    const int active = activeTeammateRepoIndex();
    if (active >= 0 && hasGenie(active))
        return active;
    for (int repoIndex : teammateRepoChoices())
        if (hasGenie(repoIndex))
            return repoIndex;
    return -1;
}

void MainWindow::replyToGenieSpider(const QString &tip)
{
    if (m_genieTip)
        m_genieTip->hide();
    if (m_genieOnDesktop)
        raiseForGenie();
    const int repoIndex = genieSpiderTeammateRepo();
    if (repoIndex < 0) {
        flashMessage(QStringLiteral("Replies go to the Genie special agent, and no open repository has one. Add "
                                    ".forkmesh/workflows/genie.json from the ForkMesh repository to one."),
                     true);
        return;
    }
    setActiveTeammateRepoIndex(repoIndex);
    static const QRegularExpression counter(QStringLiteral("\\s*Tip \\d+ of \\d+\\s*$"));
    QString quote = tip;
    quote.remove(counter);
    quote = quote.simplified().left(600);
    QString saidId;
    for (const GenieSaid &entry : genieSaidLog().said) {
        if (entry.text.simplified().left(600) == quote) {
            saidId = entry.id;
            break;
        }
    }
    const QString id = QLatin1String(GenieLessons::teammateId);
    m_teammatePromptDrafts.insert(
        id, quote.isEmpty()      ? QString()
            : saidId.isEmpty()   ? QStringLiteral("About your tip \u201C%1\u201D: ").arg(quote)
                                 : QStringLiteral("About your tip \u201C%1\u201D (message %2): ").arg(quote, saidId));
    openTeammateHome(id);
    QTimer::singleShot(50, this, [this] {
        if (!m_teammateHomeComposer)
            return;
        m_teammateHomeComposer->setFocus();
        m_teammateHomeComposer->moveCursor(QTextCursor::End);
    });
}

QString MainWindow::genieSpiderBriefing(const QString &teammateId) const
{
    if (teammateId != QLatin1String(GenieLessons::teammateId))
        return QString();
    const GenieMarketingFacts facts = genieFacts();
    return genieAgentBriefing(facts.vitals.summary(genieOrganization(facts)), GenieMarketingCoach::coach(facts).tips(),
                              genieSpiderLessons(), genieSaidLog(), facts.crm.closingStages);
}

void MainWindow::learnFromGenieSpiderRun(const QString &teammateId, const QString &summary)
{
    if (teammateId != QLatin1String(GenieLessons::teammateId) || summary.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    GenieLessons lessons = genieSpiderLessons();
    const QStringList taught = lessons.learn(summary, now);
    genieSaidLog();
    const QStringList checked = m_genieSaid.learn(summary, now);
    if (!checked.isEmpty())
        saveGenieSaidLog();
    if (taught.isEmpty() && checked.isEmpty())
        return;
    if (!taught.isEmpty())
        genieSettings().setValue(kGenieLessonsSetting, lessons.encoded());
    const QString learned =
        QStringLiteral("The Genie spider learned to %1.").arg((taught + checked).join(QStringLiteral("; ")));
    flashMessage(learned);
    recordGenieSaid(QLatin1String(GenieSaidLog::kindLearned), learned, QString(),
                    QStringLiteral("The closing summary of a Genie special agent run."));
    if (taught.isEmpty() || !m_genieSpider)
        return;
    m_genieSpider->play(GenieReaction::Nod);
    m_genieTipIndex = 0;
    if (m_genieMeasuring > 0)
        m_genieRemeasure = true;
    else
        measureGenieVitals(true);
}

QString MainWindow::genieSaidPath() const
{
    return QDir(forkmesh::paths::dataRoot()).filePath(QStringLiteral("genie/said.json"));
}

const GenieSaidLog &MainWindow::genieSaidLog() const
{
    if (!m_genieSaidLoaded) {
        m_genieSaidLoaded = true;
        QFile file(genieSaidPath());
        if (file.open(QIODevice::ReadOnly) && file.size() < 16 * 1024 * 1024)
            m_genieSaid = GenieSaidLog::decode(file.readAll());
    }
    return m_genieSaid;
}

bool MainWindow::saveGenieSaidLog()
{
    const QString path = genieSaidPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(genieSaidLog().encoded());
    return file.commit();
}

QString MainWindow::recordGenieSaid(const QString &kind, const QString &text, const QString &task,
                                    const QString &basis)
{
    genieSaidLog();
    const QString id = m_genieSaid.record(kind, text, task, basis, QDateTime::currentMSecsSinceEpoch());
    if (id.isEmpty())
        return id;
    m_genieLastSaidId = id;
    saveGenieSaidLog();
    if (m_teammateHomeId == QLatin1String(GenieLessons::teammateId) && teammateHomeVisible())
        fillTeammateHomeSaid();
    return id;
}

void MainWindow::commentOnGenieSaid(const QString &id)
{
    const GenieSaid *entry = genieSaidLog().find(id);
    if (!entry)
        return;
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, QStringLiteral("Comment on what the spider said"),
        QStringLiteral("\u201C%1\u201D\n\nWhat was wrong, missing or right about it? Genie reads your comments on its "
                       "next run and checks the message.")
            .arg(entry->text.simplified().left(400)),
        QString(), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    if (!m_genieSaid.comment(id, text, QDateTime::currentMSecsSinceEpoch()))
        return;
    if (!saveGenieSaidLog())
        flashMessage(QStringLiteral("Could not save the comment."), true);
    fillTeammateHomeSaid();
}

void MainWindow::removeGenieSaidComment(const QString &id, int index)
{
    genieSaidLog();
    if (!m_genieSaid.uncomment(id, index))
        return;
    if (!saveGenieSaidLog())
        flashMessage(QStringLiteral("Could not save the change."), true);
    fillTeammateHomeSaid();
}

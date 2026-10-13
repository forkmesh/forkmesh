#include "NativePackageExtension.h"
#include "PackageRailTile.h"

#include <QCoreApplication>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

forkmesh::market::NativePackageFactory emptyPageFactory()
{
    return [](const forkmesh::market::NativePackageHost &, QWidget *)
               -> forkmesh::market::NativePackageWidget * { return nullptr; };
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace forkmesh::market;

    NativePackageRegistration openType({
        QStringLiteral("native:sample-tool"),
        {},
        QStringLiteral("Sample Tool"),
        QStringLiteral("tools"),
        {},
        {},
        emptyPageFactory(),
    });
    check(openType.accepted() && openType.errorString().isEmpty(),
          "a package-neutral native entry point registers");
    check(nativePackageContribution(QStringLiteral("native:sample-tool"),
                                    QStringLiteral("extension.vscode")),
          "an unrestricted contribution accepts an arbitrary manifest type");

    NativePackageRegistration typed({
        QStringLiteral("native:typed-page"),
        {QStringLiteral("extension.native")},
        QStringLiteral("Typed Page"),
        QStringLiteral("apps"),
        QStringLiteral("/api/typed-page"),
        QStringLiteral("typed-page-request-v1"),
        emptyPageFactory(),
    });
    check(typed.accepted() &&
              nativePackageContribution(QStringLiteral("native:typed-page"),
                                        QStringLiteral("extension.native")),
          "a contribution activates for a declared manifest type");
    check(!nativePackageContribution(QStringLiteral("native:typed-page"),
                                     QStringLiteral("service.compute")),
          "a contribution fails closed for an undeclared manifest type");

    NativePackageRegistration duplicate({
        QStringLiteral("native:sample-tool"),
        {},
        QStringLiteral("Replacement"),
        QStringLiteral("tools"),
        {},
        {},
        emptyPageFactory(),
    });
    check(!duplicate.accepted() &&
              duplicate.errorString().contains(QStringLiteral("duplicate")) &&
              nativePackageContribution(QStringLiteral("native:sample-tool"))
                      ->navigationLabel == QStringLiteral("Sample Tool"),
          "duplicate entry points cannot replace an existing package runtime");

    NativePackageRegistration unsafe({
        QStringLiteral("native:../escape"),
        {},
        QStringLiteral("Unsafe"),
        QStringLiteral("tools"),
        QStringLiteral("/api/../escape"),
        QStringLiteral("unsafe-proof"),
        emptyPageFactory(),
    });
    check(!unsafe.accepted() &&
              !nativePackageContribution(QStringLiteral("native:../escape")),
          "unsafe entry points and API boundaries never enter the registry");

    NativePackageRegistration link({
        QStringLiteral("native:sample-link"),
        {QStringLiteral("extension.native")},
        QStringLiteral("Sample Link"),
        QStringLiteral("link"),
        {},
        {},
        {},
        QStringLiteral("/design"),
    });
    const NativePackageContribution *linkContribution = nativePackageContribution(
        QStringLiteral("native:sample-link"), QStringLiteral("extension.native"));
    check(link.accepted() && linkContribution &&
              linkContribution->browserPath == QStringLiteral("/design") &&
              !linkContribution->factory,
          "a rail link registers a host page path without a page factory");

    NativePackageRegistration linkWithPage({
        QStringLiteral("native:link-and-page"),
        {},
        QStringLiteral("Both"),
        QStringLiteral("link"),
        {},
        {},
        emptyPageFactory(),
        QStringLiteral("/design"),
    });
    check(!linkWithPage.accepted() &&
              linkWithPage.errorString() == QStringLiteral("invalid browser link"),
          "a contribution is either a page or a link, never both");

    for (const QString &path :
         {QStringLiteral("https://example.com/design"),
          QStringLiteral("/../design"), QStringLiteral("//example.com"),
          QStringLiteral("/design?next=x"), QStringLiteral("design")}) {
        NativePackageRegistration unsafeLink({
            QStringLiteral("native:unsafe-link"),
            {},
            QStringLiteral("Unsafe Link"),
            QStringLiteral("link"),
            {},
            {},
            {},
            path,
        });
        check(!unsafeLink.accepted() &&
                  !nativePackageContribution(QStringLiteral("native:unsafe-link")),
              "a rail link only names a path on the active host");
    }

    NativePackageRegistration linkWithApi({
        QStringLiteral("native:link-with-api"),
        {},
        QStringLiteral("Link With API"),
        QStringLiteral("link"),
        QStringLiteral("/api/link"),
        QStringLiteral("link-proof"),
        {},
        QStringLiteral("/design"),
    });
    check(!linkWithApi.accepted(),
          "a rail link gets no package API boundary");

    NativePackageHost host{
        QStringLiteral("sample-tool"),
        QStringLiteral("Sample Tool"),
        QStringLiteral("service.design"),
        QStringLiteral("2.4.1"),
        {QStringLiteral("documents.read")},
        {},
    };
    check(host.packageId == QStringLiteral("sample-tool") &&
              host.packageName == QStringLiteral("Sample Tool") &&
              host.packageType == QStringLiteral("service.design") &&
              host.packageVersion == QStringLiteral("2.4.1") &&
              host.permissionIds == QStringList{QStringLiteral("documents.read")},
          "the generic host carries manifest identity, type, version and permissions");

    const QString marketingInstall = nativePackageLifecyclePath(
        QStringLiteral("forkmesh"), QStringLiteral("marketing"),
        QStringLiteral("install"));
    check(marketingInstall ==
              QStringLiteral("/api/plugins/forkmesh/marketing/install"),
          "the host constructs the Marketing organization install path");
    check(nativePackageRequestPathAllowed(
              QStringLiteral("marketing"), QStringLiteral("/api/marketing"),
              marketingInstall) &&
              nativePackageRequestPathAllowed(
                  QStringLiteral("marketing"),
                  QStringLiteral("/api/marketing"),
                  QStringLiteral("/api/marketing/forkmesh/content")),
          "Marketing lifecycle and package API paths share one boundary");
    check(!nativePackageRequestPathAllowed(
              QStringLiteral("marketing"), QStringLiteral("/api/marketing"),
              QStringLiteral("/api/plugins/forkmesh/crm/install")) &&
              !nativePackageRequestPathAllowed(
                  QStringLiteral("marketing"),
                  QStringLiteral("/api/marketing"),
                  QStringLiteral("https://evil.invalid/api/marketing")) &&
              !nativePackageRequestPathAllowed(
                  QStringLiteral("marketing"),
                  QStringLiteral("/api/marketing"),
                  QStringLiteral("/api/marketing/../plugins/forkmesh/crm")) &&
              nativePackageLifecyclePath(
                  QString(), QStringLiteral("marketing"),
                  QStringLiteral("install")).isEmpty(),
          "a package cannot cross identities, origins, or an empty organization");

    NativePackageRailTile tile;
    tile.id = QStringLiteral("youtube");
    tile.label = QStringLiteral("YouTube");
    tile.count = QStringLiteral("1.5k");
    tile.countLabel = QStringLiteral("followers");
    tile.tone = NativePackageRailTone::Warning;
    tile.stateLabel = QStringLiteral("Stale numbers");
    tile.details = {QStringLiteral("Numbers from 20 days ago (youtube)")};
    tile.steps = {{QStringLiteral("refresh"), QStringLiteral("Refresh the numbers")},
                  {QStringLiteral("post"), QStringLiteral("Post on YouTube")}};
    tile.pending = 2;
    const QStringList tip = packageRailTileToolTip(tile).split(QLatin1Char('\n'));
    check(tip.value(0) == QString::fromUtf8("YouTube \xC2\xB7 Stale numbers") &&
              tip.value(1) == QStringLiteral("1.5k followers") &&
              tip.contains(QStringLiteral("Next steps:")) &&
              tip.contains(QString::fromUtf8("\xE2\x80\xA2 Refresh the numbers")) &&
              tip.contains(QString::fromUtf8("\xE2\x80\xA2 Post on YouTube")),
          "a rail tile's hover names its state, count and every next step");
    check(tip.constLast() == QStringLiteral("Click for next steps."),
          "a rail tile without a page says a click lists its steps");
    tile.opensPage = true;
    check(packageRailTileToolTip(tile).endsWith(
              QStringLiteral("Click for details, right-click for next steps.")),
          "a rail tile with a page says a click opens it");
    tile.steps.clear();
    check(!packageRailTileToolTip(tile).contains(QStringLiteral("Next steps:")),
          "a rail tile with no steps lists none");
    check(packageRailToneColor(NativePackageRailTone::Good, true) !=
                  packageRailToneColor(NativePackageRailTone::Warning, true) &&
              packageRailToneColor(NativePackageRailTone::Attention, false) !=
                  packageRailToneColor(NativePackageRailTone::Paused, false),
          "rail tones are told apart by colour");

    NativePackageRailTile stale = tile;
    stale.steps = {{QStringLiteral("refresh"), QStringLiteral("Refresh the numbers")},
                   {QStringLiteral("open"), QStringLiteral("Open YouTube")}};
    NativePackageRailTile attention;
    attention.id = QStringLiteral("reddit");
    attention.label = QStringLiteral("Reddit");
    attention.tone = NativePackageRailTone::Attention;
    attention.stateLabel = QStringLiteral("Needs attention");
    attention.pending = 3;
    attention.steps = {{QStringLiteral("open"), QStringLiteral("Open Reddit")},
                       {QStringLiteral("post"), QStringLiteral("Post on Reddit.")}};
    NativePackageRailTile setup;
    setup.id = QStringLiteral("x");
    setup.label = QStringLiteral("X");
    setup.tone = NativePackageRailTone::Neutral;
    setup.pending = 1;
    NativePackageRailTile good;
    good.id = QStringLiteral("github");
    good.tone = NativePackageRailTone::Good;
    NativePackageRailTile paused = stale;
    paused.tone = NativePackageRailTone::Paused;
    check(packageRailUrgency(good) == 0 && packageRailUrgency(paused) == 0 &&
              packageRailUrgency(NativePackageRailTile()) == 0,
          "an on-track, paused or empty channel needs no reminder");
    check(packageRailNeedsMoreAttention(stale, attention) &&
              packageRailNeedsMoreAttention(attention, setup) &&
              packageRailNeedsMoreAttention(setup, good) &&
              !packageRailNeedsMoreAttention(good, NativePackageRailTile()) &&
              !packageRailNeedsMoreAttention(paused, NativePackageRailTile()),
          "stale numbers outrank a failing check, which outranks a channel not set up");
    NativePackageRailTile worse = attention;
    worse.pending = 4;
    check(packageRailNeedsMoreAttention(worse, attention) &&
              !packageRailNeedsMoreAttention(attention, worse) &&
              !packageRailNeedsMoreAttention(attention, attention),
          "between two channels in the same state the one failing more checks wins");
    check(packageRailReminderTitle(stale) == QString::fromUtf8("ForkMesh \xE2\x80\x94 fix YouTube") &&
              packageRailReminderBody(stale) ==
                  QStringLiteral("No channel has changed in the last hour. YouTube needs the most "
                                 "attention (stale numbers, 2 checks failing). Next step: Refresh "
                                 "the numbers."),
          "the reminder names the channel, its state and its first next step");
    check(packageRailReminderBody(attention).endsWith(
              QStringLiteral("(needs attention, 3 checks failing). Next step: Post on Reddit.")),
          "the reminder skips the Open step and keeps a step's own full stop");
    NativePackageRailTile older = stale;
    older.details = {QStringLiteral("Numbers from 21 days ago (youtube)")};
    older.steps.clear();
    NativePackageRailTile grown = stale;
    grown.count = QStringLiteral("1.6k");
    check(packageRailStateKey(older) == packageRailStateKey(stale) &&
              packageRailStateKey(grown) != packageRailStateKey(stale) &&
              packageRailStateKey(attention) != packageRailStateKey(worse),
          "only a channel's count, state or failing checks count as a change");

    return failures == 0 ? 0 : 1;
}

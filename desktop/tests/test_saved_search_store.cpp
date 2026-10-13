#include "HiddenTerms.h"
#include "SavedSearchStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

QMap<QString, int> counts(std::initializer_list<std::pair<QString, int>> rows)
{
    QMap<QString, int> out;
    for (const auto &row : rows)
        out.insert(row.first, row.second);
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "FAIL: temporary directory is available\n";
        return 1;
    }
    const QString path = QDir(dir.path()).filePath(QStringLiteral("nested/saved.json"));

    const QString issue7 = SavedSearchStore::resultKey(QStringLiteral("issue"),
                                                       QStringLiteral("7"));
    check(issue7 == SavedSearchStore::resultKey(QStringLiteral("issue"),
                                                QStringLiteral("7")),
          "result keys are stable");
    check(issue7 != SavedSearchStore::resultKey(QStringLiteral("pull"),
                                                QStringLiteral("7")),
          "result keys include the result kind");
    const QString commitA = SavedSearchStore::resultKey(
        QStringLiteral("commit-message"), QStringLiteral("aaaa"));
    const QString commitB = SavedSearchStore::resultKey(
        QStringLiteral("commit-message"), QStringLiteral("bbbb"));

    QString id;
    {
        SavedSearchStore store(path);
        check(store.searches().isEmpty(), "a missing file loads as no searches");
        const SavedSearch saved =
            store.add(QStringLiteral("  Flaky Test "), QStringLiteral("me/app"),
                      false, 1000);
        id = saved.id;
        check(!id.isEmpty() && saved.query == QStringLiteral("Flaky Test"),
              "saving trims the query and assigns an id");
        check(store.add(QStringLiteral("flaky test"), QStringLiteral("me/app"),
                        true, 2000).id == id &&
                  store.searches().size() == 1,
              "saving the same words in the same scope keeps one search");
        const SavedSearch other =
            store.add(QStringLiteral("flaky test"), QString(), false, 3000);
        check(other.id != id && store.searches().size() == 2 &&
                  store.searches().first().id == other.id,
              "another scope is another search, listed first");
        check(store.findQuery(QStringLiteral("FLAKY TEST"),
                              QStringLiteral("me/app"))->id == id,
              "finding a query ignores case");
        check(!store.hasAlerts(), "alerts are off until asked for");
        check(store.setAlerts(id, true) && store.hasAlerts() &&
                  !store.setAlerts(id, true),
              "alerts toggle and report whether anything changed");
        check(store.remove(other.id) && !store.remove(other.id) &&
                  store.searches().size() == 1,
              "removing a search drops it once");

        const SavedSearchRun first = store.recordRun(
            id, {issue7, commitA, commitA},
            counts({{QStringLiteral("issue"), 1},
                    {QStringLiteral("commit-message"), 1}}),
            5000, true);
        check(first.recorded && first.baseline && first.newKeys.isEmpty(),
              "the first run is a baseline with nothing new");
        const SavedSearch *afterFirst = store.find(id);
        check(afterFirst->runs == 1 && afterFirst->lastTotal == 2 &&
                  afterFirst->seen.size() == 2 && afterFirst->lastRunMs == 5000,
              "a run records its totals and de-duplicated keys");

        const SavedSearchRun second = store.recordRun(
            id, {commitB, commitA},
            counts({{QStringLiteral("commit-message"), 2}}), 9000, false);
        check(!second.baseline && second.newKeys == QStringList{commitB},
              "a later run reports only results never seen before");
        const SavedSearch *afterSecond = store.find(id);
        check(afterSecond->newSinceViewed == 1 && afterSecond->lastNew == 1 &&
                  afterSecond->previousTotal == 2 && afterSecond->lastTotal == 2,
              "background runs accumulate unviewed new results");
        check(afterSecond->seen.contains(issue7),
              "a result that dropped out stays seen");

        const SavedSearchRun third = store.recordRun(
            id, {issue7, commitA, commitB},
            counts({{QStringLiteral("issue"), 1},
                    {QStringLiteral("commit-message"), 2}}),
            12000, false);
        check(third.newKeys.isEmpty(),
              "a result that comes back is not reported again");
        check(!store.recordRun(QStringLiteral("missing"), {}, {}, 1, true).recorded,
              "an unknown id records nothing");
        check(!store.find(id)->securityPertinent &&
                  store.setSecurityPertinent(id, true) &&
                  store.find(id)->securityPertinent,
              "security pertinent is off until asked for");
        check(store.find(id)->resultFingerprint ==
                  SavedSearchStore::resultFingerprint({commitB, issue7, commitA}),
              "a run records the fingerprint of its result set");
        check(SavedSearchStore::resultFingerprint({issue7, commitA}) ==
                      SavedSearchStore::resultFingerprint({commitA, issue7, issue7}) &&
                  SavedSearchStore::resultFingerprint({issue7}) !=
                      SavedSearchStore::resultFingerprint({commitA}),
              "fingerprints ignore order and duplicates");
        check(SavedSearchStore::parseExcluded(
                  QStringLiteral(" me/fork, Vendor ;\nvendor,, third_party ")) ==
                  QStringList({QStringLiteral("me/fork"), QStringLiteral("Vendor"),
                               QStringLiteral("third_party")}),
              "exclusions split on commas, semicolons and lines, trimmed and de-duplicated");
        check(SavedSearchStore::excludes({QStringLiteral("ME/Fork")},
                                         QStringLiteral("me/fork\nLeaked key")) &&
                  !SavedSearchStore::excludes({QStringLiteral("me/fork")},
                                              QStringLiteral("me/app\nLeaked key")) &&
                  !SavedSearchStore::excludes({}, QStringLiteral("anything")),
              "an exclusion matches any field, ignoring case");
        check(store.find(id)->excluded.isEmpty() &&
                  store.setExcluded(id, {QStringLiteral("me/fork"), QStringLiteral(" ")}) &&
                  store.find(id)->excluded == QStringList{QStringLiteral("me/fork")} &&
                  !store.setExcluded(id, {QStringLiteral("me/fork")}),
              "exclusions are stored cleaned and an unchanged list is no change");
        check(store.acknowledgeSecurityAlarm(id), "an alarm can be acknowledged");
        check(store.save(), "the store writes its file, creating the directory");
    }

    {
        SavedSearchStore reloaded(path);
        const SavedSearch *search = reloaded.find(id);
        check(search && search->alerts && search->runs == 3 &&
                  search->newSinceViewed == 1 && search->scope == QStringLiteral("me/app") &&
                  search->kindCounts.value(QStringLiteral("commit-message")) == 2 &&
                  search->seen.size() == 3 && search->lastViewedMs == 5000 &&
                  search->securityPertinent &&
                  !search->resultFingerprint.isEmpty() &&
                  search->acknowledgedFingerprint == search->resultFingerprint,
              "every field survives a reload");
        reloaded.recordRun(id, {commitA}, {}, 13000, true);
        check(reloaded.find(id)->newSinceViewed == 0 &&
                  reloaded.find(id)->lastViewedMs == 13000,
              "watching a run clears the unviewed count");

        QStringList many;
        for (int i = 0; i < SavedSearchStore::kMaxSeenKeys + 50; ++i)
            many.append(SavedSearchStore::resultKey(QStringLiteral("file"),
                                                    QString::number(i)));
        reloaded.recordRun(id, many, {}, 14000, true);
        check(reloaded.find(id)->seen.size() == SavedSearchStore::kMaxSeenKeys &&
                  reloaded.find(id)->seen.first() == many.first(),
              "the seen list is capped, keeping the latest run first");
    }

    {
        const QString legacyPath =
            QDir(dir.path()).filePath(QStringLiteral("legacy.json"));
        QFile legacy(legacyPath);
        legacy.open(QIODevice::WriteOnly);
        legacy.write(R"({"version":1,"searches":[)"
                     R"({"id":"a","query":"secret","scope":"","alerts":true,)"
                     R"("runs":2,"lastTotal":4,"lastRunMs":7000},)"
                     R"({"id":"b","query":"never ran","scope":""}]})");
        legacy.close();
        SavedSearchStore store(legacyPath);
        const SavedSearch *ran = store.find(QStringLiteral("a"));
        const SavedSearch *fresh = store.find(QStringLiteral("b"));
        check(ran && fresh && !ran->securityPertinent && !fresh->securityPertinent &&
                  ran->alerts,
              "a file from before the security flag loads with it off");
        check(ran && !ran->resultFingerprint.isEmpty() &&
                  ran->acknowledgedFingerprint.isEmpty() && fresh &&
                  fresh->resultFingerprint.isEmpty(),
              "a legacy run gets a stand-in fingerprint and no acknowledgement");
        check(store.acknowledgeSecurityAlarm(QStringLiteral("a")) &&
                  !store.acknowledgeSecurityAlarm(QStringLiteral("b")),
              "a legacy run can be acknowledged; a search never run cannot");
    }

    {
        namespace hidden = forkmesh::hiddenterms;
        QMap<QString, QString> variables;
        variables.insert(QStringLiteral("SHORT"), QStringLiteral("true"));
        variables.insert(QStringLiteral("TOKEN"), QStringLiteral("  sk-live-0123456789  "));
        variables.insert(QStringLiteral("TOKEN_PREFIX"), QStringLiteral("sk-live-0123"));
        variables.insert(QStringLiteral("TOKEN_COPY"), QStringLiteral("SK-LIVE-0123456789"));
        const QStringList values = hidden::variableTerms(variables);
        check(values == QStringList({QStringLiteral("sk-live-0123456789"),
                                     QStringLiteral("sk-live-0123")}),
              "variable values are trimmed, de-duplicated, longest first, short ones skipped");

        hidden::setTerms({QStringLiteral("probe-repo")});
        hidden::setVariableValues(values);
        check(hidden::terms() == QStringList({QStringLiteral("probe-repo")}),
              "the editable hidden list never includes variable values");
        check(hidden::variableValueCount() == 2, "the variable value count is kept");
        check(hidden::hides(QStringLiteral("key=sk-live-0123456789")) &&
                  hidden::hides(QStringLiteral("PROBE-REPO/main")),
              "variable values and listed terms are both hidden from search");
        check(hidden::redact(QStringLiteral("key=sk-live-0123456789 in probe-repo")) ==
                  QStringLiteral("key=[hidden] in [hidden]"),
              "a whole variable value is redacted before a shorter one inside it");
        check(hidden::redact(QByteArray("x SK-LIVE-0123456789")) == QByteArray("x [hidden]"),
              "byte lines are redacted, ignoring case");

        hidden::setVariableValues({});
        check(!hidden::hides(QStringLiteral("key=sk-live-0123456789")),
              "with the option off variable values show again");
        hidden::setTerms({});
    }

    if (failures == 0)
        std::cout << "saved search store tests passed\n";
    return failures == 0 ? 0 : 1;
}

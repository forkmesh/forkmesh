#include "GenieVitals.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
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

bool near(double a, double b)
{
    return std::abs(a - b) < 1e-6;
}

const qint64 kNow = 1790000000000LL;

GenieVitalSigns signs(int commits, int runs, int merged, int emails, int contacts, int visits, int replies = 0,
                      int bounced = 0, qint64 at = kNow)
{
    GenieVitalSigns s;
    s.commits = commits;
    s.agentRuns = runs;
    s.merged = merged;
    s.emailsSent = emails;
    s.contactsTouched = contacts;
    s.marketingVisits = visits;
    s.replies = replies;
    s.bounced = bounced;
    s.measuredAtMs = at;
    return s;
}

const GenieMarketingStep *find(const GenieCoaching &coaching, const char *id)
{
    for (const GenieMarketingStep &step : coaching.steps)
        if (step.id == QLatin1String(id))
            return &step;
    return nullptr;
}

void testScoring()
{
    check(GenieVitals::fill(0, 14) == 0, "no points is empty");
    check(near(GenieVitals::fill(14, 14), 1 - std::exp(-1.6)), "goal points fill 80%");
    check(GenieVitals::codePoints(signs(4, 2, 1, 0, 0, 0)) == 11, "code points weigh runs and merges");
    check(GenieVitals::marketingPoints(signs(0, 0, 0, 3, 2, 4)) == 7, "page visits are not marketing");
    check(GenieVitals::marketingPoints(signs(0, 0, 0, 3, 4, 0, 2, 1)) == 15, "replies weigh most, bounces cost");
    check(GenieVitals::marketingPoints(signs(0, 0, 0, 0, 0, 0, 0, 5)) == 0, "points never go negative");

    const GenieVitals silent = GenieVitals::measure(signs(0, 0, 0, 40, 60, 30), kNow);
    check(near(silent.marketing, GenieVitals::silentCap), "no replies caps marketing below healthy");
    check(silent.summary().contains(QStringLiteral("Nobody has replied")), "summary says nobody replied");
    check(silent.summary().contains(QStringLiteral("40 people emailed and 60 contact records touched")),
          "summary counts people, not page visits");
    check(GenieVitals::measure(signs(0, 0, 0, 40, 60, 0, 1), kNow).marketing == GenieVitals::fewRepliesCap,
          "one reply is not humming yet");
    check(GenieVitals::measure(signs(0, 0, 0, 40, 60, 0, 5), kNow).marketing > 0.9, "real replies can hum");
    check(GenieVitals::measure(signs(0, 0, 0, 0, 0, 40), kNow).marketing == 0, "only opening pages earns nothing");
    check(near(GenieVitals::combine(1, 0), 0.275), "a neglected side drags health down");
    check(GenieVitals::levelOf(0.8) == GenieVitalLevel::Thriving, "thriving");
    check(GenieVitals::levelOf(0.6) == GenieVitalLevel::Happy, "happy");
    check(GenieVitals::levelOf(0.4) == GenieVitalLevel::Peckish, "peckish");
    check(GenieVitals::levelOf(0.2) == GenieVitalLevel::Hungry, "hungry");
    check(GenieVitals::levelOf(0.05) == GenieVitalLevel::Sick, "poorly");
    check(GenieVitals::freshness(kNow - 3600000, kNow) == 1, "fresh readings do not fade");
    check(GenieVitals::freshness(kNow - 30LL * 24 * 3600 * 1000, kNow) == 0.35, "old readings fade to 35%");

    const GenieVitals idle = GenieVitals::measure(signs(0, 0, 0, 0, 0, 0), kNow);
    check(idle.health == 0 && idle.hearts() == 0, "nothing done is poorly with no hearts");
    check(idle.summary().contains(QStringLiteral("nothing new this week")), "summary says code is quiet");

    const GenieVitals busy = GenieVitals::measure(signs(30, 8, 4, 8, 10, 6, 4), kNow);
    check(busy.level() == GenieVitalLevel::Thriving && busy.hearts() == 5, "busy on both sides thrives");
    check(busy.summary().contains(QStringLiteral("30 commits, 8 agent runs and 4 merges")), "summary lists code facts");
    check(busy.summary().contains(QStringLiteral("8 people emailed, 10 contact records touched and 4 replies")),
          "summary lists marketing facts");

    const GenieVitals lopsided = GenieVitals::measure(signs(40, 10, 0, 0, 0, 0), kNow);
    check(lopsided.weakest() == QLatin1String("marketing"), "marketing is the weak side");
    check(lopsided.summary().endsWith(QStringLiteral("Reach out to a contact to feed me.")),
          "hungry summary says how to feed it");
}

void testEncoding()
{
    const GenieVitalSigns original = signs(1, 2, 3, 4, 5, 6, 7, 8, 789);
    GenieVitalSigns back;
    check(GenieVitalSigns::decode(original.encoded(), &back) && back == original, "signs round-trip");
    check(!GenieVitalSigns::decode({QStringLiteral("1")}, &back), "short lists are rejected");
    check(!GenieVitalSigns::decode(original.encoded().mid(2), &back), "readings from the looser count are dropped");
    QStringList bad = original.encoded();
    bad[0] = QStringLiteral("x");
    check(!GenieVitalSigns::decode(bad, &back), "garbage is rejected");
    check(GenieVitals::isMarketingPage(QStringLiteral("crm")) && !GenieVitals::isMarketingPage(QStringLiteral("chat")),
          "marketing pages");
}

void testCoaching()
{
    GenieMarketingFacts facts;
    facts.vitals = GenieVitals::measure(signs(0, 0, 0, 0, 0, 0), kNow);
    GenieCoaching coaching = GenieMarketingCoach::coach(facts);
    check(coaching.stage == QLatin1String("Asleep") && coaching.page == QLatin1String("agents"), "asleep starts small");
    check(coaching.next == 0 && coaching.steps.at(0).id == QLatin1String("audience"), "first step is the audience");

    facts.vitals = GenieVitals::measure(signs(40, 10, 2, 0, 0, 0), kNow);
    facts.ticked = {QStringLiteral("audience")};
    facts.hasReadme = true;
    facts.readmeHasPicture = true;
    facts.contacts = 3;
    facts.emailsSentEver = 0;
    coaching = GenieMarketingCoach::coach(facts);
    check(coaching.stage == QLatin1String("Building in the dark"), "all code and no outreach");
    check(coaching.headline.contains(QStringLiteral("42 commits")), "headline counts what shipped");
    check(coaching.page == QLatin1String("marketing"), "building in the dark points at Marketing");
    check(coaching.steps.at(coaching.next).id == QLatin1String("list"), "README done, so the list is next");
    check(find(coaching, "list")->progress == QLatin1String("3 of 20 contacts"), "list progress");
    check(find(coaching, "front-door")->done, "README with a picture counts");
    check(coaching.doneCount == 2, "two steps done");
    check(coaching.lines().first() == QLatin1String("Next marketing step: list 20 people with the problem."),
          "a click leads with the next step in one line");
    for (const GenieMarketingStep &step : GenieMarketingCoach::playbook()) {
        GenieCoaching at = coaching;
        at.steps = {step};
        at.next = 0;
        for (const QString &line : at.lines()) {
            const qsizetype words = line.split(QLatin1Char(' '), Qt::SkipEmptyParts).size();
            check(words >= 5 && words <= 10, "every click line is five to ten words");
        }
    }
    QList<GenieTip> tips = coaching.tips();
    check(tips.size() == 5, "the advice lists the headline, the advice and the next three open steps");
    check(tips.at(0).text == coaching.headline && tips.at(1).text == coaching.advice, "the stage comes first");
    check(tips.at(2).text.startsWith(
              QStringLiteral("Next: List 20 people with the problem (3 of 20 contacts). Add people")),
          "the next step with its progress and what to do");
    check(tips.at(3).text.startsWith(QStringLiteral("Then: Talk to 5 of them (0 of 5 emailed).")),
          "then the step after");
    check(tips.at(4).text.startsWith(QStringLiteral("Then: Ship in public (42 commits to share).")),
          "done steps are skipped");
    check(tips.at(0).prompt == coaching.prompt && tips.at(1).task == coaching.task,
          "the stage tips carry the stage's agent instructions");
    check(coaching.prompt.contains(QStringLiteral("This repository got 42 commits this week")) &&
              coaching.prompt.contains(QStringLiteral("git log --since=7.days")),
          "the announcement instructions count the commits and say where to read them");
    check(tips.at(2).prompt == find(coaching, "list")->prompt && tips.at(3).task == find(coaching, "reach")->task,
          "each step tip carries its own agent instructions");

    facts.vitals = GenieVitals::measure(signs(0, 0, 0, 9, 12, 8, 4), kNow);
    coaching = GenieMarketingCoach::coach(facts);
    check(coaching.stage == QLatin1String("All talk") && coaching.page == QLatin1String("agents"),
          "all outreach and no code points at agents");

    facts.vitals = GenieVitals::measure(signs(30, 8, 4, 9, 12, 8, 0, 3), kNow);
    coaching = GenieMarketingCoach::coach(facts);
    check(coaching.stage == QLatin1String("No replies") && coaching.page == QLatin1String("crm"),
          "outreach nobody answers is not working");
    check(coaching.headline.contains(QStringLiteral("emailed 9 people")), "the headline counts who was emailed");
    check(coaching.advice.contains(QStringLiteral("3 emails bounced")), "bounces are called out");
    check(coaching.prompt.contains(QStringLiteral("9 people heard from us")) &&
              coaching.prompt.contains(QStringLiteral("do not send anything")),
          "the rewrite instructions count who was emailed and keep the agent from sending");

    facts.vitals = GenieVitals::measure(signs(30, 8, 4, 0, 1, 8, 0), kNow);
    coaching = GenieMarketingCoach::coach(facts);
    check(coaching.stage == QLatin1String("Nothing sent"),
          "a week with records touched and no email sent is its own stage, not a silent audience");
    check(coaching.headline.contains(QStringLiteral("No email went out this week")) &&
              coaching.headline.contains(QStringLiteral("1 contact")) &&
              coaching.headline.contains(QStringLiteral("volume, not wording")),
          "nothing sent means the headline asks for volume, not a rewrite");
    check(!coaching.advice.contains(QStringLiteral("Rewrite the note")) &&
              coaching.prompt.contains(QStringLiteral("do not send anything")),
          "the advice sends the note it has instead of rewriting one nobody read");
    check(GenieVitals::measure(signs(0, 0, 0, 0, 1, 0), kNow)
              .summary()
              .contains(QStringLiteral("No email went out, so there was nothing for anyone to reply to")),
          "the health line says nothing was sent rather than blaming the message");

    facts.vitals = GenieVitals::measure(signs(30, 8, 4, 8, 10, 6, 4), kNow);
    facts.contacts = 25;
    facts.emailsSentEver = 12;
    facts.analyticsVisits = 2;
    coaching = GenieMarketingCoach::coach(facts);
    check(coaching.stage == QLatin1String("Flywheel"), "both moving is a flywheel");
    check(find(coaching, "ship-public")->done && find(coaching, "measure")->done, "measured steps are done");
    check(coaching.steps.at(coaching.next).id == QLatin1String("launch"), "launch is next");
    check(coaching.page == QLatin1String("analytics"), "a flywheel measures what works");

    for (const GenieMarketingStep &step : coaching.steps)
        facts.ticked.insert(step.id);
    coaching = GenieMarketingCoach::coach(facts);
    check(coaching.next == -1 && coaching.doneCount == coaching.steps.size(), "every step ticked");
    check(coaching.advice.contains(QStringLiteral("weekly rhythm")), "finished advice keeps the rhythm");
    tips = coaching.tips();
    check(tips.size() == 2 && tips.at(0).text == coaching.headline && tips.at(1).text == coaching.advice,
          "a finished playbook leaves the stage");
    check(coaching.page == QLatin1String("analytics"), "a finished flywheel measures");

    for (const GenieMarketingStep &step : GenieMarketingCoach::playbook())
        check(!step.title.isEmpty() && !step.detail.isEmpty() && !step.task.isEmpty() && step.prompt.size() > 200,
              "every step says what to do and gives an agent full instructions");
    facts.ticked.clear();
    facts.vitals = GenieVitals::measure(signs(0, 0, 0, 0, 0, 0), kNow);
    check(!GenieMarketingCoach::coach(facts).advice.isEmpty(), "every stage has advice");
    const QList<GenieVitalSigns> stages{signs(0, 0, 0, 0, 0, 0),         signs(40, 10, 2, 0, 0, 0),
                                        signs(0, 0, 0, 9, 12, 8, 4),     signs(30, 8, 4, 9, 12, 8, 0, 3),
                                        signs(30, 8, 4, 8, 10, 6, 4),    signs(6, 1, 0, 3, 4, 2, 1),
                                        signs(2, 0, 0, 3, 5, 2, 2)};
    QSet<QString> seen;
    for (const GenieVitalSigns &stage : stages) {
        facts.vitals = GenieVitals::measure(stage, kNow);
        coaching = GenieMarketingCoach::coach(facts);
        seen.insert(coaching.stage);
        check(!coaching.task.isEmpty() && coaching.prompt.size() > 200,
              "every stage gives an agent full instructions");
        for (const GenieTip &tip : coaching.tips())
            check(!tip.task.isEmpty() && !tip.prompt.isEmpty(), "every tip can be added to an agent");
    }
    check(seen.size() >= 5, "the instructions are checked across the stages");
}

GenieWatch ours()
{
    return GenieWatch::only({QStringLiteral("crm"), QStringLiteral("marketing")});
}

void testWatch()
{
    const GenieWatch everything;
    check(everything.topics.size() == GenieWatch::all().size(), "the default watches everything");
    check(GenieWatch::decode(everything.encoded()) == everything, "the watch list round-trips");
    check(GenieWatch::decode(QString()).nothing(), "an empty saved list watches nothing");
    check(GenieWatch::decode(QStringLiteral("crm,bogus")).topics == QSet<QString>{QStringLiteral("crm")},
          "unknown topics are dropped");
    check(GenieWatch::forOrganizations({QStringLiteral("acme"), QStringLiteral("ForkMesh")}) == ours(),
          "the ForkMesh organization watches only CRM and Marketing");
    check(GenieWatch::forOrganizations({QStringLiteral("acme")}) == everything, "anyone else watches everything");
    check(ours().outreach() && !ours().code(), "CRM and Marketing are the outreach side");
    for (const QString &topic : GenieWatch::all())
        check(!GenieWatch::label(topic).isEmpty() && !GenieWatch::detail(topic).isEmpty(),
              "every topic has a label and says what it reads");

    GenieVitalSigns busy = signs(40, 10, 3, 6, 6, 0, 3);
    busy.published = 4;
    const GenieVitals marketing = GenieVitals::measure(busy, kNow, ours());
    check(marketing.code == 0 && marketing.signs.commits == 0 && marketing.signs.agentRuns == 0,
          "unwatched code is not read");
    check(marketing.health == marketing.marketing, "health follows the only watched side");
    check(!marketing.summary().contains(QStringLiteral("Code is")), "the summary leaves code out");
    check(marketing.summary().contains(QStringLiteral("4 items published or posted")),
          "the summary counts what was published or posted");
    const QString named = marketing.summary(QStringLiteral("forkmesh"));
    check(named.contains(QStringLiteral("Marketing for forkmesh is ")) &&
              named.contains(QStringLiteral("4 items published or posted this week")),
          "the summary names the organization in the marketing sentence");
    check(marketing.summary(QString()) == marketing.summary() &&
              !marketing.summary().contains(QStringLiteral("Marketing for")),
          "without an organization the summary does not name one");

    const GenieVitals codeOnly = GenieVitals::measure(busy, kNow, GenieWatch::only({QStringLiteral("code")}));
    check(codeOnly.signs.emailsSent == 0 && codeOnly.signs.published == 0 && codeOnly.marketing == 0,
          "unwatched outreach is not read");
    check(codeOnly.health == codeOnly.code && codeOnly.weakest() == QLatin1String("code"), "code alone");

    GenieVitalSigns posts = signs(0, 0, 0, 0, 0, 0);
    posts.published = 10;
    const GenieVitals noCrm = GenieVitals::measure(posts, kNow, GenieWatch::only({QStringLiteral("marketing")}));
    check(noCrm.marketing > GenieVitals::fewRepliesCap, "without the CRM there are no replies to cap on");
    posts.published = 1;
    check(GenieVitals::measure(posts, kNow, GenieWatch::only({QStringLiteral("marketing")}))
              .summary()
              .endsWith(QStringLiteral("Publish something to feed me.")),
          "marketing alone is fed by publishing");

    const GenieVitals none = GenieVitals::measure(busy, kNow, GenieWatch::only({}));
    check(none.health == GenieVitals::restingHealth, "watching nothing rests");
    check(none.summary().contains(QStringLiteral("not watching")), "and says so");
    GenieMarketingFacts facts;
    facts.vitals = none;
    const GenieCoaching idle = GenieMarketingCoach::coach(facts);
    check(idle.stage == QLatin1String("Not watching") && idle.page == QLatin1String("settings"),
          "watching nothing points at Settings");
    check(idle.steps.isEmpty(), "no playbook unless it is watched");
}

void testEncodingPublished()
{
    GenieVitalSigns original = signs(1, 2, 3, 4, 5, 6, 7, 8, 789);
    original.published = 9;
    GenieVitalSigns back;
    check(original.encoded().size() == 10, "published is saved");
    check(GenieVitalSigns::decode(original.encoded(), &back) && back == original, "published round-trips");
    QStringList older = original.encoded();
    older.removeAt(8);
    check(GenieVitalSigns::decode(older, &back) && back.published == 0 && back.measuredAtMs == 789,
          "readings saved before published still load");
}

QJsonObject contact(const char *id, const char *name, const char *email, qint64 contacted = 0,
                    const char *nextStep = "")
{
    return QJsonObject{{QStringLiteral("id"), QLatin1String(id)},
                       {QStringLiteral("name"), QLatin1String(name)},
                       {QStringLiteral("email"), QLatin1String(email)},
                       {QStringLiteral("lastContactedAt"), double(contacted)},
                       {QStringLiteral("nextStep"), QLatin1String(nextStep)}};
}

QJsonObject send(const char *direction, const char *status, qint64 at, const char *contactId, const char *address)
{
    QJsonObject item{{QStringLiteral("direction"), QLatin1String(direction)},
                     {QStringLiteral("status"), QLatin1String(status)},
                     {QStringLiteral("sentAt"), double(at)},
                     {QStringLiteral("contactId"), QLatin1String(contactId)}};
    item.insert(QLatin1String(direction) == QLatin1String("inbound") ? QStringLiteral("fromEmail")
                                                                      : QStringLiteral("to"),
                QLatin1String(address));
    return item;
}

void testCrmDigest()
{
    const qint64 day = 24LL * 3600 * 1000;
    QJsonObject ana = contact("c1", "Ana", "ana@example.com", kNow - 2 * day);
    ana.insert(QStringLiteral("company"), QStringLiteral("Acme"));
    QJsonObject bo = contact("c2", "Bo", "bo@example.com", kNow - 20 * day, "send the deck");
    bo.insert(QStringLiteral("fields"), QJsonObject{{QStringLiteral("other_emails"), QStringLiteral("b@work.io")}});
    const QJsonObject summary{
        {QStringLiteral("organization"), QStringLiteral("forkmesh")},
        {QStringLiteral("contacts"),
         QJsonArray{ana, bo, contact("c3", "Cy", "cy@example.com"), contact("c4", "Di", "di@example.com"),
                    contact("c5", "", "")}},
        {QStringLiteral("emails"),
         QJsonArray{send("outbound", "delivered", kNow - 5 * day, "c1", "ana@example.com"),
                    send("inbound", "external", kNow - 3 * day, "c1", "ana@example.com"),
                    send("outbound", "delivered", kNow - 25 * day, "c2", "bo@example.com"),
                    send("outbound", "bounce", kNow - day, "c3", "cy@example.com"),
                    send("outbound", "bounce", kNow - 30 * day, "", "old@example.com")}},
        {QStringLiteral("mail"),
         QJsonArray{QJsonObject{{QStringLiteral("from"), QStringLiteral("Bo <B@Work.io>")},
                                {QStringLiteral("receivedAt"), double(kNow - day)}},
                    QJsonObject{{QStringLiteral("from"), QStringLiteral("stranger@example.com")},
                                {QStringLiteral("receivedAt"), double(kNow - day)}}}}};

    const GenieCrmCounts counts = GenieCrmCounts::read(summary, kNow - GenieVitals::windowMs);
    check(counts.replies == 2, "synced mail from a contact counts as a reply, strangers do not");

    const GenieCrmDigest digest = GenieCrmDigest::read(summary, kNow);
    check(digest.source == GenieSource::Ready && digest.organization == QLatin1String("forkmesh"), "digest is ready");
    check(digest.contacts == 5, "every contact counts");
    check(digest.awaiting.size() == 2 && digest.awaiting.at(0).startsWith(QStringLiteral("Bo (wrote ")) &&
              digest.awaiting.at(1).startsWith(QStringLiteral("Ana (Acme) (wrote ")),
          "people who wrote last, newest first, found by any of their addresses, with when they wrote");
    check(digest.bounced == QStringList({QStringLiteral("Cy")}), "this week's bounces by name");
    check(digest.nextSteps.size() == 1 && digest.nextStepContacts == 1 &&
              digest.nextSteps.first().startsWith(QStringLiteral("Bo, last contacted ")) &&
              digest.nextSteps.first().endsWith(QStringLiteral(": send the deck")),
          "a stale next step says when that contact was last reached");
    check(digest.neverEmailed == 1, "Di has an address and was never emailed; Cy bounced, so was tried");
    check(digest.addressed == 4, "the never-emailed count is read against the contacts that have an address");

    QJsonObject spammed = summary;
    spammed.insert(QStringLiteral("emails"),
                   QJsonArray{send("outbound", "delivered", kNow - 25 * day, "c2", "bo@example.com"),
                              send("inbound", "spam", kNow - day, "c2", "bo@example.com")});
    spammed.insert(QStringLiteral("mail"), QJsonArray{});
    check(GenieCrmDigest::read(spammed, kNow).awaiting.isEmpty(),
          "an inbound marked spam is not a reply, so it never makes a contact look like they wrote last");

    QJsonObject boxed = summary;
    boxed.insert(QStringLiteral("emails"), QJsonArray{});
    boxed.insert(QStringLiteral("mail"),
                 QJsonArray{QJsonObject{{QStringLiteral("from"), QStringLiteral("Bo <B@Work.io>")},
                                        {QStringLiteral("box"), QStringLiteral("Spam")},
                                        {QStringLiteral("receivedAt"), double(kNow - day)}}});
    check(GenieCrmDigest::read(boxed, kNow).awaiting.isEmpty(), "synced mail filed as spam is not a reply either");

    QJsonObject shared = summary;
    shared.insert(QStringLiteral("contacts"),
                  QJsonArray{contact("c1", "Ana", "ana@example.com", kNow - 20 * day, "check the Mastodon DMs"),
                             contact("c2", "Bo", "bo@example.com", kNow - 20 * day, "Check the Mastodon DMs "),
                             contact("c3", "Cy", "cy@example.com", kNow - 20 * day, "send the deck")});
    shared.insert(QStringLiteral("emails"), QJsonArray{});
    shared.insert(QStringLiteral("mail"), QJsonArray{});
    const GenieCrmDigest grouped = GenieCrmDigest::read(shared, kNow);
    check(grouped.nextStepContacts == 3 && grouped.nextSteps.size() == 2 &&
              grouped.nextSteps.first() == QLatin1String("2 contacts (Ana and Bo): check the Mastodon DMs"),
          "contacts sharing one next step are grouped, so the tip shows one action, not two names");
}

void testLessons()
{
    const qint64 day = 24LL * 3600 * 1000;
    QJsonObject ana = contact("c1", "Ana", "ana@fund.vc", kNow - 2 * day);
    ana.insert(QStringLiteral("stage"), QStringLiteral("passed"));
    QJsonObject bo = contact("c2", "Bo", "bo@example.com", kNow - 20 * day, "send the deck");
    bo.insert(QStringLiteral("company"), QStringLiteral("Acme"));
    const QJsonObject summary{
        {QStringLiteral("organization"), QStringLiteral("forkmesh")},
        {QStringLiteral("contacts"),
         QJsonArray{ana, bo, contact("c3", "Cy", "cy@example.com"), contact("c4", "Di", "di@example.com")}},
        {QStringLiteral("emails"),
         QJsonArray{send("outbound", "delivered", kNow - 5 * day, "c1", "ana@fund.vc"),
                    send("inbound", "external", kNow - 3 * day, "c1", "ana@fund.vc"),
                    send("outbound", "delivered", kNow - 25 * day, "c2", "bo@example.com"),
                    send("inbound", "external", kNow - day, "c2", "bo@example.com"),
                    send("outbound", "bounce", kNow - day, "c3", "cy@example.com"),
                    send("outbound", "bounce", kNow - day, "", "gone@example.com")}}};

    const GenieCrmDigest open = GenieCrmDigest::read(summary, kNow);
    check(open.closed == 1, "a passed investor is a closed contact");
    check(open.awaiting.size() == 1 && open.awaiting.first().startsWith(QStringLiteral("Bo (Acme) (wrote ")),
          "a contact who passed is not suggested even when they wrote last");
    check(GenieLessons::closedStage(QStringLiteral("Closed Lost")) && GenieLessons::closedStage(QStringLiteral("inactive")) &&
              !GenieLessons::closedStage(QStringLiteral("meeting")) && !GenieLessons::closedStage(QString()),
          "closed stages match whatever the spelling");

    QJsonObject ownStages = summary;
    QJsonObject done = contact("c5", "Eve", "eve@fund.vc", kNow - 20 * day, "send the deck");
    done.insert(QStringLiteral("stage"), QStringLiteral("complete"));
    QJsonObject tagged = contact("c6", "Fay", "fay@fund.vc", kNow - 20 * day, "send the deck");
    tagged.insert(QStringLiteral("stage"), QStringLiteral("contacted"));
    tagged.insert(QStringLiteral("tags"), QJsonArray{QStringLiteral("warm"), QStringLiteral("Declined")});
    ownStages.insert(QStringLiteral("contacts"), QJsonArray{ana, bo, done, tagged});
    ownStages.insert(QStringLiteral("settings"),
                     QJsonObject{{QStringLiteral("flows"),
                                  QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("investors")},
                                                         {QStringLiteral("stages"),
                                                          QJsonArray{QStringLiteral("new"), QStringLiteral("lead"),
                                                                     QStringLiteral("contacted"),
                                                                     QStringLiteral("complete")}}}}}});
    const GenieCrmDigest own = GenieCrmDigest::read(ownStages, kNow);
    check(own.closingStages.contains(QStringLiteral("complete")) && own.closed == 3,
          "the closing stage comes from the organization's own flows, not a fixed list of stage names");
    check(own.nextSteps.size() == 1 && own.nextSteps.first().startsWith(QStringLiteral("Bo (Acme)")),
          "a contact in the closing stage or tagged declined keeps their next step out of the tips");

    GenieLessons lessons;
    const QStringList taught = lessons.learn(
        QStringLiteral("Done.\n```forkmesh-genie\n{\"skip\": [{\"contact\": \"BO@example.com\", \"reason\": \"Passed on "
                       "investment\"}, {\"contact\": \"Cy\"}, \"gone@example.com\"]}\n```\nCaption: taught"),
        kNow);
    check(taught.size() == 3 && lessons.skip.size() == 3, "every skip in the block is learned");
    check(lessons.skip.first().contact == QLatin1String("gone@example.com") && lessons.skip.last().reason ==
                                                                                   QLatin1String("Passed on investment"),
          "newest lesson first, reason kept");
    check(lessons.learn(QStringLiteral("no block here"), kNow).isEmpty(), "output without a block teaches nothing");

    const GenieCrmDigest taughtDigest = GenieCrmDigest::read(summary, kNow, lessons);
    check(taughtDigest.awaiting.isEmpty() && taughtDigest.nextSteps.isEmpty(),
          "a taught contact is matched by address, case-insensitively");
    check(taughtDigest.bounced.isEmpty(), "taught contacts and addresses leave the bounce list");
    check(taughtDigest.taughtOut == QStringList({QStringLiteral("Bo (Acme)"), QStringLiteral("Cy")}),
          "the taught contacts found in the CRM are named");
    check(taughtDigest.neverEmailed == 1, "Di is still suggested");

    GenieLessons back = GenieLessons::decode(lessons.encoded());
    check(back.skip.size() == 3 && back.skip.last().atMs == kNow, "lessons round-trip");
    const QStringList undone = back.learn(QStringLiteral("```forkmesh-genie\n{\"unskip\": [\"cy\"]}\n```"), kNow);
    check(undone.size() == 1 && back.skip.size() == 2, "unskip lets the spider suggest a contact again");

    GenieWatch watch = GenieWatch::only({QStringLiteral("crm")});
    GenieMarketingFacts facts;
    facts.vitals = GenieVitals::measure(signs(0, 0, 0, 1, 1, 0), kNow, watch);
    facts.crm = taughtDigest;
    facts.nowMs = kNow;
    bool told = false;
    for (const GenieSuggestion &suggestion : GenieMarketingCoach::suggestions(facts))
        told = told || (suggestion.prompt.contains(QStringLiteral("closed stage (passed")) &&
                        suggestion.prompt.contains(QStringLiteral("leave alone: Bo (Acme), Cy")));
    check(told, "agent prompts leave out closed and taught contacts");

    const QString briefing = genieAgentBriefing(QStringLiteral("Marketing is slowing down."),
                                                {GenieTip{QStringLiteral("Answer Bo first.")}}, back);
    check(briefing.contains(QStringLiteral("1. Answer Bo first.")) &&
              briefing.contains(QStringLiteral("- BO@example.com: Passed on investment")) &&
              briefing.contains(QStringLiteral("```forkmesh-genie")),
          "the Genie agent sees the tips, what it was taught and how to teach");
}

void testMarketingDigest()
{
    const qint64 day = 24LL * 3600 * 1000;
    const qint64 since = kNow - GenieVitals::windowMs;
    const auto item = [](const char *status, const char *title, const char *channel, qint64 published,
                         qint64 scheduled, const char *url) {
        return QJsonObject{{QStringLiteral("status"), QLatin1String(status)},
                           {QStringLiteral("title"), QLatin1String(title)},
                           {QStringLiteral("channel"), QLatin1String(channel)},
                           {QStringLiteral("publishedAt"), double(published)},
                           {QStringLiteral("scheduledAt"), double(scheduled)},
                           {QStringLiteral("url"), QLatin1String(url)}};
    };
    const auto note = [](const char *level, const char *title, const char *detail) {
        return QJsonObject{{QStringLiteral("level"), QLatin1String(level)},
                           {QStringLiteral("title"), QLatin1String(title)},
                           {QStringLiteral("detail"), QLatin1String(detail)}};
    };
    const QJsonObject overview{
        {QStringLiteral("organization"), QStringLiteral("forkmesh")},
        {QStringLiteral("channelCatalog"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("blog")}, {QStringLiteral("name"), QStringLiteral("Blog")}}}},
        {QStringLiteral("content"),
         QJsonArray{item("published", "Watch diffs", "blog", kNow - 3 * day, 0, "https://f.com/w"),
                    item("published", "Old news", "blog", kNow - 20 * day, 0, ""),
                    item("scheduled", "Next", "x", 0, kNow + 2 * day, ""),
                    item("queued", "Later", "x", 0, kNow + 9 * day, ""),
                    item("queued", "Undated", "x", 0, 0, ""),
                    item("queued", "Overdue", "x", 0, kNow - 2 * day, ""),
                    item("draft", "Draft", "x", 0, 0, "")}},
        {QStringLiteral("posts"),
         QJsonArray{QJsonObject{{QStringLiteral("postedAt"), double(kNow - 3 * day)},
                                {QStringLiteral("url"), QStringLiteral("https://F.com/w/?utm_source=x#top")}},
                    QJsonObject{{QStringLiteral("postedAt"), double(kNow - day)},
                                {QStringLiteral("title"), QStringLiteral("Thread")},
                                {QStringLiteral("channel"), QStringLiteral("reddit")}}}},
        {QStringLiteral("redditDesk"),
         QJsonObject{{QStringLiteral("summary"), QJsonObject{{QStringLiteral("open"), 2}}}}},
        {QStringLiteral("experimentsSummary"),
         QJsonObject{{QStringLiteral("reviewsDue"), QJsonArray{"e1"}},
                     {QStringLiteral("running"), 4},
                     {QStringLiteral("runningLimit"), 3}}},
        {QStringLiteral("adsSummary"), QJsonObject{{QStringLiteral("attention"), 1}}},
        {QStringLiteral("funnelReport"),
         QJsonObject{{QStringLiteral("weekStart"), double(kNow - 7 * day)},
                     {QStringLiteral("bottleneck"), QStringLiteral("activation")},
                     {QStringLiteral("bottleneckReason"), QStringLiteral("Activation is 10% against a 30% target.")}}},
        {QStringLiteral("suggestions"),
         QJsonArray{note("setup", "Track the searches you want to win.", ""),
                    note("info", "X needs a look.", "no posts"),
                    note("warning", "Launch was due to start.", "Mark it active or move its dates.")}}};
    const QJsonObject performance{
        {QStringLiteral("performance"),
         QJsonObject{{QStringLiteral("insights"),
                      QJsonArray{note("win", "Reddit converts best.", ""),
                                 note("warning", "Visits fell 40% against the previous window.", "")}}}}};

    const GenieMarketingDigest digest = GenieMarketingDigest::read(overview, performance, kNow, since);
    check(digest.published == 2, "published this week, the same URL counted once");
    check(digest.lastPublishedTitle == QLatin1String("Thread") && digest.lastPublishedChannel == QLatin1String("reddit"),
          "the newest published item");
    check(digest.published == 2, "a post with a query string and fragment is the same item as its content");
    check(digest.scheduledAhead == 1, "only what goes out in the next 7 days is scheduled ahead");
    check(digest.queuedStuck == 2, "queued items with no date or a past slot are counted, drafts are not");
    check(digest.redditOpen == 2 && digest.experimentsDue == 1 && digest.experimentsRunning == 4 &&
              digest.experimentsLimit == 3 && digest.adsAttention == 1,
          "Reddit, experiments and ads");
    check(digest.funnelLogged && digest.bottleneck.startsWith(QStringLiteral("Activation is 10%")), "the funnel");
    check(digest.bottleneckNamed, "a named stage is flagged");
    QJsonObject unnamedOverview = overview;
    unnamedOverview.insert(QStringLiteral("funnelReport"),
                           QJsonObject{{QStringLiteral("weekStart"), double(kNow - 7 * day)},
                                       {QStringLiteral("bottleneckReason"),
                                        QStringLiteral("Only 2 of 40 visitors signed up.")}});
    const GenieMarketingDigest unnamed = GenieMarketingDigest::read(unnamedOverview, performance, kNow, since);
    check(unnamed.bottleneck == QLatin1String("Only 2 of 40 visitors signed up.") && !unnamed.bottleneckNamed,
          "a funnel reason survives when no stage is named");
    check(digest.notes == QStringList({QStringLiteral("Launch was due to start. Mark it active or move its dates."),
                                       QStringLiteral("Track the searches you want to win.")}),
          "plugin notes, warnings first and channel info left out");
    check(digest.insights.first().startsWith(QStringLiteral("Visits fell")), "Analytics insights, warnings first");

    const QByteArray log = "1790000000\x1f" "Review agent diffs on Apple Watch\n"
                           "bad line\n"
                           "1789990000\x1f" "Show received chat in Pings\n";
    const QList<GenieShippedChange> shipped = genieShippedChanges(log, QStringLiteral("forkmesh"));
    check(shipped.size() == 2 && shipped.first().atMs == 1790000000000LL &&
              shipped.first().subject == QLatin1String("Review agent diffs on Apple Watch"),
          "git log lines become shipped changes");

    GenieMarketingFacts facts;
    facts.nowMs = kNow;
    GenieVitalSigns quiet = signs(0, 0, 0, 2, 2, 0, 1);
    quiet.published = 2;
    facts.vitals = GenieVitals::measure(quiet, kNow, ours());
    facts.marketing = digest;
    facts.crm.source = GenieSource::Missing;
    facts.shippedKnown = true;
    facts.shipped = {{kNow - 1000, QStringLiteral("Review agent diffs on Apple Watch"), QStringLiteral("forkmesh")},
                     {kNow - 2000, QStringLiteral("Show received chat in Pings"), QStringLiteral("forkmesh")},
                     {kNow - 2 * day, QStringLiteral("Before the thread"), QStringLiteral("forkmesh")}};
    const GenieCoaching coaching = GenieMarketingCoach::coach(facts);
    check(coaching.steps.isEmpty(), "our watch has no playbook");
    check(!coaching.headline.contains(QStringLiteral("commit")), "with code unwatched the headline is about outreach");
    QStringList texts;
    for (const GenieSuggestion &suggestion : coaching.suggestions)
        texts << suggestion.text;
    check(texts.first().startsWith(QStringLiteral("The CRM plugin is not installed")), "a missing source is said");
    const QString shippedTip = texts.value(1);
    check(shippedTip.startsWith(QStringLiteral("2 commits shipped since Marketing last published something")),
          "only what shipped after the last published item");
    check(shippedTip.contains(QStringLiteral("Review agent diffs on Apple Watch")) &&
              !shippedTip.contains(QStringLiteral("Before the thread")),
          "names what shipped");
    check(texts.contains(QStringLiteral("Marketing: Launch was due to start. Mark it active or move its dates.")),
          "the plugin's own checks");
    check(texts.contains(QStringLiteral("2 Reddit threads are waiting for a reply or a skip in Marketing.")), "Reddit");
    check(texts.contains(QStringLiteral("Analytics: Visits fell 40% against the previous window.")), "Analytics");
    check(texts.contains(QStringLiteral("Funnel: Activation is 10% against a 30% target. That is the stage to work on.")),
          "the funnel bottleneck");
    GenieMarketingFacts unnamedFacts = facts;
    unnamedFacts.marketing = unnamed;
    const QList<GenieSuggestion> unnamedTips = GenieMarketingCoach::suggestions(unnamedFacts);
    bool sawUnnamed = false;
    for (const GenieSuggestion &suggestion : unnamedTips)
        if (suggestion.text == QLatin1String("Funnel: Only 2 of 40 visitors signed up."))
            sawUnnamed = true;
    check(sawUnnamed, "a funnel reason with no stage is said without claiming a stage");
    const auto nothingTip = [&](const GenieMarketingDigest &digestCase) {
        GenieMarketingFacts scenario = facts;
        scenario.marketing = digestCase;
        for (const GenieSuggestion &suggestion : GenieMarketingCoach::suggestions(scenario))
            if (suggestion.text.startsWith(QStringLiteral("Nothing is scheduled")))
                return suggestion.text;
        return QString();
    };
    GenieMarketingDigest idle = digest;
    idle.scheduledAhead = 0;
    idle.queuedStuck = 0;
    idle.lastPublishedAt = 0;
    check(nothingTip(idle) == QLatin1String("Nothing is scheduled in Marketing for the next 7 days. Queue the next "
                                            "post so there is no silent week."),
          "the nothing-scheduled tip with no parenthetical");
    idle.queuedStuck = 1;
    check(nothingTip(idle) == QLatin1String("Nothing is scheduled in Marketing for the next 7 days (1 queued with no "
                                            "date or past their slot). Queue the next post so there is no silent "
                                            "week."),
          "the nothing-scheduled tip says how many items are stuck");
    idle.lastPublishedAt = kNow - 3 * day;
    idle.queuedStuck = 2;
    const QString both = nothingTip(idle);
    check(both.contains(QStringLiteral("(the last item went out ")) &&
              both.contains(QStringLiteral("; 2 queued with no date or past their slot). Queue the next post")),
          "the nothing-scheduled tip keeps the last-published date and the stuck count");
    QStringList cycled;
    for (const GenieTip &tip : coaching.tips().mid(2))
        cycled << tip.text;
    check(cycled == texts, "the advice lists what it sees after the stage");
    check(coaching.suggestions.first().prompt.isEmpty(), "a missing plugin is no work for an agent");
    check(coaching.suggestions.at(1).prompt.contains(QStringLiteral("Review agent diffs on Apple Watch")) &&
              coaching.suggestions.at(1).prompt.contains(QStringLiteral("do not post or send anything")),
          "the announcement instructions quote what shipped");
    for (const GenieSuggestion &suggestion : coaching.suggestions.mid(1)) {
        const bool unseen = suggestion.text.contains(QStringLiteral("experiment")) ||
                            suggestion.text.contains(QStringLiteral("Ads manager"));
        check(unseen ? suggestion.prompt.isEmpty() : !suggestion.task.isEmpty() && suggestion.prompt.size() > 150,
              "suggestions an agent can act on give it instructions, experiments and ads do not");
    }

    facts.marketing = GenieMarketingDigest();
    facts.crm = GenieCrmDigest();
    const QList<GenieSuggestion> blind = GenieMarketingCoach::suggestions(facts);
    check(blind.size() == 1 && blind.first().text.contains(QStringLiteral("without Marketing I can't see")),
          "without Marketing it does not claim what was announced");
}

void testCrmAndActivity()
{
    const qint64 since = kNow - GenieVitals::windowMs;
    const auto email = [](const char *direction, const char *status, qint64 sent, const char *who) {
        QJsonObject item{{QStringLiteral("direction"), QLatin1String(direction)},
                         {QStringLiteral("status"), QLatin1String(status)},
                         {QStringLiteral("sentAt"), double(sent)}};
        item.insert(QLatin1String(direction) == QLatin1String("inbound") ? QStringLiteral("fromEmail")
                                                                          : QStringLiteral("to"),
                    QLatin1String(who));
        return item;
    };
    const QJsonObject summary{
        {QStringLiteral("contacts"),
         QJsonArray{QJsonObject{{QStringLiteral("updatedAt"), double(kNow - 1000)}},
                    QJsonObject{{QStringLiteral("updatedAt"), double(since - 1)},
                                {QStringLiteral("lastContactedAt"), double(kNow - 5000)}},
                    QJsonObject{{QStringLiteral("updatedAt"), double(since - 1)}}}},
        {QStringLiteral("emails"),
         QJsonArray{email("outbound", "delivered", kNow - 10, "ana@example.com"),
                    email("outbound", "open", kNow - 20, "Ana@Example.com"),
                    email("inbound", "external", kNow - 10, "ana@example.com"),
                    email("inbound", "external", kNow - 30, "ana@example.com"),
                    QJsonObject{{QStringLiteral("createdAt"), double(kNow - 20)}, {QStringLiteral("contactId"), "c2"}},
                    email("outbound", "draft", kNow - 10, "bo@example.com"),
                    email("outbound", "sending", kNow - 10, "bo@example.com"),
                    email("outbound", "bounce", kNow - 10, "cy@example.com"),
                    email("outbound", "failed", since - 100, "cy@example.com"),
                    email("outbound", "accepted", since - 100, "di@example.com")}}};
    const GenieCrmCounts counts = GenieCrmCounts::read(summary, since);
    check(counts.contacts == 3, "every contact counts towards the list");
    check(counts.contactsTouched == 1, "only a contact reached counts, not an edit or import");
    check(counts.emailsSentEver == 3, "people emailed ever, inbound, drafts and bounces left out");
    check(counts.emailsSent == 2, "people emailed this week, once each");
    check(counts.replies == 1, "replies count the people who answered");
    check(counts.bounced == 1, "bounces this week");

    QStringList log;
    log = GenieActivityLog::record(log, QStringLiteral("crm"), kNow - 60000);
    log = GenieActivityLog::record(log, QStringLiteral("crm"), kNow - 55000);
    log = GenieActivityLog::record(log, QStringLiteral("chat"), kNow - 50000);
    log = GenieActivityLog::record(log, QStringLiteral("analytics"), kNow - 40000);
    log = GenieActivityLog::record(log, QStringLiteral("bad|page"), kNow);
    check(log.size() == 3, "quick repeats and bad names are dropped");
    check(GenieActivityLog::visits(log, since) == 2, "marketing visits");
    check(GenieActivityLog::visits(log, since, QStringLiteral("analytics")) == 1, "analytics visits");
    check(GenieActivityLog::visits(log, kNow - 45000) == 1, "old visits fall out");
    for (int i = 0; i < GenieActivityLog::most + 10; ++i)
        log = GenieActivityLog::record(log, i % 2 ? QStringLiteral("crm") : QStringLiteral("chat"), kNow + i * 30000);
    check(log.size() == GenieActivityLog::most, "the log is capped");

    check(genieReadmeHasPicture(QStringLiteral("# Tool\n\n![demo](docs/demo.gif)")), "markdown image");
    check(genieReadmeHasPicture(QStringLiteral("<img src=\"shot.png\">")), "html image");
    check(!genieReadmeHasPicture(QStringLiteral("# Tool\n\nA fast tool. See [docs](docs/).")), "no picture");
}

void testSaidLog()
{
    GenieSaidLog log;
    const QString first = log.record(QStringLiteral("unasked"), QStringLiteral("Answer Bo first."),
                                     QStringLiteral("An agent can draft it."), QStringLiteral("Measured Oct 7 09:00."), kNow);
    const QString second = log.record(QStringLiteral("click"), QStringLiteral("Email Ana."), QString(),
                                      QStringLiteral("Measured Oct 7 09:00."), kNow + 1);
    check(!first.isEmpty() && first != second && log.said.size() == 2, "each message gets its own id");
    check(log.record(QStringLiteral("unasked"), QStringLiteral("Answer Bo first."), QString(),
                     QStringLiteral("Measured Oct 7 09:00."), kNow + 2) == first &&
              log.said.size() == 2 && log.said.first().id == first && log.said.first().times == 2 &&
              log.said.first().lastAtMs == kNow + 2 && log.said.first().firstAtMs == kNow,
          "the same words on the same facts count again instead of repeating");
    check(log.record(QStringLiteral("unasked"), QStringLiteral("Answer Bo first."), QString(),
                     QStringLiteral("Measured Oct 7 10:00."), kNow + 3) != first &&
              log.said.size() == 3,
          "the same words on new facts are a new message");
    check(log.record(QStringLiteral("click"), QStringLiteral("  "), QString(), QString(), kNow).isEmpty(),
          "nothing said is not recorded");

    check(!log.comment(QStringLiteral("nope"), QStringLiteral("hi"), kNow) && !log.comment(second, QStringLiteral(" "), kNow),
          "a comment needs a message and words");
    check(log.comment(second, QStringLiteral("Ana passed in June."), kNow + 10), "a comment is kept");
    check(log.awaitingCheck() == 1 && log.find(second)->awaitingCheck(), "a new comment waits for a check");

    const QString briefing = genieAgentBriefing(QStringLiteral("Fine."), {}, GenieLessons(), log);
    check(briefing.contains(QStringLiteral("id %1").arg(second)) &&
              briefing.contains(QStringLiteral("\u201CAna passed in June.\u201D")) &&
              briefing.contains(QStringLiteral("It was based on: Measured Oct 7 09:00.")) &&
              briefing.contains(QStringLiteral("Not checked since the last comment.")) &&
              briefing.contains(QStringLiteral("\"checked\"")) && !briefing.contains(QStringLiteral("Answer Bo first.")),
          "the Genie agent sees commented messages, their basis and how to check them");

    const QStringList changes = log.learn(
        QStringLiteral("```forkmesh-genie\n{\"skip\": [\"ana@fund.vc\"], \"checked\": [{\"id\": \"%1\", "
                       "\"accurate\": false, \"note\": \"Ana passed on Jun 3.\"}, {\"id\": \"missing\", "
                       "\"accurate\": true}, {\"id\": \"%2\"}]}\n```")
            .arg(second, first),
        kNow + 20);
    const GenieSaid *checked = log.find(second);
    check(changes == QStringList({QStringLiteral("mark 1 message wrong")}) && checked->verdict == QLatin1String("wrong") &&
              checked->verdictNote == QLatin1String("Ana passed on Jun 3.") && !checked->awaitingCheck() &&
              log.find(first)->verdict.isEmpty(),
          "a check marks the message by id and needs a verdict");
    check(log.comment(second, QStringLiteral("Still wrong."), kNow + 30) && log.find(second)->awaitingCheck(),
          "a later comment asks for another check");

    const GenieSaidLog back = GenieSaidLog::decode(log.encoded());
    const GenieSaid *restored = back.find(second);
    check(back.said.size() == 3 && restored && restored->comments.size() == 2 &&
              restored->verdict == QLatin1String("wrong") && restored->checkedAtMs == kNow + 20 &&
              back.find(first)->times == 2 && back.find(first)->task == QLatin1String("An agent can draft it."),
          "the log round-trips");
    check(GenieSaidLog::decode("not json").said.isEmpty(), "a broken file reads as an empty log");

    GenieSaidLog full = back;
    check(full.uncomment(second, 0) && full.find(second)->comments.size() == 1 && !full.uncomment(second, 5),
          "a comment can be removed");
    for (int n = 0; n < GenieSaidLog::most + 5; ++n)
        full.record(QStringLiteral("click"), QStringLiteral("Tip %1").arg(n), QString(), QString(), kNow + 100 + n);
    check(full.said.size() == GenieSaidLog::most && full.find(second), "old messages go, commented ones stay");

    GenieMarketingFacts facts;
    facts.vitals = GenieVitals::measure(signs(3, 0, 0, 2, 2, 0), kNow, GenieWatch::only({QStringLiteral("crm")}));
    facts.crm.source = GenieSource::Failed;
    facts.crm.error = QStringLiteral("HTTP 502");
    const QString basis = genieBasis(facts);
    check(basis.contains(QStringLiteral("watching CRM")) && basis.contains(QStringLiteral("2 people emailed")) &&
              basis.contains(QStringLiteral("CRM read failed: HTTP 502")) && !basis.contains(QStringLiteral("commits")),
          "the basis names the readings and sources behind a message");
}

}

void testBadgeLine()
{
    auto ping = [](const QString &celebrate, const QString &name, const QString &tier) {
        QJsonObject meta;
        meta.insert(QStringLiteral("celebrate"), celebrate);
        meta.insert(QStringLiteral("badgeName"), name);
        meta.insert(QStringLiteral("tier"), tier);
        QJsonObject alert;
        alert.insert(QStringLiteral("meta"), meta);
        return alert;
    };
    check(GeniePings::badgeLine(ping(QStringLiteral("badge"), QStringLiteral("Stargazer"),
                                     QStringLiteral("default")))
              == QStringLiteral("You earned Stargazer!"),
          "a first badge is announced by name");
    check(GeniePings::badgeLine(ping(QStringLiteral("badge"), QStringLiteral("Fan Club"),
                                     QStringLiteral("silver")))
              == QStringLiteral("You reached Silver Fan Club!"),
          "a level names the metal");
    check(GeniePings::badgeLine(ping(QStringLiteral("badge"), QString(), QString()))
              == QStringLiteral("You earned a new badge!"),
          "a nameless badge still cheers");
    check(GeniePings::badgeLine(ping(QStringLiteral("crm-email-tracked"),
                                     QStringLiteral("Stargazer"), QString())).isEmpty(),
          "other celebrations stay quiet");
    check(GeniePings::badgeLine(QJsonObject()).isEmpty(), "a ping without meta stays quiet");
}

int main()
{
    testScoring();
    testEncoding();
    testCoaching();
    testCrmAndActivity();
    testWatch();
    testEncodingPublished();
    testCrmDigest();
    testLessons();
    testSaidLog();
    testMarketingDigest();
    testBadgeLine();
    if (failures)
        return 1;
    std::cout << "genie vitals tests passed\n";
    return 0;
}

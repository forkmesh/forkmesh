#pragma once

#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

#include "ClientVersionGate.h"
#include "NetworkBackoff.h"

#include <functional>

class BackoffNetworkAccessManager : public QNetworkAccessManager
{
    Q_OBJECT
public:
    // What the person answered for a destination no rule covers yet. Allow
    // and Deny are remembered as rules; DenyOnce blocks just this request and
    // asks again next time.
    enum FirewallVerdict { FirewallDenyOnce, FirewallDeny, FirewallAllow };
    // How the stored rules classify a destination. When rules of both kinds
    // match, the most specific one decides and a tie goes to deny, so
    // "*.example.com denied, but example.com/api/request/* allowed" works as
    // well as the reverse.
    enum FirewallMatch { FirewallUnlisted, FirewallAllowed, FirewallDenied };
    using FirewallPrompt = std::function<FirewallVerdict(
        const QString &method, const QUrl &url, QString *ruleOut)>;
    struct RequestRecord {
        QString method;
        QString url;
        QString requestData;
        QString responseData;
        qint64 uploadBytes = 0;
        qint64 downloadBytes = 0;
        qint64 startedMs = 0;
        qint64 finishedMs = 0;
        int status = 0;
        QString error;
        bool blocked = false;
    };

    struct EndpointStats {
        QString method;
        QString endpoint;
        QString firewallDecision;
        QString lastFullUrl;
        QString lastRequestData;
        QString lastResponseData;
        int calls = 0;
        int allowed = 0;
        int blocked = 0;
        int failures = 0;
        qint64 uploadBytes = 0;
        qint64 downloadBytes = 0;
        qint64 firstCalledMs = 0;
        qint64 lastCalledMs = 0;
        int lastStatus = 0;
        QString lastError;
        QList<RequestRecord> requests;
    };

    explicit BackoffNetworkAccessManager(QObject *parent = nullptr);

    void setFirewallEnabled(bool enabled);
    bool firewallEnabled() const { return m_firewallEnabled; }
    // With the firewall on, every destination off this machine needs a
    // decision: an allow rule, a deny rule, or an answer from the prompt.
    void setFirewallRules(const QStringList &rules);
    QStringList firewallRules() const { return m_firewallRules; }
    void setFirewallDenyRules(const QStringList &rules);
    QStringList firewallDenyRules() const { return m_firewallDenyRules; }
    FirewallMatch firewallMatch(const QUrl &url) const;
    // The same verdict, plus the rule that reached it. A caller that wants to
    // attribute traffic to a rule -- "what is this allow rule actually letting
    // through?" -- cannot re-derive it from the lists without repeating the
    // most-specific-wins tie-break. *ruleOut is left empty when nothing
    // matched, and for a destination on this machine, which no rule decides.
    FirewallMatch firewallDecision(const QUrl &url, QString *ruleOut) const;
    // firewallMatch() for a connection about to be made. The rule that
    // decides it is credited in firewallRuleHits(), so the Firewall Rules page
    // can show what each rule is doing live.
    FirewallMatch recordFirewallMatch(const QUrl &url);
    // Every request and WebSocket connection a rule let through or blocked
    // this session, keyed by stored rule. Answers from the prompt are not
    // here: no rule decided those.
    struct FirewallRuleHits {
        int requests = 0;
        int blocked = 0;
    };
    QHash<QString, FirewallRuleHits> firewallRuleHits() const
    {
        return m_firewallRuleHits;
    }
    bool firewallAllowsUrl(const QUrl &url) const { return firewallAllows(url); }
    // Adding a rule to one list takes it out of the other.
    bool addFirewallRule(const QString &rule);
    bool removeFirewallRule(const QString &rule);
    bool addFirewallDenyRule(const QString &rule);
    bool removeFirewallDenyRule(const QString &rule);
    void setFirewallPrompt(FirewallPrompt prompt);

    // The test builds drive the whole window, so every poller, badge refresh
    // and thread view runs for real — and used to run against production. A
    // ctest run showed up in the relay's cloud log as a wall of 404s for the
    // fixture repositories the tests invent ("me/wtrepo", "acme/widget"), one
    // pair of hits per fixture per poll (adhoc #1642). Loopback still works,
    // so a test that stands up its own listener is unaffected; anything
    // further away is answered locally and never leaves the machine.
    void setOffMachineRequestsBlocked(bool blocked);
    bool offMachineRequestsBlocked() const { return m_offMachineBlocked; }

    // Offline mode. A machine with no route to the internet answers every
    // request with a timeout eventually, which is the worst of both worlds:
    // the app is unusable for minutes at a time and the log fills with
    // failures that all say the same thing. Blocking off-machine requests here
    // instead answers them immediately and locally, so the pollers, badges and
    // catalog reads that run on timers stop queueing work nobody can finish.
    // Loopback still goes through, so a local relay, the IDE bridge and the
    // agent transports keep working with the network down.
    void setOfflineMode(bool offline);
    bool offlineMode() const { return m_offlineMode; }

    // Version gate. A build older than the minimum supported release is not
    // allowed off this machine at all: every request and every WebSocket
    // connection is refused here, locally, and the reader is told to update
    // instead of watching calls fail one at a time against a relay that no
    // longer answers them the way that build expects. Loopback still goes
    // through, on the same reasoning as offline mode -- a local relay, the IDE
    // bridge and the agent transports are this machine talking to itself.
    // The running version is set from outside because FORKMESH_VERSION lives
    // in a generated header only the few TUs that report the version include,
    // and because a test has to stand an old build up without rebuilding.
    void setClientVersion(const QString &version);
    QString clientVersion() const { return m_clientVersion; }
    void setMinimumNetworkVersion(const QString &minimum);
    QString minimumNetworkVersion() const { return m_minimumNetworkVersion; }
    bool blockedByClientVersion() const { return m_clientVersionBlocked; }

    static bool isOnThisMachine(const QUrl &url);
    QList<EndpointStats> endpointStats() const;
    // Requests grouped by destination host, busiest first, each with what the
    // firewall rules say about that host right now. The Stats page charts it
    // live, so a rule change recolours a destination without new traffic.
    struct DestinationStats {
        QString host;
        QUrl sampleUrl;
        int calls = 0;
        int allowed = 0;
        int blocked = 0;
        qint64 lastCalledMs = 0;
        bool onThisMachine = false;
        FirewallMatch match = FirewallUnlisted;
    };
    QList<DestinationStats> destinationStats() const;
    QList<RequestRecord> endpointRequests(const QString &method,
                                          const QString &endpoint) const;
    bool hostInCooldown(const QString &host) const;
    qint64 hostCooldownRemainingMs(const QString &host) const;

    static constexpr QNetworkRequest::Attribute kBackoffSuppressedAttribute =
        QNetworkRequest::User;
    static bool isBackoffSuppressed(const QNetworkReply *reply);

    // The one request offline mode lets out: the reachability probe is how the
    // app finds out the network came back, so blocking it would make automatic
    // offline mode a one-way door.
    static constexpr QNetworkRequest::Attribute kReachabilityProbeAttribute =
        static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 1);
    static void markReachabilityProbe(QNetworkRequest &request);
    static bool isReachabilityProbe(const QNetworkRequest &request);

    // Set on the replies offline mode answered itself, so a caller can tell
    // "we never sent this" apart from "the relay failed it". NetworkReplyError.h
    // repeats the value as forkmesh::kOfflineBlockedAttribute (it words such a
    // failure and cannot include this header: moc mis-parses the raw string
    // literals in that one and loses the namespace). A static_assert in
    // BackoffNetworkAccessManager.cpp keeps the two agreeing.
    static constexpr QNetworkRequest::Attribute kOfflineBlockedAttribute =
        static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 2);
    static bool isOfflineBlocked(const QNetworkReply *reply);
    // User + 3 is kExpectedHttpStatusAttribute and User + 4 is
    // kFaviconProbeAttribute (NetworkReplyError.h).

    // Set on the replies the version gate answered itself, so a caller can
    // tell "this build is retired" apart from "the relay refused us".
    // NetworkReplyError.h repeats the value as
    // forkmesh::kOutdatedClientBlockedAttribute for the same reason as the
    // offline attribute above, and a static_assert in
    // BackoffNetworkAccessManager.cpp keeps the two agreeing.
    static constexpr QNetworkRequest::Attribute
        kOutdatedClientBlockedAttribute =
            static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 5);
    static bool isOutdatedClientBlocked(const QNetworkReply *reply);

    // Rules, as typed or stored:
    //   *                        every destination
    //   scheme:https             every request with that scheme
    //   host, host:port          one host, any path
    //   *.domain                 the domain and every subdomain
    //   host/path, host/path/*   one path, or it and everything below it, on
    //                            any scheme and with any query; * matches
    //                            anything, a scheme (https://host/api/*)
    //                            narrows it, and the host may be *.domain
    //   https://host/path?query  one exact URL
    static QString canonicalFirewallRule(const QString &rule);
    // Orders matching rules: a named host beats a wildcard host, a port beats
    // none, then an exact path beats a wildcard path, then the longer literal
    // path wins, then a named scheme.
    static qint64 firewallRuleSpecificity(const QString &rule);
    // host/path for the URL's path, or host/dir/* for everything beside and
    // below it. Empty when the URL has no path to speak of.
    static QString firewallRuleForPath(const QUrl &url, bool andBelow);
    // A stored rule in the form a person types it back in.
    static QString firewallRuleEditText(const QString &rule);
    // One entry of a shipped rule list:
    //   {"rules": [{"rule": "app.forkmesh.com/api/accounts/*", "purpose": "..."}]}
    struct FirewallRuleEntry {
        QString rule; // canonical
        QString purpose;
    };
    // Reads a rule list, canonicalizing each rule and dropping duplicates.
    // Entries that are not valid rules are skipped and named in *error.
    static QList<FirewallRuleEntry> parseFirewallRuleList(const QByteArray &json,
                                                          QString *error = nullptr);

    // What one kind of network call is for and how much of it is reasonable,
    // from the shipped endpoint catalog. `pattern` uses firewall rule syntax
    // (app.forkmesh.com/api/repo/*/issues*), so the same matcher and "most
    // specific wins" ordering apply.
    struct EndpointProfile {
        QString pattern;     // canonical rule
        QString method;      // GET, POST, WS...; empty matches any
        QString description; // what the call does, in plain words
        int importance = 0;  // 1 (cosmetic) .. 10 (the app does not work without it)
        QString trigger;     // what fires it: a timer, startup, a click, an event
        double modestPerHour = 0; // a reasonable ceiling while the app is open
        QString alternative;      // push, event or refresh path that could replace it
        int requestNeeded = 0;    // 0..100: how likely it truly has to be a request
        // Where the call is made, repo-relative, "path: Class::function".
        QStringList sources;
    };
    // Reads {"endpoints": [...]}, canonicalizing patterns. Entries with an
    // invalid pattern, no description or out-of-range scores are skipped and
    // named in *error.
    static QList<EndpointProfile> parseEndpointCatalog(const QByteArray &json,
                                                       QString *error = nullptr);
    // The most specific profile for a call, -1 when none matches. A profile
    // naming the method beats one that does not.
    static int matchEndpointProfile(const QList<EndpointProfile> &profiles,
                                    const QString &method, const QUrl &url);
    // Calls per hour since the endpoint was first called, over at least ten
    // minutes so one fresh call does not read as a storm.
    static double observedCallsPerHour(const EndpointStats &stats, qint64 nowMs);
    static QString firewallRuleForUrl(const QUrl &url, bool includePort = false);
    static QString firewallRuleForExactUrl(const QUrl &url);
    static QString firewallMethodName(Operation op);
    static QString firewallRuleLabel(const QString &rule);
    // The wildcard the prompt suggests for a host: "*.forkmesh.com" for
    // app.forkmesh.com, "*.example.co.uk" for a.example.co.uk. Empty for a
    // literal address or a single-label name, which have no parent domain.
    static QString suggestedFirewallWildcard(const QUrl &url);
    // The rules the prompt offers for one request, narrowest first: the exact
    // URL, the path, each folder above it, the host, the host's parent
    // domains, then the scheme. Every rule covers the URL. A catalog pattern,
    // when given, joins the path rules or marks the suggestion it equals.
    struct FirewallRuleSuggestion {
        QString rule;        // canonical
        QString scope;       // "This path", "Everything under /api/"
        QString explanation; // what else it lets through or blocks
    };
    static QList<FirewallRuleSuggestion> firewallRuleSuggestions(
        const QUrl &url, const QString &catalogPattern = QString());
    static bool firewallRuleMatches(const QString &rule, const QUrl &url);

signals:
    void firewallRequestDecided(const QString &method, const QUrl &url,
                                const QString &rule, bool allowed);
    // One per call the version gate refused, so the window can raise the
    // update popup the first time something actually wanted the network.
    void outdatedClientBlockedRequest(const QString &method, const QUrl &url);
    void endpointStatsChanged();
    void firewallRuleHitsChanged();

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request,
                                  QIODevice *outgoingData = nullptr) override;
    virtual QNetworkReply *createNetworkRequest(Operation op,
                                                const QNetworkRequest &request,
                                                QIODevice *outgoingData);

private:
    static int normalizedPort(const QUrl &url);
    static QString normalizedHost(const QString &host);
    static QString endpointStatsKey(Operation op, const QUrl &url);
    static QString endpointStatsUrl(const QUrl &url);
    bool firewallAllows(const QUrl &url) const;
    EndpointStats &endpointStatsFor(Operation op, const QUrl &url);
    int noteEndpointAttempt(Operation op, const QUrl &url,
                            const QString &requestData);
    void noteEndpointAllowed(Operation op, const QUrl &url);
    void noteEndpointBlocked(Operation op, const QUrl &url,
                             const QString &reason, int requestIndex);
    void noteEndpointFinished(Operation op, const QUrl &url, qint64 uploaded,
                              qint64 downloaded, int status,
                              const QString &error, int requestIndex,
                              const QString &responseData);

    NetworkBackoff m_backoff;
#ifdef FORKMESH_WINDOW_TESTS
    bool m_offMachineBlocked = true;
#else
    bool m_offMachineBlocked = false;
#endif
    bool m_offlineMode = false;
    QString m_clientVersion;
    QString m_minimumNetworkVersion = forkmesh::minimumNetworkVersion();
    bool m_clientVersionBlocked = false;
    bool m_firewallEnabled = false;
    QStringList m_firewallRules;
    QStringList m_firewallDenyRules;
    FirewallPrompt m_firewallPrompt;
    QHash<QString, EndpointStats> m_endpointStats;
    QHash<QString, FirewallRuleHits> m_firewallRuleHits;
};

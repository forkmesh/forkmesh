#include "AgentWorkspaceNameGate.h"

#include <QJsonObject>

#include <iostream>

using forkmesh::agents::WorkspaceNameGate;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

int main()
{
    {
        WorkspaceNameGate gate;
        int sent = 0;
        for (int i = 0; i < 150; ++i)
            sent += gate.tryBegin(1000) ? 1 : 0;
        check(sent == WorkspaceNameGate::kMaxInFlight,
              "a burst of launches sends at most kMaxInFlight naming requests");
        gate.finish(2000, 200, true, QJsonObject{{"ok", true}}, 0);
        check(gate.tryBegin(2000),
              "a finished request frees its slot for the next launch");
    }
    {
        WorkspaceNameGate gate;
        check(gate.tryBegin(0), "the first request is sent");
        gate.finish(10, 429, false,
                    QJsonObject{{"error", "rate_limited"},
                                {"retryAfterMs", 450089}},
                    451000);
        check(!gate.tryBegin(10 + 450088),
              "a 429 holds every request until retryAfterMs has passed");
        check(gate.tryBegin(10 + 450089),
              "requests resume once retryAfterMs has passed");
    }
    {
        WorkspaceNameGate gate;
        gate.tryBegin(0);
        gate.finish(0, 429, false, QJsonObject(), 30000);
        check(!gate.tryBegin(29999) && gate.tryBegin(30000),
              "without retryAfterMs the Retry-After header decides");
    }
    {
        WorkspaceNameGate gate;
        gate.tryBegin(0);
        gate.finish(0, 0, false, QJsonObject(), 0);
        check(!gate.tryBegin(WorkspaceNameGate::kFailureCooldownMs - 1) &&
                  gate.tryBegin(WorkspaceNameGate::kFailureCooldownMs),
              "a timed-out request pauses naming for kFailureCooldownMs");
    }
    {
        WorkspaceNameGate gate;
        gate.tryBegin(0);
        gate.finish(0, 429, false,
                    QJsonObject{{"retryAfterMs", 1e12}}, 0);
        check(gate.blockedUntilMs() == WorkspaceNameGate::kMaxCooldownMs,
              "an absurd retryAfterMs is capped");
    }
    if (failures == 0)
        std::cout << "agent workspace name gate: all checks passed\n";
    return failures == 0 ? 0 : 1;
}

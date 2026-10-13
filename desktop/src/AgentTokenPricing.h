#pragma once

#include <QString>

// Approximate list prices used for on-device cost estimates. These are not
// billing records: ForkMesh never sees the provider's invoice, so every USD
// figure derived from this table is labelled as an estimate in the UI.
struct AgentTokenPrice {
    double inputPerMillion = 0.0;
    double outputPerMillion = 0.0;
};

inline AgentTokenPrice agentTokenPriceFor(const QString &provider,
                                          const QString &model)
{
    const QString m = model.toLower();
    if (provider == QLatin1String("cloudflare-ai") ||
        provider == QLatin1String("local-llama"))
        return {0.0, 0.0};
    if (provider.startsWith(QLatin1String("claude"))) {
        if (m.contains(QLatin1String("haiku")))
            return {1.0, 5.0};
        if (m.contains(QLatin1String("sonnet")))
            return {3.0, 15.0};
        return {5.0, 25.0};
    }
    return {1.25, 10.0};
}

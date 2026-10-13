# Workflows: Analytics

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 5 steps to the Add step menu:

- Mixpanel: Track events in Mixpanel.
- Segment: Send track and identify calls to Segment.
- Amplitude: Send events to Amplitude.
- Plausible: Read aggregate stats from Plausible.
- Google Analytics: Send events to GA4 with the Measurement Protocol.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.

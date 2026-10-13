# name: Build a workflow
# description: Turn a repeated task into a ForkMesh workflow, an action or a special agent, and leave it ready to save.

Find one piece of work in this repository or organization that people repeat by hand: a report, a notification, a sync between two tools, a check before a release.

1. Describe the trigger (a button, a webhook, a schedule, a push, a pull request or a special agent's schedule) and every step after it.
2. Decide where it belongs:
   - an automation in ForkMesh Workflows when it calls outside services (Slack, GitHub, Stripe, Claude and the rest);
   - a `.forkmesh/*.yml` action when it runs commands on the repository;
   - a special agent in `.forkmesh/teammates/` when it needs judgment on a schedule or on events.
3. Build it in /dashboard/workflows. For a repository file, open the repository under Repository, add the steps on the canvas and use View file before saving.
4. Never put an API key, token or password in a repository file. Automations keep them as encrypted credentials; actions read `${{ vars.NAME }}` from the desktop.
5. Report what you built, where it lives, how to turn it on, and anything you could not verify.

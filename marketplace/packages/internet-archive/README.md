# Internet Archive

Internet Archive backs up a public ForkMesh repository to an
[archive.org](https://archive.org) item with your own archive.org S3 keys,
every day, week or month, and only when something changed. Each upload is one
file, a Git bundle of every branch and tag that anyone can restore with
`git clone <repo>.bundle`.

That copy is held by a third party that no ForkMesh mirror can change, so the
plugin also uses it as a witness. Its page compares the source and every
mirror with the archived backups and draws the result: which copies match the
latest backup, which serve an older one, which moved on since, and which
differ from everything that was ever archived. When the source is offline
and only mirrors remain, that tells you which copies still serve the real
repository. The page also lists the Git commands to check it yourself
without ForkMesh.

Removing the plugin stops every backup you set up and deletes the stored
keys. Nothing already on archive.org is removed.

Developer notes live in
[`docs/marketplace/internet-archive.md`](../../../docs/marketplace/internet-archive.md).

# Discourse Mirror

Discourse Mirror keeps a read-only copy of a public
[Discourse](https://www.discourse.org) forum in a Git repository. Push that
repository to ForkMesh and every mirror that carries it carries the forum too,
so its topics stay readable when the forum is down or blocked where a reader
lives.

Each run copies only what changed: new topics, new replies and topics the
forum bumped, plus a few older topics re-read for edits and removals. A topic
the forum removed leaves the copy on the next pass. The copy holds the forum's
posts as JSON and a set of plain HTML pages (`site/index.html`) that load
nothing from the network: pictures are copied beside them, and links to
anything else go to the forum.

The forums to copy are a setting on the computer that runs the copy, not part
of this package. Copy a forum only with its operators' agreement; posts stay
their authors' under the forum's terms.

Developer and operator notes live in
[`docs/marketplace/discourse-mirror.md`](../../../docs/marketplace/discourse-mirror.md).

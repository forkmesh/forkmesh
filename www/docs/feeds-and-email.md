# Subscribing by feed or email

Every public repository's issues can be followed without an account, without
JavaScript and without loading anything from another site: as Atom feeds, by
email through a feed-to-mail tool, or on the plain "lite" pages.

## Feeds

Replace `<host>/<owner>/<repo>` with the repository's address, for example
`https://forum.example.org/example/forum`.

| Feed | Address |
| --- | --- |
| New threads | `https://<host>/<owner>/<repo>/issues.atom` |
| New threads and every new comment | `https://<host>/<owner>/<repo>/activity.atom` |
| One thread and its comments | `https://<host>/<owner>/<repo>/issues/<number>.atom` |
| New threads with one label | `https://<host>/<owner>/<repo>/labels/<label>.atom` |

A label with a space is written with `%20`, for example
`labels/reading%20group.atom`. Feeds hold the newest 50 entries; add
`?limit=200` for up to 200. Comments carry the Atom threading extension
(RFC 4685), so feed readers and mail tools show them as replies to their
thread. The issue list and every issue page also announce their feeds, so
pasting the page address into a feed reader finds them.

## By email with rss2email

[rss2email](https://github.com/rss2email/rss2email) turns each new entry into
an email. Install it (`pip install rss2email` or your system's package), then:

```
r2e new you@example.org
r2e add forum-activity https://forum.example.org/example/forum/activity.atom
r2e run --no-send
r2e run
```

`r2e run --no-send` once marks what is already there as read, so the first
real run mails only new threads and comments. Run `r2e run` from cron (for
example every 30 minutes) to keep receiving them. To follow fewer messages,
subscribe to new threads only, or to one label:

```
r2e add forum-threads https://forum.example.org/example/forum/issues.atom
r2e add forum-iran https://forum.example.org/example/forum/labels/Iran.atom
```

Each mail's subject is the thread title (`Re:` and the author for a comment)
and its body is the post. Images that are not stored on the instance appear
as links, so opening a mail never contacts another site.

## The lite view

`https://<host>/<owner>/<repo>/lite` lists the threads, 50 to a page, newest
first (`?sort=active` for the most recently active, `?label=Iran` for one
label). `https://<host>/<owner>/<repo>/lite/issues/<number>` shows a whole
thread on one page. The pages are plain HTML with no scripts and no outside
fonts or images, made for slow or filtered connections such as DNS tunnels;
a thread list page is under 30 KB. `https://<host>/lite` lists the
instance's public repositories. Every normal page links to its lite version
for readers without JavaScript, and the sidebar has a "Lite" link.

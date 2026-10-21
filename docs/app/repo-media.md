# Videos in issue and pull request markdown

People asked to attach screen recordings to issues and pull requests. Images
were already possible on the web, but only as base64 `data:` URLs inside the
signed body, which the relay caps at 64 KB (`MAX_ISSUE_BYTES`); a video does
not fit, nothing served media with Range support (seeking, and Safari playing
at all), and the renderer dropped `<video>`.

A video is now uploaded once to the cores' replicated database and linked from
the markdown like an image:

```
![demo.mp4](/api/repo/<owner>/<repo>/media/<id>.mp4)
```

Code: `app/src/repo_media_api.py` (pure, behind `_RepoMediaRuntime` in
`app/src/entry.py`), dispatched from `app/src/app_runtime.py`; tables
`repo_uploads` and `repo_upload_parts` (migration `0282_repo_uploads`). Tests:
`app/tests/test_repo_media_api.py`.

The tables are not called `repo_media`: that name already holds repository
logos and banners (migration 0030), and `CREATE TABLE IF NOT EXISTS` would have
kept that table and then failed on indexes for columns it does not have.
`test_upload_tables_do_not_reuse_an_existing_table_name` guards every table
name.

## Why the database

The served stack has no object store. The chat's view-once video
([chat-private-rooms.md](chat-private-rooms.md)) already stores media as
512 KiB parts in the rqlite-backed D1 that every core replicates, with quotas
and a sweep; this reuses that shape. Committing videos into the repository
would make every clone download them (`.forkmesh/issues` and the pull ledger
are ordinary branches), and peers replicate only `refs/heads` and `refs/tags`,
so a hidden ref would not reach the other cores.

Parts of public media are stored as plain base64, not sealed with `DATA_KEY`:
they are served to anyone, and a range read would otherwise decrypt up to
4 MiB per request. The row's `data` (owner, repository, uploader, file name)
is sealed like every other row.

## API

| Route | Who | Does |
| --- | --- | --- |
| `POST /api/repo/<o>/<r>/media` `{name, mime, size, parts}` | signed-in account with a verified email | Creates an `uploading` row; public repositories only |
| `PUT .../media/<id>/parts/<n>` `{data}` | uploader | Stores one 512 KiB part (base64); part 0 must start like the declared type |
| `POST .../media/<id>/finish` | uploader | Checks every part arrived, marks `ready`, answers `url` |
| `GET` / `HEAD .../media/<id>[.<ext>]` | anyone | Serves a `ready` video while its repository is public |
| `DELETE .../media/<id>` | uploader | Deletes the parts, marks `removed` |

- Types: `video/mp4`, `video/webm`, `video/quicktime`, checked against the
  first bytes (`ftyp`, the EBML header, or a QuickTime atom), so an HTML or
  script file cannot be stored as a video.
- Limits: 25 MiB per video; per account 20 uploads and 200 MiB in any 24
  hours (`media_quota_count` / `media_quota_bytes`). An upload not finished
  within an hour is removed by the hourly `chat-prune` phase and by the next
  create.
- Serving: one `bytes=` range per request (206, `Content-Range`), open-ended
  ranges capped at 4 MiB, `416` past the end, `Accept-Ranges: bytes`. Headers
  `nosniff`, `Content-Disposition: inline`, `Content-Security-Policy:
  default-src 'none'; sandbox`, `Cross-Origin-Resource-Policy: same-site`,
  `Cache-Control: public, max-age=31536000, immutable`. A repository made
  private stops serving its media (`public_repository` is checked on every
  read).

## Web

- `03-issues-profile-io.js`: `uploadMarkdownVideo` (create, parts, finish),
  `insertMarkdownVideo` (an "Uploading … N%" placeholder at the cursor,
  replaced by the link). Paste, drag-and-drop and the **Attach video** button
  (`markdownVideoButtonHtml`) work in the new issue and new pull request
  forms, issue comments, pull request reviews, comment and description edits,
  and discussion replies.
- `06-repo-content.js` `mdImageHtml` and its server-render twin `image_html`
  in `app/host/forkmesh_host/repo_ssr_issue.py` render an image whose URL is
  an uploaded video (`MD_VIDEO_URL`) as `<video controls preload="metadata"
  playsinline>`. Other image URLs are unchanged. The page CSP already allows
  `media-src 'self'`.

## Not done yet

- Private repositories cannot hold videos.
- The desktop and the phones show the link, not a player, and cannot upload.
- Media is never deleted when the issue or comment that links it is deleted;
  the uploader can remove it.

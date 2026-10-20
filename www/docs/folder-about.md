# Folder About boxes

A repository page shows an **About** box on the right: description, website
and links. Any folder in a repository can have its own About box too. This is
useful for monorepos that keep one project per folder: opening the folder on
the web shows that project's name, logo, description, website, links and
topics instead of an empty right-hand side.

## Add an About box to a folder

Commit a file named `.forkmesh/info.json` inside the folder. It is the same
file name the repository uses for its own About (`.forkmesh/info.json` at the
top of the repository), so one convention covers both.

```
bolt-c/
  .forkmesh/info.json
  logo.svg
  README.md
  src/
```

`bolt-c/.forkmesh/info.json`:

```json
{
  "name": "Bolt-C",
  "about": "Track and monitor GitHub pull request readiness.",
  "website": "https://leaf.example.net/",
  "links": [
    {"label": "Source", "url": "https://code.example.org/Bolt/Bolt-C"},
    {"label": "Contributing", "url": "https://code.example.org/Bolt/Bolt/blob/main/bolt-c/CONTRIBUTING.md"}
  ],
  "topics": ["security", "github", "cloudflare-workers"]
}
```

Every field is optional; an empty object (`{}`) is enough to turn the box on.

| Field | Meaning | Limits |
| --- | --- | --- |
| `name` | Title of the box | 80 characters; defaults to the folder name |
| `about` | One-paragraph description (`description` is accepted too) | 240 characters |
| `website` | Project website (`homepage` is accepted too) | `http` or `https` only, 240 characters |
| `links` | List of `{"label", "url"}` rows | up to 8; `url` must be `http`/`https`, 400 characters; `label` 60 characters, defaults to the address's host |
| `topics` | List of topic words (`tags` is accepted too) | up to 20; lowercased, spaces become `-`, letters, digits and `-` only, 50 characters each |

Text is shown as plain text: HTML and Markdown are not interpreted. Links
with any other scheme (`javascript:`, `data:`, `ftp:`), with a user name or
password in the address, or with spaces are dropped. A field of the wrong
type is ignored. A file that is not valid JSON, or larger than 64 KB, still
shows the box, but with only the fallbacks below.

## Logo

Put `logo.svg` or `logo.png` in the folder itself (not in `.forkmesh/`). If
both exist, the SVG is used. The logo appears at the top of the folder's About
box and next to the folder path above the file list.

- Up to 512 KB.
- A PNG must be a real PNG file.
- An SVG must start with an `<svg>` element (an XML declaration, comments or a
  doctype may come first) and may not declare entities.
- The logo is always shown as an image, never inlined into the page, so
  scripts inside an SVG never run.

## Fallbacks from the folder's README

When the About file has no `about`, the description is the first paragraph of
prose in the folder's `README.md` (also `readme.md`, `Readme.md`,
`README.markdown`, `README.txt` or `README`). Headings, HTML blocks, images,
badges, code blocks, lists and lines like `**Live Site:** ...` are skipped;
links keep their text. It is cut to 240 characters.

When the About file has no `website`, a README line such as
`**Live Site:** https://...`, `**Website:** ...` or `**Homepage:** ...`
supplies it.

## Monorepos with project addresses

On an instance whose administrator set up project addresses (one address per
folder, such as `/Bolt/Bolt-C`), every folder in that project table gets an
About box without committing an About file: its name comes from the table, and
the description, website and logo come from the README and logo files above.
A committed `.forkmesh/info.json` still wins for every field it sets. The web
page looks for a folder About when the folder contains a `.forkmesh` folder,
`logo.svg` or `logo.png`, so a project folder needs one of them.

## Contributor roster

The repository page's Contributors section counts git author names, so bots
are left out but one person with several commit names still counts more than
once, and there are no pictures. To show your own list, commit
`.forkmesh/contributors/contributors.json` at the repository root:

```json
{
  "count": 2,
  "contributors": [
    {"slug": "alice", "name": "Alice", "commits": 120, "profile": "https://example.com/@alice"},
    {"slug": "bob", "name": "Bob", "commits": 40}
  ]
}
```

and a square picture for each person at `.forkmesh/contributors/<slug>.png`.
The page shows `count`, the eight people with the most `commits` (name and
commit count on hover, a link to `profile` when it is an `https://` address,
initials when there is no picture), and a "+N" link to the file.

## What stays the same

Folders without an About file (and not in a project table) look exactly as
before. The repository's own About box on the main page is unchanged; inside a
folder that has an About, the folder's box takes the right-hand column.
Private repositories show folder About boxes and logos only to people who can
read the repository.

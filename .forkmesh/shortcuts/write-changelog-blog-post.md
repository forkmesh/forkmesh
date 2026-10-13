# name: Write the next changelog blog post
# description: Publish a polished product update from everything landed since the previous changelog post, with original artwork matching the blog.
# agent: claude-code
# model: claude-opus-5-5

Publish the next ForkMesh changelog blog post as a complete, production-ready change to this repository. This is implementation work: when permissions allow the requested changes, do not stop after producing a plan or briefing-only summary.

First establish the reporting window from repository evidence. Find the newest changelog-style product update in `www/public/blog.html` (currently the newest dated card in the Product updates section), locate the commit that published it, and inspect every meaningful change from that publication commit through `HEAD`. Use Git history and diffs, merged pull-request records, closed issues, releases, and relevant documentation as evidence. Do not infer shipped work from titles alone, include changes already covered by the previous post, or turn the article into a raw commit list. Group the important user-facing progress into a coherent story and be precise about what is live versus experimental or still in progress.

Then implement the post in the existing blog, not as a standalone Markdown draft:

- Follow the article structure, typography, metadata, social-sharing block, and tone of the latest product-update posts under `www/public/blog/`.
- Choose a concise editorial title, slug, lede, description, publication date, and honest reading time. Write clear, warm prose with useful technical specificity and strong section headings. Avoid hype, filler, and repetitive “we added” phrasing.
- Create an original 16:9 hero image that visually summarizes this reporting window and matches the blog's existing cinematic, nocturnal, green/cyan ForkMesh illustration language. Use the image-generation capability available to you; when your agent has no built-in image tool, pipe the prompt to `codex exec -i <reference.webp> -` with its image generation, as the special-agent portraits were made. Inspect the existing files in `www/public/assets/blog/features/` as visual references and pass two or three of them as attachments. Do not reuse an old hero, paste UI screenshots together, or imitate a third-party copyrighted work. Optimize the final asset as WebP and save it at `www/public/assets/blog/features/<slug>.webp` with accurate alt text and social image dimensions.
- Use `www/public/assets/logo.png` as the official ForkMesh mark reference. Show the mark faithfully in the hero and include a vertically mirrored reflection of that same logo, matching the established wet-ground treatment in recent product-update artwork.
- Add the complete article at `www/public/blog/<slug>/index.html` and integrate it as the newest Product updates card and search result in `www/public/blog.html`. Keep dates, canonical/Open Graph/Twitter metadata, image paths, excerpts, keywords, previous/next navigation, RSS parsing conventions, and social placeholders consistent with the existing site.
- Update the former newest post's next/previous navigation where needed so readers can move chronologically between posts. Preserve unrelated cards and feature articles.
- Extend any count- or content-sensitive blog tests so the new post, image, index entry, and navigation are covered.

Before finishing, inspect the rendered page at desktop and mobile widths if browser tooling is available, run the focused blog frontend/RSS/social tests, and correct any failures. Report the evidence window you used, the story themes you chose, the files created or updated, image dimensions/format, and test results.

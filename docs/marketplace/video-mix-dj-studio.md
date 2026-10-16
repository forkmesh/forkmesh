# Video Mix DJ Studio

`video-mix-dj-studio` is an account-synced Marketplace package with a responsive dashboard page at `/dashboard/video-mix-dj-studio` and a desktop rail link. It can be shown on mobile, desktop and web; phones open the page through the package’s web surface.

The studio accepts a YouTube URL or video id for each deck, embeds the selected videos through YouTube’s privacy-enhanced host, and uses the player message protocol for transport, position, tempo and volume. The equal-power crossfader sets each player’s effective volume and its matching opacity, so sound and picture make the same transition. Cues, eight-second loops, deck level, tempo and the crossfader are retained only in `localStorage`; no package API or worker is present.

The page is intentionally controlled by the selected video’s availability policy. YouTube advertising, embedding rules and any playback restrictions remain under YouTube and the video owner’s control.

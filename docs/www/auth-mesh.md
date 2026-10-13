# Auth backdrop: the dotted world map behind the login card

The login, signup, forgot-password, reset-password and verify-pending pages
mount `<div data-forkmesh-auth-mesh>`, which `www/public/auth-mesh.js`
replaces with a dotted world map carrying the mesh's nodes: a small dot per
city, and a teardrop portrait pin for the four nodes that have a face. The
backdrop is decorative: it is `aria-hidden`, takes no pointer events, and a
page without JavaScript simply shows its plain background.

On 2026-09-27 the map was rebuilt after the Cal.com login the page design was
taken from. The old map was a hand-drawn 157 × 69 grid (`mesh-map.svg`) whose
continents were blobs; making it denser only made the blobs finer, so the map
is now drawn from Natural Earth coastlines.

## Code map

| Path | Responsibility |
| --- | --- |
| `www/public/auth-mesh.js` | Builds the backdrop, draws the map on a canvas, places nodes and pins |
| `www/public/auth-mesh.css` | Backdrop layout, the theme colours of dots, nodes and pins |
| `www/public/assets/land-mask.png` | 1440 × 720 equirectangular land mask the map is drawn from |
| `app/tools/build_land_mask.py` | Generates the land mask from Natural Earth TopoJSON |
| `app/tests/test_auth_mesh_frontend.py`, `app/tests/test_build_land_mask.py` | Contracts for the backdrop and the generator |

## Architecture

- **Map.** A canvas covers the backdrop and is drawn from `land-mask.png`:
  one dot every 6 CSS pixels at every viewport size, radius 1.35px, in six
  alpha shades chosen by smooth value noise so the dots have the reference's
  uneven texture. The dot colour is the CSS custom property `--auth-mesh-dot`,
  read on each draw. The canvas redraws on resize (`ResizeObserver`) and when
  `site-header.js` changes the theme class on `<html>` (`MutationObserver`),
  and fades in once drawn. Its backing store is capped at twice the CSS size
  and 16 million pixels, below the iOS Safari canvas limit.
- **Projection.** Mercator from 78°N to 57°S, centred on 4°E. The stage keeps
  that aspect ratio (set from JavaScript as `--auth-mesh-aspect`) and covers
  the backdrop, at least 80rem wide. The centred card sits over the Atlantic,
  Europe and Africa, with the Americas to its left and Asia and Australia to
  its right, as in the reference. The 80rem floor keeps the Santiago and
  Bengaluru pins clear of a 480px card down to about a 1024px viewport.
- **Nodes.** Each node is a latitude and longitude projected with the same
  constants, drawn as a 6px dot ringed in the page colour; six of them carry a
  slow ping. The four portrait nodes also get a pin standing on their dot.
  Nodes marked `wide` appear from 1180px.
- **Header.** The backdrop starts below the 3.5rem site header, and the header
  keeps its own solid background (white in light mode). It used to be made
  transparent over the map, which the user did not want once the page's green
  top glow, which had set the bar apart, was removed.

## Regenerating the land mask

The mask is Natural Earth's 1:50m land (public domain), as packaged in
TopoJSON by `world-atlas`:

```sh
curl -sSLo land-50m.json https://cdn.jsdelivr.net/npm/world-atlas@2.0.2/land-50m.json
python3 app/tools/build_land_mask.py land-50m.json
```

The generator needs Pillow. It rasterises at 4× supersampling, keeps cells
that are at least half land, cuts everything south of 60°S, and writes a
1-bit PNG with 0.25° cells (about 11 KB) to `www/public/assets/land-mask.png`.
The data file itself is not committed.

## Traps

- A ring that crosses ±180° jumps from one edge of the map to the other; drawn
  as-is it paints a band across the whole Arctic. The generator unwraps each
  ring's longitudes and draws it at −360, 0 and +360 degrees.
- `styles.css` sets `svg { fill: none; stroke: currentColor; }` globally. The
  old backdrop drew nodes as SVG circles, which inherited a grey stroke scaled
  with the map and rendered as grey discs. Nodes are plain elements now; give
  any SVG drawn over the backdrop an explicit fill and stroke, as the pins do.
- Headless Chrome will not make a window narrower than about 500px, so
  `--window-size=390,…` screenshots show the card cut off. Check phone layouts
  with device emulation instead.

## Recent changes

- 2026-09-27: the map is drawn on a canvas from Natural Earth land at a 6px
  pitch, nodes are placed from coordinates, `mesh-map.svg` is gone, and the
  site header stays solid over auth pages.

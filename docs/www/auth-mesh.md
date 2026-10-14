# Auth backdrop: the dotted world map behind the login card

The login, signup, forgot-password, reset-password and verify-pending pages
mount `<div data-forkmesh-auth-mesh>`, which `www/public/auth-mesh.js`
replaces with a dotted world map carrying the mesh's nodes. The backdrop is
decorative: it is `aria-hidden`, takes no pointer events, and a page without
JavaScript shows its plain background.

## Code map

| Path | Responsibility |
| --- | --- |
| `www/public/auth-mesh.js` | Builds the backdrop, draws the map on a canvas, places nodes and pins |
| `www/public/auth-mesh.css` | Backdrop layout, theme colours of dots, nodes and pins |
| `www/public/assets/land-mask.png` | 1440 × 720 equirectangular land mask the map is drawn from |
| `app/tools/build_land_mask.py` | Generates the land mask from Natural Earth TopoJSON |
| `app/tests/test_auth_mesh_frontend.py`, `app/tests/test_build_land_mask.py` | Contracts for the backdrop and the generator |

## Architecture

- **Map.** A canvas drawn from `land-mask.png`: one dot every 6 CSS pixels,
  radius 1.35px, in six alpha shades chosen by smooth value noise. The dot
  colour is `--auth-mesh-dot`, read on each draw. It redraws on resize
  (`ResizeObserver`) and on theme changes (`MutationObserver` on `<html>`).
  The backing store is capped at twice the CSS size and 16 million pixels.
- **Projection.** Mercator from 78°N to 57°S, centred on 4°E. The stage keeps
  that aspect ratio (`--auth-mesh-aspect`) and covers the backdrop, at least
  80rem wide, so the centred card sits over the Atlantic.
- **Nodes.** Latitude/longitude points projected with the same constants,
  drawn as 6px dots; some carry a slow ping or a portrait pin. Nodes marked
  `wide` appear from 1180px.
- **Header.** The backdrop starts below the 3.5rem site header, which keeps
  its own solid background.

## Regenerating the land mask

The mask is Natural Earth's 1:50m land (public domain), as packaged in
TopoJSON by `world-atlas`:

```sh
curl -sSLo land-50m.json https://cdn.jsdelivr.net/npm/world-atlas@2.0.2/land-50m.json
python3 app/tools/build_land_mask.py land-50m.json
```

The generator needs Pillow. It rasterises at 4× supersampling, keeps cells
that are at least half land, cuts everything south of 60°S, and writes a
1-bit PNG with 0.25° cells to `www/public/assets/land-mask.png`. The data
file is not committed.

## Traps

- A ring that crosses ±180° jumps across the map; the generator unwraps each
  ring's longitudes and draws it at −360, 0 and +360 degrees.
- `styles.css` sets `svg { fill: none; stroke: currentColor; }` globally;
  give any SVG drawn over the backdrop an explicit fill and stroke.
- Headless Chrome will not make a window narrower than about 500px; check
  phone layouts with device emulation.

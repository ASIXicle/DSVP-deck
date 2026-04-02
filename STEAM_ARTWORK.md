# Steam Library Artwork Setup

DSVP includes custom artwork for your Steam library. These images use DSVP's native 5×7 bitmap font and match the player's look.

## Files

| File | Size | Purpose |
|------|------|---------|
| `docs/steam_grid_600x900.png` | 600×900 | Library grid tile (portrait capsule) |
| `docs/steam_header_920x430.png` | 920×430 | Library header (wide banner in list view) |
| `docs/steam_hero_1920x620.png` | 1920×620 | Hero banner (top of game page) |

## Setup

1. Copy the three images to your Steam Deck (e.g., `~/DSVP/docs/` or anywhere accessible)
2. Open Steam in **Desktop Mode**
3. Find **DSVP** in your library (must already be added as a non-steam game — see [SteamOS.md](SteamOS.md))
4. Right-click → **Manage** → **Set Custom Artwork**
5. Select the appropriate image when prompted for each type

The artwork will persist across reboots and SteamOS updates. It's stored in Steam's local library cache, not on the filesystem.

## Notes

- You only need to do this once — Steam remembers custom artwork for non-steam games.
- If artwork disappears after a Steam client update, just re-apply the images.
- A future Flatpak release will handle artwork installation automatically via desktop entry metadata and appstream icons.

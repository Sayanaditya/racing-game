# Argon: Overdrive — First-Person Software-Rasterized 3D Racer (C++20 / Qt 6)

A complete CPU software-rasterized first-person pseudo-3D arcade racing game built over C++20 and Qt 6. It implements all 8 phases of the [Implementation Blueprint](Software-Rasterized%20Pseudo-3D%20Racer%20Implementation%20Blueprint%20(C++%20Qt%206).md) plus the full game expansion in [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md).

## New Game Features & Architecture Upgrades

- **Interactive Launch Screen & Full Game Loop (`game.h/.cpp`)**:
  - **Attract-Mode 3D Background**: Live autopilot camera flying through the currently selected track on the Launch Screen.
  - **Interactive Menu**: Keyboard (`W`/`S` or `Up`/`Down`, `A`/`D` or `Left`/`Right`, `Enter`) and mouse hover/click support for selecting Tracks, Game Modes, Pixel Quality, Display Scaling, Fullscreen, and CRT Scanlines.
  - **Starting Grid Countdown (`3... 2... 1... GO!`)**, **Interactive Pause Menu**, and **Post-Race Results / High-Score Celebration Screen**.
- **Variable Pixel Size & High-Resolution 960x720 Engine (`framebuffer.h/.cpp`)**:
  - **Scale3x / EPX Edge-Preserving Upscaler**: Converts the 3D raster canvas to `960x720` with diagonal edge smoothing while rendering HUD text, 2D GPS splines, rally callouts, and menus at native `960x720`.
  - **5 Live Pixel Size Presets (`1`–`5` or `[` / `]` keys)**:
    - `1px Ultra HD (960x720)`
    - `2px High (480x360)`
    - `3px Classic (320x240)`
    - `4px Retro (240x180)`
    - `6px Chunky (160x120)`
- **Fullscreen & Window Scaling Fix (`gamewidget.h/.cpp`)**:
  - **Full Window Stretch (Default)**: Expands to 100% of the window or fullscreen display with zero black borders.
  - **3 Scaling Modes (`L` key)**: `Full Window Stretch`, `4:3 Smooth Aspect Fit`, and `Integer Pixel-Perfect`.
  - **Instant Fullscreen Toggle (`F` or `F11` key, or Menu)**: Switches cleanly between fullscreen and windowed mode.
- **2D GPS Navigation System & Rally Callouts (`track.h/.cpp`, `game.cpp`)**:
  - **Dual-Mode 2D GPS (`Tab` key)**: Rotating player-centric radar (where "Up" matches your current heading) + Full Circuit Overview map, with upcoming road segments colour-coded by turn severity (Green = Straight, Yellow = Medium Bend, Red = Sharp Hairpin), checkpoint markers, and live rival car blips.
  - **Turn-by-Turn Rally Callouts**: Scans 55 segments ahead to display directional warnings (`< EASY LEFT`, `>> MEDIUM RIGHT`, `<<< HARD LEFT!`, `/\ BLIND CREST AHEAD`).
- **5 Distinct Racing Circuits & Biomes (`track.cpp`, `roadrender.cpp`)**:
  1. **Emerald Coast** (*Classic Daylight* — sweeping coastal highway, normal difficulty)
  2. **Neon Mirage** (*Synthwave Sunset* — high-speed sunset freeway, long nitro straights)
  3. **Cyber Tokyo** (*Midnight Expressway* — tight night chicanes, rain weather)
  4. **Alpine Glacier** (*Snowy Mountain Switchbacks* — steep climbs, blind crests, snow weather)
  5. **Crimson Canyon** (*Desert Endurance Gauntlet* — extreme canyon drops and high-speed rivals)
- **Persistent High Scores & Best Lap Records (`highscores.h/.cpp`)**:
  - Automatically loads and saves per-track High Scores, Best Lap Times, Top Speeds, and Overtake records to `highscores.dat`.
  - Live `SCORE`, `HI-SCORE`, Overtake Combo Multiplier (`1X`–`8X`), and 3D overhead Checkpoint & Start/Finish arches.

## Controls

| Key / Input | Action |
| --- | --- |
| `W` / `Up`, `S` / `Down`, `A` / `Left`, `D` / `Right` | Throttle, Brake, Steer Left/Right (or Navigate Menus) |
| `Enter` / `Space` / `Left Click` | Confirm Menu Option / Activate Nitro Boost in Race |
| `1` / `2` / `3` / `4` / `5` or `[` / `]` | Change Pixel Size / Visual Quality (`1px` Ultra `960x720` to `6px` Chunky) |
| `F` / `F11` | Toggle Fullscreen Mode |
| `L` | Cycle Display Scaling (`Full Window Stretch` / `4:3 Aspect Fit` / `Integer`) |
| `Tab` | Cycle GPS Navigation Mode (`Rotating Radar` / `Full Circuit Map` / `Off`) |
| `B` / `V` / `X` / `C` | Cycle Biome (`B`), Rear-View Mirror (`V`), Wipers (`X`), Weather (`C`) |
| `M` / `N` / `P` / `R` / `F3` | Mute (`M`), Music (`N`), Pause Menu (`P`), Restart (`R`), Profiler (`F3`) |

## Building & Running

### 1. Build Qt 6 Application (CMake)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/racer
```

### 2. Run Automated Verification Suite (CLI)

```bash
make test
```

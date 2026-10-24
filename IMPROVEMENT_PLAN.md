# Argon Overdrive: Game Improvement & Feature Expansion Plan

This document outlines the step-by-step engineering plan to evolve the software-rasterized pseudo-3D Qt 6 prototype into a complete, full-featured first-person arcade racing game while strictly preserving all hard constraints (**100% CPU software rasterization, Qt 6 presentation, First-Person perspective, zero steady-state allocations in the core engine, and all 42 Phase 1–8 verification checks passing**).

---

## Step 1: Fullscreen & Window Scaling Fix + Variable Pixel Size / High-Resolution Rasterization

1. **Fullscreen & Dynamic Viewport Scaling (`gamewidget.h/.cpp`)**:
   - **Root Cause Fix**: Remove the restrictive integer `std::floor(s)` letterboxing lock that kept the rendered view inside a small sub-rectangle when maximizing or entering fullscreen.
   - **Scaling Modes (`L` key or Launch Menu)**:
     - `Full Window (Stretch)`: Fills 100% of the window or fullscreen display with zero black bars (default).
     - `Aspect Fit (4:3)`: Scales smoothly to the maximum 4:3 rectangle touching the window edges.
     - `Integer Pixel-Perfect`: Crisp integer-multiple scaling for purist retro monitors.
   - **Fullscreen Toggle (`F` or `F11` key, or Launch/Pause Menu)**: Calls `showFullScreen()` / `showNormal()` seamlessly.

2. **Variable Pixel Size & High-Resolution Quality Engine (`framebuffer.h`, `game.h/.cpp`)**:
   - Introduce a high-resolution software display buffer (`960x720` max resolution) alongside the `320x240` base buffer.
   - Support **5 User-Selectable Pixel Size / Quality Presets** (live-switchable via `1`–`5` or `[` / `]` keys, and configurable on the Launch Screen):
     - **Ultra Crisp (1px / `960x720` effective)**: 3x super-sampled scanline road, smooth curb/lane edges, high-res cockpit gauges, and crisp vector-raster GPS.
     - **High Quality (2px / `480x360` effective)**: Sharp arcade raster look.
     - **Classic Arcade (3px / `320x240` effective)**: Original blueprint resolution.
     - **Retro Chunky (4px / `240x180` effective)**: 90s DOS/Saturn chunky pixel aesthetic.
     - **Lo-Fi 8-Bit (6px / `160x120` effective)**: Ultra-chunky retro mosaic mode.

---

## Step 2: 5 Distinct Racing Tracks & Biomes (`track.h/.cpp`, `roadrender.h/.cpp`)

Build 5 multi-sector tracks with distinct curvature, elevation profiles, scenery placement, traffic density, and colour palettes:
1. **Emerald Coast Highway** (*Classic Daylight*): Sweeping coastal bends, gentle hills, balanced traffic.
2. **Neon Mirage Speedway** (*Synthwave Sunset*): High-speed sunset highway, long nitro straights, rhythm S-curves.
3. **Cyber Tokyo Expressway** (*Cyber Neon Night*): Tight night chicanes, rapid elevation drops, dense highway traffic.
4. **Alpine Glacier Pass** (*Alpine Snow*): Steep mountain grades (`+2400 wu`), blind crests, technical hairpins, snow weather.
5. **Crimson Canyon Gauntlet** (*Desert Canyon Dusk*): Red-rock canyon endurance track with extreme curves, steep plunges, and fast rivals.

---

## Step 3: 2D GPS Navigation System & Rally Turn Callouts (`track.h/.cpp`, `game.h/.cpp`)

1. **Precomputed 2D Track Map Spline**:
   - Integrate segment curvature into 2D `(mapX, mapY)` coordinates across the entire circuit and normalize into a closed loop.
2. **Dual-Mode Cockpit GPS Navigator**:
   - **Rotating Local Radar + Full Circuit Map**: Embedded in the cockpit navigation display (and expandable with `Tab`), showing the road geometry ahead colour-coded by curvature severity (Green = Straight, Yellow = Medium Turn, Red = Sharp Hairpin), player heading arrow, checkpoint gates, and live rival car blips.
3. **Turn-by-Turn GPS Callout Banner**:
   - Scans 60 segments ahead of the player and displays real-time rally callout icons and text (`<< EASY LEFT`, `>> MEDIUM RIGHT`, `<<< HARD HAIRPIN LEFT!`, `/\ CREST AHEAD`).

---

## Step 4: Persistent High Scores & Best Lap Records (`highscores.h/.cpp`)

1. **Per-Track Record Storage (`highscores.dat`)**:
   - Automatically loads at startup and saves on race completion or new record:
     - Highest Score per track
     - Fastest Lap Time per track
     - Top Speed achieved per track
     - Best Overtake count per track
2. **Live Score & Multiplier System**:
   - Score accumulates continuously from speed $\times$ combo multiplier, `+500` per overtake, `+1000` for high-speed near misses, and `+2500` per checkpoint/lap completion.
   - Live `HI-SCORE` indicator in the HUD turns gold when the player surpasses the current track record.

---

## Step 5: Interactive Launch Screen, Game Modes, Countdown & Post-Race Flow (`game.h/.cpp`, `gamewidget.cpp`)

1. **Launch / Title Screen (`GameState::LaunchMenu`)**:
   - Live 3D autopilot camera flying through the currently selected track in the background.
   - Software-rasterized title logo (**`ARGON OVERDRIVE`**), interactive menu with keyboard (`Up`/`Down`/`Left`/`Right`/`Enter`) and mouse support, 2D GPS track preview card, track stats, and saved High Score / Best Lap display.
2. **Game Modes**:
   - **Arcade Checkpoint Mode**: Race against the countdown clock (`+15s` at each checkpoint arch).
   - **Grand Prix Mode (3 Laps)**: Race 24 opponents from P25 to P1 over 3 laps.
   - **Free Run Mode**: Endless racing with full telemetry.
3. **3-2-1-GO Starting Grid Countdown & Finish / High-Score Celebration Screen**:
   - Starting gantry lights with synthesized audio beeps, checkpoint arches along the road, floating score popups, and a comprehensive Post-Race Results / New High Score screen.

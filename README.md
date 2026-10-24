# Argon: Overdrive — Modern First-Person Software-Rasterized 3D Racer (C++20 / Qt 6)

A complete CPU software-rasterized first-person pseudo-3D racing game built over C++20 and Qt 6. It implements all 8 phases of the [Implementation Blueprint](Software-Rasterized%20Pseudo-3D%20Racer%20Implementation%20Blueprint%20(C++%20Qt%206).md) plus the v3.0 Modern Realism & Gameplay Overhaul in [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md).

## Modern Realism & Gameplay Features

- **Realistic Physical Bump Collisions & AR Passing-Gap Guides (`collision.cpp`, `vehicle.cpp`, `game.cpp`)**:
  - **Fair Lane-Splitting Clearance**: Refined gameplay hitboxes allow realistic lane splitting between adjacent traffic cars.
  - **Physical Bump & Momentum Transfer**: Traffic and scenery impacts apply lateral rebound impulses, speed scrubbing, and proportional **Car Health damage** instead of interrupting the race with a cutscene on every bump.
  - **3D AR Bumper Trajectory Rails & Target Brackets (`Q` toggle)**: Projects your car's exact bumper width onto the road ahead (`Green` = Clear Corridor, `Cyan` = Slipstream Drafting, `Amber` = Tight Squeeze, `Red` = Blocked + `<< PASS LEFT / PASS RIGHT >>` indicator above the obstacle).
- **Car Health (`0%`–`100%`), Pit Repair & On-Spot Wreck Recovery (`vehicle.cpp`, `game.cpp`)**:
  - **Live Health Bar**: Displayed on the left digital cockpit screen, top HUD, and repair banners.
  - **Checkpoint & Field Repair**: Passing through Checkpoint Arches restores `+25%` Health; pressing `H` performs an instant **Field Repair** (`+50%` Health).
  - **Zero-Health Wrecked State**: If Health reaches `0%`, the car stalls and lets you choose between **`[R]` / `[H]` On-Spot Repair & Continue** right where you stopped or **`[Enter]` Restart from Grid**.
- **4 Selectable Supercar Options (`vehicle.h/.cpp`)**:
  1. **`APEX GT-R`** (*Balanced Twin-Turbo Coupe* — `300 km/h`, `100%` Armor, `100%` Grip, Cyan Accent)
  2. **`VORTEX V12`** (*Hypercar Interceptor* — `345 km/h`, `124%` Accel, `85%` Armor, Crimson Accent)
  3. **`DRIFT SPEC-R`** (*Touge Track Special* — `288 km/h`, `128%` Grip, `95%` Armor, Amber Accent)
  4. **`TITAN ENFORCER`** (*Armored Endurance GT* — `294 km/h`, `155%` Armor, `102%` Grip, Emerald Accent)
- **Traffic Congestion Selector (`T` key or Menu)**:
  - **`LOW (8 Cars)`**, **`MEDIUM (16 Cars)`**, and **`HIGH / RUSH HOUR (24 Cars)`**.
- **Toggleable Steering / Cornering Assist (`G` key or Menu)**:
  - **`OFF (Manual Realism — Default)`**: Curves apply full centrifugal push outward with zero automatic turning.
  - **`ON (ADAS Apex/Lane Assist)`**: Gently assists steering into corners and stabilizes lane position.
- **Detailed Modern First-Person Cockpit (`cockpit.cpp`, `game.cpp`)**:
  - Carbon-weave centre console, sequential 8-LED RPM shift-light array, sculpted side air vents, ambient LED dash strip matching your selected car, left blind-spot wing mirror (`! CAR NEAR !`), panoramic top rear-view mirror, and a flat-bottom GT steering wheel with rotating metallic paddle shifters and driver racing gloves.
- **5 Racing Tracks, 2D GPS Radar & Variable Pixel Size (`1px` `960x720` to `6px` `160x120`)**:
  - Switch pixel granularity live with `1`–`5` or `[` / `]`, toggle fullscreen with `F` / `F11`, and cycle scaling modes with `L`.

## Controls

| Key / Input | Action |
| --- | --- |
| `W` / `Up`, `S` / `Down`, `A` / `Left`, `D` / `Right` | Throttle, Brake, Steer Left/Right (or Navigate Menus) |
| `Space` / `Shift` | Activate Nitro Boost (or Confirm Menu Selection) |
| `H` / `R` | Field Repair (`+50%` Health) / On-Spot Full Repair when Wrecked |
| `G` | Toggle Steering Assist (`ADAS ON` vs `Manual OFF`) |
| `T` | Cycle Traffic Congestion (`Low 8` / `Medium 16` / `High 24`) |
| `Q` | Toggle 3D AR Passing-Gap Trajectory Rails & Target Brackets |
| `1` – `5` or `[` / `]` | Change Pixel Size / Quality (`1px` Ultra `960x720` to `6px` Chunky) |
| `F` / `F11` & `L` | Toggle Fullscreen (`F`/`F11`) & Cycle Display Scaling (`L`) |
| `Tab` / `B` / `V` / `X` / `C` | GPS Mode (`Tab`), Biome (`B`), Mirror (`V`), Wipers (`X`), Weather (`C`) |
| `M` / `N` / `P` / `F3` / `Esc` | Mute (`M`), Music (`N`), Pause (`P`), Profiler (`F3`), Menu (`Esc`) |

## Building & Running

```bash
# Build Qt 6 Application via CMake
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/racer

# Or run the 49-check Automated Verification Suite
make test
```

# Argon: Overdrive — Modern First-Person Software-Rasterized 3D Racer (C++20 / Qt 6)

A complete CPU software-rasterized first-person 3D racing game built over C++20 and Qt 6. It implements all 8 phases of the [Implementation Blueprint](Software-Rasterized%20Pseudo-3D%20Racer%20Implementation%20Blueprint%20(C++%20Qt%206).md), the v3.0 Modern Realism & Gameplay Overhaul, and the **v4.0 Full-Depth Software 3D Graphics Pipeline (`raster3d.h` / `raster3d.cpp`)** documented in [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md).

## Full-Depth Software 3D Graphics Pipeline (`raster3d.h` / `raster3d.cpp`)

- **Per-Pixel Depth Buffer (`RasterPipeline::depthBuffer[960 * 720]`)**:
  - 64-byte aligned `float` Z-buffer cleared to $+\infty$ at the start of every frame (`clearDepth()`), with zero dynamic heap allocations during gameplay.
- **Road & Track Perspective Projection with Banking/Camber (`DrawTrapezoid`)**:
  - Maps 3D camera coordinates $(X, Y, Z)$ via $X_{\text{screen}} = (X / Z) \cdot f_x + C_x$ and $Y_{\text{screen}} = (Y / Z) \cdot f_y + C_y$.
  - Supports road curvature, elevation slopes ($dY$), lateral curve banking/camber (`nearBankSlope`, `farBankSlope`), per-pixel $1/Z$ depth interpolation, dual tire-wear grooves, and 3D beveled curbs.
  - **Mip-Filtered / Analytical Anti-Aliased Road Striping**: Distance-based frequency fade (`mipFade`) and sub-pixel edge feathering eliminate horizon moiré shimmer on lane dashes, road edges, and rumble strips.
- **Software 3D Triangle & Quad Rasterizer with Z-Test & Lambertian Shading (`DrawTriangle3D`, `DrawQuad3D`)**:
  - Sutherland-Hodgman near-plane clipping ($Z \ge Z_{\text{near}}$), back-face culling, perspective-correct $1/Z$ interpolation, per-pixel Z-buffer occlusion testing (`z < depthBuffer[y * width + x]`), and Lambertian $(N \cdot L)$ diffuse + specular shading.
  - **Detailed 3D Low-Poly Opponent Cars & Trucks (`DrawOpponentCarMesh3D`, `DrawTruckMesh3D`)**: 3D widebody chassis, wheel-arch fenders, 4 3D wheels with alloy rims, tapered glass cabin, raised carbon GT rear wing on twin pylons, rear underbody diffuser, and glowing 3D LED tail/brake light bars.
  - **3D Roadside Props (`DrawSceneryMesh3D`, `DrawCheckpointArchMesh3D`)**: Multi-tier 3D pyramidal pine/deciduous trees, 3D cantilevered streetlamps with glowing luminaire heads, 3D twin-pylon highway billboards, 3D faceted rocks, 3D curve guardrail posts with reflective chevrons, and 3D overhead checkpoint arches.
- **Atmospheric Depth Cues (`applyExponentialFog`)**:
  - Per-pixel exponential distance fog $\text{factor} = \exp(-\text{fogDensity} \cdot Z)$ blending surface colors smoothly into the horizon haze, plus twin volumetric headlight cones on night/dusk tracks.
- **Distinct Cockpit Forward Layering Pass (`RenderCockpit3D`)**:
  - Renders the interior cockpit and steering wheel in a dedicated forward pass with depth testing disabled, adding 3D normal-mapped shading across the carbon instrument binnacle hood, 3D beveled gauge rings, illuminated needles, and 3D toroidal flat-bottom GT steering wheel rim.
  - Toggle live between the **Full-Depth 3D Z-Buffer Pipeline (`3D Z-BUF`)** and classic scanline mode at any time with **`E`**.

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
- **5 Racing Tracks, 2D GPS Radar & Variable Pixel Size (`1px` `960x720` to `6px` `160x120`)**:
  - Switch pixel granularity live with `1`–`5` or `[` / `]`, toggle fullscreen with `F` / `F11`, and cycle scaling modes with `L`.

## Controls

| Key / Input | Action |
| --- | --- |
| `W` / `Up`, `S` / `Down`, `A` / `Left`, `D` / `Right` | Throttle, Brake, Steer Left/Right (or Navigate Menus) |
| `Space` / `Shift` | Activate Nitro Boost (or Confirm Menu Selection) |
| `E` | Toggle Full-Depth 3D Z-Buffer Pipeline (`3D Z-BUF` vs `2.5D`) |
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

# Or run the 54-check Automated Verification Suite
make test
```

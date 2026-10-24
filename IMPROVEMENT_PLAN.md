# Argon Overdrive: Full-Depth 3D Software RasterPipeline & Modern Realism Plan (v4.0)

This plan details the engineering architecture of **Argon: Overdrive**, combining the **Modern Realism & Gameplay Overhaul (Steps 1–4)** with the **Full-Depth Software 3D Graphics Pipeline (`raster3d.h` / `raster3d.cpp`, Step 5)**—all purely in CPU software (`uint32_t` framebuffer + `float` Z-buffer) presented via Qt 6 `QWidget` without modifying existing vehicle physics, telemetry, collision masks, or input handling.

---

## Step 1: Realistic & Fair Collision Physics + Visual Passing-Gap & Lane Guides

1. **Fair Physical Hitboxes & Non-Interrupting Bump Physics (`collision.h/.cpp`, `vehicle.h/.cpp`)**:
   - **Refined Gameplay Hitboxes**: Normalized player and traffic lateral hitboxes in gameplay mode provide realistic clearance for lane-splitting and overtaking between adjacent cars.
   - **Physical Impulse & Bounce Response**: Traffic and scenery collisions apply **lateral momentum transfer (bump push-away)**, speed scrubbing, metallic crunch/scrape audio, and proportional **Car Health damage** (`5%`–`35%`).
2. **AR Lane-Width Trajectory Rails & Passing-Gap Indicators (`game.cpp`)**:
   - **Projected Bumper Trajectory Lines (`Q` key)**: Renders 3D perspective lane-guide rails on the asphalt ahead showing the exact width of the player's car projected down the road (`Green` = Clear Path, `Cyan` = Slipstream Zone, `Amber` = Tight Squeeze, `Red` = Obstacle in Path).
   - **Overhead Traffic Gap & Proximity Brackets**: Draws HUD target brackets above nearby traffic cars indicating closing distance, safe passing side (`<< PASS LEFT` / `PASS RIGHT >>`), and **Slipstream / Drafting** lock.

---

## Step 2: Toggleable Steering Assist (ADAS), 4 Selectable Cars & Traffic Congestion Levels

1. **Toggleable Steering / Cornering Assist (`STEER ASSIST: ON / OFF` via `G` key or Menu)**:
   - **OFF (Pure Manual Realism — Default)**: Full centrifugal force pushes the car toward the outside of bends; the car never turns automatically in corners.
   - **ON (ADAS Apex / Lane Assist)**: Provides gentle cornering counter-force (`55%` centrifugal compensation) and subtle lane stabilization.
2. **4 Selectable Player Cars (`CarSpec` `0..3` via Launch Menu)**:
   - **1. `APEX GT-R`** (*Balanced Twin-Turbo GT* — `300 km/h`, `100%` Armor, `100%` Grip, Cyan Carbon Interior)
   - **2. `VORTEX V12`** (*Hypercar Interceptor* — `345 km/h`, `124%` Accel, `85%` Armor, Crimson Alcantara Interior)
   - **3. `DRIFT SPEC-R`** (*Touge Track Special* — `288 km/h`, `128%` Grip, `95%` Armor, Amber Titanium Interior)
   - **4. `TITAN ENFORCER`** (*Armored Endurance GT* — `294 km/h`, `155%` Armor, `102%` Grip, Emerald Tactical Interior)
3. **Traffic Congestion Selector (`LOW` / `MEDIUM` / `HIGH` via Launch/Pause Menu or `T` key)**:
   - **Low (`8` cars)**, **Medium (`16` cars)**, and **High / Rush Hour (`24` cars)**.

---

## Step 3: Car Health, On-Spot Repair & Wreck Recovery System (`vehicle.h/.cpp`, `game.cpp`)

1. **Car Health Bar (`0%`–`100%` Integrity)**:
   - Displayed on both the digital dashboard cluster and top HUD (`Green` -> `Amber` -> `Flashing Red` below `25%`).
   - Passing through 3D Checkpoint Arches repairs `+25%` Health; pressing `H` performs an instant **Field Repair** (`+50%` Health).
2. **Zero-Health Wrecked State (`ScreenState::Wrecked`)**:
   - When Car Health reaches `0%`, the engine stalls and lets the player choose **`[R]` / `[H]` to Repair on the Spot & Continue Driving** or **`[ENTER]` to Restart from the Grid**.

---

## Step 4: Modern Detailed First-Person Cockpit (`cockpit.cpp`, `game.cpp`, `raster3d.cpp`)

1. **Modern Supercar Interior (`960x720` High-Detail Rasterization)**:
   - Carbon-fibre instrument binnacle, stitched leather dash top, metallic A-pillar trim with tweeter grilles, sculpted side air vents, left blind-spot mirror (`! CAR NEAR !`), panoramic top rear-view mirror, and dynamic LED ambient accent strip matching the selected car.
   - **Sequential RPM Shift-Light Array**: 8-LED horizontal shift bar above the steering column (`Green` -> `Yellow` -> `Red` -> `Blue`).
   - **3D-Shaded Flat-Bottom GT Steering Wheel + Paddle Shifters + Driver Gloves**: Rendered in a distinct forward pass (`RenderCockpit3D`) with depth testing disabled.

---

## Step 5: Full-Depth Software 3D Graphics Pipeline (`raster3d.h` / `raster3d.cpp`)

1. **Per-Pixel Z-Buffer (`RasterPipeline`)**:
   - Statically allocated 64-byte aligned `float depthBuffer[960 * 720]` cleared to $+\infty$ (`1e30f`) at the start of each frame (`clearDepth()`).
   - Zero heap allocations during steady-state `tick()` and `render()`.
2. **Perspective Road Trapezoid Projection with Camber & Anti-Aliased Striping (`DrawTrapezoid`)**:
   - Projects 3D camera coordinates $(X, Y, Z)$ via $X_{\text{screen}} = (X / Z) \cdot f_x + C_x$, $Y_{\text{screen}} = (Y / Z) \cdot f_y + C_y$.
   - Supports road curvature, elevation slopes ($dY$), curve banking/camber (`nearBankSlope`, `farBankSlope`), per-pixel $1/Z$ depth writes, dual tire-wear tracks, 3D beveled curbs, and analytical box-filtered anti-aliased lane markings (`mipFade`).
3. **3D Triangle & Quad Rasterizer with Near-Plane Clipping & Lambertian Shading (`DrawTriangle3D`, `DrawQuad3D`)**:
   - Sutherland-Hodgman clipping against $Z \ge Z_{\text{near}}$, back-face culling, surface normal calculation, Lambertian diffuse $(N \cdot L)$ + specular highlight shading, perspective-correct $1/Z$ barycentric interpolation, and per-pixel Z-buffer test (`z < depthBuffer[y * width + x]`).
   - Renders multi-part 3D opponent cars (`DrawOpponentCarMesh3D`), 3D freight trucks (`DrawTruckMesh3D`), 3D trees/lamps/billboards/rocks/guardrails (`DrawSceneryMesh3D`), and 3D checkpoint arches (`DrawCheckpointArchMesh3D`).
4. **Atmospheric Depth Cues (`applyExponentialFog`)**:
   - Exponential distance fog $\text{factor} = \exp(-\text{fogDensity} \cdot Z)$ blending surface colors into the horizon haze.
5. **Top-Level `RenderFrame()` Hook**:
   - Clean, self-contained entry point invoked from `Game::render()` (toggleable live with `E`).

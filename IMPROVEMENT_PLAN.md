# Argon Overdrive: Modern Realism & Gameplay Overhaul Plan (v3.0)

This plan details the step-by-step engineering upgrades to transform **Argon: Overdrive** into a modern, realistic first-person software-rasterized racing game while maintaining all core constraints (**100% CPU software rasterization, Qt 6 presentation, First-Person perspective, and all automated verification checks passing**).

---

## Step 1: Realistic & Fair Collision Physics + Visual Passing-Gap & Lane Guides

1. **Fair Physical Hitboxes & Non-Interrupting Bump Physics (`collision.h/.cpp`, `vehicle.h/.cpp`)**:
   - **Refined Gameplay Hitboxes**: Reduce normalized player and traffic lateral hitboxes in gameplay mode so a 3-lane road provides wide, realistic clearance for lane-splitting and overtaking between adjacent cars.
   - **Physical Impulse & Bounce Response**: Instead of forcing a 3-second fade-out respawn cutscene on every contact, traffic and scenery collisions now apply realistic **lateral momentum transfer (bump push-away)**, speed scrubbing, metallic crunch/scrape audio, and proportional **Car Health damage** (`5%`–`35%`).
2. **AR Lane-Width Trajectory Rails & Passing-Gap Indicators (`game.cpp`)**:
   - **Projected Bumper Trajectory Lines**: Renders subtle 3D perspective lane-guide rails on the asphalt ahead showing the exact width of the player's car projected down the road (`Green` = Clear Path, `Amber` = Tight Squeeze / Slipstream Zone, `Red` = Obstacle in Path).
   - **Overhead Traffic Gap & Proximity Brackets**: Draws modern HUD target brackets above nearby traffic cars indicating closing distance, safe passing side, and **Slipstream / Drafting** lock.

---

## Step 2: Toggleable Steering Assist (ADAS), 4 Selectable Cars & Traffic Congestion Levels

1. **Toggleable Steering / Cornering Assist (`STEER ASSIST: ON / OFF` via `G` key or Menu)**:
   - **OFF (Pure Manual Realism)**: Full centrifugal force pushes the car toward the outside of bends; the car never turns automatically in corners.
   - **ON (Apex / Lane Assist)**: Provides gentle cornering counter-force (`55%` centrifugal compensation) and subtle lane stabilization for relaxed arcade driving.
2. **4 Selectable Player Cars (`CarSpec` `0..3` via Launch Menu or `V` in Menu)**:
   - **1. `APEX GT-R`** (*Balanced Twin-Turbo GT* — `300 km/h`, Balanced Grip & Acceleration, `100 HP` Health, Cyan Carbon Interior)
   - **2. `VORTEX V12`** (*Hypercar Interceptor* — `345 km/h`, Extreme Acceleration, `85 HP` Health, Crimson Alcantara Interior)
   - **3. `DRIFT SPEC-R`** (*Touge Track Special* — `285 km/h`, Maximum Cornering Grip & Nimble Width, `95 HP` Health, Amber Titanium Interior)
   - **4. `TITAN ENFORCER`** (*Armored Endurance GT* — `295 km/h`, Heavy Impact Resistance, `150 HP` Health, Emerald Tactical Interior)
3. **Traffic Congestion Selector (`LOW` / `MEDIUM` / `HIGH` via Launch/Pause Menu or `T` key)**:
   - **Low Congestion**: `8` active cars, long spacing (`22` segments minimum separation).
   - **Medium Congestion**: `15` active cars, balanced spacing (`15` segments separation).
   - **High / Hard Congestion**: `24` active cars, dense rush-hour packs (`10` segments separation).

---

## Step 3: Car Health, On-Spot Repair & Wreck Recovery System (`vehicle.h/.cpp`, `game.cpp`)

1. **Car Health Bar (`0%`–`100%` Integrity)**:
   - Displayed on both the digital dashboard cluster and top HUD (`Green` -> `Amber` -> `Flashing Red` below `25%`).
   - Passing through 3D Checkpoint Arches repairs `+25%` Health; pressing `H` performs an instant **Field Repair** (`+50%` Health).
2. **Zero-Health Wrecked State (`ScreenState::Wrecked`)**:
   - When Car Health reaches `0%`, the engine stalls with warning alarms and presents an immediate choice:
     - **Press `[R]` to Repair on the Spot & Continue Driving** (restores `100%` Health at your current track position with temporary collision immunity).
     - **Press `[ENTER]` to Restart the Race from the Starting Grid**.

---

## Step 4: Modern Detailed First-Person Cockpit & Enhanced Raster Graphics (`cockpit.cpp`, `roadrender.cpp`, `sprites.cpp`, `game.cpp`)

1. **Modern Supercar Interior (`960x720` High-Detail Rasterization)**:
   - **Multi-Layered Dashboard**: Carbon-fibre instrument binnacle, stitched leather dash top, metallic A-pillar trim with tweeter grilles, sculpted side air vents, and dynamic LED ambient accent strip matching the selected car.
   - **Sequential RPM Shift-Light Array**: 8-LED horizontal shift bar above the steering column lighting up `Green` -> `Yellow` -> `Red` -> `Blue` as RPM approaches redline.
   - **Flat-Bottom GT Steering Wheel + Paddle Shifters + Driver Gloves**: Detailed flat-bottom wheel rim, perforated leather grips, rotating `-` / `+` metallic paddle shifters, 3D spokes, and driver racing gloves at 9-and-3.
2. **Realistic World & Sprite Raster Shading**:
   - **Asphalt Specular Sheen & Headlight Beams**: Subtle road center sheen, dynamic twin headlight cones illuminating the road ahead in night/sunset tracks, and 3D roadside curve chevrons.
   - **Enhanced Rival Vehicle Sprites**: Contact shadow patches on the asphalt, metallic body gradients, rear spoilers, dual exhausts, and glowing LED tail/brake light bars.
   - **Slipstream Drafting & Thread-the-Needle Mechanics**: Aerodynamic drafting bonus behind rival cars (`DRAFTING +SPEED!`) and `THREAD THE NEEDLE +2000!` bonus when splitting between two cars.

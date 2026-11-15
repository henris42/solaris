# Voyager 2, simulated

Voyager 2's route from 1977 to 2026, flown by a small orbit library under gravity alone and compared with NASA's own trajectory. The point is to show that the library (`orbit/orbit.hpp`, one header) is general: the same integrator that carries a navigation satellite around the Earth carries a probe past four planets.

## Run it

```
git clone --recurse-submodules https://github.com/henris42/solaris.git     # GameApi is a submodule; it is only needed for the game
./run.sh
```

Needs a C++20 compiler and nothing else. It takes about 15 seconds, prints the table below and writes the 3D views to `out/`.

## Result

The route is flown in nine legs. Each leg starts from NASA's position and velocity and is then on its own. "Error" is the distance between the simulated spacecraft and NASA's at the same moment.

| Leg | From | Days | Path | Largest error | Error / path |
|---|---|---|---|---|---|
| Earth to Jupiter | 1977-09-01 | 670 | 1 070 million km | 1 574 655 km | 1 in 679 |
| Jupiter flyby | 1979-07-05 | 9 | 10.4 million km | 6 506 km | 1 in 1 601 |
| Jupiter to Saturn | 1979-07-14 | 770 | 1 159 million km | 12 750 km | 1 in 90 886 |
| Saturn flyby | 1981-08-22 | 8 | 9.0 million km | 9 811 km | 1 in 916 |
| Saturn to Uranus | 1981-08-30 | 1600 | 2 642 million km | 2 142 270 km | 1 in 1 233 |
| Uranus flyby | 1986-01-21 | 7 | 9.1 million km | 27 km | 1 in 334 948 |
| Uranus to Neptune | 1986-01-28 | 1300 | 2 161 million km | 3 088 630 km | 1 in 700 |
| Neptune flyby | 1989-08-22 | 6 | 8.9 million km | 41 km | 1 in 219 335 |
| Neptune to 2026 | 1989-08-28 | 13 500 | 18 277 million km | 2 412 224 km | 1 in 7 577 |

Controls, flown by the same code: bodies whose NASA paths are gravity alone.

| Control | Days | Path | Largest error | Error / path |
|---|---|---|---|---|
| Uranus | 18 080 (49 years) | 10 391 million km | 474 km | 1 in 21.9 million |
| Tethys, a moon of Saturn | 10 | 9.8 million km | 113 km | 1 in 87 056 |
| Tethys, with Saturn taken as round | 10 | 9.8 million km | 19 595 km | 1 in 500 |

The flybys, with and without the planet's flattening (J2):

| Flyby | Nearest | From the centre | Path turned by | Error with J2 | Without |
|---|---|---|---|---|---|
| Jupiter | 1979-07-09 22:30 | 721 336 km | 91.9° | 6 506 km | 6 993 km |
| Saturn | 1981-08-26 03:20 | 160 721 km | 84.6° | 9 811 km | 177 km |
| Uranus | 1986-01-24 18:00 | 107 155 km | 23.0° | 27 km | 103 km |
| Neptune | 1989-08-25 04:00 | 29 599 km | 54.0° | 41 km | 6 415 km |

Times are TDB, to the nearest 10 minutes of NASA's table.

## What the numbers say

- **The model is sound.** It follows Uranus for 49 years to 474 km and Tethys for 10 days to 113 km. Anything much larger on a Voyager leg is therefore not a fault of the gravity model.
- **Uranus and Neptune flybys fit to tens of kilometres**, and only with the planet's flattening in the model. Neptune without it is off by 6 415 km: Voyager passed 4 800 km above the cloud tops.
- **The cruise legs drift by millions of kilometres.** The spacecraft corrected its course with its thrusters many times, and none of that is in a gravity model. A velocity change of one metre per second is 86 km a day.
- **Jupiter: an engine burn is visible.** In a separate check (not part of this program) NASA's trajectory changes its velocity by about 13 m/s within hours after the closest approach, in a way gravity does not account for. That is the size and shape of a course correction; we have not looked it up in the mission log.
- **Saturn: NASA's file fits a round Saturn.** With Saturn's flattening the simulation is 9 811 km off, without it 177 km. The flattening itself is right in our model: Tethys needs it (113 km against 19 595 km). So this stretch of the trajectory in Horizons behaves as if it had been computed without Saturn's flattening. We cannot confirm that from NASA's documentation.
- **Einstein does not matter here.** The relativistic correction is in the model. Switching it off changes the first leg by 170 km out of 1.5 million, the other legs by a few kilometres, the flybys by 0.1 km.

## How it works

```mermaid
flowchart LR
  H[(NASA JPL Horizons<br/>vector tables in data/)] --> R[read tables]
  R --> T[Voyager 2: position<br/>and velocity over time]
  R --> A[Sun, planets, moons:<br/>where each one is over time]
  T -->|first state of a leg| I
  A -->|attractors| I[orbit.hpp<br/>integrator]
  I --> S[simulated position<br/>at NASA's epochs]
  T --> C{compare}
  S --> C
  C --> O[table on the console]
  S --> G[out/*.glb and *.txt]
  T --> G
  G --> V[GameApi]
```

A leg is flown in one of two frames, and that is the only difference between a cruise and a flyby:

```mermaid
flowchart TB
  subgraph Cruise leg
    direction TB
    c1[frame fixed to the solar system's centre of mass] --> c2[no central body]
    c2 --> c3[attractors: Sun, Jupiter, Saturn, Uranus, Neptune<br/>and the inner planets while they are near]
  end
  subgraph Flyby leg
    direction TB
    f1[frame centred on the planet, z along its pole] --> f2[central body: the planet with its flattening J2]
    f2 --> f3[attractors: Sun, the large moons, the nearest other planet]
  end
```

Between two of NASA's epochs the integrator takes fixed steps (fourth-order Runge-Kutta). In every step it asks four times for the acceleration:

```mermaid
flowchart TD
  S[state: position, velocity, time] --> CB[central body, if any:<br/>gravity, flattening J2, Einstein's correction]
  S --> AT[each attractor:<br/>interpolate its table to this time]
  AT --> P[pull toward it]
  AT --> Q[less its pull on the central body,<br/>which is the frame's origin]
  AT --> E[Einstein's correction for the Sun]
  CB --> SUM((sum))
  P --> SUM
  Q --> SUM
  E --> SUM
  SUM --> RK[Runge-Kutta: four such sums give the next state]
  RK -->|next step| S
  RK -->|at NASA's epoch| CMP[distance to NASA's position]
```

The step is 5 to 20 seconds in a flyby and one to six hours in a cruise. Halving every step changes the errors above by 11 km at most, on legs that are off by millions.

## Fly along: the game

![The four encounters](screens/all.jpg)

`./play.sh [jupiter|saturn|uranus|neptune]` opens a GameApi window and plays that system's tour: three minutes with every flyby in it, of the planet and of each moon Voyager 2 passed.

![The Jupiter tour](screens/tour-jupiter.jpg)

Each flyby is shown at a slow rate while the body is near, and the way between flybys is run through fast. The view keeps on the body whose turn it is, through the camera Voyager would have used for it: its narrow-angle camera (0.42 degrees wide) for the far moons, its wide-angle camera (3.2 degrees) for the nearer ones, and the eye beside the spacecraft for what fills the sky.

| Tour | Flybys, in order, with the nearest distance from the body's centre |
|---|---|
| Jupiter, July 1979 | Callisto 215 028 km, Ganymede 62 063 km, Europa 205 532 km, Jupiter 721 336 km, Io 1 129 435 km |
| Saturn, August 1981 | Iapetus 908 042 km, Titan 665 536 km, Dione 501 840 km, Mimas 309 412 km, Saturn 160 721 km, Enceladus 87 581 km, Tethys 93 363 km, Rhea 644 399 km |
| Uranus, January 1986 | Titania 364 408 km, Oberon 469 492 km, Ariel 127 298 km, Miranda 28 600 km, Uranus 107 155 km, Umbriel 324 445 km |
| Neptune, August 1989 | Neptune 29 599 km, Triton 39 754 km |

These distances come out of the simulation; the terminal lists them with their dates when a tour starts.

| | |
|---|---|
| drag with the mouse | look around; the view then stays where you put it until the next flyby |
| wheel | nearer to the spacecraft, farther |
| `1` `2` `3` `4` | Jupiter, Saturn, Uranus, Neptune |
| `z` | by eye, wide-angle camera, narrow-angle camera |
| `v` | the spacecraft in view or not |
| `n`, `p` | next, previous flyby, facing it |
| `f` | face the planet |
| `+` `-` | faster, slower |
| `t` | the tour on or off (off: time at one rate, the view where you put it) |
| space, `r` | pause, start of the scene |
| Esc | quit |

`./play.sh saturn -14 60` starts without the tour, 14 hours before the nearest approach to Saturn at 60 times real speed; `./play.sh jupiter ganymede 1` starts at Ganymede's nearest approach in real time.

True to life: the direction and apparent size of every body at every moment (from the simulation), the stars (NASA's star map, every star where it is in the sky, turned into the planet's frame), the planets in visible light, which side of each body is in sunlight, the planet's turning, Saturn's rings, the spacecraft's shape and surface with its dish toward the Earth, the fields of its two cameras.

Not true to life: time; the Sun, drawn at least a fifth of a degree wide; no shadows (of Saturn on its rings, of a planet on a moon); how the spacecraft is rolled about its dish; through a camera the stars stay as the eye sees them; GameApi's own camera is a long lens, 17 degrees from top to bottom, so stars look larger than points.

It has been played by hand once (the scene keys worked) and otherwise run on a virtual screen, where these pictures were taken. Memory stayed level in a one-minute run. One run in about ten stopped by itself with an out-of-memory error a few seconds after the start; the cause is not found.

Building GameApi on Ubuntu: `sudo apt-get install libsdl2-dev libsdl2-mixer-dev libglew-dev libfreetype-dev libopencv-dev` (its own README does not list the last one), then `make -j 8 depbuild` in `GameApi/GameApi`. GameApi asks for an antialiased window, which some X servers give only through EGL; `play.sh` sets `SDL_VIDEO_X11_FORCE_EGL=1` for that.

How the game draws, in GameApi's terms: every body is its own model with its own matrix, set anew each frame. GameApi scales each model it loads to 400 units across; the first triangle of each of our files spans the model's box so that this is exact. Bodies and sky are drawn in a first pass with the eye at the spacecraft, each at a distance that keeps their order and with the size that gives its true apparent size; the spacecraft is drawn in a second pass. Sunlight on a body is a second ball over it, turned to the Sun, clear on the day side and darkening to the night side.

## The overview views

`out/` also holds six views as glTF files (`.glb`), each centred and scaled to 150 units, with a GameApi script of the same name (`.txt`): `route-planets` (1977 to 1989), `route-whole` (to 2026) and the four flybys in the planet's own frame with the planet and moons to scale. The blue line is the simulated trajectory; planet orbits are grey, moon orbits orange. From `out/`:

```
SDL_VIDEO_X11_FORCE_EGL=1 LD_LIBRARY_PATH=../GameApi/GameApi ../GameApi/GameApi/cmdline/gameapi_cmdline --file neptune.txt
```

## Files

| | |
|---|---|
| `voyager2.cpp` | the simulation: the legs, the comparison, the glTF writer, the game's data |
| `game.cpp`, `play.sh` | the game, on GameApi |
| `orbit/orbit.hpp` | the orbit library (a copy; one header, no dependencies) |
| `data/` | 38 vector tables from NASA JPL Horizons; `fetch.sh` fetches them again |
| `textures/` | surface maps and the star map; `fetch-textures.sh` fetches and prepares them again |
| `models/probe/` | NASA's Voyager model in three plain glTF parts; `tools/undraco.py` made them from `models/voyager-probe-b.glb` |
| `screens/` | pictures from the game |
| `GameApi/` | the engine, as a git submodule |
| `run.sh` | builds and runs the simulation |

## Not in the model

Thrust, radiation and heat recoil, flattening beyond J2, rings and small moons, and everything else that is not gravity of point masses, one flattened planet and Einstein's first correction. The simulation does not predict the route from the launch alone: each leg starts from NASA's state, and a leg ends where the next begins.

## Sources

- Trajectory of Voyager 2 and positions of the Sun, planets and moons: NASA JPL Horizons (`ssd.jpl.nasa.gov/horizons`), fetched 2026-11-15. Masses from the same service.
- Planet poles: IAU working group on cartographic coordinates. Flattening (J2) with its reference radius: published gravity fields of the planets. Both are written out in `voyager2.cpp`.
- Spacecraft model: NASA 3D Resources (`github.com/nasa/NASA-3D-Resources`, "Voyager Probe (B)"), which NASA describes as free and without copyright; unpacked from its mesh compression for GameApi.
- Moon maps: NASA 3D Resources, "Images and Textures", the same terms.
- Planet, Sun and ring maps in visible light: Solar System Scope (`solarsystemscope.com/textures`), licence CC BY 4.0, made from NASA data.
- Star map: NASA Scientific Visualization Studio, Deep Star Maps 2020 (`svs.gsfc.nasa.gov/4851`), from the Hipparcos, Tycho and Gaia catalogues.
- GameApi: by its author, under its own licence (GPL and LGPL), used here with the author's consent.

## Licence

Our own files (the simulation, the game, `orbit/orbit.hpp`, the scripts) are under the Apache License 2.0, see `LICENSE`. The data, the maps, the spacecraft model and GameApi are their owners' and keep the terms listed under Sources.

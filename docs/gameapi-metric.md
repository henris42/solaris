# GameApi in metres: what it would take

An evaluation of GameApi's `more_stable` branch (commit d106c8b), made by reading the source. Nothing in the engine was changed for it. Line numbers are for that commit; all files are in `GameApi/GameApi/`.

The question, from the engine's author: can metric be done as boxes, or is it fundamental?

## The answer

**It is not fundamental. It can be a box, after one small preparatory change that alters no behaviour.**

Nothing in the engine's structure assumes a unit. The matrix chain (`in_P * in_T * in_MV * position`), the shaders, the box system and the loaders would all work the same with a scene 2 units across or 2,000. What stands in the way is about forty constants, tuned to a scene some hundreds of units across, written as literals in the places that use them: the camera's distance, the near and far planes, the walker's turning point, the shadow volume, a viewer position inside six shaders.

A box cannot reach a literal. So the work has two steps:

1. **Gather the constants** into one settings structure whose defaults are today's values. Every existing script behaves exactly as before. This is the only part that touches existing code, and it touches it without changing what it does.
2. **Add a box**, say `metric(ev, ml)`, that sets the structure to metre values, turns off the rescale on load, and puts things back when it goes. A script wrapped in it is in metres. A script without it is what it is today.

That also means the break does not have to come all at once. The examples can move to metres one at a time, and the version in which the old way is finally removed can come later, or never.

What a box alone can already do today, with no preparatory change, and where that stops, is in "What a box can do today" below.

## Why units mean nothing now

Every model is rescaled on load so that the longest side of its bounding box is 400 units, centred on the origin (`GameApi_pl.cc:15274-15279`: `val2 = 1.0/val; val2 *= 400.0`). The same calculation serves glTF, OBJ and STL. A watch and a cathedral arrive the same size, and two files that belong together stay in register only if their author arranged it; the ant scene does it with a triangle spanning a common box at the start of each file.

Everything else is then tuned to that 400:

| what | value | where |
|---|---|---|
| the eye | 500 units from the origin (`in_T = Translate(0,0,-500)`) | `GameApi_main.cc:3378`, also 568 and 805 |
| projection | 90° field of view, near 10.1, far 1600 | six places: `GameApi_main.cc:559, 803, 2173`, `GameApi_sh.cc:60`, `GameApi.cc:38381`, `Main.cc:997` |
| the walker's turning point | 400 units along z | `GameApi.cc`, in `QuakeML`, `QuakeML2`, `QuakeML3` |
| walker speed | parameter × 70 units a second (default 20: 1,400 units a second) | `GameApi_in.cc:164`, `GameApi_main.cc:1030-1038` |
| viewer position inside lighting shaders | `vec3(0.0,0.0,-400.0)`, in object space | six places in `Shader.cc` |
| shadows | light 400 units away, a volume of ±300 by ±300 by ±8000, distances packed by dividing by 800 | `GameApi_pl.cc:11057, 11081, 11213`; `Shader.cc` |
| fog, lights | fog distance 300; light distance 500; point-light falloff `100.0/dist` | box defaults; `Shader.cc` |
| box defaults | cube ±100, sphere radius 100, torus 250/50, heightmap ±300 | `GameApi_cmd_polygon.cc` |

A model that kept its real size, a chair 0.8 units tall, would sit 500 units from the eye, in front of a near plane at 10.1, lit by a shadow volume 600 across. That is why dropping the rescale alone does not give a metric engine: it gives a dot.

## What already exists

Three pieces are already in the engine and point the way.

- **One switch for the rescale.** `g_resize_disabled` (`GameApi_pl.cc:14842`) is checked in the one function every loader uses (`find_mesh_scale`, `:15303-15308`). When it is set, models keep their file's size. It is set and never cleared: once on, it stays on for the life of the program, including for the next script loaded in the builder.
- **Two boxes named for metres.** `meters_scale` and `meters_default` (`GameApi_sp.cc:316-544`) set that switch and map a box of the scene, by default -400..400, onto the engine's 400 space with a scale in `in_MV`. They do not make anything metric: they fit a region to the view. Their two matrices also disagree: `in_MV` is scaled by `400/((sx2-sx1)/2)` and `env` by `400/(sx2-sx1)` (`:336-337`), a factor of two.
- **A scale in the VR code.** The OpenVR path multiplies the projection by `4.0/300.0` (`GameApi_vr.cc:602`): 300 units are 4 metres, so one unit is 1.33 cm and a freshly loaded model is 5.3 m across. A box `vr_scale` exists to set this, but it writes a variable nothing reads (`:49-54`).

## What a box can do today

Without touching any existing code, a new box at the root of a scene could set `g_resize_disabled`, scale `in_MV` by some number of units to the metre, and replace `in_P` and `in_T` in the environment it passes down (as `ogl_perspective` and `isometric_ml` already do for one each). Models would arrive in metres and the script's own positions and sizes would be in metres.

Where it stops:

- **Parameters that act after `in_MV` stay in engine units.** The walker's speed and turning point, fog distance, shadow volume, collision radius. A script would be in metres in some boxes and in engine units in others.
- **The shaders' built-in viewer position** is in object space, before any matrix. No box reaches it. It is already wrong today for anything not at the origin; with metre-sized objects it is wrong by more.
- **The shadow volume** is built from literals inside the shadow box.
- **The walker** carries its 400 inside the three walker classes.

So a pure box gives a scene that looks metric from the script and is not underneath. It is good enough for a model viewer. It is not good enough for walking, shadows or a headset.

## The preparatory change

One structure, one instance, defaults equal to today's literals:

```
struct WorldSettings {
  float eye_back      = 500.0;   // in_T
  float near_plane    = 10.1;
  float far_plane     = 1600.0;
  float fov_degrees   = 90.0;
  float walker_pivot  = 400.0;   // quake_ml, quake_ml2, fly_ml
  float walker_rate   = 7.0;     // the factor on get_delta_time()
  float shader_viewer = -400.0;  // the vec3(0,0,-400) in six shaders
  float shadow_dist   = 400.0, shadow_half = 300.0, shadow_depth = 8000.0, shadow_pack = 800.0;
  float falloff       = 100.0;   // point-light falloff
  bool  resize        = true;    // today's g_resize_disabled, the other way round
};
```

Each site reads the structure where it had the literal. For the GLSL the literal sits in a C++ string: either the number is formatted into the string when the shader is built, or it becomes a uniform. The first is the smaller change.

How much code that is, by counting the sites the surveys found:

| group | sites |
|---|---|
| projection (near, far, field of view) | 7 |
| default `in_T` | 3 |
| walker turning point, in three classes and two helpers | about 12 |
| walker rate | 2 |
| shader viewer position | 6 |
| shadow volume and packing | 3 in C++, about 8 live lines in GLSL |
| point-light falloff | 2 |
| rescale switch | already one |

About forty lines in seven files, each a literal replaced by a name. Behaviour is unchanged while the defaults hold, and that can be checked the way the walking boxes were: the same scene gives the same picture, byte for byte, before and after.

## The metric box

`metric(ev, ml)` sets the structure when it is made and restores the old values when it is destroyed (the walking boxes clear `quake_pos_height` the same way; `g_resize_disabled` shows what happens otherwise). Values worth starting from:

| setting | metres |
|---|---|
| rescale | off: glTF files are in metres by their own standard |
| eye | at the origin of the view; camera and walker boxes place it |
| near, far | 0.1 and 2,000: a ratio of 20,000, which the 24-bit depth buffer carries |
| field of view | 90°, as now |
| walker | turning about the eye itself; speed given in metres a second |
| shadows | a volume given by the box that makes them, sized to the scene |
| shader viewer position | the real eye, passed as a uniform |

OBJ, STL and PLY files carry no unit. In a metric script they would be read as metres, and a script that knows better scales them itself.

## What a box cannot carry: the defaults

A box's default values are strings in its registration: cube ±100, sphere radius 100, fog 300, walker speed 20. In a metric script they would be a 200-metre cube. By count:

- 999 boxes are registered and active; 466 take a float; about 275 take a float whose name says it is a length (867 such parameters).
- 196 boxes have a float default of 100 or more; about 178 have one on a size or a position.
- The type of a length is `float`, the same as an angle, a time or a factor. Only the parameter's name tells them apart.

Two facts about how scripts are read matter here, and both were checked in the parser:

- **A missing parameter is filled with the box's current default** (`GameApi_gui.cc:7167-7175`). Changing a default changes every old script that left that parameter out. Defaults must not be edited in place.
- **Scripts carry no version.** There is no header, no number, no alias for a renamed box. An unknown box prints an error and yields nothing; the script goes on.

So metric defaults need their own slot: a second default string for the parameters that are lengths, used when the metric box is in force, in the builder's dialogs and when a parameter is missing. That is the largest piece of plain work in the whole change, about 180 boxes to go through by hand, and it can be done family by family. Until a box has its metric default, its old default shows, which is wrong but visible.

A unit type for parameters (`"length"` beside `"float"`) would let the builder show "m" and would let defaults be converted by rule. It is a larger change to the registration and is not needed to start.

## Things that are not lengths, and stay

- **2D.** One unit is one pixel, with the origin at the top left and y downward (`Ortho(0, sx, sy, 0, 0, 1)`). That is right for 2D and is separate from all of the above. One thing to mind: the movement boxes decide between 2D and 3D from a global flag, `is_move_2d`, that a `turn_to_2d` box sets and nothing clears (`GameApi_sp.cc:193-200`).
- **Angles** are radians in some boxes and degrees in others.
- **Time** is in tenths of a second in movement boxes and in `get_delta_time()`, in seconds in `MainLoopEnv.time`, in milliseconds in `get_time()`. A metric walker needs seconds; the rest can wait.
- **Handedness.** The eye looks down -z, y is up, as glTF has it. The walkers mirror z once or twice internally. None of that changes with units.

## VR

Metres are necessary for a headset and are not what stands between the engine and one. What the source shows:

- The only VR code is for OpenVR on the desktop and for the old WebVR interface in the browser (`emscripten/vr.h`). There is no WebXR and no OpenXR. Browsers have dropped WebVR, so the browser path cannot run as it is.
- The OpenVR path has its own fixed scale (4/300), squeezes the projection by half in y (`GameApi_vr.cc:600`), asks the headset for its projection with 0.03 and 36 m regardless of the near and far it was given (`:579`), and moves the scene 1,200 units back (`:288`).
- The headset's pose is used in metres without the 4/300, so a real step moves the viewer by a different amount than the scene is scaled by.
- By the survey's reading, the pose is applied on the wrong side of the projection (`:658`). I have not run it.

With the settings structure in place, the VR path would take its scale from there (1, in a metric script) and the pose would agree with the scene. The rest is its own job: a WebXR backend for the browser build, and the faults above. It is larger than the metric change itself.

## Existing content

In the engine's tree, counted:

- 33 script files with 4,564 box lines; one of them, `cmdline/script.txt`, has 3,121.
- 49 builder files with 6,863 boxes.
- 1,648 box calls in the C++ demos, 1,148 inside the engine's own sources, 351 in the PHP, HTML and TeX around it.
- In the scripts, -400 appears 124 times, 300 127 times, 100 91 times, 800 39 times.

Outside the tree, user scripts are stored on meshpage.org (`php/mesh_save.php`, `php/user.php`). The source does not say how many.

With the box approach none of this breaks. Each piece moves to metres when someone wraps it and rewrites its numbers. A script already in the tree that no longer runs for other reasons (94 lines call boxes that have since been renamed, such as `move_api.empty`) is a separate matter.

## Faults met on the way

Found while reading; none was fixed. The first three were checked in the source by me, the rest are as the surveys reported them.

- `meters_scale` and `meters_default` scale `in_MV` and `env` by amounts that differ by two (`GameApi_sp.cc:336-337, 464-465`).
- `g_resize_disabled` and `is_move_2d` are set by a box and never cleared.
- `vr_scale` sets a variable nothing reads.
- `MainLoopApi::in_P(ev, true)` passes 80 to a function that takes radians (`GameApi_main.cc:3355`; `Matrix::Perspective` computes `tan(fovy/2)`). It is the fallback projection for the VR window.
- `ogl_perspective` uses an aspect of 800/600 whatever the window is (`GameApi_pl.cc:26210`).
- The rescale's bounding box is sampled from about 100 faces for static models, which is why a model can come out slightly off 400.
- A key for the rescale's bookkeeping is computed as an id times 400,000 in a 32-bit integer, which overflows for ids above 5,368.
- Fog measures z in `in_MV` space, before the 500-unit `in_T`, not distance from the eye.

## Order of work

| step | what | size | changes behaviour? |
|---|---|---|---|
| 1 | the settings structure, read at about forty sites | small: a day or two with the picture check | no |
| 2 | `metric` box that sets and restores it; a walker that takes metres a second and an eye height | small | only inside the box |
| 3 | shadows and fog take their sizes from the box that makes them | small to medium | only inside the box |
| 4 | metric defaults for the boxes with lengths, family by family | medium to large: about 180 boxes | only inside the box |
| 5 | examples rewritten in metres, one at a time | as long as there are examples | no |
| 6 | VR: scale from the settings, the faults above, a WebXR backend | large, separate | VR only |
| later | a `length` parameter type; a version line in scripts; removing the old mode | optional | yes |

Steps 1 and 2 give a scene in which a glTF model has its real size, the camera is where a person's eye would be, and the walker moves at walking pace. That is the point at which it can be tried on real content, and everything after it can be judged against something that runs.

The sizes are estimates from counting sites, not from doing the work.

## How this was made

Three read-only surveys of the source ran in parallel (camera and projection; model data in and out; the script API and existing content), each reporting file and line. The claims the conclusions rest on were then read in the source directly: the rescale and its switch, the `meters` boxes, the default `in_T`, the projection function, the shader and shadow literals, the time functions, the parser's handling of missing parameters, and the VR scale. The counts of boxes and of script lines are the surveys' own, made by parsing the registration files, and were not recounted. Nothing here was measured by running the engine except the eye position, which was printed from inside a box in the ant scene: `in_T` moves the origin to (0, 0, -500), as the source says.

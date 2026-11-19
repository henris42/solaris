# Two boxes for GameApi's walker: follow the ground, stay out of things

GameApi's `quake_ml2` lets you walk through a scene, on one level and through everything. These two new boxes go with it, for scenes whose ground and obstacles are meshes:

- `quake_ground` makes the walker follow the ground up and down.
- `quake_collide` keeps the walker out of trunks, stems and the like.

They are an addition to the engine, a patch for its author: `quake_ml2` and every other existing box do what they did, and a scene without the new boxes runs as before. The patch is not in the engine this repository pins.

## In a script

Both take the part of the scene to draw (an `ML`) and the faces to use (a `P`), draw the first unchanged, and work from the second. A `P` comes from a loaded model with `gltf_mesh_all_p`. They go anywhere under `quake_ml2`, around the part of the scene their faces belong to.

The ant scene's ground and trees, as they are:

```
TF I9=ev.mainloop_api.gltf_loadKK2(https://meshpage.org/antscene_ground.glb);
ML I10=ev.mainloop_api.gltf_mesh_all(ev,I9,1.0,3.0,1.0,0,0.1,400.0,0.1,0.0,ff000000,false,true,true,-1);
...
TF I17=ev.mainloop_api.gltf_loadKK2(https://meshpage.org/antscene_trees.glb);
ML I18=ev.mainloop_api.gltf_mesh_all(ev,I17,1.0,3.0,1.0,0,0.1,400.0,0.1,0.0,ff000000,false,true,true,-1);
```

and with the boxes (then use `I95` where the script used `I10`, and `I97` where it used `I18`):

```
P I94=ev.mainloop_api.gltf_mesh_all_p(ev,I9);
ML I95=ev.move_api.quake_ground(ev,I10,I94,60.0);

P I96=ev.mainloop_api.gltf_mesh_all_p(ev,I17);
ML I97=ev.move_api.quake_collide(ev,I18,I96,60.0,300.0);
```

| box | parameters |
|---|---|
| `quake_ground(ev, ml, ground, climb_speed)` | `climb_speed`: how fast the view may rise or fall, in the walker's own measure of speed (`quake_ml2`'s `speed`); 60 is three times the usual walking speed of 20 |
| `quake_collide(ev, ml, obstacles, radius, height)` | the walker's body is a post from the eye down by `height`, `radius` thick, both in the view's units (the eye is 400 units from the middle of a freshly loaded model). What lies lower than `height` below the eye is stepped over: with the eye 384 above the ground and `height` 300, things up to 84 high |

Several `quake_collide` boxes in one scene add up. Give `quake_collide` the things that should stop the walker and not the ground, which would stop it on every slope.

## How they work

`quake_ml2` turns and moves the whole scene about the walker, who stays at one point of the view: (0, 0, 400). A box under it receives the matrix that takes its faces to the view (`in_MV`, then `in_T`). Inverting that matrix gives the walker's place among the faces, whatever was done to the scene on the way down (the ant scene scales it by 40 and lowers it by 400). That is all either box needs to know about where it sits.

`quake_ground` puts the faces' heights on a net of 256 by 256 cells when it first draws: the highest face over each cell, and cells no face reaches take their neighbours' height. Each frame it looks up the height under the walker, finds where that point of the ground is drawn, and moves `quake_pos_height` (a shift of the view in y that `quake_ml2` applies) so that the point comes to where the ground under the walker was at the start. The eye height the scene was made with is kept, and no number for it is asked. The shift changes by at most `climb_speed` times the frame's time.

`quake_collide` marks the space its faces take up in a block of 384 by 128 by 384 cells when it first draws, by walking over every triangle in steps of half a cell. Each frame it looks at the cells within `radius` of the walker, between the eye and `height` below it. For every marked one it adds a push away from that cell, larger the deeper the cell is inside the radius, and hands the sum to `quake_ml2` through `quake_push_x` and `quake_push_z`. `quake_ml2` turns that from the view's directions into its own position at the start of the next frame and clears it. Because the push is along the line away from the thing, walking on at a slant slides along it.

Faces without area are left out in both. The scene's files each begin with one such triangle that spans the scene's box, and it would otherwise count as a wall across the world.

What `quake_ml2` itself gained: the shift by `quake_pos_height`, and taking the push. Both are zero unless a new box sets them.

## What was seen

In the engine's browser build, in Chrome on Windows with a graphics card, in the ant scene with this repository's new ground and trees:

- Walking forward from the start, the ground under the walker rose from 0 to 1,480 units (the rim of the clearing) and the view rose with it; where the ground under the walker was drawn stayed within about 10 units of its starting place on the way up and returned to it exactly at rest.
- Walking backward from the start, the walker met a trunk after 4,250 units, slid 430 units along its foot and stopped between two of its ribs. Without the box it walks through the trunk and out of the scene.
- The scene without the boxes, on the patched engine: on the desktop the same picture, byte for byte, as with the engine before the patch.

## Not done

- While a key pushes the walker into something, the walker is moved in by the step and out by the push each frame. The wobble is under one cell; it was not seen in pictures but was not looked for in motion by a person.
- The walker cannot be stopped by a slope too steep to climb: `quake_ground` follows any height. A cliff is climbed at `climb_speed`.
- Things that move (the ants) are not obstacles: the cells are filled once.
- A face larger than 200 cells is marked with gaps.
- Nothing was tried on Android, where `quake_ml2` builds its matrix differently for landscape; the push is worked out for the ordinary case.
- The new boxes were run in the browser build only. The desktop build compiles them and was run without them.

# Notes for an AI agent: making replacement models for a GameApi scene

Scope: what was learned making `tools/antforest.cpp` (see `docs/antforest.md`). Written to be read by a model at the start of a similar job. Facts are marked MEASURED (seen in a run or a file) or INFERRED.

## Task shape

- Input: a GameApi scene script (lines like `TF I9=ev.mainloop_api.gltf_loadKK2(<url>);` then `gltf_mesh_all` / `gltf_mesh_all_anim`), whose models are `.glb` files at URLs.
- Goal: better-looking models without touching the script or the other models.
- Method that worked: a generator that reads the old files for layout and writes drop-in files with the same names.

## Constraints that decide the design

1. MEASURED. GameApi recentres and scales every loaded model to its bounding box, sampled from about 100 faces. Files of one scene stay in register only because each starts with one triangle spanning a shared box (here corners at -8000 and +8000 on all axes) and declares POSITION min/max as that box. Write that triangle first, with the same box.
2. MEASURED. One mesh, one primitive, one material, one embedded PNG per file loads and draws. Attributes POSITION, NORMAL, TEXCOORD_0 as float, indices as 32-bit. Not tried: several primitives, vertex colours, quantized attributes, external images.
3. MEASURED. `doubleSided: true`; GameApi does not cull back faces anyway.
4. MEASURED. The engine's lighting can leave surfaces near black (the old trunks). Do not rely on it: put the shading into the colour each point picks (dark feet, light tips, dark undersides that are not black).
5. MEASURED. A texture used as a colour table works. Layout used: 256x256, eight bands of 32 rows; u = shade (dark to light), v inside a band = blend between two related colours. Keep 4 rows of margin per band and never let one triangle span two bands (interpolation would cross the bands between).
6. MEASURED. First colours were too dark by about a factor of two in the scene. Choose mid tones near (80,170,50) for grass, (150,113,80) for bark, then look.
7. INFERRED. Positions in other files (animated ants, flowers) assume the old ground heights. Keep heights; change only tessellation and colour.

## Reading layout out of generated files

- Open the JSON chunk first: generator name, node and mesh names, material, counts.
- Palette files: TEXCOORD_0.u * (number of colours) gives a colour index per point; indices classify parts (here ground 9..11 = earth; cap colour 18 red, 21 brown, 8 yellow).
- Pieces: weld points by rounded position, union-find over triangles, then count triangles per piece. Generated files have a few fixed counts (here 8 per blade, 120 trunk, 80 leaf ball, 36 root or bough, 210 cap; 84 is probably a stem). Check the total: 1 + sum(count x pieces) must equal the file's triangles.
- A piece's box gives position, size and height; lowest and highest points give lean.
- Ground height: rasterise the ground's triangles onto a grid (400x400 here), fill empty cells from neighbours, sample bilinearly.

## Shapes that paid off per triangle

- Grass blade: ribbon, 4 to 5 segments, quadratic bend, width tapering to a point, edge normals tilted outward by 0.5 of the side vector. 10 triangles.
- Trunk: tube 72 around x 30 rings; radius = taper x (1 + foot flare with one rib per root + furrow term + noise). Roots as tubes that follow the ground. Boughs as tubes on a quadratic curve to each crown.
- Crown: icosphere (one split) as a dark core plus about 70 four-point leaves on its surface, random direction biased outward and down.
- Mushroom: rings around an axis (`revolve`): gills as a disc with alternate columns darker, dome with a turned-under rim, warts as small fans, ring as a short cone.
- Ground: regular grid, normals from height differences, colour from two or three octaves of value noise.
- Variety is cheap and reads strongly: seed stalks, clover, rosettes, patches driven by low-frequency noise.

## Testing loop

- MEASURED. Headless Chromium with the software renderer spends minutes compiling this scene's shaders: the page looks hung, the main thread is in `ShaderSeq::GetShader`. It is not a deadlock. Use a browser with a GPU for pictures.
- MEASURED. Driving Chrome through the DevTools protocol (console, `Module.HEAPU8.length`, `Page.captureScreenshot`, `Debugger.pause` to see where a stuck page is) is enough; no test framework needed.
- MEASURED. `web/scene.html?script=<file>&from=<host in script>&to=<local folder>` runs a script against local copies of the models. The page must define `g_ready_bit` and `Module.onStartup`. The server must send COOP/COEP headers and answer Range requests (`tools/serve.py`).
- MEASURED. The desktop script runner (`GameApi/GameApi/cmdline/gameapi_cmdline --file <script> --homepage <url>`) loaded the models from `https://meshpage.org/` but got empty files from a local `http://127.0.0.1` server. Not investigated.
- Take pictures at the same seconds for old and new; the scene's camera moves by itself and not identically between runs, so compare the first seconds for a like-for-like view.
- Two test runs sharing one virtual X screen grab each other's pictures. Run them one after the other.

## Numbers from this job

| | old | new |
|---|---|---|
| ground | 1.5 MB, 22k triangles | 3.6 MB, 122k |
| grass | 5.0 MB, 84k | 11.9 MB, 248k |
| trees | 1.4 MB, 16k | 7.9 MB, 172k |
| mushrooms | 0.6 MB, 19k | 2.0 MB, 58k |
| browser heap, whole scene | 502 MB | 868 and 953 MB (two runs) |

The engine holds each file several times over: here the heap grew by 21 to 26 times the added file size. Budget triangles accordingly.

## Mistakes made here, to skip next time

- Naming a scratch script `inspect.py`: it shadows Python's standard module and breaks numpy.
- Reading a slow software-rendered page as a hang, and spending time on locks.
- Starting a server in the same shell command as a long test: stopping the test stopped the server.
- Leaving a hole where two revolved surfaces should meet (gills and stem): the inside of the cap showed through as a red spot.
- A JSON `operator[]` overloaded for both `const char*` and `size_t` is ambiguous for the literal `0`.

## Free-material rule

"Only free resources" was met by computing everything. If outside material is ever needed, record for each item its source URL and licence in the docs before using it, and prefer CC0.

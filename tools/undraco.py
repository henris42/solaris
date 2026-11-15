# Unpacks NASA's Voyager model (glTF binary with KHR_draco_mesh_compression and WebP
# textures) into plain files GameApi can read: one per textured part, with positions,
# normals and texture coordinates (part-NN.glb), and the part's texture (part-NN.jpg).
#
# Sunlight is worked out here, not by GameApi. The spacecraft keeps its dish (it
# opens to the model's +y) toward the Earth, and from the outer planets the Sun is within a few
# degrees of the Earth, so the light always comes down the dish's axis. Every
# triangle is written twice, once for each side, and each side gets the brightness
# of a matt surface: the cosine of the angle between the Sun and the side's normal
# (to the power 0.75, as on the moons), zero when it faces away, zero when another
# part of the spacecraft is between it and the Sun. Nothing is drawn darker than a
# fifth of full brightness, so that the unlit side can still be seen (the same on
# the moons). The brightness is in the
# texture: part-NN.jpg holds the part's texture eight times, from a fifth to full,
# four across and two down, and each triangle's texture coordinates point into the
# copy of its brightness. The two sides of a surface are a little out from it, each
# on its own side, so that the one toward the viewer is the one seen: the side the
# model's maker drew by 5 mm, its other side by 2 mm (where the model has a surface
# drawn twice, back to back, each then shows its own front).
# Usage: undraco.py <in.glb> <out folder>. Needs DracoPy, numpy and ImageMagick.
# Scratch tooling for the model; not part of the simulation or the game.
#
# The first triangle of each file is a flat one through two opposite corners of a
# cube around the model's origin: GameApi measures a model by sampling its triangles
# and scales it to its own size, and this triangle makes it measure the same box for
# every part, so that the parts fit together.
import json, os, struct, subprocess, sys, DracoPy, numpy as np
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read(); jl = struct.unpack('<I', d[12:16])[0]; j = json.loads(d[20:20 + jl]); binb = d[28 + jl:]
def chunk(view):
    bv = j['bufferViews'][view]; return binb[bv.get('byteOffset', 0):bv.get('byteOffset', 0) + bv['byteLength']]
parts = []
for node in j['nodes']:
    if 'mesh' not in node: continue
    for p in j['meshes'][node['mesh']]['primitives']:
        if p.get('material') is None: continue                       # a helper shape without a surface
        m = DracoPy.decode(chunk(p['extensions']['KHR_draco_mesh_compression']['bufferView']))
        pos = np.asarray(m.points, dtype='<f4').reshape(-1, 3) * np.array(node.get('scale', [1, 1, 1]), dtype='<f4') + np.array(node.get('translation', [0, 0, 0]), dtype='<f4')
        nrm = np.asarray(m.normals, dtype='<f4').reshape(-1, 3); uv = np.asarray(m.tex_coord, dtype='<f4').reshape(-1, 2)
        idx = np.asarray(m.faces, dtype='<u4').reshape(-1)
        tex = j['textures'][j['materials'][p['material']]['pbrMetallicRoughness']['baseColorTexture']['index']]
        parts.append((pos.astype('<f4'), nrm, uv, idx, tex['source'], j['materials'][p['material']].get('name', '')))
centre = np.median(np.vstack([p[0] for p in parts]), axis=0)   # the body of the spacecraft, not the middle of its long boom
half = float(max(np.abs(p[0] - centre).max() for p in parts))
os.makedirs(dst, exist_ok=True)
GAIN = 1.7  # the model's textures are painted dim (its white dish a middle grey): brought up so that white paint in sunlight is white
SUN = 1.0   # the Sun is up the model's +y axis: the side the dish opens to (its rim is higher in y than its middle)
# what is nearest the Sun over every spot: the height toward the Sun of the topmost surface, on a grid across x and z
cells = 900; size = 2.0 * half / cells
top = np.full((cells, cells), -1e9)
for pos, nrm, uv, idx, image, name in parts:
    q = (pos - centre).astype('f8') * [1, SUN, 1]; tri = idx.reshape(-1, 3)
    for a, b, c in q[tri]:
        lo = np.floor((np.minimum(np.minimum(a, b), c)[[0, 2]] + half) / size).astype(int); hi = np.floor((np.maximum(np.maximum(a, b), c)[[0, 2]] + half) / size).astype(int)
        lo = np.clip(lo, 0, cells - 1); hi = np.clip(hi, 0, cells - 1)
        den = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
        if abs(den) < 1e-14: continue                                 # edge on to the Sun: shades nothing
        x, z = np.meshgrid((np.arange(lo[0], hi[0] + 1) + 0.5) * size - half, (np.arange(lo[1], hi[1] + 1) + 0.5) * size - half, indexing='ij')
        w0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / den; w1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / den; w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        y = np.where(inside, w0 * a[1] + w1 * b[1] + w2 * c[1], -1e9)
        view_ = top[lo[0]:hi[0] + 1, lo[1]:hi[1] + 1]; np.maximum(view_, y, out=view_)
for n, (pos, nrm, uv, idx, image, name) in enumerate(parts):
    png = '%s/tex-%d.png' % (dst, n)
    open(png, 'wb').write(chunk(j['images'][image]['bufferView']))
    tiles = []
    for lv in range(8):
        tiles.append('%s/tile-%d.png' % (dst, lv))
        subprocess.run(['convert', png, '-resize', '1024x1024!', '-evaluate', 'multiply', str(GAIN * (0.2 + 0.8 * lv / 7.0)), tiles[-1]], check=True)
    subprocess.run(['montage'] + tiles + ['-tile', '4x2', '-geometry', '+0+0', '-strip', '-interlace', 'none', '-quality', '88', '%s/part-%02d.jpg' % (dst, n)], check=True)
    for f in tiles + [png]: os.remove(f)
    q = (pos - centre).astype('f8'); tri = idx.reshape(-1, 3)
    P = []; N = []; T = []; lit = 0
    for i0, i1, i2 in tri:
        a, b, c = q[i0], q[i1], q[i2]
        g = np.cross(b - a, c - a); area = np.linalg.norm(g)
        if area < 1e-16: continue
        g /= area; mid = (a + b + c) / 3.0
        cell = np.clip(np.floor((mid[[0, 2]] + half) / size).astype(int), 0, cells - 1)
        for order, normal, lift in (((i0, i1, i2), g, 0.005), ((i0, i2, i1), -g, 0.002)):
            cos = SUN * float(normal[1])
            # in shadow: something above this spot, by more than the surface's own slope across a grid cell
            shaded = cos > 0 and top[cell[0], cell[1]] > SUN * mid[1] + 0.02 + min(0.5, 2.0 * size * (1 - cos * cos) ** 0.5 / max(cos, 1e-6))
            level = 0 if cos <= 0 or shaded else int(round(7 * cos ** 0.75))
            lit += level > 0
            for i in order:
                u, v = np.clip(uv[i] % 1.0 if (uv[i].max() > 1 or uv[i].min() < 0) else uv[i], 0.004, 0.996)
                out_ = nrm[i].astype('f8') * (1.0 if np.dot(nrm[i], normal) >= 0 else -1.0)   # the model's own (rounded) normal, on this side
                P.append(q[i] + lift * out_); N.append(normal); T.append([(level % 4 + u) / 4.0, (level // 4 + v) / 2.0])
    pos = np.vstack([[[-half] * 3, [half] * 3, [-half] * 3], P]).astype('<f4'); nrm = np.vstack([[[0, 1, 0]] * 3, N]).astype('<f4')
    uv = np.vstack([[[0, 0]] * 3, T]).astype('<f4'); idx = np.arange(len(pos)).astype('<u4')
    out = bytearray(); views = []
    def view(b, target=None):
        off = len(out); out.extend(b)
        while len(out) % 4: out.append(0)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(b)}
        if target: v["target"] = target
        views.append(v); return len(views) - 1
    g = {"asset": {"version": "2.0", "generator": "NASA 3D Resources, Voyager Probe (B), part '%s', unpacked, sunlight in the texture (part-%02d.jpg)" % (name, n)}, "scene": 0, "scenes": [{"nodes": [0]}],
         "nodes": [{"mesh": 0, "name": name}],
         "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0, "mode": 4}]}],
         "accessors": [{"bufferView": view(pos.tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC3", "min": pos.min(0).tolist(), "max": pos.max(0).tolist()},
                       {"bufferView": view(nrm.tobytes(), 34962), "componentType": 5126, "count": len(nrm), "type": "VEC3"},
                       {"bufferView": view(uv.tobytes(), 34962), "componentType": 5126, "count": len(uv), "type": "VEC2"},
                       {"bufferView": view(idx.tobytes(), 34963), "componentType": 5125, "count": len(idx), "type": "SCALAR"}],
         "materials": [{"name": name, "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.8, 0.8, 1], "metallicFactor": 0, "roughnessFactor": 0.6}}]}
    g["buffers"] = [{"byteLength": len(out)}]; g["bufferViews"] = views
    js = json.dumps(g).encode(); js += b' ' * ((4 - len(js) % 4) % 4)
    open('%s/part-%02d.glb' % (dst, n), 'wb').write(struct.pack('<III', 0x46546C67, 2, 28 + len(js) + len(out)) + struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(out), 0x004E4942) + bytes(out))
    print('part-%02d.glb  %-16s %6d triangles, %d sides in sunlight' % (n, name, len(tri), lit // 3))
print('half size of the model', round(half, 3), 'centre', centre.round(3).tolist())

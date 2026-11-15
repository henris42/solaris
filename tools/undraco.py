# Unpacks NASA's Voyager model (glTF binary with KHR_draco_mesh_compression and WebP
# textures) into plain files GameApi can read: one per textured part, with positions,
# normals, texture coordinates and the part's texture as JPEG.
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
for n, (pos, nrm, uv, idx, image, name) in enumerate(parts):
    png = '%s/tex-%d.png' % (dst, n); jpg = '%s/tex-%d.jpg' % (dst, n)
    open(png, 'wb').write(chunk(j['images'][image]['bufferView']))
    subprocess.run(['convert', png, '-resize', '2048x2048>', '-strip', '-interlace', 'none', '-quality', '88', jpg], check=True)
    jpeg = open(jpg, 'rb').read(); os.remove(png); os.remove(jpg)
    pos = pos - centre
    pos = np.vstack([[[-half] * 3, [half] * 3, [-half] * 3], pos]).astype('<f4'); nrm = np.vstack([[[0, 1, 0]] * 3, nrm]).astype('<f4')
    uv = np.vstack([[[0, 0]] * 3, uv]).astype('<f4'); idx = np.concatenate([[0, 1, 2], idx + 3]).astype('<u4')
    out = bytearray(); views = []
    def view(b, target=None):
        off = len(out); out.extend(b)
        while len(out) % 4: out.append(0)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(b)}
        if target: v["target"] = target
        views.append(v); return len(views) - 1
    g = {"asset": {"version": "2.0", "generator": "NASA 3D Resources, Voyager Probe (B), part '%s', unpacked" % name}, "scene": 0, "scenes": [{"nodes": [0]}],
         "nodes": [{"mesh": 0, "name": name}],
         "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0, "mode": 4}]}],
         "accessors": [{"bufferView": view(pos.tobytes(), 34962), "componentType": 5126, "count": len(pos), "type": "VEC3", "min": pos.min(0).tolist(), "max": pos.max(0).tolist()},
                       {"bufferView": view(nrm.tobytes(), 34962), "componentType": 5126, "count": len(nrm), "type": "VEC3"},
                       {"bufferView": view(uv.tobytes(), 34962), "componentType": 5126, "count": len(uv), "type": "VEC2"},
                       {"bufferView": view(idx.tobytes(), 34963), "componentType": 5125, "count": len(idx), "type": "SCALAR"}],
         "images": [{"bufferView": view(jpeg), "mimeType": "image/jpeg"}], "samplers": [{"magFilter": 9729, "minFilter": 9729}], "textures": [{"source": 0, "sampler": 0}],
         "materials": [{"name": name, "doubleSided": True, "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0, "roughnessFactor": 0.6}}]}
    g["buffers"] = [{"byteLength": len(out)}]; g["bufferViews"] = views
    js = json.dumps(g).encode(); js += b' ' * ((4 - len(js) % 4) % 4)
    open('%s/part-%02d.glb' % (dst, n), 'wb').write(struct.pack('<III', 0x46546C67, 2, 28 + len(js) + len(out)) + struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(out), 0x004E4942) + bytes(out))
    print('part-%02d.glb  %-16s %6d triangles, texture %d KB' % (n, name, len(idx) // 3 - 1, len(jpeg) // 1024))
print('half size of the model', round(half, 3), 'centre', centre.round(3).tolist())

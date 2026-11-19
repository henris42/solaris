// antforest: new ground, grass, trees and mushrooms for the ant scene.
//
//   antforest <folder with the scene's files> <output folder>
//
// Reads antscene_ground.glb (heights, and where the bare earth is), antscene_grass.glb (where
// the blades stand), antscene_trees.glb (where the trunks and crowns are) and
// antscene_mushrooms.glb (where the caps are, how large and of which kind), and writes the four
// files anew, in the same box and the same form (one mesh, one material, one embedded image),
// so that they replace the old files as they are. The ground keeps its heights: the ants,
// flowers and the hill stand on it as before.
//
// Everything is computed here: no model, texture or other material from anywhere else.
//
//   c++ -std=c++20 -O2 antforest.cpp -o antforest
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace {

const double pi = 3.14159265358979323846;

struct v3 {
  double x = 0, y = 0, z = 0;
};
v3 operator+(v3 a, v3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
v3 operator-(v3 a, v3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
v3 operator*(v3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
v3 cross(v3 a, v3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(v3 a) { return std::sqrt(dot(a, a)); }
v3 unit(v3 a) {
  const double l = len(a);
  return l > 1e-12 ? a * (1 / l) : v3{0, 1, 0};
}
v3 mix(v3 a, v3 b, double t) { return a * (1 - t) + b * t; }
double clamp01(double a) { return std::clamp(a, 0.0, 1.0); }
double smooth(double a) {
  a = clamp01(a);
  return a * a * (3 - 2 * a);
}
const v3 up{0, 1, 0};
v3 flat(double angle) { return {std::cos(angle), 0, std::sin(angle)}; }

// ---- chance and noise --------------------------------------------------------------------

std::uint64_t seed = 0x2545F4914F6CDD1Dull;
double rnd() {  // 0 .. 1
  seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
  return static_cast<double>(seed >> 11) / 9007199254740992.0;
}
double rnd(double a, double b) { return a + (b - a) * rnd(); }

double hash3(int x, int y, int z) {
  std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + static_cast<std::uint32_t>(z) * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<double>((h ^ (h >> 16)) & 0xffffff) / 16777215.0;
}
double noise(v3 p) {  // 0 .. 1, smooth
  const int x = static_cast<int>(std::floor(p.x)), y = static_cast<int>(std::floor(p.y)), z = static_cast<int>(std::floor(p.z));
  const double fx = smooth(p.x - x), fy = smooth(p.y - y), fz = smooth(p.z - z);
  double c[2][2];
  for (int j = 0; j < 2; ++j)
    for (int k = 0; k < 2; ++k) c[j][k] = hash3(x, y + j, z + k) * (1 - fx) + hash3(x + 1, y + j, z + k) * fx;
  return (c[0][0] * (1 - fy) + c[1][0] * fy) * (1 - fz) + (c[0][1] * (1 - fy) + c[1][1] * fy) * fz;
}

// ---- reading the scene's files -----------------------------------------------------------

struct json {
  std::map<std::string, json> members;
  std::vector<json> items;
  double number = 0;
  std::string text;
  const json& operator[](const char* k) const { return members.at(k); }
  const json& operator[](int i) const { return items.at(static_cast<std::size_t>(i)); }
  bool has(const char* k) const { return members.count(k) != 0; }
  int integer() const { return static_cast<int>(number); }
};

struct json_reader {
  const std::string& s;
  std::size_t at = 0;
  void space() {
    while (at < s.size() && std::strchr(" \t\r\n", s[at])) ++at;
  }
  std::string string() {
    std::string out;
    for (++at; at < s.size() && s[at] != '"'; ++at) {
      if (s[at] == '\\') ++at;
      out += s[at];
    }
    ++at;
    return out;
  }
  json value() {
    json v;
    space();
    if (s[at] == '{') {
      for (++at, space(); s[at] != '}'; space()) {
        if (s[at] == ',') { ++at; continue; }
        space();
        const std::string key = string();
        space(), ++at;  // the colon
        v.members[key] = value();
      }
      ++at;
    } else if (s[at] == '[') {
      for (++at, space(); s[at] != ']'; space()) {
        if (s[at] == ',') { ++at; continue; }
        v.items.push_back(value());
      }
      ++at;
    } else if (s[at] == '"') {
      v.text = string();
    } else {
      const std::size_t from = at;
      while (at < s.size() && !std::strchr(",]} \t\r\n", s[at])) ++at;
      v.text = s.substr(from, at - from);
      v.number = std::atof(v.text.c_str());
    }
    return v;
  }
};

struct triangles {
  std::vector<v3> points;
  std::vector<int> colour;             // per point: which of the old palette's 32 colours
  std::vector<std::uint32_t> corners;  // three per triangle, the first triangle (the box) left out
};

triangles read_glb(const std::string& file) {
  std::ifstream in(file, std::ios::binary);
  const std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (d.size() < 28 || d.compare(0, 4, "glTF") != 0) {
    std::fprintf(stderr, "antforest: %s is not a glb file\n", file.c_str());
    std::exit(1);
  }
  std::uint32_t n;
  std::memcpy(&n, &d[12], 4);
  const std::string text = d.substr(20, n);
  const json j = json_reader{text}.value();
  const char* bin = d.data() + 28 + n;
  const json& prim = j["meshes"][0]["primitives"][0];
  const auto start = [&](const json& a) {
    const json& view = j["bufferViews"][a["bufferView"].integer()];
    return bin + (view.has("byteOffset") ? view["byteOffset"].integer() : 0) + (a.has("byteOffset") ? a["byteOffset"].integer() : 0);
  };
  triangles t;
  const json& pos = j["accessors"][prim["attributes"]["POSITION"].integer()];
  const char* p = start(pos);
  for (int i = 0; i < pos["count"].integer(); ++i) {
    float f[3];
    std::memcpy(f, p + 12 * i, 12);
    t.points.push_back({f[0], f[1], f[2]});
  }
  const json& tex = j["accessors"][prim["attributes"]["TEXCOORD_0"].integer()];
  const char* uv = start(tex);
  for (int i = 0; i < tex["count"].integer(); ++i) {
    float f[2];
    std::memcpy(f, uv + 8 * i, 8);
    t.colour.push_back(std::clamp(static_cast<int>(f[0] * 32), 0, 31));
  }
  const json& idx = j["accessors"][prim["indices"].integer()];
  const char* q = start(idx);
  const bool wide = idx["componentType"].integer() == 5125;
  for (int i = 3; i < idx["count"].integer(); ++i) {
    std::uint32_t a = 0;
    std::memcpy(&a, q + (wide ? 4 : 2) * i, wide ? 4 : 2);
    t.corners.push_back(a);
  }
  return t;
}

// ---- the ground ----------------------------------------------------------------------------

const int cells = 400;
const double box = 8000;
std::vector<float> heights(cells * cells, -1e9f), earth(cells * cells, 0.0f);  // earth: 1 where the old ground is bare

void read_ground(const triangles& t) {
  const auto cell = [](double a) { return (a + box) / (2 * box) * cells - 0.5; };
  for (std::size_t i = 0; i + 2 < t.corners.size(); i += 3) {
    const v3 a = t.points[t.corners[i]], b = t.points[t.corners[i + 1]], c = t.points[t.corners[i + 2]];
    const double den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
    if (std::abs(den) < 1e-9) continue;
    const int x0 = std::max(0, static_cast<int>(std::floor(cell(std::min({a.x, b.x, c.x}))))), x1 = std::min(cells - 1, static_cast<int>(std::ceil(cell(std::max({a.x, b.x, c.x})))));
    const int z0 = std::max(0, static_cast<int>(std::floor(cell(std::min({a.z, b.z, c.z}))))), z1 = std::min(cells - 1, static_cast<int>(std::ceil(cell(std::max({a.z, b.z, c.z})))));
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x) {
        const double px = (x + 0.5) / cells * 2 * box - box, pz = (z + 0.5) / cells * 2 * box - box;
        const double u = ((b.z - c.z) * (px - c.x) + (c.x - b.x) * (pz - c.z)) / den, v = ((c.z - a.z) * (px - c.x) + (a.x - c.x) * (pz - c.z)) / den;
        if (u < -0.02 || v < -0.02 || u + v > 1.02) continue;
        heights[z * cells + x] = std::max(heights[z * cells + x], static_cast<float>(u * a.y + v * b.y + (1 - u - v) * c.y));
        const int old = t.colour[t.corners[i]];
        earth[z * cells + x] = old >= 9 && old <= 11 ? 1.0f : 0.0f;  // the old palette's three browns
      }
  }
  for (int pass = 0; pass < cells; ++pass) {  // cells no triangle reached take their neighbours' height
    bool open = false;
    std::vector<float> next = heights;
    for (int z = 0; z < cells; ++z)
      for (int x = 0; x < cells; ++x) {
        if (heights[z * cells + x] > -1e8f) continue;
        double sum = 0;
        int count = 0;
        for (int k = 0; k < 4; ++k) {
          const int xx = x + (k == 0) - (k == 1), zz = z + (k == 2) - (k == 3);
          if (xx < 0 || zz < 0 || xx >= cells || zz >= cells || heights[zz * cells + xx] < -1e8f) continue;
          sum += heights[zz * cells + xx], ++count;
        }
        if (count) next[z * cells + x] = static_cast<float>(sum / count);
        else open = true;
      }
    heights = next;
    if (!open) break;
  }
  for (int pass = 0; pass < 3; ++pass) {  // soften the edge between earth and green
    std::vector<float> next = earth;
    for (int z = 1; z + 1 < cells; ++z)
      for (int x = 1; x + 1 < cells; ++x) next[z * cells + x] = (earth[z * cells + x] * 2 + earth[z * cells + x - 1] + earth[z * cells + x + 1] + earth[(z - 1) * cells + x] + earth[(z + 1) * cells + x]) / 6;
    earth = next;
  }
}

double on_grid(const std::vector<float>& grid, double x, double z) {
  const double fx = std::clamp((x + box) / (2 * box) * cells - 0.5, 0.0, cells - 1.001), fz = std::clamp((z + box) / (2 * box) * cells - 0.5, 0.0, cells - 1.001);
  const int ix = static_cast<int>(fx), iz = static_cast<int>(fz);
  const double tx = fx - ix, tz = fz - iz;
  const auto h = [&](int a, int b) { return static_cast<double>(grid[b * cells + a]); };
  return (h(ix, iz) * (1 - tx) + h(ix + 1, iz) * tx) * (1 - tz) + (h(ix, iz + 1) * (1 - tx) + h(ix + 1, iz + 1) * tx) * tz;
}
double ground(double x, double z) { return on_grid(heights, x, z); }

// ---- the colours ---------------------------------------------------------------------------
//
// One image, eight bands from top to bottom. Across (u) each band runs from shade to light; down
// the band (v) it runs from one kind of colour to another: fresh to dry grass, dark to light
// leaves, bark to moss, straw to seed, bare earth to green floor, red to brown caps, cream to
// white, grey to brown stone. A point of a mesh picks its colour with (shade, band, kind).

enum band { grass_band, leaf_band, bark_band, straw_band, earth_band, cap_band, pale_band, stone_band };
const int image_size = 256, band_height = image_size / 8;

struct rgb {
  double r, g, b;
};
struct ramp {
  rgb dark, mid, light;
};
const ramp ramps[8][2] = {
    {{{22, 66, 18}, {78, 168, 46}, {186, 232, 104}}, {{70, 70, 20}, {172, 172, 62}, {234, 226, 130}}},
    {{{18, 60, 22}, {62, 148, 48}, {150, 216, 84}}, {{48, 70, 14}, {134, 170, 40}, {214, 230, 96}}},
    {{{64, 46, 33}, {150, 113, 80}, {216, 182, 142}}, {{38, 66, 22}, {96, 146, 44}, {166, 204, 80}}},
    {{{92, 72, 32}, {188, 160, 92}, {240, 224, 160}}, {{96, 48, 30}, {176, 104, 70}, {232, 176, 130}}},
    {{{58, 40, 26}, {134, 98, 62}, {198, 160, 112}}, {{20, 56, 18}, {50, 112, 34}, {104, 162, 58}}},
    {{{110, 12, 8}, {214, 34, 20}, {250, 96, 44}}, {{70, 40, 20}, {150, 92, 46}, {214, 160, 96}}},
    {{{120, 104, 70}, {226, 208, 160}, {250, 240, 210}}, {{150, 146, 136}, {236, 232, 222}, {255, 254, 250}}},
    {{{54, 54, 52}, {136, 134, 128}, {206, 204, 196}}, {{62, 50, 38}, {138, 114, 88}, {200, 178, 146}}},
};

std::vector<std::uint8_t> image_pixels() {
  std::vector<std::uint8_t> px(image_size * image_size * 3);
  for (int y = 0; y < image_size; ++y)
    for (int x = 0; x < image_size; ++x) {
      const int b = y / band_height;
      const double kind = clamp01((y % band_height - 4.0) / (band_height - 9.0)), shade = clamp01((x - 2.0) / (image_size - 5.0));
      const auto at = [&](const ramp& r) {
        const rgb a = shade < 0.5 ? r.dark : r.mid, c = shade < 0.5 ? r.mid : r.light;
        const double t = shade < 0.5 ? shade * 2 : shade * 2 - 1;
        return rgb{a.r + (c.r - a.r) * t, a.g + (c.g - a.g) * t, a.b + (c.b - a.b) * t};
      };
      const rgb one = at(ramps[b][0]), two = at(ramps[b][1]);
      std::uint8_t* p = &px[(y * image_size + x) * 3];
      p[0] = static_cast<std::uint8_t>(one.r + (two.r - one.r) * kind + 0.5);
      p[1] = static_cast<std::uint8_t>(one.g + (two.g - one.g) * kind + 0.5);
      p[2] = static_cast<std::uint8_t>(one.b + (two.b - one.b) * kind + 0.5);
    }
  return px;
}

// A PNG file without compression (the image is small).
std::string png(const std::vector<std::uint8_t>& px, int w, int h) {
  std::uint32_t table[256];
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
  const auto be = [](std::uint32_t a) { return std::string{static_cast<char>(a >> 24), static_cast<char>(a >> 16), static_cast<char>(a >> 8), static_cast<char>(a)}; };
  const auto chunk = [&](const char* name, const std::string& data) {
    const std::string body = std::string(name) + data;
    std::uint32_t c = 0xffffffffu;
    for (const unsigned char ch : body) c = table[(c ^ ch) & 0xff] ^ (c >> 8);
    return be(static_cast<std::uint32_t>(data.size())) + body + be(~c);
  };
  std::string raw;
  for (int y = 0; y < h; ++y) {
    raw += '\0';
    raw.append(reinterpret_cast<const char*>(&px[y * w * 3]), w * 3);
  }
  std::string z = "\x78\x01";
  std::uint32_t a = 1, b = 0;
  for (const unsigned char ch : raw) a = (a + ch) % 65521, b = (b + a) % 65521;
  for (std::size_t at = 0; at < raw.size(); at += 65535) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
    z += static_cast<char>(at + n == raw.size());
    z += static_cast<char>(n & 0xff), z += static_cast<char>(n >> 8), z += static_cast<char>(~n & 0xff), z += static_cast<char>((~n >> 8) & 0xff);
    z.append(raw, at, n);
  }
  z += be((b << 16) | a);
  std::string head = be(w) + be(h);
  head += '\x08', head += '\x02', head += '\0', head += '\0', head += '\0';
  return std::string("\x89PNG\r\n\x1a\n") + chunk("IHDR", head) + chunk("IDAT", z) + chunk("IEND", "");
}

// ---- a mesh --------------------------------------------------------------------------------

struct mesh {
  std::vector<float> points, normals, colours;
  std::vector<std::uint32_t> corners;

  mesh() {  // the triangle that spans the scene's box, as in the scene's other files
    for (const double s : {-box, box, -box}) add({s, s, s}, up, 0.5, grass_band, 0);
    tri(0, 1, 2);
  }
  std::uint32_t count() const { return static_cast<std::uint32_t>(points.size() / 3); }
  std::uint32_t add(v3 p, v3 n, double shade, band b, double kind) {
    points.insert(points.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)});
    normals.insert(normals.end(), {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)});
    colours.insert(colours.end(), {static_cast<float>((2 + clamp01(shade) * (image_size - 5)) / image_size), static_cast<float>((b * band_height + 4 + clamp01(kind) * (band_height - 9)) / image_size)});
    return count() - 1;
  }
  void tri(std::uint32_t a, std::uint32_t b, std::uint32_t c) { corners.insert(corners.end(), {a, b, c}); }
  v3 point(std::uint32_t i) const { return {points[3 * i], points[3 * i + 1], points[3 * i + 2]}; }

  // Smooth normals for the points from `first` on, from the triangles from `first_corner` on,
  // turned to the side `outward` names for each point.
  void round_off(std::uint32_t first, std::size_t first_corner, const std::function<v3(std::uint32_t)>& outward) {
    std::vector<v3> sum(count() - first);
    for (std::size_t i = first_corner; i + 2 < corners.size(); i += 3) {
      const v3 n = cross(point(corners[i + 1]) - point(corners[i]), point(corners[i + 2]) - point(corners[i]));
      for (int k = 0; k < 3; ++k) sum[corners[i + k] - first] = sum[corners[i + k] - first] + n;
    }
    for (std::uint32_t i = first; i < count(); ++i) {
      v3 n = unit(sum[i - first]);
      if (dot(n, outward(i)) < 0) n = n * -1;
      normals[3 * i] = static_cast<float>(n.x), normals[3 * i + 1] = static_cast<float>(n.y), normals[3 * i + 2] = static_cast<float>(n.z);
    }
  }

  void write(const std::string& file, const char* name) const {
    const std::string image = png(image_pixels(), image_size, image_size);
    const std::size_t parts[5] = {corners.size() * 4, points.size() * 4, normals.size() * 4, colours.size() * 4, image.size()};
    std::size_t at[5], total = 0;
    for (int i = 0; i < 5; ++i) at[i] = total, total += (parts[i] + 3) / 4 * 4;
    char text[4000];
    std::snprintf(text, sizeof text,
                  "{\"asset\":{\"version\":\"2.0\",\"generator\":\"antforest\"},\"scene\":0,\"scenes\":[{\"name\":\"Scene\",\"nodes\":[0]}],"
                  "\"nodes\":[{\"name\":\"%s\",\"mesh\":0}],"
                  "\"meshes\":[{\"name\":\"%s\",\"primitives\":[{\"attributes\":{\"POSITION\":1,\"NORMAL\":2,\"TEXCOORD_0\":3},\"indices\":0,\"material\":0}]}],"
                  "\"materials\":[{\"name\":\"palette\",\"doubleSided\":true,\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,1,1,1],\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0.0,\"roughnessFactor\":0.85}}],"
                  "\"textures\":[{\"sampler\":0,\"source\":0}],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,\"wrapS\":33071,\"wrapT\":33071}],"
                  "\"images\":[{\"name\":\"palette\",\"mimeType\":\"image/png\",\"bufferView\":4}],"
                  "\"accessors\":[{\"bufferView\":0,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"},"
                  "{\"bufferView\":1,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\",\"min\":[-8000,-8000,-8000],\"max\":[8000,8000,8000]},"
                  "{\"bufferView\":2,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\"},{\"bufferView\":3,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}],"
                  "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},"
                  "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},"
                  "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu}],\"buffers\":[{\"byteLength\":%zu}]}",
                  name, name, corners.size(), count(), count(), count(), at[0], parts[0], at[1], parts[1], at[2], parts[2], at[3], parts[3], at[4], parts[4], total);
    std::string j = text;
    while (j.size() % 4) j += ' ';
    std::string bin(total, '\0');
    std::memcpy(&bin[at[0]], corners.data(), parts[0]);
    std::memcpy(&bin[at[1]], points.data(), parts[1]);
    std::memcpy(&bin[at[2]], normals.data(), parts[2]);
    std::memcpy(&bin[at[3]], colours.data(), parts[3]);
    std::memcpy(&bin[at[4]], image.data(), parts[4]);
    std::ofstream out(file, std::ios::binary);
    const auto word = [&](std::uint32_t a) { out.write(reinterpret_cast<const char*>(&a), 4); };
    out.write("glTF", 4), word(2), word(static_cast<std::uint32_t>(28 + j.size() + bin.size()));
    word(static_cast<std::uint32_t>(j.size())), out.write("JSON", 4), out.write(j.data(), static_cast<std::streamsize>(j.size()));
    word(static_cast<std::uint32_t>(bin.size())), out.write("BIN\0", 4), out.write(bin.data(), static_cast<std::streamsize>(bin.size()));
    std::printf("%s: %u points, %zu triangles, %.1f MB\n", file.c_str(), count(), corners.size() / 3, (28 + j.size() + bin.size()) / 1e6);
  }
};

struct colour {
  double shade, kind;
};

// A tube along a line of centres. `swell(along, angle)` scales the radius, `paint(along, angle,
// swell)` gives the colour; along runs 0 .. 1.
void tube(mesh& m, const std::vector<v3>& centres, const std::vector<double>& radii, int around, band b, const std::function<double(double, double)>& swell,
          const std::function<colour(double, double, double)>& paint) {
  const std::uint32_t first = m.count();
  const std::size_t first_corner = m.corners.size();
  const int rings = static_cast<int>(centres.size());
  std::vector<v3> middle;
  v3 side{1, 0, 0};
  for (int j = 0; j < rings; ++j) {
    const v3 along = unit(centres[std::min(j + 1, rings - 1)] - centres[std::max(j - 1, 0)]);
    side = unit(side - along * dot(side, along));  // carried along, so that the tube does not twist
    const v3 other = cross(along, side);
    for (int k = 0; k < around; ++k) {
      const double angle = 2 * pi * k / around, s = static_cast<double>(j) / (rings - 1), f = swell(s, angle);
      const colour c = paint(s, angle, f);
      m.add(centres[j] + (side * std::cos(angle) + other * std::sin(angle)) * (radii[j] * f), up, c.shade, b, c.kind);
      middle.push_back(centres[j]);
    }
  }
  for (int j = 0; j + 1 < rings; ++j)
    for (int k = 0; k < around; ++k) {
      const std::uint32_t a = first + j * around + k, c = first + j * around + (k + 1) % around;
      m.tri(a, c, c + around), m.tri(a, c + around, a + around);
    }
  m.round_off(first, first_corner, [&](std::uint32_t i) { return m.point(i) - middle[i - first]; });
}

// ---- grass ---------------------------------------------------------------------------------

// One blade: a ribbon that leans over in the direction `lean`, widest low down, ending in a point.
// Dark at the foot, light at the tip; the edges' normals are turned outward, which rounds it.
void blade(mesh& m, v3 foot, double height, double lean, double bend, double width, double kind, double light, int parts) {
  const v3 dir = flat(lean), side = flat(lean + pi / 2 + rnd(-0.3, 0.3));
  const std::uint32_t first = m.count();
  for (int j = 0; j <= parts; ++j) {
    const double s = static_cast<double>(j) / parts;
    const v3 c = foot + dir * (bend * height * s * s * 0.75) + up * (height * (s - 0.25 * bend * s * s) - 6);
    const v3 along = unit(dir * (1.5 * bend * s) + up * (1 - 0.5 * bend * s));
    const double w = width * (s < 0.12 ? 0.75 + 2 * s : std::pow(1 - (s - 0.12) / 0.88, 0.75));
    const v3 face = unit(cross(side, along));
    const double shade = light * (0.18 + 0.76 * std::pow(s, 0.7)), k = kind + 0.4 * s * s * s;
    m.add(c - side * w, unit(face - side * 0.5), shade * 0.92, grass_band, k);
    m.add(c + side * w, unit(face + side * 0.5), shade, grass_band, k);
  }
  for (int j = 0; j < parts; ++j) {
    const std::uint32_t a = first + 2 * j;
    m.tri(a, a + 1, a + 3), m.tri(a, a + 3, a + 2);
  }
}

// A stalk with an ear of seeds.
void seed_stalk(mesh& m, v3 foot, double height, double lean) {
  const v3 dir = flat(lean);
  const double bend = rnd(0.08, 0.3), kind = rnd(0.0, 0.7);
  const auto at = [&](double s) { return foot + dir * (bend * height * s * s) + up * (height * s - 6); };
  std::vector<v3> centres;
  std::vector<double> radii;
  for (int j = 0; j <= 6; ++j) centres.push_back(at(j / 6.0)), radii.push_back(7 - 3.5 * j / 6.0);
  tube(m, centres, radii, 4, straw_band, [](double, double) { return 1.0; }, [&](double s, double, double) { return colour{0.35 + 0.4 * s, 0.1}; });
  const int seeds = 14;
  for (int i = 0; i < seeds; ++i) {
    const double s = 0.70 + 0.30 * i / seeds, turn = i * 2.4;
    const v3 out = flat(turn), c = at(s) + out * 9, across = flat(turn + pi / 2);
    const double size = (22 + 16 * std::sin(pi * (i + 0.5) / seeds)) * height / 900;
    const v3 tip = c + up * (2.2 * size) + out * (1.1 * size), n = unit(out + up * 0.4);
    const double shade = rnd(0.55, 0.95);
    const std::uint32_t a = m.add(c, n, shade * 0.7, straw_band, kind), b = m.add(c + up * size + out * (0.45 * size) - across * (0.45 * size), n, shade, straw_band, kind);
    const std::uint32_t d = m.add(c + up * size + out * (0.45 * size) + across * (0.45 * size), n, shade, straw_band, kind), e = m.add(tip, n, shade * 1.05, straw_band, kind);
    m.tri(a, b, d), m.tri(b, e, d);
  }
}

// A clover: a thin stalk with three round leaflets, each with a pale middle.
void clover(mesh& m, v3 foot, double height, double size) {
  const double turn = rnd(0, 2 * pi), tilt = rnd(-0.25, 0.25), kind = rnd(0.0, 0.35);
  const v3 top = foot + up * height + flat(turn) * (height * tilt);
  std::vector<v3> centres;
  std::vector<double> radii;
  for (int j = 0; j <= 3; ++j) centres.push_back(mix(foot - up * 6, top, j / 3.0) + flat(turn) * (std::sin(pi * j / 3.0) * height * 0.06)), radii.push_back(5 - j * 0.7);
  tube(m, centres, radii, 3, leaf_band, [](double, double) { return 1.0; }, [&](double s, double, double) { return colour{0.4 + 0.3 * s, kind}; });
  for (int leaf = 0; leaf < 3; ++leaf) {
    const double a0 = turn + leaf * 2 * pi / 3 + rnd(-0.15, 0.15), droop = rnd(0.05, 0.35);
    const v3 out = flat(a0), across = flat(a0 + pi / 2);
    const v3 n = unit(up + out * droop);
    const std::uint32_t hub = m.add(top + out * (size * 0.55) - up * (size * 0.55 * droop) + up * 3, n, 0.86, leaf_band, kind + 0.35);
    const int rim = 9;
    std::uint32_t before = 0, start = 0;
    for (int k = 0; k < rim; ++k) {
      const double a = 2 * pi * k / rim + pi;  // k = 0 at the stalk
      const double notch = 1 - 0.22 * std::exp(-8 * (1 + std::cos(a + pi)) );  // a dent at the far end
      const double along = size * (0.55 + 0.52 * std::cos(a) * notch), wide = size * 0.50 * std::sin(a);
      const v3 p = top + out * along + across * wide - up * (along * droop) + up * (std::abs(std::sin(a)) * size * 0.08);
      const std::uint32_t v = m.add(p, unit(n + across * (0.3 * std::sin(a))), 0.42 + 0.14 * rnd(), leaf_band, kind);
      if (k) m.tri(hub, before, v);
      else start = v;
      before = v;
    }
    m.tri(hub, before, start);
  }
}

// A rosette of broad leaves lying outward from one point.
void rosette(mesh& m, v3 foot, double size) {
  const int leaves = static_cast<int>(rnd(6, 10));
  const double turn = rnd(0, 2 * pi), kind = rnd(0.0, 0.25);
  for (int leaf = 0; leaf < leaves; ++leaf) {
    const double a0 = turn + leaf * 2 * pi / leaves + rnd(-0.25, 0.25), length = size * rnd(0.7, 1.15), rise = rnd(0.25, 0.6);
    const v3 out = flat(a0), across = flat(a0 + pi / 2);
    const int rows = 7;
    const std::uint32_t first = m.count();
    for (int j = 0; j <= rows; ++j) {
      const double s = static_cast<double>(j) / rows;
      const double w = length * 0.2 * std::pow(std::sin(pi * (0.06 + 0.94 * s)), 0.7);
      const v3 c = foot + out * (length * s) + up * (length * rise * std::sin(pi * s * 0.85) - 4);
      const v3 along = unit(out + up * (rise * 0.85 * pi * std::cos(pi * s * 0.85)));
      const v3 face = unit(cross(across, along) * -1.0);
      const double shade = 0.22 + 0.5 * s;
      m.add(c - across * w + face * (w * 0.25), unit(face - across * 0.45), shade + 0.12, grass_band, kind);
      m.add(c, face, shade * 0.8, grass_band, kind + 0.25);  // the rib, a little lower and paler
      m.add(c + across * w + face * (w * 0.25), unit(face + across * 0.45), shade + 0.12, grass_band, kind);
    }
    for (int j = 0; j < rows; ++j)
      for (int k = 0; k < 2; ++k) {
        const std::uint32_t a = first + 3 * j + k;
        m.tri(a, a + 1, a + 4), m.tri(a, a + 4, a + 3);
      }
  }
}

void make_grass(const triangles& old, const std::string& file) {
  mesh m;
  struct place {
    v3 foot;
    double height, lean;
  };
  std::vector<place> places;
  for (std::size_t i = 0; i + 23 < old.corners.size(); i += 24) {  // the old blades are eight triangles each
    v3 low{0, 1e9, 0}, high{0, -1e9, 0};
    for (int k = 0; k < 24; ++k) {
      const v3 p = old.points[old.corners[i + k]];
      if (p.y < low.y) low = p;
      if (p.y > high.y) high = p;
    }
    v3 foot;
    int n = 0;
    for (int k = 0; k < 24; ++k) {
      const v3 p = old.points[old.corners[i + k]];
      if (p.y < low.y + 0.1 * (high.y - low.y)) foot = foot + p, ++n;
    }
    foot = foot * (1.0 / n);
    const v3 over = high - foot;
    places.push_back({foot, high.y - low.y, std::hypot(over.x, over.z) > 2 ? std::atan2(over.z, over.x) : rnd(0, 2 * pi)});
  }
  for (const place& p : places) {
    // Patches of drier and of fresher grass, and of taller and shorter, so that it is not one carpet.
    const double patch = noise({p.foot.x / 900, 0, p.foot.z / 900}), tall = 0.75 + 0.6 * noise({p.foot.x / 600, 7, p.foot.z / 600});
    const int blades = 1 + (rnd() < 0.4);
    for (int b = 0; b < blades; ++b) {
      const double angle = rnd(0, 2 * pi), away = b ? rnd(18, 70) : 0;
      v3 foot = p.foot + flat(angle) * away;
      if (b) foot.y = ground(foot.x, foot.z);
      const double height = p.height * tall * (b ? rnd(0.45, 0.95) : rnd(0.9, 1.15));
      const double lean = b ? p.lean + rnd(-1.6, 1.6) : p.lean + rnd(-0.3, 0.3);
      blade(m, foot, height, lean, rnd(0.15, 0.95), std::clamp(height * rnd(0.035, 0.06), 9.0, 38.0), clamp01(0.55 * patch + rnd(-0.12, 0.2)), rnd(0.8, 1.1), height > 260 ? 5 : 4);
    }
    const double r = rnd();
    const v3 near = p.foot + flat(rnd(0, 2 * pi)) * rnd(20, 60);
    const v3 spot{near.x, ground(near.x, near.z), near.z};
    if (r < 0.045 && p.height > 300) seed_stalk(m, spot, p.height * rnd(1.25, 1.7), rnd(0, 2 * pi));
    else if (r < 0.053) {
      for (int k = static_cast<int>(rnd(4, 9)); k > 0; --k) {
        const v3 c = spot + flat(rnd(0, 2 * pi)) * rnd(0, 160);
        clover(m, {c.x, ground(c.x, c.z), c.z}, rnd(80, 210), rnd(42, 70));
      }
    } else if (r < 0.063)
      rosette(m, spot, rnd(200, 380));
  }
  m.write(file, "grass");
}

// ---- trees ---------------------------------------------------------------------------------

struct part {
  v3 low{1e9, 1e9, 1e9}, high{-1e9, -1e9, -1e9};
  int count = 0;
  std::uint32_t colours = 0;  // one bit per colour of the old palette
  v3 middle() const { return (low + high) * 0.5; }
};

// The old file's separate pieces: 120 triangles is a trunk, 80 a ball of leaves.
std::vector<part> pieces(const triangles& t) {
  std::map<std::array<int, 3>, std::uint32_t> welded;
  std::vector<std::uint32_t> same(t.points.size()), group;
  for (std::size_t i = 0; i < t.points.size(); ++i) {
    const std::array<int, 3> key{static_cast<int>(std::lround(t.points[i].x * 10)), static_cast<int>(std::lround(t.points[i].y * 10)), static_cast<int>(std::lround(t.points[i].z * 10))};
    same[i] = welded.try_emplace(key, static_cast<std::uint32_t>(welded.size())).first->second;
  }
  group.resize(welded.size());
  std::iota(group.begin(), group.end(), 0u);
  const std::function<std::uint32_t(std::uint32_t)> root = [&](std::uint32_t a) { return group[a] == a ? a : group[a] = root(group[a]); };
  for (std::size_t i = 0; i + 2 < t.corners.size(); i += 3)
    for (int k = 1; k < 3; ++k) group[root(same[t.corners[i + k]])] = root(same[t.corners[i]]);
  std::map<std::uint32_t, part> found;
  for (std::size_t i = 0; i + 2 < t.corners.size(); i += 3) {
    part& p = found[root(same[t.corners[i]])];
    ++p.count;
    for (int k = 0; k < 3; ++k) {
      const v3 q = t.points[t.corners[i + k]];
      p.colours |= 1u << t.colour[t.corners[i + k]];
      p.low = {std::min(p.low.x, q.x), std::min(p.low.y, q.y), std::min(p.low.z, q.z)};
      p.high = {std::max(p.high.x, q.x), std::max(p.high.y, q.y), std::max(p.high.z, q.z)};
    }
  }
  std::vector<part> out;
  for (const auto& [id, p] : found) out.push_back(p);
  return out;
}

// A sphere of triangles: an icosahedron, each face split in four, `level` times.
struct sphere {
  std::vector<v3> ball;
  std::vector<std::uint32_t> faces;
};
const sphere& sphere_of(int level) {
  static std::map<int, sphere> made;
  sphere& s = made[level];
  std::vector<v3>& ball = s.ball;
  std::vector<std::uint32_t>& faces = s.faces;
  if (ball.empty()) {
    const double g = (1 + std::sqrt(5.0)) / 2;
    for (const v3 p : {v3{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g}, {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}}) ball.push_back(unit(p));
    faces = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8, 3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (int pass = 0; pass < level; ++pass) {
      std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> halves;
      const auto half = [&](std::uint32_t a, std::uint32_t b) {
        const auto [it, fresh] = halves.try_emplace(std::minmax(a, b), static_cast<std::uint32_t>(ball.size()));
        if (fresh) ball.push_back(unit(ball[a] + ball[b]));
        return it->second;
      };
      std::vector<std::uint32_t> next;
      for (std::size_t i = 0; i < faces.size(); i += 3) {
        const std::uint32_t a = faces[i], b = faces[i + 1], c = faces[i + 2], ab = half(a, b), bc = half(b, c), ca = half(c, a);
        next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
      }
      faces = next;
    }
  }
  return s;
}

// A lump of a crown: a dark ball inside, and leaves standing out of it all around, lit from above.
void leaves(mesh& m, v3 centre, v3 radii, double kind) {
  const sphere& s = sphere_of(1);
  const std::uint32_t first = m.count();
  const std::size_t first_corner = m.corners.size();
  const v3 shift{rnd(0, 50), rnd(0, 50), rnd(0, 50)};
  const auto reach = [&](v3 d) { return 0.72 + 0.4 * noise(d * 2.3 + shift); };
  for (const v3 d : s.ball) m.add(centre + v3{d.x * radii.x, d.y * radii.y, d.z * radii.z} * (reach(d) * 0.86), d, 0.2 + 0.2 * (0.5 + 0.5 * d.y), leaf_band, kind);
  for (const std::uint32_t f : s.faces) m.corners.push_back(first + f);
  m.round_off(first, first_corner, [&](std::uint32_t i) { return m.point(i) - centre; });
  const double size = (radii.x + radii.y + radii.z) / 3;
  for (int i = 0; i < 72; ++i) {
    const v3 d = unit({rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)});
    const v3 p = centre + v3{d.x * radii.x, d.y * radii.y, d.z * radii.z} * reach(d);
    const v3 along = unit(d + v3{rnd(-0.7, 0.7), rnd(-0.9, 0.3), rnd(-0.7, 0.7)});
    const v3 across = unit(cross(along, d + v3{rnd(-0.4, 0.4), rnd(-0.4, 0.4), rnd(-0.4, 0.4)}));
    const double length = size * rnd(0.34, 0.55), width = length * rnd(0.24, 0.34);
    const double shade = 0.34 + 0.44 * (0.5 + 0.5 * d.y) + rnd(-0.1, 0.14), k = kind + rnd(-0.22, 0.22) + 0.2 * d.y;
    const v3 n = unit(d + along * 0.3), fold = n * (width * 0.25);
    const std::uint32_t a = m.add(p - along * (0.15 * length), n, shade * 0.75, leaf_band, k), b = m.add(p + along * (0.42 * length) - across * width + fold, unit(n - across * 0.4), shade, leaf_band, k);
    const std::uint32_t c = m.add(p + along * (0.42 * length) + across * width + fold, unit(n + across * 0.4), shade, leaf_band, k), e = m.add(p + along * length - up * (0.12 * length), n, shade * 1.08, leaf_band, k + 0.1);
    m.tri(a, b, c), m.tri(b, e, c);
  }
}

void make_trees(const triangles& old, const std::string& file) {
  mesh m;
  std::vector<part> trunks, crowns;
  for (const part& p : pieces(old)) {
    if (p.count == 120) trunks.push_back(p);
    if (p.count == 80) crowns.push_back(p);
  }
  std::printf("trees: %zu trunks, %zu balls of leaves\n", trunks.size(), crowns.size());
  const v3 north = unit({0.6, 0, -0.8});  // the side the moss grows on
  std::vector<std::vector<part>> own(trunks.size());
  for (const part& c : crowns) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < trunks.size(); ++i)
      if (std::hypot(c.middle().x - trunks[i].middle().x, c.middle().z - trunks[i].middle().z) < std::hypot(c.middle().x - trunks[best].middle().x, c.middle().z - trunks[best].middle().z)) best = i;
    if (!trunks.empty()) own[best].push_back(c);
  }
  for (std::size_t t = 0; t < trunks.size(); ++t) {
    const part& old_trunk = trunks[t];
    const v3 mid = old_trunk.middle();
    const double radius = 0.27 * ((old_trunk.high.x - old_trunk.low.x) + (old_trunk.high.z - old_trunk.low.z)) / 2;
    const int lobes = static_cast<int>(rnd(5, 8)), ridges = 18;
    const double turn = rnd(0, 2 * pi), phase = rnd(0, 6), spot = rnd(0, 90);
    // the ground around the foot, by direction (the trunks stand on a slope)
    const auto foot = [&](double angle) { return ground(mid.x + std::cos(angle) * radius * 1.6, mid.z + std::sin(angle) * radius * 1.6); };
    double lowest = 1e9;
    for (int k = 0; k < 24; ++k) lowest = std::min(lowest, foot(2 * pi * k / 24));
    const double base = lowest - 120, height = old_trunk.high.y - base;
    const auto root_at = [&](int i) { return turn + 2 * pi * i / lobes; };

    // the trunk: wide ribs at the foot that run out into the roots, furrowed bark, moss low on one side
    std::vector<v3> centres;
    std::vector<double> radii;
    const int rings = 30;
    for (int j = 0; j < rings; ++j) {
      const double s = std::pow(static_cast<double>(j) / (rings - 1), 1.5);
      const double lean_x = 60 * std::sin(s * 2.1 + phase), lean_z = 60 * std::cos(s * 1.7 + phase);
      centres.push_back({mid.x + lean_x * s, base + height * s, mid.z + lean_z * s});
      radii.push_back(radius * (1 - 0.45 * s) * (s > 0.93 ? std::sqrt(std::max(0.0, 1 - std::pow((s - 0.93) / 0.07, 2))) + 0.03 : 1.0));
    }
    const auto furrow = [&](double s, double angle) {  // 0 in a furrow, 1 on a ridge
      const double y = height * std::pow(s, 1.5);
      return 0.5 + 0.5 * std::cos(ridges * angle + 1.3 * std::sin(y / 650 + phase) + 7 * noise({angle * 2.2, y / 420, spot}));
    };
    tube(
        m, centres, radii, 72, bark_band,
        [&](double s, double angle) {
          const double y = base + height * std::pow(s, 1.5), above = std::max(0.0, y - foot(angle));
          const double rib = std::pow(std::max(0.0, std::cos(lobes * (angle - turn) / 2)), 2);  // one rib per root
          return 1 + std::exp(-above / (0.085 * height)) * (0.35 + 1.0 * rib) + 0.075 * (furrow(s, angle) - 0.5) + 0.06 * (noise({angle * 1.3, y / 900, spot + 9}) - 0.5);
        },
        [&](double s, double angle, double) {
          const double y = base + height * std::pow(s, 1.5), above = std::max(0.0, y - foot(angle));
          const double f = furrow(s, angle), speck = noise({angle * 9, y / 120, spot});
          const double moss = clamp01((0.25 + 0.9 * std::max(0.0, dot(flat(angle), north))) * std::exp(-above / (0.10 * height)) * (0.4 + 1.5 * noise({angle * 3, y / 300, spot + 3})));
          return colour{(0.24 + 0.4 * f + 0.3 * speck) * (0.6 + 0.4 * smooth(above / 500)), smooth(moss * 1.4)};
        });

    // the roots: from each rib outward over the ground, thinning
    for (int i = 0; i < lobes; ++i) {
      const double angle = root_at(i) + rnd(-0.12, 0.12), length = rnd(900, 1900), thick = radius * rnd(0.3, 0.45), wave = rnd(0, 6);
      std::vector<v3> line;
      std::vector<double> size;
      const int steps = 12;
      for (int j = 0; j <= steps; ++j) {
        const double s = static_cast<double>(j) / steps, a = angle + 0.3 * std::sin(3.5 * s + wave) * s;
        const double r = thick * std::pow(1 - s, 1.2) + 22, out = radius * 1.05 + length * s;
        const v3 p{mid.x + std::cos(a) * out, 0, mid.z + std::sin(a) * out};
        line.push_back({p.x, ground(p.x, p.z) + r * (0.45 - 0.75 * s) + (j == 0 ? thick * 0.5 : 0), p.z});
        size.push_back(r);
      }
      tube(
          m, line, size, 10, bark_band, [&](double s, double a) { return 1 + 0.12 * (noise({a * 2, s * 9, spot + i}) - 0.5); },
          [&](double s, double a, double) {
            const double speck = noise({a * 4, s * 25, spot + i});
            return colour{0.24 + 0.4 * speck + 0.1 * s, smooth(1.6 * noise({a, s * 5, spot + 20 + i}) - 0.35)};
          });
    }

    // the boughs: one to each ball of leaves, the lower balls from lower on the trunk
    std::vector<part>& mine = own[t];
    std::sort(mine.begin(), mine.end(), [](const part& a, const part& b) { return a.middle().y < b.middle().y; });
    for (std::size_t i = 0; i < mine.size(); ++i) {
      const v3 to = mine[i].middle();
      const double s0 = 0.62 + 0.3 * (i + 0.5) / mine.size();
      const v3 from = centres[std::min<std::size_t>(rings - 1, static_cast<std::size_t>(std::pow(s0, 1 / 1.5) * (rings - 1)))];
      const v3 knee = from + (to - from) * 0.45 + up * (0.18 * len(to - from)) + v3{rnd(-150, 150), 0, rnd(-150, 150)};
      const double thick = std::min(radius * (1 - 0.45 * s0) * 0.5, 190.0);
      std::vector<v3> line;
      std::vector<double> size;
      for (int j = 0; j <= 9; ++j) {
        const double s = j / 9.0;
        line.push_back(mix(mix(from, knee, s), mix(knee, to, s), s));
        size.push_back(thick * (1 - 0.8 * s) + 14);
      }
      tube(m, line, size, 9, bark_band, [](double, double) { return 1.0; }, [&](double s, double a, double) { return colour{0.22 + 0.4 * noise({a * 3, s * 14, spot + i}), 0.0}; });
    }

    // the leaves: each old ball becomes a few lumps of different greens
    for (const part& c : mine) {
      const v3 r = (c.high - c.low) * 0.5;
      const double tint = rnd(0.05, 0.6);
      for (int lump = 0; lump < 3; ++lump) {
        const v3 at = c.middle() + v3{rnd(-0.4, 0.4) * r.x, rnd(-0.3, 0.3) * r.y, rnd(-0.4, 0.4) * r.z};
        leaves(m, at, r * rnd(0.6, 0.85), tint + rnd(-0.15, 0.15));
      }
    }
  }
  m.write(file, "trees");
}

// ---- the ground ----------------------------------------------------------------------------

// The old heights on a finer, smooth net; earth where the old ground was brown, a green floor
// elsewhere, both mottled; and small stones along the edges of the earth.
void make_ground(const std::string& file) {
  mesh m;
  const int n = 210;
  const double reach = 7900, step = 2 * reach / n;
  const std::uint32_t first = m.count();
  for (int j = 0; j <= n; ++j)
    for (int i = 0; i <= n; ++i) {
      const double x = -reach + i * step, z = -reach + j * step;
      const v3 slope{ground(x - 40, z) - ground(x + 40, z), 80, ground(x, z - 40) - ground(x, z + 40)};
      const double bare = on_grid(earth, x, z) + 0.5 * (noise({x / 150, 3, z / 150}) - 0.5) * (on_grid(earth, x, z) > 0.03);
      const double coarse = noise({x / 420, 0, z / 420}), fine = noise({x / 70, 5, z / 70}), grain = noise({x / 23, 9, z / 23});
      m.add({x, ground(x, z), z}, unit(slope), 0.2 + 0.3 * coarse + 0.25 * fine + 0.2 * grain, earth_band, 1 - smooth(bare * 1.6 - 0.15));
    }
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      const std::uint32_t a = first + j * (n + 1) + i;
      m.tri(a, a + n + 1, a + n + 2), m.tri(a, a + n + 2, a + 1);
    }
  const sphere& s = sphere_of(1);
  for (int stones = 0, tries = 0; stones < 420 && tries < 200000; ++tries) {
    const double x = rnd(-6500, 6500), z = rnd(-3500, 7800), bare = on_grid(earth, x, z);
    if (bare < 0.08 || bare > 0.7 || ground(x, z) > 600) continue;  // beside the earth, not on it: the ants walk there
    ++stones;
    const double size = rnd(10, 42), kind = rnd(0, 1), light = rnd(0.75, 1.1);
    const v3 centre{x, ground(x, z) + size * 0.2, z}, shift{rnd(0, 50), rnd(0, 50), rnd(0, 50)};
    const std::uint32_t from = m.count();
    const std::size_t from_corner = m.corners.size();
    for (const v3 d : s.ball) {
      const double lump = noise(d * 1.7 + shift);
      m.add(centre + v3{d.x * 1.25, d.y * 0.6, d.z} * (size * (0.7 + 0.6 * lump)), d, light * (0.3 + 0.35 * (0.5 + 0.5 * d.y) + 0.3 * lump), stone_band, kind);
    }
    for (const std::uint32_t f : s.faces) m.corners.push_back(from + f);
    m.round_off(from, from_corner, [&](std::uint32_t i) { return m.point(i) - centre; });
  }
  m.write(file, "ground");
}

// ---- mushrooms -----------------------------------------------------------------------------

// Rings around an axis: each ring has a height along the axis, a radius and a colour.
struct ring {
  double height, radius, shade, kind;
};
void revolve(mesh& m, v3 origin, v3 axis, const std::vector<ring>& rings, int around, band b, double facing, const std::function<double(int, double)>& swell,
             const std::function<double(int, int)>& shade) {
  const std::uint32_t first = m.count();
  const std::size_t first_corner = m.corners.size();
  const v3 side = unit(cross(axis, std::abs(axis.x) < 0.9 ? v3{1, 0, 0} : v3{0, 0, 1})), other = cross(axis, side);
  for (std::size_t j = 0; j < rings.size(); ++j)
    for (int k = 0; k < around; ++k) {
      const double angle = 2 * pi * k / around;
      m.add(origin + axis * rings[j].height + (side * std::cos(angle) + other * std::sin(angle)) * (rings[j].radius * swell(static_cast<int>(j), angle)), up, rings[j].shade * shade(static_cast<int>(j), k), b, rings[j].kind);
    }
  for (std::size_t j = 0; j + 1 < rings.size(); ++j)
    for (int k = 0; k < around; ++k) {
      const std::uint32_t a = first + static_cast<std::uint32_t>(j) * around + k, c = first + static_cast<std::uint32_t>(j) * around + (k + 1) % around;
      m.tri(a, c, c + around), m.tri(a, c + around, a + around);
    }
  const v3 inside = origin + axis * facing;  // the normals point away from this point
  m.round_off(first, first_corner, [&](std::uint32_t i) { return m.point(i) - inside; });
}

// One mushroom under an old cap: a stem from the ground, gills under the cap, and by kind a red
// cap with white warts and a ring on the stem, a thick brown cap, or a yellow one with a wavy rim.
void mushroom(mesh& m, const part& cap) {
  enum { red, brown, yellow } kind = cap.colours & (1u << 18) ? red : cap.colours & (1u << 21) ? brown : yellow;
  const v3 mid = cap.middle();
  const double radius = ((cap.high.x - cap.low.x) + (cap.high.z - cap.low.z)) / 4, tall = (cap.high.y - cap.low.y) * (kind == brown ? 1.15 : 1.0);
  const double floor = ground(mid.x, mid.z) - 12;
  const double lean = rnd(0, 2 * pi), tilt = rnd(0.02, 0.14), wave = rnd(0, 6), spot = rnd(0, 90);
  const v3 axis = unit(up + flat(lean) * tilt);
  const v3 under{mid.x, cap.low.y, mid.z};  // the middle of the cap's underside
  const double stem = radius * (kind == brown ? 0.26 : kind == red ? 0.17 : 0.13), length = std::max(under.y - floor, radius * 0.3);

  // the stem: thick at the foot, a little bent, fibres down its length
  std::vector<v3> centres;
  std::vector<double> radii;
  const int steps = 9;
  for (int j = 0; j <= steps; ++j) {
    const double s = static_cast<double>(j) / steps;
    centres.push_back(v3{mid.x, floor, mid.z} + up * (length * s) + flat(lean) * (tilt * length * (s * s - s) * 1.2) + axis * (tall * 0.25 * s * s));
    radii.push_back(stem * (kind == brown ? 1.35 - 0.5 * s : 1 + 0.75 * std::exp(-s * 7) - 0.2 * s));
  }
  const v3 top = centres.back();
  tube(m, centres, radii, 18, pale_band, [&](double s, double a) { return 1 + 0.06 * (noise({a * 3, s * 4, spot}) - 0.5); },
       [&](double s, double a, double) { return colour{0.34 + 0.3 * noise({a * 6, s * 1.5, spot}) + 0.25 * smooth(s * 3), kind == red ? 0.75 : 0.15}; });

  const int around = 40;
  const auto rim_wave = [&](double angle) { return 1 + (kind == yellow ? 0.09 : 0.03) * std::sin(angle * (kind == yellow ? 5 : 3) + wave) + 0.05 * (noise({std::cos(angle) * 2 + spot, std::sin(angle) * 2, wave}) - 0.5); };
  // the gills: from the stem out to the rim, every other line darker
  std::vector<ring> below;
  for (int j = 0; j <= 4; ++j) {
    const double s = j / 4.0;
    below.push_back({tall * (0.12 + 0.18 * s - 0.3 * s * s), stem * 0.8 + (radius * 0.97 - stem * 0.8) * s, 0.5 + 0.2 * s, kind == yellow ? 0.05 : 0.45});
  }
  revolve(m, top - axis * (tall * 0.12), axis, below, around, pale_band, tall * 2, [&](int j, double angle) { return 1 + (rim_wave(angle) - 1) * j / 4.0; },
          [&](int j, int k) { return j > 0 && j < 4 && k % 2 ? 0.6 : 1.0; });
  // the cap
  std::vector<ring> dome;
  const int levels = 11;
  dome.push_back({0, radius * 0.94, 0.4, 0});
  for (int j = 0; j <= levels; ++j) {
    const double a = pi / 2 * (1 - static_cast<double>(j) / levels);  // from the rim up to the top
    const double r = kind == brown ? std::pow(std::sin(a), 0.75) : kind == red ? std::pow(std::sin(a), 0.9) : std::pow(std::sin(a), 1.4);
    const double h = kind == yellow ? 0.55 + 0.45 * std::cos(a) - 0.5 * std::exp(-std::pow(a / 0.5, 2)) * 0.5 : std::cos(a);
    dome.push_back({tall * (0.04 + 0.96 * h), std::max(radius * r, radius * 0.002), 0.42 + 0.5 * std::cos(a), 0});
  }
  const band cap_colour = kind == yellow ? straw_band : cap_band;
  for (ring& r : dome) r.kind = kind == red ? 0.03 : kind == brown ? 0.85 : 0.1;
  revolve(m, top - axis * (tall * 0.12), axis, dome, around, cap_colour, -tall, [&](int j, double angle) { return 1 + (rim_wave(angle) - 1) * (1 - static_cast<double>(j) / levels); },
          [&](int j, int k) { return 0.9 + 0.2 * noise({k * 0.7, j * 0.9, spot}); });

  const v3 side = unit(cross(axis, std::abs(axis.x) < 0.9 ? v3{1, 0, 0} : v3{0, 0, 1})), other = cross(axis, side);
  if (kind == red) {
    // the warts: small white patches standing a little off the cap
    for (int i = static_cast<int>(rnd(16, 30)); i > 0; --i) {
      const double a = rnd(0.12, 1.25), angle = rnd(0, 2 * pi), size = radius * rnd(0.035, 0.08);
      const v3 out = side * std::cos(angle) + other * std::sin(angle);
      const v3 n = unit(out * std::sin(a) * 1.0 + axis * std::cos(a) * (radius / std::max(tall, 1.0)));
      const v3 p = top - axis * (tall * 0.12) + axis * (tall * (0.04 + 0.96 * std::cos(a))) + out * (radius * std::pow(std::sin(a), 0.9)) + n * (size * 0.1);
      const v3 u = unit(cross(n, axis)), v = cross(n, u);
      const std::uint32_t hub = m.add(p + n * (size * 0.22), n, 0.95, pale_band, 1);
      std::uint32_t before = 0, start = 0;
      for (int k = 0; k < 6; ++k) {
        const double t = 2 * pi * k / 6 + angle, r = size * rnd(0.75, 1.15);
        const std::uint32_t e = m.add(p + (u * std::cos(t) + v * std::sin(t)) * r - n * (size * 0.12), unit(n + (u * std::cos(t) + v * std::sin(t)) * 0.6), 0.74, pale_band, 0.9);
        if (k) m.tri(hub, before, e);
        else start = e;
        before = e;
      }
      m.tri(hub, before, start);
    }
    // the ring on the stem: a skirt hanging down
    const v3 at = centres[6];
    std::vector<ring> skirt;
    for (int j = 0; j <= 3; ++j) skirt.push_back({-stem * 0.9 * j / 3.0 * (1 + 0.4 * j / 3.0), stem * (0.85 + 1.3 * j / 3.0), 0.8 - 0.1 * j, 0.7});
    revolve(m, at, axis, skirt, 18, pale_band, stem * 3, [&](int j, double angle) { return 1 + 0.07 * j * std::sin(angle * 7 + wave); }, [](int, int) { return 1.0; });
  }
}

void make_mushrooms(const triangles& old, const std::string& file) {
  mesh m;
  int count = 0;
  for (const part& p : pieces(old))
    if (p.count == 210) mushroom(m, p), ++count;  // the old caps
  std::printf("mushrooms: %d\n", count);
  m.write(file, "mushrooms");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "antforest <folder with the scene's files> <output folder>\n");
    return 1;
  }
  const std::string in = argv[1], out = argv[2];
  read_ground(read_glb(in + "/antscene_ground.glb"));
  make_ground(out + "/antscene_ground.glb");
  make_mushrooms(read_glb(in + "/antscene_mushrooms.glb"), out + "/antscene_mushrooms.glb");
  make_grass(read_glb(in + "/antscene_grass.glb"), out + "/antscene_grass.glb");
  make_trees(read_glb(in + "/antscene_trees.glb"), out + "/antscene_trees.glb");
}

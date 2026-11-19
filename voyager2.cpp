// Voyager 2 from 1977 to 2026 with the orbit library (orbit/orbit.hpp),
// against NASA's own trajectory.
//
// The route is flown in nine legs. Each leg starts from NASA's position and
// velocity and is then carried forward by the library's integrator alone, under
// gravity alone, and compared with NASA's trajectory along the way:
//
//   cruise legs   in a frame fixed to the solar system's centre of mass, with no
//                 central body: the Sun and the planets are moving masses
//                 (attractors), each given by a table of where it is
//   flyby legs    in a frame centred on the planet with z along its pole: the
//                 planet is the central body with its flattening (J2), the Sun
//                 and the planet's large moons are attractors
//
// Not in the model: thrust (the spacecraft corrected its course many times),
// radiation and heat recoil, relativity, flattening beyond J2, small moons and
// rings. A leg therefore drifts from NASA's trajectory, and the size of the
// drift is the result this program prints.
//
// Input: NASA JPL Horizons vector tables in data/ (see fetch.sh).
// Output: a table on the console and, per view, a glTF file (.glb) and a GameApi
// script (.txt) in out/ (GameApi: https://github.com/terop2/GameApi, the submodule
// here). NASA's trajectory is a row of beads, the simulated one a line through them.
//
// Build and run: ./run.sh    (any C++20 compiler; nothing else is needed)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "orbit.hpp"

using namespace glass::orbit;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kStarRows = 16;  // rows of brightness in out/star-dots.png
constexpr double kAu = 149'597'870'700.0;  // m
#ifndef VOYAGER2_EINSTEIN
#define VOYAGER2_EINSTEIN 1
#endif
constexpr bool kEinstein = VOYAGER2_EINSTEIN;

// ---- input: Horizons vector tables ----

struct row {
  double t;  // s (TDB)
  std::string date;
  orbit_state s;
};

std::string g_data = "data";
std::string g_maps = "textures";

std::vector<row> read_horizons(const std::string& name) {
  std::vector<row> out;
  std::ifstream f(g_data + "/" + name);
  std::string line;
  bool in = false;
  while (std::getline(f, line)) {
    if (line.rfind("$$SOE", 0) == 0) { in = true; continue; }
    if (line.rfind("$$EOE", 0) == 0) break;
    if (!in) continue;
    double jd, x, y, z, vx, vy, vz;
    char date[40];
    if (std::sscanf(line.c_str(), "%lf, A.D. %39[^,], %lf, %lf, %lf, %lf, %lf, %lf", &jd, date, &x, &y, &z, &vx, &vy, &vz) == 8)
      out.push_back({(jd - 2440000.5) * 86400.0, std::string(date).substr(0, 17), {{x * 1e3, y * 1e3, z * 1e3}, {vx * 1e3, vy * 1e3, vz * 1e3}}});
  }
  if (out.size() < 2) {
    std::fprintf(stderr, "no data in %s/%s (run from this folder, or pass the data folder)\n", g_data.c_str(), name.c_str());
    std::exit(1);
  }
  return out;
}

// ---- frames ----

struct frame {  // rows are the new axes, in the old frame
  vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
  vec3 turn(vec3 v) const { return {dot(x, v), dot(y, v), dot(z, v)}; }
};
// z along a pole given by right ascension and declination (degrees, ICRF)
frame pole_frame(double ra, double dec) {
  const double a = ra * kPi / 180.0, d = dec * kPi / 180.0;
  frame f;
  f.z = {std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d)};
  const vec3 n = cross({0, 0, 1}, f.z);
  f.x = (1.0 / norm(n)) * n;
  f.y = cross(f.z, f.x);
  return f;
}
// the ecliptic: ICRF turned about x by the obliquity, so that the planets' orbits lie flat
frame ecliptic() { return pole_frame(270.0, 66.560709); }

// ---- the legs ----

struct mass {
  const char* name;
  double gm;         // km^3/s^2 (NASA JPL Horizons)
  const char* file;
};
struct planet {
  const char* name;
  double gm;         // km^3/s^2, the planet alone
  double radius;     // km, the radius its J2 refers to
  double j2;
  double pole_ra, pole_dec;  // degrees, at the flyby
  double day = 0.0;  // hours for one turn (negative: backwards)
};
// Mean radius of a moon, km.
double moon_radius(const std::string& name) {
  static const std::pair<const char*, double> r[] = {
      {"Io", 1821.6}, {"Europa", 1560.8}, {"Ganymede", 2631.2}, {"Callisto", 2410.3}, {"Mimas", 198.8}, {"Enceladus", 252.3}, {"Iapetus", 734.5}, {"Tethys", 531.0}, {"Dione", 561.4}, {"Rhea", 763.8},
      {"Titan", 2574.7}, {"Ariel", 578.9}, {"Umbriel", 584.7}, {"Titania", 788.4}, {"Oberon", 761.4}, {"Miranda", 235.8}, {"Triton", 1353.4}};
  for (const auto& [n, km] : r) if (name == n) return km;
  return 0.0;
}
struct leg {
  const char* name;
  const char* truth;
  double step;       // s
  const planet* centre;  // null: cruise
  std::vector<mass> masses;
};

// Masses: NASA JPL Horizons (GM of each body or planet system). Poles: IAU working
// group on cartographic coordinates, at the flyby date. J2 with its reference
// radius: the published gravity fields of the planets (JPL satellite ephemerides).
constexpr double kSun = 132712440041.279;
constexpr double kInner = 22031.868551 + 324858.592 + 403503.235625 + 42828.375816;  // Mercury, Venus, Earth-Moon, Mars
const planet kJupiter{"Jupiter", 126686531.900, 71492.0, 0.014696572, 268.056595, 64.495303, 9.925};
const planet kSaturn{"Saturn", 37931206.234, 60330.0, 0.016290573, 40.5956, 83.5377, 10.656};
const planet kUranus{"Uranus", 5793950.6103, 25559.0, 0.00351068, 257.311, -15.175, -17.24};
const planet kNeptune{"Neptune", 6835099.97, 25225.0, 0.00340843, 299.268, 42.954, 16.11};

const std::vector<mass> kGiants{{"Jupiter", 126712764.1, "bary_5.txt"}, {"Saturn", 37940584.8418, "bary_6.txt"},
                                {"Uranus", 5794556.4, "bary_7.txt"}, {"Neptune", 6836527.10058, "bary_8.txt"}};

std::vector<mass> cruise_masses(bool inner_apart) {
  std::vector<mass> m;
  if (inner_apart) {
    m = {{"Sun", kSun, "bary_10.txt"}, {"Mercury", 22031.868551, "bary_1.txt"}, {"Venus", 324858.592, "bary_2.txt"},
         {"Earth and Moon", 403503.235625, "bary_3.txt"}, {"Mars", 42828.375816, "bary_4.txt"}};
  } else {
    m = {{"Sun and inner planets", kSun + kInner, "bary_10.txt"}};
  }
  m.insert(m.end(), kGiants.begin(), kGiants.end());
  return m;
}

const std::vector<leg> kLegs{
    {"Earth to Jupiter", "v2_leg1.txt", 3600.0, nullptr, cruise_masses(true)},
    {"Jupiter flyby", "jup_v2.txt", 20.0, &kJupiter,
     {{"Sun", kSun + kInner, "jup_10.txt"}, {"Io", 5959.9155, "jup_501.txt"}, {"Europa", 3202.7121, "jup_502.txt"},
      {"Ganymede", 9887.8328, "jup_503.txt"}, {"Callisto", 7179.2834, "jup_504.txt"}, {"Saturn", 37940584.8418, "jup_6.txt"}}},
    {"Jupiter to Saturn", "v2_leg3.txt", 21600.0, nullptr, cruise_masses(false)},
    {"Saturn flyby", "sat_v2.txt", 20.0, &kSaturn,
     {{"Sun", kSun + kInner, "sat_10.txt"}, {"Mimas", 2.503489, "sat_601.txt"}, {"Enceladus", 7.210367, "sat_602.txt"},
      {"Tethys", 41.21, "sat_603.txt"}, {"Dione", 73.116, "sat_604.txt"}, {"Rhea", 153.94, "sat_605.txt"}, {"Titan", 8978.14, "sat_606.txt"},
      {"Iapetus", 120.52, "sat_608.txt"}, {"Jupiter", 126712764.1, "sat_5.txt"}}},
    {"Saturn to Uranus", "v2_leg5.txt", 21600.0, nullptr, cruise_masses(false)},
    {"Uranus flyby", "ura_v2.txt", 10.0, &kUranus,
     {{"Sun", kSun + kInner, "ura_10.txt"}, {"Ariel", 83.43, "ura_701.txt"}, {"Umbriel", 85.40, "ura_702.txt"},
      {"Titania", 222.8, "ura_703.txt"}, {"Oberon", 205.34, "ura_704.txt"}, {"Miranda", 4.3, "ura_705.txt"}}},
    {"Uranus to Neptune", "v2_leg7.txt", 21600.0, nullptr, cruise_masses(false)},
    {"Neptune flyby", "nep_v2.txt", 5.0, &kNeptune, {{"Sun", kSun + kInner, "nep_10.txt"}, {"Triton", 1428.495, "nep_801.txt"}}},
    {"Neptune to 2026", "v2_leg9.txt", 3600.0, nullptr, cruise_masses(false)},
};

// Controls: bodies whose NASA paths are gravity alone, flown by the same code. What
// is left here is the model's own error; what a Voyager leg shows beyond it is not
// gravity.
const planet kSaturnRound{"Saturn", 37931206.234, 60330.0, 0.0, 40.5956, 83.5377};
const std::vector<leg> kControls{
    {"Uranus, 49 years", "bary_7.txt", 86400.0, nullptr,
     {{"Sun and inner planets", kSun + kInner, "bary_10.txt"}, {"Jupiter", 126712764.1, "bary_5.txt"}, {"Saturn", 37940584.8418, "bary_6.txt"},
      {"Neptune", 6836527.10058, "bary_8.txt"}}},
    {"Tethys, 10 days", "sat_603.txt", 20.0, &kSaturn,
     {{"Sun", kSun + kInner, "sat_10.txt"}, {"Dione", 73.116, "sat_604.txt"}, {"Rhea", 153.94, "sat_605.txt"}, {"Titan", 8978.14, "sat_606.txt"},
      {"Jupiter", 126712764.1, "sat_5.txt"}}},
    {"Tethys, round Saturn", "sat_603.txt", 20.0, &kSaturnRound,
     {{"Sun", kSun + kInner, "sat_10.txt"}, {"Dione", 73.116, "sat_604.txt"}, {"Rhea", 153.94, "sat_605.txt"}, {"Titan", 8978.14, "sat_606.txt"},
      {"Jupiter", 126712764.1, "sat_5.txt"}}},
};

// ---- flying one leg ----

struct flown {
  std::vector<row> nasa;                         // in the leg's frame
  std::vector<vec3> sim;                         // at NASA's epochs
  std::vector<double> err;                       // m
  std::vector<std::vector<orbit_sample>> paths;  // the attractors, in the leg's frame
  double path = 0.0, worst = 0.0, last = 0.0;
  double nearest = 0.0;                          // m, to the planet (flyby)
  std::string nearest_date;
  double bend = 0.0;                             // degrees between arrival and departure direction (flyby)
};

flown fly(const leg& l, bool with_j2) {
  flown f;
  const frame fr = l.centre ? pole_frame(l.centre->pole_ra, l.centre->pole_dec) : frame{};
  f.nasa = read_horizons(l.truth);
  for (row& r : f.nasa) r.s = {fr.turn(r.s.pos), fr.turn(r.s.vel)};
  for (const mass& m : l.masses) {
    std::vector<orbit_sample> p;
    for (const row& r : read_horizons(m.file)) p.push_back({r.t, fr.turn(r.s.pos)});
    f.paths.push_back(std::move(p));
  }
  // Einstein's correction for the Sun (always the first mass) and for the planet at the centre
  std::vector<attractor> pull;
  for (std::size_t i = 0; i < l.masses.size(); ++i) pull.push_back({l.masses[i].gm * 1e9, f.paths[i], kEinstein && i == 0});

  central_body body{};  // cruise: no central body, a frame that does not turn
  if (l.centre) body = {l.centre->gm * 1e9, 0.0, l.centre->radius * 1e3, with_j2 ? l.centre->j2 : 0.0, kEinstein};

  orbit_state s = f.nasa.front().s;
  f.sim.push_back(s.pos);
  f.err.push_back(0.0);
  f.nearest = norm(s.pos);
  f.nearest_date = f.nasa.front().date;
  for (std::size_t i = 1; i < f.nasa.size(); ++i) {
    const state_vector_ephemeris e{s.pos, s.vel, {}, l.step, f.nasa[i - 1].t, pull};
    const auto next = state_at(e, f.nasa[i].t - f.nasa[i - 1].t, body);
    if (!next) {
      std::fprintf(stderr, "%s: stopped at %s (outside a table, or too many steps)\n", l.name, f.nasa[i].date.c_str());
      std::exit(1);
    }
    s = *next;
    f.sim.push_back(s.pos);
    f.err.push_back(norm(s.pos - f.nasa[i].s.pos));
    f.path += norm(f.nasa[i].s.pos - f.nasa[i - 1].s.pos);
    f.worst = std::max(f.worst, f.err.back());
    if (norm(s.pos) < f.nearest) { f.nearest = norm(s.pos); f.nearest_date = f.nasa[i].date; }
  }
  f.last = f.err.back();
  const vec3 a = f.nasa.front().s.vel, b = s.vel;
  f.bend = std::acos(dot(a, b) / (norm(a) * norm(b))) * 180.0 / kPi;
  return f;
}

// ---- output: a picture (PNG, not compressed) ----

void write_png(const std::string& path, int w, int h, const std::string& rgba) {
  const auto big = [](std::string& s, std::uint32_t v) {
    for (int k = 24; k >= 0; k -= 8) s.push_back(static_cast<char>(v >> k));
  };
  const auto chunk = [&](std::string& file, const char* type, const std::string& data) {
    std::string body = std::string(type, 4) + data;
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char ch : body) {
      crc ^= ch;
      for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    big(file, static_cast<std::uint32_t>(data.size()));
    file += body;
    big(file, ~crc);
  };
  std::string raw;  // every row with a zero in front: "no filter"
  for (int y = 0; y < h; ++y) raw += '\0' + rgba.substr(static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
  std::string z = "\x78\x01";  // zlib, stored blocks
  std::uint32_t a = 1, b = 0;
  for (std::size_t at = 0; at < raw.size(); at += 65535) {
    const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
    z.push_back(at + n == raw.size() ? 1 : 0);
    for (std::size_t v : {n, ~n}) { z.push_back(static_cast<char>(v)); z.push_back(static_cast<char>(v >> 8)); }
    z += raw.substr(at, n);
  }
  for (unsigned char ch : raw) { a = (a + ch) % 65521; b = (b + a) % 65521; }
  big(z, (b << 16) | a);
  std::string head;
  big(head, static_cast<std::uint32_t>(w));
  big(head, static_cast<std::uint32_t>(h));
  head += std::string("\x08\x06\x00\x00\x00", 5);
  std::string file = "\x89PNG\r\n\x1a\n";
  chunk(file, "IHDR", head);
  chunk(file, "IDAT", z);
  chunk(file, "IEND", "");
  std::ofstream(path, std::ios::binary) << file;
}

// ---- output: glTF ----

// Triangles of one colour, or with one surface map.
struct part {
  std::string name;
  float rgb[3];
  std::string map;  // a file in textures/ without ".jpg"; empty: the plain colour
  int shade = 0;    // 0: lit and glowing (readable anywhere), 1: lit only (day and night), 2: glowing only (the sky, the Sun)
  std::vector<float> pos, nrm, uv;
  std::vector<std::uint32_t> idx;
};

// glTF has y up; ours is z up.
void put(part& m, vec3 p, vec3 n, double u = 0.0, double v = 0.0) {
  for (double c : {p.x, p.z, -p.y}) m.pos.push_back(static_cast<float>(c));
  for (double c : {n.x, n.z, -n.y}) m.nrm.push_back(static_cast<float>(c));
  m.uv.push_back(static_cast<float>(u));
  m.uv.push_back(static_cast<float>(v));
}

// A ball with its north pole along z; a surface map wraps once around it, north at the top.
void add_ball(part& m, vec3 c, double r, int around = 12, int rings = 8) {
  const auto base = static_cast<std::uint32_t>(m.pos.size() / 3);
  for (int i = 0; i <= rings; ++i) {
    const double th = kPi * i / rings;
    for (int j = 0; j <= around; ++j) {
      const double ph = 2.0 * kPi * j / around;
      const vec3 n{std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)};
      put(m, c + r * n, n, static_cast<double>(j) / around, static_cast<double>(i) / rings);
    }
  }
  const auto w = static_cast<std::uint32_t>(around + 1);
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(rings); ++i)
    for (std::uint32_t j = 0; j < static_cast<std::uint32_t>(around); ++j) {
      const std::uint32_t a = base + i * w + j, b = a + w;
      for (std::uint32_t k : {a, b, a + 1, a + 1, b, b + 1}) m.idx.push_back(k);
    }
}

void add_tube(part& m, const std::vector<vec3>& p, double r, int sides = 6) {
  if (p.size() < 2) return;
  const auto base = static_cast<std::uint32_t>(m.pos.size() / 3);
  vec3 side{};
  for (std::size_t i = 0; i < p.size(); ++i) {
    vec3 t = p[std::min(i + 1, p.size() - 1)] - p[i > 0 ? i - 1 : 0];
    if (norm(t) == 0.0) t = {1, 0, 0};
    t = (1.0 / norm(t)) * t;
    // carry the sideways direction along the line so that the tube does not twist
    vec3 u = i == 0 ? cross(t, std::fabs(t.z) < 0.9 ? vec3{0, 0, 1} : vec3{1, 0, 0}) : side - dot(side, t) * t;
    if (norm(u) < 1e-9) u = cross(t, {0, 1, 0});
    u = (1.0 / norm(u)) * u;
    side = u;
    const vec3 v = cross(t, u);
    for (int k = 0; k < sides; ++k) {
      const double a = 2.0 * kPi * k / sides;
      const vec3 n = std::cos(a) * u + std::sin(a) * v;
      put(m, p[i] + r * n, n);
    }
  }
  const auto s = static_cast<std::uint32_t>(sides);
  for (std::uint32_t i = 0; i + 1 < p.size(); ++i)
    for (std::uint32_t k = 0; k < s; ++k) {
      const std::uint32_t a = base + i * s + k, b = base + i * s + (k + 1) % s;
      for (std::uint32_t q : {a, b, a + s, b, b + s, a + s}) m.idx.push_back(q);
    }
}

std::string file_bytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// A binary glTF 2.0 file: one mesh; per part one primitive and one material, plain
// or with the part's surface map packed into the file.
void write_glb(const std::string& path, const std::vector<part>& parts) {
  std::string bin, views, accessors, prims, materials, images, textures;
  int view = 0, accessor = 0, material = 0, image = 0;
  char b[900];
  const auto blob = [&](const void* d, std::size_t n, int target) {
    const std::size_t at = bin.size();
    bin.append(static_cast<const char*>(d), n);
    while (bin.size() % 4) bin.push_back('\0');
    char t[40] = "";
    if (target) std::snprintf(t, sizeof t, ",\"target\":%d", target);
    std::snprintf(b, sizeof b, "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu%s}", view ? "," : "", at, n, t);
    views += b;
    return view++;
  };
  const auto vectors = [&](const std::vector<float>& d, const char* type, int width, bool bounds) {
    const int v = blob(d.data(), d.size() * 4, 34962);
    std::string minmax;
    if (bounds) {
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (std::size_t i = 0; i < d.size(); ++i) { lo[i % 3] = std::min(lo[i % 3], d[i]); hi[i % 3] = std::max(hi[i % 3], d[i]); }
      std::snprintf(b, sizeof b, ",\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
      minmax = b;
    }
    std::snprintf(b, sizeof b, "%s{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"%s\"%s}", accessor ? "," : "", v,
                  d.size() / static_cast<std::size_t>(width), type, minmax.c_str());
    accessors += b;
    return accessor++;
  };
  for (const part& p : parts) {
    if (p.idx.empty()) continue;
    const std::string jpeg = p.map.empty() ? "" : file_bytes(g_maps + "/" + p.map + ".jpg");
    const int ap = vectors(p.pos, "VEC3", 3, true), an = vectors(p.nrm, "VEC3", 3, false);
    const int at = vectors(p.uv, "VEC2", 2, false);  // also without a map of its own: the game may bring one
    const int vi = blob(p.idx.data(), p.idx.size() * 4, 34963);
    std::snprintf(b, sizeof b, ",{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}", vi, p.idx.size());
    accessors += b;
    const int ai = accessor++;
    char tc[40] = "";
    if (at >= 0) std::snprintf(tc, sizeof tc, ",\"TEXCOORD_0\":%d", at);
    std::snprintf(b, sizeof b, "%s{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d%s},\"indices\":%d,\"material\":%d,\"mode\":4}", material ? "," : "", ap, an, tc, ai, material);
    prims += b;
    if (jpeg.empty()) {
      const float base = p.shade == 2 ? 0.0f : 1.0f, glow = p.shade == 1 ? 0.0f : p.shade == 2 ? 1.0f : 0.6f;
      std::snprintf(b, sizeof b, "%s{\"name\":\"%s\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.3f,%.3f,%.3f,1],\"metallicFactor\":0,\"roughnessFactor\":0.9},"
                    "\"emissiveFactor\":[%.3f,%.3f,%.3f]}",
                    material ? "," : "", p.name.c_str(), base * p.rgb[0], base * p.rgb[1], base * p.rgb[2], glow * p.rgb[0], glow * p.rgb[1], glow * p.rgb[2]);
    } else {
      const int vj = blob(jpeg.data(), jpeg.size(), 0);
      std::snprintf(b, sizeof b, "%s{\"bufferView\":%d,\"mimeType\":\"image/jpeg\",\"name\":\"%s\"}", image ? "," : "", vj, p.map.c_str());
      images += b;
      std::snprintf(b, sizeof b, "%s{\"source\":%d,\"sampler\":0}", image ? "," : "", image);
      textures += b;
      if (p.shade == 1)
        std::snprintf(b, sizeof b, "%s{\"name\":\"%s\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":%d},\"metallicFactor\":0,\"roughnessFactor\":1.0}}",
                      material ? "," : "", p.name.c_str(), image);
      else if (p.shade == 2)
        std::snprintf(b, sizeof b, "%s{\"name\":\"%s\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[0,0,0,1],\"metallicFactor\":0,\"roughnessFactor\":1.0},"
                      "\"emissiveTexture\":{\"index\":%d},\"emissiveFactor\":[1,1,1]}", material ? "," : "", p.name.c_str(), image);
      else
        std::snprintf(b, sizeof b, "%s{\"name\":\"%s\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":%d},\"metallicFactor\":0,\"roughnessFactor\":0.9},"
                      "\"emissiveTexture\":{\"index\":%d},\"emissiveFactor\":[0.7,0.7,0.7]}", material ? "," : "", p.name.c_str(), image, image);
      ++image;
    }
    materials += b;
    ++material;
  }
  std::string js = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"voyager2 (orbit.hpp)\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                   "\"nodes\":[{\"mesh\":0,\"name\":\"voyager2\"}],\"meshes\":[{\"primitives\":[" + prims + "]}],\"materials\":[" + materials + "],";
  if (image) js += "\"images\":[" + images + "],\"textures\":[" + textures + "],\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,\"wrapS\":10497,\"wrapT\":33071}],";
  js += "\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],\"bufferViews\":[" + views + "],\"accessors\":[" + accessors + "]}";
  while (js.size() % 4) js.push_back(' ');
  const auto u32 = [](std::ofstream& f, std::size_t v) { const auto x = static_cast<std::uint32_t>(v); f.write(reinterpret_cast<const char*>(&x), 4); };
  std::ofstream f(path, std::ios::binary);
  u32(f, 0x46546C67); u32(f, 2); u32(f, 12 + 8 + js.size() + 8 + bin.size());
  u32(f, js.size()); u32(f, 0x4E4F534A); f.write(js.data(), static_cast<std::streamsize>(js.size()));
  u32(f, bin.size()); u32(f, 0x004E4942); f.write(bin.data(), static_cast<std::streamsize>(bin.size()));
}

// A planet or a moon as a ball of its own, with its surface map if there is one in textures/.
part ball_of(const std::string& name, vec3 c, double r, int around, int rings) {
  std::string map = name;
  for (char& ch : map) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  part p{name, {0.62f, 0.85f, 0.88f}, std::ifstream(g_maps + "/" + map + ".jpg").good() ? map : "", 0, {}, {}, {}, {}};
  add_ball(p, c, r, around, rings);
  return p;
}

// One view: the simulated trajectory as a line, the bodies around it.
struct view {
  part nasa{"NASA trajectory", {0.82f, 0.80f, 0.74f}, "", 0, {}, {}, {}, {}};
  part sim{"simulated", {0.16f, 0.47f, 0.84f}, "", 0, {}, {}, {}, {}};
  part orbits{"orbits", {0.50f, 0.50f, 0.48f}, "", 0, {}, {}, {}, {}};
  part moons{"moon orbits", {0.92f, 0.41f, 0.20f}, "", 0, {}, {}, {}, {}};
  part sun{"Sun", {1.0f, 0.82f, 0.25f}, "", 0, {}, {}, {}, {}};
  std::vector<part> balls;  // planets and moons

  // Writes <dir>/<name>.glb, centred and scaled to reach 150 units from the middle (the
  // size GameApi's default camera sees), and <dir>/<name>.txt, a GameApi script that
  // shows it tilted by `tilt` radians, with NASA's model of the spacecraft at `probe`.
  // Returns units per `unit`.
  double save(const std::string& dir, const std::string& name, vec3 probe, double tilt) {
    std::vector<part> all{nasa, sim, orbits, moons, sun};
    all.insert(all.end(), balls.begin(), balls.end());
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (std::size_t q = 1; q < 2; ++q)  // the simulated line frames the view
      for (std::size_t i = 0; i < all[q].pos.size(); ++i) { lo[i % 3] = std::min(lo[i % 3], all[q].pos[i]); hi[i % 3] = std::max(hi[i % 3], all[q].pos[i]); }
    float mid[3], half = 1e-9f;
    for (int i = 0; i < 3; ++i) { mid[i] = (lo[i] + hi[i]) / 2; half = std::max(half, (hi[i] - lo[i]) / 2); }
    const float k = 150.0f / half;
    for (part& p : all)
      for (std::size_t i = 0; i < p.pos.size(); ++i) p.pos[i] = (p.pos[i] - mid[i % 3]) * k;
    write_glb(dir + "/" + name + ".glb", all);
    std::ofstream f(dir + "/" + name + ".txt");
    char b[200], c[80];
    std::snprintf(b, sizeof b, "MN I6=ev.move_api.trans2(I5,%.2f,%.2f,%.2f);\n", k * (probe.x - mid[0]), k * (probe.z - mid[1]), k * (-probe.y - mid[2]));
    std::snprintf(c, sizeof c, "MN I11=ev.move_api.rotatex(I10,%.2f);\n", tilt);
    // GameApi fetches files with curl; $(pwd) is its own name for the folder it runs in
    f << "TF I1=ev.mainloop_api.gltf_loadKK2(file://$(pwd)/" << name << ".glb);\n"
         "ML I2=ev.mainloop_api.gltf_mesh_all(ev,I1,1.0,1.0,1.0,0,400.0,-400.0,300.0,0.0,ff000000,false,true,true);\n"
         "TF I3=ev.mainloop_api.gltf_loadKK2(file://$(pwd)/voyager-probe-a.glb);\n"
         "ML I4=ev.mainloop_api.gltf_mesh_all(ev,I3,1.0,1.0,1.0,0,400.0,-400.0,300.0,0.0,ff000000,false,true,true);\n"
         "MN I5=ev.move_api.mn_empty();\n"
      << b
      << "MN I7=ev.move_api.scale2(I6,40.0,40.0,40.0);\n"
      << "ML I8=ev.move_api.move_ml(ev,I4,I7,1,10.0);\n"
         "ML I9=ev.mainloop_api.or_elem_ml(ev,I2,I8);\n"
         "MN I10=ev.move_api.mn_empty();\n"
      << c
      << "ML I12=ev.move_api.move_ml(ev,I9,I11,1,10.0);\n"
         "ML I13=ev.mainloop_api.touch_rotate(ev,I12,true,true,0.01,0.01);\n"
         "RUN I14=ev.blocker_api.game_window2(ev,I13,false,false,0.0,100000.0);\n";
    return k;
  }
};

// Half the largest extent of a set of points: the measure the marks of a view are sized by.
double half_extent(const std::vector<vec3>& p) {
  vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  for (const vec3& v : p) {
    lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
    hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
  }
  return std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) / 2.0;
}

std::vector<vec3> scaled(const std::vector<orbit_sample>& p, double t0, double t1, double unit, const frame& fr) {
  std::vector<vec3> o;
  for (const orbit_sample& s : p) if (s.t >= t0 && s.t <= t1) o.push_back((1.0 / unit) * fr.turn(s.pos));
  return o;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) g_data = argv[1];
  const std::string out = argc > 2 ? argv[2] : "out";

  std::printf("Voyager 2, simulated with orbit.hpp, against NASA's trajectory (JPL Horizons)\n\n");
  std::printf("%-19s %-17s %7s %14s %12s %12s  %-14s\n", "leg", "from", "days", "path, km", "error at end", "largest", "error / path");
  std::vector<flown> flights;
  for (const leg& l : kLegs) {
    flights.push_back(fly(l, true));
    const flown& f = flights.back();
    std::printf("%-19s %-17s %7.1f %14.0f %9.1f km %9.1f km  1 in %-10.0f\n", l.name, f.nasa.front().date.c_str(),
                (f.nasa.back().t - f.nasa.front().t) / 86400.0, f.path / 1e3, f.last / 1e3, f.worst / 1e3, f.path / std::max(f.worst, 1e-9));
  }
  std::printf("\nFlybys: the planet is the central body, with and without its flattening (J2)\n");
  std::printf("%-15s %-17s %12s %8s %16s %16s\n", "", "nearest (TDB)", "from centre", "turned", "error with J2", "without J2");
  for (std::size_t i = 0; i < kLegs.size(); ++i) {
    if (!kLegs[i].centre) continue;
    const flown plain = fly(kLegs[i], false);
    const flown& f = flights[i];
    std::printf("%-15s %-17s %9.0f km %6.1f deg %13.1f km %13.1f km\n", kLegs[i].name, f.nearest_date.c_str(), f.nearest / 1e3, f.bend,
                f.worst / 1e3, plain.worst / 1e3);
  }

  std::printf("\nControls: moons and planets, whose paths are gravity alone, flown by the same code\n");
  for (const leg& l : kControls) {
    const flown f = fly(l, true);
    std::printf("%-21s %7.1f days %14.0f km  largest error %9.1f km  1 in %.0f\n", l.name, (f.nasa.back().t - f.nasa.front().t) / 86400.0,
                f.path / 1e3, f.worst / 1e3, f.path / std::max(f.worst, 1e-9));
  }

  // ---- views ----
  // The route among the planets, in astronomical units, the ecliptic as the flat plane.
  const frame ecl = ecliptic();
  const flown& late = flights[8];  // its attractors are the Sun and the four giant planets
  const std::size_t flyby_leg[4] = {1, 3, 5, 7};
  for (int whole = 0; whole < 2; ++whole) {
    view v;
    const double t_end = whole ? 1e300 : flights[7].nasa.back().t;
    std::vector<vec3> nasa, sim;
    for (std::size_t i = 0; i < kLegs.size(); ++i) {
      if (kLegs[i].centre) continue;
      for (std::size_t j = 0; j < flights[i].nasa.size() && flights[i].nasa[j].t <= t_end; ++j) {
        nasa.push_back((1.0 / kAu) * ecl.turn(flights[i].nasa[j].s.pos));
        sim.push_back((1.0 / kAu) * ecl.turn(flights[i].sim[j]));
      }
    }
    const double h = half_extent(nasa);
    add_tube(v.sim, sim, h / 220.0);
    for (std::size_t p = 0; p < 4; ++p) {
      const std::vector<orbit_sample>& path = late.paths[1 + p];
      add_tube(v.orbits, scaled(path, -1e300, whole ? 1e300 : t_end + 3e7, kAu, ecl), h / 500.0);
      const auto at = state_at(std::span<const orbit_sample>(path), flights[flyby_leg[p]].nasa.front().t);
      if (at) v.balls.push_back(ball_of(kLegs[flyby_leg[p]].centre->name, (1.0 / kAu) * ecl.turn(at->pos), h / 40.0, 24, 14));  // where Voyager met it
    }
    add_ball(v.sun, {0, 0, 0}, h / 35.0);
    const double k = v.save(out, whole ? "route-whole" : "route-planets", sim.back(), 1.0);
    if (!whole) std::printf("\nviews written to %s/\n", out.c_str());
    std::printf("%-17s 1 unit = %.3f AU\n", whole ? "route-whole.glb" : "route-planets.glb", 1.0 / k);
  }
  // Each flyby in the planet's own frame, in thousands of km, the planet to scale.
  const char* files[4] = {"jupiter", "saturn", "uranus", "neptune"};
  for (std::size_t n = 0; n < 4; ++n) {
    const leg& l = kLegs[flyby_leg[n]];
    const flown& f = flights[flyby_leg[n]];
    const double unit = 1e6;
    double reach = 6.0 * f.nearest;  // the part of the leg shown: out to past the farthest moon
    for (std::size_t m = 1; m < l.masses.size(); ++m)
      if (l.masses[m].gm < 1e5 && norm(f.paths[m].front().pos) < 2.0e9) reach = std::max(reach, 1.6 * norm(f.paths[m].front().pos));
    view v;
    std::vector<vec3> nasa, sim;
    double tn = f.nasa.front().t;
    for (std::size_t j = 0; j < f.nasa.size(); ++j) {
      if (f.nasa[j].date == f.nearest_date) tn = f.nasa[j].t;
      if (norm(f.nasa[j].s.pos) > reach) continue;
      nasa.push_back((1.0 / unit) * f.nasa[j].s.pos);
      sim.push_back((1.0 / unit) * f.sim[j]);
    }
    const double h = half_extent(nasa);
    add_tube(v.sim, sim, h / 220.0);
    for (std::size_t m = 1; m < l.masses.size(); ++m) {
      if (norm(f.paths[m].front().pos) > reach) continue;  // the Sun, another planet
      add_tube(v.moons, scaled(f.paths[m], f.nasa.front().t, f.nasa.back().t, unit, frame{}), h / 500.0);
      const auto at = state_at(std::span<const orbit_sample>(f.paths[m]), tn);  // the moon when Voyager was nearest
      if (at) v.balls.push_back(ball_of(l.masses[m].name, (1.0 / unit) * at->pos, h / 45.0, 24, 14));
    }
    v.balls.push_back(ball_of(l.centre->name, {0, 0, 0}, l.centre->radius * 1e3 / unit, 48, 28));
    vec3 probe{};  // the spacecraft where it was nearest to the planet
    for (std::size_t j = 0; j < f.nasa.size(); ++j) if (f.nasa[j].t == tn) probe = (1.0 / unit) * f.sim[j];
    const double k = v.save(out, files[n], probe, 0.6);
    std::printf("%-17s 1 unit = %.0f km\n", (std::string(files[n]) + ".glb").c_str(), 1000.0 / k);
  }
  std::printf("each with a GameApi script of the same name (.txt)\n");

  // ---- the game's data: each encounter as seen from the spacecraft ----
  // out/<planet>.scene: the planet, its moons and the Sun relative to Voyager 2 at
  // every epoch, km, in the planet's frame (z along its pole). The game (game.cpp)
  // puts the spacecraft in the middle and these around it.
  for (std::size_t n = 0; n < 4; ++n) {
    const leg& l = kLegs[flyby_leg[n]];
    const flown& f = flights[flyby_leg[n]];
    std::ofstream o(out + "/" + files[n] + ".scene");
    o.precision(10);
    o << "planet " << l.centre->name << " " << l.centre->radius << " " << l.centre->day << "\n";
    {  // the planet's frame: its three axes in celestial (ICRF) axes
      const frame axes = pole_frame(l.centre->pole_ra, l.centre->pole_dec);
      o << "frame";
      for (const vec3& r : {axes.x, axes.y, axes.z}) o << " " << r.x << " " << r.y << " " << r.z;
      o << "\n";
    }
    o << "epochs " << f.nasa.size() << " " << f.nasa[1].t - f.nasa[0].t << " " << f.nasa.front().date << "\n";
    o << "body " << l.centre->name << " " << l.centre->radius << "\n";
    for (const vec3& p : f.sim) o << -p.x / 1e3 << " " << -p.y / 1e3 << " " << -p.z / 1e3 << "\n";
    for (std::size_t m = 0; m < l.masses.size(); ++m) {
      const bool sun = m == 0;
      if (!sun && moon_radius(l.masses[m].name) == 0.0) continue;  // another planet
      o << "body " << l.masses[m].name << " " << (sun ? 695700.0 : moon_radius(l.masses[m].name)) << "\n";
      for (std::size_t j = 0; j < f.sim.size(); ++j) {
        const auto at = state_at(std::span<const orbit_sample>(f.paths[m]), f.nasa[j].t);
        const vec3 d = (at ? at->pos : vec3{}) - f.sim[j];
        o << d.x / 1e3 << " " << d.y / 1e3 << " " << d.z / 1e3 << "\n";
      }
    }
    // the Earth: not a mass of the model, but where the spacecraft's dish points
    const frame fr = pole_frame(l.centre->pole_ra, l.centre->pole_dec);
    std::vector<orbit_sample> earth;
    for (const row& r : read_horizons(std::string(l.truth).substr(0, 3) + "_399.txt")) earth.push_back({r.t, fr.turn(r.s.pos)});
    o << "body Earth 6371\n";
    for (std::size_t j = 0; j < f.sim.size(); ++j) {
      const auto at = state_at(std::span<const orbit_sample>(earth), f.nasa[j].t);
      const vec3 d = (at ? at->pos : vec3{}) - f.sim[j];
      o << d.x / 1e3 << " " << d.y / 1e3 << " " << d.z / 1e3 << "\n";
    }
  }
  // out/stars-<1|2|3>.glb: the real stars, one small square each, every star where
  // the Hipparcos catalogue (ESA 1997) has it: data/stars.txt holds right
  // ascension, declination (degrees, ICRS, at 1991.25), V magnitude, B-V colour,
  // proper motion (mas a year) and parallax (mas). Each star is moved back to 1984
  // by its proper motion. The files are in celestial axes (x to the equinox, z to
  // the celestial pole); the game turns them into each planet's frame, which every
  // scene file gives. One set serves all four planets: the nearest star shifts by
  // 22 seconds of arc between here and Neptune, the fastest by 50 between 1979 and
  // 1989, a pixel or less by eye. Three files: for the eye (to magnitude 6.5), and for
  // the wide-angle and narrow-angle cameras (to 8 and 9), whose squares are that
  // many times smaller, since a camera magnifies them again.
  // out/star-dots.png: the dots. Eight columns of colour (blue-white to orange,
  // by B-V), sixteen rows of brightness, each three quarters as solid as the next.
  {
    const int tile = 8;
    const double tint[8][3] = {{179, 202, 255}, {204, 219, 255}, {236, 238, 255}, {255, 243, 239}, {255, 228, 206}, {255, 215, 177}, {255, 203, 152}, {255, 190, 122}};
    std::string dots;  // rows from the top: red, green, blue, opacity
    for (int y = 0; y < kStarRows * tile; ++y)
      for (int x = 0; x < 8 * tile; ++x) {
        const double dx = x % tile + 0.5 - tile / 2.0, dy = y % tile + 0.5 - tile / 2.0, sigma = tile / 6.0;
        (void)sigma;
        const double g = std::clamp(tile / 2.0 - std::sqrt(dx * dx + dy * dy), 0.0, 1.0);  // solid, with a soft last texel: the dot is a pixel, and a pixel has no inside to shade
        const double* c = tint[x / tile];
        // the eye sees little colour in a point of light: the tint at 40 %
        for (int k : {0, 1, 2}) dots.push_back(static_cast<char>(static_cast<unsigned char>(255.0 + 0.4 * (c[k] - 255.0))));
        dots.push_back(static_cast<char>(static_cast<unsigned char>(255.0 * g * std::pow(0.75, kStarRows - 1 - y / tile) + 0.5)));
      }
    write_png(out + "/star-dots.png", 8 * tile, kStarRows * tile, dots);
  }
  struct star { double ra, dec, mag, bv, pm_ra, pm_dec, parallax; };
  std::vector<star> stars;
  {
    std::ifstream f(g_data + "/stars.txt");
    star s{};
    while (f >> s.ra >> s.dec >> s.mag >> s.bv >> s.pm_ra >> s.pm_dec >> s.parallax) stars.push_back(s);
  }
  // `full`: the magnitude from which a dot is fully solid (a camera gathers more light).
  const double reach[3] = {6.5, 8.0, 9.0}, full[3] = {2.0, 3.5, 5.0}, lens[3] = {1.0, 5.4, 40.0};
  for (int cam = 0; cam < 3; ++cam) {
    part p{"stars", {1.0f, 1.0f, 1.0f}, "", 2, {}, {}, {}, {}};
    put(p, {-1, -1, -1}, {0, 0, 1});
    put(p, {1, 1, 1}, {0, 0, 1});
    put(p, {-1, -1, -1}, {0, 0, 1});
    for (std::uint32_t i : {0u, 1u, 2u}) p.idx.push_back(i);
    const double pixel = 3.76e-4 / lens[cam];  // one pixel of the game's view, radians
    for (const star& s : stars) {
      if (s.mag > reach[cam]) continue;
      const double dt = 1984.0 - 1991.25, mas = kPi / 180.0 / 3.6e6;  // the middle of the four flybys
      const double dec = s.dec * kPi / 180.0 + s.pm_dec * dt * mas;
      const double ra = s.ra * kPi / 180.0 + s.pm_ra * dt * mas / std::cos(dec);
      const vec3 d{std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec)};
      // A star is a point: every dot is one pixel of light, and a brighter star is a more solid
      // pixel, never a larger dot. How solid: the star's light to the power 0.4, which keeps
      // the faint ones in sight beside the bright, as an eye does.
      const double half = 0.65 * pixel;
      const double solid = std::min(1.0, std::pow(10.0, -0.4 * 0.4 * (s.mag - full[cam])));
      const int row = std::clamp(kStarRows - 1 + static_cast<int>(std::lround(std::log(solid) / std::log(1.0 / 0.75))), 0, kStarRows - 1);
      const double edges[7] = {-0.12, 0.15, 0.45, 0.75, 1.05, 1.35, 1.7};
      int col = 0;
      while (col < 7 && s.bv > edges[col]) ++col;
      vec3 e1 = cross(std::fabs(d.z) < 0.9 ? vec3{0, 0, 1} : vec3{1, 0, 0}, d);
      e1 = (1.0 / norm(e1)) * e1;
      const vec3 e2 = cross(d, e1);
      const auto base = static_cast<std::uint32_t>(p.pos.size() / 3);
      for (int k = 0; k < 4; ++k) {  // up and left, up and right, down and left, down and right, seen from inside
        const double a = k % 2 ? -half : half, b = k < 2 ? half : -half;
        put(p, d + a * e1 + b * e2, -1.0 * d, (col + (k % 2 ? 1.0 : 0.0)) / 8.0, (row + (k < 2 ? 0.0 : 1.0)) / kStarRows);
      }
      for (std::uint32_t k : {0u, 1u, 2u, 1u, 3u, 2u}) p.idx.push_back(base + k);
    }
    write_glb(out + "/stars-" + std::to_string(cam + 1) + ".glb", {p});
  }
  // out/star-names.txt: the well-known bright stars, for the game to name: each looked up in the
  // catalogue near where it is known to be (right ascension in hours, declination in degrees),
  // so that the name sits on the catalogue's own star.
  {
    struct named { const char* name; double ra_h, dec; };
    const named known[] = {
        {"Sirius", 6.752, -16.72}, {"Canopus", 6.399, -52.70}, {"Alpha Centauri", 14.660, -60.83}, {"Arcturus", 14.261, 19.18}, {"Vega", 18.616, 38.78},
        {"Capella", 5.278, 46.00}, {"Rigel", 5.242, -8.20}, {"Procyon", 7.655, 5.22}, {"Achernar", 1.629, -57.24}, {"Betelgeuse", 5.919, 7.41},
        {"Hadar", 14.064, -60.37}, {"Altair", 19.846, 8.87}, {"Acrux", 12.443, -63.10}, {"Aldebaran", 4.599, 16.51}, {"Antares", 16.490, -26.43},
        {"Spica", 13.420, -11.16}, {"Pollux", 7.755, 28.03}, {"Fomalhaut", 22.961, -29.62}, {"Deneb", 20.690, 45.28}, {"Mimosa", 12.795, -59.69},
        {"Regulus", 10.140, 11.97}, {"Adhara", 6.977, -28.97}, {"Castor", 7.577, 31.89}, {"Shaula", 17.560, -37.10}, {"Gacrux", 12.519, -57.11},
        {"Bellatrix", 5.419, 6.35}, {"Elnath", 5.438, 28.61}, {"Miaplacidus", 9.220, -69.72}, {"Alnilam", 5.604, -1.20}, {"Alnair", 22.137, -46.96},
        {"Alioth", 12.900, 55.96}, {"Dubhe", 11.062, 61.75}, {"Mirfak", 3.405, 49.86}, {"Alkaid", 13.792, 49.31}, {"Polaris", 2.530, 89.26},
        {"Alphard", 9.460, -8.66}, {"Hamal", 2.120, 23.46}, {"Denebola", 11.818, 14.57}, {"Alpheratz", 0.140, 29.09}, {"Mizar", 13.399, 54.93},
        {"Kaus Australis", 18.403, -34.38}, {"Sargas", 17.622, -43.00}, {"Avior", 8.375, -59.51}, {"Atria", 16.811, -69.03}, {"Alhena", 6.629, 16.40},
        {"Peacock", 20.427, -56.74}, {"Wezen", 7.140, -26.39}, {"Nunki", 18.921, -26.30}, {"Rasalhague", 17.582, 12.56}, {"Diphda", 0.727, -17.99},
        {"Mirach", 1.162, 35.62}, {"Algol", 3.136, 40.96}, {"Saiph", 5.796, -9.67}, {"Mintaka", 5.533, -0.30}, {"Alnitak", 5.679, -1.94}, {"Kochab", 14.845, 74.16}};
    std::ofstream f(out + "/star-names.txt");
    for (const named& k : known) {
      const double a = k.ra_h * 15.0 * kPi / 180.0, b = k.dec * kPi / 180.0;
      const vec3 want{std::cos(b) * std::cos(a), std::cos(b) * std::sin(a), std::sin(b)};
      const star* found = nullptr;
      for (const star& s : stars) {  // the brightest within 0.4 degrees
        const double ra = s.ra * kPi / 180.0, dec = s.dec * kPi / 180.0;
        const vec3 d{std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec)};
        if (dot(d, want) > std::cos(0.4 * kPi / 180.0) && (!found || s.mag < found->mag)) found = &s;
      }
      if (!found || found->mag > 3.2) {
        std::fprintf(stderr, "star names: no bright star in the catalogue where %s should be\n", k.name);
        continue;
      }
      std::string name = k.name;
      for (char& ch : name) if (ch == ' ') ch = '_';
      const double dt = 1984.0 - 1991.25, mas = kPi / 180.0 / 3.6e6;
      const double dec = found->dec + found->pm_dec * dt * mas * 180.0 / kPi;
      f << name << ' ' << found->ra + found->pm_ra * dt * mas * 180.0 / kPi / std::cos(dec * kPi / 180.0) << ' ' << dec << ' ' << found->mag << '\n';
    }
  }
  // out/ring.glb: Saturn's rings, flat in the planet's equator, in units of the
  // planet's radius, from the inner edge of the D ring to the F ring. The texture
  // coordinate runs outward across them.
  {
    const double inner = 1.11, outer = 2.33;
    part p{"rings", {0.8f, 0.74f, 0.62f}, "", 0, {}, {}, {}, {}};
    put(p, {-outer, -outer, -outer}, {0, 0, 1});
    put(p, {outer, outer, outer}, {0, 0, 1});
    put(p, {-outer, -outer, -outer}, {0, 0, 1});
    for (std::uint32_t i : {0u, 1u, 2u}) p.idx.push_back(i);
    const int around = 180;
    for (int side = 0; side < 2; ++side) {  // seen from above and from below
      const auto base = static_cast<std::uint32_t>(p.pos.size() / 3);
      for (int j = 0; j <= around; ++j) {
        const double ph = 2.0 * kPi * j / around;
        put(p, {inner * std::cos(ph), inner * std::sin(ph), 0}, {0, 0, side ? -1.0 : 1.0}, 0.0, 0.5);
        put(p, {outer * std::cos(ph), outer * std::sin(ph), 0}, {0, 0, side ? -1.0 : 1.0}, 1.0, 0.5);
      }
      for (std::uint32_t j = 0; j < static_cast<std::uint32_t>(around); ++j) {
        const std::uint32_t q = base + 2 * j, b = q + 1, c = q + 2, d = q + 3;
        const std::uint32_t up[6] = {q, b, c, c, b, d}, down[6] = {q, c, b, b, c, d};
        for (std::uint32_t k : side ? down : up) p.idx.push_back(k);
      }
    }
    write_glb(out + "/ring.glb", {p});
  }
  // out/ball-<name>.glb: a ball of radius 1 with the body's surface map
  // (ball-shade.glb: without one; the game draws sunlight with it). GameApi
  // measures every model it loads and scales it to its own size, by sampling the
  // model's triangles; the first triangle here is a flat one through two opposite
  // corners of the ball's box, so that it measures exactly that box.
  for (const char* name : {"Jupiter", "Saturn", "Uranus", "Neptune", "Io", "Europa", "Ganymede", "Callisto", "Mimas", "Enceladus", "Tethys", "Dione", "Rhea", "Titan",
                           "Iapetus", "Ariel", "Umbriel", "Titania", "Oberon", "Miranda", "Triton", "Sun", "Shade"}) {
    const bool sky = false;
    part p{name, {0.62f, 0.85f, 0.88f}, "", 1, {}, {}, {}, {}};
    if (std::string(name) == "Sun") p.rgb[0] = 1.0f, p.rgb[1] = 0.96f, p.rgb[2] = 0.92f;  // sunlight is white
    if (sky || std::string(name) == "Sun") p.shade = 2;
    put(p, {-1, -1, -1}, {0, 0, 1});
    put(p, {1, 1, 1}, {0, 0, 1});
    put(p, {-1, -1, -1}, {0, 0, 1});
    for (std::uint32_t i : {0u, 1u, 2u}) p.idx.push_back(i);
    part b = ball_of(name, {0, 0, 0}, 1.0, 64, 36);
    p.map = b.map;
    const auto base = static_cast<std::uint32_t>(p.pos.size() / 3);
    p.pos.insert(p.pos.end(), b.pos.begin(), b.pos.end());
    p.nrm.insert(p.nrm.end(), b.nrm.begin(), b.nrm.end());
    p.uv.insert(p.uv.end(), b.uv.begin(), b.uv.end());
    for (std::size_t i = 0; i < b.idx.size(); i += 3) {  // the sky is seen from inside
      const std::uint32_t t[3] = {b.idx[i], b.idx[i + (sky ? 2 : 1)], b.idx[i + (sky ? 1 : 2)]};
      for (std::uint32_t v : t) p.idx.push_back(base + v);
    }
    std::string file = name;
    for (char& ch : file) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    write_glb(out + "/ball-" + file + ".glb", {p});
  }
  std::printf("game data: %s/<planet>.scene and %s/ball-<body>.glb\n", out.c_str(), out.c_str());
  return 0;
}

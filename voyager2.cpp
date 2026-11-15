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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "orbit.hpp"

using namespace glass::orbit;

namespace {

constexpr double kPi = 3.14159265358979323846;
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
};
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
const planet kJupiter{"Jupiter", 126686531.900, 71492.0, 0.014696572, 268.056595, 64.495303};
const planet kSaturn{"Saturn", 37931206.234, 60330.0, 0.016290573, 40.5956, 83.5377};
const planet kUranus{"Uranus", 5793950.6103, 25559.0, 0.00351068, 257.311, -15.175};
const planet kNeptune{"Neptune", 6835099.97, 25225.0, 0.00340843, 299.268, 42.954};

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
     {{"Sun", kSun + kInner, "sat_10.txt"}, {"Tethys", 41.21, "sat_603.txt"}, {"Dione", 73.116, "sat_604.txt"},
      {"Rhea", 153.94, "sat_605.txt"}, {"Titan", 8978.14, "sat_606.txt"}, {"Jupiter", 126712764.1, "sat_5.txt"}}},
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

// ---- output: glTF ----

// Triangles of one colour.
struct part {
  std::string name;
  float rgb[3];
  std::vector<float> pos, nrm;
  std::vector<std::uint32_t> idx;
};

// glTF has y up; ours is z up.
void put(part& m, vec3 p, vec3 n) {
  for (double v : {p.x, p.z, -p.y}) m.pos.push_back(static_cast<float>(v));
  for (double v : {n.x, n.z, -n.y}) m.nrm.push_back(static_cast<float>(v));
}

void add_ball(part& m, vec3 c, double r, int around = 12, int rings = 8) {
  const auto base = static_cast<std::uint32_t>(m.pos.size() / 3);
  for (int i = 0; i <= rings; ++i) {
    const double th = kPi * i / rings;
    for (int j = 0; j <= around; ++j) {
      const double ph = 2.0 * kPi * j / around;
      const vec3 n{std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)};
      put(m, c + r * n, n);
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

// A binary glTF 2.0 file: one mesh, one primitive and one plain material per part.
void write_glb(const std::string& path, const std::vector<part>& parts) {
  std::string bin, views, accessors, prims, materials;
  int view = 0, m = 0;
  const auto blob = [&](const void* d, std::size_t n, int target) {
    const std::size_t at = bin.size();
    bin.append(static_cast<const char*>(d), n);
    while (bin.size() % 4) bin.push_back('\0');
    char b[160];
    std::snprintf(b, sizeof b, "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":%d}", view ? "," : "", at, n, target);
    views += b;
    return view++;
  };
  for (const part& p : parts) {
    if (p.idx.empty()) continue;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (std::size_t i = 0; i < p.pos.size(); ++i) { lo[i % 3] = std::min(lo[i % 3], p.pos[i]); hi[i % 3] = std::max(hi[i % 3], p.pos[i]); }
    const int vp = blob(p.pos.data(), p.pos.size() * 4, 34962);
    const int vn = blob(p.nrm.data(), p.nrm.size() * 4, 34962);
    const int vi = blob(p.idx.data(), p.idx.size() * 4, 34963);
    char b[700];
    std::snprintf(b, sizeof b,
                  "%s{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\",\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]},"
                  "{\"bufferView\":%d,\"componentType\":5126,\"count\":%zu,\"type\":\"VEC3\"},"
                  "{\"bufferView\":%d,\"componentType\":5125,\"count\":%zu,\"type\":\"SCALAR\"}",
                  m ? "," : "", vp, p.pos.size() / 3, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], vn, p.nrm.size() / 3, vi, p.idx.size());
    accessors += b;
    std::snprintf(b, sizeof b, "%s{\"attributes\":{\"POSITION\":%d,\"NORMAL\":%d},\"indices\":%d,\"material\":%d,\"mode\":4}", m ? "," : "", 3 * m, 3 * m + 1, 3 * m + 2, m);
    prims += b;
    std::snprintf(b, sizeof b, "%s{\"name\":\"%s\",\"pbrMetallicRoughness\":{\"baseColorFactor\":[%.3f,%.3f,%.3f,1],\"metallicFactor\":0,\"roughnessFactor\":0.9}}",
                  m ? "," : "", p.name.c_str(), p.rgb[0], p.rgb[1], p.rgb[2]);
    materials += b;
    ++m;
  }
  std::string js = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"voyager2 (orbit.hpp)\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
                   "\"nodes\":[{\"mesh\":0,\"name\":\"voyager2\"}],\"meshes\":[{\"primitives\":[" + prims + "]}],\"materials\":[" + materials +
                   "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}],\"bufferViews\":[" + views + "],\"accessors\":[" + accessors + "]}";
  while (js.size() % 4) js.push_back(' ');
  const auto u32 = [](std::ofstream& f, std::size_t v) { const auto x = static_cast<std::uint32_t>(v); f.write(reinterpret_cast<const char*>(&x), 4); };
  std::ofstream f(path, std::ios::binary);
  u32(f, 0x46546C67); u32(f, 2); u32(f, 12 + 8 + js.size() + 8 + bin.size());
  u32(f, js.size()); u32(f, 0x4E4F534A); f.write(js.data(), static_cast<std::streamsize>(js.size()));
  u32(f, bin.size()); u32(f, 0x004E4942); f.write(bin.data(), static_cast<std::streamsize>(bin.size()));
}

// One view: NASA's trajectory as beads, the simulated one as a line, the bodies around them.
struct view {
  part nasa{"NASA trajectory", {0.82f, 0.80f, 0.74f}, {}, {}, {}};
  part sim{"simulated", {0.16f, 0.47f, 0.84f}, {}, {}, {}};
  part orbits{"orbits", {0.50f, 0.50f, 0.48f}, {}, {}, {}};
  part moons{"moons", {0.92f, 0.41f, 0.20f}, {}, {}, {}};
  part body{"planet", {0.86f, 0.74f, 0.52f}, {}, {}, {}};
  part sun{"Sun", {1.0f, 0.82f, 0.25f}, {}, {}, {}};
  // Writes <dir>/<name>.glb, scaled to fit 300 units either side of the middle (the
  // size GameApi's default camera sees), and <dir>/<name>.txt, a GameApi script that
  // shows it with NASA's model of the spacecraft at `probe`. Returns units per `unit`.
  double save(const std::string& dir, const std::string& name, vec3 probe) {
    float far = 1e-9f;
    for (float v : nasa.pos) far = std::max(far, std::fabs(v));
    const float k = 300.0f / far;
    for (part* p : {&nasa, &sim, &orbits, &moons, &body, &sun}) for (float& v : p->pos) v *= k;
    write_glb(dir + "/" + name + ".glb", {nasa, sim, orbits, moons, body, sun});
    std::ofstream f(dir + "/" + name + ".txt");
    char b[200];
    std::snprintf(b, sizeof b, "MN I7=ev.move_api.trans2(I6,%.2f,%.2f,%.2f);\n", k * probe.x, k * probe.z, -k * probe.y);
    f << "TF I1=ev.mainloop_api.gltf_loadKK2(" << name << ".glb);\n"
         "ML I2=ev.mainloop_api.gltf_mesh_all(ev,I1,1.0,1.0,1.0,0,400.0,-400.0,300.0,0.0,ff000000,false,true,false);\n"
         "TF I3=ev.mainloop_api.gltf_loadKK2(voyager-probe-a.glb);\n"
         "ML I4=ev.mainloop_api.gltf_mesh_all(ev,I3,1.0,1.0,1.0,0,400.0,-400.0,300.0,0.0,ff000000,false,true,false);\n"
         "MN I5=ev.move_api.mn_empty();\n"
         "MN I6=ev.move_api.scale2(I5,100.0,100.0,100.0);\n"
      << b
      << "ML I8=ev.move_api.move_ml(ev,I4,I7,1,10.0);\n"
         "ML I9=ev.mainloop_api.or_elem_ml(ev,I2,I8);\n"
         "ML I10=ev.mainloop_api.touch_rotate(ev,I9,true,true,0.01,0.01);\n"
         "RUN I11=ev.blocker_api.game_window2(ev,I10,false,false,0.0,100000.0);\n";
    return k;
  }
};

void add_beads(part& m, const std::vector<vec3>& p, double r, std::size_t want) {
  const std::size_t k = std::max<std::size_t>(1, p.size() / want);
  for (std::size_t i = 0; i < p.size(); i += k) add_ball(m, p[i], r, 8, 5);
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
    const double size = whole ? 3.0 : 1.0;  // the whole route is five times as far out
    const double t_end = whole ? 1e300 : flights[7].nasa.back().t;
    std::vector<vec3> nasa, sim;
    for (std::size_t i = 0; i < kLegs.size(); ++i) {
      if (kLegs[i].centre) continue;
      for (std::size_t j = 0; j < flights[i].nasa.size() && flights[i].nasa[j].t <= t_end; ++j) {
        nasa.push_back((1.0 / kAu) * ecl.turn(flights[i].nasa[j].s.pos));
        sim.push_back((1.0 / kAu) * ecl.turn(flights[i].sim[j]));
      }
    }
    add_beads(v.nasa, nasa, 0.10 * size, 260);
    add_tube(v.sim, sim, 0.04 * size);
    for (std::size_t p = 0; p < 4; ++p) {
      const std::vector<orbit_sample>& path = late.paths[1 + p];
      add_tube(v.orbits, scaled(path, -1e300, whole ? 1e300 : t_end + 3e7, kAu, ecl), 0.015 * size);
      const auto at = state_at(std::span<const orbit_sample>(path), flights[flyby_leg[p]].nasa.front().t);
      if (at) add_ball(v.body, (1.0 / kAu) * ecl.turn(at->pos), 0.35 * size);  // the planet where Voyager met it
    }
    add_ball(v.sun, {0, 0, 0}, 0.5 * size);
    const double k = v.save(out, whole ? "route-whole" : "route-planets", sim.back());
    if (!whole) std::printf("\nviews written to %s/\n", out.c_str());
    std::printf("%-17s 1 unit = %.3f AU\n", whole ? "route-whole.glb" : "route-planets.glb", 1.0 / k);
  }
  // Each flyby in the planet's own frame, in thousands of km, the planet to scale.
  const char* files[4] = {"jupiter", "saturn", "uranus", "neptune"};
  for (std::size_t n = 0; n < 4; ++n) {
    const leg& l = kLegs[flyby_leg[n]];
    const flown& f = flights[flyby_leg[n]];
    const double unit = 1e6;
    const double reach = std::max(12.0 * f.nearest, 2.5e9);  // the part of the leg within sight of the planet
    view v;
    std::vector<vec3> nasa, sim;
    double tn = f.nasa.front().t;
    for (std::size_t j = 0; j < f.nasa.size(); ++j) {
      if (f.nasa[j].date == f.nearest_date) tn = f.nasa[j].t;
      if (norm(f.nasa[j].s.pos) > reach) continue;
      nasa.push_back((1.0 / unit) * f.nasa[j].s.pos);
      sim.push_back((1.0 / unit) * f.sim[j]);
    }
    const double size = reach / unit / 400.0;
    add_beads(v.nasa, nasa, 1.6 * size, 220);
    add_tube(v.sim, sim, 0.7 * size);
    for (std::size_t m = 1; m < l.masses.size(); ++m) {
      if (norm(f.paths[m].front().pos) > reach) continue;  // the Sun, another planet
      add_tube(v.moons, scaled(f.paths[m], f.nasa.front().t, f.nasa.back().t, unit, frame{}), 0.3 * size);
      const auto at = state_at(std::span<const orbit_sample>(f.paths[m]), tn);  // the moon when Voyager was nearest
      if (at) add_ball(v.moons, (1.0 / unit) * at->pos, 2.5 * size);
    }
    add_ball(v.body, {0, 0, 0}, l.centre->radius * 1e3 / unit, 32, 20);
    vec3 probe{};  // the spacecraft where it was nearest to the planet
    for (std::size_t j = 0; j < f.nasa.size(); ++j) if (f.nasa[j].t == tn) probe = (1.0 / unit) * f.sim[j];
    const double k = v.save(out, files[n], probe);
    std::printf("%-17s 1 unit = %.0f km\n", (std::string(files[n]) + ".glb").c_str(), 1000.0 / k);
  }
  std::printf("each with a GameApi script of the same name (.txt)\n");
  return 0;
}

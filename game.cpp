// Voyager 2's planetary encounters, to fly along with: the spacecraft in the middle,
// the planet, its moons, the Sun and the stars around it as seen from the
// spacecraft, speeded up. Drawn with GameApi (https://github.com/terop2/GameApi).
//
//   drag with the mouse   look around the spacecraft
//   wheel                 nearer, farther
//   1 2 3 4               Jupiter, Saturn, Uranus, Neptune
//   n, p                  next, previous flyby in the scene (the planet and each moon), facing it
//   f                     face the planet
//   + -                   faster, slower        space   pause       r   start of the scene
//   Esc                   quit
//
// What is true to life: the direction and the apparent size of every body at every
// moment (from out/<planet>.scene, written by the simulation), the stars (NASA's
// star map, each star where it is in the sky), the surface maps in visible light,
// the planet's turning, which side of each body is in sunlight, the spacecraft's
// shape. What is not: time (speeded up); the Sun, drawn at least a fifth of a degree
// wide so that it can be seen; shadows (of the planet on its rings and moons); the
// way the spacecraft is turned.
//
// GameApi scales every model it loads to 400 units across, centred. Each model here
// is made to be measured exactly (the first triangle of every file spans its box),
// so that a ball comes out with radius 200 and is then sized and placed by a matrix
// of its own, set anew for every frame. GameApi's camera is a long lens: 17 degrees
// from top to bottom.
//
// Build and run: ./play.sh [jupiter|saturn|uranus|neptune] [hours from the nearest approach, default -1] [speed, default 600]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <GL/gl.h>

#include "GameApi.hh"
#include "Tasks.hh"
#include "orbit.hpp"

IMPORT extern pthread_t g_main_thread_id;

using glass::orbit::orbit_sample;
using glass::orbit::vec3;

namespace {

constexpr double kPi = 3.14159265358979323846;
// GameApi's camera sees from 10 to 1600 units. The bodies are drawn in a first pass
// with the eye at the spacecraft, so that looking around does not shift them, each
// at a distance that keeps their order (nearer in space, nearer here) and with the
// size that gives its true apparent size. The spacecraft is drawn in a second pass.
constexpr double kReach = 1400.0;     // where the farthest bodies are drawn, units
constexpr double kNear = 1.0e5;       // km: bodies nearer than this are drawn nearer in proportion
constexpr float kSky = 1500.0f;       // radius of the sky
constexpr float kBall = 200.0f;       // radius GameApi gives the ball files: it makes every model 400 units across

struct body {
  std::string name;
  double radius = 0.0;                // km
  std::vector<orbit_sample> path;     // km from the spacecraft, planet frame (z along the pole), t in s from the start
  GameApi::ML ml{};
  GameApi::MN mn{}, night{};
  double nearest = 0.0, nearest_t = 0.0;
};

struct scene {
  std::string planet, start_date;
  double day = 0.0;                   // hours for one turn of the planet
  double step = 0.0, length = 0.0;    // s
  std::vector<body> bodies;           // the planet first, the Sun second, then the moons
  std::vector<std::size_t> flybys;    // bodies in the order Voyager passed them
  GameApi::ML ml{};                   // the bodies and the sky
  std::vector<GameApi::MN> rings;     // Saturn's
  GameApi::M sunward{};               // turns a ball's pole toward the Sun
  bool built = false;
};

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool read_scene(const std::string& file, scene& s) {
  std::ifstream f(file);
  std::string word, line;
  std::size_t n = 0;
  if (!(f >> word >> s.planet >> word >> s.day)) return false;  // "planet <name> <radius> <day>"
  f >> word >> n >> s.step;
  std::getline(f, s.start_date);
  s.length = static_cast<double>(n - 1) * s.step;
  while (f >> word) {
    body b;
    f >> b.name >> b.radius;
    b.nearest = 1e300;
    for (std::size_t i = 0; i < n; ++i) {
      vec3 p;
      f >> p.x >> p.y >> p.z;
      b.path.push_back({static_cast<double>(i) * s.step, p});
      const double d = glass::orbit::norm(p);
      if (d < b.nearest) { b.nearest = d; b.nearest_t = static_cast<double>(i) * s.step; }
    }
    s.bodies.push_back(std::move(b));
  }
  for (std::size_t i = 0; i < s.bodies.size(); ++i) if (s.bodies[i].name != "Sun") s.flybys.push_back(i);
  std::sort(s.flybys.begin(), s.flybys.end(), [&](std::size_t a, std::size_t b) { return s.bodies[a].nearest_t < s.bodies[b].nearest_t; });
  return !s.bodies.empty();
}

// "1989-Aug-22 00:00" plus seconds, as text
std::string date_at(const scene& s, double t) {
  int y = 0, d = 0, h = 0, mi = 0;
  char mon[8] = "";
  std::sscanf(s.start_date.c_str(), " %d-%3s-%d %d:%d", &y, mon, &d, &h, &mi);
  long minutes = static_cast<long>(t / 60.0) + h * 60 + mi;
  d += static_cast<int>(minutes / 1440);
  minutes %= 1440;
  char b[64];
  std::snprintf(b, sizeof b, "%d %s %d %02ld:%02ld", d, mon, y, minutes / 60, minutes % 60);
  return b;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string home = argc > 2 ? argv[2] : ".";
  const std::string names[4] = {"jupiter", "saturn", "uranus", "neptune"};
  int current = 3;
  for (int i = 0; i < 4; ++i) if (argc > 1 && names[i] == argv[1]) current = i;
  const int w = 1280, h = 800;

  scene scenes[4];
  for (int i = 0; i < 4; ++i)
    if (!read_scene(home + "/out/" + names[i] + ".scene", scenes[i])) {
      std::fprintf(stderr, "no scene %s/out/%s.scene: run ./run.sh first\n", home.c_str(), names[i].c_str());
      return 1;
    }

  g_main_thread_id = pthread_self();  // as GameApi's own runner does before anything else
  tasks_init();
  GameApi::Env env;
  GameApi::EveryApi ev(env);
  ev.mainloop_api.init_window(w, h, "Voyager 2");
  ev.mainloop_api.set_screen_size(w, h);
  ev.shader_api.load_default();
  const GameApi::SH sh = ev.shader_api.colour_shader();
  const GameApi::SH sh2 = ev.shader_api.texture_shader();
  const GameApi::SH sh3 = ev.shader_api.texture_shader();
  ev.mainloop_api.init_3d(sh);
  ev.mainloop_api.init_3d(sh2);
  ev.mainloop_api.init_3d(sh3);
  ev.mainloop_api.alpha(true);
  ev.mainloop_api.depth_test(true);

  // One model, with a matrix of its own. `light` is the direction of the Sun, for the
  // models that GameApi lights (the spacecraft). With a surface map (a file in
  // textures/), the map is drawn as it is, by GameApi's plain texture material;
  // sunlight on a body is a second ball over it, its pole turned to the Sun, that
  // darkens toward the night side (out/ball-shade.glb with textures/shade.png).
  const auto model = [&](const std::string& file, vec3 light, const std::string& map, bool sky, GameApi::MN& mn) {
    env.async_load_url("file://" + file, "");  // GameApi draws only files it has fetched beforehand
    const GameApi::TF tf = ev.mainloop_api.gltf_loadKK2("file://" + file);
    const GameApi::P p = ev.mainloop_api.gltf_mesh_all_p(ev, tf);
    const float lx = static_cast<float>(400.0 * light.x), ly = static_cast<float>(400.0 * light.y), lz = static_cast<float>(400.0 * light.z);
    GameApi::MT mt{};
    if (map.empty()) {
      mt = ev.materials_api.gltf_material(ev, tf, 0, 1.0f, 1.0f, 1.0f, lx, ly, lz, sky);
    } else {
      mt = ev.materials_api.texture(ev, ev.bitmap_api.loadbitmap(map), 1.0f);
    }
    mn = ev.move_api.matrix(ev.move_api.mn_empty(), ev.matrix_api.identity());
    return ev.move_api.move_ml(ev, ev.materials_api.bind(p, mt), mn, 1, 10.0f);
  };
  const auto map_of = [&](const std::string& name) {
    for (const char* kind : {".jpg", ".png"}) {
      const std::string f = home + "/textures/" + lower(name) + kind;
      if (std::ifstream(f).good()) return f;
    }
    return std::string();
  };
  // our axes (z up) to GameApi's (y up)
  const auto to_view = [](vec3 p) { return vec3{p.x, p.z, -p.y}; };

  GameApi::MN sky_mn{};
  const auto build = [&](scene& s) {
    std::vector<GameApi::ML> all;
    const body& sun = s.bodies[1];
    const vec3 to_sun = to_view((1.0 / glass::orbit::norm(sun.path[sun.path.size() / 2].pos)) * sun.path[sun.path.size() / 2].pos);
    for (body& b : s.bodies) {
      b.ml = model(home + "/out/ball-" + lower(b.name) + ".glb", to_sun, map_of(b.name), b.name == "Sun", b.mn);
      all.push_back(b.ml);
      if (b.name != "Sun") all.push_back(model(home + "/out/ball-shade.glb", to_sun, map_of("shade"), true, b.night));
    }
    all.push_back(model(home + "/out/sky-" + lower(s.planet) + ".glb", to_sun, map_of("stars"), true, sky_mn));
    if (s.planet == "Saturn") {
      s.rings.push_back({});
      all.push_back(model(home + "/out/ring.glb", to_sun, map_of("ring"), true, s.rings.back()));
    }
    // the turn that brings the Sun straight ahead, undone, after one that lays the pole straight ahead
    const double a = std::atan2(-to_sun.x, -to_sun.z);
    const double p = std::atan2(to_sun.y, -to_sun.x * std::sin(a) - to_sun.z * std::cos(a));
    const float quarter = static_cast<float>(-kPi / 2.0);
    s.sunward = ev.matrix_api.mult(ev.matrix_api.xrot(quarter), ev.matrix_api.mult(ev.matrix_api.xrot(static_cast<float>(p)), ev.matrix_api.yrot(static_cast<float>(a))));
    s.ml = ev.mainloop_api.array_ml(ev, all);
    s.built = true;
    std::printf("\n%s: Voyager 2 passed\n", s.planet.c_str());
    for (std::size_t i : s.flybys)
      std::printf("  %-9s %9.0f km from its centre, %s\n", s.bodies[i].name.c_str(), s.bodies[i].nearest, date_at(s, s.bodies[i].nearest_t).c_str());
  };
  // the spacecraft: NASA's model, one file per coloured part
  std::vector<GameApi::MN> probe_mn;
  std::vector<GameApi::ML> parts;
  for (int i = 0; i < 64; ++i) {
    char part[64];
    std::snprintf(part, sizeof part, "/models/probe/part-%02d.glb", i);
    if (!std::ifstream(home + part).good()) break;
    probe_mn.push_back({});
    parts.push_back(model(home + part, {0.5, 0.6, 0.6}, "", false, probe_mn.back()));
  }
  const GameApi::ML probe = ev.mainloop_api.array_ml(ev, parts);

  // optional: where to start, in hours from the nearest approach to the planet, and the speed
  const double start_hours = argc > 3 ? std::atof(argv[3]) : -1.0;
  double t = 0.0, speed = argc > 4 ? std::atof(argv[4]) : 600.0;
  bool paused = false;
  float yaw = 0.6f, pitch = 0.35f, eye = 500.0f;
  bool drag = false;
  float last_x = 0.0f, last_y = 0.0f;
  // turn the view so that a body is straight ahead
  const auto face = [&](const body& b) {
    const auto at = glass::orbit::state_at(std::span<const orbit_sample>(b.path), t, 8);
    if (!at) return;
    const vec3 d = to_view((1.0 / glass::orbit::norm(at->pos)) * at->pos);
    const double a = std::atan2(-d.x, -d.z);                      // about y: brings the body into the y-z plane, ahead
    const double z1 = -d.x * std::sin(a) - d.z * std::cos(a);     // how far ahead it then is
    yaw = static_cast<float>(-a);  // GameApi turns the other way about y
    pitch = static_cast<float>(-std::atan2(d.y, z1));
  };
  const auto enter = [&](int i) {
    current = i;
    scene& s = scenes[i];
    if (!s.built) build(s);
    t = std::max(0.0, s.bodies[0].nearest_t + start_hours * 3600.0);
    face(s.bodies[0]);
  };
  enter(current);

  auto before = std::chrono::steady_clock::now();
  double shown = -1.0;
  for (bool run = true; run;) {
    scene& s = scenes[current];
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - before).count();
    before = now;
    if (!paused) t += speed * dt;
    if (t > s.length) t = 0.0;
    if (t < 0.0) t = 0.0;

    GameApi::MainLoopApi::Event e;
    while ((e = ev.mainloop_api.get_event()).last) {
      if (e.type == 0x300) {
        if (e.ch == 27) run = false;
        if (e.ch >= '1' && e.ch <= '4') enter(e.ch - '1');
        if (e.ch == '+' || e.ch == '=') speed *= 2.0;
        if (e.ch == '-') speed /= 2.0;
        if (e.ch == ' ') paused = !paused;
        if (e.ch == 'r') t = 0.0;
        if (e.ch == 'n' || e.ch == 'p') {  // the next or the previous flyby, from an hour before it
          const scene& c = scenes[current];
          double to = e.ch == 'n' ? 1e300 : -1e300;
          for (std::size_t i : c.flybys) {
            const double at = c.bodies[i].nearest_t - 3600.0;
            if (e.ch == 'n' && at > t + 1.0 && at < to) to = at;
            if (e.ch == 'p' && at < t - speed * 2.0 && at > to) to = at;
          }
          if (std::fabs(to) < 1e299) {
            t = std::max(0.0, to);
            for (std::size_t i : c.flybys) if (std::fabs(c.bodies[i].nearest_t - 3600.0 - to) < 1.0) face(c.bodies[i]);
          }
        }
        if (e.ch == 'f') face(scenes[current].bodies[0]);
        shown = -1.0;
      }
      const float x = ev.point_api.pt_x(e.cursor_pos), y = ev.point_api.pt_y(e.cursor_pos);
      if (e.type == 0x401) { drag = true; last_x = x; last_y = y; }
      if (e.type == 0x402) drag = false;
      if (e.type == 0x400 && drag) {
        yaw += (x - last_x) * 0.008f;
        pitch = std::clamp(pitch + (y - last_y) * 0.008f, -1.5f, 1.5f);
        last_x = x;
        last_y = y;
      }
      if (e.type == 0x403) eye = std::clamp(eye * (e.mouse_wheel_y > 0 ? 0.87f : 1.15f), 150.0f, 5000.0f);
    }

    // every body where and as large as it looks from the spacecraft
    scene& c = scenes[current];
    for (body& b : c.bodies) {
      const auto at = glass::orbit::state_at(std::span<const orbit_sample>(b.path), t, 8);
      if (!at) continue;
      const double dist = glass::orbit::norm(at->pos);
      const vec3 dir = to_view((1.0 / dist) * at->pos);
      const double far = std::max(kReach * dist / (dist + kNear), 12.0 + b.radius / dist * 20.0);
      double r = far * b.radius / dist;
      if (b.name == "Sun") r = std::max(r, far * 0.0017);  // at least a fifth of a degree across
      const auto k = static_cast<float>(r) / kBall;
      GameApi::M m = ev.matrix_api.scale(k, k, k);
      if (&b == &c.bodies[0] && c.day != 0.0) m = ev.matrix_api.mult(ev.matrix_api.yrot(static_cast<float>(2.0 * kPi * t / (c.day * 3600.0))), m);
      m = ev.matrix_api.mult(m, ev.matrix_api.trans(static_cast<float>(far * dir.x), static_cast<float>(far * dir.y), static_cast<float>(far * dir.z)));
      ev.move_api.set_matrix(b.mn, m);
      if (&b == &c.bodies[0])  // the rings: measured by GameApi as a box 2.33 planet radii each way
        for (const GameApi::MN& ring : c.rings)
          ev.move_api.set_matrix(ring, ev.matrix_api.mult(ev.matrix_api.scale(k * 2.33f, k * 2.33f, k * 2.33f),
                                                          ev.matrix_api.trans(static_cast<float>(far * dir.x), static_cast<float>(far * dir.y), static_cast<float>(far * dir.z))));
      if (b.name != "Sun")  // sunlight does not turn with the planet; GameApi makes its ball the body's size, so it is enlarged a little here
        ev.move_api.set_matrix(b.night, ev.matrix_api.mult(ev.matrix_api.mult(c.sunward, ev.matrix_api.scale(k * 1.01f, k * 1.01f, k * 1.01f)), ev.matrix_api.trans(static_cast<float>(far * dir.x), static_cast<float>(far * dir.y),
                                                                                                         static_cast<float>(far * dir.z))));
    }
    ev.move_api.set_matrix(sky_mn, ev.matrix_api.scale(kSky / kBall, kSky / kBall, kSky / kBall));
    for (const GameApi::MN& mn : probe_mn) ev.move_api.set_matrix(mn, ev.matrix_api.scale(0.5f, 0.5f, 0.5f));

    const GameApi::M look = ev.matrix_api.mult(ev.matrix_api.yrot(yaw), ev.matrix_api.xrot(pitch));
    ev.mainloop_api.clear_3d(0xff000000);
    ev.mainloop_api.execute_ml(ev, c.ml, sh, sh2, sh2, sh3, look, ev.matrix_api.identity(), look, w, h);  // the eye at the spacecraft
    glClear(GL_DEPTH_BUFFER_BIT);
    ev.mainloop_api.execute_ml(ev, probe, sh, sh2, sh2, sh3, look, ev.matrix_api.trans(0.0f, 0.0f, -eye), look, w, h);
    ev.mainloop_api.swapbuffers();

    if (std::fabs(t - shown) > speed * 0.5 || shown < 0.0) {
      shown = t;
      const body& p = c.bodies[0];
      const auto at = glass::orbit::state_at(std::span<const orbit_sample>(p.path), t, 8);
      std::printf("\r%-8s %s   %10.0f km from the centre   time x %-6.0f %s   ", c.planet.c_str(), date_at(c, t).c_str(),
                  at ? glass::orbit::norm(at->pos) : 0.0, speed, paused ? "(paused)" : "        ");
      std::fflush(stdout);
    }
  }
  std::printf("\n");
  return 0;
}

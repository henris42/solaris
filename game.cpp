// Voyager 2's planetary encounters, to fly along with: the spacecraft in the middle,
// the planet, its moons, the Sun and the stars around it as seen from the
// spacecraft, speeded up. Drawn with GameApi (https://github.com/terop2/GameApi).
//
//   drag with the mouse   look around the spacecraft
//   wheel                 nearer, farther
//   1 2 3 4               Jupiter, Saturn, Uranus, Neptune
//   n, p                  next, previous flyby in the scene (the planet and each moon), facing it
//   f                     face the planet       v       the spacecraft in view or not
//   z                     by eye, through Voyager's wide-angle camera (3.2 degrees), through its narrow-angle camera (0.42 degrees)
//   + -                   faster, slower        space   pause       r   start of the scene
//   t                     the tour on or off
//
// The tour (on at the start): each system in three minutes. Every flyby, of the
// planet and of each moon, is shown at a slow rate while the body is near, and the
// way between them is run through fast; the view keeps on each body as its turn
// comes, until the mouse takes it. With the tour off, time runs at one rate and the
// view stays where it is put.
//   Esc                   quit
//
// What is true to life: the direction and the apparent size of every body at every
// moment (from out/<planet>.scene, written by the simulation), the stars (NASA's
// star map, each star where it is in the sky), the surface maps in visible light,
// the planet's turning, which side of each body is in sunlight, the spacecraft's
// shape with its dish toward the Earth, the fields of its two cameras. What is not:
// time (speeded up); the Sun, drawn at least a fifth of a degree wide so that it can
// be seen; shadows (of the planet on its rings and moons); how the spacecraft is
// rolled about its dish; the stars through a camera (they stay as the eye sees them).
//
// GameApi scales every model it loads to 400 units across, centred. Each model here
// is made to be measured exactly (the first triangle of every file spans its box),
// so that a ball comes out with radius 200 and is then sized and placed by a matrix
// of its own, set anew for every frame. GameApi's camera is a long lens: 17 degrees
// from top to bottom.
//
// Build and run: ./play.sh [jupiter|saturn|uranus|neptune]                 the tour
//                ./play.sh <planet> <hours from the nearest approach> [speed, default 600]   without it
//                ./play.sh <planet> <moon> [speed]                                           at that moon's nearest approach
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
  struct stretch { double t0, t1, rate; std::size_t target; bool flyby; };
  std::vector<stretch> tour;          // the three-minute tour: flybys at a slow rate, the way between them fast
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
  for (std::size_t i = 0; i < s.bodies.size(); ++i) if (s.bodies[i].name != "Sun" && s.bodies[i].name != "Earth") s.flybys.push_back(i);
  std::sort(s.flybys.begin(), s.flybys.end(), [&](std::size_t a, std::size_t b) { return s.bodies[a].nearest_t < s.bodies[b].nearest_t; });

  // The tour: three minutes for the system. A flyby is the time a body is within
  // 2.5 times its nearest distance (three hours either side at most); neighbours
  // share the time between their nearest approaches. Every flyby gets the same
  // share of 130 seconds, and the way between them 50 seconds in all.
  struct window { double a, b; std::size_t body; };
  std::vector<window> win;
  for (std::size_t i : s.flybys) {
    const body& b = s.bodies[i];
    const auto dist = [&](double t) { return glass::orbit::norm(b.path[static_cast<std::size_t>(std::clamp(t / s.step, 0.0, static_cast<double>(n - 1)))].pos); };
    double from = b.nearest_t, to = b.nearest_t;
    while (from > 0.0 && b.nearest_t - from < 10800.0 && dist(from - s.step) < 2.5 * b.nearest) from -= s.step;
    while (to < s.length && to - b.nearest_t < 10800.0 && dist(to + s.step) < 2.5 * b.nearest) to += s.step;
    win.push_back({std::min(from, b.nearest_t - s.step), std::max(to, b.nearest_t + s.step), i});
  }
  for (std::size_t k = 0; k + 1 < win.size(); ++k)
    if (win[k].b > win[k + 1].a) win[k].b = win[k + 1].a = 0.5 * (s.bodies[win[k].body].nearest_t + s.bodies[win[k + 1].body].nearest_t);
  double between = 0.0;
  for (std::size_t k = 0; k + 1 < win.size(); ++k) between += win[k + 1].a - win[k].b;
  for (std::size_t k = 0; k < win.size(); ++k) {
    if (k > 0 && win[k].a > win[k - 1].b) s.tour.push_back({win[k - 1].b, win[k].a, between / 50.0, win[k].body, false});
    s.tour.push_back({win[k].a, win[k].b, (win[k].b - win[k].a) / (130.0 / static_cast<double>(win.size())), win[k].body, true});
  }
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
      if (b.name == "Earth") continue;  // not drawn: from out there it is a point beside the Sun
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
    std::printf("\n%s in three minutes: what Voyager 2 passed\n", s.planet.c_str());
    for (const scene::stretch& leg : s.tour)
      if (leg.flyby)
        std::printf("  %-9s %9.0f km from its centre, %s   shown for %2.0f s at %5.0f times real speed\n", s.bodies[leg.target].name.c_str(),
                    s.bodies[leg.target].nearest, date_at(s, s.bodies[leg.target].nearest_t).c_str(), (leg.t1 - leg.t0) / leg.rate, leg.rate);
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
  // without them: the three-minute tour
  const double start_hours = argc > 3 ? std::atof(argv[3]) : -1.0;
  double t = 0.0, speed = argc > 4 ? std::atof(argv[4]) : 600.0;
  bool tour = argc <= 3, craft = true;
  double pace = 1.0;        // the tour's rate times this
  std::size_t stretch = static_cast<std::size_t>(-1);
  bool follow = true;       // the view keeps on the body of the tour's stretch, until the mouse takes it
  double zoom = 1.0;        // 1: the eye beside the spacecraft; 5.4 and 40: through its wide-angle and narrow-angle cameras
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
    yaw = static_cast<float>(-a) + (zoom > 1.0 ? 0.0f : 0.1f);  // GameApi turns the other way about y; a little aside when the spacecraft is in view
    pitch = static_cast<float>(-std::atan2(d.y, z1));
  };
  const auto enter = [&](int i) {
    current = i;
    scene& s = scenes[i];
    if (!s.built) build(s);
    t = tour ? s.tour.front().t0 : std::max(0.0, s.bodies[0].nearest_t + start_hours * 3600.0);
    stretch = static_cast<std::size_t>(-1);
    std::size_t look_at = tour ? s.tour.front().target : 0;
    if (!tour && argc > 3)  // a moon's name in place of the hours: at its nearest approach
      for (std::size_t k = 0; k < s.bodies.size(); ++k)
        if (lower(s.bodies[k].name) == lower(argv[3])) { look_at = k; t = s.bodies[k].nearest_t; }
    face(s.bodies[look_at]);
  };
  enter(current);

  auto before = std::chrono::steady_clock::now();
  double shown = -1.0;
  for (bool run = true; run;) {
    scene& s = scenes[current];
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - before).count();
    before = now;
    double rate = speed;
    if (tour) {  // the stretch of the tour we are in sets the rate and what to look at
      if (t < s.tour.front().t0 || t >= s.tour.back().t1) t = s.tour.front().t0;
      std::size_t k = 0;
      while (k + 1 < s.tour.size() && t >= s.tour[k].t1) ++k;
      rate = s.tour[k].rate * pace;
      if (k != stretch) {
        stretch = k;
        follow = true;
        // the camera Voyager would use: the one whose field the body about fills at its nearest
        const double across = 2.0 * std::asin(std::min(1.0, s.bodies[s.tour[k].target].radius / s.bodies[s.tour[k].target].nearest)) * 180.0 / kPi;
        zoom = across < 0.5 ? 40.0 : across < 3.5 ? 5.4 : 1.0;
        if (s.tour[k].flyby)
          std::printf("\n  %s: nearest %0.f km, %s, %s\n", s.bodies[s.tour[k].target].name.c_str(), s.bodies[s.tour[k].target].nearest,
                      date_at(s, s.bodies[s.tour[k].target].nearest_t).c_str(),
                      zoom > 10.0 ? "through the narrow-angle camera" : zoom > 1.0 ? "through the wide-angle camera" : "by eye, beside the spacecraft");
      }
    }
    if (!paused) t += rate * dt;
    if (tour && follow && stretch < s.tour.size()) face(s.bodies[s.tour[stretch].target]);
    if (t > s.length) t = 0.0;
    if (t < 0.0) t = 0.0;

    GameApi::MainLoopApi::Event e;
    while ((e = ev.mainloop_api.get_event()).last) {
      if (e.type == 0x300) {
        if (e.ch == 27) run = false;
        if (e.ch >= '1' && e.ch <= '4') enter(e.ch - '1');
        if (e.ch == '+' || e.ch == '=') (tour ? pace : speed) *= 2.0;
        if (e.ch == '-') (tour ? pace : speed) /= 2.0;
        if (e.ch == 't') { tour = !tour; stretch = static_cast<std::size_t>(-1); }
        if (e.ch == 'v') craft = !craft;
        if (e.ch == 'z') zoom = zoom < 2.0 ? 5.4 : zoom < 10.0 ? 40.0 : 1.0;
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
      if (e.type == 0x401) { drag = true; follow = false; last_x = x; last_y = y; }
      if (e.type == 0x402) drag = false;
      if (e.type == 0x400 && drag) {
        yaw += (x - last_x) * 0.008f;
        pitch = std::clamp(pitch + (y - last_y) * 0.008f, -1.5f, 1.5f);
        last_x = x;
        last_y = y;
      }
      if (e.type == 0x403) eye = std::clamp(eye * (e.mouse_wheel_y > 0 ? 0.87f : 1.15f), 150.0f, 5000.0f);
    }

    // every body where and as large as it looks from the spacecraft; through a camera,
    // everything is that many times larger and farther from the middle of the view
    scene& c = scenes[current];
    const vec3 aim{-std::sin(-yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(-yaw) * std::cos(pitch)};  // where the view points
    vec3 to_earth{0, 1, 0};
    for (body& b : c.bodies) {
      const auto at = glass::orbit::state_at(std::span<const orbit_sample>(b.path), t, 8);
      if (!at) continue;
      const double dist = glass::orbit::norm(at->pos);
      vec3 dir = to_view((1.0 / dist) * at->pos);
      if (b.name == "Earth") { to_earth = dir; continue; }
      const double far = std::max(kReach * dist / (dist + kNear), 12.0 + b.radius / dist * 20.0);
      double r = far * b.radius / dist;
      if (b.name == "Sun") r = std::max(r, far * 0.0017);  // at least a fifth of a degree across
      if (zoom > 1.0) {
        const double along = glass::orbit::dot(dir, aim);
        const vec3 off = dir - along * aim;
        const double angle = zoom * std::atan2(glass::orbit::norm(off), along);
        const double size = zoom * std::asin(std::min(1.0, b.radius / dist));
        if (angle > 1.2 || glass::orbit::norm(off) < 1e-12) {
          if (angle > 1.2) r = 0.0;  // out of the camera's field
        } else {
          dir = std::cos(angle) * aim + (std::sin(angle) / glass::orbit::norm(off)) * off;
        }
        if (r > 0.0) r = far * std::sin(std::min(size, 1.3));
      }
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
    // the spacecraft with its dish (the model's y axis) toward the Earth
    const double ea = std::atan2(-to_earth.x, -to_earth.z);
    const double ep = std::atan2(to_earth.y, -to_earth.x * std::sin(ea) - to_earth.z * std::cos(ea));
    const GameApi::M earthward = ev.matrix_api.mult(ev.matrix_api.xrot(static_cast<float>(-kPi / 2.0)),
                                                    ev.matrix_api.mult(ev.matrix_api.xrot(static_cast<float>(ep)), ev.matrix_api.yrot(static_cast<float>(ea))));
    for (const GameApi::MN& mn : probe_mn) ev.move_api.set_matrix(mn, ev.matrix_api.mult(earthward, ev.matrix_api.scale(0.5f, 0.5f, 0.5f)));

    const GameApi::M look = ev.matrix_api.mult(ev.matrix_api.yrot(yaw), ev.matrix_api.xrot(pitch));
    ev.mainloop_api.clear_3d(0xff000000);
    ev.mainloop_api.execute_ml(ev, c.ml, sh, sh2, sh2, sh3, look, ev.matrix_api.identity(), look, w, h);  // the eye at the spacecraft
    glClear(GL_DEPTH_BUFFER_BIT);
    if (craft && zoom <= 1.0) ev.mainloop_api.execute_ml(ev, probe, sh, sh2, sh2, sh3, look, ev.matrix_api.trans(0.0f, 0.0f, -eye), look, w, h);
    ev.mainloop_api.swapbuffers();

    if (std::fabs(t - shown) > rate * 0.5 || shown < 0.0) {
      shown = t;
      const body& p = c.bodies[0];
      const auto at = glass::orbit::state_at(std::span<const orbit_sample>(p.path), t, 8);
      std::printf("\r%-8s %s   %10.0f km from the centre   time x %-6.0f %s   ", c.planet.c_str(), date_at(c, t).c_str(),
                  at ? glass::orbit::norm(at->pos) : 0.0, rate, paused ? "(paused)" : "        ");
      std::fflush(stdout);
    }
  }
  std::printf("\n");
  return 0;
}

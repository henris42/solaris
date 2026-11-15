// Voyager 2's planetary encounters, to fly along with: the spacecraft in the middle,
// the planet, its moons, the Sun and the stars around it as seen from the
// spacecraft, speeded up. Drawn with GameApi (https://github.com/terop2/GameApi).
//
//   drag with the mouse   turn the view about the spacecraft (by eye only; a camera stays on its body)
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
// moment (from out/<planet>.scene, written by the simulation), the stars (the
// Hipparcos catalogue: each star a dot at its measured place for the year of the
// flyby, as bright and of the colour measured; to magnitude 6.5 for the eye, deeper
// through the cameras), the surface maps in visible light,
// the planet's turning, which side of each body is in sunlight, the spacecraft's
// shape with its dish toward the Earth, the fields of its two cameras. What is not:
// time (speeded up); the Sun, drawn at least a fifth of a degree wide so that it can
// be seen; shadows (of the planet on its rings and moons); how the spacecraft is
// rolled about its dish.
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
#include <utility>
#include <vector>

#include "panels.hpp"

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
  GameApi::ML ml{};                   // the bodies
  GameApi::ML stars[3]{};             // the stars: for the eye, the wide-angle and the narrow-angle camera
  GameApi::MN stars_mn[3]{};
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

// ---- on the screen besides the picture: what the spacecraft is doing, and where it is ----

const colour kInk{0.86f, 0.88f, 0.90f, 1.0f}, kMuted{0.56f, 0.59f, 0.63f, 1.0f}, kAmber{1.0f, 0.72f, 0.2f, 1.0f};
const colour kPanel{0.02f, 0.03f, 0.05f, 0.74f}, kEdge{1.0f, 1.0f, 1.0f, 0.2f};

std::string km(double v) {  // 1 234 567
  char raw[40];
  std::snprintf(raw, sizeof raw, "%.0f", std::fabs(v));
  const std::string d = raw;
  std::string s = v < 0 ? "-" : "";
  for (std::size_t i = 0; i < d.size(); ++i) {
    s += d[i];
    if ((d.size() - 1 - i) % 3 == 0 && i + 1 < d.size()) s += ' ';
  }
  return s;
}
std::string span_of(double s) {  // a length of time in words
  s = std::fabs(s);
  char b[40];
  if (s >= 2.0 * 86400.0) std::snprintf(b, sizeof b, "%d d %d h", static_cast<int>(s / 86400.0), static_cast<int>(std::fmod(s, 86400.0) / 3600.0));
  else if (s >= 3600.0) std::snprintf(b, sizeof b, "%d h %02d min", static_cast<int>(s / 3600.0), static_cast<int>(std::fmod(s, 3600.0) / 60.0));
  else if (s >= 60.0) std::snprintf(b, sizeof b, "%d min", static_cast<int>(s / 60.0));
  else std::snprintf(b, sizeof b, "%d s", static_cast<int>(s));
  return b;
}

// The panels. `target` is the body the tour is on (or none); `zoom` the camera.
void draw_panels(panels& ui, const scene& c, double t, double rate, bool paused, std::size_t target, double zoom, bool data, bool map) {
  ui.start();
  const auto W = static_cast<float>(ui.width), H = static_cast<float>(ui.height);
  const body& planet = c.bodies[0];
  const auto p = glass::orbit::state_at(std::span<const orbit_sample>(planet.path), t, 8);
  if (!p) return;
  const float lh = 20.0f, pad = 12.0f, cw = static_cast<float>(kGlyphW);

  if (data) {
    // left: where and when
    std::vector<std::pair<std::string, std::string>> rows;
    char b[80];
    rows.push_back({"date", date_at(c, t)});
    rows.push_back({"from " + c.planet + "'s centre", km(glass::orbit::norm(p->pos)) + " km"});
    rows.push_back({"above its cloud tops", km(glass::orbit::norm(p->pos) - planet.radius) + " km"});
    std::snprintf(b, sizeof b, "%.1f km/s", glass::orbit::norm(p->vel));
    rows.push_back({"speed past " + c.planet, b});
    std::snprintf(b, sizeof b, "x %.0f", rate);
    rows.push_back({"time runs", paused ? "paused" : b});
    rows.push_back({"seen", zoom > 10.0 ? "narrow-angle camera, 0.42 deg" : zoom > 1.0 ? "wide-angle camera, 3.2 deg" : "by eye, beside the spacecraft"});
    if (target < c.bodies.size()) rows.push_back({"looking at", c.bodies[target].name});
    const float w = 48.0f * cw + 2.0f * pad, h = lh * (static_cast<float>(rows.size()) + 1.0f) + 2.0f * pad;
    ui.box(16, 16, w, h, kPanel);
    ui.frame(16, 16, w, h, kEdge);
    std::string title = "VOYAGER 2 AT " + c.planet;
    for (char& ch : title) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    ui.text(16 + pad, 16 + pad, title, kAmber);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const float y = 16 + pad + lh * (static_cast<float>(i) + 1.0f);
      ui.text(16 + pad, y, rows[i].first, kMuted);
      ui.text(16 + w - pad - panels::text_width(rows[i].second), y, rows[i].second, kInk);
    }

    // right: every body it passes: how far now, how near at the nearest, and when
    const float tw = 57.0f * cw + 2.0f * pad, th = lh * (static_cast<float>(c.flybys.size()) + 1.0f) + 2.0f * pad, tx = W - 16 - tw;
    ui.box(tx, 16, tw, th, kPanel);
    ui.frame(tx, 16, tw, th, kEdge);
    const float col[4] = {tx + pad + 2.0f * cw, tx + pad + 25.0f * cw, tx + pad + 39.0f * cw, tx + tw - pad};  // name from; the rest end at
    ui.text(col[0], 16 + pad, "passing", kMuted);
    ui.text(col[1] - panels::text_width("now, km"), 16 + pad, "now, km", kMuted);
    ui.text(col[2] - panels::text_width("nearest, km"), 16 + pad, "nearest, km", kMuted);
    ui.text(col[3] - panels::text_width("nearest"), 16 + pad, "nearest", kMuted);
    for (std::size_t k = 0; k < c.flybys.size(); ++k) {
      const body& m = c.bodies[c.flybys[k]];
      const auto at = glass::orbit::state_at(std::span<const orbit_sample>(m.path), t, 8);
      const float y = 16 + pad + lh * (static_cast<float>(k) + 1.0f);
      const colour ink = c.flybys[k] == target ? kAmber : kInk;
      if (c.flybys[k] == target) ui.text(tx + pad, y, ">", kAmber);
      ui.text(col[0], y, m.name, ink);
      const std::string now = at ? km(glass::orbit::norm(at->pos)) : "", near = km(m.nearest);
      const std::string when = m.nearest_t > t ? "in " + span_of(m.nearest_t - t) : span_of(t - m.nearest_t) + " ago";
      ui.text(col[1] - panels::text_width(now), y, now, ink);
      ui.text(col[2] - panels::text_width(near), y, near, kMuted);
      ui.text(col[3] - panels::text_width(when), y, when, m.nearest_t > t ? ink : kMuted);
    }
    // the keys
    const char* keys[2] = {"1-4 planet   t tour   n/p flyby   z camera   v spacecraft", "f planet   +/- speed   space pause   m map   i data   Esc quit"};
    for (int i = 0; i < 2; ++i) ui.text(W - 16 - panels::text_width(keys[i]), H - 16 - lh * static_cast<float>(2 - i), keys[i], kMuted);
    ui.draw();
  }

  if (map) {
    // Two maps of the planet's system, to scale, the planet in the middle: from above
    // its north pole and from the side (north up). Both have the same axis across:
    // the direction Voyager came from, on the left. They close in as Voyager does.
    const vec3 start = -1.0 * planet.path.front().pos;  // the spacecraft from the planet, at the start
    double ax = -start.x, ay = -start.y;
    const double al = std::hypot(ax, ay);
    if (al < 1.0) ax = 1.0, ay = 0.0; else ax /= al, ay /= al;
    const vec3 craft = -1.0 * p->pos;
    double reach = 0.0;
    for (const orbit_sample& q : planet.path) reach = std::max(reach, glass::orbit::norm(q.pos));
    const double half = std::clamp(1.6 * glass::orbit::norm(craft), 5.0 * planet.radius, 1.05 * reach);
    const float S = 230.0f, top_y = H - 16 - S;
    const std::size_t n = planet.path.size(), skip = std::max<std::size_t>(1, n / 500);
    const std::size_t now = std::min(n - 1, static_cast<std::size_t>(t / c.step));
    const colour tint = c.planet == "Jupiter" ? colour{0.86f, 0.72f, 0.56f, 1} : c.planet == "Saturn" ? colour{0.90f, 0.82f, 0.62f, 1} : c.planet == "Uranus" ? colour{0.66f, 0.86f, 0.90f, 1} : colour{0.42f, 0.56f, 0.96f, 1};
    for (int view = 0; view < 2; ++view) {
      const float x0 = 16 + static_cast<float>(view) * (S + 10), cx = x0 + S / 2, cy = top_y + S / 2;
      const auto at = [&](vec3 q, float& x, float& y) {
        const double u = q.x * ax + q.y * ay, v = view == 0 ? -q.x * ay + q.y * ax : q.z;
        x = cx + static_cast<float>(u / half) * S / 2;
        y = cy - static_cast<float>(v / half) * S / 2;
      };
      ui.box(x0, top_y, S, S, kPanel);
      ui.draw();
      float x = 0, y = 0, px = 0, py = 0;
      // the moons' paths during these days
      for (std::size_t i : c.flybys) {
        if (i == 0) continue;
        const body& m = c.bodies[i];
        for (std::size_t j = 0; j < n; j += skip) {
          at(m.path[j].pos - planet.path[j].pos, x, y);
          if (j) ui.line(px, py, x, y, 1.0f, {0.62f, 0.68f, 0.76f, 0.4f});
          px = x, py = y;
        }
      }
      // the planet (and Saturn's rings)
      const float r = static_cast<float>(planet.radius / half) * S / 2;
      if (c.planet == "Saturn") {
        if (view == 0) ui.ring(cx, cy, 1.11f * r, 1.0f, {tint.r, tint.g, tint.b, 0.6f}), ui.ring(cx, cy, 2.33f * r, 1.0f, {tint.r, tint.g, tint.b, 0.6f});
        else ui.line(cx - 2.33f * r, cy, cx + 2.33f * r, cy, 1.0f, {tint.r, tint.g, tint.b, 0.6f});
      }
      ui.dot(cx, cy, std::max(2.5f, r), tint);
      // Voyager's path: behind it solid, ahead of it faint
      for (std::size_t j = 0; j < n; j += skip) {
        at(-1.0 * planet.path[j].pos, x, y);
        if (j) ui.line(px, py, x, y, j <= now ? 1.6f : 1.0f, {kAmber.r, kAmber.g, kAmber.b, j <= now ? 0.95f : 0.35f});
        px = x, py = y;
      }
      // the moons now, and the spacecraft
      for (std::size_t i : c.flybys) {
        if (i == 0) continue;
        const body& m = c.bodies[i];
        const auto ms = glass::orbit::state_at(std::span<const orbit_sample>(m.path), t, 8);
        if (!ms) continue;
        at(ms->pos - p->pos, x, y);
        ui.dot(x, y, 2.6f, i == target ? kAmber : kInk);
        if (view == 0) ui.text(x + 6, y - 9, m.name, i == target ? kAmber : kMuted);
      }
      at(craft, x, y);
      ui.ring(x, y, 6.5f, 1.4f, kAmber);
      ui.dot(x, y, 2.6f, kAmber);
      // where the sunlight comes from
      const body& sun = c.bodies[1];
      if (const auto ss = glass::orbit::state_at(std::span<const orbit_sample>(sun.path), t, 8)) {
        const vec3 q = ss->pos - p->pos;
        const double u = q.x * ax + q.y * ay, v = view == 0 ? -q.x * ay + q.y * ax : q.z, l = std::hypot(u, v);
        if (l > 0.2 * glass::orbit::norm(q)) {
          const auto dx = static_cast<float>(u / l), dy = static_cast<float>(-v / l);
          ui.line(cx + dx * (S / 2 - 26), cy + dy * (S / 2 - 26), cx + dx * (S / 2 - 6), cy + dy * (S / 2 - 6), 2.0f, {1.0f, 0.97f, 0.8f, 0.9f});
          ui.text(cx + dx * (S / 2 - 44) - 13, cy + dy * (S / 2 - 44) - 9, "Sun", {1.0f, 0.97f, 0.8f, 0.9f});
        }
      }
      const float clip[4] = {x0, top_y, S, S};
      ui.draw(clip);
      // the scale: a round length, at most 45 % of the map across
      double bar = std::pow(10.0, std::floor(std::log10(0.9 * half)));
      if (5.0 * bar <= 0.9 * half) bar *= 5.0; else if (2.0 * bar <= 0.9 * half) bar *= 2.0;
      const float bl = static_cast<float>(bar / half) * S / 2;
      ui.frame(x0, top_y, S, S, kEdge);
      ui.box(x0 + 8, top_y + S - 10, bl, 2, kInk);
      ui.text(x0 + 8, top_y + S - 30, km(bar) + " km", kInk);
      ui.text(x0 + 2, top_y - 21, view == 0 ? "from above the north pole" : "from the side, north up", kMuted);
    }
    ui.draw();
  }
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

  const auto build = [&](scene& s) {
    std::vector<GameApi::ML> all;
    const body& sun = s.bodies[1];
    const vec3 to_sun = to_view((1.0 / glass::orbit::norm(sun.path[sun.path.size() / 2].pos)) * sun.path[sun.path.size() / 2].pos);
    for (body& b : s.bodies) {
      if (b.name == "Earth") continue;  // not drawn: from out there it is a point beside the Sun
      b.ml = model(home + "/out/ball-" + lower(b.name) + ".glb", to_sun, b.name == "Sun" ? std::string() : map_of(b.name), b.name == "Sun", b.mn);  // the Sun: plain white
      all.push_back(b.ml);
      if (b.name != "Sun") all.push_back(model(home + "/out/ball-shade.glb", to_sun, map_of("shade"), true, b.night));
    }
    for (int cam = 0; cam < 3; ++cam)
      s.stars[cam] = model(home + "/out/stars-" + lower(s.planet) + "-" + std::to_string(cam + 1) + ".glb", to_sun, home + "/out/star-dots.png", true, s.stars_mn[cam]);
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
  // the spacecraft: NASA's model, one file per textured part, sunlight already in its
  // texture (tools/undraco.py): each side of each surface as bright as it faces the Sun
  std::vector<GameApi::MN> probe_mn;
  std::vector<GameApi::ML> parts;
  for (int i = 0; i < 64; ++i) {
    char part[64];
    std::snprintf(part, sizeof part, "/models/probe/part-%02d.glb", i);
    if (!std::ifstream(home + part).good()) break;
    probe_mn.push_back({});
    std::string map = home + part;
    map.replace(map.size() - 3, 3, "jpg");
    parts.push_back(model(home + part, {0.5, 0.6, 0.6}, map, false, probe_mn.back()));
  }
  const GameApi::ML probe = ev.mainloop_api.array_ml(ev, parts);

  // optional: where to start, in hours from the nearest approach to the planet, and the speed
  // without them: the three-minute tour
  const double start_hours = argc > 3 ? std::atof(argv[3]) : -1.0;
  double t = 0.0, speed = argc > 4 ? std::atof(argv[4]) : 600.0;
  bool tour = argc <= 3, craft = true, show_map = true, show_data = true;
  panels ui;
  const bool ui_ok = ui.init();
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
        zoom = across < 0.34 ? 40.0 : across < 2.6 ? 5.4 : 1.0;  // at most four fifths of the camera's field
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
        if (e.ch == 'm') show_map = !show_map;
        if (e.ch == 'i') show_data = !show_data;
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
      // dragging turns the view about the spacecraft, and only that: through a camera
      // (the spacecraft out of view) the view stays on its body
      if (e.type == 0x401 && zoom <= 1.0) { drag = true; follow = false; last_x = x; last_y = y; }
      if (e.type == 0x402) drag = false;
      if (e.type == 0x400 && drag && zoom <= 1.0) {
        yaw += (x - last_x) * 0.008f;
        pitch = std::clamp(pitch + (y - last_y) * 0.008f, -1.5f, 1.5f);
        last_x = x;
        last_y = y;
      }
      if (e.type == 0x403) eye = std::clamp(eye * (e.mouse_wheel_y > 0 ? 0.87f : 1.15f), 150.0f, 5000.0f);
    }

    // every body where and as large as it looks from the spacecraft
    scene& c = scenes[current];
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
      const auto k = static_cast<float>(r) / kBall;
      GameApi::M m = ev.matrix_api.scale(k, k, k);
      if (&b == &c.bodies[0] && c.day != 0.0) m = ev.matrix_api.mult(ev.matrix_api.yrot(static_cast<float>(2.0 * kPi * t / (c.day * 3600.0))), m);
      m = ev.matrix_api.mult(m, ev.matrix_api.trans(static_cast<float>(far * dir.x), static_cast<float>(far * dir.y), static_cast<float>(far * dir.z)));
      ev.move_api.set_matrix(b.mn, m);
      if (&b == &c.bodies[0])  // the rings: measured by GameApi as a box 2.33 planet radii each way
        for (const GameApi::MN& ring : c.rings)
          ev.move_api.set_matrix(ring, ev.matrix_api.mult(ev.matrix_api.scale(k * 2.33f, k * 2.33f, k * 2.33f),
                                                          ev.matrix_api.trans(static_cast<float>(far * dir.x), static_cast<float>(far * dir.y), static_cast<float>(far * dir.z))));
      if (b.name != "Sun") {  // sunlight does not turn with the planet. Its ball is the body's, 1 % nearer the eye and 1 % smaller: the same outline, in front everywhere
        const double in = 0.99 * far;
        ev.move_api.set_matrix(b.night, ev.matrix_api.mult(ev.matrix_api.mult(c.sunward, ev.matrix_api.scale(k * 0.99f, k * 0.99f, k * 0.99f)),
                                                           ev.matrix_api.trans(static_cast<float>(in * dir.x), static_cast<float>(in * dir.y), static_cast<float>(in * dir.z))));
      }
    }
    // the spacecraft with its dish (the model's y axis) toward the Earth
    const double ea = std::atan2(-to_earth.x, -to_earth.z);
    const double ep = std::atan2(to_earth.y, -to_earth.x * std::sin(ea) - to_earth.z * std::cos(ea));
    const GameApi::M earthward = ev.matrix_api.mult(ev.matrix_api.xrot(static_cast<float>(-kPi / 2.0)),
                                                    ev.matrix_api.mult(ev.matrix_api.xrot(static_cast<float>(ep)), ev.matrix_api.yrot(static_cast<float>(ea))));
    for (const GameApi::MN& mn : probe_mn) ev.move_api.set_matrix(mn, ev.matrix_api.mult(earthward, ev.matrix_api.scale(0.5f, 0.5f, 0.5f)));

    const GameApi::M look = ev.matrix_api.mult(ev.matrix_api.yrot(yaw), ev.matrix_api.xrot(pitch));
    ev.mainloop_api.clear_3d(0xff000000);
    // the eye at the spacecraft. A camera is a longer lens: the whole picture that many times larger.
    const auto power = static_cast<float>(zoom);
    const GameApi::M lens = ev.matrix_api.scale(power, power, 1.0f);
    const int cam = zoom <= 1.0 ? 0 : zoom < 10.0 ? 1 : 2;
    ev.move_api.set_matrix(c.stars_mn[cam], ev.matrix_api.scale(kSky / kBall, kSky / kBall, kSky / kBall));
    glDepthMask(GL_FALSE);  // the stars are behind everything and do not hide each other
    ev.mainloop_api.execute_ml(ev, c.stars[cam], sh, sh2, sh2, sh3, look, lens, look, w, h);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    ev.mainloop_api.execute_ml(ev, c.ml, sh, sh2, sh2, sh3, look, lens, look, w, h);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (craft && zoom <= 1.0) {
      ev.mainloop_api.execute_ml(ev, probe, sh, sh2, sh2, sh3, look, ev.matrix_api.trans(0.0f, 0.0f, -eye), look, w, h);
    }
    if (ui_ok) draw_panels(ui, c, t, rate, paused, tour && stretch < c.tour.size() ? c.tour[stretch].target : static_cast<std::size_t>(-1), zoom, show_data, show_map);
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

// Voyager 2's planetary encounters, to fly along with: the spacecraft in the middle,
// the planet, its moons, the Sun and the stars around it as seen from the
// spacecraft, speeded up. Drawn with GameApi (https://github.com/terop2/GameApi).
//
//   drag with the mouse   turn the eye about the spacecraft; the turn glides on a little. Through a camera
//                         the picture stays on its body and only the spacecraft, beside it, turns
//   wheel                 nearer, farther       c       the eye back to where it started
//   click a name          a planet (top middle): that system; a body in the list (top right): look at it
//   Tab                   look at the next body in the list       f   at the planet
//   , .                   to the previous, the next flyby
//   1 to 9                time at one rate: 1 real time, then 10, 30, 100 ... 9 is 30 000 times
//   t (or 0)              time at the tour's own pace again       + -   faster, slower       space   pause
//   a                     autonomous: the view follows Voyager's own pictures, one after the other: where its
//                         camera pointed, through the camera that took each (NASA's record of all of them)
//   z                     by eye, through Voyager's wide-angle camera (3.2 degrees), through its narrow-angle camera (0.42 degrees)
//   v, m, i               the spacecraft, the maps, the data panels: on or off
//   j s u n               Jupiter, Saturn, Uranus, Neptune       r   start of the scene       Esc   quit
//
// The tour (on at the start): each system in three minutes, from arrival to
// departure. Time runs slowly near things and fast between them, without steps:
// the planet grows at an even pace on the way in, every moon gets its share around
// its nearest approach, and the planet shrinks again behind. The view is on the
// body whose flyby it is, else on the planet, through the camera Voyager would use
// for a body of that size in the sky; it swings from one to the next. A body
// picked by hand holds until the tour moves on.
//
// What is true to life: the direction and the apparent size of every body at every
// moment (from out/<planet>.scene, written by the simulation), the stars (the
// Hipparcos catalogue: each star a dot at its measured place, as bright and of the colour measured; to magnitude 6.5 for the eye, deeper
// through the cameras; one star field for all four planets, at 1984), the surface maps in visible light,
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "panels.hpp"

#include "GameApi.hh"
#include "Tasks.hh"
#include "orbit.hpp"

IMPORT extern pthread_t g_main_thread_id;

#ifdef GAME_TEST
// For testing without hands: GAME_TEST_EVENTS="20,key,51 30,down,900,95 30.1,up,900,95"
// sends keys and mouse events to the window at those seconds after the start.
#include <SDL2/SDL.h>
void test_events(double dt) {
  static double clock = 0.0;
  static std::vector<std::string> todo;
  static bool read = false;
  if (!read) {
    read = true;
    std::istringstream in(std::getenv("GAME_TEST_EVENTS") ? std::getenv("GAME_TEST_EVENTS") : "");
    for (std::string item; in >> item;) todo.push_back(item);
  }
  clock += dt;
  while (!todo.empty()) {
    double at = 0.0;
    char kind[16] = "";
    int a = 0, b = 0;
    std::sscanf(todo.front().c_str(), "%lf,%15[^,],%d,%d", &at, kind, &a, &b);
    if (at > clock) break;
    todo.erase(todo.begin());
    SDL_Event e{};
    const std::string k = kind;
    if (k == "key") { e.type = SDL_KEYDOWN; e.key.keysym.sym = a; e.key.state = SDL_PRESSED; }
    if (k == "down" || k == "up" || k == "move") SDL_WarpMouseInWindow(SDL_GL_GetCurrentWindow(), a, b);
    if (k == "down" || k == "up") { e.type = k == "down" ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP; e.button.button = SDL_BUTTON_LEFT; e.button.x = a; e.button.y = b; }
    if (k == "move") { e.type = SDL_MOUSEMOTION; e.motion.x = a; e.motion.y = b; e.motion.state = SDL_BUTTON_LMASK; }
    SDL_PushEvent(&e);
  }
}
#endif

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
  double frame[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // the planet's axes in celestial axes, row by row
  // The three-minute tour: how slowly time runs near each body (seconds of play for
  // a second of flight): near_planet / distance to the planet, plus for each moon
  // near_body * (its nearest distance / its distance) squared.
  double near_planet = 0.0;
  std::vector<double> near_body;
  std::vector<double> played;         // seconds of the tour gone by at each epoch
  std::vector<double> turns;          // when the tour's sight passes from one flyby to the next
  // what Voyager 2 photographed (data/shots_<planet>.txt): when, where the camera
  // pointed (a direction in the game's axes), with which camera, and at what
  struct shot { double t; vec3 dir; bool narrow; std::string target; };
  std::vector<shot> shots;
  GameApi::ML ml{};                   // the bodies
  GameApi::M sky{};                   // turns the stars from celestial axes into this planet's
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
  f >> word;                                                      // "frame" and nine numbers
  for (double& v : s.frame) f >> v;
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

  // The tour: three minutes from arrival to departure. Time runs slowly near things
  // and fast between them, without steps. Near the planet the slowness is as one
  // over the distance, so that the planet grows and shrinks at an even pace; each
  // moon adds a slow stretch around its nearest approach, and every moon gets the
  // same share of the three minutes (30 seconds at most, 100 seconds among them).
  const double total = 180.0;
  const std::size_t moons = s.flybys.size() - 1;
  const double each = moons ? std::min(30.0, 100.0 / static_cast<double>(moons)) : 0.0;
  s.near_body.assign(s.bodies.size(), 0.0);
  double sum = 0.0;
  for (std::size_t j = 0; j < n; ++j) sum += s.step / glass::orbit::norm(s.bodies[0].path[j].pos);
  s.near_planet = (total - each * static_cast<double>(moons)) / sum;
  for (std::size_t i : s.flybys) {
    if (i == 0) continue;
    sum = 0.0;
    for (std::size_t j = 0; j < n; ++j) sum += s.step * std::pow(s.bodies[i].nearest / glass::orbit::norm(s.bodies[i].path[j].pos), 2);
    s.near_body[i] = each / sum;
  }
  s.played.assign(n, 0.0);
  for (std::size_t j = 1; j < n; ++j) {
    double slow = s.near_planet / glass::orbit::norm(s.bodies[0].path[j].pos);
    for (std::size_t i : s.flybys) slow += s.near_body[i] * std::pow(s.bodies[i].nearest / glass::orbit::norm(s.bodies[i].path[j].pos), 2);
    s.played[j] = s.played[j - 1] + slow * s.step;
  }
  // the sight passes from one flyby to the next halfway between them, in the tour's time
  const auto played_at = [&](double t) { return s.played[std::min(n - 1, static_cast<std::size_t>(t / s.step))]; };
  for (std::size_t k = 0; k + 1 < s.flybys.size(); ++k) {
    const double half = 0.5 * (played_at(s.bodies[s.flybys[k]].nearest_t) + played_at(s.bodies[s.flybys[k + 1]].nearest_t));
    std::size_t j = 0;
    while (j + 1 < n && s.played[j] < half) ++j;
    s.turns.push_back(static_cast<double>(j) * s.step);
  }
  return !s.bodies.empty();
}

// Voyager 2's pictures during the scene. Times in the file are UTC; the scene runs
// on the time of the ephemeris (TDB), which was ahead of UTC by 50 to 56 seconds.
void read_shots(const std::string& file, scene& s) {
  const auto days = [](int y, int m, int d) {  // days from a fixed day, by the calendar
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097L + yoe * 365L + yoe / 4 - yoe / 100 + doy;
  };
  int y0 = 0, d0 = 0, h0 = 0, mi0 = 0;
  char mon[8] = "";
  std::sscanf(s.start_date.c_str(), " %d-%3s-%d %d:%d", &y0, mon, &d0, &h0, &mi0);
  const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* at = std::strstr(months, mon);
  const int m0 = at ? static_cast<int>(at - months) / 3 + 1 : 1;
  const double ahead = y0 < 1980 ? 50.184 : y0 < 1982 ? 52.184 : y0 < 1988 ? 55.184 : 56.184;
  std::ifstream f(file);
  std::string when, camera, target;
  double ra = 0.0, dec = 0.0;
  while (f >> when >> ra >> dec >> camera >> target) {
    int y = 0, m = 0, d = 0, h = 0, mi = 0;
    double sec = 0.0;
    if (std::sscanf(when.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &m, &d, &h, &mi, &sec) != 6) continue;
    const double t = static_cast<double>(days(y, m, d) - days(y0, m0, d0)) * 86400.0 + (h - h0) * 3600.0 + (mi - mi0) * 60.0 + sec + ahead;
    const double a = ra * kPi / 180.0, b = dec * kPi / 180.0;
    const vec3 sky{std::cos(b) * std::cos(a), std::cos(b) * std::sin(a), std::sin(b)};  // celestial axes
    const double* fr = s.frame;                                                         // into the planet's, then into the game's (x, z, -y)
    const vec3 p{fr[0] * sky.x + fr[1] * sky.y + fr[2] * sky.z, fr[3] * sky.x + fr[4] * sky.y + fr[5] * sky.z, fr[6] * sky.x + fr[7] * sky.y + fr[8] * sky.z};
    for (char& ch : target) if (ch == '_') ch = ' ';
    s.shots.push_back({t, {p.x, p.z, -p.y}, camera == "N", target});
  }
  // A check on everything at once (the simulation, the frames, the clocks): in how
  // many of the pictures of a body of the scene is that body, where the simulation
  // has it, inside the picture? A picture is 0.42 or 3.2 degrees square.
  std::size_t aimed = 0, inside = 0;
  for (const scene::shot& k : s.shots)
    for (const body& b : s.bodies) {
      if (b.name != k.target) continue;
      const auto at = glass::orbit::state_at(std::span<const orbit_sample>(b.path), k.t, 8);
      if (!at) continue;
      const vec3 d = (1.0 / glass::orbit::norm(at->pos)) * at->pos;
      const double apart = std::acos(std::clamp(d.x * k.dir.x + d.z * k.dir.y - d.y * k.dir.z, -1.0, 1.0)) * 180.0 / kPi;
      const double disc = std::asin(std::min(1.0, b.radius / glass::orbit::norm(at->pos))) * 180.0 / kPi;
      ++aimed;
      if (apart < disc + (k.narrow ? 0.30 : 2.24)) ++inside;  // the disc reaches into the picture (half its diagonal)
    }
  std::printf("%s: %zu pictures by Voyager 2; of %zu aimed at a body of the scene, %zu have it in the picture where the simulation puts it\n", s.planet.c_str(), s.shots.size(),
              aimed, inside);
}

// seconds of play for a second of flight, at a moment of the tour
double slowness(const scene& c, double t) {
  double slow = 0.0;
  for (std::size_t i : c.flybys) {
    const auto at = glass::orbit::state_at(std::span<const orbit_sample>(c.bodies[i].path), t, 8);
    if (!at) continue;
    const double d = glass::orbit::norm(at->pos);
    slow += i == 0 ? c.near_planet / d : c.near_body[i] * std::pow(c.bodies[i].nearest / d, 2);
  }
  return std::max(slow, 1e-9);
}

// The tour's sight at a moment: the body whose flyby it is, while it is within
// twice its nearest distance; before, after and between, the planet.
std::size_t sight(const scene& c, double t) {
  std::size_t k = 0;
  while (k < c.turns.size() && t >= c.turns[k]) ++k;
  const std::size_t i = c.flybys[k];
  if (i == 0) return 0;
  const auto at = glass::orbit::state_at(std::span<const orbit_sample>(c.bodies[i].path), t, 8);
  return at && glass::orbit::norm(at->pos) < 2.0 * c.bodies[i].nearest ? i : 0;
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
// which row of the table of bodies (top right) a point of the window is on: the body, or none
std::size_t body_at(const panels& ui, const scene& c, float x, float y) {
  const float tw = 57.0f * static_cast<float>(kGlyphW) + 24.0f, tx = static_cast<float>(ui.width) - 16 - tw;
  const float row = (y - 16.0f - 12.0f) / 20.0f - 1.0f;
  if (x < tx || x > tx + tw || row < 0.0f || row >= static_cast<float>(c.flybys.size())) return static_cast<std::size_t>(-1);
  return c.flybys[static_cast<std::size_t>(row)];
}

// the four planets' names, top middle, two by two: where each is, and which one a point is on
const char* const kPlanets[4] = {"Jupiter", "Saturn", "Uranus", "Neptune"};
void planet_box(const panels& ui, int i, float& x, float& y, float& w, float& h) {
  w = 100.0f, h = 26.0f;
  x = static_cast<float>(ui.width) / 2 - 32.0f + static_cast<float>(i % 2 - 1) * (w + 6.0f) + 3.0f;
  y = 16.0f + static_cast<float>(i / 2) * (h + 6.0f);
}
int planet_at(const panels& ui, float px, float py) {
  for (int i = 0; i < 4; ++i) {
    float x = 0, y = 0, w = 0, h = 0;
    planet_box(ui, i, x, y, w, h);
    if (px >= x && px < x + w && py >= y && py < y + h) return i;
  }
  return -1;
}

void draw_panels(panels& ui, const scene& c, double t, double rate, bool paused, std::size_t target, double zoom, bool data, bool map, double tour_at, const scene::shot* shot,
                 std::size_t shot_number) {
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
    rows.push_back({"time runs", paused ? "paused" : rate < 1.5 ? "as it did (x 1)" : b});
    rows.push_back({"seen", zoom > 10.0 ? "narrow-angle camera, 0.42 deg" : zoom > 1.0 ? "wide-angle camera, 3.2 deg" : "by eye, beside the spacecraft"});
    if (shot) {  // following Voyager's own pictures
      rows.push_back({"Voyager photographs", shot->target});
      std::snprintf(b, sizeof b, "%s of %s", km(static_cast<double>(shot_number)).c_str(), km(static_cast<double>(c.shots.size())).c_str());
      rows.push_back({"picture", b});
    } else if (target < c.bodies.size()) {
      rows.push_back({"looking at", c.bodies[target].name});
    }
    if (tour_at >= 0.0) {
      std::snprintf(b, sizeof b, "%d:%02d of 3:00", static_cast<int>(tour_at) / 60, static_cast<int>(tour_at) % 60);
      rows.push_back({"tour", b});
    }
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
    if (shot) {  // the corners of the picture Voyager took: its cameras' pictures were square
      const float side = static_cast<float>(ui.height) - 8.0f, x0 = (W - side) / 2, y0 = 4.0f, arm = 28.0f;
      for (int k = 0; k < 4; ++k) {
        const float x = x0 + (k % 2 ? side : 0.0f), y = y0 + (k / 2 ? side : 0.0f), dx = k % 2 ? -arm : arm, dy = k / 2 ? -arm : arm;
        ui.line(x, y, x + dx, y, 1.5f, kAmber);
        ui.line(x, y, x, y + dy, 1.5f, kAmber);
      }
    }
    // the planets, to click on
    for (int i = 0; i < 4; ++i) {
      float x = 0, y = 0, w = 0, h = 0;
      planet_box(ui, i, x, y, w, h);
      const bool here = c.planet == kPlanets[i];
      ui.box(x, y, w, h, kPanel);
      ui.frame(x, y, w, h, here ? kAmber : kEdge);
      ui.text(x + (w - panels::text_width(kPlanets[i])) / 2, y + 4, kPlanets[i], here ? kAmber : kInk);
    }
    // the keys
    const char* keys[4] = {"drag: turn about the spacecraft   wheel: nearer, farther   c: back", "click a name, or Tab: look at it   , . previous, next flyby   f: the planet",
                           "1-9: speed (1 real time)   t: tour pace   space: pause   a: Voyager's own pictures", "z: camera   v: spacecraft   m: map   i: data   j s u n: planets   Esc"};
    for (int i = 0; i < 4; ++i) ui.text(W - 16 - panels::text_width(keys[i]), H - 16 - lh * static_cast<float>(4 - i), keys[i], kMuted);
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

// GameApi reads models on several threads, and about one start in ten these wait
// for each other for ever (or one runs out of memory). A watch on the side starts
// the program afresh when no picture has been drawn for 45 seconds, or when the
// program is about to stop on an error soon after its start; three times at most.
std::atomic<long> g_drawn{0};  // when the last picture was drawn, seconds on the steady clock
char** g_args = nullptr;
long seconds_now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void start_afresh(const char* why) {
  const char* tries = std::getenv("VOYAGER_STARTS");
  const int n = tries ? std::atoi(tries) : 0;
  if (n >= 3) {
    std::fprintf(stderr, "\n%s, three times over: giving up\n", why);
    std::_Exit(1);
  }
  std::fprintf(stderr, "\n%s: starting afresh\n", why);
  setenv("VOYAGER_STARTS", std::to_string(n + 1).c_str(), 1);
  execv("/proc/self/exe", g_args);
  std::_Exit(1);
}

int main(int argc, char** argv) {
  g_args = argv;
  g_drawn = seconds_now();
  const long started = seconds_now();
  std::set_terminate([] { start_afresh("the program stopped on an error"); });
  std::thread([started] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (seconds_now() - g_drawn > 45) start_afresh("no picture for 45 seconds (GameApi's loading has stalled)");
      if (seconds_now() - started > 120) std::set_terminate(nullptr);  // later errors are not the start's
    }
  }).detach();
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
  for (int i = 0; i < 4; ++i) read_shots(home + "/data/shots_" + names[i] + ".txt", scenes[i]);

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

  // Any turn as a GameApi matrix. GameApi gives turns about its three axes; which
  // way each turns a point is measured here, the wanted turn is taken apart into
  // three such turns, and the result is checked against what was wanted.
  const auto moved = [&](GameApi::M m, vec3 p) {
    const GameApi::PT q = ev.matrix_api.mult(ev.point_api.point(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)), m);
    return vec3{ev.point_api.pt_x(q), ev.point_api.pt_y(q), ev.point_api.pt_z(q)};
  };
  const double sx = moved(ev.matrix_api.xrot(0.5f), {0, 1, 0}).z > 0 ? 1.0 : -1.0;  // a turn about x takes y toward +z, or the other way
  const double sy = moved(ev.matrix_api.yrot(0.5f), {0, 0, 1}).x > 0 ? 1.0 : -1.0;
  const double sz = moved(ev.matrix_api.zrot(0.5f), {1, 0, 0}).y > 0 ? 1.0 : -1.0;
  const auto turn = [&](const double r[3][3]) {  // r takes a point p to r p; as turns about x, then y, then z
    const double about_y = -std::asin(std::clamp(r[2][0], -1.0, 1.0)), about_x = std::atan2(r[2][1], r[2][2]), about_z = std::atan2(r[1][0], r[0][0]);
    const GameApi::M m = ev.matrix_api.mult(ev.matrix_api.mult(ev.matrix_api.xrot(static_cast<float>(sx * about_x)), ev.matrix_api.yrot(static_cast<float>(sy * about_y))),
                                            ev.matrix_api.zrot(static_cast<float>(sz * about_z)));
    double off = 0.0;
    for (int col = 0; col < 3; ++col) {
      const vec3 got = moved(m, {col == 0 ? 1.0 : 0.0, col == 1 ? 1.0 : 0.0, col == 2 ? 1.0 : 0.0});
      off = std::max({off, std::fabs(got.x - r[0][col]), std::fabs(got.y - r[1][col]), std::fabs(got.z - r[2][col])});
    }
    if (off > 1e-3) std::fprintf(stderr, "a turn came out wrong by %.3f: the stars will be misplaced\n", off);
    return m;
  };

  // the stars: one field for all four planets, for the eye and for each camera
  GameApi::ML stars[3]{};
  GameApi::MN stars_mn[3]{};
  for (int cam = 0; cam < 3; ++cam) stars[cam] = model(home + "/out/stars-" + std::to_string(cam + 1) + ".glb", {0, 0, 1}, home + "/out/star-dots.png", true, stars_mn[cam]);

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
    // The stars' file is in celestial axes, written as (x, z, -y) like every model
    // here; this planet's view axes are its frame, written the same way.
    double want[3][3];
    for (int r = 0; r < 3; ++r)
      for (int col = 0; col < 3; ++col) {
        const int pr[3] = {0, 2, 1}, sign[3] = {1, 1, -1};  // view row r is frame row pr[r] (minus for the third), and the same for the columns
        want[r][col] = sign[r] * sign[col] * s.frame[pr[r] * 3 + pr[col]];
      }
    s.sky = turn(want);
    std::printf("\n%s in three minutes, from arrival to departure: what Voyager 2 passed\n", s.planet.c_str());
    for (std::size_t i : s.flybys)
      std::printf("  %-9s %9.0f km from its centre, %s\n", s.bodies[i].name.c_str(), s.bodies[i].nearest, date_at(s, s.bodies[i].nearest_t).c_str());
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
  const bool tour = argc <= 3;  // started without a time: the tour, from arrival to departure
  bool paced = tour;        // time at the tour's own pace (slow near things), or at one rate
  bool craft = true, show_map = true, show_data = true;
  panels ui;
  const bool ui_ok = ui.init();
  const std::size_t none = static_cast<std::size_t>(-1);
  double pace = 1.0;        // the tour's rate times this
  bool paused = false;
  // What the view is on, and how. The view (or the camera) points at a body, the
  // focus: the tour's sight, or one picked by hand, which holds until the tour
  // moves on. It swings over to a new focus and then stays on it. By eye, the mouse
  // turns the eye about the spacecraft away from there, and the turn glides on a
  // little when the mouse lets go. Through a camera the picture stays on the focus
  // and the mouse turns only the spacecraft, which is drawn beside the picture.
  std::size_t focus = 0, tour_sight = none;
  bool own_lens = false;    // the camera picked by hand: holds until the focus changes
  // Autonomous: the view is Voyager's own. It goes where the camera pointed for each
  // picture it took, through the camera that took it, a little softened so that the
  // eye can follow; the camera changes at most once a second.
  bool autonomous = false;
  double lens_held = 0.0;
  double zoom = 1.0;        // 1: the eye beside the spacecraft; 5.4 and 40: through its wide-angle and narrow-angle cameras
  double aim_yaw = 0.0, aim_pitch = 0.0, from_yaw = 0.0, from_pitch = 0.0, swing = 1.0;
  double off_yaw = 0.1, off_pitch = 0.0, glide_yaw = 0.0, glide_pitch = 0.0;
  bool homing = false;      // the eye on its way back to where it started
  float eye = 500.0f;
  bool drag = false;
  float last_x = 0.0f, last_y = 0.0f;
  // the turns that bring a body straight ahead
  const auto ahead = [](vec3 d, double& yaw, double& pitch) {    // a direction in the game's axes
    const double a = std::atan2(-d.x, -d.z);                      // about y: brings it into the y-z plane, ahead
    const double z1 = -d.x * std::sin(a) - d.z * std::cos(a);     // how far ahead it then is
    yaw = -a;                                                     // GameApi turns the other way about y
    pitch = -std::atan2(d.y, z1);
  };
  const auto toward = [&](const body& b, double& yaw, double& pitch) {
    const auto at = glass::orbit::state_at(std::span<const orbit_sample>(b.path), t, 8);
    if (at) ahead(to_view((1.0 / glass::orbit::norm(at->pos)) * at->pos), yaw, pitch);
  };
  const auto nearer = [](double to, double from) { return std::remainder(to - from, 2.0 * kPi); };  // the short way round
  const auto look_at = [&](std::size_t i) {  // swing over to another body
    if (i == focus && !autonomous) return;
    focus = i;
    from_yaw = aim_yaw, from_pitch = aim_pitch, swing = 0.0;
    own_lens = false;
  };
  const auto enter = [&](int i) {
    current = i;
    scene& s = scenes[i];
    if (!s.built) {  // GameApi reads a model when it first draws it: say so, and draw everything once now
      const auto t0 = std::chrono::steady_clock::now();
      ev.mainloop_api.clear_3d(0xff000000);
      if (ui_ok) {
        ui.start();
        const std::string wait = "loading " + s.planet + " ...";
        ui.text((static_cast<float>(ui.width) - panels::text_width(wait)) / 2, static_cast<float>(ui.height) / 2, wait, kInk);
        ui.draw();
      }
      ev.mainloop_api.swapbuffers();
      build(s);
      const GameApi::M still = ev.matrix_api.identity();
      for (const GameApi::ML& field : stars) ev.mainloop_api.execute_ml(ev, field, sh, sh2, sh2, sh3, still, still, still, w, h);
      ev.mainloop_api.execute_ml(ev, s.ml, sh, sh2, sh2, sh3, still, still, still, w, h);
      std::printf("loaded in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
      g_drawn = seconds_now();
    }
    t = tour ? 0.0 : std::max(0.0, s.bodies[0].nearest_t + start_hours * 3600.0);
    focus = 0;
    if (!tour && argc > 3)  // a moon's name in place of the hours: at its nearest approach
      for (std::size_t k = 0; k < s.bodies.size(); ++k)
        if (lower(s.bodies[k].name) == lower(argv[3])) { focus = k; t = s.bodies[k].nearest_t; }
    tour_sight = sight(s, t);
    if (tour) focus = tour_sight;
    toward(s.bodies[focus], aim_yaw, aim_pitch);
    swing = 1.0, own_lens = false;
    off_yaw = 0.1, off_pitch = 0.0, glide_yaw = glide_pitch = 0.0;
  };
  enter(current);

  auto before = std::chrono::steady_clock::now();
  double shown = -1.0;
  for (bool run = true; run;) {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::min(0.25, std::chrono::duration<double>(now - before).count());
    before = now;
#ifdef GAME_TEST
    test_events(dt);
#endif
    GameApi::MainLoopApi::Event e;
    while ((e = ev.mainloop_api.get_event()).last) {
      scene& c = scenes[current];
      if (e.type == 0x300) {
        if (e.ch == 27) run = false;
        for (int i = 0; i < 4; ++i) if (e.ch == std::tolower(kPlanets[i][0])) enter(i);  // j, s, u, n
        if (e.ch >= '1' && e.ch <= '9') {  // one rate: 1 is real time, 9 a day in three seconds
          const double rates[9] = {1, 10, 30, 100, 300, 1000, 3000, 10000, 30000};
          speed = rates[e.ch - '1'];
          paced = false;
        }
        if (e.ch == '+' || e.ch == '=') (paced ? pace : speed) *= 2.0;
        if (e.ch == '-') (paced ? pace : speed) /= 2.0;
        if (e.ch == 't' || e.ch == '0') { paced = true; pace = 1.0; }  // back to the tour's own pace
        if (e.ch == 'v') craft = !craft;
        if (e.ch == 'm') show_map = !show_map;
        if (e.ch == 'i') show_data = !show_data;
        if (e.ch == 'z') { zoom = zoom < 2.0 ? 5.4 : zoom < 10.0 ? 40.0 : 1.0; own_lens = true; }
        if (e.ch == 'c') homing = true;
        if (e.ch == 'a') {
          autonomous = !autonomous;
          if (!autonomous) from_yaw = aim_yaw, from_pitch = aim_pitch, swing = 0.0, own_lens = false;  // swing back to the tour's sight
        }
        if (e.ch == ' ') paused = !paused;
        if (e.ch == 'r') t = 0.0;
        if (e.ch == 'f') { look_at(0); autonomous = false; }
        if (e.ch == 9) {  // Tab: the next body in the list
          std::size_t k = 0;
          while (k < c.flybys.size() && c.flybys[k] != focus) ++k;
          look_at(c.flybys[(k + 1) % c.flybys.size()]);
          autonomous = false;
        }
        if (e.ch == '.' || e.ch == ',') {  // to the next or the previous flyby, a little before its nearest
          std::size_t to = none;
          for (std::size_t i : c.flybys) {
            const double at = c.bodies[i].nearest_t;
            if (e.ch == '.' && at > t + 600.0 && (to == none || at < c.bodies[to].nearest_t)) to = i;
            if (e.ch == ',' && at < t - 600.0 && (to == none || at > c.bodies[to].nearest_t)) to = i;
          }
          if (to != none) {
            t = std::max(0.0, c.bodies[to].nearest_t - (to == 0 ? 3.0 : 1.0) * c.bodies[to].nearest / 15.0);  // about that far out, at 15 km/s
            tour_sight = none;
            look_at(to);
          }
        }
        shown = -1.0;
      }
      const float x = ev.point_api.pt_x(e.cursor_pos), y = ev.point_api.pt_y(e.cursor_pos);
      if (e.type == 0x401) {
        const std::size_t picked = show_data && ui_ok ? body_at(ui, c, x, y) : none;
        const int planet = show_data && ui_ok ? planet_at(ui, x, y) : -1;
        if (planet >= 0) { if (planet != current) enter(planet); }
        else if (picked != none) { look_at(picked); autonomous = false; }
        else { drag = true; homing = false; glide_yaw = glide_pitch = 0.0; last_x = x; last_y = y; }
      }
      if (e.type == 0x402) drag = false;
      if (e.type == 0x400 && drag) {
        const double dx = (x - last_x) * 0.008, dy = (y - last_y) * 0.008;
        off_yaw += dx, off_pitch += dy;
        glide_yaw = 0.6 * glide_yaw + 0.4 * dx / std::max(dt, 0.005);  // how fast it is turning, for the glide after
        glide_pitch = 0.6 * glide_pitch + 0.4 * dy / std::max(dt, 0.005);
        last_x = x;
        last_y = y;
      }
      if (e.type == 0x403) eye = std::clamp(eye * (e.mouse_wheel_y > 0 ? 0.87f : 1.15f), 150.0f, 5000.0f);
    }

    scene& c = scenes[current];
    // time: the tour's own pace, or one rate
    const double rate = paced ? pace / slowness(c, t) : speed;
    {  // the view goes with the flybys, whatever the pace
      const std::size_t see = sight(c, t);
      if (see != tour_sight) {  // the tour moves on: the view goes with it
        tour_sight = see;
        if (!autonomous) look_at(see);
        else focus = see;
        if (see != 0 && !autonomous) std::printf("\n  %s: nearest %0.f km, %s\n", c.bodies[see].name.c_str(), c.bodies[see].nearest, date_at(c, c.bodies[see].nearest_t).c_str());
      }
    }
    if (!paused) t += rate * dt;
    if (t > c.length || t < 0.0) t = 0.0;

    // the view: on the focus, after a swing of a second and a half
    double to_yaw = aim_yaw, to_pitch = aim_pitch;
    toward(c.bodies[focus], to_yaw, to_pitch);
    swing = std::min(1.0, swing + dt / 1.5);
    const double ease = swing * swing * (3.0 - 2.0 * swing);
    // Voyager's picture at this moment: the last one it took
    const scene::shot* shot = nullptr;
    if (autonomous && !c.shots.empty()) {
      const auto next = std::upper_bound(c.shots.begin(), c.shots.end(), t, [](double at, const scene::shot& k) { return at < k.t; });
      if (next != c.shots.begin()) shot = &*(next - 1);
    }
    lens_held += dt;
    if (shot) {
      double sy = aim_yaw, sp = aim_pitch;
      ahead(shot->dir, sy, sp);
      const double k = 1.0 - std::exp(-dt / 0.25);
      aim_yaw += nearer(sy, aim_yaw) * k, aim_pitch += (sp - aim_pitch) * k;
      const double lens_now = shot->narrow ? 40.0 : 5.4;
      if (lens_now != zoom && lens_held > 1.0) zoom = lens_now, lens_held = 0.0;
      own_lens = true;
    } else {
      aim_yaw = swing < 1.0 ? from_yaw + nearer(to_yaw, from_yaw) * ease : to_yaw;
      aim_pitch = swing < 1.0 ? from_pitch + (to_pitch - from_pitch) * ease : to_pitch;
      if (autonomous) own_lens = false;  // before Voyager's first picture of the scene: the tour's own view
    }
    // the mouse's turn: glides on after the mouse lets go, and goes home when asked
    if (!drag) {
      off_yaw += glide_yaw * dt, off_pitch += glide_pitch * dt;
      const double slow = std::exp(-dt / 0.45);
      glide_yaw *= slow, glide_pitch *= slow;
      if (homing) {
        const double k = 1.0 - std::exp(-dt / 0.35);
        off_yaw += nearer(0.1, off_yaw) * k, off_pitch += (0.0 - off_pitch) * k;
        if (std::fabs(nearer(0.1, off_yaw)) + std::fabs(off_pitch) < 1e-3) homing = false;
      }
    }
    const double eye_pitch = std::clamp(aim_pitch + off_pitch, -1.5, 1.5);
    off_pitch = eye_pitch - aim_pitch;
    // the camera Voyager would use for a body of this size in the sky: the picture
    // at most four fifths of the camera's field (0.42 and 3.2 degrees), else the eye
    if (!own_lens) {
      const auto at = glass::orbit::state_at(std::span<const orbit_sample>(c.bodies[focus].path), t, 8);
      const double across = at ? 2.0 * std::asin(std::min(1.0, c.bodies[focus].radius / glass::orbit::norm(at->pos))) * 180.0 / kPi : 90.0;
      if (zoom > 10.0) { if (across > 0.36) zoom = 5.4; }
      else if (zoom > 1.0) { if (across < 0.28) zoom = 40.0; else if (across > 2.7) zoom = 1.0; }
      else if (across < 0.28) zoom = 40.0;
      else if (across < 2.1) zoom = 5.4;
    }

    // every body where and as large as it looks from the spacecraft
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

    // By eye, one view: the eye beside the spacecraft, turned by the mouse. Through a
    // camera, two: the camera's picture, on the focus, and the spacecraft beside it.
    const GameApi::M around = ev.matrix_api.mult(ev.matrix_api.yrot(static_cast<float>(aim_yaw + off_yaw)), ev.matrix_api.xrot(static_cast<float>(eye_pitch)));
    const GameApi::M look = zoom > 1.0 ? ev.matrix_api.mult(ev.matrix_api.yrot(static_cast<float>(aim_yaw)), ev.matrix_api.xrot(static_cast<float>(aim_pitch))) : around;
    ev.mainloop_api.clear_3d(0xff000000);
    // A camera is a longer lens: the whole picture that many times larger.
    const auto power = static_cast<float>(zoom);
    const GameApi::M lens = ev.matrix_api.scale(power, power, 1.0f);
    const int cam = zoom <= 1.0 ? 0 : zoom < 10.0 ? 1 : 2;
    ev.move_api.set_matrix(stars_mn[cam], ev.matrix_api.mult(ev.matrix_api.scale(kSky / kBall, kSky / kBall, kSky / kBall), c.sky));
    glDepthMask(GL_FALSE);  // the stars are behind everything and do not hide each other
    ev.mainloop_api.execute_ml(ev, stars[cam], sh, sh2, sh2, sh3, look, lens, look, w, h);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    ev.mainloop_api.execute_ml(ev, c.ml, sh, sh2, sh2, sh3, look, lens, look, w, h);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (craft)  // in the middle by eye; to the right of the camera's picture
      ev.mainloop_api.execute_ml(ev, probe, sh, sh2, sh2, sh3, around, ev.matrix_api.trans(zoom > 1.0 ? 0.15f * eye : 0.0f, 0.0f, -eye), around, w, h);
    std::size_t marked = focus;  // in the list: the body in view; following Voyager, the body it photographs, if it is one of the list
    if (shot) {
      marked = none;
      for (std::size_t i : c.flybys) if (c.bodies[i].name == shot->target || c.bodies[i].name + " Rings" == shot->target) marked = i;
    }
    if (ui_ok)
      draw_panels(ui, c, t, rate, paused, marked, zoom, show_data, show_map, paced ? c.played[std::min(c.played.size() - 1, static_cast<std::size_t>(t / c.step))] : -1.0, shot,
                  shot ? static_cast<std::size_t>(shot - c.shots.data()) + 1 : 0);
    ev.mainloop_api.swapbuffers();
    g_drawn = seconds_now();

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

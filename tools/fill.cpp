// Fills the parts of a moon's map that nobody has seen with surface made up to
// look like the parts that have been seen. A map here covers the whole globe,
// longitude across and latitude down; the unseen parts are black.
//
//   fill <craters 0..1> <seed> [<from> <to>] < map.ppm > filled.ppm       (binary PPM in and out)
//
// What is taken from the seen surface: how bright it is and how often (the made-up
// surface gets the same distribution of brightness, without its darkest and
// brightest ends: by default from the 4th to the 97th of a hundred, since the
// darkest of a map are shadows and the night's edge, not ground), its colours (each
// brightness gets the colour the seen surface has at that brightness), how rough
// it is (how much brightness changes over 2 and over 16 pixels gives the fractal
// dimension), and its brightness near the edge (the made-up surface starts from
// there and drifts to the average farther in).
//
// What is made up: fractal noise on the sphere, eight octaves, each finer octave
// weaker by the measured roughness; and, by the first argument, craters: their
// number falls with the square of their size, as on real cratered surfaces, and
// each is drawn as light falls on a bowl with a raised rim. The join is feathered
// over a few pixels. Nothing of the seen surface is changed, except a rim at its
// edge (an eightieth of the map's width), where maps made from pictures of a limb
// are smeared and in shadow.
//
// The result is a guess, not a map. Scratch tooling for the textures; C++20, no
// libraries.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct v3 { double x, y, z; };
v3 operator+(v3 a, v3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
v3 operator*(double k, v3 a) { return {k * a.x, k * a.y, k * a.z}; }
double dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
v3 unit(v3 a) { return (1.0 / std::sqrt(dot(a, a))) * a; }

std::uint32_t g_seed = 1;
double lattice(int x, int y, int z) {  // a fixed random number, 0 to 1, for each point of a grid
  std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + static_cast<std::uint32_t>(z) * 2147483647u + g_seed * 362437u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return h / 4294967295.0;
}
double noise(v3 p) {  // smooth between the grid's points
  const int x = static_cast<int>(std::floor(p.x)), y = static_cast<int>(std::floor(p.y)), z = static_cast<int>(std::floor(p.z));
  const auto s = [](double t) { return t * t * (3.0 - 2.0 * t); };
  const double fx = s(p.x - x), fy = s(p.y - y), fz = s(p.z - z);
  double v = 0.0;
  for (int k = 0; k < 8; ++k) {
    const int dx = k & 1, dy = (k >> 1) & 1, dz = k >> 2;
    v += lattice(x + dx, y + dy, z + dz) * (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy) * (dz ? fz : 1.0 - fz);
  }
  return v;
}
double fractal(v3 p, double weaker) {  // eight octaves; `weaker` is each octave's size against the one before
  double v = 0.0, a = 1.0, f = 3.0;
  for (int o = 0; o < 8; ++o, a *= weaker, f *= 2.0)  // the two coarsest held back: continents of one shade look made up
    v += (o == 0 ? 0.4 : o == 1 ? 0.7 : 1.0) * a * (noise(f * p + v3{17.0 * o, 31.0 * o, 5.0 * o}) - 0.5);
  return v;
}
double rnd() {
  g_seed = g_seed * 1664525u + 1013904223u;
  return (g_seed >> 8) / 16777216.0;
}

}  // namespace

int main(int argc, char** argv) {
  const double craters = argc > 1 ? std::atof(argv[1]) : 1.0;
  const std::uint32_t seed = argc > 2 ? static_cast<std::uint32_t>(std::atoi(argv[2])) : 1u;
  const double q_from = argc > 3 ? std::atof(argv[3]) : 0.04, q_to = argc > 4 ? std::atof(argv[4]) : 0.97;
  std::string magic;
  int w = 0, h = 0, top = 0;
  std::cin >> magic >> w >> h >> top;
  std::cin.get();
  if (magic != "P6" || top != 255 || w < 8 || h < 8) {
    std::fprintf(stderr, "fill: want a binary PPM with 8 bits a colour\n");
    return 1;
  }
  const std::size_t n = static_cast<std::size_t>(w) * h;
  std::vector<unsigned char> rgb(n * 3);
  std::cin.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
  const auto at = [&](int x, int y) { return static_cast<std::size_t>(y) * w + static_cast<std::size_t>(((x % w) + w) % w); };
  const auto light = [&](std::size_t i) { return (rgb[3 * i] + rgb[3 * i + 1] + rgb[3 * i + 2]) / 3.0; };

  // ---- the unseen: black, in patches of some size (a crater's shadow is not unseen) ----
  std::vector<char> unseen(n, 0);
  {
    std::vector<char> dark(n), done(n, 0);
    for (std::size_t i = 0; i < n; ++i) dark[i] = std::max({rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]}) <= 10;
    std::vector<std::size_t> patch;
    for (std::size_t i = 0; i < n; ++i) {
      if (!dark[i] || done[i]) continue;
      patch.assign(1, i);
      done[i] = 1;
      for (std::size_t k = 0; k < patch.size(); ++k) {
        const int x = static_cast<int>(patch[k] % w), y = static_cast<int>(patch[k] / w);
        const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
        for (int d = 0; d < 4; ++d) {
          if (ny[d] < 0 || ny[d] >= h) continue;
          const std::size_t j = at(nx[d], ny[d]);
          if (dark[j] && !done[j]) { done[j] = 1; patch.push_back(j); }
        }
      }
      if (patch.size() * 2500 >= n) for (std::size_t j : patch) unseen[j] = 1;  // 0.04 % of the globe or more
    }
    for (int round = 0; round < std::max(6, w / 80); ++round) {  // and the smeared, shadowed rim of the seen
      std::vector<char> grown = unseen;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          if (!unseen[at(x, y)] && (unseen[at(x - 1, y)] || unseen[at(x + 1, y)] || (y > 0 && unseen[at(x, y - 1)]) || (y + 1 < h && unseen[at(x, y + 1)]))) grown[at(x, y)] = 1;
      unseen = grown;
    }
  }
  std::size_t holes = 0;
  for (char u : unseen) holes += u;
  if (holes == 0 || holes == n) {
    std::fprintf(stderr, "fill: %s\n", holes ? "nothing seen" : "nothing unseen, map unchanged");
    std::printf("P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), stdout);
    return 0;
  }

  // ---- the seen: brightness, colour at each brightness, roughness ----
  std::vector<std::pair<double, std::size_t>> seen;
  for (std::size_t i = 0; i < n; ++i) if (!unseen[i]) seen.push_back({light(i), i});
  std::sort(seen.begin(), seen.end());
  double mean = 0.0;
  for (const auto& s : seen) mean += s.first;
  mean /= static_cast<double>(seen.size());
  const int shades = 64;
  double tint[shades][3] = {}, shade_light[shades] = {};
  for (int b = 0; b < shades; ++b) {
    const std::size_t from = seen.size() * b / shades, to = std::max(from + 1, seen.size() * (b + 1) / shades);
    for (std::size_t k = from; k < to; ++k) {
      for (int c = 0; c < 3; ++c) tint[b][c] += rgb[3 * seen[k].second + c];
      shade_light[b] += seen[k].first;
    }
    for (int c = 0; c < 3; ++c) tint[b][c] /= static_cast<double>(to - from);
    shade_light[b] /= static_cast<double>(to - from);
  }
  double change[2] = {0.0, 0.0};  // mean square change of brightness over 2 and over 16 pixels, up and down the map
  std::size_t pairs[2] = {0, 0};
  for (int y = 0; y + 16 < h; ++y)
    for (int x = 0; x < w; x += 2) {
      const int lag[2] = {2, 16};
      for (int k = 0; k < 2; ++k)
        if (!unseen[at(x, y)] && !unseen[at(x, y + lag[k])]) {
          const double d = light(at(x, y)) - light(at(x, y + lag[k]));
          change[k] += d * d;
          ++pairs[k];
        }
    }
  double rough = 0.5;  // the Hurst exponent: 0 rough like noise, 1 smooth
  if (pairs[0] > 100 && pairs[1] > 100 && change[0] > 0.0) rough = std::clamp(0.5 * std::log((change[1] / pairs[1]) / (change[0] / pairs[0])) / std::log(8.0), 0.3, 0.9);
  const double weaker = std::pow(2.0, -rough);

  // ---- made-up surface over the whole globe: fractal noise, and craters ----
  g_seed = seed;
  std::vector<double> made(n);
  std::vector<v3> where(n);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double lat = kPi / 2.0 - kPi * (y + 0.5) / h, lon = 2.0 * kPi * (x + 0.5) / w;
      where[at(x, y)] = {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
      made[at(x, y)] = fractal(where[at(x, y)], weaker);
    }
  double spread = 0.0;
  for (double v : made) spread += v * v;
  spread = std::sqrt(spread / static_cast<double>(n));
  if (craters > 0.0) {
    const int count = static_cast<int>(6000 * craters);
    for (int k = 0; k < count; ++k) {
      const double z = 2.0 * rnd() - 1.0, a = 2.0 * kPi * rnd();
      const v3 c{std::sqrt(1.0 - z * z) * std::cos(a), std::sqrt(1.0 - z * z) * std::sin(a), z};
      const double small = 0.004, large = 0.14;
      const double r = small / std::sqrt(1.0 - rnd() * (1.0 - small * small / (large * large)));  // as many again at half the size, times four
      const double reach = 1.7 * r, lat = std::asin(c.z), lon = std::atan2(c.y, c.x);
      const int y0 = std::max(0, static_cast<int>((kPi / 2.0 - lat - reach) / kPi * h)), y1 = std::min(h - 1, static_cast<int>((kPi / 2.0 - lat + reach) / kPi * h));
      for (int y = y0; y <= y1; ++y) {
        const double row = kPi / 2.0 - kPi * (y + 0.5) / h;
        const double wide = std::cos(row) > reach ? std::asin(std::min(1.0, reach / std::cos(row))) : kPi;
        const int xc = static_cast<int>(std::floor((lon < 0 ? lon + 2.0 * kPi : lon) / (2.0 * kPi) * w)), half = std::min(w / 2, static_cast<int>(wide / (2.0 * kPi) * w) + 1);
        for (int dx = -half; dx <= half; ++dx) {
          const std::size_t i = at(xc + dx, y);
          const v3 p = where[i];
          const double away = std::acos(std::clamp(dot(p, c), -1.0, 1.0)) / r;
          if (away > 1.7) continue;
          // light from the west on a bowl with a rim: the slope toward the light, along the line from the middle
          const v3 east = unit({-p.y, p.x, 1e-9});
          const double toward = dot(unit(c + (-dot(c, p)) * p + v3{1e-12, 0, 0}), east);  // +1: the middle lies to the east of here
          const double slope = away < 1.0 ? 2.0 * away : -4.0 * std::exp(-(away - 1.0) / 0.22);  // bowl rising outward; rim falling away outside
          made[i] += 0.55 * spread * slope * toward * std::min(1.0, 0.35 + r / 0.03);
        }
      }
    }
  }

  // ---- from every unseen pixel: the nearest seen one, and how far ----
  std::vector<int> from(n, -1);
  std::vector<float> far(n, 1e9f);
  for (std::size_t i = 0; i < n; ++i) if (!unseen[i]) { from[i] = static_cast<int>(i); far[i] = 0.0f; }
  const auto take = [&](std::size_t i, std::size_t j) {
    if (from[j] < 0) return;
    const int sx = from[j] % w, sy = from[j] / w, x = static_cast<int>(i % w), y = static_cast<int>(i / w);
    int dx = std::abs(sx - x);
    dx = std::min(dx, w - dx);
    const float d = std::sqrt(static_cast<float>(dx * dx + (sy - y) * (sy - y)));
    if (d < far[i]) { far[i] = d; from[i] = from[j]; }
  };
  for (int round = 0; round < 2; ++round) {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const std::size_t i = at(x, y);
        take(i, at(x - 1, y));
        if (y > 0) { take(i, at(x, y - 1)); take(i, at(x - 1, y - 1)); take(i, at(x + 1, y - 1)); }
      }
    for (int y = h - 1; y >= 0; --y)
      for (int x = w - 1; x >= 0; --x) {
        const std::size_t i = at(x, y);
        take(i, at(x + 1, y));
        if (y + 1 < h) { take(i, at(x, y + 1)); take(i, at(x - 1, y + 1)); take(i, at(x + 1, y + 1)); }
      }
  }
  // the seen surface's brightness around each of its pixels (a square of 25)
  std::vector<double> around(n, mean);
  {
    std::vector<double> sum(n, 0.0), cnt(n, 0.0), row_sum(n, 0.0), row_cnt(n, 0.0);
    const int r = 12;
    for (int y = 0; y < h; ++y) {
      double s = 0.0, c = 0.0;
      for (int x = -r; x <= r; ++x) if (!unseen[at(x, y)]) { s += light(at(x, y)); c += 1.0; }
      for (int x = 0; x < w; ++x) {
        row_sum[at(x, y)] = s, row_cnt[at(x, y)] = c;
        if (!unseen[at(x - r, y)]) { s -= light(at(x - r, y)); c -= 1.0; }
        if (!unseen[at(x + r + 1, y)]) { s += light(at(x + r + 1, y)); c += 1.0; }
      }
    }
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        double s = 0.0, c = 0.0;
        for (int dy = -r; dy <= r; ++dy) if (y + dy >= 0 && y + dy < h) { s += row_sum[at(x, y + dy)]; c += row_cnt[at(x, y + dy)]; }
        if (c > 0.0) around[at(x, y)] = s / c;
      }
  }

  // ---- the made-up surface gets the seen one's brightness, pixel for pixel in rank ----
  std::vector<std::pair<double, std::size_t>> order;
  for (std::size_t i = 0; i < n; ++i) if (unseen[i]) order.push_back({made[i], i});
  std::sort(order.begin(), order.end());
  const double edge = 12.0, drift = 0.08 * w;
  // near the edge the made-up surface is as bright as the seen one there: that
  // difference from the average, fading with distance, and smoothed along the edge
  std::vector<double> lift(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) if (unseen[i]) lift[i] = (around[static_cast<std::size_t>(from[i])] - mean) * std::exp(-far[i] / drift);
  for (int pass = 0; pass < 4; ++pass) {
    std::vector<double> next = lift;
    const int r = std::max(4, w / 120);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        if (!unseen[at(x, y)]) continue;
        double s = 0.0, c = 0.0;
        for (int d = -r; d <= r; ++d) {
          const std::size_t j = pass % 2 ? (y + d >= 0 && y + d < h ? at(x, y + d) : n) : at(x + d, y);
          if (j < n && unseen[j]) { s += lift[j]; c += 1.0; }
        }
        next[at(x, y)] = s / c;
      }
    lift = next;
  }
  for (std::size_t k = 0; k < order.size(); ++k) {
    const std::size_t i = order[k].second;
    const double q = q_from + (q_to - q_from) * (k + 0.5) / static_cast<double>(order.size());
    double l = seen[std::min(seen.size() - 1, static_cast<std::size_t>(q * static_cast<double>(seen.size())))].first;
    const std::size_t src = static_cast<std::size_t>(from[i]);
    l = std::clamp(l + lift[i], 0.0, 255.0);
    // the colour the seen surface has at this brightness
    const std::size_t rank = static_cast<std::size_t>(std::lower_bound(seen.begin(), seen.end(), std::make_pair(l, std::size_t{0})) - seen.begin());
    const int b = std::min(shades - 1, static_cast<int>(rank * shades / seen.size()));
    const double mix = std::clamp(far[i] / edge, 0.0, 1.0), blend = mix * mix * (3.0 - 2.0 * mix);
    for (int c = 0; c < 3; ++c) {
      const double colour = tint[b][c] * (shade_light[b] > 1.0 ? l / shade_light[b] : 1.0);
      rgb[3 * i + c] = static_cast<unsigned char>(std::clamp((1.0 - blend) * rgb[3 * src + c] + blend * colour, 0.0, 255.0));
    }
  }
  std::fprintf(stderr, "fill: %.0f %% of the globe made up; roughness %.2f; brightness %.0f of 255 on average\n", 100.0 * static_cast<double>(holes) / static_cast<double>(n), rough, mean);
  std::printf("P6\n%d %d\n255\n", w, h);
  std::fwrite(rgb.data(), 1, rgb.size(), stdout);
  return 0;
}

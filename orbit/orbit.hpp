// Orbit library: an ephemeris goes in, position and velocity come out.
//
// Three kinds of ephemeris, one result (orbit_state: position and velocity in the
// working frame, metres and metres per second):
//
//   kepler_ephemeris        the broadcast form of GPS, Galileo, BeiDou MEO and QZSS:
//                           Kepler elements, their rates and six harmonic terms
//                           (Galileo OS SIS ICD 5.1.1, IS-GPS-200 table 20-IV), with
//                           the two extra rates of the newer GPS messages.
//   state_vector_ephemeris  position and velocity at an epoch, carried forward by
//                           numerical integration (the GLONASS form; a likely form
//                           for low orbits). The force model is what the caller
//                           states: a central body with its J2 term or none, a frame
//                           that turns or not, any number of other moving masses
//                           (Sun, Moon, planets), each given by a table of where it
//                           is, and Einstein's correction where it is wanted.
//   table of orbit_sample   positions at known times (a precise orbit file, a
//                           prediction received over a link), interpolated.
//
// Nothing in it is tied to the Earth: the body and the frame are parameters
// (central_body). The same integrator carries a navigation satellite around the
// Earth in the Earth-fixed frame and a probe past Jupiter in a frame fixed to the
// solar system's centre of mass.
//
// Time is always "seconds since the ephemeris epoch", in the time scale of that
// ephemeris. The caller forms it (wrap_week() for week-based systems).
//
// Frame: the one the ephemeris is given in, and nothing is converted. For navigation
// satellites that is Earth-centred, Earth-fixed (ECEF): origin at the Earth's centre
// of mass, z along the rotation axis, x through the Greenwich meridian, turning with
// the Earth; GTRF for a Galileo broadcast ephemeris, WGS 84 for GPS, IGS20 for
// today's precise orbit files, which agree to a few centimetres. Velocity is the one
// seen in the working frame. A frame that turns does so at the constant rate
// central_body::rotation about z: no polar motion, precession or nutation, which is
// right for minutes and not for days. With rotation 0 the frame does not turn and
// can be any inertial one. No conversion between frames and none to latitude,
// longitude and height.
//
// On top of the state: sight() gives what a receiver at a known place sees (range,
// range rate, flight time, with Earth's rotation during the flight), and
// relativistic_clock() the periodic clock term that goes with an orbit.
//
// What it does not do: read navigation messages or files, the special BeiDou GEO
// case, open (hyperbolic) orbits in the Kepler form, drag, radiation pressure,
// thrust, gravity of the central body beyond J2. Relativity in the motion is an
// option of the integrated form (the Schwarzschild term of a mass). The
// accuracy of state_vector_ephemeris is set by the force model it is given and
// falls with the time span.
//
// No heap, no exceptions, no state. Double precision throughout.
#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace glass::orbit {

inline constexpr double kLightSpeed = 299'792'458.0;  // m/s
inline constexpr double kSecondsPerWeek = 604'800.0;

struct vec3 {
  double x = 0.0, y = 0.0, z = 0.0;
};
constexpr vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr vec3 operator*(double s, vec3 a) { return {s * a.x, s * a.y, s * a.z}; }
constexpr double dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr vec3 cross(vec3 a, vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double norm(vec3 a) { return std::sqrt(dot(a, a)); }

// Position and velocity in the working frame. The velocity is the one seen in that
// frame; velocity_inertial() adds the frame's rotation.
struct orbit_state {
  vec3 pos;
  vec3 vel;
};

// The body at the frame's origin and how the frame turns. All zero: no central body
// and a frame that does not turn (every mass is then an attractor, see below).
struct central_body {
  double gm = 0.0;        // m^3/s^2
  double rotation = 0.0;  // rad/s about z, of the frame against inertial space
  double radius = 0.0;    // equatorial radius used with j2, m
  double j2 = 0.0;        // the body's pole is the frame's z axis
  bool relativity = false;  // add the relativistic correction to this body's pull (integrated form only)
};
// The Earth with the constants each system's interface document prescribes; radius
// of WGS 84, J2 of EGM2008 (tide free).
inline constexpr central_body kEarthGalileo{3.986004418e14, 7.2921151467e-5, 6'378'137.0, 1.0826262e-3};
inline constexpr central_body kEarthGps{3.986005e14, 7.2921151467e-5, 6'378'137.0, 1.0826262e-3};

constexpr vec3 velocity_inertial(const orbit_state& s, const central_body& em) {
  return {s.vel.x - em.rotation * s.pos.y, s.vel.y + em.rotation * s.pos.x, s.vel.z};
}

// Seconds between two times of week, brought into half a week either side of zero.
constexpr double wrap_week(double dt) {
  if (dt > kSecondsPerWeek / 2) return dt - kSecondsPerWeek;
  if (dt < -kSecondsPerWeek / 2) return dt + kSecondsPerWeek;
  return dt;
}

// ---- Kepler elements with harmonic terms (broadcast ephemeris) ----

struct kepler_ephemeris {
  double toe;        // epoch in seconds of week: the node's longitude omega0 refers to
                     // the start of that week (0 if omega0 is given at the epoch)
  double sqrt_a;     // m^1/2
  double e;
  double m0;         // mean anomaly at the epoch, rad
  double delta_n;    // correction to the mean motion, rad/s
  double i0;         // inclination at the epoch, rad
  double idot;       // rad/s
  double omega0;     // longitude of the ascending node, rad
  double omega_dot;  // rad/s
  double omega;      // argument of perigee, rad
  double cuc = 0.0, cus = 0.0;  // rad
  double crc = 0.0, crs = 0.0;  // m
  double cic = 0.0, cis = 0.0;  // rad
  double a_dot = 0.0;           // m/s (newer GPS messages; 0 otherwise)
  double delta_n_dot = 0.0;     // rad/s^2
};

// Kepler's equation M = E - e sin E, by Newton's method.
inline double eccentric_anomaly(double m, double e) {
  double ea = m + e * std::sin(m);
  for (int i = 0; i < 20; ++i) {
    const double d = (ea - e * std::sin(ea) - m) / (1.0 - e * std::cos(ea));
    ea -= d;
    if (std::fabs(d) < 1e-14) break;
  }
  return ea;
}

// State `tk` seconds after the epoch.
inline orbit_state state_at(const kepler_ephemeris& k, double tk, const central_body& em) {
  const double a0 = k.sqrt_a * k.sqrt_a;
  const double a = a0 + k.a_dot * tk;
  const double n0 = std::sqrt(em.gm / (a0 * a0 * a0));
  const double m = k.m0 + (n0 + k.delta_n + 0.5 * k.delta_n_dot * tk) * tk;
  const double mdot = n0 + k.delta_n + k.delta_n_dot * tk;

  const double ea = eccentric_anomaly(m, k.e);
  const double se = std::sin(ea), ce = std::cos(ea);
  const double den = 1.0 - k.e * ce;
  const double root = std::sqrt(1.0 - k.e * k.e);
  const double nu = std::atan2(root * se, ce - k.e);
  const double phi = nu + k.omega;
  const double s2 = std::sin(2.0 * phi), c2 = std::cos(2.0 * phi);

  const double u = phi + k.cus * s2 + k.cuc * c2;
  const double r = a * den + k.crs * s2 + k.crc * c2;
  const double inc = k.i0 + k.idot * tk + k.cis * s2 + k.cic * c2;
  const double node = k.omega0 + (k.omega_dot - em.rotation) * tk - em.rotation * k.toe;

  const double su = std::sin(u), cu = std::cos(u);
  const double si = std::sin(inc), ci = std::cos(inc);
  const double sn = std::sin(node), cn = std::cos(node);
  const double xp = r * cu, yp = r * su;

  // rates (IS-GPS-200 table 20-IV, velocity part)
  const double eadot = mdot / den;
  const double nudot = eadot * root / den;
  const double udot = nudot + 2.0 * nudot * (k.cus * c2 - k.cuc * s2);
  const double rdot = a * k.e * se * eadot + k.a_dot * den + 2.0 * nudot * (k.crs * c2 - k.crc * s2);
  const double idot = k.idot + 2.0 * nudot * (k.cis * c2 - k.cic * s2);
  const double nodedot = k.omega_dot - em.rotation;
  const double xpdot = rdot * cu - r * udot * su;
  const double ypdot = rdot * su + r * udot * cu;

  orbit_state s;
  s.pos = {xp * cn - yp * ci * sn, xp * sn + yp * ci * cn, yp * si};
  s.vel = {-xp * nodedot * sn + xpdot * cn - ypdot * sn * ci - yp * (nodedot * cn * ci - idot * sn * si),
           xp * nodedot * cn + xpdot * sn + ypdot * cn * ci - yp * (nodedot * sn * ci + idot * cn * si),
           ypdot * si + yp * idot * ci};
  return s;
}

// ---- table of positions, interpolated ----

struct orbit_sample {
  double t;   // s, any origin, the same for the whole table
  vec3 pos;   // m
};

// State at time `t` from a table sorted by time: a polynomial through the `points`
// samples nearest to t (at most 16), velocity as its derivative. Nothing is
// returned outside the table or for a table of fewer than two samples.
inline std::optional<orbit_state> state_at(std::span<const orbit_sample> table, double t, std::size_t points = 10) {
  constexpr std::size_t kMax = 16;
  const std::size_t size = table.size();
  if (size < 2 || t < table.front().t || t > table.back().t) return std::nullopt;
  std::size_t n = points < 2 ? 2 : points;
  if (n > kMax) n = kMax;
  if (n > size) n = size;

  std::size_t lo = 0, hi = size;  // first sample at or after t
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (table[mid].t < t) lo = mid + 1; else hi = mid;
  }
  std::size_t first = lo > n / 2 ? lo - n / 2 : 0;
  if (first + n > size) first = size - n;

  double tau[kMax];  // sample times relative to t
  for (std::size_t j = 0; j < n; ++j) tau[j] = table[first + j].t - t;

  orbit_state s;
  for (std::size_t j = 0; j < n; ++j) {
    double w = 1.0;   // L_j(0)
    double dw = 0.0;  // L_j'(0)
    for (std::size_t m = 0; m < n; ++m) {
      if (m == j) continue;
      const double d = tau[j] - tau[m];
      if (d == 0.0) return std::nullopt;  // two samples at one time
      w *= -tau[m] / d;
    }
    for (std::size_t q = 0; q < n; ++q) {
      if (q == j) continue;
      double p = 1.0 / (tau[j] - tau[q]);
      for (std::size_t m = 0; m < n; ++m) {
        if (m == j || m == q) continue;
        p *= -tau[m] / (tau[j] - tau[m]);
      }
      dw += p;
    }
    s.pos = s.pos + w * table[first + j].pos;
    s.vel = s.vel + dw * table[first + j].pos;
  }
  return s;
}

// ---- state vector, integrated ----

// A mass that moves: its gravitational parameter and a table of where it is, in the
// frame and the time argument of the propagation (Sun and Moon for an Earth
// satellite, the planets for a probe).
struct attractor {
  double gm;                            // m^3/s^2
  std::span<const orbit_sample> path;
  bool relativity = false;              // add the relativistic correction to this mass's pull
};

// Einstein's correction to the pull of one mass, to first order in 1/c^2 (the
// Schwarzschild term): what turns Mercury's perihelion by 43 arc seconds a century.
// `r` and `v` are position and velocity relative to the mass, in a frame that does
// not turn.
inline vec3 relativistic_acceleration(double gm, vec3 r, vec3 v) {
  const double d = norm(r);
  const double k = gm / (kLightSpeed * kLightSpeed * d * d * d);
  return k * ((4.0 * gm / d - dot(v, v)) * r + (4.0 * dot(r, v)) * v);
}

struct state_vector_ephemeris {
  vec3 pos;           // m
  vec3 vel;           // m/s, as seen in the working frame
  vec3 acc;           // extra acceleration carried by the ephemeris (Sun, Moon), m/s^2; held constant
  double step = 10.0; // largest integration step, s
  double t0 = 0.0;    // the epoch in the time argument of the attractors' tables
  std::span<const attractor> attractors{};
};

// Acceleration in the working frame: central gravity with J2, the frame's rotation, extra.
inline vec3 orbit_acceleration(const orbit_state& s, vec3 extra, const central_body& em) {
  const double w = em.rotation;
  vec3 a{w * w * s.pos.x + 2.0 * w * s.vel.y + extra.x, w * w * s.pos.y - 2.0 * w * s.vel.x + extra.y, extra.z};
  if (em.gm != 0.0) {
    const double r2 = dot(s.pos, s.pos);
    const double mu_r3 = em.gm / (r2 * std::sqrt(r2));
    const double k = 1.5 * em.j2 * em.radius * em.radius / r2;
    const double z2 = 5.0 * s.pos.z * s.pos.z / r2;
    a.x -= mu_r3 * s.pos.x * (1.0 + k * (1.0 - z2));
    a.y -= mu_r3 * s.pos.y * (1.0 + k * (1.0 - z2));
    a.z -= mu_r3 * s.pos.z * (1.0 + k * (3.0 - z2));
    if (em.relativity) a = a + relativistic_acceleration(em.gm, s.pos, velocity_inertial(s, em));
  }
  return a;
}

// Pull of the attractors on a body in state `s` at table time `t`. With a central
// body the frame's origin is that body, which the attractors pull as well: that part
// is taken off (`indirect`). Nothing is returned outside an attractor's table. The
// relativistic correction of an attractor takes the frame as one that does not turn.
inline std::optional<vec3> attraction(std::span<const attractor> bodies, const orbit_state& s, double t, bool indirect) {
  vec3 a;
  for (const attractor& b : bodies) {
    const std::optional<orbit_state> p = state_at(b.path, t);
    if (!p) return std::nullopt;
    const vec3 d = p->pos - s.pos;
    const double r = norm(d);
    a = a + (b.gm / (r * r * r)) * d;
    if (indirect) {
      const double rb = norm(p->pos);
      a = a - (b.gm / (rb * rb * rb)) * p->pos;
    }
    if (b.relativity) a = a + relativistic_acceleration(b.gm, s.pos - p->pos, s.vel - p->vel);
  }
  return a;
}

// State `dt` seconds after (or before) the epoch, by fourth-order Runge-Kutta.
// Spans that would take more than 100 000 steps are refused; carry on from the
// result for longer ones.
inline std::optional<orbit_state> state_at(const state_vector_ephemeris& e, double dt, const central_body& em) {
  orbit_state s{e.pos, e.vel};
  if (dt == 0.0) return s;
  if (!(e.step > 0.0)) return std::nullopt;
  const double steps = std::ceil(std::fabs(dt) / e.step);
  if (!(steps <= 100000.0)) return std::nullopt;
  const int n = static_cast<int>(steps);
  const double h = dt / n;
  const bool indirect = em.gm != 0.0;
  const auto accel = [&](const orbit_state& a, double t) -> std::optional<vec3> {
    const vec3 own = orbit_acceleration(a, e.acc, em);
    if (e.attractors.empty()) return own;
    const std::optional<vec3> pull = attraction(e.attractors, a, t, indirect);
    if (!pull) return std::nullopt;
    return own + *pull;
  };
  const auto advance = [](const orbit_state& a, vec3 v, vec3 acc, double hh) {
    return orbit_state{a.pos + hh * v, a.vel + hh * acc};
  };
  for (int i = 0; i < n; ++i) {
    const double t = e.t0 + i * h;
    const auto a1 = accel(s, t);
    if (!a1) return std::nullopt;
    const orbit_state s2 = advance(s, s.vel, *a1, 0.5 * h);
    const auto a2 = accel(s2, t + 0.5 * h);
    if (!a2) return std::nullopt;
    const orbit_state s3 = advance(s, s2.vel, *a2, 0.5 * h);
    const auto a3 = accel(s3, t + 0.5 * h);
    if (!a3) return std::nullopt;
    const orbit_state s4 = advance(s, s3.vel, *a3, h);
    const auto a4 = accel(s4, t + h);
    if (!a4) return std::nullopt;
    s.pos = s.pos + (h / 6.0) * (s.vel + 2.0 * s2.vel + 2.0 * s3.vel + s4.vel);
    s.vel = s.vel + (h / 6.0) * (*a1 + 2.0 * *a2 + 2.0 * *a3 + *a4);
  }
  return s;
}

// ---- what a receiver sees ----

struct sighting {
  orbit_state sat;      // at emission, in the working frame as it stands at the reception instant
  double range;         // m, geometric
  double range_rate;    // m/s, for a receiver at rest in the working frame (first order in v/c)
  double flight_time;   // s
};

// The satellite as seen at time `t_rx` from the place `rx`, fixed in the working frame. `state` is
// any callable double -> orbit_state (or optional of it) in the caller's time
// argument, for example
//   sight([&](double tk) { return state_at(eph, tk, kEarthGalileo); }, tk_rx, rx, kEarthGalileo.rotation)
// The emission time is found by iteration; the frame turns by rotation * flight time
// while the signal is on its way.
template <class State>
inline std::optional<sighting> sight(State&& state, double t_rx, vec3 rx, double rotation) {
  sighting g{};
  double tau = 0.0;
  for (int i = 0; i < 6; ++i) {
    const std::optional<orbit_state> s = state(t_rx - tau);
    if (!s) return std::nullopt;
    const double ang = rotation * tau;
    const double c = std::cos(ang), sn = std::sin(ang);
    g.sat.pos = {c * s->pos.x + sn * s->pos.y, -sn * s->pos.x + c * s->pos.y, s->pos.z};
    g.sat.vel = {c * s->vel.x + sn * s->vel.y, -sn * s->vel.x + c * s->vel.y, s->vel.z};
    const vec3 los = g.sat.pos - rx;
    g.range = norm(los);
    g.range_rate = dot(g.sat.vel, los) / g.range;
    const double next = g.range / kLightSpeed;
    const bool done = std::fabs(next - tau) < 1e-13;
    tau = next;
    g.flight_time = tau;
    if (done) break;
  }
  return g;
}

// ---- clock terms that go with an orbit ----

// The periodic relativistic term of the satellite clock, seconds, to be added to
// the clock polynomial.
//
// From a Kepler ephemeris, exactly as the interface documents define it
// (F e sqrt(A) sin E): the form to use with a broadcast clock.
inline double relativistic_clock(const kepler_ephemeris& k, double tk, const central_body& em) {
  const double a0 = k.sqrt_a * k.sqrt_a;
  const double n0 = std::sqrt(em.gm / (a0 * a0 * a0));
  const double m = k.m0 + (n0 + k.delta_n + 0.5 * k.delta_n_dot * tk) * tk;
  const double f = -2.0 * std::sqrt(em.gm) / (kLightSpeed * kLightSpeed);
  return f * k.e * k.sqrt_a * std::sin(eccentric_anomaly(m, k.e));
}
// From a state of any kind (-2 r.v / c^2): the form for the other two kinds. For a
// Kepler ephemeris it differs from the one above by the effect of the harmonic
// terms, some tens of picoseconds for a navigation satellite.
constexpr double relativistic_clock(const orbit_state& s) {
  return -2.0 * dot(s.pos, s.vel) / (kLightSpeed * kLightSpeed);
}

struct clock_poly {
  double af0 = 0.0;  // s
  double af1 = 0.0;  // s/s
  double af2 = 0.0;  // s/s^2
};
// Satellite clock offset `tc` seconds after the clock epoch, without the relativistic term.
constexpr double clock_offset(const clock_poly& c, double tc) { return c.af0 + (c.af1 + c.af2 * tc) * tc; }

}  // namespace glass::orbit

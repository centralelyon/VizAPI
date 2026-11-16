// Searches for disjoint oriented network footprints inside a simple polygon.
// All geometry uses a local Cartesian frame. Transformations never change
// incidence.
#pragma once
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>
namespace mega {
using J = nlohmann::json;
using P = std::array<double, 2>;
using Box = std::array<double, 4>;
inline double orient(P a, P b, P c) {
  return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}
inline bool on(P a, P b, P p) {
  return std::abs(orient(a, b, p)) < 1e-10 &&
         (p[0] - a[0]) * (p[0] - b[0]) + (p[1] - a[1]) * (p[1] - b[1]) <= 1e-10;
}
inline bool hit(P a, P b, P c, P d) {
  return (orient(a, b, c) * orient(a, b, d) < 0 &&
          orient(c, d, a) * orient(c, d, b) < 0) ||
         on(a, b, c) || on(a, b, d) || on(c, d, a) || on(c, d, b);
}
inline bool inside(P p, const std::vector<P> &poly) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    auto a = poly[i], b = poly[j];
    if ((a[1] > p[1]) != (b[1] > p[1]) &&
        p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0])
      in = !in;
  }
  return in;
}
inline Box bounds(const std::vector<P> &p) {
  Box b{p[0][0], p[0][1], p[0][0], p[0][1]};
  for (auto q : p)
    for (int k = 0; k < 2; k++) {
      b[k] = std::min(b[k], q[k]);
      b[k + 2] = std::max(b[k + 2], q[k]);
    }
  return b;
}
inline bool fits(Box b, const std::vector<P> &poly) {
  P corners[] = {{b[0], b[1]}, {b[2], b[1]}, {b[2], b[3]}, {b[0], b[3]}};
  if (!inside(corners[0], poly))
    return false;
  for (size_t i = 0; i < poly.size(); i++)
    for (int k = 0; k < 4; k++)
      if (hit(poly[i], poly[(i + 1) % poly.size()], corners[k],
              corners[(k + 1) % 4]))
        return false;
  return true;
}
struct FixedFeature {
  std::vector<P> points;
  bool closed;
};
inline bool obstructed(const Box &b, const FixedFeature &f) {
  P corners[] = {{b[0], b[1]}, {b[2], b[1]}, {b[2], b[3]}, {b[0], b[3]}};
  if (f.closed && inside(corners[0], f.points))
    return true;
  for (size_t j = 0; j < f.points.size(); ++j) {
    auto q = f.points[j];
    if (q[0] >= b[0] && q[0] <= b[2] && q[1] >= b[1] && q[1] <= b[3])
      return true;
    if (j)
      for (int k = 0; k < 4; k++)
        if (hit(q, f.points[j - 1], corners[k], corners[(k + 1) % 4]))
          return true;
  }
  return false;
}
struct Placement {
  P center;
  double w = 0, h = 0, stretch = 1;
  bool rotated = false;
};
// Monotone separable warp on [-1/2, 1/2]. It fixes the sides of the
// footprint. The same map is applied to station, route and obstacle vertices.
inline P refine(const J &group, const Placement &placement) {
  auto nodes = group.at("points").get<std::vector<P>>();
  auto edges = group.at("edges").get<std::vector<std::array<int, 2>>>();
  auto bends = group.at("bends").get<std::vector<std::array<int, 3>>>();
  double best = std::numeric_limits<double>::infinity();
  P chosen{0, 0};
  for (double x : {0., -.10, .10})
    for (double y : {0., -.10, .10}) {
      auto points = nodes;
      for (auto &p : points) {
        p[0] += x * (1 - 4 * p[0] * p[0]);
        p[1] += y * (1 - 4 * p[1] * p[1]);
        if (placement.rotated)
          p = {-p[1], p[0]};
        p[0] *= placement.w;
        p[1] *= placement.h;
      }
      double length = 0;
      for (auto e : edges) {
        auto a = points[e[0]], b = points[e[1]];
        length += std::hypot(a[0] - b[0], a[1] - b[1]);
      }
      double mean = length / std::max<size_t>(1, edges.size()), crowding = 0,
             turn = 0;
      for (auto e : edges) {
        auto a = points[e[0]], b = points[e[1]];
        double d = std::hypot(a[0] - b[0], a[1] - b[1]);
        crowding += std::pow(std::max(0., 1. - d / std::max(mean, 1e-12)), 2);
      }
      for (auto b : bends) {
        auto a = points[b[0]], o = points[b[1]], c = points[b[2]];
        double dx = a[0] - o[0], dy = a[1] - o[1], ex = c[0] - o[0],
               ey = c[1] - o[1];
        double d = std::hypot(dx, dy) * std::hypot(ex, ey);
        if (d > 0)
          turn += 1 + (dx * ex + dy * ey) / d;
      }
      // Count proper intersections using an x sweep, excluding incidences.
      std::vector<unsigned> sorted(edges.size());
      for (unsigned j = 0; j < sorted.size(); ++j)
        sorted[j] = j;
      auto left = [&](unsigned j) {
        auto e = edges[j];
        return std::min(points[e[0]][0], points[e[1]][0]);
      };
      std::sort(sorted.begin(), sorted.end(),
                [&](unsigned a, unsigned b) { return left(a) < left(b); });
      unsigned crossings = 0;
      for (size_t j = 0; j < sorted.size(); ++j) {
        auto e = edges[sorted[j]];
        auto a = points[e[0]], b = points[e[1]];
        for (size_t k = j + 1;
             k < sorted.size() && left(sorted[k]) <= std::max(a[0], b[0]);
             ++k) {
          auto f = edges[sorted[k]];
          auto c = points[f[0]], d = points[f[1]];
          if (orient(a, b, c) * orient(a, b, d) < 0 &&
              orient(c, d, a) * orient(c, d, b) < 0)
            ++crossings;
        }
      }
      double cost = crossings + crowding / std::max<size_t>(1, edges.size()) +
                    .18 * turn / std::max<size_t>(1, bends.size()) +
                    .11 * (x * x + y * y);
      if (cost < best) {
        best = cost;
        chosen = {x, y};
      }
    }
  return chosen;
}
inline J pack(const J &req) {
  auto poly = req.at("polygon").get<std::vector<P>>();
  if (poly.size() < 3 || poly.size() > 512)
    throw std::invalid_argument("Container needs 3 to 512 vertices");
  auto extent = bounds(poly);
  double span = std::max(extent[2] - extent[0], extent[3] - extent[1]);
  if (!(span > 0))
    throw std::invalid_argument("Empty container");
  for (auto &p : poly)
    for (int k = 0; k < 2; k++)
      p[k] = (p[k] - extent[k]) / span;
  double area = 0;
  for (size_t i = 0; i < poly.size(); i++) {
    size_t j = (i + 1) % poly.size();
    if (poly[i] == poly[j])
      throw std::invalid_argument("Repeated container vertex");
    area += orient({0, 0}, poly[i], poly[j]);
    for (size_t k = i + 1; k < poly.size(); k++) {
      size_t l = (k + 1) % poly.size();
      if (j != k && l != i && hit(poly[i], poly[j], poly[k], poly[l]))
        throw std::invalid_argument("Container intersects itself");
    }
  }
  if (std::abs(area) < 1e-8)
    throw std::invalid_argument("Degenerate container");
  auto box = bounds(poly);
  auto groups = req.at("groups");
  const int n = groups.size();
  const auto method = req.at("method").get<std::string>();
  if (method != "relative" && method != "shape" && method != "graphviz")
    throw std::invalid_argument("Unknown layout method");
  // The legacy shape identifier now uses the geographic-order optimizer too.
  const bool baseline = method == "graphviz", relative = !baseline;
  std::vector<P> candidates;
  for (int x = 0; x < 25; x++)
    for (int y = 0; y < 25; y++) {
      P p{box[2] * (x + .5) / 25, box[3] * (y + .5) / 25};
      if (inside(p, poly))
        candidates.push_back(p);
    }
  std::vector<FixedFeature> fixed;
  for (const auto &f : req.value("fixedFeatures", J::array())) {
    auto points = f.at("points").get<std::vector<P>>();
    for (auto &p : points)
      for (int k = 0; k < 2; k++)
        p[k] = (p[k] - extent[k]) / span;
    fixed.push_back({std::move(points), f.at("closed")});
  }
  std::vector<Placement> placed(n);
  const double gap = req.value("margin", 0.01);
  // Revisit all placements to enlarge each footprint in the space left by its
  // neighbours.
  for (int sweep = 0; sweep < 3; sweep++)
    for (int i = 0; i < n; i++) {
      const double width = groups[i].at("width"),
                   height = groups[i].at("height");
      double best = -1e30;
      for (auto center : candidates) {
        bool orderOK = true;
        double drift = 0;
        for (int j = 0; j < n; j++)
          if (relative && j != i && placed[j].w > 0)
            for (int k = 0; k < 2; k++) {
              auto a = groups[i].at("geo"), b = groups[j].at("geo");
              if (a.is_null() || b.is_null())
                continue;
              double diff = center[k] - placed[j].center[k];
              if ((a[k + 2] < b[k] && diff >= 0) ||
                  (b[k + 2] < a[k] && diff <= 0))
                orderOK = false;
              if (diff * (a[k].get<double>() + a[k + 2].get<double>() -
                          b[k].get<double>() - b[k + 2].get<double>()) <
                  0)
                drift += .008;
            }
        if (!orderOK)
          continue;
        for (int turn = 0; turn <= (baseline ? 0 : req.value("rotate", false));
             turn++)
          for (double stretch : std::vector<double>{1., .75, 1.33}) {
            if ((baseline || req.value("rigid", false)) && stretch != 1)
              continue;
            double aspect = (turn ? height / width : width / height) * stretch,
                   aw = std::min(1., aspect), ah = std::min(1., 1. / aspect);
            double lo = 0, hi = sweep == 0 && !baseline
                                    ? std::min(1., 1.4 / std::sqrt(double(n)))
                                    : 1.;
            for (int step = 0; step < 11; step++) {
              double mid = (lo + hi) / 2, w = mid * aw, h = mid * ah;
              Box b{center[0] - w / 2 - gap, center[1] - h / 2 - gap,
                    center[0] + w / 2 + gap, center[1] + h / 2 + gap};
              bool ok = true;
              for (int j = 0; j < n; j++)
                if (i != j && placed[j].w > 0 &&
                    std::abs(center[0] - placed[j].center[0]) <
                        (w + placed[j].w) / 2 + gap &&
                    std::abs(center[1] - placed[j].center[1]) <
                        (h + placed[j].h) / 2 + gap) {
                  ok = false;
                  break;
                }
              if (ok)
                for (const auto &f : fixed)
                  if (obstructed(b, f)) {
                    ok = false;
                    break;
                  }
              if (ok && fits(b, poly))
                lo = mid;
              else
                hi = mid;
            }
            double w = lo * aw, h = lo * ah;
            if (std::min(w, h) < .0001)
              continue;
            double value =
                std::sqrt(w * h) / (1 + .12 * std::abs(std::log(stretch))) -
                drift;
            if (!sweep && relative && !groups[i]["target"].is_null()) {
              auto t = groups[i]["target"].get<P>();
              value -= .55 * std::hypot(center[0] - t[0] * box[2],
                                        center[1] - t[1] * box[3]);
            }
            if (value > best) {
              best = value;
              placed[i] = {center, w, h, stretch, bool(turn)};
            }
          }
      }
      if (best <= -1e29)
        throw std::invalid_argument(
            "No feasible packing at the requested margin. Enlarge or reshape "
            "the container.");
    }
  J result = J::array();
  for (int i = 0; i < n; i++) {
    auto r = placed[i];
    auto warp = (baseline || req.value("rigid", false)) ? P{0, 0}
                                                        : refine(groups[i], r);
    result.push_back(
        {{"center",
          {extent[0] + span * r.center[0], extent[1] + span * r.center[1]}},
         {"width", span * r.w},
         {"height", span * r.h},
         {"rotate", r.rotated},
         {"warp", warp}});
  }
  return {{"placements", result}};
}
} // namespace mega

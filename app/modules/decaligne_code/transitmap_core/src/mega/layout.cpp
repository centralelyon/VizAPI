#include "mega/layout.h"
#include "mega/packing.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>

namespace mega {
namespace {
using Strings = std::set<std::string>;
const J emptyArray = J::array(), emptyObject = J::object();
constexpr double pi = 3.14159265358979323846, earth = 6378137.;
std::string id(const J &j) {
  return j.is_string() ? j.get<std::string>() : j.dump();
}
std::string uid() {
  std::random_device r;
  std::ostringstream s;
  for (int i = 0; i < 4; ++i)
    s << std::hex << r();
  return s.str();
}
Strings strings(const J &j) {
  Strings s;
  for (auto &v : j)
    s.insert(id(v));
  return s;
}
P point(const J &j) { return {j.at(0).get<double>(), j.at(1).get<double>()}; }
P center(Box b) { return {(b[0] + b[2]) / 2, (b[1] + b[3]) / 2}; }
double distance(P a, P b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }
P project(P p, bool planar) {
  if (planar)
    return p;
  return {earth * p[0] * pi / 180,
          earth * std::log(std::tan(pi / 4 + std::clamp(p[1], -85.05, 85.05) *
                                                 pi / 360))};
}
P unproject(P p, bool planar) {
  if (planar)
    return p;
  return {p[0] / earth * 180 / pi,
          (2 * std::atan(std::exp(p[1] / earth)) - pi / 2) * 180 / pi};
}
P canvas(P p, const J &t, bool inverse = false) {
  double x = t.at("x"), y = t.at("y"), s = t.at("scale");
  return inverse ? P{x + (p[0] - 600) / s, y - (p[1] - 400) / s}
                 : P{600 + (p[0] - x) * s, 400 - (p[1] - y) * s};
}
P transform(P p, const J &t, bool inverse = false) {
  P a = point(t[0]), b = point(t[1]),
    size = t.size() > 5 ? point(t[5]) : P{1, 1},
    warp = t.size() > 5 ? point(t[6]) : P{0, 0};
  double sx = t[2], sy = t[3];
  bool rot = t[4];
  auto deform = [&](double v, int k) {
    double d = size[k], w = warp[k];
    if (w == 0 || std::abs(v) > d * .500001)
      return v;
    double z = v / d;
    return inverse ? d * 2 * (z - w) /
                         (1 + std::sqrt(std::max(0., 1 - 16 * w * (z - w))))
                   : d * (z + w * (1 - 4 * z * z));
  };
  if (inverse) {
    double x = (p[0] - b[0]) / sx, y = (p[1] - b[1]) / sy;
    if (rot) {
      double tmp = x;
      x = y;
      y = -tmp;
    }
    return {deform(x, 0) + a[0], deform(y, 1) + a[1]};
  }
  double x = deform(p[0] - a[0], 0), y = deform(p[1] - a[1], 1);
  if (rot) {
    double tmp = x;
    x = -y;
    y = tmp;
  }
  return {b[0] + sx * x, b[1] + sy * y};
}
J coordinates(const J &tree, const std::function<P(P)> &fn) {
  if (tree.empty())
    return tree;
  if (tree[0].is_number())
    return fn(point(tree));
  J out = emptyArray;
  for (auto &p : tree)
    out.push_back(coordinates(p, fn));
  return out;
}
// Geometry predicates use a local origin to avoid cancellation for Mercator
// coordinates.
double area(const std::vector<P> &p) {
  if (p.size() < 3)
    return 0;
  double a = 0;
  for (size_t i = 1; i + 1 < p.size(); ++i)
    a += orient(p[0], p[i], p[i + 1]);
  return std::abs(a) / 2;
}
bool boundary(P p, const std::vector<P> &poly) {
  for (size_t i = 0; i < poly.size(); ++i)
    if (on(poly[i], poly[(i + 1) % poly.size()], p))
      return true;
  return false;
}
bool covers(P p, const std::vector<P> &poly) {
  return boundary(p, poly) || inside(p, poly);
}
std::vector<double> cuts(P a, P b, const std::vector<P> &ring) {
  std::vector<double> ts{0, 1};
  P d{b[0] - a[0], b[1] - a[1]};
  double dd = d[0] * d[0] + d[1] * d[1];
  if (dd == 0)
    return ts;
  for (size_t i = 0; i < ring.size(); ++i) {
    P c = ring[i], e = ring[(i + 1) % ring.size()], v{e[0] - c[0], e[1] - c[1]},
      w{c[0] - a[0], c[1] - a[1]};
    double den = d[0] * v[1] - d[1] * v[0];
    if (std::abs(den) > 1e-16) {
      double t = (w[0] * v[1] - w[1] * v[0]) / den,
             u = (w[0] * d[1] - w[1] * d[0]) / den;
      if (t >= 0 && t <= 1 && u >= 0 && u <= 1)
        ts.push_back(t);
    } else if (on(a, b, c) || on(a, b, e)) {
      for (P q : {c, e}) {
        double t = ((q[0] - a[0]) * d[0] + (q[1] - a[1]) * d[1]) / dd;
        if (t > 0 && t < 1)
          ts.push_back(t);
      }
    }
  }
  std::sort(ts.begin(), ts.end());
  return ts;
}
P interpolate(P a, P b, double t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
}
bool coversLine(P a, P b, const std::vector<P> &poly) {
  if (!covers(a, poly) || !covers(b, poly))
    return false;
  auto ts = cuts(a, b, poly);
  for (size_t i = 1; i < ts.size(); ++i)
    if (!covers(interpolate(a, b, (ts[i - 1] + ts[i]) / 2), poly))
      return false;
  return true;
}
bool crosses(P a, P b, P c, P d) {
  return orient(a, b, c) * orient(a, b, d) < 0 &&
         orient(c, d, a) * orient(c, d, b) < 0;
}
void validatePolygon(const std::vector<P> &p, bool limitVertices = true) {
  if (p.size() < 3 || (limitVertices && p.size() > 512) || area(p) <= 0)
    throw std::invalid_argument(
        "Draw a valid container with 3 to 512 vertices.");
  for (size_t i = 0; i < p.size(); ++i) {
    if (distance(p[i], p[(i + 1) % p.size()]) < 1e-12)
      throw std::invalid_argument("Duplicate container vertex.");
    for (size_t j = i + 1; j < p.size(); ++j)
      if (j != i + 1 && !(i == 0 && j + 1 == p.size()) &&
          hit(p[i], p[(i + 1) % p.size()], p[j], p[(j + 1) % p.size()]))
        throw std::invalid_argument("Invalid container: self-intersection.");
  }
}
std::vector<P> hull(std::vector<P> p) {
  std::sort(p.begin(), p.end());
  p.erase(std::unique(p.begin(), p.end()), p.end());
  if (p.size() < 3)
    return p;
  std::vector<P> h;
  for (auto q : p) {
    while (h.size() > 1 && orient(h[h.size() - 2], h.back(), q) <= 0)
      h.pop_back();
    h.push_back(q);
  }
  size_t lo = h.size();
  for (auto i = p.rbegin() + 1; i != p.rend(); ++i) {
    while (h.size() > lo && orient(h[h.size() - 2], h.back(), *i) <= 0)
      h.pop_back();
    h.push_back(*i);
  }
  h.pop_back();
  return h;
}
double overlap(std::vector<P> a, const std::vector<P> &b) {
  if (a.size() < 3 || b.size() < 3)
    return 0;
  for (size_t i = 0; i < b.size() && !a.empty(); ++i) {
    P c = b[i], d = b[(i + 1) % b.size()];
    std::vector<P> out;
    P prev = a.back();
    double vp = orient(c, d, prev);
    for (P q : a) {
      double v = orient(c, d, q);
      if ((v >= 0) != (vp >= 0))
        out.push_back(interpolate(prev, q, vp / (vp - v)));
      if (v >= 0)
        out.push_back(q);
      prev = q;
      vp = v;
    }
    a = std::move(out);
  }
  return area(a);
}
bool polygonHit(P a, P b, const J &rings) {
  if (rings.empty())
    return false;
  std::vector<double> ts{0, 1};
  for (auto &ring : rings) {
    auto v = cuts(a, b, ring.get<std::vector<P>>());
    ts.insert(ts.end(), v.begin(), v.end());
  }
  std::sort(ts.begin(), ts.end());
  for (size_t i = 1; i < ts.size(); ++i) {
    if (ts[i] - ts[i - 1] < 1e-14)
      continue;
    P p = interpolate(a, b, (ts[i] + ts[i - 1]) / 2);
    auto outer = rings[0].get<std::vector<P>>();
    if (boundary(p, outer) || !inside(p, outer))
      continue;
    bool hole = false;
    for (size_t k = 1; k < rings.size(); ++k)
      if (covers(p, rings[k].get<std::vector<P>>()))
        hole = true;
    if (!hole)
      return true;
  }
  return false;
}
// Match crosses/within for a segment against a polyline, including internal
// polyline vertices and collinear coverage. Endpoint-only touches are allowed.
bool lineHit(P a, P b, const J &lines) {
  const P d{b[0] - a[0], b[1] - a[1]};
  const double dd = d[0] * d[0] + d[1] * d[1];
  if (dd == 0)
    return false;
  std::map<P, int> ends;
  for (const auto &line : lines)
    if (!line.empty()) {
      ++ends[point(line.front())];
      ++ends[point(line.back())];
    }
  bool crossing = false;
  std::vector<std::pair<double, double>> intervals;
  auto parameter = [&](P p) {
    return ((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1]) / dd;
  };
  for (const auto &line : lines)
    for (size_t i = 1; i < line.size(); ++i) {
      P c = point(line[i - 1]), e = point(line[i]);
      if (std::abs(orient(a, b, c)) < 1e-10 &&
          std::abs(orient(a, b, e)) < 1e-10) {
        double lo = std::max(0., std::min(parameter(c), parameter(e)));
        double hi = std::min(1., std::max(parameter(c), parameter(e)));
        if (hi > lo)
          intervals.emplace_back(lo, hi);
      } else {
        crossing |= crosses(a, b, c, e);
        for (P p : {c, e})
          if (on(a, b, p) && parameter(p) > 0 && parameter(p) < 1 &&
              ends[p] % 2 == 0)
            crossing = true;
      }
    }
  std::sort(intervals.begin(), intervals.end());
  double covered = 0;
  for (auto interval : intervals) {
    if (interval.first > covered + 1e-12)
      break;
    covered = std::max(covered, interval.second);
  }
  return covered >= 1 - 1e-12 || (intervals.empty() && crossing);
}
bool obstacleHit(P a, P b, const std::string &type, const J &tree) {
  if (type == "Polygon")
    return polygonHit(a, b, tree);
  if (type == "MultiPolygon") {
    for (auto &p : tree)
      if (polygonHit(a, b, p))
        return true;
  }
  if (type == "LineString")
    return lineHit(a, b, J::array({tree}));
  if (type == "MultiLineString")
    return lineHit(a, b, tree);
  return false;
}
struct Groups {
  std::map<std::string, std::vector<J>> nodes;
  size_t count = 0;
};
Groups components(const J &shape, const J &owners, const Strings &members) {
  std::map<std::string, std::string> parent;
  std::function<std::string(std::string)> root = [&](std::string n) {
    auto it = parent.emplace(n, n).first;
    if (it->second != n)
      it->second = root(it->second);
    return it->second;
  };
  auto selected = [&](const J &routes) {
    for (auto &r : routes)
      if (members.count(owners.value(id(r), std::string())))
        return true;
    return false;
  };
  for (auto &e : shape.at("segments"))
    if (selected(e.at("routeIds"))) {
      auto a = root(id(e["a"])), b = root(id(e["b"]));
      parent[b] = a;
    }
  std::map<std::string, std::vector<J>> parts;
  for (auto &n : shape.at("nodes"))
    if (parent.count(id(n["id"])))
      parts[root(id(n["id"]))].push_back(n);
  Groups g;
  g.count = parts.size();
  for (auto &entry : parts) {
    Strings cities;
    std::string first;
    for (auto &n : entry.second) {
      auto nid = id(n["id"]);
      if (first.empty() || nid < first)
        first = nid;
      for (auto &r : n["routeIds"]) {
        auto c = owners.value(id(r), std::string());
        if (members.count(c))
          cities.insert(c);
      }
    }
    if (cities.empty())
      continue;
    auto key = cities.size() == 1 ? *cities.begin() : "joined:" + first;
    auto &dest = g.nodes[key];
    dest.insert(dest.end(), entry.second.begin(), entry.second.end());
  }
  return g;
}
// Only fixed program names and generated temporary paths enter the command
// line. User data is written to DOT files, never interpolated into a shell
// command.
std::vector<P> graphviz(const J &records) {
  struct Temp {
    std::filesystem::path dir;
    Temp() {
      dir =
          std::filesystem::temp_directory_path() / ("transitmap-mega-" + uid());
      std::filesystem::create_directory(dir);
    }
    ~Temp() {
      std::error_code e;
      std::filesystem::remove_all(dir, e);
    }
  } temp;
  auto quote = [](const std::filesystem::path &p) {
    std::string q = "'";
    for (char c : p.string())
      q += c == '\'' ? "'\\''" : std::string(1, c);
    return q + "'";
  };
  auto run = [&](std::string args, std::string in, std::string out) {
    auto command = args + " < " + quote(temp.dir / in) + " > " +
                   quote(temp.dir / out) + " 2> " + quote(temp.dir / "error");
    const int status = std::system(command.c_str());
    if (status != 0) {
      std::ifstream error(temp.dir / "error");
      std::string detail((std::istreambuf_iterator<char>(error)),
                         std::istreambuf_iterator<char>());
      if (detail.size() > 1200)
        detail = detail.substr(detail.size() - 1200);
      const auto program = args.substr(0, args.find(' '));
      if (status == 127 * 256 || detail.find("not found") != std::string::npos)
        throw std::invalid_argument(
            "Graphviz executable missing: " + program +
            ". Install the graphviz package in the VizAPI runtime Docker "
            "image, then rebuild and recreate the container.");
      throw std::invalid_argument("Graphviz command failed (" + args +
                                  ", status " + std::to_string(status) +
                                  "): " + detail);
    }
  };
  double unit = 0;
  for (auto &r : records)
    unit = std::max(
        unit, std::max(r["width"].get<double>(), r["height"].get<double>()));
  unit /= 5;
  std::ofstream all(temp.dir / "all.dot");
  for (size_t i = 0; i < records.size(); ++i) {
    std::ofstream f(temp.dir / "input.dot");
    f.precision(17);
    f << "graph g" << i << " {n" << i
      << " [pos=\"0,0!\",shape=box,label=\"\",fixedsize=true,width="
      << std::max(.02, records[i]["width"].get<double>() / unit)
      << ",height=" << std::max(.02, records[i]["height"].get<double>() / unit)
      << "]; }";
    f.close();
    run("neato -n2 -Tdot", "input.dot", "one.dot");
    std::ifstream one(temp.dir / "one.dot");
    all << one.rdbuf() << '\n';
  }
  all.close();
  run("gvpack -g -m8", "all.dot", "packed.dot");
  run("neato -n2 -Tplain", "packed.dot", "plain");
  std::ifstream f(temp.dir / "plain");
  std::string line;
  std::vector<P> out(records.size());
  size_t count = 0;
  while (std::getline(f, line)) {
    std::istringstream s(line);
    std::string tag, name;
    double x, y;
    s >> tag;
    if (tag == "node" && s >> name >> x >> y) {
      out.at(std::stoul(name.substr(1))) = {x * unit, y * unit};
      ++count;
    }
  }
  if (count != records.size())
    throw std::invalid_argument("Incomplete Graphviz packing result.");
  return out;
}
void optimize(J &state, const J &body, bool afterOcti) {
  auto start = std::chrono::steady_clock::now();
  auto &meta = state.at("transitNetworks");
  J prev = meta.value("composite", emptyObject);
  auto t = state.at("transform");
  J memberList = body.value("members", prev.value("members", emptyArray));
  Strings members = strings(memberList), loaded;
  for (auto &c : meta.at("cities"))
    loaded.insert(id(c["id"]));
  if (members.size() < 2 || !std::includes(loaded.begin(), loaded.end(),
                                           members.begin(), members.end()))
    throw std::invalid_argument("Select at least two loaded networks.");
  auto method =
      body.value("method", prev.value("method", std::string("relative")));
  // Migrate the former unconstrained mode when reopening older maps.
  if (method == "shape")
    method = "relative";
  if (method != "relative" && method != "graphviz")
    throw std::invalid_argument("Unknown layout method.");
  auto kind = body.value("kind", prev.value("kind", std::string("polygon")));
  std::vector<P> pts;
  if (body.contains("points")) {
    for (auto &p : body["points"])
      pts.push_back(canvas(point(p), t, true));
    if ((kind == "rectangle" || kind == "circle") && pts.size() == 2) {
      P a = pts[0], b = pts[1];
      if (kind == "rectangle")
        pts = {a, {b[0], a[1]}, b, {a[0], b[1]}};
      else {
        double radius = distance(a, b);
        pts.clear();
        for (int i = 0; i < 64; ++i)
          pts.push_back({a[0] + radius * std::cos(i * 2 * pi / 64),
                         a[1] + radius * std::sin(i * 2 * pi / 64)});
      }
    }
  } else
    pts = prev.value("boundary", std::vector<P>{});
  if (pts.size() > 2 && pts.front() == pts.back())
    pts.pop_back();
  validatePolygon(pts, false);
  // Freehand points are simplified only when removal preserves the simple
  // polygon.
  if (kind == "freehand" && pts.size() > 3) {
    double tolerance = std::sqrt(area(pts)) * .002;
    for (size_t i = 0; i < pts.size() && pts.size() > 3;) {
      P a = pts[(i + pts.size() - 1) % pts.size()],
        b = pts[(i + 1) % pts.size()], q = pts[i];
      double d = distance(a, b);
      double u = d ? std::clamp(((q[0] - a[0]) * (b[0] - a[0]) +
                                 (q[1] - a[1]) * (b[1] - a[1])) /
                                    (d * d),
                                0., 1.)
                   : 0;
      if (distance(q, interpolate(a, b, u)) < tolerance) {
        auto trial = pts;
        trial.erase(trial.begin() + i);
        try {
          validatePolygon(trial, false);
          pts = std::move(trial);
          continue;
        } catch (const std::invalid_argument &) {
        }
      }
      ++i;
    }
  }
  validatePolygon(pts);
  auto &shape = state.at("shape");
  auto owners = meta.at("routes");
  auto groups = components(shape, owners, members);
  if (groups.nodes.empty())
    throw std::invalid_argument("Selected networks contain no routes.");
  J old = prev.value("transforms", emptyObject),
    last = prev.value("nodeGroups", emptyObject), nodegroups = emptyObject;
  Strings merged;
  for (auto &[k, ns] : groups.nodes) {
    Strings keys;
    for (auto &n : ns) {
      auto nid = id(n["id"]);
      if (last.contains(nid))
        keys.insert(last[nid]);
      nodegroups[nid] = k;
    }
    if (keys.size() > 1)
      merged.insert(k);
  }
  std::map<std::string, P> src;
  std::map<std::string, Strings> citykeys;
  bool planar = state.value("planar", false);
  for (auto &[k, ns] : groups.nodes)
    for (auto &n : ns) {
      auto nid = id(n["id"]), prior = last.value(nid, k);
      P p{n["x"], n["y"]};
      src[nid] = old.contains(prior) && !afterOcti && !merged.count(k)
                     ? transform(p, old[prior], true)
                     : p;
      for (auto &r : n["routeIds"])
        citykeys[owners.value(id(r), std::string())].insert(k);
    }
  J geo = prev.value("geography", emptyObject);
  std::map<std::string, std::vector<P>> refs;
  for (auto &f :
       state.value("original", emptyObject).value("features", emptyArray))
    if (f["geometry"]["type"] == "LineString")
      for (auto &l : f["properties"].value("lines", emptyArray)) {
        auto city = owners.value(id(l["id"]), std::string());
        if (members.count(city))
          for (auto &p : f["geometry"]["coordinates"])
            refs[city].push_back(project(point(p), planar));
      }
  for (auto &[city, ps] : refs)
    if (!geo.contains(city) && !ps.empty())
      geo[city] = bounds(ps);
  struct Obstacle {
    size_t index;
    std::string key, type;
    J tree;
  };
  std::vector<Obstacle> obstacles;
  std::map<std::string, std::vector<P>> local;
  for (auto &[k, ns] : groups.nodes)
    for (auto &n : ns)
      local[k].push_back(src[id(n["id"])]);
  auto &features = state["obstacles"]["features"];
  for (size_t i = 0; i < features.size(); ++i) {
    auto &f = features[i];
    auto prop = f.value("properties", emptyObject);
    Strings associated;
    if (prop.value("scope", std::string()) != "global") {
      associated = strings(prop.value("networkIds", emptyArray));
      if (associated.empty() && prop.contains("networkId"))
        associated.insert(id(prop["networkId"]));
    }
    bool selected = associated.empty();
    Strings keys;
    for (auto &c : associated) {
      selected |= members.count(c) > 0;
      keys.insert(citykeys[c].begin(), citykeys[c].end());
    }
    if (!selected)
      continue;
    std::string k = keys.size() == 1 ? *keys.begin() : "";
    J tree = coordinates(f["geometry"]["coordinates"], [&](P p) {
      P q = project(p, planar);
      if (old.contains(k) && !afterOcti)
        q = transform(q, old[k], true);
      if (!k.empty())
        local[k].push_back(q);
      return q;
    });
    obstacles.push_back({i, k, f["geometry"]["type"], tree});
  }
  J records = emptyArray;
  for (auto &[k, ns] : groups.nodes) {
    auto bbox = bounds(local[k]);
    std::vector<P> locations;
    for (auto &c : members)
      if (citykeys[c].count(k) && geo.contains(c)) {
        Box b = geo[c].get<Box>();
        locations.push_back({b[0], b[1]});
        locations.push_back({b[2], b[3]});
      }
    double w = bbox[2] - bbox[0], h = bbox[3] - bbox[1],
           eps = std::max({w, h, 1.}) * .025;
    J r = {{"key", k},
           {"source", center(bbox)},
           {"width", std::max(eps, w)},
           {"height", std::max(eps, h)},
           {"geo",
            locations.empty() || planar ? J(nullptr) : J(bounds(locations))},
           {"target", nullptr},
           {"points", emptyArray},
           {"edges", emptyArray},
           {"bends", emptyArray}};
    std::map<std::string, int> index;
    P c = point(r["source"]);
    for (auto &n : ns) {
      auto nid = id(n["id"]);
      index[nid] = index.size();
      P p = src[nid];
      r["points"].push_back(P{(p[0] - c[0]) / r["width"].get<double>(),
                              (p[1] - c[1]) / r["height"].get<double>()});
    }
    std::map<std::pair<std::string, int>, std::vector<int>> nbr;
    for (auto &e : shape["segments"]) {
      auto a = id(e["a"]), b = id(e["b"]);
      if (!index.count(a) || !index.count(b))
        continue;
      int i = index[a], j = index[b];
      r["edges"].push_back({i, j});
      for (auto &route : e["routeIds"]) {
        nbr[{id(route), i}].push_back(j);
        nbr[{id(route), j}].push_back(i);
      }
    }
    for (auto &[key, vs] : nbr)
      if (vs.size() == 2)
        r["bends"].push_back({vs[0], key.second, vs[1]});
    records.push_back(r);
  }
  std::vector<P> cs;
  for (auto &r : records)
    if (!r["geo"].is_null())
      cs.push_back(center(r["geo"].get<Box>()));
  if (!cs.empty()) {
    Box b = bounds(cs);
    for (auto &r : records)
      if (!r["geo"].is_null()) {
        P c = center(r["geo"].get<Box>()), target;
        for (int i = 0; i < 2; ++i)
          target[i] = b[i + 2] != b[i]
                          ? .15 + .7 * (c[i] - b[i]) / (b[i + 2] - b[i])
                          : .5;
        r["target"] = target;
      }
  }
  bool rotate = body.value("rotate", prev.value("rotate", false));
  Box ext = bounds(pts);
  double span = std::max(ext[2] - ext[0], ext[3] - ext[1]), radius = 6;
  J symbolStyles =
      body.value("symbolStyles", prev.value("symbolStyles", emptyArray));
  auto number = [](const J &values, const std::string &key, double def) {
    if (!values.contains(key))
      return def;
    auto &v = values[key];
    return v.is_number() ? v.get<double>() : std::stod(v.get<std::string>());
  };
  for (auto &st : symbolStyles) {
    J overrides = st.value("overrides", emptyArray);
    overrides.push_back(emptyObject);
    for (auto &over : overrides) {
      J values = st.value("values", emptyObject);
      values.update(over);
      for (std::string prefix : {"normal-", "interchange-"}) {
        auto num = [&](std::string key, double def) {
          return number(values, prefix + key, def);
        };
        double outer =
            values.value(prefix + "shape", std::string()) == "rectangle"
                ? std::hypot(num("width", 8), num("height", 8)) / 2
                : num("radius", 4);
        radius = std::max(
            {radius, outer + num("outline-width", 1.5) / 2,
             num("glyph-radius", 2),
             std::hypot(num("glyph-width", 4), num("glyph-height", 4)) / 2});
      }
    }
    if (st.value("labels", false)) {
      size_t longest = 0;
      for (auto &[k, ns] : groups.nodes)
        for (auto &n : ns)
          for (auto &route : n["routeIds"])
            if (owners.value(id(route), std::string()) == id(st["id"])) {
              auto name = n.value("name", std::string());
              size_t len = 0;
              for (unsigned char c : name)
                if ((c & 0xc0) != 0x80)
                  ++len;
              longest = std::max(longest, len);
            }
      radius = std::max(radius, 10 + .65 * longest *
                                         number(st.value("values", emptyObject),
                                                "label-size", 8));
    }
  }
  for (auto &style : state.value("styles", emptyObject)) {
    auto rs = style.value("routes", emptyObject);
    for (auto it = rs.begin(); it != rs.end(); ++it)
      if (members.count(owners.value(it.key(), std::string())))
        radius = std::max(radius, it.value().value("width", 0.) / 2);
  }
  double margin =
      std::max(.012, radius * std::max(body.value("spacingScale", 1.) /
                                           t["scale"].get<double>() / span,
                                       1.65 / 1200));
  J fixed = emptyArray;
  for (auto &o : obstacles)
    if (o.key.empty()) {
      auto add = [&](const J &ps, bool closed) {
        fixed.push_back({{"points", ps}, {"closed", closed}});
      };
      if (o.type == "Polygon")
        add(o.tree[0], true);
      if (o.type == "LineString")
        add(o.tree, false);
      if (o.type == "MultiLineString")
        for (auto &line : o.tree)
          add(line, false);
      if (o.type == "MultiPolygon")
        for (auto &p : o.tree)
          add(p[0], true);
    }
  J placements;
  if (method == "graphviz") {
    auto centers = graphviz(records);
    std::vector<P> corners;
    for (size_t i = 0; i < records.size(); ++i)
      for (int s : {-1, 1})
        corners.push_back(
            {centers[i][0] + s * records[i]["width"].get<double>() / 2,
             centers[i][1] + s * records[i]["height"].get<double>() / 2});
    Box b = bounds(corners);
    double w = b[2] - b[0], h = b[3] - b[1];
    auto place = pack({{"groups", J::array({{{"width", w}, {"height", h}}})},
                       {"polygon", pts},
                       {"method", method},
                       {"margin", margin},
                       {"fixedFeatures", fixed}})["placements"][0];
    double scale = std::min(place["width"].get<double>() / w,
                            place["height"].get<double>() / h);
    P pc = point(place["center"]), bc = center(b);
    placements = emptyArray;
    for (size_t i = 0; i < records.size(); ++i)
      placements.push_back(
          {{"center", P{pc[0] + scale * (centers[i][0] - bc[0]),
                        pc[1] + scale * (centers[i][1] - bc[1])}},
           {"width", records[i]["width"].get<double>() * scale},
           {"height", records[i]["height"].get<double>() * scale},
           {"rotate", false}});
  } else
    placements = pack({{"groups", records},
                       {"polygon", pts},
                       {"method", method},
                       {"rotate", rotate},
                       {"margin", margin},
                       {"rigid", afterOcti},
                       {"fixedFeatures", fixed}})["placements"];
  J transforms = emptyObject;
  for (size_t i = 0; i < records.size(); ++i) {
    auto &r = records[i];
    auto &p = placements[i];
    double w = r["width"], h = r["height"];
    if (p["rotate"].get<bool>())
      std::swap(w, h);
    transforms[r["key"].get<std::string>()] =
        J::array({r["source"], p["center"], p["width"].get<double>() / w,
                  p["height"].get<double>() / h, p["rotate"],
                  J::array({r["width"], r["height"]}),
                  p.value("warp", J::array({0, 0}))});
  }
  std::map<std::string, P> dst;
  for (auto &[n, p] : src)
    dst[n] = transform(p, transforms[nodegroups[n].get<std::string>()]);
  J edges = emptyArray;
  for (auto &e : shape["segments"])
    if (dst.count(id(e["a"])) && dst.count(id(e["b"])))
      edges.push_back(e);
  for (auto &e : edges)
    if (!coversLine(dst[id(e["a"])], dst[id(e["b"])], pts))
      throw std::invalid_argument(
          "Packing would place a segment outside the container.");
  // X sweep prunes disjoint segment pairs before exact crossing tests.
  struct Segment {
    P a, b;
    Box box;
  };
  std::vector<Segment> segments;
  for (auto &e : edges) {
    P a = dst[id(e["a"])], b = dst[id(e["b"])];
    segments.push_back({a, b, bounds({a, b})});
  }
  std::sort(segments.begin(), segments.end(),
            [](auto &a, auto &b) { return a.box[0] < b.box[0]; });
  int crossing = 0;
  for (size_t i = 0; i < segments.size(); ++i)
    for (size_t j = i + 1;
         j < segments.size() && segments[j].box[0] <= segments[i].box[2]; ++j)
      if (segments[j].box[1] <= segments[i].box[3] &&
          segments[i].box[1] <= segments[j].box[3] &&
          crosses(segments[i].a, segments[i].b, segments[j].a, segments[j].b))
        ++crossing;
  std::vector<std::vector<P>> hulls;
  std::vector<P> stations;
  for (auto &[k, ns] : groups.nodes) {
    std::vector<P> ps;
    for (auto &n : ns) {
      P p = dst[id(n["id"])];
      ps.push_back(p);
      if (n.value("isStation", false))
        stations.push_back(p);
    }
    hulls.push_back(hull(ps));
  }
  double totalArea = 0, totalOverlap = 0;
  for (size_t i = 0; i < hulls.size(); ++i) {
    totalArea += area(hulls[i]);
    for (size_t j = i + 1; j < hulls.size(); ++j)
      totalOverlap += overlap(hulls[i], hulls[j]);
  }
  double spacing = 0;
  if (stations.size() > 1) {
    std::sort(stations.begin(), stations.end());
    spacing = distance(stations[0], stations[1]);
    for (size_t i = 0; i < stations.size(); ++i)
      for (size_t j = i + 1;
           j < stations.size() && stations[j][0] - stations[i][0] <= spacing;
           ++j)
        spacing = std::min(spacing, distance(stations[i], stations[j]));
  }
  std::map<std::pair<std::string, std::string>, std::vector<std::string>> adj;
  for (auto &e : edges)
    for (auto &rid : e["routeIds"]) {
      auto a = id(e["a"]), b = id(e["b"]);
      adj[{id(rid), a}].push_back(b);
      adj[{id(rid), b}].push_back(a);
    }
  int bends = 0;
  for (auto &[key, nb] : adj)
    if (nb.size() == 2) {
      P a = dst[nb[0]], b = dst[nb[1]], c = dst[key.second];
      double dx = a[0] - c[0], dy = a[1] - c[1], ex = b[0] - c[0],
             ey = b[1] - c[1];
      if (dx * ex + dy * ey > -.999 * std::hypot(dx, dy) * std::hypot(ex, ey))
        ++bends;
    }
  int obstacleHits = 0;
  for (auto &o : obstacles) {
    auto &f = features[o.index];
    J mapped = o.key.empty() ? o.tree : coordinates(o.tree, [&](P p) {
      return transform(p, transforms[o.key]);
    });
    auto prop = f.value("properties", emptyObject);
    Strings associated = strings(prop.value("networkIds", emptyArray));
    if (associated.empty() && prop.contains("networkId"))
      associated.insert(id(prop["networkId"]));
    for (auto &e : edges) {
      bool selected = associated.empty(), topology = false;
      for (auto &r : e["routeIds"]) {
        selected |= associated.count(owners.value(id(r), std::string())) > 0;
        topology |= id(r) == prop.value("topologyRouteId", std::string());
      }
      if (!selected || topology)
        continue;
      auto a = id(e["a"]), b = id(e["b"]);
      if (obstacleHit(dst[a], dst[b], o.type, mapped)) {
        ++obstacleHits;
        if (!obstacleHit(src[a], src[b], o.type, o.tree))
          throw std::invalid_argument(
              "Layout introduces a geographic obstacle violation.");
      }
    }
    if (!o.key.empty())
      f["geometry"]["coordinates"] =
          coordinates(mapped, [&](P p) { return unproject(p, planar); });
  }
  int ordering = 0;
  for (size_t i = 0; i < records.size(); ++i)
    for (size_t j = 0; j < records.size(); ++j)
      if (!records[i]["geo"].is_null() && !records[j]["geo"].is_null())
        for (int k = 0; k < 2; ++k)
          if (records[i]["geo"][k + 2].get<double>() <
                  records[j]["geo"][k].get<double>() &&
              placements[i]["center"][k].get<double>() >=
                  placements[j]["center"][k].get<double>())
            ++ordering;
  if (method == "relative" && ordering)
    throw std::invalid_argument(
        "Geographic ordering is infeasible for this container.");
  J geographicNodes = prev.value("geographicNodes", emptyObject);
  for (auto &f :
       state.value("original", emptyObject).value("features", emptyArray))
    if (f["geometry"]["type"] == "Point") {
      auto nid = id(f["properties"]["id"]);
      if (dst.count(nid) && !geographicNodes.contains(nid))
        geographicNodes[nid] = f["geometry"]["coordinates"];
    }
  for (auto &n : shape["nodes"]) {
    auto nid = id(n["id"]);
    if (dst.count(nid)) {
      n["x"] = dst[nid][0];
      n["y"] = dst[nid][1];
    }
  }
  std::string containerId = body.value("containerId", std::string());
  if (containerId.empty() && !body.contains("points"))
    containerId = prev.value("containerId", std::string());
  if (containerId.empty())
    containerId = uid();
  meta["containers"][containerId] = {{"kind", kind}, {"boundary", pts}};
  double deformation = 0;
  for (auto &tr : transforms)
    deformation +=
        std::abs(std::log(tr[2].get<double>() / tr[3].get<double>())) +
        std::hypot(tr[6][0].get<double>(), tr[6][1].get<double>());
  J metrics = {
      {"utilization", totalArea / area(pts)},
      {"crossings", crossing},
      {"bends", bends},
      {"minimumStationSpacing", spacing},
      {"interNetworkOverlap", totalOverlap},
      {"boundaryViolations", 0},
      {"orderingViolations", ordering},
      {"deformation", deformation / transforms.size()},
      {"obstacleViolations", obstacleHits},
      {"runtimeMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count()}};
  meta["composite"] = {{"symbolStyles", symbolStyles},
                       {"containerId", containerId},
                       {"geographicNodes", geographicNodes},
                       {"id", prev.value("id", uid())},
                       {"name", "MegaMetroMap"},
                       {"members", memberList},
                       {"method", method},
                       {"kind", kind},
                       {"boundary", pts},
                       {"geography", geo},
                       {"transforms", transforms},
                       {"nodeGroups", nodegroups},
                       {"rotate", rotate},
                       {"componentCount", groups.count},
                       {"metrics", metrics}};
  state["shapeRevision"] = state.value("shapeRevision", 0) + 1;
  state.erase("shapeTraversals");
}
void remove(J &state, const std::string &containerId) {
  auto &meta = state.at("transitNetworks");
  J c = meta.value("composite", emptyObject);
  if (!containerId.empty() &&
      containerId != c.value("containerId", std::string())) {
    if (meta.contains("containers"))
      meta["containers"].erase(containerId);
    return;
  }
  if (c.empty())
    return;
  meta.erase("composite");
  if (meta.contains("containers"))
    meta["containers"].erase(c.value("containerId", std::string()));
  auto groups = c.at("nodeGroups"), ts = c.at("transforms");
  for (auto &n : state["shape"]["nodes"]) {
    auto k = groups.value(id(n["id"]), std::string());
    if (ts.contains(k)) {
      P p = transform({n["x"], n["y"]}, ts[k], true);
      n["x"] = p[0];
      n["y"] = p[1];
    }
  }
  bool planar = state.value("planar", false);
  std::map<std::string, Strings> citykeys;
  auto owners = meta.at("routes");
  for (auto &n : state["shape"]["nodes"])
    if (groups.contains(id(n["id"])))
      for (auto &r : n["routeIds"])
        citykeys[owners.value(id(r), std::string())].insert(
            groups[id(n["id"])]);
  for (auto &f : state["obstacles"]["features"]) {
    auto prop = f.value("properties", emptyObject);
    if (prop.value("scope", std::string()) == "global")
      continue;
    Strings assoc = strings(prop.value("networkIds", emptyArray));
    if (assoc.empty() && prop.contains("networkId"))
      assoc.insert(id(prop["networkId"]));
    Strings keys;
    for (auto &city : assoc)
      keys.insert(citykeys[city].begin(), citykeys[city].end());
    if (keys.size() == 1 && ts.contains(*keys.begin()))
      f["geometry"]["coordinates"] =
          coordinates(f["geometry"]["coordinates"], [&](P p) {
            return unproject(
                transform(project(p, planar), ts[*keys.begin()], true), planar);
          });
  }
  state["shapeRevision"] = state.value("shapeRevision", 0) + 1;
  state.erase("shapeTraversals");
}
void validateEdit(const J &state) {
  auto meta = state.at("transitNetworks"), c = meta.at("composite");
  auto members = strings(c["members"]);
  std::map<std::string, P> nodes;
  for (auto &n : state["shape"]["nodes"])
    nodes[id(n["id"])] = {n["x"], n["y"]};
  auto poly = c["boundary"].get<std::vector<P>>();
  for (auto &e : state["shape"]["segments"]) {
    bool selected = false;
    for (auto &r : e["routeIds"])
      selected |= members.count(meta["routes"].value(id(r), std::string())) > 0;
    if (selected &&
        !coversLine(nodes.at(id(e["a"])), nodes.at(id(e["b"])), poly))
      throw std::invalid_argument(
          "Edit would take a route outside the composite container.");
  }
}
void retainNonmembers(const J &before, J &after) {
  auto meta = before.at("transitNetworks");
  auto members = strings(meta["composite"]["members"]);
  auto owners = meta.at("routes");
  for (std::string key : {"nodes", "segments", "routes"}) {
    auto selected = [&](const J &row) {
      auto routes = key == "routes" ? J::array({row["id"]}) : row["routeIds"];
      for (auto &r : routes)
        if (members.count(owners.value(id(r), std::string())))
          return true;
      return false;
    };
    J rows = emptyArray;
    for (auto &r : after["shape"][key])
      if (selected(r))
        rows.push_back(r);
    for (auto &r : before["shape"][key])
      if (!selected(r))
        rows.push_back(r);
    after["shape"][key] = rows;
  }
  for (size_t i = 0; i < after["shape"]["nodes"].size(); ++i)
    after["shape"]["nodes"][i]["internalId"] = i;
  auto features = before["obstacles"]["features"];
  for (size_t i = 0; i < features.size(); ++i) {
    auto p = features[i].value("properties", emptyObject);
    if (!members.count(p.value("networkId", std::string())) ||
        !p.value("networkIds", emptyArray).empty() ||
        p.value("scope", std::string()) == "global")
      after["obstacles"]["features"][i] = features[i];
  }
}
} // namespace
J presentation(const J &state) {
  J result = emptyObject;
  auto meta = state.value("transitNetworks", emptyObject);
  auto t = state.value("transform", emptyObject);
  if (t.empty())
    return result;
  if (meta.contains("containers")) {
    J containers = emptyArray;
    for (auto it = meta["containers"].begin(); it != meta["containers"].end();
         ++it) {
      J pts = emptyArray;
      for (auto &p : it.value()["boundary"])
        pts.push_back(canvas(point(p), t));
      containers.push_back(
          {{"id", it.key()}, {"kind", it.value()["kind"]}, {"points", pts}});
    }
    result["containers"] = containers;
  }
  if (meta.contains("composite")) {
    auto c = meta["composite"];
    J summary = emptyObject;
    for (std::string key : {"id", "name", "members", "method", "kind", "rotate",
                            "metrics", "containerId"})
      summary[key] = c.at(key);
    std::vector<P> ps;
    for (auto &p : c["boundary"])
      ps.push_back(canvas(point(p), t));
    summary["boundary"] = ps;
    summary["componentCount"] =
        components(state["shape"], meta["routes"], strings(c["members"])).count;
    result["composite"] = summary;
    Box b = bounds(ps);
    double w = std::max({1., (b[2] - b[0]) * 1.1, (b[3] - b[1]) * 1.65});
    P mid = center(b);
    result["fitView"] = {
        {"x", mid[0] - w / 2}, {"y", mid[1] - w / 3}, {"w", w}, {"h", w / 1.5}};
  }
  return result;
}
J layoutRequest(const J &request) {
  auto op = request.at("op").get<std::string>();
  J state = request.at("state");
  if (op == "mega-layout") {
    if (request.contains("before"))
      retainNonmembers(request["before"], state);
    optimize(state, request.value("options", emptyObject),
             request.value("afterOcti", false));
  } else if (op == "mega-remove")
    remove(state, request.value("containerId", std::string()));
  else if (op == "mega-validate") {
    validateEdit(state);
    return {{"valid", true}};
  } else
    throw std::invalid_argument("Unknown composite operation.");
  return {{"state", state}};
}
} // namespace mega

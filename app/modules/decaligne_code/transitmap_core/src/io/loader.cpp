#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <cctype>

#include "json.hpp"
#include "loader.h"
#include "core/projection.h"

using json = nlohmann::json;

static std::filesystem::path resolveInputPath(const std::string& path) {
    std::filesystem::path p(path);
    if (p.is_absolute() || std::filesystem::exists(p)) return p;

    std::filesystem::path cwd = std::filesystem::current_path();
    for (std::filesystem::path probe = cwd; !probe.empty(); probe = probe.parent_path()) {
        std::filesystem::path candidate = probe / p;
        if (std::filesystem::exists(candidate)) return candidate;
        if (probe == probe.parent_path()) break;
    }

    return p;
}

static bool parseTransitCssFile(const std::filesystem::path& filePath, TransitMapStyle& outStyle) {
    std::ifstream in(filePath);
    if (!in.is_open()) return false;

    auto trim = [](const std::string& value) {
        const auto b = value.find_first_not_of(" \t");
        if (b == std::string::npos) return std::string();
        const auto e = value.find_last_not_of(" \t;\r\n");
        return value.substr(b, e - b + 1);
        };
    auto makeShape = [](const std::string& name) -> StationShapeStyle {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return (lower.find("rounded") != std::string::npos || lower.find("rectangle") != std::string::npos)
            ? StationShapeStyle{ RoundedRectangleStationStyle{} }
        : StationShapeStyle{ CircleStationStyle{} };
        };

    std::string line;
    bool routeSet = false, normalSet = false, transferSet = false, backgroundSet = false;
    StationShapeStyle* currentShape = nullptr;
    while (std::getline(in, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const std::string routeKey = "route-width:", backgroundKey = "background:", normalShapeKey = "normal:station:";
        const std::string transferShapeKeyA = "transfer:station:", transferShapeKeyB = "transfer station:";
        if (t.rfind(routeKey, 0) == 0) { outStyle.routeWidth = std::strtof(trim(t.substr(routeKey.size())).c_str(), nullptr); routeSet = true; }
        else if (t.rfind(normalShapeKey, 0) == 0) { outStyle.normalStationShape = makeShape(trim(t.substr(normalShapeKey.size()))); currentShape = &outStyle.normalStationShape; normalSet = true; }
        else if (t.rfind(transferShapeKeyA, 0) == 0) { outStyle.transferStationShape = makeShape(trim(t.substr(transferShapeKeyA.size()))); currentShape = &outStyle.transferStationShape; transferSet = true; }
        else if (t.rfind(transferShapeKeyB, 0) == 0) { outStyle.transferStationShape = makeShape(trim(t.substr(transferShapeKeyB.size()))); currentShape = &outStyle.transferStationShape; transferSet = true; }
        else if (t.rfind("radius:", 0) == 0 && currentShape) { if (auto* circle = std::get_if<CircleStationStyle>(currentShape)) circle->radius = std::strtof(trim(t.substr(7)).c_str(), nullptr); }
        else if (t.rfind("w:", 0) == 0 && currentShape) { if (auto* rectangle = std::get_if<RoundedRectangleStationStyle>(currentShape)) rectangle->width = std::strtof(trim(t.substr(2)).c_str(), nullptr); }
        else if (t.rfind("h:", 0) == 0 && currentShape) { if (auto* rectangle = std::get_if<RoundedRectangleStationStyle>(currentShape)) rectangle->height = std::strtof(trim(t.substr(2)).c_str(), nullptr); }
        else if (t.rfind("r:", 0) == 0 && currentShape) { if (auto* rectangle = std::get_if<RoundedRectangleStationStyle>(currentShape)) rectangle->cornerRadius = std::strtof(trim(t.substr(2)).c_str(), nullptr); }
        else if (t.rfind("rotation:", 0) == 0 && currentShape) { if (auto* rectangle = std::get_if<RoundedRectangleStationStyle>(currentShape)) rectangle->rotationDegrees = std::strtof(trim(t.substr(9)).c_str(), nullptr); }
        else if (t.rfind("normal_offset:", 0) == 0 && currentShape) { if (auto* rectangle = std::get_if<RoundedRectangleStationStyle>(currentShape)) rectangle->normalOffset = std::strtof(trim(t.substr(14)).c_str(), nullptr); }
        else if (t.rfind("fill_color:", 0) == 0 && currentShape) { std::visit([&](auto& shape) { shape.stationstyle.fillColorHex = trim(t.substr(11)); }, *currentShape); }
        else if (t.rfind("path_width:", 0) == 0 && currentShape) { std::visit([&](auto& shape) { shape.stationstyle.pathWidth = std::strtof(trim(t.substr(11)).c_str(), nullptr); }, *currentShape); }
        else if (t.rfind("path_color:", 0) == 0 && currentShape) { std::visit([&](auto& shape) { shape.stationstyle.pathColorHex = trim(t.substr(11)); }, *currentShape); }

        else if (t.rfind(backgroundKey, 0) == 0) {
            const std::string value = trim(t.substr(backgroundKey.size()));
            if (!value.empty() && value[0] == '#' && value.size() >= 7) { outStyle.backgroundHex = value.substr(0, 7); backgroundSet = true; }
            else { int r = 255, g = 255, b = 255; if (std::sscanf(value.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3) { char hex[8]{}; std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)); outStyle.backgroundHex = hex; backgroundSet = true; } }
        }
    }
    return routeSet && normalSet && transferSet && backgroundSet;
}

// change to RGB color
static void hexToRGB01(const std::string& hex, float out[3]) {

    //default color --> white
    if (hex.size() != 6) {
        out[0] = out[1] = out[2] = 1.0f;
        return;
    }

    auto hexToInt = [](char c) -> int {
        if ('0' <= c && c <= '9') return c - '0';
        if ('a' <= c && c <= 'f') return c - 'a' + 10;
        if ('A' <= c && c <= 'F') return c - 'A' + 10;
        return 0;
        };

    int r = hexToInt(hex[0]) * 16 + hexToInt(hex[1]);
    int g = hexToInt(hex[2]) * 16 + hexToInt(hex[3]);
    int b = hexToInt(hex[4]) * 16 + hexToInt(hex[5]);

    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
}

static bool looksLikeLonLat(double x, double y) {
    // lon/lat ~ [-180,180] x [-90,90]
    return std::abs(x) <= 180.0 && std::abs(y) <= 90.0;
}

static Point readPointAutoProject(const json& coord) {

    const double x = coord[0].get<double>();
    const double y = coord[1].get<double>();
    if (looksLikeLonLat(x, y)) {
        return lonlatToWebMerc(x, y);
    }
    // already planar (octi schematic coordinates)
    return { x, y };
}

// read JSON file
void Loader::loadRoutesJson(const json& j, GeoData& data) {
    data.stations.clear(); data.routes.clear(); data.stationAdj.clear();
    std::unordered_map<std::string, int> id2Index;
    std::unordered_map<std::string, Point> id2Point;

    auto readPoint = [&](const json& c) -> Point {
        if (!c.is_array() || c.size() < 2 || !c[0].is_number() || !c[1].is_number()) {
            return Point{ 0.0, 0.0 };
        }

        double x = c[0].get<double>();
        double y = c[1].get<double>();

        if (j.value("coordinateSystem", std::string("auto")) != "planar" && std::abs(x) <= 180.0 && std::abs(y) <= 90.0) {
            return lonlatToWebMerc(x, y);
        }

        return Point{ x, y };
        };

    // stations
    for (const auto& feat : j["features"]) {
        if (!feat.contains("geometry")) continue;
        const auto& geom = feat["geometry"];
        if (!geom.contains("type") || geom["type"] != "Point") continue;
        if (!geom.contains("coordinates")) continue;

        if (!feat.contains("properties")) continue;
        const auto& props = feat["properties"];

        if (!props.contains("id") || !props["id"].is_string()) continue;

        const std::string pointId = props["id"].get<std::string>();

        const auto& c = geom["coordinates"];
        if (!c.is_array() || c.size() < 2) continue;

        const Point pos = readPoint(c);
        id2Point[pointId] = pos;

        const bool hasStationName =
            (props.contains("station_label") && props["station_label"].is_string())
            || (props.contains("name") && props["name"].is_string());
        const bool kindSaysShapePoint =
            props.contains("kind") && props["kind"].is_string() && props["kind"].get<std::string>() == "shape_point";
        if (!hasStationName && kindSaysShapePoint) continue;
        if (!hasStationName) continue;

        Station s;
        s.id = pointId;
        s.station_id = (props.contains("station_id") && props["station_id"].is_string())
            ? props["station_id"].get<std::string>()
            : "";

        if (props.contains("station_label") && props["station_label"].is_string())
            s.name = props["station_label"].get<std::string>();
        else if (props.contains("name") && props["name"].is_string())
            s.name = props["name"].get<std::string>();

        s.pos = pos;

        int idx = (int)data.stations.size();
        data.stations.push_back(s);
        id2Index[s.id] = idx;
    }

    data.stationAdj.assign(data.stations.size(), {});

    auto addUndirectedEdge = [&](int a, int b) {
        if (a < 0 || b < 0) return;
        if (a == b) return;
        if (a >= (int)data.stationAdj.size() || b >= (int)data.stationAdj.size()) return;

        data.stationAdj[a].push_back(b);
        data.stationAdj[b].push_back(a);
        };

    // routes and adjacency
    std::unordered_map<std::string, Route> routeMap;

    for (const auto& feat : j["features"]) {

        if (!feat.contains("geometry")) continue;
        const auto& geom = feat["geometry"];
        if (!geom.contains("type") || geom["type"] != "LineString") continue;
        if (!geom.contains("coordinates")) continue;

        std::vector<Point> segmentPts;
        for (const auto& c : geom["coordinates"]) {
            if (!c.is_array() || c.size() < 2) continue;
            segmentPts.push_back(readPoint(c));
        }
        if (segmentPts.size() < 2) continue;

        if (!feat.contains("properties")) continue;
        const auto& props = feat["properties"];

        int fromIdx = -1, toIdx = -1;
        std::string fromId;
        std::string toId;
        if (props.contains("from") && props["from"].is_string()) {
            fromId = props["from"].get<std::string>();
            auto it = id2Index.find(fromId);
            if (it != id2Index.end()) fromIdx = it->second;
        }
        if (props.contains("to") && props["to"].is_string()) {
            toId = props["to"].get<std::string>();
            auto it = id2Index.find(toId);
            if (it != id2Index.end()) toIdx = it->second;
        }

        if (!fromId.empty()) {
            auto it = id2Point.find(fromId);
            if (it != id2Point.end()) segmentPts.front() = it->second;
        }
        if (!toId.empty()) {
            auto it = id2Point.find(toId);
            if (it != id2Point.end()) segmentPts.back() = it->second;
        }

        addUndirectedEdge(fromIdx, toIdx);

        if (!props.contains("lines") || !props["lines"].is_array()) continue;

        for (const auto& lineObj : props["lines"]) {
            if (!lineObj.is_object()) continue;
            std::string lineId;
            if (lineObj.contains("label") && lineObj["label"].is_string()) {
                lineId = lineObj["label"].get<std::string>();
            }
            if (lineId.empty() && lineObj.contains("id") && lineObj["id"].is_string()) {
                lineId = lineObj["id"].get<std::string>();
            }
            if (lineId.empty() && lineObj.contains("name") && lineObj["name"].is_string()) {
                lineId = lineObj["name"].get<std::string>();
            }
            if (lineId.empty()) continue;

            if (!routeMap.count(lineId)) {
                Route r;
                r.name = lineObj.contains("name") && lineObj["name"].is_string()
                    ? lineObj["name"].get<std::string>()
                    : lineId;
                r.id = lineId;
                r.color[0] = r.color[1] = r.color[2] = 1.0f;
                //r.route_width = 15.0f;
                if (lineObj.contains("color") && lineObj["color"].is_string()) {
                    hexToRGB01(lineObj["color"].get<std::string>(), r.color);
                }
                if (lineObj.contains("route_width") && lineObj["route_width"].is_number()) {
                    r.route_width = lineObj["route_width"].get<float>();
                }
                routeMap[lineId] = r;
            }
            else if (lineObj.contains("route_width") && lineObj["route_width"].is_number()) {
                routeMap[lineId].route_width = lineObj["route_width"].get<float>();
            }

            Route& r = routeMap[lineId];
            r.segments.push_back(segmentPts);

            auto addStationOnce = [&](int idx) {
                if (idx < 0) return;
                if (std::find(r.stationIndices.begin(), r.stationIndices.end(), idx) == r.stationIndices.end())
                    // unordered
                    r.stationIndices.push_back(idx);
                };
            addStationOnce(fromIdx);
            addStationOnce(toIdx);
        }
    }

    for (auto& nbrs : data.stationAdj) {
        std::sort(nbrs.begin(), nbrs.end());
        nbrs.erase(std::unique(nbrs.begin(), nbrs.end()), nbrs.end());
    }

    for (auto& [_, r] : routeMap) {
        data.routes.push_back(r);
    }
    //std::cout << "routes: " << data.routes.size() << std::endl;
}

static void loadRoutesFile(const std::string& path, GeoData& data) {
    std::ifstream file(path);
    if(!file) throw std::runtime_error("Cannot open transit JSON: " + path);
    json value; file >> value; Loader::loadRoutesJson(value, data);
}
void Loader::loadRoutes(const std::string& path, GeoData& data) { loadRoutesFile(path, data); }
void Loader::loadLoomGraphGeoJson(const std::string& path, GeoData& data) { loadRoutesFile(path, data); }

// read GeoJSON file
void Loader::loadObstacles(const std::string& path, GeoData& data) {

    /*
    Obstacles GeoJSON:
        [0] - outline
        [1] - hole_0
        [2] - hole_1
        [3] - hole_2
        ...
    */

    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "Failed to open " << path << std::endl;
        return;
    }

    json j;
    f >> j;

    data.obstacles.clear();

    auto readPoint = [&](const json& c) -> Point {
        if (!c.is_array() || c.size() < 2) return { 0, 0 };
        return readPointAutoProject(c);
        };

    auto addPolygon = [&](const json& rings) {
        if (!rings.is_array() || rings.empty()) return;

        Obstacle p;

        // outer ring
        if (rings[0].is_array()) {
            for (const auto& c : rings[0]) {
                if (!c.is_array() || c.size() < 2) continue;
                p.outer.push_back(readPoint(c));
            }
        }

        // holes
        for (size_t i = 1; i < rings.size(); ++i) {
            if (!rings[i].is_array()) continue;
            std::vector<Point> hole;
            hole.reserve(rings[i].size());
            for (const auto& c : rings[i]) {
                if (!c.is_array() || c.size() < 2) continue;
                hole.push_back(readPoint(c));
            }
            if (hole.size() >= 3) p.holes.push_back(std::move(hole));
        }

        // basic validity
        if (p.outer.size() >= 3) data.obstacles.push_back(std::move(p));
        };

    for (auto& feat : j["features"]) {
        if (!feat.contains("geometry")) continue;
        auto& g = feat["geometry"];
        if (!g.contains("type") || !g.contains("coordinates")) continue;

        const std::string type = g["type"].get<std::string>();

        if (type == "Polygon") {
            addPolygon(g["coordinates"]);
        }
        else if (type == "MultiPolygon") {
            // MultiPolygon: [ [rings], [rings], ... ]
            for (const auto& poly : g["coordinates"]) {
                addPolygon(poly);
            }
        }
    }
}

bool Loader::loadTransitCssStyle(const std::string& path, TransitMapStyle& outStyle) {
    return parseTransitCssFile(resolveInputPath(path), outStyle);
}

bool Loader::loadTransitCssStyles(const std::string& folder, std::vector<TransitCssStyleEntry>& outStyles) {
    outStyles.clear();

    const std::filesystem::path styleFolder = resolveInputPath(folder);
    if (!std::filesystem::exists(styleFolder)) return false;

    for (const auto& entry : std::filesystem::directory_iterator(styleFolder)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".css") continue;

        TransitCssStyleEntry styleEntry;
        styleEntry.name = entry.path().stem().string();
        styleEntry.filePath = entry.path().string();
        if (!parseTransitCssFile(entry.path(), styleEntry.style)) {
            std::cerr << "Failed to parse transitCSS style: " << entry.path() << "\n";
            continue;
        }

        outStyles.push_back(std::move(styleEntry));
    }

    std::sort(outStyles.begin(), outStyles.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return true;
}
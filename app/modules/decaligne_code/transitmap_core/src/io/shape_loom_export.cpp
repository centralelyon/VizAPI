#include <type_traits>
#include <variant>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <filesystem>
#include <vector>
#include <cctype>
#include <iostream>
#include <functional>

#include "json.hpp"
#include "io/shape_loom_export.h"
#include "core/projection.h"
#include "render/render_geometry.h"

using json = nlohmann::json;

static inline json coord(const Point& p) {

    Point ll = webMercToLonLat(p.x, p.y);
    return json::array({ ll.x, ll.y }); // [lon, lat]
}

static std::string rgb01ToHexNoHash(const float c[3]) {

    auto clamp01 = [](float v) { return std::max(0.f, std::min(1.f, v)); };
    int r = (int)std::lround(clamp01(c[0]) * 255.f);
    int g = (int)std::lround(clamp01(c[1]) * 255.f);
    int b = (int)std::lround(clamp01(c[2]) * 255.f);

    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << r
        << std::setw(2) << g
        << std::setw(2) << b;
    return oss.str(); // "RRGGBB"
}

static std::string base64Encode(const std::vector<unsigned char>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned int value = (static_cast<unsigned int>(bytes[i]) << 16)
            | (i + 1 < bytes.size() ? static_cast<unsigned int>(bytes[i + 1]) << 8 : 0)
            | (i + 2 < bytes.size() ? static_cast<unsigned int>(bytes[i + 2]) : 0);
        encoded.push_back(alphabet[(value >> 18) & 63]);
        encoded.push_back(alphabet[(value >> 12) & 63]);
        encoded.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        encoded.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return encoded;
}

static std::string cssStringEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char c : value) {
        if (c == '\\' || c == '\'') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

static std::string embeddedFontCss(const std::string& family, const std::string* fontPath) {
    if (!fontPath || fontPath->empty()) return {};
    std::ifstream font(*fontPath, std::ios::binary);
    if (!font) return {};
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(font)),
        std::istreambuf_iterator<char>());
    if (bytes.empty()) return {};
    std::string extension = std::filesystem::path(*fontPath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const char* mime = extension == ".otf" ? "font/otf" : "font/ttf";
    const char* format = extension == ".otf" ? "opentype" : "truetype";
    return "  <style>@font-face{font-family:'" + cssStringEscape(family)
        + "';src:url(data:" + mime + ";base64," + base64Encode(bytes)
        + ") format('" + format + "');}</style>\n";
}

static bool isRegionRouteForExport(const ShapeRoute& route) { return route.isObstacle && (route.obstacleKind == ObstacleKind::Region || route.id.rfind("obsr_", 0) == 0); }

// segment -> routes
static std::vector<std::vector<int>> buildSegToRoutes(const Shape& shape, bool excludeRegions) {

    std::vector<std::vector<int>> seg2routes(shape.segments.size());
    for (int ri = 0; ri < (int)shape.routes.size(); ++ri) {
        if (excludeRegions && isRegionRouteForExport(shape.routes[ri])) continue;
        for (int segIdx : shape.routes[ri].segmentIndices) {
            if (segIdx < 0 || segIdx >= (int)shape.segments.size()) continue;
            seg2routes[segIdx].push_back(ri);
        }
    }
    for (auto& v : seg2routes) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    return seg2routes;
}

json shapeToLoomGeoJson(const Shape& shape, bool excludeRegions) {

    json fc;
    fc["type"] = "FeatureCollection";
    fc["features"] = json::array();

    // id
    std::vector<std::string> nodeRefId(shape.nodes.size());
    for (int i = 0; i < (int)shape.nodes.size(); ++i) {
        const auto& n = shape.nodes[i];

        if (!n.uid.empty()) nodeRefId[i] = n.uid;
        else if (isStationLike(n.type)) {
            if (!n.station_id.empty()) nodeRefId[i] = n.station_id;
            else nodeRefId[i] = "st_" + std::to_string(i);
        }
        else {
            // shape points
            nodeRefId[i] = "sp_" + std::to_string(i);
        }
    }

    // point
    for (int i = 0; i < (int)shape.nodes.size(); ++i) {
        const auto& n = shape.nodes[i];

        json feat;
        feat["type"] = "Feature";
        feat["geometry"] = {
            {"type", "Point"},
            {"coordinates", coord(n.pos)}
        };

        json props;
        props["id"] = nodeRefId[i];

        if (isStationLike(n.type)) {
            // station
            props["station_label"] = n.name;
            if (!n.station_id.empty()) props["station_id"] = n.station_id;
            props["kind"] = "station";
        }
        else {
            // shape point
            props["kind"] = "shape_point";
        }

        feat["properties"] = props;
        fc["features"].push_back(feat);
    }
    
    // Linestring
    auto seg2routes = buildSegToRoutes(shape, excludeRegions);

    for (int si = 0; si < (int)shape.segments.size(); ++si) {
        const auto& seg = shape.segments[si];
        if (seg.a < 0 || seg.a >= (int)shape.nodes.size()) continue;
        if (seg.b < 0 || seg.b >= (int)shape.nodes.size()) continue;
        if (seg.a == seg.b) continue;

        // Linestring --> (point a,point b) NOT A POLYLINE
        const Point& a = shape.nodes[seg.a].pos;
        const Point& b = shape.nodes[seg.b].pos;

        if (seg2routes[si].empty()) continue;

        json feat;
        feat["type"] = "Feature";
        feat["geometry"] = {
            {"type", "LineString"},
            {"coordinates", json::array({ coord(a), coord(b) })}
        };

        json props;
        props["id"] = seg.uid.empty() ? "edge_" + std::to_string(si) : seg.uid;
        props["from"] = nodeRefId[seg.a];
        props["to"] = nodeRefId[seg.b];

        // route --> segments
        json lines = json::array();
        for (int ri : seg2routes[si]) {
            const auto& r = shape.routes[ri];
            json lo;
            if (!r.name.empty()) {
                lo["name"] = r.name;
            }
            lo["id"] = r.id;
            lo["label"] = r.name.empty() ? r.id : r.name;
            lo["color"] = rgb01ToHexNoHash(r.color);
            lo["route_width"] = r.route_width;
            lines.push_back(lo);
        }
        props["lines"] = lines;

        feat["properties"] = props;
        fc["features"].push_back(feat);
    }

    return fc;
}

bool exportShapeToLoomGeoJson(const Shape& shape, const std::string& outPath, bool excludeRegions) {
    std::ofstream out(outPath);
    if (!out.is_open()) return false;
    out << shapeToLoomGeoJson(shape, excludeRegions).dump(2);
    return true;
}

bool exportStyledShapeToGeoJson(const Shape& shape, const StyledShape& styledShape,
    const Camera& camera, const std::string& outPath, bool excludeRegions) {
    const StyledShapeDisplayData display = buildStyledShapeDisplayData(styledShape, camera);

    json fc;
    fc["type"] = "FeatureCollection";
    fc["properties"] = json::object();
    fc["features"] = json::array();

    struct ExportPoint {
        Point pos;
        std::string id;
        std::string stationId;
        std::string label;
        int degree = 0;
        bool station = false;
    };
    std::vector<ExportPoint> points;
    std::unordered_map<std::string, int> stationDegrees;
    for (size_t nodeIndex = 0; nodeIndex < shape.nodes.size(); ++nodeIndex) {
        const ShapeNode& node = shape.nodes[nodeIndex];
        if (!isStationLike(node.type)) continue;
        int degree = 0;
        for (const ShapeSegment& segment : shape.segments)
            if (segment.a == static_cast<int>(nodeIndex) ||
                segment.b == static_cast<int>(nodeIndex)) ++degree;
        const std::string key = !node.station_id.empty() ? node.station_id : node.name;
        stationDegrees[key] = degree;
    }

    auto addStation = [&](const auto& station) {
        ExportPoint point;
        point.pos = station.pos;
        point.stationId = station.station_id;
        point.label = station.name;
        point.id = !station.station_id.empty() ? station.station_id :
            (!station.id.empty() ? station.id : "station_" + std::to_string(points.size()));
        const std::string key = !station.station_id.empty() ? station.station_id : station.name;
        const auto degreeIt = stationDegrees.find(key);
        point.degree = degreeIt == stationDegrees.end() ? 0 : degreeIt->second;
        point.station = true;
        points.push_back(std::move(point));
    };
    for (const auto& station : display.normalStations) addStation(station);
    for (const auto& station : display.transferStations) addStation(station);

    const double stationToleranceWorld =
        0.75 / std::max(camera.getScale(), 1e-9);
    const double stationToleranceSquared =
        stationToleranceWorld * stationToleranceWorld;
    auto pointIdFor = [&](const Point& position, int routeIndex,
        int pathIndex, int pointIndex) {
        for (const ExportPoint& point : points) {
            if (!point.station) continue;
            const double dx = position.x - point.pos.x;
            const double dy = position.y - point.pos.y;
            if (dx * dx + dy * dy <= stationToleranceSquared) return point.id;
        }
        ExportPoint point;
        point.pos = position;
        point.id = "shape_" + std::to_string(routeIndex) + "_" +
            std::to_string(pathIndex) + "_" + std::to_string(pointIndex);
        points.push_back(point);
        return point.id;
    };

    struct ExportEdge {
        Point a;
        Point b;
        std::string from;
        std::string to;
        int routeIndex = -1;
    };
    std::vector<ExportEdge> edges;
    for (int routeIndex = 0;
        routeIndex < static_cast<int>(display.worldRoutePaths.size()); ++routeIndex) {
        if (routeIndex >= static_cast<int>(shape.routes.size())) continue;
        if (excludeRegions && isRegionRouteForExport(shape.routes[routeIndex])) continue;
        const auto& paths = display.worldRoutePaths[routeIndex];
        for (int pathIndex = 0; pathIndex < static_cast<int>(paths.size()); ++pathIndex) {
            const auto& path = paths[pathIndex];
            if (path.size() < 2) continue;
            std::vector<std::string> ids(path.size());
            for (int pointIndex = 0; pointIndex < static_cast<int>(path.size()); ++pointIndex)
                ids[pointIndex] = pointIdFor(
                    path[pointIndex], routeIndex, pathIndex, pointIndex);
            for (int pointIndex = 1; pointIndex < static_cast<int>(path.size()); ++pointIndex) {
                if (ids[pointIndex - 1] == ids[pointIndex]) continue;
                edges.push_back({ path[pointIndex - 1], path[pointIndex],
                    ids[pointIndex - 1], ids[pointIndex], routeIndex });
            }
        }
    }

    std::unordered_map<std::string, int> exportedDegrees;
    for (const ExportEdge& edge : edges) {
        ++exportedDegrees[edge.from];
        ++exportedDegrees[edge.to];
    }
    for (ExportPoint& point : points) {
        const auto degreeIt = exportedDegrees.find(point.id);
        if (degreeIt != exportedDegrees.end()) point.degree = degreeIt->second;
        json properties;
        properties["deg"] = std::to_string(point.degree);
        properties["deg_in"] = std::to_string(point.degree);
        properties["deg_out"] = std::to_string(point.degree);
        properties["id"] = point.id;
        if (point.station) {
            properties["station_id"] = point.stationId;
            properties["station_label"] = point.label;
        }
        json feature;
        feature["type"] = "Feature";
        feature["geometry"] = { {"type", "Point"}, {"coordinates", coord(point.pos)} };
        feature["properties"] = std::move(properties);
        fc["features"].push_back(std::move(feature));
    }

    for (int edgeIndex = 0; edgeIndex < static_cast<int>(edges.size()); ++edgeIndex) {
        const ExportEdge& edge = edges[edgeIndex];
        const ShapeRoute& route = shape.routes[edge.routeIndex];
        const float* color = route.color;
        if (edge.routeIndex < static_cast<int>(styledShape.routes_colors.size()))
            color = styledShape.routes_colors[edge.routeIndex].data();

        json line;
        line["id"] = route.id;
        line["label"] = route.id.empty() ? route.name : route.id;
        line["color"] = rgb01ToHexNoHash(color);
        if (!route.name.empty()) line["name"] = route.name;

        json properties;
        properties["dbg_lines"] = route.id;
        properties["from"] = edge.from;
        properties["id"] = "edge_" + std::to_string(edgeIndex);
        properties["lines"] = json::array({ std::move(line) });
        properties["to"] = edge.to;

        json feature;
        feature["type"] = "Feature";
        feature["geometry"] = {
            {"type", "LineString"},
            {"coordinates", json::array({ coord(edge.a), coord(edge.b) })}
        };
        feature["properties"] = std::move(properties);
        fc["features"].push_back(std::move(feature));
    }

    std::ofstream out(outPath);
    if (!out) return false;
    out << std::fixed << std::setprecision(10) << fc.dump(2);
    return static_cast<bool>(out);
}

static std::string xmlEscape(const std::string& value) {

    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += ch; break;
        }
    }
    return out;
}

static std::string rgb01ToHex(const float c[3]) {

    return "#" + rgb01ToHexNoHash(c);
}

static bool hexToRgb01Local(const std::string& hex, float out[3]) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
        };
    for (int i = 0; i < 3; ++i) {
        const int hi = nibble(hex[1 + i * 2]);
        const int lo = nibble(hex[2 + i * 2]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<float>((hi * 16 + lo) / 255.0);
    }
    return true;
}

static std::string resolveSvgColor(const std::string& color, const float routeColor[3]) {
    if (color == "route_color") return rgb01ToHex(routeColor);
    float rgb[3];
    if (hexToRgb01Local(color, rgb)) return color;
    if (color.rfind("rgb(", 0) == 0) return color;
    int red = 0, green = 0, blue = 0;
    char trailing = '\0';
    if (std::sscanf(color.c_str(), " ( %d , %d , %d ) %c",
        &red, &green, &blue, &trailing) == 3) {
        const float normalized[3] = {
            std::clamp(red, 0, 255) / 255.0f,
            std::clamp(green, 0, 255) / 255.0f,
            std::clamp(blue, 0, 255) / 255.0f
        };
        return rgb01ToHex(normalized);
    }
    return "#000000";
}

static bool isTransferShapeStation(const Shape& shape, int nodeIndex) {
    std::unordered_set<int> routeIndices;
    for (int ri = 0; ri < (int)shape.routes.size(); ++ri) {
        const auto& route = shape.routes[ri];
        if (isRegionRouteForExport(route)) continue;
        for (int segIndex : route.segmentIndices) {
            if (segIndex < 0 || segIndex >= (int)shape.segments.size()) continue;
            const auto& seg = shape.segments[segIndex];
            if (seg.a == nodeIndex || seg.b == nodeIndex) {
                routeIndices.insert(ri);
                break;
            }
        }
    }
    return routeIndices.size() >= 2;
}

static const ShapeRoute* firstRouteForShapeStation(const Shape& shape, int nodeIndex) {
    for (const auto& route : shape.routes) {
        if (isRegionRouteForExport(route)) continue;
        for (int segIndex : route.segmentIndices) {
            if (segIndex < 0 || segIndex >= (int)shape.segments.size()) continue;
            const auto& seg = shape.segments[segIndex];
            if (seg.a == nodeIndex || seg.b == nodeIndex) return &route;
        }
    }
    return nullptr;
}



static Point stationScreenNormal(const Shape& shape, int nodeIndex,
    const std::function<Point(const Point&)>& toScreen) {
    Point tangentSum{};
    for (const auto& route : shape.routes) {
        if (isRegionRouteForExport(route)) continue;
        for (int segmentIndex : route.segmentIndices) {
            if (segmentIndex < 0 || segmentIndex >= static_cast<int>(shape.segments.size())) continue;
            const auto& segment = shape.segments[segmentIndex];
            if (segment.a != nodeIndex && segment.b != nodeIndex) continue;
            const int otherIndex = segment.a == nodeIndex ? segment.b : segment.a;
            if (otherIndex < 0 || otherIndex >= static_cast<int>(shape.nodes.size())) continue;

            const Point segmentStart = toScreen(shape.nodes[segment.a].pos);
            const Point segmentEnd = toScreen(shape.nodes[segment.b].pos);
            


            Point tangent{ segmentEnd.x - segmentStart.x, segmentEnd.y - segmentStart.y };
            const double length = std::hypot(tangent.x, tangent.y);
            if (length <= 1e-9) continue;
            tangent.x /= length;
            tangent.y /= length;
            if (tangentSum.x * tangent.x + tangentSum.y * tangent.y < 0.0) {
                tangent.x = -tangent.x;
                tangent.y = -tangent.y;
            }
            tangentSum.x += tangent.x;
            tangentSum.y += tangent.y;
        }
    }
    const double length = std::hypot(tangentSum.x, tangentSum.y);
    if (length <= 1e-9) return {};


    return { tangentSum.y / length, -tangentSum.x / length };
}

//static std::vector<TransitMapStyle::StationPrimitiveStyle> svgStationPrimitives(const TransitMapStyle::StationShapeStyle& shapeStyle) {
//    if (!shapeStyle.primitives.empty()) return shapeStyle.primitives;
//    TransitMapStyle::StationPrimitiveStyle p;
//    p.offsetX = shapeStyle.offsetX;
//    p.offsetY = shapeStyle.offsetY;
//    p.superellipse.width = std::max(1.0f, shapeStyle.width);
//    p.superellipse.height = std::max(1.0f, shapeStyle.height);
//    p.superellipse.n = (shapeStyle.shape == TransitMapStyle::StationShapeType::Circle) ? 2.0f :
//        (shapeStyle.cornerRadius <= 0.0f ? 100.0f : 4.0f);
//    p.fillColorHex = shapeStyle.fillColorHex;
//    p.pathWidth = shapeStyle.pathWidth;
//    p.pathColorHex = shapeStyle.pathColorHex;
//    return { p };
//}
//
//static void writeSuperellipsePath(std::ostream& out, double cx, double cy, const TransitMapStyle::StationPrimitiveStyle& prim) {
//    const int steps = 72;
//    const double a = std::max(0.5, (double)prim.superellipse.width * 0.5);
//    const double b = std::max(0.5, (double)prim.superellipse.height * 0.5);
//    constexpr double kPi = 3.14159265358979323846;
//    const double n = std::max(0.01, (double)prim.superellipse.n);
//    const double ct = std::cos((double)prim.theta);
//    const double st = std::sin((double)prim.theta);
//    out << "d=\"";
//    for (int i = 0; i < steps; ++i) {
//        const double t = 2.0 * kPi * (double)i / (double)steps;
//        const double c = std::cos(t);
//        const double s = std::sin(t);
//        const double x = a * std::copysign(std::pow(std::abs(c), 2.0 / n), c);
//        const double y = b * std::copysign(std::pow(std::abs(s), 2.0 / n), s);
//        const double rx = x * ct - y * st;
//        const double ry = x * st + y * ct;
//        out << (i == 0 ? "M " : " L ") << (cx + rx) << ' ' << (cy - ry);
//    }
//    out << " Z\"";
//}

bool exportShapeToSvg(const Shape& shape, const std::string& outPath, bool excludeRegions,
    const TransitMapStyle* style, const Camera* camera,
    const StyledShape* styledShape,
    const std::unordered_map<int, Point>* labelOffsets,
    const std::unordered_set<int>* visibleLabelIndices,
    const std::string* labelFontPath,
    double labelBaselineOffset) {
    if (shape.nodes.empty()) return false;

    double minX = shape.nodes.front().pos.x, maxX = minX, minY = shape.nodes.front().pos.y, maxY = minY;
    for (const auto& node : shape.nodes) { minX = std::min(minX, node.pos.x); maxX = std::max(maxX, node.pos.x); minY = std::min(minY, node.pos.y); maxY = std::max(maxY, node.pos.y); }
    const double width = std::max(1.0, maxX - minX), height = std::max(1.0, maxY - minY);
    const double padding = std::max(10.0, std::max(width, height) * 0.04);
    const double viewX = camera ? 0.0 : minX - padding, viewY = camera ? 0.0 : -(maxY + padding);
    const double viewW = camera ? camera->getScreenW() : width + padding * 2.0, viewH = camera ? camera->getScreenH() : height + padding * 2.0;
    auto point = [&](const Point& p) {
        return camera ? worldToTopLeftScreen(*camera, p) : Point{ p.x, -p.y };
        };

    std::ofstream out(outPath); if (!out) return false;
    out << std::fixed << std::setprecision(3) << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" viewBox=\"" << viewX << ' ' << viewY << ' ' << viewW << ' ' << viewH << "\">\n";
    if (style && style->labelStyle.available) {
        std::error_code fontError;
        const bool fontExists = labelFontPath && !labelFontPath->empty()
            && std::filesystem::is_regular_file(*labelFontPath, fontError);
        const std::string fontCss = fontExists
            ? embeddedFontCss(style->labelStyle.fontFamily, labelFontPath) : std::string{};
        const bool fontEmbedded = !fontCss.empty();
        std::cout << "[SVG Export] font family: " << style->labelStyle.fontFamily << '\n'
            << "[SVG Export] resolved font path: "
            << (labelFontPath && !labelFontPath->empty() ? *labelFontPath : "<none>") << '\n'
            << "[SVG Export] font exists: " << (fontExists ? "true" : "false") << '\n'
            << "[SVG Export] font embedded: " << (fontEmbedded ? "true" : "false") << std::endl;
        if (!fontEmbedded) {
            std::cerr << "[SVG Export] WARNING: selected font '"
                << style->labelStyle.fontFamily
                << "' could not be embedded; labels will remain editable but may use a fallback font."
                << std::endl;
        }
        out << fontCss;
    }
    if (style) {
        const float unusedRouteColor[3] = { 0.0f, 0.0f, 0.0f };
        const std::string backgroundColor = resolveSvgColor(style->backgroundHex, unusedRouteColor);
        out << "  <rect id=\"background\" x=\"" << viewX << "\" y=\"" << viewY
            << "\" width=\"" << viewW << "\" height=\"" << viewH << "\" fill=\""
            << xmlEscape(backgroundColor) << "\"/>\n";
    }
    out << "  <g id=\"routes\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n";
    for (const auto& route : shape.routes) {
        if (excludeRegions && isRegionRouteForExport(route)) continue;
        std::string color = rgb01ToHex(route.color); float routeWidth = route.route_width;
        const int routeIndex = static_cast<int>(&route - shape.routes.data());
        if (styledShape && routeIndex < (int)styledShape->routes_colors.size()) color = rgb01ToHex(styledShape->routes_colors[routeIndex].data());
        if (styledShape && routeIndex < (int)styledShape->routes_width.size()) routeWidth = styledShape->routes_width[routeIndex];
        if (style) { routeWidth = style->routeWidth; auto c = style->routeColorsById.find(route.id); if (c != style->routeColorsById.end()) color = c->second; auto w = style->routeWidthsById.find(route.id); if (w != style->routeWidthsById.end()) routeWidth = w->second; }
        std::vector<Point> pts;
        out << "    <g id=\"" << xmlEscape(route.id.empty() ? route.name : route.id) << "\">\n";
        for (int si : route.segmentIndices) {
            if (si < 0 || si >= (int)shape.segments.size()) continue; const auto& seg = shape.segments[si];
            if (seg.a < 0 || seg.b < 0 || seg.a >= (int)shape.nodes.size() || seg.b >= (int)shape.nodes.size()) continue;
            Point a = point(shape.nodes[seg.a].pos), b = point(shape.nodes[seg.b].pos);
            if (pts.empty()) { pts = { a,b }; }
            else if (std::hypot(pts.back().x - a.x, pts.back().y - a.y) < .01) pts.push_back(b); else if (std::hypot(pts.back().x - b.x, pts.back().y - b.y) < .01) pts.push_back(a);
            else out << "      <line x1=\"" << a.x << "\" y1=\"" << a.y << "\" x2=\"" << b.x << "\" y2=\"" << b.y << "\" stroke=\"" << color << "\" stroke-width=\"" << std::max(.1f, routeWidth) << "\"/>\n";
        }
        if (pts.size() > 1) {
            out << "      <path d=\"M " << pts[0].x << ' ' << pts[0].y;
            for (size_t i = 1; i < pts.size(); ++i) {
                bool rounded = false;
                if (styledShape && i + 1 < pts.size()) {
                    Point u{ pts[i].x - pts[i - 1].x,pts[i].y - pts[i - 1].y }, v{ pts[i + 1].x - pts[i].x,pts[i + 1].y - pts[i].y }; double lu = std::hypot(u.x, u.y), lv = std::hypot(v.x, v.y);
                    if (lu > 1e-6 && lv > 1e-6) { double cosine = (u.x * v.x + u.y * v.y) / (lu * lv), err = 1e9; const StyledShape::BendStyle* chosen = nullptr; for (const auto& bend : styledShape->routes_bends) if (bend.enabled && std::abs(cosine - bend.angleCos) <= bend.angleCosTolerance && std::abs(cosine - bend.angleCos) < err) { chosen = &bend; err = std::abs(cosine - bend.angleCos); } if (chosen) { double r1 = std::min<double>(chosen->radiusX, lu * .49), r2 = std::min<double>(chosen->radiusY, lv * .49); Point p0{ pts[i].x - u.x / lu * r1,pts[i].y - u.y / lu * r1 }, p2{ pts[i].x + v.x / lv * r2,pts[i].y + v.y / lv * r2 }; out << " L " << p0.x << ' ' << p0.y << " Q " << pts[i].x << ' ' << pts[i].y << ' ' << p2.x << ' ' << p2.y; rounded = true; } }
                }
                if (!rounded) out << " L " << pts[i].x << ' ' << pts[i].y;
            }
            out << "\" stroke=\"" << color << "\" stroke-width=\"" << std::max(.1f, routeWidth) << "\"/>\n";
        }
        out << "    </g>\n";
    }
    out << "  </g>\n  <g id=\"stations\">\n";
    for (int i = 0; i < (int)shape.nodes.size(); ++i) {
        const auto& node = shape.nodes[i]; if (!isStationLike(node.type)) continue; Point p = point(node.pos);
        if (!style) { out << "    <circle cx=\"" << p.x << "\" cy=\"" << p.y << "\" r=\"5\" fill=\"white\" stroke=\"black\"/>\n"; continue; }
        const bool transfer = isTransferShapeStation(shape, i); StationShapeStyle stationShape = transfer ? style->transferStationShape : style->normalStationShape; if (styledShape) { const std::string key = !node.station_id.empty() ? node.station_id : (!node.name.empty() ? node.name : std::to_string(node.id)); auto overrideIt = styledShape->stationShapeOverrides.find(key); if (overrideIt != styledShape->stationShapeOverrides.end())stationShape = overrideIt->second; } const ShapeRoute* r = firstRouteForShapeStation(shape, i); float fallback[3] = { r ? r->color[0] : 0,r ? r->color[1] : 0,r ? r->color[2] : 0 };
        std::visit([&](const auto& sh) {const auto& ss = sh.stationstyle; std::string fill = ss.fillUsesRouteColor ? rgb01ToHex(fallback) : resolveSvgColor(ss.fillColorHex, fallback), stroke = ss.pathUsesRouteColor ? rgb01ToHex(fallback) : resolveSvgColor(ss.pathColorHex, fallback); using T = std::decay_t<decltype(sh)>; if constexpr (std::is_same_v<T, CircleStationStyle>) out << "    <circle cx=\"" << p.x << "\" cy=\"" << p.y << "\" r=\"" << std::max(.5f, sh.radius) << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << ss.pathWidth << "\"/>\n"; else { Point center = p; if (sh.normalOffset != 0.0f) { const Point normal = stationScreenNormal(shape, i, point); center.x += normal.x * sh.normalOffset; center.y += normal.y * sh.normalOffset; } out << "    <rect x=\"" << center.x - sh.width * .5 << "\" y=\"" << center.y - sh.height * .5 << "\" width=\"" << sh.width << "\" height=\"" << sh.height << "\" rx=\"" << sh.cornerRadius << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << ss.pathWidth << "\" transform=\"rotate(" << sh.rotationDegrees << ' ' << center.x << ' ' << center.y << ")\"/>\n"; } }, stationShape);
    }
    out << "  </g>\n";
    if (style && style->labelStyle.available) {
        const std::string labelFill = rgb01ToHex(style->labelStyle.color);
        out << "  <g id=\"labels\" font-family=\"" << xmlEscape(style->labelStyle.fontFamily)
            << "\" font-size=\"" << style->labelStyle.fontSize << "\" fill=\"" << labelFill << "\">\n";
        for (int i = 0; i < static_cast<int>(shape.nodes.size()); ++i) {
            const auto& node = shape.nodes[i];
            if (node.type != ShapeNodeType::Station
                || (visibleLabelIndices && !visibleLabelIndices->count(i))) continue;
            const std::string text = node.name.empty() ? node.station_id : node.name;
            if (text.empty()) continue;
            const Point position = point(node.pos);
            Point offset{};
            if (labelOffsets) {
                const auto offsetIt = labelOffsets->find(i);
                if (offsetIt != labelOffsets->end()) offset = offsetIt->second;
            }
            const Point labelPosition = finalLabelScreenPosition(position, offset,
                style->labelStyle.fontSize);
            const double baseline = labelPosition.y + (labelBaselineOffset >= 0.0
                ? labelBaselineOffset : style->labelStyle.fontSize);
            out << "    <text x=\"" << labelPosition.x << "\" y=\"" << baseline
                << "\" font-size=\"" << style->labelStyle.fontSize << "px\">"
                << xmlEscape(text) << "</text>\n";
        }
        out << "  </g>\n";
    }
    out << "</svg>\n"; return (bool)out;
}

//bool exportShapeToSvgStyled(const Shape& shape, const TransitMapStyle& style, const std::string& outPath, bool excludeRegions) {
//    if (shape.nodes.empty()) return false;
//
//    double minX = shape.nodes.front().pos.x, maxX = minX, minY = shape.nodes.front().pos.y, maxY = minY;
//    for (const auto& node : shape.nodes) {
//        minX = std::min(minX, node.pos.x); maxX = std::max(maxX, node.pos.x);
//        minY = std::min(minY, node.pos.y); maxY = std::max(maxY, node.pos.y);
//    }
//    const double width = std::max(1.0, maxX - minX);
//    const double height = std::max(1.0, maxY - minY);
//    const double padding = std::max(10.0, std::max(width, height) * 0.04);
//    const double viewX = minX - padding;
//    const double viewY = -(maxY + padding);
//    const double viewW = width + padding * 2.0;
//    const double viewH = height + padding * 2.0;
//
//    std::ofstream out(outPath);
//    if (!out.is_open()) return false;
//    out << std::fixed << std::setprecision(3);
//    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
//    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" viewBox=\"" << viewX << ' ' << viewY << ' ' << viewW << ' ' << viewH << "\">\n";
//    std::string bg = style.backgroundHex;
//    std::string parsedBg;
//    if (parseHexColorForSvg(bg, parsedBg)) bg = parsedBg;
//    out << "  <rect x=\"" << viewX << "\" y=\"" << viewY << "\" width=\"" << viewW << "\" height=\"" << viewH << "\" fill=\"" << xmlEscape(bg) << "\"/>\n";
//    out << "  <g id=\"routes\" fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n";
//    for (const auto& route : shape.routes) {
//        if (excludeRegions && isRegionRouteForExport(route)) continue;
//        std::string stroke = rgb01ToHex(route.color);
//        auto colorIt = style.routeColorsById.find(route.id);
//        if (colorIt != style.routeColorsById.end()) {
//            std::string parsed;
//            if (parseHexColorForSvg(colorIt->second, parsed)) stroke = parsed;
//        }
//        const float routeWidth = style.routeWidth > 0.0f ? style.routeWidth : route.route_width;
//        out << "    <g id=\"" << xmlEscape(route.id.empty() ? route.name : route.id) << "\"";
//        if (!route.name.empty()) out << " data-name=\"" << xmlEscape(route.name) << "\"";
//        out << ">\n";
//        for (int segIndex : route.segmentIndices) {
//            if (segIndex < 0 || segIndex >= (int)shape.segments.size()) continue;
//            const auto& seg = shape.segments[segIndex];
//            if (seg.a < 0 || seg.b < 0 || seg.a >= (int)shape.nodes.size() || seg.b >= (int)shape.nodes.size()) continue;
//            const Point& a = shape.nodes[seg.a].pos; const Point& b = shape.nodes[seg.b].pos;
//            out << "      <line x1=\"" << a.x << "\" y1=\"" << -a.y << "\" x2=\"" << b.x << "\" y2=\"" << -b.y << "\" stroke=\"" << stroke << "\" stroke-width=\"" << std::max(0.1f, routeWidth) << "\"/>\n";
//        }
//        out << "    </g>\n";
//    }
//    out << "  </g>\n  <g id=\"stations\">\n";
//    for (int ni = 0; ni < (int)shape.nodes.size(); ++ni) {
//        const auto& node = shape.nodes[ni];
//        if (!isStationLike(node.type) || node.type == ShapeNodeType::PseudoStation) continue;
//        const auto& shapeStyle = isTransferStationNode(shape, ni) ? style.transferStationShape : style.normalStationShape;
//        std::vector<TransitMapStyle::StationPrimitiveStyle> fallback;
//        const auto* primitives = &shapeStyle.primitives;
//        if (primitives->empty()) {
//            TransitMapStyle::StationPrimitiveStyle p;
//            p.offsetX = shapeStyle.offsetX; p.offsetY = shapeStyle.offsetY;
//            p.superellipse.width = std::max(1.0f, shapeStyle.width);
//            p.superellipse.height = std::max(1.0f, shapeStyle.height);
//            p.superellipse.n = 4.0f; p.fillColorHex = shapeStyle.fillColorHex; p.pathWidth = shapeStyle.pathWidth; p.pathColorHex = shapeStyle.pathColorHex;
//            fallback.push_back(p); primitives = &fallback;
//        }
//        out << "    <g";
//        if (!node.name.empty()) out << " data-name=\"" << xmlEscape(node.name) << "\"";
//        if (!node.station_id.empty()) out << " data-station-id=\"" << xmlEscape(node.station_id) << "\"";
//        out << ">\n";
//        for (const auto& prim : *primitives) {
//            std::string stroke = "#000000";
//            if (prim.pathColorHex != "route_color") parseHexColorForSvg(prim.pathColorHex, stroke);
//            std::string fill = "#FFFFFF"; parseHexColorForSvg(prim.fillColorHex, fill);
//            out << "      <path d=\"" << superellipsePath(node.pos.x + prim.offsetX, node.pos.y + prim.offsetY, prim.superellipse.width, prim.superellipse.height, prim.superellipse.n, prim.theta)
//                << "\" fill=\"" << fill << "\" stroke=\"" << stroke << "\" stroke-width=\"" << std::max(0.0f, prim.pathWidth) << "\"/>\n";
//        }
//        out << "    </g>\n";
//    }
//    out << "  </g>\n</svg>\n";
//    return true;
//}

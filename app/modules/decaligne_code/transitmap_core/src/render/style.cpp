#include <type_traits>
#include <variant>
#include "render/style.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <unordered_map>
#include <vector>

#include "json.hpp"

#if !defined(TRANSIT_HEADLESS) && __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define TRANSIT_STYLE_OPENCV 1
#else
#define TRANSIT_STYLE_OPENCV 0
#endif

using json = nlohmann::json;

namespace {
    constexpr double kPi = 3.14159265358979323846;

    static double normalizeUndirectedAngle(double angle) {
        while (angle < 0.0) angle += kPi;
        while (angle >= kPi) angle -= kPi;
        return angle;
    }

    static double angularDistanceUndirected(double a, double b) {
        double d = std::fabs(normalizeUndirectedAngle(a) - normalizeUndirectedAngle(b));
        return std::min(d, kPi - d);
    }

    static bool jsonPoint2(const json& value, Point& out) {
        if (!value.is_array() || value.size() < 2 || !value.at(0).is_number() || !value.at(1).is_number()) {
            return false;
        }
        out.x = value.at(0).get<double>();
        out.y = value.at(1).get<double>();
        return true;
    }

    static void appendUniqueBendAngle(std::vector<double>& angles, double angle) {
        angle = normalizeUndirectedAngle(angle);
        constexpr double minSeparation = 5.0 * kPi / 180.0;
        for (double existing : angles) {
            if (angularDistanceUndirected(existing, angle) < minSeparation) return;
        }
        angles.push_back(angle);
    }

    static bool parseRouteGeometryStyle(const json& root, TransitMapStyle::RouteGeometryStyle& out, TransitMapStyle::RouteStyle& routeStyle) {
        out = {};
        routeStyle.bendStyles.clear();

        const json explicitGeometry = root.value("routeGeometry", json::object());
        const json explicitAngles = explicitGeometry.value("bendAnglesRad", json::array());
        for (const auto& angleJson : explicitAngles) {
            if (angleJson.is_number()) appendUniqueBendAngle(out.bendAnglesRad, angleJson.get<double>());
        }

        const json fittedRoutes = root.value("routes", json::array());
        for (const auto& routeJson : fittedRoutes) {
            Point p0{}, p1{};
            if (!jsonPoint2(routeJson.value("p0", json::array()), p0)
                || !jsonPoint2(routeJson.value("p1", json::array()), p1)) {
                continue;
            }
            const double dx = p1.x - p0.x;
            const double dy = p1.y - p0.y;
            if ((dx * dx + dy * dy) <= 1e-12) continue;
            appendUniqueBendAngle(out.bendAnglesRad, std::atan2(dy, dx));
        }

        for (double angle : out.bendAnglesRad) {
            TransitMapStyle::RouteGeometryStyle::BendStyle bend;
            bend.angleRad = angle;
            routeStyle.bendStyles.push_back(bend);
        }
        const json route = root.value("route", json::object());
        const json bends = route.value("bendStyles", root.value("bendStyles", json::array()));
        for (const auto& bendJson : bends) {
            if (!bendJson.is_object()) continue;
            TransitMapStyle::RouteGeometryStyle::BendStyle bend;
            bend.angleRad = bendJson.value("angleRad", bendJson.value("angle", 0.0));
            json c1 = bendJson.value("control1", json::array({ 0.35, 0.0 }));
            json c2 = bendJson.value("control2", json::array({ 0.65, 1.0 }));
            jsonPoint2(c1, bend.control1);
            jsonPoint2(c2, bend.control2);
            routeStyle.bendStyles.push_back(bend);
            appendUniqueBendAngle(out.bendAnglesRad, bend.angleRad);
        }
        out.enabled = !routeStyle.bendStyles.empty();
        return out.enabled;
    }

    [[maybe_unused]] static void applyRouteGeometryStyle(const TransitMapStyle::RouteGeometryStyle& geometryStyle, GeoData& ioData) {
        if (!geometryStyle.enabled || geometryStyle.bendAnglesRad.empty()) return;

        auto closestAngle = [&](double angle) {
            double best = normalizeUndirectedAngle(angle);
            double bestDistance = std::numeric_limits<double>::max();
            for (double candidate : geometryStyle.bendAnglesRad) {
                const double distance = angularDistanceUndirected(angle, candidate);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = normalizeUndirectedAngle(candidate);
                }
            }
            const double directionDelta = std::fabs(std::atan2(std::sin(angle - best), std::cos(angle - best)));
            const double flippedDelta = std::fabs(std::atan2(std::sin(angle - (best + kPi)), std::cos(angle - (best + kPi))));
            return flippedDelta < directionDelta ? best + kPi : best;
            };

        for (auto& route : ioData.routes) {
            for (auto& segment : route.segments) {
                if (segment.size() < 2) continue;
                for (size_t i = 1; i < segment.size(); ++i) {
                    const Point previous = segment[i - 1];
                    const Point current = segment[i];
                    const double dx = current.x - previous.x;
                    const double dy = current.y - previous.y;
                    const double length = std::hypot(dx, dy);
                    if (length <= 1e-9) continue;
                    const double snapped = closestAngle(std::atan2(dy, dx));
                    segment[i].x = previous.x + std::cos(snapped) * length;
                    segment[i].y = previous.y + std::sin(snapped) * length;
                }
            }
        }
    }


    static std::string rgbToHex(int r, int g, int b) {
        char buf[8]{};

        std::snprintf(
            buf,
            sizeof(buf),
            "#%02X%02X%02X",
            std::clamp(r, 0, 255),
            std::clamp(g, 0, 255),
            std::clamp(b, 0, 255)
        );

        return buf;
    }

    static bool hexToRgb01(const std::string& color, float out[3]) {
        if (color.empty()) return false;
        if (color[0] == '#') {
            if (color.size() != 7) return false;
            int r = 0, g = 0, b = 0;
            if (std::sscanf(color.c_str() + 1, "%02x%02x%02x", &r, &g, &b) != 3) return false;
            out[0] = r / 255.0f; out[1] = g / 255.0f; out[2] = b / 255.0f;
            return true;
        }
        int r = 0, g = 0, b = 0;
        if (std::sscanf(color.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3 ||
            std::sscanf(color.c_str(), "rgb(%d, %d, %d)", &r, &g, &b) == 3 ||
            std::sscanf(color.c_str(), "(%d,%d,%d)", &r, &g, &b) == 3 ||
            std::sscanf(color.c_str(), "(%d, %d, %d)", &r, &g, &b) == 3) {
            out[0] = std::clamp(r, 0, 255) / 255.0f;
            out[1] = std::clamp(g, 0, 255) / 255.0f;
            out[2] = std::clamp(b, 0, 255) / 255.0f;
            return true;
        }
        return false;
    }

} // namespace

bool StyleExtractor::extractFromImage(
    const std::string& imagePath,
    TransitMapStyle& outStyle,
    std::string& outError
) {
#if !TRANSIT_STYLE_OPENCV
    (void)imagePath;
    (void)outStyle;

    outError = "OpenCV not available at build time.";
    return false;
#else
    try {
        cv::Mat img = cv::imread(imagePath, cv::IMREAD_COLOR);

        if (img.empty()) {
            outError = "Failed to load image: " + imagePath;
            return false;
        }

        const cv::Vec3b bg = img.at<cv::Vec3b>(0, 0);
        outStyle.backgroundHex = rgbToHex(bg[2], bg[1], bg[0]);

        cv::Mat pixels = img.reshape(1, img.rows * img.cols);
        pixels.convertTo(pixels, CV_32F);

        constexpr int k = 6;

        cv::Mat labels;
        cv::Mat centers;

        cv::kmeans(
            pixels,
            k,
            labels,
            cv::TermCriteria(
                cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER,
                20,
                1.0
            ),
            3,
            cv::KMEANS_PP_CENTERS,
            centers
        );

        std::vector<int> counts(k, 0);

        for (int i = 0; i < labels.rows; ++i) {
            const int label = labels.at<int>(i, 0);

            if (label >= 0 && label < k) {
                counts[label]++;
            }
        }

        std::vector<std::pair<int, std::string>> ranked;

        for (int i = 0; i < k; ++i) {
            const float b = centers.at<float>(i, 0);
            const float g = centers.at<float>(i, 1);
            const float r = centers.at<float>(i, 2);

            ranked.push_back({
                counts[i],
                rgbToHex(
                    static_cast<int>(std::round(r)),
                    static_cast<int>(std::round(g)),
                    static_cast<int>(std::round(b))
                )
                });
        }

        std::sort(
            ranked.begin(),
            ranked.end(),
            [](const auto& a, const auto& b) {
                return a.first > b.first;
            }
        );

        outStyle.paletteHex.clear();

        for (const auto& kv : ranked) {
            const std::string& color = kv.second;

            if (color != outStyle.backgroundHex) {
                outStyle.paletteHex.push_back(color);
            }

            if (outStyle.paletteHex.size() >= 8) {
                break;
            }
        }

        outStyle.routeWidth = 8.0f;
        outStyle.routeSpacing = 7.0f;

        return true;
    }
    catch (const cv::Exception& e) {
        outError = std::string("OpenCV error: ") + e.what();
        return false;
    }
    catch (const std::exception& e) {
        outError = std::string("Style extraction error: ") + e.what();
        return false;
    }
#endif
}

bool StyleIO::save(
    const std::string& path,
    const TransitMapStyle& style,
    std::string& outError
) {
    json j;

    j["background"] = {
        {"color", style.backgroundHex}
    };

    j["route"] = {
        {"width", style.routeWidth},
        {"spacing", style.routeSpacing}
    };
    if (!style.routeWidthsByColor.empty()) {
        j["route"]["widthsByColor"] = json::array();
        for (const auto& routeStyle : style.routeWidthsByColor) {
            j["route"]["widthsByColor"].push_back({
                {"color", {routeStyle.color[0], routeStyle.color[1], routeStyle.color[2]}},
                {"width", routeStyle.width}
                });
        }
    }
    if (style.routeGeometry.enabled && !style.routeGeometry.bendAnglesRad.empty()) {
        j["routeGeometry"]["bendAnglesRad"] = style.routeGeometry.bendAnglesRad;
    }
    if (!style.routeStyle.bendStyles.empty()) {
        j["route"]["bendStyles"] = json::array();
        for (const auto& bend : style.routeStyle.bendStyles) {
            j["route"]["bendStyles"].push_back({
                {"angleRad", bend.angleRad},
                {"control1", {bend.control1.x, bend.control1.y}},
                {"control2", {bend.control2.x, bend.control2.y}}
                });
        }
    }

    auto saveShape = [](const StationShapeStyle& sh) {
        json n;
        std::visit([&](const auto& stationStyle) {
            using T = std::decay_t<decltype(stationStyle)>;
            if constexpr (std::is_same_v<T, CircleStationStyle>) {
                n["type"] = n["shape"] = "circle";
                n["radius"] = stationStyle.radius;
            }
            else {
                n["type"] = n["shape"] = stationStyle.cornerRadius == 0.0f ? "rectangle" : "rounded_rectangle";
                n["w"] = stationStyle.width;
                n["h"] = stationStyle.height;
                n["r"] = stationStyle.cornerRadius;
                n["rotation"] = stationStyle.rotationDegrees;
                n["normal_offset"] = stationStyle.normalOffset;
            }
            n["fill_color"] = stationStyle.stationstyle.fillColorHex;
            n["path_width"] = stationStyle.stationstyle.pathWidth;
            n["path_color"] = stationStyle.stationstyle.pathColorHex;
            n["fill_uses_route_color"] = stationStyle.stationstyle.fillUsesRouteColor;
            n["path_uses_route_color"] = stationStyle.stationstyle.pathUsesRouteColor;
            }, sh);
        return n;
        };
    j["station"]["normal"] = saveShape(style.normalStationShape);
    j["station"]["transfer"] = saveShape(style.transferStationShape);

    j["palette"] = style.paletteHex;
    j["routeColors"] = style.routeColorsById;

    std::ofstream ofs(path);

    if (!ofs) {
        outError = "Failed to open style output: " + path;
        return false;
    }

    ofs << std::setw(2) << j << "\n";
    return true;
}

bool StyleIO::load(
    const std::string& path,
    TransitMapStyle& outStyle,
    std::string& outError
) {
    std::ifstream ifs(path);

    if (!ifs) {
        outError = "Failed to open style file: " + path;
        return false;
    }

    try {
        json j;
        ifs >> j;

        outStyle.backgroundHex = j.value("background", json::object()).value("color", "#FFFFFF");

        outStyle.routeWidth = j.value("route", json::object()).value("width", 6.0f);
        outStyle.routeSpacing = j.value("route", json::object()).value("spacing", 7.0f);
        outStyle.routeWidthsByColor.clear();
        const json routeWidthStyles = j.value("route", json::object()).value("widthsByColor", json::array());
        for (const auto& routeStyleJson : routeWidthStyles) {
            const json color = routeStyleJson.value("color", json::array());
            if (!color.is_array() || color.size() < 3
                || !color.at(0).is_number() || !color.at(1).is_number() || !color.at(2).is_number()) {
                continue;
            }
            TransitMapStyle::RouteWidthStyle routeStyle;
            routeStyle.color[0] = std::clamp(color.at(0).get<float>(), 0.0f, 1.0f);
            routeStyle.color[1] = std::clamp(color.at(1).get<float>(), 0.0f, 1.0f);
            routeStyle.color[2] = std::clamp(color.at(2).get<float>(), 0.0f, 1.0f);
            routeStyle.width = std::clamp(routeStyleJson.value("width", outStyle.routeWidth), 1.0f, 64.0f);
            outStyle.routeWidthsByColor.push_back(routeStyle);
        }
        parseRouteGeometryStyle(j, outStyle.routeGeometry, outStyle.routeStyle);

        const json station = j.value("station", json::object());
        auto loadShape = [](const json& n, StationShapeStyle& sh) {
            const std::string shape = n.value("type", n.value("shape", std::string("circle")));
            StationStyle stationstyle;
            stationstyle.fillColorHex = n.value("fill_color", std::string("#FFFFFF"));
            stationstyle.pathWidth = n.value("path_width", 1.5f);
            stationstyle.pathColorHex = n.value("path_color", std::string("#000000"));
            stationstyle.fillUsesRouteColor = n.value("fill_uses_route_color", false);
            stationstyle.pathUsesRouteColor = n.value("path_uses_route_color", false);
            if (shape == "rounded_rectangle" || shape == "rectangle") {
                RoundedRectangleStationStyle rectangle;
                rectangle.width = n.value("w", 14.0f);
                rectangle.height = n.value("h", 8.0f);
                rectangle.cornerRadius = shape == "rectangle" ? 0.0f : n.value("r", 3.0f);
                rectangle.rotationDegrees = n.value("rotation", 0.0f);
                rectangle.normalOffset = n.value("normal_offset", 0.0f);
                rectangle.stationstyle = std::move(stationstyle);
                sh = rectangle;
            }
            else {
                CircleStationStyle circle;
                circle.radius = n.value("radius", 4.0f);
                circle.stationstyle = std::move(stationstyle);
                sh = circle;
            }
            };
        loadShape(station.value("normal", json::object()), outStyle.normalStationShape);
        loadShape(station.value("transfer", json::object()), outStyle.transferStationShape);

        outStyle.labelStyle = {};
        const json label = j.value("label_style", json());
        if (label.is_object()) {
            const json color = label.value("color", json::array());
            if (label.value("font_family", std::string()).size() && label.value("font_size", 0.0f) > 0.0f
                && color.is_array() && color.size() >= 3) {
                outStyle.labelStyle.available = true;
                outStyle.labelStyle.fontFamily = label.value("font_family", std::string());
                outStyle.labelStyle.fontSize = std::clamp(label.value("font_size", 13.0f), 6.0f, 96.0f);
                for (int i = 0; i < 3; ++i) outStyle.labelStyle.color[i] = std::clamp(color.at(i).get<float>(), 0.0f, 1.0f);
            }
        }








        outStyle.paletteHex = j.value("palette", std::vector<std::string>{});

        outStyle.routeColorsById = j.value("routeColors", std::unordered_map<std::string, std::string>{});

        return true;
    }
    catch (const std::exception& e) {
        outError = std::string("Failed to parse style file: ") + e.what();
        return false;
    }
}

void StyleApplicator::applyToGeoData(
    const TransitMapStyle& style,
    GeoData& ioData
) {
    for (auto& route : ioData.routes) {
        if (style.routeWidthsByColor.empty()) {
            route.route_width = style.routeWidth;
        }
        else {
            constexpr double colorMatchThreshold2 = 0.18 * 0.18;
            const TransitMapStyle::RouteWidthStyle* bestStyle = nullptr;
            double bestDistance2 = std::numeric_limits<double>::max();
            for (const auto& routeStyle : style.routeWidthsByColor) {
                const double dr = static_cast<double>(route.color[0]) - static_cast<double>(routeStyle.color[0]);
                const double dg = static_cast<double>(route.color[1]) - static_cast<double>(routeStyle.color[1]);
                const double db = static_cast<double>(route.color[2]) - static_cast<double>(routeStyle.color[2]);
                const double distance2 = dr * dr + dg * dg + db * db;
                if (distance2 < bestDistance2) {
                    bestDistance2 = distance2;
                    bestStyle = &routeStyle;
                }
            }
            if (bestStyle && bestDistance2 <= colorMatchThreshold2) {
                route.route_width = bestStyle->width;
            }
        }

        const auto widthOverrideIt = style.routeWidthsById.find(route.id);
        if (widthOverrideIt != style.routeWidthsById.end()) {
            route.route_width = widthOverrideIt->second;
        }

        const auto it = style.routeColorsById.find(route.id);

        if (it == style.routeColorsById.end()) {
            continue;
        }

        float rgb[3];

        if (!hexToRgb01(it->second, rgb)) {
            continue;
        }

        route.color[0] = rgb[0];
        route.color[1] = rgb[1];
        route.color[2] = rgb[2];
    }
}

std::vector<TransitMapStyle::DisplayRoute> StyleApplicator::buildDisplayRoutes(
    const TransitMapStyle& style,
    const GeoData& geometryData
) {
    auto samePoint = [](const Point& a, const Point& b) {
        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        return dx * dx + dy * dy <= 1e-12;
        };
    auto appendPoint = [&](std::vector<Point>& out, const Point& point) {
        if (out.empty() || !samePoint(out.back(), point)) out.push_back(point);
        };
    auto displaySegmentFromGeometry = [&](const std::vector<Point>& segment) {
        std::vector<Point> displaySegment;
        for (const Point& p : segment) {
            appendPoint(displaySegment, p);
        }
        return displaySegment;
        };
    auto signedTurn = [](const Point& a, const Point& b, const Point& c) {
        const double ux = b.x - a.x, uy = b.y - a.y;
        const double vx = c.x - b.x, vy = c.y - b.y;
        return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        };
    auto bestBend = [&](double turn) -> const TransitMapStyle::RouteGeometryStyle::BendStyle* {
        const double magnitude = std::fabs(turn);
        const TransitMapStyle::RouteGeometryStyle::BendStyle* best = nullptr;
        double bestDistance = std::numeric_limits<double>::max();
        for (const auto& bend : style.routeStyle.bendStyles) {
            const double distance = std::fabs(std::fabs(bend.angleRad) - magnitude);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = &bend;
            }
        }
        constexpr double kToleranceRad = 5.0 * kPi / 180.0;
        return bestDistance <= kToleranceRad ? best : nullptr;
        };
    auto bezier = [](const Point& p0, const Point& p1, const Point& p2, const Point& p3, double t) {
        const double u = 1.0 - t;
        return Point{
            u * u * u * p0.x + 3.0 * u * u * t * p1.x + 3.0 * u * t * t * p2.x + t * t * t * p3.x,
            u * u * u * p0.y + 3.0 * u * u * t * p1.y + 3.0 * u * t * t * p2.y + t * t * t * p3.y
        };
        };
    auto normalize = [](Point p) {
        const double len = std::hypot(p.x, p.y);
        return len > 1e-9 ? Point{ p.x / len, p.y / len } : Point{ 1.0, 0.0 };
        };

    GeoData styledData = geometryData;
    StyleApplicator::applyToGeoData(style, styledData);

    std::vector<TransitMapStyle::DisplayRoute> displayRoutes;
    displayRoutes.reserve(styledData.routes.size());
    for (const Route& route : styledData.routes) {
        TransitMapStyle::DisplayRoute display;
        display.routeId = route.id;
        display.width = route.route_width;
        display.color[0] = route.color[0];
        display.color[1] = route.color[1];
        display.color[2] = route.color[2];
        display.segments.reserve(route.segments.size());
        for (const auto& geometrySegment : route.segments) {
            std::vector<Point> displaySegment = displaySegmentFromGeometry(geometrySegment);
            if (displaySegment.size() < 2) continue;

            if (displaySegment.size() >= 3 && !style.routeStyle.bendStyles.empty()) {
                std::vector<Point> bent;
                bent.push_back(displaySegment.front());
                for (size_t i = 1; i + 1 < displaySegment.size(); ++i) {
                    const Point a = displaySegment[i - 1], b = displaySegment[i], c = displaySegment[i + 1];
                    const auto* bend = bestBend(signedTurn(a, b, c));
                    if (!bend) {
                        bent.push_back(b);
                        continue;
                    }
                    Point in = normalize({ b.x - a.x, b.y - a.y });
                    Point out = normalize({ c.x - b.x, c.y - b.y });
                    const double trim = std::min(std::hypot(b.x - a.x, b.y - a.y), std::hypot(c.x - b.x, c.y - b.y)) * 0.25;
                    Point p0{ b.x - in.x * trim, b.y - in.y * trim };
                    Point p3{ b.x + out.x * trim, b.y + out.y * trim };
                    Point p1{ p0.x + in.x * trim * bend->control1.x + out.x * trim * bend->control1.y,
                              p0.y + in.y * trim * bend->control1.x + out.y * trim * bend->control1.y };
                    Point p2{ p0.x + in.x * trim * bend->control2.x + out.x * trim * bend->control2.y,
                              p0.y + in.y * trim * bend->control2.x + out.y * trim * bend->control2.y };
                    appendPoint(bent, p0);
                    for (int step = 1; step <= 12; ++step) appendPoint(bent, bezier(p0, p1, p2, p3, step / 12.0));
                }
                appendPoint(bent, displaySegment.back());
                displaySegment = std::move(bent);
            }
            display.segments.push_back(std::move(displaySegment));
        }
        displayRoutes.push_back(std::move(display));
    }
    return displayRoutes;
}
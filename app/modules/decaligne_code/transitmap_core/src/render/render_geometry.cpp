#include <climits>
#include "render/render_geometry.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>
#include <map>

static Point normalizeP(Point p) { double l=std::hypot(p.x,p.y); return l>1e-12 ? Point{p.x/l,p.y/l} : Point{}; }
static double dotP(Point a, Point b) { return a.x*b.x+a.y*b.y; }

namespace {
    constexpr double kStyledEps = 1e-6;
    constexpr int kBezierSamples = 16;
    constexpr bool kDebugStyledRouteOverlays = false;
    constexpr double kStationMergeEpsilonPixels = 1.0;



    constexpr double kTransferStationIntersectionPathTWindow = 0.03;
    using DisplayGeometry = std::vector<std::vector<std::vector<Point>>>;

    static Point addP(Point a, Point b) { return { a.x + b.x, a.y + b.y }; }
    static Point subP(Point a, Point b) { return { a.x - b.x, a.y - b.y }; }
    static Point mulP(Point a, double s) { return { a.x * s, a.y * s }; }
    static double lenP(Point a) { return std::sqrt(a.x * a.x + a.y * a.y); }
    static double crossP(Point a, Point b) { return a.x * b.y - a.y * b.x; }
    static bool finiteP(Point p) { return std::isfinite(p.x) && std::isfinite(p.y); }
    static double clampD(double v, double lo, double hi) { return std::max(lo, std::min(hi, v)); }

    static Point evaluatePathAtRelativeArcLength(const std::vector<Point>& path, double pathT) {
        if (path.empty()) return {};
        if (path.size() == 1) return path.front();
        pathT = clampD(pathT, 0.0, 1.0);
        double total = 0.0;
        for (size_t i = 1; i < path.size(); ++i) total += lenP(subP(path[i], path[i - 1]));
        if (total <= kStyledEps) return path.front();
        const double target = pathT * total;
        double before = 0.0;
        for (size_t i = 1; i < path.size(); ++i) {
            const Point delta = subP(path[i], path[i - 1]);
            const double length = lenP(delta);
            if (length > kStyledEps && target <= before + length) return addP(path[i - 1], mulP(delta, (target - before) / length));
            before += length;
        }
        return path.back();
    }





    static bool segmentIntersection(Point a, Point b, Point c, Point d, Point& intersection, double& outT, double& outU) {
        const Point r = subP(b, a);
        const Point s = subP(d, c);
        const double denominator = crossP(r, s);
        if (std::abs(denominator) <= kStyledEps) return false;

        const Point cMinusA = subP(c, a);
        const double t = crossP(cMinusA, s) / denominator;
        const double u = crossP(cMinusA, r) / denominator;
        if (t < -kStyledEps || t > 1.0 + kStyledEps || u < -kStyledEps || u > 1.0 + kStyledEps) return false;

        intersection = addP(a, mulP(r, clampD(t, 0.0, 1.0)));
        if (!finiteP(intersection)) return false;
        outT = clampD(t, 0.0, 1.0);
        outU = clampD(u, 0.0, 1.0);
        return true;
    }

    static double relativeArcLengthAtSegment(const std::vector<Point>& path, size_t segmentIndex, double segmentT) {
        if (path.size() < 2 || segmentIndex >= path.size() - 1) return 0.0;
        double totalLength = 0.0;
        double lengthBefore = 0.0;
        for (size_t i = 1; i < path.size(); ++i) {
            const double segmentLength = lenP(subP(path[i], path[i - 1]));
            totalLength += segmentLength;
            if (i - 1 < segmentIndex) lengthBefore += segmentLength;
        }
        if (totalLength <= kStyledEps) return 0.0;
        const double segmentLength = lenP(subP(path[segmentIndex + 1], path[segmentIndex]));
        return clampD((lengthBefore + clampD(segmentT, 0.0, 1.0) * segmentLength) / totalLength, 0.0, 1.0);
    }

    static std::vector<StyledShape::NormalStation> updateNormalStations(const StyledShape& styledShape, const DisplayGeometry& finalRoutePaths) {
        std::vector<StyledShape::NormalStation> output;
        output.reserve(styledShape.normal_stations.size());
        for (const auto& canonical : styledShape.normal_stations) {
            if (canonical.routeIndex < 0 || canonical.routeIndex >= static_cast<int>(finalRoutePaths.size()) ||
                canonical.pathIndex < 0 || canonical.pathIndex >= static_cast<int>(finalRoutePaths[canonical.routeIndex].size()) ||
                !std::isfinite(canonical.pathT)) {
                std::cerr << "[updateNormalStations] invalid route/path metadata for station=" << canonical.id << "\n";
                continue;
            }
            const auto& path = finalRoutePaths[canonical.routeIndex][canonical.pathIndex];
            if (path.size() < 2) { std::cerr << "[updateNormalStations] invalid final path for station=" << canonical.id << "\n"; continue; }
            StyledShape::NormalStation station = canonical;
            station.pos = evaluatePathAtRelativeArcLength(path, canonical.pathT);
            if (!finiteP(station.pos)) { std::cerr << "[updateNormalStations] non-finite position for station=" << canonical.id << "\n"; continue; }
            output.push_back(std::move(station));
        }
        return output;
    }




    static std::vector<StyledShape::TransferStation> updateTransferStations(
        const StyledShape& styledShape, const DisplayGeometry& finalRoutePaths, double mergeEpsilon) {
        struct PositionedTransfer {
            StyledShape::TransferStation::Position metadata;
            Point pos;
        };

        std::vector<StyledShape::TransferStation> output;
        for (const auto& source : styledShape.transfer_stations) {
            std::vector<PositionedTransfer> positions;
            positions.reserve(source.positions.size());
            for (const auto& position : source.positions) {
                if (position.routeIndex < 0 || position.routeIndex >= static_cast<int>(finalRoutePaths.size()) ||
                    position.pathIndex < 0 || position.pathIndex >= static_cast<int>(finalRoutePaths[position.routeIndex].size()) ||
                    !std::isfinite(position.pathT)) {
                    std::cerr << "[updateTransferStations] invalid route/path metadata for station=" << source.id << "\n";
                    continue;
                }
                const auto& path = finalRoutePaths[position.routeIndex][position.pathIndex];
                if (path.size() < 2) {
                    std::cerr << "[updateTransferStations] invalid final path for station=" << source.id << "\n";
                    continue;
                }
                const Point pos = evaluatePathAtRelativeArcLength(path, position.pathT);
                if (finiteP(pos)) positions.push_back({ position, pos });
            }

            if (positions.empty()) {
                std::cerr << "[updateTransferStations] no final route position for station=" << source.id << "\n";
                continue;
            }






            std::vector<std::vector<PositionedTransfer>> intersectionGroups;
            for (size_t first = 0; first < positions.size(); ++first) {
                const auto& firstPath = finalRoutePaths[positions[first].metadata.routeIndex][positions[first].metadata.pathIndex];
                for (size_t second = first + 1; second < positions.size(); ++second) {
                    const auto& secondPath = finalRoutePaths[positions[second].metadata.routeIndex][positions[second].metadata.pathIndex];
                    for (size_t ai = 1; ai < firstPath.size(); ++ai) {
                        for (size_t bi = 1; bi < secondPath.size(); ++bi) {
                            Point intersection;
                            double firstSegmentT = 0.0;
                            double secondSegmentT = 0.0;
                            if (!segmentIntersection(firstPath[ai - 1], firstPath[ai], secondPath[bi - 1], secondPath[bi], intersection, firstSegmentT, secondSegmentT)) continue;
                            const double firstHitPathT = relativeArcLengthAtSegment(firstPath, ai - 1, firstSegmentT);
                            const double secondHitPathT = relativeArcLengthAtSegment(secondPath, bi - 1, secondSegmentT);
                            if (std::abs(firstHitPathT - positions[first].metadata.pathT) > kTransferStationIntersectionPathTWindow ||
                                std::abs(secondHitPathT - positions[second].metadata.pathT) > kTransferStationIntersectionPathTWindow) continue;
                            PositionedTransfer hit{ positions[first].metadata, intersection };
                            auto group = std::find_if(intersectionGroups.begin(), intersectionGroups.end(), [&](const auto& existing) {
                                return lenP(subP(intersection, existing.front().pos)) <= mergeEpsilon;
                                });
                            if (group == intersectionGroups.end()) {
                                intersectionGroups.push_back({ hit, { positions[second].metadata, intersection } });
                            }
                            else {
                                const auto addMetadata = [&](const StyledShape::TransferStation::Position& metadata) {
                                    const bool alreadyPresent = std::any_of(group->begin(), group->end(), [&](const PositionedTransfer& existing) {
                                        return existing.metadata.routeIndex == metadata.routeIndex &&
                                            existing.metadata.pathIndex == metadata.pathIndex &&
                                            std::abs(existing.metadata.pathT - metadata.pathT) <= kStyledEps;
                                        });
                                    if (!alreadyPresent) group->push_back({ metadata, intersection });
                                    };
                                addMetadata(positions[first].metadata);
                                addMetadata(positions[second].metadata);
                            }
                        }
                    }
                }
            }




            std::vector<PositionedTransfer> candidates;
            if (intersectionGroups.empty()) {
                candidates = positions;
            }
            else {
                for (const auto& group : intersectionGroups) {
                    candidates.insert(candidates.end(), group.begin(), group.end());
                }
            }
            std::vector<std::vector<PositionedTransfer>> clusters;
            for (const auto& position : candidates) {
                auto cluster = std::find_if(clusters.begin(), clusters.end(), [&](const auto& existing) {
                    return lenP(subP(position.pos, existing.front().pos)) <= mergeEpsilon;
                    });
                if (cluster == clusters.end()) clusters.push_back({ position });
                else cluster->push_back(position);
            }
            for (size_t i = 0; i < clusters.size(); ++i) {
                const auto& cluster = clusters[i];
                Point sum{};
                StyledShape::TransferStation station = source;
                station.positions.clear();
                for (const auto& position : cluster) {
                    sum = addP(sum, position.pos);
                    station.positions.push_back(position.metadata);
                }
                station.pos = mulP(sum, 1.0 / static_cast<double>(cluster.size()));
                if (clusters.size() > 1) station.id = source.id + "_split_" + std::to_string(i);
                output.push_back(std::move(station));
            }
        }
        return output;
    }

    struct ValidSharedRef { StyledShape::StyledRouteSegmentRef ref; Point a; Point b; Point dir; bool reversed = false; };

    static DisplayGeometry buildCanonicalScreenGeometry(const StyledShape& s, const Camera& cam) {
        DisplayGeometry screen = s.routes_geometry_vertices;
        for (auto& route : screen) {
            for (auto& path : route) {
                for (Point& p : path) p = { cam.sx(p.x), cam.sy(p.y) };
            }
        }
        return screen;
    }

    static bool spacingForRoutes(const StyledShape& s, int i, int j, double& out) {
        const int n = static_cast<int>(s.routes_geometry_vertices.size());
        if (static_cast<int>(s.routes_spacing.size()) != n) { std::cerr << "[drawStyledShape] routes_spacing row count mismatch\n"; out = 0; return false; }
        for (int r = 0; r < n; ++r) if (static_cast<int>(s.routes_spacing[r].size()) != n) { std::cerr << "[drawStyledShape] routes_spacing column count mismatch at row " << r << "\n"; out = 0; return false; }
        const double a = s.routes_spacing[i][j];
        const double b = s.routes_spacing[j][i];
        if (std::abs(a - b) > 1e-3) std::cerr << "[drawStyledShape] routes_spacing asymmetric for routes " << i << "," << j << ": " << a << " vs " << b << "\n";
        out = 0.5 * (a + b);
        return true;
    }

    static bool validateSharedRef(const StyledShape& s, const DisplayGeometry& canonicalScreen, const StyledShape::StyledRouteSegmentRef& r, int bundle, ValidSharedRef& out) {
        if (r.routeIndex < 0 || r.routeIndex >= static_cast<int>(s.routes_geometry_vertices.size()) ||
            r.pathIndex < 0 || r.pathIndex >= static_cast<int>(s.routes_geometry_vertices[r.routeIndex].size())) {
            std::cerr << "[drawStyledShape] invalid shared ref bundle " << bundle << " route " << r.routeIndex << " path " << r.pathIndex << " points " << r.pointIndexA << "," << r.pointIndexB << "\n"; return false;
        }
        const auto& path = canonicalScreen[r.routeIndex][r.pathIndex];
        if (r.pointIndexA < 0 || r.pointIndexB < 0 || r.pointIndexA >= static_cast<int>(path.size()) || r.pointIndexB >= static_cast<int>(path.size()) || r.pointIndexA == r.pointIndexB || std::abs(r.pointIndexA - r.pointIndexB) != 1) {
            std::cerr << "[drawStyledShape] invalid non-consecutive shared ref bundle " << bundle << " route " << r.routeIndex << " path " << r.pathIndex << " points " << r.pointIndexA << "," << r.pointIndexB << "\n"; return false;
        }
        Point d = subP(path[r.pointIndexB], path[r.pointIndexA]);
        if (lenP(d) < kStyledEps) { std::cerr << "[drawStyledShape] zero-length shared ref bundle " << bundle << " route " << r.routeIndex << " path " << r.pathIndex << "\n"; return false; }
        out = { r, path[r.pointIndexA], path[r.pointIndexB], normalizeP(d), false };
        return true;
    }

    struct SpacingBundle {
        std::vector<ValidSharedRef> references;
        std::vector<int> routes;
        std::unordered_map<int, double> laneOffsets;
        Point normal{};
    };

    struct PointCorrection { Point sum{}; int count = 0; };

    static DisplayGeometry buildSpacingAdjustedGeometry(
        const StyledShape& styledShape,
        const DisplayGeometry& canonicalScreenGeometry) {
        DisplayGeometry displayGeometry = canonicalScreenGeometry;
        std::vector<SpacingBundle> bundles;



        for (int bundleIndex = 0;
            bundleIndex < static_cast<int>(styledShape.shared_indices.size()); ++bundleIndex) {
            SpacingBundle bundle;
            for (const auto& reference : styledShape.shared_indices[bundleIndex]) {
                if(reference.routeIndex>=0&&reference.routeIndex<int(styledShape.routes_is_obstacle.size())&&styledShape.routes_is_obstacle[reference.routeIndex])continue;
                ValidSharedRef valid;
                if (validateSharedRef(styledShape, canonicalScreenGeometry,
                    reference, bundleIndex, valid)) bundle.references.push_back(valid);
            }
            if (bundle.references.size() < 2) continue;
            const Point direction = normalizeP(bundle.references.front().dir);
            bundle.normal = { -direction.y, direction.x };
            for (const auto& reference : bundle.references)
                bundle.routes.push_back(reference.ref.routeIndex);
            std::sort(bundle.routes.begin(), bundle.routes.end());
            bundle.routes.erase(std::unique(bundle.routes.begin(), bundle.routes.end()), bundle.routes.end());
            if (bundle.routes.size() < 2) continue;
            auto loom = styledShape.loomLaneRanks.find(bundleIndex);
            if (loom != styledShape.loomLaneRanks.end()) {
                const auto& ranks = loom->second;
                auto rank = [&](int route) { return ranks.count(route) ? ranks.at(route) : INT_MAX; };
                std::stable_sort(bundle.routes.begin(), bundle.routes.end(),
                    [&](int a, int b) { return rank(a) < rank(b); });
            }

            double offset = 0.0;
            bundle.laneOffsets[bundle.routes.front()] = offset;
            for (size_t i = 1; i < bundle.routes.size(); ++i) {
                double spacing = 0.0;
                spacingForRoutes(styledShape, bundle.routes[i - 1], bundle.routes[i], spacing);
                offset += spacing;
                bundle.laneOffsets[bundle.routes[i]] = offset;
            }
            // The bundle as a whole remains free to translate during solving.
            double mean = 0.0;
            for (const int route : bundle.routes) mean += bundle.laneOffsets[route];
            mean /= static_cast<double>(bundle.routes.size());
            for (const int route : bundle.routes) bundle.laneOffsets[route] -= mean;
            bundles.push_back(std::move(bundle));
        }

        using Corrections = std::vector<std::vector<std::vector<PointCorrection>>>;
        Corrections corrections(displayGeometry.size());
        for (size_t route = 0; route < displayGeometry.size(); ++route) {
            corrections[route].resize(displayGeometry[route].size());
            for (size_t path = 0; path < displayGeometry[route].size(); ++path)
                corrections[route][path].resize(displayGeometry[route][path].size());
        }

        constexpr int maximumIterations = 256;
        constexpr double convergenceTolerance = 1e-4;
        for (int iteration = 0; iteration < maximumIterations; ++iteration) {
            for (auto& route : corrections)
                for (auto& path : route)
                    for (auto& point : path) point = {};

            double maximumError = 0.0;
            // Jacobi bundle projection: every constraint sees the same geometry,
            // and shared vertices receive the mean of all simultaneous updates.
            for (const SpacingBundle& bundle : bundles) {
                std::unordered_map<int, double> currentOffsets;
                std::unordered_map<int, int> offsetCounts;
                for (const auto& reference : bundle.references) {
                    const int route = reference.ref.routeIndex;
                    const int path = reference.ref.pathIndex;
                    for (const int point : { reference.ref.pointIndexA, reference.ref.pointIndexB }) {
                        currentOffsets[route] += dotP(subP(displayGeometry[route][path][point],
                            canonicalScreenGeometry[route][path][point]), bundle.normal);
                        ++offsetCounts[route];
                    }
                }
                double translation = 0.0;
                for (const int route : bundle.routes) {
                    currentOffsets[route] /= std::max(1, offsetCounts[route]);
                    translation += currentOffsets[route] - bundle.laneOffsets.at(route);
                }
                translation /= static_cast<double>(bundle.routes.size());

                for (const auto& reference : bundle.references) {
                    const int route = reference.ref.routeIndex;
                    const double error = bundle.laneOffsets.at(route) + translation - currentOffsets[route];
                    maximumError = std::max(maximumError, std::abs(error));
                    const Point correction = mulP(bundle.normal, error);
                    for (const int point : { reference.ref.pointIndexA, reference.ref.pointIndexB }) {
                        auto& value = corrections[route][reference.ref.pathIndex][point];
                        value.sum = addP(value.sum, correction);
                        ++value.count;
                    }
                }
            }

            for (size_t route = 0; route < displayGeometry.size(); ++route)
                for (size_t path = 0; path < displayGeometry[route].size(); ++path)
                    for (size_t point = 0; point < displayGeometry[route][path].size(); ++point) {
                        const PointCorrection& value = corrections[route][path][point];
                        if (value.count > 0) displayGeometry[route][path][point] = addP(
                            displayGeometry[route][path][point], mulP(value.sum, 1.0 / value.count));
                    }



            for (size_t route = 0; route < displayGeometry.size(); ++route) {
                for (size_t path = 0; path < displayGeometry[route].size(); ++path) {
                    auto& display = displayGeometry[route][path];
                    const auto& canonical = canonicalScreenGeometry[route][path];
                    for (size_t segment = 0; segment + 1 < display.size(); ++segment) {
                        const Point canonicalDelta = subP(canonical[segment + 1], canonical[segment]);
                        const double canonicalLength = lenP(canonicalDelta);
                        if (canonicalLength <= kStyledEps) continue;
                        const Point direction = mulP(canonicalDelta, 1.0 / canonicalLength);
                        const Point delta = subP(display[segment + 1], display[segment]);
                        const Point validDelta = mulP(direction,
                            std::max(canonicalLength * 0.01, dotP(delta, direction)));
                        const Point error = subP(delta, validDelta);
                        maximumError = std::max(maximumError, lenP(error));
                        display[segment] = addP(display[segment], mulP(error, 0.5));
                        display[segment + 1] = subP(display[segment + 1], mulP(error, 0.5));
                    }
                }
            }
            if (maximumError < convergenceTolerance) break;
        }

        // Finish on the hard direction constraint so the renderer can never
        // observe a rotated or reversed segment at the iteration limit.
        for (size_t route = 0; route < displayGeometry.size(); ++route) {
            for (size_t path = 0; path < displayGeometry[route].size(); ++path) {
                auto& display = displayGeometry[route][path];
                const auto& canonical = canonicalScreenGeometry[route][path];
                for (size_t segment = 0; segment + 1 < display.size(); ++segment) {
                    const Point canonicalDelta = subP(canonical[segment + 1], canonical[segment]);
                    const double canonicalLength = lenP(canonicalDelta);
                    if (canonicalLength <= kStyledEps) continue;
                    const Point direction = mulP(canonicalDelta, 1.0 / canonicalLength);
                    const Point delta = subP(display[segment + 1], display[segment]);
                    const double validLength = std::max(canonicalLength * 0.01,
                        dotP(delta, direction));
                    display[segment + 1] = addP(display[segment],
                        mulP(direction, validLength));
                }
                for (size_t point = 0; point < display.size(); ++point)
                    if (!finiteP(display[point])) display[point] = canonical[point];
            }
        }
        return displayGeometry;
    }

    // River vertices stay fixed. Transit vertices move in the nullspace of
    // segment-direction and junction constraints. Signed half-planes preserve
    // crossings while clearance is measured between the two visible edges.
    static DisplayGeometry applyRiverClearance(const StyledShape& s, const DisplayGeometry& base) {
        struct Vertex { int route,path,point; };
        struct Edge { int a,b,route; Point p,q,d,n; double length; };
        std::vector<Vertex> vertices;std::vector<Edge> routes,rivers;
        std::map<int,std::vector<int>> junctions;
        for(size_t r=0;r<base.size();++r)for(size_t p=0;p<base[r].size();++p){
            const auto& path=base[r][p];const bool river=r<s.routes_is_obstacle.size()&&s.routes_is_obstacle[r];
            int start=vertices.size();
            if(!river)for(size_t k=0;k<path.size();++k){
                int v=vertices.size();vertices.push_back({int(r),int(p),int(k)});
                if(r<s.route_path_topology.size()&&p<s.route_path_topology[r].size()&&k<s.route_path_topology[r][p].geometryNodes.size())
                    junctions[s.route_path_topology[r][p].geometryNodes[k]].push_back(v);
            }
            for(size_t k=1;k<path.size();++k){Point delta=subP(path[k],path[k-1]);double length=lenP(delta);if(length<1e-7)continue;
                Point d=mulP(delta,1/length);Edge edge{start+int(k)-1,start+int(k),int(r),path[k-1],path[k],d,{-d.y,d.x},length};
                (river?rivers:routes).push_back(edge);
            }
        }
        if(rivers.empty()||routes.empty())return base;
        auto clearance=[&](const Edge& e,const Edge& river,double& a,double& b,double& spacing){
            if(std::abs(crossP(e.d,river.d))>0.08715574275)return false;
            a=dotP(subP(e.p,river.p),river.n);b=dotP(subP(e.q,river.p),river.n);
            if(a*b<=0||std::abs(a+b)<2e-5)return false;
            double u=dotP(subP(e.p,river.p),river.d),v=dotP(subP(e.q,river.p),river.d);
            if(std::min(std::max(u,v),river.length)-std::max(std::min(u,v),0.0)<1e-5)return false;
            spacingForRoutes(s,e.route,river.route,spacing);return spacing>0;
        };
        bool needsMovement=false;
        for(const auto& e:routes){for(const auto& river:rivers){double a,b,spacing;
            if(clearance(e,river,a,b,spacing)&&std::min(std::abs(a),std::abs(b))<spacing-1e-6){needsMovement=true;break;}}
            if(needsMovement)break;
        }
        // A new line often has no nearby parallel transit segment. In
        // particular, the unstyled geometry pass never needs clearance work.
        if(!needsMovement)return base;
        const int dimensions=2*vertices.size();using Row=std::vector<double>;
        auto dot=[](const Row& a,const Row& b){double v=0;for(size_t i=0;i<a.size();++i)v+=a[i]*b[i];return v;};
        struct BasisRow { std::vector<std::pair<int,double>> values;int first,last; };
        std::vector<BasisRow> basis;
        auto project=[&](Row& row){
            int first=dimensions,last=-1;
            for(int i=0;i<dimensions;++i)if(row[i]!=0){first=std::min(first,i);last=i;}
            for(int pass=0;pass<2;++pass)for(const auto& q:basis){
                if(q.last<first||q.first>last)continue;
                double value=0;for(auto [i,v]:q.values)value+=row[i]*v;
                if(std::abs(value)<1e-15)continue;
                for(auto [i,v]:q.values)row[i]-=value*v;
                first=std::min(first,q.first);last=std::max(last,q.last);
            }
        };
        auto equality=[&](Row row){project(row);double length=std::sqrt(dot(row,row));
            if(length<=1e-8)return;
            BasisRow q{{},dimensions,-1};
            for(int i=0;i<dimensions;++i)if(std::abs(row[i]/length)>1e-14){q.values.push_back({i,row[i]/length});q.first=std::min(q.first,i);q.last=i;}
            basis.push_back(std::move(q));
        };
        for(const auto& e:routes){Row row(dimensions);row[2*e.a]=-e.n.x;row[2*e.a+1]=-e.n.y;row[2*e.b]=e.n.x;row[2*e.b+1]=e.n.y;equality(std::move(row));
            // Translate short optimizer bend samples together, rather than
            // stretching an already sub-pixel curve into a different shape.
            if(e.length<2){Row tangent(dimensions);tangent[2*e.a]=-e.d.x;tangent[2*e.a+1]=-e.d.y;tangent[2*e.b]=e.d.x;tangent[2*e.b+1]=e.d.y;equality(std::move(tangent));}
        }
        for(const auto& item:junctions)for(size_t k=1;k<item.second.size();++k)for(int axis=0;axis<2;++axis){
            Row row(dimensions);row[2*item.second[0]+axis]=1;row[2*item.second[k]+axis]=-1;equality(std::move(row));}
        struct Constraint { std::vector<std::pair<int,double>> raw,row;double bound,norm=0,extra=0,lambda=0; };
        std::vector<Constraint> constraints;
        // Keep the sparse half-plane until movement actually reaches it. Most
        // route/river pairs never constrain the solution; projecting every pair
        // eagerly makes adding a line scale with the entire network.
        auto inequality=[&](Row row,double bound,double extra=0){
            Constraint c{{},{},bound,0,extra};
            for(int i=0;i<dimensions;++i)if(row[i]!=0)c.raw.push_back({i,row[i]});
            constraints.push_back(std::move(c));};
        auto vertexSide=[&](int v,Point normal,double bound,double extra=0){Row row(dimensions);row[2*v]=normal.x;row[2*v+1]=normal.y;inequality(std::move(row),bound,extra);};
        for(const auto& e:routes){Row row(dimensions);row[2*e.a]=-e.d.x;row[2*e.a+1]=-e.d.y;row[2*e.b]=e.d.x;row[2*e.b+1]=e.d.y;
            inequality(std::move(row),-e.length*.99);
            for(const auto& river:rivers){
                double a=dotP(subP(e.p,river.p),river.n),b=dotP(subP(e.q,river.p),river.n);
                double c=dotP(subP(river.p,e.p),e.n),d=dotP(subP(river.q,e.p),e.n);
                const double eps=1e-5;
                auto keepRiverSide=[&](double side){double sign=side<0?-1:1;vertexSide(e.a,mulP(river.n,sign),-sign*a*.99);vertexSide(e.b,mulP(river.n,sign),-sign*b*.99);};
                auto keepRouteSide=[&](double side){double sign=side<0?-1:1;vertexSide(e.a,mulP(e.n,-sign),-std::min(sign*c,sign*d)*.99);};
                if(a*b>0&&std::min(std::abs(a),std::abs(b))>eps)keepRiverSide(a);
                else if(c*d>0&&std::min(std::abs(c),std::abs(d))>eps)keepRouteSide(c);
                else {
                    for(auto item:{std::pair<int,double>{e.a,a},{e.b,b}}){
                        if(std::abs(item.second)>eps){double sign=item.second<0?-1:1;vertexSide(item.first,mulP(river.n,sign),-std::abs(item.second)*.99);}
                        else {vertexSide(item.first,river.n,-eps);vertexSide(item.first,mulP(river.n,-1),-eps);}
                    }
                    for(double side:{c,d})if(std::abs(side)>eps){double sign=side<0?-1:1;vertexSide(e.a,mulP(e.n,-sign),-std::abs(side)*.99);}
                }
                // A hand-drawn river can deviate slightly from a parallel line.
                // Crossing segments deliberately keep their original crossing.
                double spacing=0;if(!clearance(e,river,a,b,spacing))continue;
                double side=(a+b)*.5;if(std::abs(side)<eps)continue;
                double sign=side<0?-1:1;
                double widths=(s.routes_width[e.route]+s.routes_width[river.route])*.5;
                double gap=std::max(0.0,spacing-widths);
                vertexSide(e.a,mulP(river.n,sign),spacing-gap-sign*a,gap);
                vertexSide(e.b,mulP(river.n,sign),spacing-gap-sign*b,gap);
            }
        }
        auto solve=[&](double fraction,Row& delta){
            delta.assign(dimensions,0);for(auto& c:constraints)c.lambda=0;
            std::vector<size_t> active;std::vector<bool> included(constraints.size());
            auto value=[&](const Constraint& c){double result=0;for(auto [i,v]:(c.row.empty()?c.raw:c.row))result+=v*delta[i];return result;};
            // Most length/crossing constraints are already satisfied. Solve
            // only violated rows, adding constraints when movement reaches them.
            while(true){
                bool added=false;
                for(size_t i=0;i<constraints.size();++i)if(!included[i]){
                    auto& c=constraints[i];
                    // delta is in the direction/junction nullspace, so an
                    // unprojected half-plane has the same value on delta.
                    if(c.bound+fraction*c.extra-value(c)>1e-6){
                        if(c.row.empty()){
                            Row row(dimensions);for(auto [j,v]:c.raw)row[j]=v;
                            project(row);c.norm=dot(row,row);
                            if(c.norm<1e-14)continue;
                            for(int j=0;j<dimensions;++j)if(std::abs(row[j])>1e-12)c.row.push_back({j,row[j]});
                        }
                        included[i]=true;active.push_back(i);added=true;
                    }
                }
                if(!added)return true;
                for(int iteration=0;iteration<4000;++iteration){double change=0;
                    for(size_t index:active){auto& c=constraints[index];
                        double next=std::max(0.0,c.lambda+(c.bound+fraction*c.extra-value(c))/c.norm),step=next-c.lambda;c.lambda=next;
                        if(std::abs(step)<1e-12)continue;
                        for(auto [i,v]:c.row)delta[i]+=step*v;
                        change=std::max(change,std::abs(step)*std::sqrt(c.norm));
                    }
                    if(change<1e-7)break;
                }
                for(size_t index:active){const auto& c=constraints[index];
                    if(c.bound+fraction*c.extra-value(c)>1e-6)return false;}
            }
        };
        Row delta;
        if(!solve(1,delta)){
            // A fixed river and preserved crossing intervals can limit the
            // available gap. Keep the largest feasible gap, never reverse a
            // segment or change a crossing to satisfy an impossible request.
            if(!solve(0,delta))return base;
            double lo=0,hi=1;
            for(int i=0;i<10;++i){double middle=(lo+hi)*.5;Row candidate;
                if(solve(middle,candidate)){lo=middle;delta=std::move(candidate);}else hi=middle;}
        }
        DisplayGeometry result=base;
        for(size_t i=0;i<vertices.size();++i){const auto& v=vertices[i];result[v.route][v.path][v.point]=addP(base[v.route][v.path][v.point],{delta[2*i],delta[2*i+1]});}
        return result;
    }

    struct CornerBend { bool matched = false; int style = -1; double rx = 0, ry = 0; Point in{}, out{}; };
    static void appendUnique(std::vector<Point>& out, Point p) { if (out.empty() || lenP(subP(out.back(), p)) > 1e-7) out.push_back(p); }

    static std::vector<Point> buildStyledDisplayPath(const std::vector<Point>& path, const std::vector<StyledShape::BendStyle>& styles, int samples, int routeIndex, int pathIndex, std::vector<RenderCommand>* commands = nullptr) {
        if (commands && !path.empty()) commands->push_back({RenderCommand::Type::Move, path.front(), {}});
        if (path.size() < 3) {
            if (commands) for (size_t i=1;i<path.size();++i) commands->push_back({RenderCommand::Type::Line,path[i],{}});
            return path;
        }
        const int n = static_cast<int>(path.size()); std::vector<CornerBend> bends(n);
        for (int i = 1; i < n - 1; ++i) {
            Point vin = subP(path[i], path[i - 1]), vout = subP(path[i + 1], path[i]); double lin = lenP(vin), lout = lenP(vout); if (lin < kStyledEps || lout < kStyledEps) continue;
            Point in = mulP(vin, 1.0 / lin), out = mulP(vout, 1.0 / lout); double c = clampD(dotP(in, out), -1, 1); int best = -1; double bestErr = 1e9;
            for (int si = 0; si < static_cast<int>(styles.size()); ++si) if (styles[si].enabled) { double err = std::abs(c - styles[si].angleCos); double angle = 180.0 - std::acos(c)*180.0/3.14159265358979323846; bool matches = styles[si].minInnerAngle >= 0 ? angle >= styles[si].minInnerAngle-1e-7 && angle <= styles[si].maxInnerAngle+1e-7 : err <= styles[si].angleCosTolerance; if (matches && err < bestErr) { best = si; bestErr = err; } }
            if (best >= 0) { bends[i].matched = true; bends[i].style = best; bends[i].in = in; bends[i].out = out; bends[i].rx = clampD(styles[best].radiusX, 0, 0.98 * lin); bends[i].ry = clampD(styles[best].radiusY, 0, 0.98 * lout); (void)crossP(in, out); }
        }
        for (int seg = 0; seg < n - 1; ++seg) {
            double L = lenP(subP(path[seg + 1], path[seg])); double a = bends[seg].matched ? bends[seg].ry : 0, b = bends[seg + 1].matched ? bends[seg + 1].rx : 0;
            if (L > kStyledEps && a + b > 0.98 * L) { double scale = (0.98 * L) / (a + b); if (bends[seg].matched) bends[seg].ry *= scale; if (bends[seg + 1].matched) bends[seg + 1].rx *= scale; std::cerr << "[drawStyledShape] reduced overlapping bend trims route " << routeIndex << " path " << pathIndex << " segment " << seg << "\n"; }
        }
        std::vector<Point> out; appendUnique(out, path.front());
        for (int i = 1; i < n - 1; ++i) {
            if (!bends[i].matched) { appendUnique(out, path[i]); if(commands) commands->push_back({RenderCommand::Type::Line,path[i],{}}); continue; }
            Point p0 = subP(path[i], mulP(bends[i].in, bends[i].rx)); Point p2 = addP(path[i], mulP(bends[i].out, bends[i].ry)); appendUnique(out, p0);
            if(commands) { commands->push_back({RenderCommand::Type::Line,p0,{}}); commands->push_back({RenderCommand::Type::Quadratic,p2,path[i]}); }
            for (int s = 1; s <= samples; ++s) { double t = double(s) / samples, u = 1 - t; Point q = addP(addP(mulP(p0, u * u), mulP(path[i], 2 * u * t)), mulP(p2, t * t)); appendUnique(out, q); }
            if (!finiteP(p0) || !finiteP(p2)) std::cerr << "[drawStyledShape] non-finite bend route " << routeIndex << " path " << pathIndex << " corner " << i << " style " << bends[i].style << "\n";
        }
        appendUnique(out, path.back()); if(commands) commands->push_back({RenderCommand::Type::Line,path.back(),{}}); return out;
    }


}

StyledShapeDisplayData buildStyledShapeDisplayData(
    const StyledShape& styledShape, const Camera& camera, double symbolScale) {
    StyledShapeDisplayData result;
    const auto canonicalScreenGeometry = buildCanonicalScreenGeometry(styledShape, camera);
    const auto spacingAdjustedGeometry =
        applyRiverClearance(styledShape, buildSpacingAdjustedGeometry(styledShape, canonicalScreenGeometry));
    result.screenRoutePaths = spacingAdjustedGeometry;
    result.commands.resize(spacingAdjustedGeometry.size());
    for(size_t ri=0;ri<spacingAdjustedGeometry.size();++ri) {
        result.commands[ri].resize(spacingAdjustedGeometry[ri].size());
        const auto& rules = ri < styledShape.route_bend_overrides.size()
            ? styledShape.route_bend_overrides[ri] : styledShape.routes_bends;
        for(size_t pi=0;pi<spacingAdjustedGeometry[ri].size();++pi)
            result.screenRoutePaths[ri][pi] = buildStyledDisplayPath(spacingAdjustedGeometry[ri][pi],
                rules, kBezierSamples, int(ri), int(pi), &result.commands[ri][pi]);
    }
    result.worldRoutePaths = result.screenRoutePaths;
    for (auto& route : result.worldRoutePaths)
        for (auto& path : route)
            for (Point& point : path)
                point = camera.screenToWorld(point.x, point.y);
    auto stationStyle=styledShape;
    if(std::any_of(styledShape.routes_is_obstacle.begin(),styledShape.routes_is_obstacle.end(),[](bool value){return value;})) {
        // Keep each station on its local segment when spacing changes lengths.
        // A fraction of the entire route would slide it along other segments.
        auto nearest=[](const std::vector<Point>& path,Point p,double preferred,int& segment,double& t){
            double best=1e100,bestVisit=1e100,total=0,before=0;segment=0;t=0;
            for(size_t i=1;i<path.size();++i)total+=lenP(subP(path[i],path[i-1]));
            for(size_t i=1;i<path.size();++i){Point d=subP(path[i],path[i-1]);double length=lenP(d);
                double u=length>0?clampD(dotP(subP(p,path[i-1]),d)/(length*length),0,1):0;
                double distance=lenP(subP(p,addP(path[i-1],mulP(d,u))));
                double visit=std::abs(before+u*length-preferred*total);
                if(distance<best-1e-7||(std::abs(distance-best)<1e-7&&visit<bestVisit)){best=distance;bestVisit=visit;segment=i-1;t=u;}
                before+=length;
            }
        };
        auto remap=[&](int r,int p,float& pathT){
            if(r<0||r>=int(canonicalScreenGeometry.size())||p<0||p>=int(canonicalScreenGeometry[r].size()))return;
            const auto& original=canonicalScreenGeometry[r][p];if(original.size()<2)return;
            double total=0,before=0;for(size_t i=1;i<original.size();++i)total+=lenP(subP(original[i],original[i-1]));
            double distance=clampD(pathT,0,1)*total,t=0;int segment=0;
            for(size_t i=1;i<original.size();++i){double length=lenP(subP(original[i],original[i-1]));
                if(distance<=before+length||i+1==original.size()){segment=i-1;t=length>0?clampD((distance-before)/length,0,1):0;break;}before+=length;}
            const auto& adjusted=spacingAdjustedGeometry[r][p];Point target=addP(adjusted[segment],mulP(subP(adjusted[segment+1],adjusted[segment]),t));
            const double preferred=relativeArcLengthAtSegment(adjusted,segment,t);
            const auto& finalPath=result.screenRoutePaths[r][p];nearest(finalPath,target,preferred,segment,t);
            pathT=relativeArcLengthAtSegment(finalPath,segment,t);
        };
        for(auto& station:stationStyle.normal_stations)remap(station.routeIndex,station.pathIndex,station.pathT);
        for(auto& station:stationStyle.transfer_stations)for(auto& position:station.positions)remap(position.routeIndex,position.pathIndex,position.pathT);
    }
    result.normalStations = updateNormalStations(stationStyle, result.worldRoutePaths);
    result.transferStations = updateTransferStations(
        stationStyle, result.worldRoutePaths,
        kStationMergeEpsilonPixels * symbolScale / std::max(camera.getScale(), kStyledEps));
    return result;
}


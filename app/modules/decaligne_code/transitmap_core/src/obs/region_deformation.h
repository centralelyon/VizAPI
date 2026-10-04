#pragma once
// Cell contour extraction follows desktop app/interaction.cpp. Propagation
// solves affected station positions with ORIGINAL segment-direction constraints.
// Distance weights localize displacement without locking shared cell vertices.
// Apply only updates existing positions; nodes, segments and routes are unchanged.
#include "core/shape.h"
#include "obs/region.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <numeric>
namespace RegionDeformation {
static constexpr double kSnapStep = 3.14159265358979323846 / 4.0;
    struct FaceInfo {
        std::vector<int> nodeLoop;
        std::vector<Point> polygon;
        Point centroid{ 0.0, 0.0 };
        double area = 0.0;
        int activeRegionCount = 0;
        double activeRegionArea = 0.0;
        double scale = 1.0;
    };

    static double polygonAreaSigned(const std::vector<Point>& poly) {
        if (poly.size() < 3) return 0.0;
        double accum = 0.0;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            accum += (poly[j].x * poly[i].y - poly[i].x * poly[j].y);
        }
        return 0.5 * accum;
    }

    static Point polygonCentroid(const std::vector<Point>& poly) {
        Point c{ 0.0, 0.0 };
        if (poly.empty()) return c;
        for (const auto& p : poly) {
            c.x += p.x;
            c.y += p.y;
        }
        c.x /= (double)poly.size();
        c.y /= (double)poly.size();
        return c;
    }

    static std::vector<FaceInfo> buildRouteFaces(const Shape& shape) {
        std::vector<FaceInfo> faces;
        const int n = (int)shape.nodes.size();
        if (n <= 2) return faces;

        std::vector<std::vector<int>> adj(n);
        std::unordered_set<long long> seenEdge;
        for (const auto& route : shape.routes) {
            if (route.isObstacle) continue;
            for (int segIndex : route.segmentIndices) {
                if (segIndex < 0 || segIndex >= (int)shape.segments.size()) continue;
                const auto& seg = shape.segments[segIndex];
                if (seg.a < 0 || seg.b < 0 || seg.a >= n || seg.b >= n || seg.a == seg.b) continue;
                const int lo = std::min(seg.a, seg.b);
                const int hi = std::max(seg.a, seg.b);
                const long long key = ((long long)lo << 32) | (unsigned int)hi;
                if (seenEdge.count(key)) continue;
                seenEdge.insert(key);
                adj[seg.a].push_back(seg.b);
                adj[seg.b].push_back(seg.a);
            }
        }

        std::vector<std::unordered_map<int, int>> nextCw(n);
        for (int i = 0; i < n; ++i) {
            auto& nb = adj[i];
            if (nb.empty()) continue;
            std::sort(nb.begin(), nb.end(), [&](int lhs, int rhs) {
                const Point& p = shape.nodes[i].pos;
                const Point& a = shape.nodes[lhs].pos;
                const Point& b = shape.nodes[rhs].pos;
                return std::atan2(a.y - p.y, a.x - p.x) < std::atan2(b.y - p.y, b.x - p.x);
                });
            for (int k = 0; k < (int)nb.size(); ++k) {
                const int prev = nb[(k - 1 + (int)nb.size()) % (int)nb.size()];
                const int curr = nb[k];
                nextCw[i][curr] = prev;
            }
        }

        std::unordered_set<long long> usedHalfEdges;
        auto halfEdgeKey = [](int u, int v) -> long long {
            return ((long long)u << 32) | (unsigned int)v;
            };

        for (int u = 0; u < n; ++u) {
            for (int v : adj[u]) {
                const long long startKey = halfEdgeKey(u, v);
                if (usedHalfEdges.count(startKey)) continue;
                if (!nextCw[v].count(u)) continue;

                std::vector<int> faceNodes;
                int a = u;
                int b = v;
                bool closed = false;
                for (int guard = 0; guard < n * 8; ++guard) {
                    const long long hk = halfEdgeKey(a, b);
                    if (usedHalfEdges.count(hk)) break;
                    usedHalfEdges.insert(hk);
                    faceNodes.push_back(a);

                    auto it = nextCw[b].find(a);
                    if (it == nextCw[b].end()) break;
                    const int c = it->second;
                    a = b;
                    b = c;
                    if (a == u && b == v) {
                        closed = true;
                        break;
                    }
                }
                if (!closed || faceNodes.size() < 3) continue;

                // A dead-end is traversed out and back in the same face. It
                // must not abort the half-edge walk or become a contour anchor.
                bool trimmed=true;
                while(trimmed && faceNodes.size()>2) {
                    trimmed=false;
                    for(size_t k=0;k<faceNodes.size();++k) {
                        size_t next=(k+1)%faceNodes.size(),prev=(k+faceNodes.size()-1)%faceNodes.size();
                        if(faceNodes[prev]!=faceNodes[next])continue;
                        if(next==0){faceNodes.erase(faceNodes.begin()+k);faceNodes.erase(faceNodes.begin());}
                        else {faceNodes.erase(faceNodes.begin()+next);faceNodes.erase(faceNodes.begin()+k);}
                        trimmed=true;break;
                    }
                }
                FaceInfo face;
                face.nodeLoop = std::move(faceNodes);
                face.polygon.reserve(face.nodeLoop.size());
                for (int nodeIndex : face.nodeLoop) {
                    if (nodeIndex < 0 || nodeIndex >= n) continue;
                    face.polygon.push_back(shape.nodes[nodeIndex].pos);
                }
                if (face.polygon.size() < 3) continue;
                const double signedArea = polygonAreaSigned(face.polygon);
                face.area = std::abs(signedArea);
                if (face.area <= 1e-6) continue;
                if (signedArea < 0.0) continue; // skip exterior face

                face.centroid = polygonCentroid(face.polygon);
                faces.push_back(std::move(face));
            }
        }
        return faces;
    }

inline Point sub(Point a,Point b){return {a.x-b.x,a.y-b.y};}
inline double dot(Point a,Point b){return a.x*b.x+a.y*b.y;}
inline Point lerp(Point a,Point b,double t){return {a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t};}
inline double cross(Point a,Point b,Point c){return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);}

// The desktop view boundary closes perimeter cells. Use virtual frame edges
// only for face discovery: they never enter Shape, rendering, or export.
inline std::vector<FaceInfo> cells(const Shape& shape) {
    Shape framed=shape;
    double x0=1e30,y0=1e30,x1=-1e30,y1=-1e30;
    for(auto n:shape.nodes){x0=std::min(x0,n.pos.x);x1=std::max(x1,n.pos.x);y0=std::min(y0,n.pos.y);y1=std::max(y1,n.pos.y);}
    if(x1-x0<1e-6){x0-=100;x1+=100;}if(y1-y0<1e-6){y0-=100;y1+=100;}
    for(Point p:std::vector<Point>{{x0,y0},{x1,y0},{x1,y1},{x0,y1}}) {
        bool exists=false;for(auto n:framed.nodes)if(std::hypot(n.pos.x-p.x,n.pos.y-p.y)<1e-6){exists=true;break;}
        if(!exists){ShapeNode n;n.pos=p;framed.nodes.push_back(n);}
    }
    ShapeRoute frame;frame.id="region-cell-frame";
    for(int side=0;side<4;++side) {
        std::vector<int> ns;
        for(int i=0;i<int(framed.nodes.size());++i){auto p=framed.nodes[i].pos;if(std::abs((side%2?p.x:p.y)-(side==0?y0:side==1?x1:side==2?y1:x0))<1e-6)ns.push_back(i);}
        std::sort(ns.begin(),ns.end(),[&](int a,int b){return side%2?framed.nodes[a].pos.y<framed.nodes[b].pos.y:framed.nodes[a].pos.x<framed.nodes[b].pos.x;});
        for(size_t i=1;i<ns.size();++i){frame.segmentIndices.push_back(framed.segments.size());framed.segments.push_back({ns[i-1],ns[i]});}
    }
    framed.routes.push_back(frame);return buildRouteFaces(framed);
}

inline Point center(const std::vector<Point>& poly) {
    double area=0;Point c{};
    for(size_t i=0;i<poly.size();++i){auto a=poly[i],b=poly[(i+1)%poly.size()];double k=a.x*b.y-b.x*a.y;area+=k;c.x+=(a.x+b.x)*k;c.y+=(a.y+b.y)*k;}
    if(std::abs(area)>1e-8){c.x/=3*area;c.y/=3*area;if(Geometry::pointInPolygon(c,poly))return c;}
    c=polygonCentroid(poly);if(Geometry::pointInPolygon(c,poly))return c;
    for(size_t i=0;i<poly.size();++i){auto p=lerp(poly[i],poly[(i+1)%poly.size()],0.5);p=lerp(p,c,0.01);if(Geometry::pointInPolygon(p,poly))return p;}
    return c;
}
inline double boundaryDistance(Point p,const std::vector<Point>& poly) {
    double best=1e30;
    for(size_t i=0;i<poly.size();++i){auto a=poly[i],d=sub(poly[(i+1)%poly.size()],a);double t=dot(d,d)>0?std::clamp(dot(sub(p,a),d)/dot(d,d),0.0,1.0):0;auto q=Point{a.x+t*d.x,a.y+t*d.y};best=std::min(best,std::hypot(p.x-q.x,p.y-q.y));}
    return best;
}
inline bool embeddingSafe(const Shape& before,const Shape& after) {
    auto intersects=[](Point a,Point b,Point c,Point d){
        if(std::max(a.x,b.x)<std::min(c.x,d.x)-1e-6||std::max(c.x,d.x)<std::min(a.x,b.x)-1e-6||std::max(a.y,b.y)<std::min(c.y,d.y)-1e-6||std::max(c.y,d.y)<std::min(a.y,b.y)-1e-6)return false;
        if(cross(a,b,c)*cross(a,b,d)<-1e-8&&cross(c,d,a)*cross(c,d,b)<-1e-8)return true;
        auto on=[](Point p,Point a,Point b){return boundaryDistance(p,{a,b})<1e-6;};
        return on(a,c,d)||on(b,c,d)||on(c,a,b)||on(d,a,b);
    };
    for(const auto& e:before.segments){auto old=sub(before.nodes[e.b].pos,before.nodes[e.a].pos),now=sub(after.nodes[e.b].pos,after.nodes[e.a].pos);if(dot(old,now)<0.02*dot(old,old))return false;}
    for(size_t i=0;i<after.segments.size();++i)for(size_t j=i+1;j<after.segments.size();++j){auto a=after.segments[i],b=after.segments[j];if(a.a==b.a||a.a==b.b||a.b==b.a||a.b==b.b)continue;
        if(intersects(after.nodes[a.a].pos,after.nodes[a.b].pos,after.nodes[b.a].pos,after.nodes[b.b].pos)&&!intersects(before.nodes[a.a].pos,before.nodes[a.b].pos,before.nodes[b.a].pos,before.nodes[b.b].pos))return false;
    }return true;
}


inline bool clearOf(const Shape& shape,const std::vector<Point>& polygon) {
    for(auto n:shape.nodes)if(Geometry::pointInPolygon(n.pos,polygon)&&boundaryDistance(n.pos,polygon)>1e-6)return false;
    for(auto e:shape.segments)if(segmentIntersectsAnyRegionPolygonInterior(shape.nodes[e.a].pos,shape.nodes[e.b].pos,{polygon}))return false;
    return true;
}
// A scalar row of a small, matrix-free weighted projection problem. Direction
// rows are equalities; clearance and positive-length rows are inequalities.
struct PositionConstraint {
    int a=-1,b=-1;Point ca{},cb{};double rhs=0;bool inequality=false;double multiplier=0;
};
inline double rowValue(const PositionConstraint& row,const std::vector<Point>& displacement) {
    return dot(row.ca,displacement[row.a])+(row.b>=0?dot(row.cb,displacement[row.b]):0);
}
// Dual coordinate projection (Hildreth/Dykstra). Each pass is linear in the
// number of constraints, needs no matrix/solver dependency, and projects the
// whole connected displacement field instead of locking arbitrary cell nodes.
inline bool solvePositions(std::vector<Point>& displacement,std::vector<PositionConstraint>& rows,const std::vector<double>& inverseWeight) {
    for(auto& row:rows)row.multiplier=0;
    displacement.assign(inverseWeight.size(),{});
    for(int pass=0;pass<60000;++pass) {
        for(auto& row:rows){double den=dot(row.ca,row.ca)*inverseWeight[row.a]+(row.b>=0?dot(row.cb,row.cb)*inverseWeight[row.b]:0);if(den<1e-20)continue;
            double next=row.multiplier+(row.rhs-rowValue(row,displacement))/den;if(row.inequality)next=std::max(0.0,next);
            double delta=next-row.multiplier;row.multiplier=next;
            displacement[row.a].x+=delta*inverseWeight[row.a]*row.ca.x;displacement[row.a].y+=delta*inverseWeight[row.a]*row.ca.y;
            if(row.b>=0){displacement[row.b].x+=delta*inverseWeight[row.b]*row.cb.x;displacement[row.b].y+=delta*inverseWeight[row.b]*row.cb.y;}
        }
        if(pass%10==0){double error=0;for(const auto& row:rows){double residual=row.rhs-rowValue(row,displacement);error=std::max(error,row.inequality?std::max(0.0,residual):std::abs(residual));}if(error<2e-6)return true;}
    }
    return false;
}
inline std::vector<Point> supportNormals(const std::vector<Point>& polygon) {
    std::vector<Point> normals;auto c=center(polygon);
    for(size_t i=0;i<polygon.size();++i){auto a=polygon[i],b=polygon[(i+1)%polygon.size()],d=sub(b,a);double length=std::hypot(d.x,d.y);if(length<1e-6)continue;Point n{d.y/length,-d.x/length};if(dot(n,sub(lerp(a,b,.5),c))<0){n.x=-n.x;n.y=-n.y;}bool duplicate=false;for(auto q:normals)if(dot(n,q)>.999999)duplicate=true;if(!duplicate)normals.push_back(n);}
    return normals;
}
inline double support(const std::vector<Point>& polygon,Point normal) {
    double value=-1e30;for(auto p:polygon)value=std::max(value,dot(p,normal));return value;
}
inline bool separateCrossings(const Shape& before,const Shape& candidate,std::vector<PositionConstraint>& rows,std::unordered_set<long long>& separated) {
    bool added=false;
    for(size_t i=0;i<candidate.segments.size();++i)for(size_t j=i+1;j<candidate.segments.size();++j){auto e=candidate.segments[i],f=candidate.segments[j];if(e.a==f.a||e.a==f.b||e.b==f.a||e.b==f.b)continue;
        auto a=candidate.nodes[e.a].pos,b=candidate.nodes[e.b].pos,c=candidate.nodes[f.a].pos,d=candidate.nodes[f.b].pos;
        if(std::max(a.x,b.x)<std::min(c.x,d.x)-1e-6||std::max(c.x,d.x)<std::min(a.x,b.x)-1e-6||std::max(a.y,b.y)<std::min(c.y,d.y)-1e-6||std::max(c.y,d.y)<std::min(a.y,b.y)-1e-6)continue;
        bool collision=(cross(a,b,c)*cross(a,b,d)<-1e-8&&cross(c,d,a)*cross(c,d,b)<-1e-8)||boundaryDistance(a,{c,d})<1e-6||boundaryDistance(b,{c,d})<1e-6||boundaryDistance(c,{a,b})<1e-6||boundaryDistance(d,{a,b})<1e-6;
        if(!collision)continue;
        a=before.nodes[e.a].pos;b=before.nodes[e.b].pos;c=before.nodes[f.a].pos;d=before.nodes[f.b].pos;
        Point axis{};double best=-1e30;
        for(auto edge:std::vector<Point>{sub(b,a),sub(d,c)}){double length=std::hypot(edge.x,edge.y);if(length<1e-8)continue;for(double sign:std::vector<double>{-1,1}){Point n{-edge.y/length*sign,edge.x/length*sign};double gap=std::min(dot(c,n),dot(d,n))-std::max(dot(a,n),dot(b,n));if(gap>best){best=gap;axis=n;}}}
        // No separating axis in the original drawing means this crossing/contact
        // was already present. Preserve it rather than demanding a new topology.
        if(best<1e-6)continue;
        long long key=(static_cast<long long>(i)<<32)|j;if(!separated.insert(key).second)continue;
        for(int u:{e.a,e.b})for(int v:{f.a,f.b})rows.push_back({u,v,{-axis.x,-axis.y},axis,1e-5-dot(axis,sub(before.nodes[v].pos,before.nodes[u].pos)),true});
        added=true;
    }
    return added;
}
inline void apply(Shape& shape,const Shape& baseline,const Region& region) {
    shape=baseline;if(shape.nodes.empty())return;
    std::vector<std::vector<Point>> polygons;buildRegionPolygons(region,polygons);
    std::vector<std::vector<Point>> normals;std::vector<double> radius;
    for(const auto& polygon:polygons){normals.push_back(supportNormals(polygon));auto c=center(polygon);double r=0;for(auto p:polygon)r=std::max(r,std::hypot(p.x-c.x,p.y-c.y));radius.push_back(r);}
    std::vector<double> inverseWeight(baseline.nodes.size());
    for(size_t n=0;n<baseline.nodes.size();++n){double distance=1e30,range=40;for(size_t r=0;r<polygons.size();++r){double d=Geometry::pointInPolygon(baseline.nodes[n].pos,polygons[r])?0:boundaryDistance(baseline.nodes[n].pos,polygons[r]);if(d<distance){distance=d;range=std::max(40.0,radius[r]*.75);}}
        inverseWeight[n]=1.0/(1.0+std::min(160.0,8.0*distance*distance/(range*range)));
    }
    std::vector<int> degree(baseline.nodes.size());for(auto e:baseline.segments){++degree[e.a];++degree[e.b];}
    std::vector<PositionConstraint> directionRows;
    for(auto e:baseline.segments){auto d=sub(baseline.nodes[e.b].pos,baseline.nodes[e.a].pos);double length=std::hypot(d.x,d.y);if(length<1e-8)continue;Point t{d.x/length,d.y/length},n{-t.y,t.x};
        directionRows.push_back({e.a,e.b,{-n.x,-n.y},n,0,false});
        directionRows.push_back({e.a,e.b,{-t.x,-t.y},t,std::max(1e-5,length*.03)-length,true});
    }
    bool accepted=false;
    // Clearance sides are discrete choices. Try the closest side first, then
    // a few consistent outward preferences if incident edges chose conflicting
    // sides. Every attempt uses the same original-direction constraints.
    for(int preference=0;preference<9&&!accepted;++preference){auto rows=directionRows;std::unordered_set<long long> avoided,separated,isolated;Shape candidate=baseline;
        for(int step=0;step<32;++step){bool added=false;
            for(size_t r=0;r<polygons.size();++r)for(size_t node=0;node<baseline.nodes.size();++node)if(degree[node]==0&&Geometry::pointInPolygon(candidate.nodes[node].pos,polygons[r])&&boundaryDistance(candidate.nodes[node].pos,polygons[r])>1e-6){
                long long key=(static_cast<long long>(r)<<32)|node;if(!isolated.insert(key).second)continue;Point normal{};double best=1e30;
                for(auto n:normals[r]){double cost=support(polygons[r],n)+2-dot(candidate.nodes[node].pos,n);if(cost<best){best=cost;normal=n;}}
                rows.push_back({int(node),-1,normal,{},support(polygons[r],normal)+2-dot(baseline.nodes[node].pos,normal),true});added=true;
            }

            for(size_t r=0;r<polygons.size();++r)for(size_t ei=0;ei<baseline.segments.size();++ei){auto e=baseline.segments[ei];auto a=candidate.nodes[e.a].pos,b=candidate.nodes[e.b].pos;
                if(!segmentIntersectsAnyRegionPolygonInterior(a,b,{polygons[r]}))continue;
                long long key=(static_cast<long long>(r)<<32)|ei;if(!avoided.insert(key).second)continue;
                Point normal{};double best=1e30;Point bias{};if(preference){double angle=(preference-1)*kSnapStep;bias={std::cos(angle),std::sin(angle)};}
                for(auto n:normals[r]){double bound=support(polygons[r],n)+std::clamp(radius[r]*.05,2.0,8.0);double da=std::max(0.0,bound-dot(a,n)),db=std::max(0.0,bound-dot(b,n));double cost=da*da/inverseWeight[e.a]+db*db/inverseWeight[e.b];
                    if(preference)cost+=(1-dot(n,bias))*std::max(100.0,radius[r]*radius[r])*1e9;
                    if(cost<best){best=cost;normal=n;}}
                double bound=support(polygons[r],normal)+std::clamp(radius[r]*.05,2.0,8.0);
                for(int node:{e.a,e.b})rows.push_back({node,-1,normal,{},bound-dot(baseline.nodes[node].pos,normal),true});added=true;
            }
            added=separateCrossings(baseline,candidate,rows,separated)||added;
            if(!added){bool clear=true;for(const auto& polygon:polygons)if(!clearOf(candidate,polygon))clear=false;
                if(clear&&embeddingSafe(baseline,candidate)){shape=std::move(candidate);accepted=true;}break;
            }
            std::vector<Point> displacement;if(!solvePositions(displacement,rows,inverseWeight))break;
            for(size_t n=0;n<candidate.nodes.size();++n)candidate.nodes[n].pos={baseline.nodes[n].pos.x+displacement[n].x,baseline.nodes[n].pos.y+displacement[n].y};
        }
    }
    if(!accepted)throw std::invalid_argument("Region constraints conflict with the existing network; overlapping regions or existing crossings may need adjustment");
    // Do not fit/subdivide contours after solving: that would change segment
    // directions and introduce new bends. All existing shape points participate
    // in the same position solve as stations. Distant points have higher movement
    // costs, but remain movable when exact incident directions require it.
}
} // namespace RegionDeformation

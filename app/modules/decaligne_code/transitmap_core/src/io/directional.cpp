#include "io/directional.h"
#include "core/projection.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
using json = nlohmann::json;
namespace {
void check(bool ok,const std::string& why) {if(!ok)throw std::invalid_argument(why);}
double numeric(const std::string& s) {
    size_t end=0;double v=std::stod(s,&end);
    check(end==s.size()&&std::isfinite(v),"Invalid GTFS number");return v;
}
void order(json& rows,const char* key) {
    std::stable_sort(rows.begin(),rows.end(),[&](const auto& a,const auto& b){return numeric(a.at(key).template get<std::string>())<numeric(b.at(key).template get<std::string>());});
}
json coordinate(const json& row,const char* lon,const char* lat) {
    double x=numeric(row.at(lon)),y=numeric(row.at(lat));
    check(std::abs(x)<=180&&std::abs(y)<=90,"Invalid GTFS lon/lat");return json::array({x,y});
}
Point position(const json& row,bool planar) {
    const auto& c=row.at("position");
    check(c.is_array()&&c.size()==2&&c[0].is_number()&&c[1].is_number(),"Invalid directional position");
    Point p{c[0],c[1]};check(std::isfinite(p.x)&&std::isfinite(p.y)&&std::abs(p.x)<1e10&&std::abs(p.y)<1e10,"Invalid directional coordinates");
    if(!planar){check(std::abs(p.x)<=180&&std::abs(p.y)<=90,"Invalid directional lon/lat");p=lonlatToWebMerc(p.x,std::clamp(p.y,-85.05112878,85.05112878));}return p;
}
double distance(Point a,Point b) {return std::hypot(a.x-b.x,a.y-b.y);}
}
json gtfsDirectionalPatterns(const json& tables) {
    std::map<std::string,json> stops,times,shapes,groups;
    for(const auto& s:tables.at("stops"))stops[s.at("stop_id")]=s;
    for(const auto& t:tables.at("stop_times")) {
        auto id=t.at("trip_id").get<std::string>();if(!times.count(id))times[id]=json::array();times[id].push_back(t);
    }
    for(const auto& p:tables.at("shapes")) {
        auto id=p.at("shape_id").get<std::string>();if(!shapes.count(id))shapes[id]=json::array();shapes[id].push_back(p);
    }
    for(auto& entry:times)order(entry.second,"stop_sequence");
    for(auto& entry:shapes)order(entry.second,"shape_pt_sequence");
    for(const auto& trip:tables.at("trips")) {
        std::string route=trip.at("route_id"),id=trip.at("trip_id"),shape=trip.value("shape_id","");
        if(!times.count(id)||times[id].size()<2)continue;
        json orderedStops=json::array(),shapePoints=json::array(),identity=json::array();
        for(const auto& t:times[id]) {
            const auto sid=t.at("stop_id").get<std::string>();check(stops.count(sid),"GTFS stop_times references missing stop: "+sid);
            const auto& s=stops.at(sid);
            json stop={{"stopId",sid},{"name",s.value("stop_name","")},{"stopSequence",numeric(t.at("stop_sequence"))},{"position",coordinate(s,"stop_lon","stop_lat")}};
            if(!t.value("shape_dist_traveled","").empty())stop["shapeDistTraveled"]=numeric(t["shape_dist_traveled"]);
            // Sequence numbers may have gaps; only the ordered visits/distances define a variant.
            identity.push_back({{"stopId",sid},{"distance",stop.value("shapeDistTraveled",json())}});orderedStops.push_back(stop);
        }
        const auto dir=trip.value("direction_id","");
        const auto key=json::array({route,dir,shape,identity}).dump();
        if(!groups.count(key)) {
            if(shapes.count(shape))for(const auto& p:shapes[shape]) {
                json v={{"shapePtSequence",numeric(p.at("shape_pt_sequence"))},{"position",coordinate(p,"shape_pt_lon","shape_pt_lat")}};
                if(!p.value("shape_dist_traveled","").empty())v["shapeDistTraveled"]=numeric(p["shape_dist_traveled"]);
                shapePoints.push_back(v);
            }
            groups[key]={{"routeId",route},{"directionId",dir},{"shapeId",shape},{"tripId",id},{"tripCount",0},{"orderedStops",orderedStops},{"orderedShapePoints",shapePoints}};
        }
        auto& p=groups[key];p["tripCount"]=p["tripCount"].get<int>()+1;
        if(id<p["tripId"].get<std::string>()){p["tripId"]=id;p["orderedStops"]=orderedStops;}
    }
    std::map<std::string,json> routes;
    for(auto& entry:groups){auto& p=entry.second;auto id=p["routeId"].get<std::string>();if(!routes.count(id))routes[id]=json::array();routes[id].push_back(p);}
    json result={{"version",1},{"coordinateSystem","lonlat"},{"routes",json::array()}};
    for(auto& entry:routes) {
        auto& patterns=entry.second;
        std::stable_sort(patterns.begin(),patterns.end(),[](const auto& a,const auto& b){
            if(a["directionId"]!=b["directionId"])return a["directionId"]<b["directionId"];
            if(a["tripCount"]!=b["tripCount"])return a["tripCount"]>b["tripCount"];
            if(a["orderedStops"].size()!=b["orderedStops"].size())return a["orderedStops"].size()>b["orderedStops"].size();
            if(a["shapeId"]!=b["shapeId"])return a["shapeId"]<b["shapeId"];
            return a["tripId"]<b["tripId"];
        });
        std::set<std::string> seen;int index=0;
        for(auto& p:patterns){p["patternId"]=std::to_string(index++);p["representative"]=seen.insert(p["directionId"]).second;}
        result["routes"].push_back({{"routeId",entry.first},{"patterns",patterns}});
    }
    validateDirectionalData(result);return result;
}
void validateDirectionalData(const json& data) {
    check(data.is_object()&&data.value("version",0)==1,"Unsupported directional data version");
    const auto crs=data.value("coordinateSystem","");check(crs=="lonlat"||crs=="planar","Unknown directional coordinate system");
    check(data.contains("routes")&&data["routes"].is_array()&&data["routes"].size()<=500,"Invalid directional routes");
    size_t count=0;std::set<std::string> routeIds;
    for(const auto& r:data["routes"]) {
        check(routeIds.insert(r.at("routeId").get<std::string>()).second,"Duplicate directional route");
        check(r.at("patterns").is_array(),"Invalid directional patterns");std::set<std::string> ids,representatives;
        for(const auto& p:r["patterns"]) {
            check(ids.insert(p.at("patternId").get<std::string>()).second,"Duplicate directional pattern");
            if(p.at("representative").get<bool>())check(representatives.insert(p.at("directionId").get<std::string>()).second,"Duplicate direction representative");
            check(p.at("shapeId").is_string()&&p.at("tripId").is_string(),"Invalid GTFS identity");
            for(const auto* key:{"orderedStops","orderedShapePoints"}) {
                const auto& rows=p.at(key);check(rows.is_array(),"Invalid directional sequence");double previous=-1;
                for(const auto& row:rows){position(row,crs=="planar");const auto* sequence=std::string(key)=="orderedStops"?"stopSequence":"shapePtSequence";
                    const double v=row.at(sequence).get<double>();check(std::isfinite(v)&&v>previous,"Directional sequences must increase");previous=v;
                    if(std::string(key)=="orderedStops")check(row.at("stopId").is_string()&&row.at("name").is_string(),"Invalid directional stop");
                    if(row.contains("shapeDistTraveled"))check(row["shapeDistTraveled"].is_number()&&std::isfinite(row["shapeDistTraveled"].get<double>())&&row["shapeDistTraveled"].get<double>()>=0,"Invalid shape distance");
                    check(++count<=500000,"Too many directional points");
                }
            }
            check(p["orderedStops"].size()>=2,"Directional pattern needs two stops");
        }
    }
}
std::vector<OrderedTraversal> orderedTraversals(const json& data) {
    validateDirectionalData(data);
    std::vector<OrderedTraversal> result;
    const bool planar=data.value("coordinateSystem","")=="planar";
    for(const auto& r:data.at("routes"))for(const auto& p:r.at("patterns")) {
        OrderedTraversal traversal{r.at("routeId").get<std::string>(),&p,{}};
            const auto& stops=p.at("orderedStops");const auto& points=p.at("orderedShapePoints");
            std::vector<Point> line;for(const auto& row:points)line.push_back(position(row,planar));
            if(line.size()<2 || std::all_of(line.begin(),line.end(),[&](Point q){return distance(q,line.front())<1e-8;}))line.clear(); // Missing/degenerate shapes use the stop order.
            struct Visit {double t;Point pos;const json* stop;int stopIndex=-1;};std::vector<Visit> visits;
            for(size_t i=0;i<line.size();++i)visits.push_back({double(i),line[i],nullptr});
            double last=0;
            bool distances=line.size()>1;double prev=-1;
            for(const auto& row:points){if(!row.contains("shapeDistTraveled")){distances=false;break;}double d=row["shapeDistTraveled"];if(d<prev)distances=false;prev=d;}
            for(size_t si=0;si<stops.size();++si) {
                const auto& stop=stops[si];const Point pos=position(stop,planar);double at=last;
                if(line.size()<2)at=double(si);
                else if(distances&&stop.contains("shapeDistTraveled")) {
                    const double d=stop["shapeDistTraveled"];
                    check(d>=points.front()["shapeDistTraveled"].get<double>()&&d<=points.back()["shapeDistTraveled"].get<double>(),"Stop distance outside shape");
                    for(size_t i=1;i<points.size();++i) {
                        double a=points[i-1]["shapeDistTraveled"],b=points[i]["shapeDistTraveled"];
                        if(b>=d&&b>a){at=i-1+(d-a)/(b-a);break;}
                    }
                    check(at+1e-9>=last,"Stop distances must follow the traversal");
                } else {
                    double best=std::numeric_limits<double>::infinity();
                    for(size_t i=std::min(size_t(last),line.size()-2);i+1<line.size();++i) {
                        const auto a=line[i],b=line[i+1];double dx=b.x-a.x,dy=b.y-a.y,len=dx*dx+dy*dy;
                        if(len<1e-18)continue;
                        double t=std::clamp(((pos.x-a.x)*dx+(pos.y-a.y)*dy)/len,std::max(0.0,last-double(i)),1.0);
                        double d=distance(pos,{a.x+t*dx,a.y+t*dy});
                        if(d<best-1e-8){best=d;at=i+t;}
                    }
                }
                last=at;visits.push_back({at,pos,&stop,int(si)});
            }
            std::stable_sort(visits.begin(),visits.end(),[](const auto& a,const auto& b){return a.t<b.t;});
        for(const auto& visit:visits)traversal.samples.push_back({visit.pos,visit.stopIndex});
        result.push_back(std::move(traversal));
    }
    return result;
}
Shape directionalRenderShape(const Shape& source,const json& data,const json& selected) {
    Shape out=source;out.routes.clear();std::set<std::string> enabled;
    for(const auto& id:selected)enabled.insert(id.get<std::string>());
    std::map<std::string,std::vector<OrderedTraversal>> patterns;
    for(auto& traversal:orderedTraversals(data))patterns[traversal.routeId].push_back(std::move(traversal));
    int next=0;for(const auto& n:source.nodes)next=std::max(next,n.id+1);
    for(const auto& parent:source.routes) {
        if(!enabled.count(parent.id)||!patterns.count(parent.id)){out.routes.push_back(parent);continue;}
        for(const auto& traversal:patterns[parent.id]) {
            const auto& p=*traversal.pattern;
            if(!p.at("representative").get<bool>())continue;
            ShapeRoute route=parent;route.segmentIndices.clear();route.orderedNodes.clear();route.logicalRouteId=parent.id;
            route.directionId=p.at("directionId");route.patternId=p.at("patternId");
            route.id="gtfs:"+json::array({parent.id,route.patternId}).dump();
            const auto& stops=p.at("orderedStops");
            for(const auto& sample:traversal.samples) {
                struct {Point pos;const json* stop;} visit{sample.position,sample.stopIndex<0?nullptr:&stops.at(sample.stopIndex)};
                // Coalesce a coincident shape sample with a stop, never two stop occurrences.
                if(!route.orderedNodes.empty()) {
                    auto& n=out.nodes[route.orderedNodes.back()];
                    if(distance(n.pos,visit.pos)<1e-8&&(!visit.stop||!isStationLike(n.type))) {
                        if(visit.stop){n.type=ShapeNodeType::Station;n.name=visit.stop->at("name");n.station_id=visit.stop->at("stopId");}
                        continue;
                    }
                }
                ShapeNode node;node.id=next++;node.uid="gtfs-node:"+json::array({parent.id,route.patternId,route.orderedNodes.size()}).dump();node.pos=visit.pos;
                if(visit.stop){node.type=ShapeNodeType::Station;node.name=visit.stop->at("name");node.station_id=visit.stop->at("stopId");}
                const int ni=out.nodes.size();out.nodes.push_back(node);
                if(!route.orderedNodes.empty()){route.segmentIndices.push_back(out.segments.size());out.segments.push_back({route.orderedNodes.back(),ni,"gtfs-edge:"+node.uid});}
                route.orderedNodes.push_back(ni);
            }
            if(route.orderedNodes.size()>=2)out.routes.push_back(route);
        }
    }
    return out;
}
// Share collinear overlapping display segments even when GTFS sampling and stop
// IDs differ. This only adds bundles involving a directional render instance;
// the existing spacing solver and its propagation remain authoritative.
void directionalSharedGeometry(const Shape& shape,StyledShape& styled) {
    using Ref=StyledShape::StyledRouteSegmentRef;
    struct Span {double lo,hi;Ref ref;};
    using Key=std::tuple<long long,long long,long long>;
    std::map<Key,std::vector<Span>> lines;
    for(size_t r=0;r<styled.routes_geometry_vertices.size();++r)for(size_t p=0;p<styled.routes_geometry_vertices[r].size();++p) {
        const auto& path=styled.routes_geometry_vertices[r][p];
        for(size_t i=1;i<path.size();++i) {
            Point a=path[i-1],b=path[i];double dx=b.x-a.x,dy=b.y-a.y,len=std::hypot(dx,dy);if(len<1e-8)continue;
            dx/=len;dy/=len;if(dx<0||(std::abs(dx)<1e-12&&dy<0)){dx=-dx;dy=-dy;}
            double lo=a.x*dx+a.y*dy,hi=b.x*dx+b.y*dy;if(lo>hi)std::swap(lo,hi);
            lines[{std::llround(dx*1e8),std::llround(dy*1e8),std::llround((-a.x*dy+a.y*dx)*1e4)}].push_back({lo,hi,{int(r),int(p),int(i-1),int(i)}});
        }
    }
    for(auto& entry:lines) {
        auto& spans=entry.second;std::sort(spans.begin(),spans.end(),[](const auto& a,const auto& b){return a.lo<b.lo;});
        // One bundle per constant-membership interval (not separate pairwise
        // bundles, which would incorrectly put three routes in two lanes).
        std::map<double,std::vector<std::pair<int,bool>>> events;
        for(size_t i=0;i<spans.size();++i){events[spans[i].lo].push_back({i,true});events[spans[i].hi].push_back({i,false});}
        std::set<int> active;
        for(auto it=events.begin();it!=events.end();++it) {
            for(auto e:it->second)if(e.second)active.insert(e.first);else active.erase(e.first);
            auto next=std::next(it);if(next==events.end()||next->first-it->first<1e-6)continue;
            std::vector<Ref> refs;std::set<int> routes;bool directional=false;
            for(int i:active){const auto ref=spans[i].ref;refs.push_back(ref);routes.insert(ref.routeIndex);directional|=!shape.routes[ref.routeIndex].logicalRouteId.empty();}
            if(directional&&routes.size()>1)styled.shared_indices.push_back(refs);
        }
    }
}

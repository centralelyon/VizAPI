#include "io/route_directions.h"
#include "io/directional.h"
#include "core/projection.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
using json = nlohmann::json;
namespace {
std::string id(const json& j) {
    if(j.is_string())return j.get<std::string>();
    if(j.is_number_integer())return j.dump();
    throw std::invalid_argument("Direction endpoint must be a string/integer ID");
}
Point coordinate(const json& c, bool planar) {
    Point p{c.at(0),c.at(1)};
    return !planar&&std::abs(p.x)<=180&&std::abs(p.y)<=90 ? lonlatToWebMerc(p.x,p.y) : p;
}
double distance(Point a,Point b){return std::hypot(a.x-b.x,a.y-b.y);}
struct Graph {
    const Shape& shape;
    std::map<std::string,int> nodes;
    std::map<std::string,std::map<int,std::set<int>>> adj;
    explicit Graph(const Shape& s):shape(s) {
        for(size_t i=0;i<s.nodes.size();++i)nodes[s.nodes[i].uid]=int(i);
        for(const auto& r:s.routes)for(int ei:r.segmentIndices){const auto& e=s.segments[ei];adj[r.id][e.a].insert(e.b);adj[r.id][e.b].insert(e.a);}
    }
    // Only route topology supplies connectivity. Direction comes from the ordered
    // input anchors. Without a shape guide, competing paths are rejected.
    std::vector<int> path(const std::string& route,int a,int b,bool stopBarrier=false, const std::set<int>* barriers=nullptr) const {
        if(a==b)return {a};
        auto ri=adj.find(route);if(ri==adj.end())return {};
        const auto& links=ri->second;
        std::vector<double> cost(shape.nodes.size(),std::numeric_limits<double>::infinity());
        std::vector<int> previous(shape.nodes.size(),-1),ways(shape.nodes.size(),0);
        using Item=std::pair<double,int>;std::priority_queue<Item,std::vector<Item>,std::greater<Item>> queue;
        cost[a]=0;ways[a]=1;queue.push({0,a});
        while(!queue.empty()) {
            auto [d,u]=queue.top();queue.pop();if(d>cost[u]+1e-8)continue;
            if(u==b)break;
            if(stopBarrier&&u!=a&&(barriers ? barriers->count(u)>0 : isStationLike(shape.nodes[u].type)))continue;
            auto it=links.find(u);if(it==links.end())continue;
            for(int v:it->second) {
                double next=d+std::max(1e-6,distance(shape.nodes[u].pos,shape.nodes[v].pos));
                if(next<cost[v]-1e-8){cost[v]=next;previous[v]=u;ways[v]=ways[u];queue.push({next,v});}
                else if(std::abs(next-cost[v])<1e-8)ways[v]=2;
            }
        }
        if(previous[b]<0)return {};
        if(ways[b]>1)throw std::invalid_argument("Ambiguous directional path for route "+route);
        std::vector<int> out;for(int n=b;n!=-1;n=previous[n]){out.push_back(n);if(n==a)break;}
        std::reverse(out.begin(),out.end());
        if(stopBarrier)for(size_t i=1;i<out.size();++i) {
            // Stop order alone cannot choose a branch in a loop, even if one
            // branch is shorter. Check for an alternative to each path edge.
            std::set<int> seen{a};std::vector<int> pending{a};
            while(!pending.empty()) {
                int u=pending.back();pending.pop_back();
                if(u==b)throw std::invalid_argument("Ambiguous GTFS/OCTI traversal: shape guidance is required for route "+route);
                if(u!=a&&(barriers ? barriers->count(u)>0 : isStationLike(shape.nodes[u].type)))continue;
                auto it=links.find(u);if(it==links.end())continue;
                for(int v:it->second) {
                    if((u==out[i-1]&&v==out[i])||(v==out[i-1]&&u==out[i]))continue;
                    if(seen.insert(v).second)pending.push_back(v);
                }
            }
        }
        return out;
    }
    bool edge(const json& r) const {
        auto route=adj.find(r.at("routeId"));auto a=nodes.find(r.at("from")),b=nodes.find(r.at("to"));
        if(route==adj.end()||a==nodes.end()||b==nodes.end())return false;
        auto it=route->second.find(a->second);return it!=route->second.end()&&it->second.count(b->second);
    }
};
void append(json& out,json record,const Shape& s,const std::vector<int>& nodes) {
    for(size_t i=1;i<nodes.size();++i){record["from"]=s.nodes[nodes[i-1]].uid;record["to"]=s.nodes[nodes[i]].uid;out.push_back(record);}
}

}
bool hasRouteDirections(const json& map) {
    for(const auto& f:map.at("features"))if(f.at("geometry").at("type")=="LineString")
        for(const auto& line:f.at("properties").at("lines"))if(line.contains("from")||line.contains("to")||line.contains("directions"))return true;
    return false;
}
void retainMapNodeIds(Shape& s,const json& map) {
    const bool planar=map.value("coordinateSystem","")=="planar";
    for(const auto& f:map.at("features"))if(f.at("geometry").at("type")=="Point") {
        auto p=coordinate(f["geometry"]["coordinates"],planar);
        for(auto& n:s.nodes)if(n.uid.empty()&&distance(n.pos,p)<1e-6){n.uid=id(f["properties"]["id"]);break;}
    }
}
Shape loadDirectedTopology(const json& map) {
    // An exported directed graph is already simplified. Re-simplifying would
    // remove identified shape points and could merge distinct coincident nodes.
    Shape s;std::map<std::string,int> nodes,routes;std::set<std::string> reserved;
    const bool planar=map.value("coordinateSystem","")=="planar";
    for(const auto& f:map.at("features"))if(f["geometry"]["type"]=="Point") {
        const auto& p=f.at("properties");const auto uid=id(p.at("id"));if(nodes.count(uid))continue;
        ShapeNode n;n.id=int(s.nodes.size());n.uid=uid;n.pos=coordinate(f["geometry"]["coordinates"],planar);
        if(p.contains("station_id")||p.contains("station_label")||p.contains("name")) {
            n.type=ShapeNodeType::Station;n.station_id=p.value("station_id","");n.name=p.value("station_label",p.value("name",std::string()));
        }
        nodes[uid]=n.id;reserved.insert(uid);s.nodes.push_back(n);
    }
    int serial=0;
    for(const auto& f:map.at("features"))if(f["geometry"]["type"]=="LineString") {
        const auto& p=f.at("properties");const auto& coords=f["geometry"]["coordinates"];
        int a=nodes.at(id(p.at("from"))),b=nodes.at(id(p.at("to")));
        std::vector<int> chain{a};
        for(size_t i=1;i+1<coords.size();++i){ShapeNode n;n.id=int(s.nodes.size());n.pos=coordinate(coords[i],planar);
            do{n.uid="direction-point-"+std::to_string(serial++);}while(reserved.count(n.uid));
            reserved.insert(n.uid);chain.push_back(n.id);s.nodes.push_back(n);}
        chain.push_back(b);std::vector<int> edges;
        for(size_t i=1;i<chain.size();++i)if(chain[i-1]!=chain[i]) {
            edges.push_back(int(s.segments.size()));s.segments.push_back({chain[i-1],chain[i],chain.size()==2&&p.contains("id")?id(p["id"]):std::string()});
        }
        for(const auto& line:p.at("lines")) {
            const auto rid=id(line.contains("id")?line["id"]:line.at("label"));
            if(!routes.count(rid)) {
                ShapeRoute r;r.id=rid;r.name=line.value("name",line.value("label",rid));r.route_width=line.value("route_width",6.f);
                auto hex=line.value("color",std::string("777777"));if(!hex.empty()&&hex[0]=='#')hex.erase(0,1);
                unsigned rgb=0x777777;try{if(hex.size()==6)rgb=std::stoul(hex,nullptr,16);}catch(...){}
                r.color[0]=((rgb>>16)&255)/255.f;r.color[1]=((rgb>>8)&255)/255.f;r.color[2]=(rgb&255)/255.f;
                routes[rid]=int(s.routes.size());s.routes.push_back(r);
            }
            auto& dest=s.routes[routes.at(rid)].segmentIndices;
            // Opposite traversals may repeat the route ID in public lines[].
            // They are one logical route and one geometry membership.
            for(int edge:edges)if(std::find(dest.begin(),dest.end(),edge)==dest.end())dest.push_back(edge);
        }
    }
    return s;
}
json importRouteDirections(const Shape& s,const json& map) {
    Graph g(s);json records=json::array();size_t edgeOffset=0;
    for(const auto& f:map.at("features"))if(f["geometry"]["type"]=="LineString") {
        const auto& p=f.at("properties");auto a=id(p.at("from")),b=id(p.at("to"));
        // loadDirectedTopology keeps feature order, including every interior
        // sample. Follow this specific segment, never a shorter parallel path.
        std::vector<int> chain{g.nodes.at(a)};
        for(size_t i=1;i<f["geometry"]["coordinates"].size();++i) {
            if(f["geometry"]["coordinates"].size()==2&&a==b)continue;
            const auto& edge=s.segments.at(edgeOffset++);chain.push_back(edge.b);
        }
        for(const auto& line:p.at("lines")) {
            auto items=line.value("directions",json::array());
            if(!items.is_array())throw std::invalid_argument("lines[].directions must be an array");
            if(line.contains("from")||line.contains("to")){if(!items.empty())throw std::invalid_argument("Use from/to or directions, not both");items.push_back(line);}
            for(const auto& item:items) {
                auto from=id(item.at("from")),to=id(item.at("to"));
                if(!((from==a&&to==b)||(from==b&&to==a)))throw std::invalid_argument("Route direction must reference its segment endpoints");
                json record={{"routeId",id(line.contains("id")?line["id"]:line.at("label"))}};
                for(const auto* key:{"direction","pattern","directionId","patternId","traversalIndex"})if(item.contains(key))record[key]=item[key];
                auto path=chain;if(from==b)std::reverse(path.begin(),path.end());
                append(records,record,s,path);
            }
        }
    }
    return records;
}
json mapGtfsDirections(const Shape& s,const json& data,std::uint64_t revision) {
    Graph g(s);
    json result={{"revision",revision},{"traversals",json::array()},{"diagnostics",json::array()}};
    for(const auto& input:orderedTraversals(data)) {
        const auto& pattern=*input.pattern;
        ShapeTraversal t; t.logicalRouteId=input.routeId;t.directionId=pattern.at("directionId");
        t.patternId=pattern.at("patternId");t.revision=revision;
        json diagnostic={{"routeId",t.logicalRouteId},{"directionId",t.directionId},{"patternId",t.patternId}};
        try {
            const auto ri=g.adj.find(t.logicalRouteId);
            if(ri==g.adj.end())throw std::invalid_argument("Logical route is absent from this Shape revision");
            std::vector<int> anchors;
            std::vector<json> origins;
            const auto& stops=pattern.at("orderedStops");
            for(size_t si=0;si<input.samples.size();++si) {
                const auto& sample=input.samples[si];
                json origin={{"sampleIndex",si},{"position",json::array({sample.position.x,sample.position.y})}};
                if(sample.stopIndex>=0)origin["stop"]=stops.at(sample.stopIndex);
                diagnostic["visit"]=origin;
                diagnostic.erase("transition");diagnostic.erase("candidateNodes");
                double best=std::numeric_limits<double>::infinity();int chosen=-1;
                bool exact=false;std::vector<int> candidates;
                for(const auto& entry:ri->second) {
                    const auto& n=s.nodes[entry.first];
                    if(sample.stopIndex>=0&&!isStationLike(n.type))continue;
                    const bool match=sample.stopIndex>=0&&n.station_id==stops[sample.stopIndex].at("stopId");
                    if(match&&!exact){exact=true;best=std::numeric_limits<double>::infinity();candidates.clear();}
                    if(exact&&!match)continue;
                    const double d=distance(sample.position,n.pos);
                    if(d<best-1e-6){best=d;chosen=entry.first;candidates={chosen};}
                    else if(std::abs(d-best)<1e-6)candidates.push_back(entry.first);
                }
                diagnostic["candidateNodes"]=json::array();
                for(int n:candidates)diagnostic["candidateNodes"].push_back({{"nodeId",s.nodes[n].uid},{"distance",best}});
                if(chosen<0)throw std::invalid_argument("No route station candidate for GTFS visit");
                // Explicitly bounded spatial association, never name matching or
                // requiring platform IDs to equal a logical station ID.
                if(sample.stopIndex>=0&&best>250.0)
                    throw std::invalid_argument("Nearest route station exceeds 250 projected-coordinate units; correspondence is unresolved");
                if(candidates.size()>1) {
                    if(sample.stopIndex<0)continue; // An ambiguous geometry sample is not a stop visit.
                    throw std::invalid_argument("Equally plausible Shape station candidates");
                }
                if(sample.stopIndex>=0)t.stopNodes.push_back(chosen);
                if(anchors.empty()||anchors.back()!=chosen){anchors.push_back(chosen);origins.push_back(origin);}
            }
            if(anchors.empty())throw std::invalid_argument("No Shape anchors");
            // Barriers are observed ordered anchors, not every Shape station.
            // Thus adding a station between GTFS stops does not break a traversal.
            const std::set<int> barriers(anchors.begin(),anchors.end());
            t.orderedNodes.push_back(anchors.front());
            const auto route=std::find_if(s.routes.begin(),s.routes.end(),[&](const auto& r){return r.id==t.logicalRouteId;});
            for(size_t i=1;i<anchors.size();++i) {
                diagnostic["transition"]={{"fromVisit",origins[i-1]},{"toVisit",origins[i]},
                    {"fromNode",s.nodes[anchors[i-1]].uid},{"toNode",s.nodes[anchors[i]].uid}};
                diagnostic.erase("visit");diagnostic.erase("candidateSegments");
                diagnostic["candidateNodes"]=json::array({s.nodes[anchors[i-1]].uid,s.nodes[anchors[i]].uid});
                const auto path=g.path(t.logicalRouteId,anchors[i-1],anchors[i],true,&barriers);
                if(path.empty())throw std::invalid_argument("No route connection between ordered anchors without crossing another traversal anchor");
                for(size_t j=1;j<path.size();++j) {
                    std::vector<int> edges;
                    for(int ei:route->segmentIndices) {const auto& e=s.segments[ei];
                        if((e.a==path[j-1]&&e.b==path[j])||(e.b==path[j-1]&&e.a==path[j]))edges.push_back(ei);
                    }
                    diagnostic["candidateSegments"]=json::array();
                    for(int ei:edges)diagnostic["candidateSegments"].push_back(s.segments[ei].uid);
                    if(edges.size()!=1)throw std::invalid_argument("Parallel Shape segments need an explicit geometric correspondence");
                    t.orderedSegments.push_back(edges.front());t.orderedNodes.push_back(path[j]);
                }
            }
            json mapped={{"routeId",t.logicalRouteId},{"directionId",t.directionId},{"patternId",t.patternId},
                {"orderedNodes",json::array()},{"orderedSegments",json::array()},{"stopNodes",json::array()}};
            for(int n:t.orderedNodes)mapped["orderedNodes"].push_back(s.nodes[n].uid);
            for(int e:t.orderedSegments)mapped["orderedSegments"].push_back(s.segments[e].uid);
            for(int n:t.stopNodes)mapped["stopNodes"].push_back(s.nodes[n].uid);
            result["traversals"].push_back(mapped);
        } catch(const std::invalid_argument& e) {
            diagnostic["reason"]=e.what();result["diagnostics"].push_back(diagnostic);
        }
    }
    return result;
}
void exportShapeTraversals(json& graph,const Shape& shape,const json& mapping,std::uint64_t revision) {
    if(mapping.at("revision").get<std::uint64_t>()!=revision)
        throw std::invalid_argument("Stale Shape traversal revision");
    // Validate every segment reference before exposing even a partial mapping.
    std::map<std::string,json> index;
    for(const auto& t:mapping.at("traversals")) {
        const auto& ns=t.at("orderedNodes");const auto& es=t.at("orderedSegments");
        if(ns.size()!=es.size()+1)throw std::invalid_argument("Invalid Shape traversal sequence");
        auto route=std::find_if(shape.routes.begin(),shape.routes.end(),[&](const auto& r){return r.id==t.at("routeId");});
        if(route==shape.routes.end())throw std::invalid_argument("Stale Shape traversal route");
        for(size_t i=0;i<es.size();++i) {
            bool valid=false;
            for(int ei:route->segmentIndices){const auto& e=shape.segments[ei];
                if(e.uid==es[i]&&((shape.nodes[e.a].uid==ns[i]&&shape.nodes[e.b].uid==ns[i+1])||
                                  (shape.nodes[e.b].uid==ns[i]&&shape.nodes[e.a].uid==ns[i+1])))valid=true;
            }
            if(!valid)throw std::invalid_argument("Stale Shape traversal segment: "+es[i].get<std::string>());
            const auto key = json::array({ t.at("routeId"),es[i] }).dump();
            if (!index.count(key))index[key] = json::array();

            const json occurrence = {
                {"from", ns[i]},
                {"to", ns[i + 1]}
            };

            bool exists = false;
            for (const auto& existing : index[key]) {
                if (existing.at("from") == occurrence.at("from") &&
                    existing.at("to") == occurrence.at("to")) {
                    exists = true;
                    break;
                }
            }

            if (!exists)
                index[key].push_back(occurrence);
        }
    }
    for(auto& f:graph["features"])if(f["geometry"]["type"]=="LineString") {
        auto& p=f["properties"];
        json lines=json::array();
        for(const auto& line:p["lines"]) {
            auto it=index.find(json::array({line.at("id"),p.at("id")}).dump());
            if(it==index.end()){lines.push_back(line);continue;}
            for(const auto& traversal:it->second) {
                auto occurrence=line;
                occurrence["from"]=traversal.at("from");occurrence["to"]=traversal.at("to");
                lines.push_back(occurrence);
            }
        }
        p["lines"]=lines;
    }
    // Public legacy JSON exposes only per-line ordered endpoints.
    // GTFS provenance and the complete ordered mapping remain in session state.
}
json editRouteDirections(const Shape& before,const Shape& after,const json& records,const std::string& op,const json& req) {
    Graph old(before),now(after);json changed=records;
    if(op=="merge-stations") {
        std::set<std::string> ids;int keep=int(before.nodes.size());
        for(const auto& n:req.at("nodeIds")){auto uid=id(n);ids.insert(uid);keep=std::min(keep,old.nodes.at(uid));}
        for(auto& r:changed)for(const auto* key:{"from","to"})if(ids.count(r.at(key)))r[key]=before.nodes[keep].uid;
    } else if(op=="split-station") {
        std::string added;for(const auto& n:after.nodes)if(!old.nodes.count(n.uid)){added=n.uid;break;}
        std::set<std::string> routes;for(const auto& r:req.at("routeIds"))routes.insert(id(r));
        for(auto& r:changed)if(routes.count(r.at("routeId")))for(const auto* key:{"from","to"})if(r.at(key)==req.at("nodeId"))r[key]=added;
    } else if(op=="add-route"&&req.contains("nodeIds")) {
        const auto& ns=req.at("nodeIds");for(size_t i=1;i<ns.size();++i)changed.push_back({{"routeId",id(req.at("routeId"))},{"from",id(ns[i-1])},{"to",id(ns[i])}});
    } else if(op=="delete-node") {
        // Compose only compatible directed incidences through the removed node.
        const auto uid=id(req.at("nodeId"));
        for(const auto& a:records)if(a.at("to")==uid)for(const auto& b:records)if(b.at("from")==uid) {
            auto left=a,right=b;left.erase("from");left.erase("to");right.erase("from");right.erase("to");
            if(left==right&&a.at("from")!=b.at("to")){auto r=a;r["to"]=b.at("to");changed.push_back(r);}
        }
    }
    json out=json::array();
    for(const auto& r:changed) {
        if(now.edge(r)){out.push_back(r);continue;}
        if((op=="split-segment"||op=="add-station")&&now.nodes.count(r.at("from"))&&now.nodes.count(r.at("to"))) {
            // A split inserts nodes on this exact former segment (including
            // other collinear segments split by the desktop operation).
            auto a=now.nodes.at(r.at("from")),b=now.nodes.at(r.at("to"));
            auto path=now.path(r.at("routeId"),a,b);
            double length=0;for(size_t i=1;i<path.size();++i)length+=distance(after.nodes[path[i-1]].pos,after.nodes[path[i]].pos);
            if(!path.empty()&&std::abs(length-distance(after.nodes[a].pos,after.nodes[b].pos))<1e-5)append(out,r,after,path);
            else throw std::invalid_argument("Cannot preserve traversal across segment split");
        }
    }
    return out;
}
json remapRouteDirections(const Shape& before,const Shape& after,const json& records) {
    // Legacy records have only explicitly supplied per-edge traversal. They do
    // not justify reconstructing an ordered GTFS trip from a directed edge set.
    Graph now(after);std::map<std::string,int> mapped;
    for(const auto& n:before.nodes) {
        if(now.nodes.count(n.uid)){mapped[n.uid]=now.nodes.at(n.uid);continue;}
        if(n.station_id.empty())continue;
        int match=-1;
        for(size_t i=0;i<after.nodes.size();++i)if(after.nodes[i].station_id==n.station_id) {
            if(match>=0){match=-2;break;}match=int(i);
        }
        if(match>=0)mapped[n.uid]=match;
    }
    json out=json::array();
    for(const auto& record:records) {
        const auto a=record.at("from").get<std::string>(),b=record.at("to").get<std::string>();
        if(!mapped.count(a)||!mapped.count(b))throw std::invalid_argument(
            "Cannot reconcile explicit legacy traversal without GTFS source: "+record.dump());
        const auto path=now.path(record.at("routeId"),mapped.at(a),mapped.at(b),true);
        if(path.size()<2)throw std::invalid_argument("Lost explicit legacy traversal: "+record.dump());
        append(out,record,after,path);
    }
    return out;
}
void exportRouteDirections(json& graph,const json& records) {
    std::map<std::string,json> index;
    for(const auto& record:records) {
        auto a=record.at("from").get<std::string>(),b=record.at("to").get<std::string>();if(b<a)std::swap(a,b);
        const auto key=json::array({record.at("routeId"),a,b}).dump();
        if(!index.count(key))index[key]=json::array();
        const json occurrence = { {"from",record.at("from")},{"to",record.at("to")} };

        bool exists = false;
        for (const auto& existing : index[key]) {
            if (existing.at("from") == occurrence.at("from") &&
                existing.at("to") == occurrence.at("to")) {
                exists = true;
                break;
            }
        }

        if (!exists)index[key].push_back(occurrence);
    }
    for(auto& f:graph["features"])if(f["geometry"]["type"]=="LineString") {
        auto& p=f["properties"];auto a=p.at("from").get<std::string>(),b=p.at("to").get<std::string>();if(b<a)std::swap(a,b);
        json lines=json::array();
        for(const auto& line:p["lines"]) {
            auto it=index.find(json::array({line.at("id"),a,b}).dump());
            if(it==index.end()){lines.push_back(line);continue;}
            for(const auto& traversal:it->second) {
                auto occurrence=line;
                occurrence["from"]=traversal.at("from");occurrence["to"]=traversal.at("to");
                lines.push_back(occurrence);
            }
        }
        p["lines"]=lines;
    }
}

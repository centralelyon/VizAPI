"""Local crossing elements, ordering and preview geometry (server only)."""
from collections import defaultdict
import hashlib
import json
import math



def flatten(commands, tolerance):
    """Adaptive Bezier subdivision in the same canvas coordinates as the renderer."""
    result, current = [], None
    def curve(points, depth=0):
        a, b = points[0], points[-1]
        length = math.dist(a, b)
        error = max((abs((b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0])) / length
                     if length else math.dist(a, p)) for p in points[1:-1])
        if error <= tolerance or depth == 12:
            return [b]
        levels = [points]
        while len(levels[-1]) > 1:
            levels.append([((a[0]+b[0])/2, (a[1]+b[1])/2) for a,b in zip(levels[-1], levels[-1][1:])])
        return curve([row[0] for row in levels], depth+1) + curve([row[-1] for row in reversed(levels)], depth+1)
    for index, command in enumerate(commands):
        kind = command['type']
        if kind == 'close':
            current = None
            continue
        point = (command['x'], command['y'])
        if kind == 'move' or current is None:
            current = point
            continue
        controls = ([current, (command['cx'], command['cy']), point] if kind == 'quadratic' else
                    [current, (command['c1x'], command['c1y']), (command['c2x'], command['c2y']), point] if kind == 'cubic' else None)
        samples = curve(controls) if controls else [point]
        for sample in samples:
            if math.dist(current, sample) > 1e-7:
                result.append((current, sample, index))
            current = sample
    return result


def intersection(a, b, c, d):
    ux, uy, vx, vy = b[0]-a[0], b[1]-a[1], d[0]-c[0], d[1]-c[1]
    det = ux*vy-uy*vx
    if abs(det) <= 1e-8 * math.hypot(ux,uy)*math.hypot(vx,vy):
        return None  # Shared/collinear segments are a different element.
    dx, dy = c[0]-a[0], c[1]-a[1]
    t, u = (dx*vy-dy*vx)/det, (dx*uy-dy*ux)/det
    if -1e-8 <= t <= 1+1e-8 and -1e-8 <= u <= 1+1e-8:
        return (a[0]+t*ux, a[1]+t*uy)


def clip_line(a, b, bounds):
    x, y, w, h = bounds
    dx, dy = b[0]-a[0], b[1]-a[1]
    lo, hi = 0., 1.
    for p,q in [(-dx,a[0]-x),(dx,x+w-a[0]),(-dy,a[1]-y),(dy,y+h-a[1])]:
        if abs(p)<1e-12:
            if q<0: return None
        elif p<0: lo=max(lo,q/p)
        else: hi=min(hi,q/p)
    return ((a[0]+lo*dx,a[1]+lo*dy),(a[0]+hi*dx,a[1]+hi*dy)) if lo<=hi else None


def route_data(routes):
    paths={}
    strokes=[]
    for route in routes:
        for path in route['paths']:
            key=(route['routeId'],str(path['id']))
            distance=0.
            parts=[]
            for a,b,command in flatten(path['commands'], .1):
                length=math.dist(a,b)
                parts.append((a,b,distance,distance+length))
                strokes.append({'route':key[0],'path':key,'command':command,'a':a,'b':b,'start':distance,'length':length})
                distance+=length
            paths[key]=(parts,distance)
    return paths,strokes


def path_slice(paths,key,start,end,canonical_path):
    """Transfer a network interval to its rendered path, on the server."""
    canonical_parts,canonical_length=canonical_path
    if key not in paths or canonical_length==0:return []
    parts,length=paths[key]
    def locate(distance):
        if distance<=1e-5:return 0.
        if distance>=canonical_length-1e-5:return length
        a,b,lo,hi=next(part for part in canonical_parts if part[3]>=distance-1e-5)
        fraction=max(0,min(1,(distance-lo)/(hi-lo)))
        point=[a[i]+(b[i]-a[i])*fraction for i in (0,1)]
        candidates=[]
        for a,b,lo,hi in parts:
            dx,dy=b[0]-a[0],b[1]-a[1]
            t=max(0,min(1,((point[0]-a[0])*dx+(point[1]-a[1])*dy)/(dx*dx+dy*dy)))
            along=lo+t*(hi-lo)
            error=(point[0]-a[0]-t*dx)**2+(point[1]-a[1]-t*dy)**2
            candidates.append((error+1e-8*(along-length*distance/canonical_length)**2,along))
        return min(candidates)[1]
    start,end=locate(start),locate(end)
    commands=[]
    for a,b,lo,hi in parts:
        if hi<=start or lo>=end:continue
        first=max(start,lo);last=min(end,hi)
        def at(t):return [a[i]+(b[i]-a[i])*(t-lo)/(hi-lo) for i in (0,1)]
        x,y=at(first)
        if not commands:commands.append({'type':'move','x':x,'y':y})
        x,y=at(last);commands.append({'type':'line','x':x,'y':y})
    return commands


def network_elements(routes):
    """Fixed network coordinates only: no station data, camera scale or lane width."""
    paths,strokes=route_data(routes)
    grid=defaultdict(list);events={};splits=defaultdict(list)
    cell=64.
    def node(p):return tuple(round(v,5) for v in p)
    # Identical shared bends are not crossings; diverging/shared junctions are.
    rays=defaultdict(lambda:defaultdict(set))
    for stroke in strokes:
        a,b=stroke['a'],stroke['b'];rid=stroke['route']
        rays[node(a)][rid].add(round(math.atan2(b[1]-a[1],b[0]-a[0]),7))
        rays[node(b)][rid].add(round(math.atan2(a[1]-b[1],a[0]-b[0]),7))
    for i,stroke in enumerate(strokes):
        a,b=stroke['a'],stroke['b'];rid=stroke['route']
        keys=[(x,y) for x in range(math.floor(min(a[0],b[0])/cell),math.floor(max(a[0],b[0])/cell)+1)
                    for y in range(math.floor(min(a[1],b[1])/cell),math.floor(max(a[1],b[1])/cell)+1)]
        for j in sorted(set(j for key in keys for j in grid[key])):
            other=strokes[j]
            if other['route']==rid:continue
            point=intersection(a,b,other['a'],other['b'])
            if point is None:
                # Shared collinear strokes can have different shape vertices.
                # Split them into identical graph edges without adding crossings.
                c,d=other['a'],other['b']
                for q in (c,d):
                    if abs(math.dist(a,q)+math.dist(q,b)-math.dist(a,b))<1e-6:splits[i].append(node(q))
                for q in (a,b):
                    if abs(math.dist(c,q)+math.dist(q,d)-math.dist(c,d))<1e-6:splits[j].append(node(q))
                continue
            key=node(point)
            if rays[key].get(rid) and rays[key].get(rid)==rays[key].get(other['route']):continue
            event=events.setdefault(key,{'point':key,'routes':set(),'identities':set()})
            event['routes'].update((rid,other['route']))
            event['identities'].add(tuple(sorted(((rid,stroke['path'][1],stroke['command']),(other['route'],other['path'][1],other['command'])))))
            splits[i].append(key);splits[j].append(key)
        for key in keys:grid[key].append(i)
    occurrences=defaultdict(list)
    for event in events.values():occurrences[repr(sorted(event['identities']))].append(event)
    for identity,items in occurrences.items():
        for index,event in enumerate(sorted(items,key=lambda e:e['point'])):
            event['id']='network:'+hashlib.sha1((identity+':'+str(index)).encode()).hexdigest()[:20]
    # Atomic crossings stay independent of zoom and retain their saved orders.
    groups=[{'events':[event]} for event in sorted(events.values(),key=lambda e:e['id'])]
    # Split the graph at crossings. Ordinary shape vertices remain traversable.
    edges={};adjacency=defaultdict(list)
    for i,stroke in enumerate(strokes):
        a,b=stroke['a'],stroke['b']
        points=sorted(set([node(a),node(b),*splits[i]]),key=lambda q:math.dist(a,q))
        for first,last in zip(points,points[1:]):
            if math.dist(first,last)<1e-6:continue
            key=tuple(sorted((first,last)))
            if key not in edges:
                edges[key]={'routes':set(),'intervals':[]}
                adjacency[first].append(key);adjacency[last].append(key)
            edges[key]['routes'].add(stroke['route'])
            edges[key]['intervals'].append((stroke['path'],stroke['start']+math.dist(a,first),stroke['start']+math.dist(a,last)))
    stops={e['point'] for e in events.values()}
    for group in groups:
        group['id']=min(e['id'] for e in group['events'])
        group['members']=set(r for e in group['events'] for r in e['routes'])
        starts={e['point'] for e in group['events']}
        reached=set()
        for rid in group['members']:
            queue=list(starts);visited=set()
            while queue:
                point=queue.pop()
                if point in visited:continue
                visited.add(point)
                if point in stops and point not in starts:continue
                for key in adjacency[point]:
                    if rid not in edges[key]['routes']:continue
                    reached.add(key);queue.append(key[1] if key[0]==point else key[0])
        group['edges']=reached
    return paths,groups,edges


def ordered_members(routes,members,record):
    order=[r for r in record.get('order',[]) if r in members]
    return order+[r['routeId'] for r in reversed(routes) if r['routeId'] in members and r['routeId'] not in order]


def square_box(box):
    x,y,right,bottom=box
    size=max(right-x,bottom-y)
    cx,cy=(x+right)/2,(y+bottom)/2
    return [cx-size/2,cy-size/2,cx+size/2,cy+size/2]


def stroke_rectangle(a,b,radius):
    length=math.dist(a,b)
    if length==0:return []
    ux,uy=(b[0]-a[0])/length,(b[1]-a[1])/length
    nx,ny=-uy*radius,ux*radius
    # Include round end caps and joins conservatively.
    a=(a[0]-ux*radius,a[1]-uy*radius)
    b=(b[0]+ux*radius,b[1]+uy*radius)
    return [(a[0]-nx,a[1]-ny),(b[0]-nx,b[1]-ny),
            (b[0]+nx,b[1]+ny),(a[0]+nx,a[1]+ny)]


def polygon_overlap(subject,clip):
    for a,b in zip(clip,clip[1:]+clip[:1]):
        if not subject:break
        def side(p):return (b[0]-a[0])*(p[1]-a[1])-(b[1]-a[1])*(p[0]-a[0])
        output=[];previous=subject[-1];before=side(previous)
        for point in subject:
            after=side(point)
            if (before>=0)!=(after>=0):
                t=before/(before-after)
                output.append((previous[0]+t*(point[0]-previous[0]),previous[1]+t*(point[1]-previous[1])))
            if after>=0:output.append(point)
            previous=point;before=after
        subject=output
    return subject


def display_groups(atoms,route_strokes,route_map,scale,mode):
    groups=[]
    for atom in atoms:
        point=atom['events'][0]['point'];footprint=[]
        for rid in atom['members']:
            nearest=None;distance=math.inf
            for a,b in route_strokes[rid]:
                dx,dy=b[0]-a[0],b[1]-a[1]
                length2=dx*dx+dy*dy
                t=max(0,min(1,((point[0]-a[0])*dx+(point[1]-a[1])*dy)/length2)) if length2 else 0
                q=(a[0]+t*dx,a[1]+t*dy)
                d=math.dist(point,q)
                if d<distance:nearest=q;distance=d
            if nearest is not None:
                radius=((route_map[rid]['width'] if mode=='styled' else 1)/2+4)*scale
                footprint.append([nearest[0]-radius,nearest[1]-radius,nearest[0]+radius,nearest[1]+radius])
        # Width-expanded strokes overlap farther along an oblique crossing
        # than half a route width. Include that entire overlap in the cap.
        seed=[min(b[0] for b in footprint),min(b[1] for b in footprint),
              max(b[2] for b in footprint),max(b[3] for b in footprint)]
        nearby=[]
        for rid in sorted(atom['members']):
            radius=(route_map[rid]['width'] if mode=='styled' else 1)*scale/2
            for a,b in route_strokes[rid]:
                if not clip_line(a,b,[seed[0]-radius,seed[1]-radius,
                                     seed[2]-seed[0]+2*radius,seed[3]-seed[1]+2*radius]):continue
                polygon=stroke_rectangle(a,b,radius)
                if polygon:nearby.append((rid,a,b,polygon))
        for i,(rid,a,b,polygon) in enumerate(nearby):
            for other,c,d,clip in nearby[:i]:
                if rid==other:continue
                cross=(b[0]-a[0])*(d[1]-c[1])-(b[1]-a[1])*(d[0]-c[0])
                if abs(cross)<1e-8*math.dist(a,b)*math.dist(c,d):continue
                overlap=polygon_overlap(polygon,clip)
                if overlap:
                    pad=4*scale
                    footprint.append([min(p[0] for p in overlap)-pad,min(p[1] for p in overlap)-pad,
                                      max(p[0] for p in overlap)+pad,max(p[1] for p in overlap)+pad])
        atom['box']=square_box([min(b[0] for b in footprint),min(b[1] for b in footprint),
                                max(b[2] for b in footprint),max(b[3] for b in footprint)])
        group={'atoms':[atom],'box':atom['box'][:]}
        changed=True
        while changed:
            changed=False
            for old in groups[:]:
                a,b=group['box'],old['box']
                if a[0]<=b[2] and b[0]<=a[2] and a[1]<=b[3] and b[1]<=a[3]:
                    group['atoms']+=old['atoms']
                    group['box']=square_box([min(a[0],b[0]),min(a[1],b[1]),max(a[2],b[2]),max(a[3],b[3])])
                    groups.remove(old);changed=True
        groups.append(group)
    for group in groups:
        group['events']=[e for atom in group['atoms'] for e in atom['events']]
        group['id']=min(e['id'] for e in group['events'])
        group['members']=set(r for atom in group['atoms'] for r in atom['members'])
    return groups


def crossing_records(atoms,orders,routes):
    for atom in atoms:
        atom['record']=orders.get(atom['id'],{})
        atom['order']=ordered_members(routes,atom['members'],atom['record'])
    edge_records={}
    for group in sorted(atoms,key=lambda g:g['record'].get('serial',0)):
        if not group['record']:continue
        for key in group['edges']:edge_records[key]=group['record']
    # An unedited crossing inherits the order reaching it along its segments.
    # Its cap must not repaint that propagated order with the default route order.
    for atom in atoms:
        if atom['record']:continue
        point=atom['events'][0]['point']
        incoming=[record for key,record in edge_records.items() if point in key
                  and len(atom['members'].intersection(record['order']))>1]
        atom['record']=max(incoming,key=lambda record:record.get('serial',0),default={})
        atom['order']=ordered_members(routes,atom['members'],atom['record'])
    return edge_records


def crossing_scene(routes, network, orders, scale, mode, edit=None):
    canonical,network_atoms,edges=network
    atoms=[dict(atom) for atom in network_atoms]
    rendered,strokes=route_data(routes)
    route_map={r['routeId']:r for r in routes}
    route_strokes=defaultdict(list)
    for stroke in strokes:route_strokes[stroke['route']].append((stroke['a'],stroke['b']))
    groups=display_groups(atoms,route_strokes,route_map,scale,mode)
    crossing_records(atoms,orders,routes)
    found=False
    for group in groups:
        ids=[e['id'] for e in group['events']]
        record=max((atom['record'] for atom in group['atoms']),key=lambda item:item.get('serial',0),default={})
        group['order']=ordered_members(routes,group['members'],record)
        if edit and group['id']==edit['crossingId']:
            found=True;order=group['order'];rid=edit['routeId'];target=edit.get('beforeRouteId')
            if rid not in order or (target is not None and target not in order):raise ValueError('Route is no longer in this crossing')
            if rid!=target:
                order.remove(rid);order.insert(order.index(target) if target is not None else len(order),rid)
                record={'order':order[:],'serial':1+max((o.get('serial',0) for o in orders.values()),default=0)}
                for eid in ids:orders[eid]=record
        group['record']=record
    if edit and not found:raise ValueError('Crossing changed; select it again')
    # On a shared section, the latest adjacent edit supplies one coherent order.
    # Paint entire graph intervals, not rectangular snippets at shape points.
    edge_records=crossing_records(atoms,orders,routes)
    for group in groups:
        record=max((atom['record'] for atom in group['atoms']),key=lambda record:record.get('serial',0),default={})
        group['order']=ordered_members(routes,group['members'],record)
    buckets=defaultdict(lambda:defaultdict(list))
    for key,record in edge_records.items():
        edge=edges[key]
        order=ordered_members(routes,edge['routes'],record)
        signature=(record['serial'],tuple(order))
        for path,start,end in edge['intervals']:buckets[signature][path].append((start,end))
    paint=[]
    for (serial,order),intervals in sorted(buckets.items()):
        for rid in reversed(order):
            for key,spans in intervals.items():
                if key[0]!=rid:continue
                merged=[]
                for start,end in sorted(spans):
                    if merged and start<=merged[-1][1]+1e-4:merged[-1][1]=max(end,merged[-1][1])
                    else:merged.append([start,end])
                for start,end in merged:
                    commands=path_slice(rendered,key,start,end,canonical[key])
                    if commands:paint.append({'routeId':rid,'commands':commands,'width':route_map[rid]['width'] if mode=='styled' else 1})
    output=[]
    for group in groups:
        x,y,right,bottom=group['box'];order=group['order'];members=group['members']
        marker_size=max(right-x,bottom-y)
        markers=[{'x':(x+right-marker_size)/2,'y':(y+bottom-marker_size)/2,'width':marker_size,'height':marker_size}]
        # Preview area never limits propagation. Station symbols do not take part.
        pad=max(marker_size*.25,4*scale)
        bounds=[x-pad,y-pad,right-x+2*pad,bottom-y+2*pad]
        local=[r['routeId'] for r in routes if r['routeId'] not in members]+list(reversed(order))
        layers=[{'routeId':rid,'width':route_map[rid]['width'] if mode=='styled' else 1} for rid in local
                if any(clip_line(a,b,bounds) for a,b in route_strokes[rid])]
        factor=170/max(bounds[2],bounds[3],1);height=min(24,100/max(1,len(order)-1));projection=[]
        for level,rid in enumerate(reversed(order)):
            def project(p):
                dx,dy=p[0]-(x+right)/2,p[1]-(y+bottom)/2
                return [160+(dx-dy)*.7*factor,165+(dx+dy)*.32*factor-level*height]
            segments=[list(map(project,part)) for a,b in route_strokes[rid] if (part:=clip_line(a,b,bounds))]
            bx,by,w,h=bounds
            projection.append({'routeId':rid,'segments':segments,'plate':list(map(project,[(bx,by),(bx+w,by),(bx+w,by+h),(bx,by+h)])),
                               'width':max(2,min(12,(route_map[rid]['width'] if mode=='styled' else 1)*scale*factor))})
        size=max(bounds[2],bounds[3])
        # A small crossing-only cap preserves the next crossing's own order.
        # No rectangular redraw is allowed to end at a shape point.
        caps=[]
        for atom in group['atoms']:
            ax,ay,ar,ab=atom['box']
            local_order=[r['routeId'] for r in routes if r['routeId'] not in atom['members']]+list(reversed(atom['order']))
            caps.append({'id':atom['id'],'bounds':{'x':ax,'y':ay,'width':ar-ax,'height':ab-ay},
                         'layers':[{'routeId':rid,'width':route_map[rid]['width'] if mode=='styled' else 1} for rid in local_order]})
        output.append({'id':group['id'],'caps':caps,'eventIds':[e['id'] for e in group['events']],'bounds':dict(zip(('x','y','width','height'),[x,y,right-x,bottom-y])),
                       'view':{'x':(x+right-size)/2,'y':(y+bottom-size)/2,'w':size,'h':size},
                       'markers':markers,'order':order,'layers':layers,'projection':projection})
    return output,paint


def add_crossings(result,payload):
    if 'renderGeometries' not in result:return result
    state=result['state'];scale=result.get('spacingScale',1);edit=payload.get('crossingEdit')
    if edit and (edit.get('side') not in result['renderGeometries'] or edit.get('mode') not in ('styled','geometry')):raise ValueError('Invalid crossing window')
    networks={}
    for side,scene in result['renderGeometries'].items():
        orders=state.setdefault('styles',{}).setdefault(side,{}).setdefault('crossingOrders',{})
        geometry=scene.get('geometryRoutes',[])
        key=json.dumps(geometry,separators=(',',':'))
        if key not in networks:networks[key]=network_elements(geometry)
        network=networks[key]
        # Process the edited mode first; both views share the same network order.
        modes=[('styled','routes'),('geometry','geometryRoutes')]
        if edit and edit['mode']=='geometry':modes.reverse()
        for mode,key in modes:
            scene[mode+'Crossings'],scene[mode+'CrossingPaint']=crossing_scene(scene.get(key,[]),network,orders,scale,mode,
                edit if edit and edit['side']==side and edit['mode']==mode else None)
        scene['crossingOrders']=orders
    return result

"""GTFS persistence and server-side animation planning (no per-frame core calls)."""
import base64
import csv
import gzip
import hashlib
import heapq
import io
import json
import math
from collections import defaultdict
from functools import lru_cache


def read_tables(archive):
    tables = {}
    for name in ('stop_times', 'trips', 'stops', 'calendar', 'calendar_dates', 'frequencies', 'agency'):
        if name + '.txt' in archive.namelist():
            with archive.open(name + '.txt') as stream:
                reader = csv.reader(io.TextIOWrapper(stream, encoding='utf-8-sig', newline=''))
                tables[name] = {'columns': next(reader), 'rows': list(reader)}
    return tables


def pack(tables):
    raw = json.dumps(tables, ensure_ascii=False, separators=(',', ':')).encode()
    return {'version': 1, 'feeds': [{'id': hashlib.sha256(raw).hexdigest(),
            'data': base64.b64encode(gzip.compress(raw, mtime=0)).decode()}]}


def unpack(data):
    tables = json.loads(gzip.decompress(base64.b64decode(data)))
    return {name: [dict(zip(table['columns'], row)) for row in table['rows']]
            for name, table in tables.items()}


def preserve(document, state):
    if 'transitMapTimetable' in state.get('original', {}):
        document['transitMapTimetable'] = {**state['original']['transitMapTimetable'],
            'routeSources':state.get('timelineRouteSources', state['original']['transitMapTimetable'].get('routeSources', {}))}


def merge(original, incoming):
    feeds = {f['id']: f for source in (original, incoming)
             for f in source.get('transitMapTimetable', {}).get('feeds', [])}
    if feeds:
        original['transitMapTimetable'] = {'version': 1, 'feeds': list(feeds.values())}


def seconds(value):
    if not value:
        return None
    h, m, s = map(int, value.split(':'))
    return h * 3600 + m * 60 + s


@lru_cache(maxsize=2)
def timetable(data):
    tables = unpack(data)
    parents = {s['stop_id']: s.get('parent_station') for s in tables['stops']}
    def root(stop):
        seen = set()
        while parents.get(stop) and stop not in seen:
            seen.add(stop)
            stop = parents[stop]
        return stop
    times = defaultdict(list)
    for row in tables['stop_times']:
        a, d = seconds(row.get('arrival_time')), seconds(row.get('departure_time'))
        times[row['trip_id']].append((int(row['stop_sequence']), root(row['stop_id']), a if a is not None else d, d if d is not None else a))
    for trip, rows in times.items():
        rows.sort()
        # GTFS permits untimed intermediate stops. Interpolate between timed stops.
        known = [i for i, r in enumerate(rows) if r[2] is not None]
        for left, right in zip(known, known[1:]):
            for i in range(left + 1, right):
                t = rows[left][3] + (rows[right][2] - rows[left][3]) * (i-left)/(right-left)
                rows[i] = (*rows[i][:2], t, t)
        times[trip] = [r[1:] for r in rows if r[2] is not None]
    frequencies = defaultdict(list)
    for row in tables.get('frequencies', []):
        frequencies[row['trip_id']].append(row)
    calendar = {name: tables.get(name, []) for name in ('trips','calendar','calendar_dates','agency')}
    return calendar, times, frequencies


def distance(a, b):
    return math.hypot(a[0]-b[0], a[1]-b[1])


def flatten(commands):
    points = []
    for c in commands:
        end = (c['x'], c['y'])
        if c['type'] == 'quadratic' and points:
            start = points[-1]
            control = (c['cx'], c['cy'])
            count = max(4, min(64, math.ceil((distance(start, control)+distance(control, end))/2)))
            for i in range(1, count+1):
                t = i/count
                points.append(tuple((1-t)**2*start[k]+2*t*(1-t)*control[k]+t*t*end[k] for k in (0, 1)))
        else:
            points.append(end)
    return points


def projection(point, points):
    best = (float('inf'), 0, points[0])
    offset = 0
    for a, b in zip(points, points[1:]):
        length = distance(a, b)
        t = max(0, min(1, sum((point[k]-a[k])*(b[k]-a[k]) for k in (0, 1))/(length*length))) if length else 0
        p = tuple(a[k]+t*(b[k]-a[k]) for k in (0, 1))
        candidate = (distance(point, p), offset+t*length, p)
        if candidate[0] < best[0]:
            best = candidate
        offset += length
    return best[1:]


def slice_path(points, start, end):
    a, p = start
    b, q = end
    if a > b:
        return list(reversed(slice_path(points, end, start)))
    result, offset = [p], 0
    for x, y in zip(points, points[1:]):
        offset += distance(x, y)
        if a < offset < b:
            result.append(y)
    result.append(q)
    return result


class RoutePaths:
    def __init__(self, state, geometry, route, direction, pattern, styled):
        shape = state["shape"]
        nodes = {n['id']: n for n in geometry.get('displayNodes', shape['nodes'])}
        topology = {p['id']: p for r in shape['routes'] for p in r.get('paths', [])}
        anchors = {s['nodeId']: s['point'] for s in geometry.get('stations', []) if route['routeId'] in s['routeIds']} if styled else {}
        self.graph = defaultdict(list)
        self.stops = defaultdict(list)
        self.cache = {}
        self.ordered = next((r['orderedNodes'] for r in state.get('shapeTraversals', {}).get('traversals', [])
                             if r['routeId']==route['routeId'] and r['directionId']==direction and r['patternId']==pattern), [])
        rendered_patterns = {p.get('patternId') for p in route['paths'] if p.get('directionId') in (None, '', direction)}
        rendered_pattern = pattern if pattern in rendered_patterns else None
        for path in route['paths']:
            if path.get('directionId') not in (None, '', direction):
                continue
            if rendered_pattern is not None and path.get('patternId') not in (None, rendered_pattern):
                continue
            ids = path.get('nodeIds', topology.get(path['id'], {}).get('nodeIds', []))
            if len(ids) < 2:
                continue
            if path.get('patternId') is not None:
                self.ordered = ids
            points = flatten(path['commands'])
            if len(points) < 2:
                continue
            # Renderer and topology share path order, including repeated loop nodes.
            positions = [projection(anchors.get(n, nodes[n]['position']), points) for n in ids]
            positions[0] = (0, points[0])
            positions[-1] = (sum(distance(a,b) for a,b in zip(points,points[1:])), points[-1])
            for i, (a, b) in enumerate(zip(ids, ids[1:])):
                segment = slice_path(points, positions[i], positions[i+1])
                weight = sum(distance(x,y) for x,y in zip(segment,segment[1:]))
                self.graph[a].append((b, weight, segment))
                self.graph[b].append((a, weight, list(reversed(segment))))
            for n in ids:
                station = nodes[n].get('stationId')
                if station and n not in self.stops[station]:
                    self.stops[station].append(n)

    def legs(self, stops):
        if not self.ordered:
            return [(a,b,self.between(a[0],b[0])) for a,b in zip(stops,stops[1:])]
        key = tuple(s[0] for s in stops)
        if key not in self.cache:
            positions, previous = [], -1
            for stop in stops:
                candidates = set(self.stops.get(stop[0], []))
                position = next((i for i in range(previous+1,len(self.ordered)) if self.ordered[i] in candidates), None)
                positions.append(position)
                if position is not None:
                    previous = position
            legs = []
            for start,end in zip(positions,positions[1:]):
                points = []
                if start is not None and end is not None:
                    for a,b in zip(self.ordered[start:end],self.ordered[start+1:end+1]):
                        segment = next((p for n,_,p in self.graph[a] if n==b), None)
                        if segment is None:
                            points=[]
                            break
                        points.extend(segment)
                legs.append(points or None)
            self.cache[key] = legs
        return [(a,b,p) for a,b,p in zip(stops,stops[1:],self.cache[key])]

    def between(self, start, end):
        key = (start, end)
        if key in self.cache:
            return self.cache[key]
        targets = set(self.stops.get(end, []))
        queue = [(0, n, []) for n in self.stops.get(start, [])]
        heapq.heapify(queue)
        visited = set()
        answer = None
        while queue:
            cost, n, points = heapq.heappop(queue)
            if n in visited:
                continue
            visited.add(n)
            if n in targets and points:
                answer = points
                break
            for other, length, segment in self.graph[n]:
                if other not in visited:
                    heapq.heappush(queue, (cost+length, other, points+segment))
        self.cache[key] = answer
        return answer


def animation(points, departure, arrival):
    lengths = [0]
    for a, b in zip(points, points[1:]):
        lengths.append(lengths[-1]+distance(a,b))
    total = lengths[-1]
    return [(departure+(arrival-departure)*(length/total if total else 0), p) for length,p in zip(lengths,points)]


def build(state, geometry, mode):
    # Application policy: replay the imported timetable every day, ignoring dates.
    feeds = state.get('original', {}).get('transitMapTimetable', {}).get('feeds', [])
    routes = {r['routeId']: r for r in geometry.get('routes' if mode=='styled' else 'geometryRoutes', [])}
    aliases = state.get('timelineRouteSources', state.get('original', {}).get('transitMapTimetable', {}).get('routeSources', {}))
    patterns = {(r['routeId'], p['directionId'], p.get('shapeId',''), tuple(s['stopId'] for s in p['orderedStops'])):p['patternId']
                for r in state.get('directionalData', {}).get('routes', []) for p in r['patterns']}
    decoded = [timetable(feed['data']) for feed in feeds]
    scheduled = 0
    paths, trains, intervals, seen = {}, [], [], set()
    for tables, times, frequencies in decoded:
        for previous in (True, False):
            day_offset = -86400 if previous else 0
            trips = ({**trip, 'route_id':rid} for trip in tables['trips']
                     for rid in routes if aliases.get(rid, rid)==trip['route_id'])
            for trip in trips:
                rid, tid = trip['route_id'], trip['trip_id']
                if rid not in routes:
                    continue
                stops = times.get(tid, [])
                if len(stops)<2:
                    continue
                shifts = [0]
                if frequencies.get(tid):
                    shifts = [t-stops[0][2] for row in frequencies[tid]
                              for t in range(seconds(row['start_time']), seconds(row['end_time']), int(row['headway_secs']))]
                shifts = [shift for shift in shifts if stops[-1][2]+shift+day_offset>0 and stops[0][1]+shift+day_offset<86400]
                if not shifts:
                    continue
                scheduled += len(shifts)
                direction = trip.get('direction_id', '')
                pattern = patterns.get((aliases.get(rid,rid),direction,trip.get('shape_id',''),tuple(s[0] for s in stops)))
                key = (rid, direction, pattern)
                if key not in paths:
                    paths[key] = RoutePaths(state, geometry, routes[rid], direction, pattern, mode=='styled')
                path = paths[key]
                # Cache station pairs across every trip; never rebuild route geometry per train.
                legs = path.legs(stops)
                for shift in shifts:
                    offset = shift+day_offset
                    identity = (rid, tid, offset)
                    if identity in seen:
                        continue
                    seen.add(identity)
                    frames = []
                    def finish():
                        if len(frames)<2 or frames[-1][0]<=frames[0][0] or frames[-1][0]<=0 or frames[0][0]>=86400:
                            return
                        first,last=frames[0][0],frames[-1][0]
                        # Browser only displays backend-computed linear keyframes.
                        trains.append({'routeId':rid,'tripId':tid,'direction':key[1], 'start':first,'end':last,
                            'values':';'.join(f'{p[0]:.3f} {p[1]:.3f}' for _,p in frames),
                            'keyTimes':';'.join(f'{(t-first)/(last-first):.9f}' for t,_ in frames)})
                        intervals.append([max(0,first),min(86400,last)])
                    for a,b,points in legs:
                        if not points or b[1]<a[2]:
                            finish();frames=[]
                            continue
                        departure, arrival = a[2]+offset,b[1]+offset
                        if frames and departure>frames[-1][0]:
                            frames.append((departure,frames[-1][1]))
                        elif not frames and a[1]<a[2]:
                            frames.append((a[1]+offset,points[0]))
                        for frame in animation(points,departure,arrival):
                            if not frames or frame[0]>frames[-1][0]:
                                frames.append(frame)
                            elif frame[0]==frames[-1][0]:
                                frames[-1]=frame
                    finish()
    merged=[]
    for a,b in sorted(intervals):
        if merged and a<=merged[-1][1]:
            merged[-1][1]=max(b,merged[-1][1])
        else:
            merged.append([a,b])
    motions, registry = [], {}
    for train in trains:
        motion = (train.pop('values'), train.pop('keyTimes'))
        if motion not in registry:
            registry[motion] = len(motions)
            motions.append({'values':motion[0], 'keyTimes':motion[1]})
        train['motion'] = registry[motion]
    return {'trains':trains,'motions':motions,'intervals':merged,'hasTimetable':bool(feeds),'scheduledTrips':scheduled}

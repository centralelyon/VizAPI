"""Convert GPX tracks/routes to geographic networks; samples are not stations."""
import math
from pathlib import Path
import xml.etree.ElementTree as ET


def convert_gpx(name, data):
    try:
        root = ET.fromstring(data)
    except ET.ParseError as exc:
        raise ValueError('Invalid GPX XML') from exc
    title = root.findtext('./{*}metadata/{*}name') or Path(name).stem
    waypoints = [(p.get('lon'), p.get('lat'), p.findtext('{*}name')) for p in root.findall('./{*}wpt')]
    features = []
    modes = {}
    parts = [(t.findtext('{*}name') or title, s.findall('{*}trkpt')) for t in root.findall('./{*}trk') for s in t.findall('{*}trkseg')]
    parts += [(r.findtext('{*}name') or title, r.findall('{*}rtept')) for r in root.findall('./{*}rte')]
    def coordinate(p):
        lon, lat = float(p.get('lon')), float(p.get('lat'))
        if not math.isfinite(lon+lat) or not -180 <= lon <= 180 or not -85 <= lat <= 85:
            raise ValueError('GPX coordinates are outside the supported geographic range')
        return [lon, lat]
    for part, (label, samples) in enumerate(parts):
        coords = []
        for sample in samples:
            p = coordinate(sample)
            if not coords or p != coords[-1]: coords.append(p)
        if len(coords) < 2: continue
        latitude = math.radians(sum(p[1] for p in coords)/len(coords))
        projected = [(p[0]*111320*math.cos(latitude), p[1]*111320) for p in coords]
        anchors = {0: label+' — start', len(coords)-1: label+' — end'}
        for lon, lat, station in waypoints:
            if not station: continue
            x, y = float(lon)*111320*math.cos(latitude), float(lat)*111320
            i = min(range(len(coords)), key=lambda j: (projected[j][0]-x)**2+(projected[j][1]-y)**2)
            if math.hypot(projected[i][0]-x, projected[i][1]-y) <= 150: anchors[i] = station
        rid = f'gpx:{part}'
        modes[rid] = 'hiking'
        indices = sorted(anchors)
        for i in indices:
            features.append({'type':'Feature','geometry':{'type':'Point','coordinates':coords[i]},
                             'properties':{'id':f'{rid}:{i}','station_id':f'{rid}:{i}','station_label':anchors[i]}})
        # Douglas–Peucker in metres, preserving every named waypoint anchor.
        for a, b in zip(indices, indices[1:]):
            keep = {a,b}; pending = [(a,b)]
            while pending:
                first,last = pending.pop()
                x,y = projected[first]; dx,dy = projected[last][0]-x,projected[last][1]-y
                length = dx*dx+dy*dy
                best,index = 10.0,None
                for j in range(first+1,last):
                    px,py = projected[j]; t = max(0,min(1,((px-x)*dx+(py-y)*dy)/length)) if length else 0
                    distance = math.hypot(px-x-t*dx,py-y-t*dy)
                    if distance > best: best,index = distance,j
                if index is not None: keep.add(index);pending.extend([(first,index),(index,last)])
            features.append({'type':'Feature','geometry':{'type':'LineString','coordinates':[coords[i] for i in sorted(keep)]},
                             'properties':{'id':f'{rid}:edge:{a}','from':f'{rid}:{a}','to':f'{rid}:{b}',
                                           'lines':[{'id':rid,'label':label,'color':'38804A'}]}})
    if not features: raise ValueError('GPX contains no track or route with at least two points')
    return {'type':'FeatureCollection','name':title,'networkName':title,'inputType':'hiking','routeModes':modes,'features':features}

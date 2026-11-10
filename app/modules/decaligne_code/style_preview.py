"""Style preview geometry, without loading or editing a network."""
import math


def preview(body):
    if body['kind'] != 'bend':
        value = float(body['value'])
        return {'value': max(0, value), 'lines': [50-value/2, 50+value/2]}
    incoming, outgoing = max(0, float(body['incoming'])), max(0, float(body['outgoing']))
    drag = body.get('drag')
    if drag:
        offset = round(max(0, drag['initial'] + drag['delta'] * drag['scale'] / drag['length']), 1)
        if drag['axis'] == 'x':
            incoming = offset
        else:
            outgoing = offset
    scale = max(100, incoming, outgoing)
    rad = math.radians(180-float(body['angle']))
    c, s = math.cos(rad), math.sin(rad)
    # Fit complete legs inside the unchanged preview, leaving space for handles.
    low_x, high_x = min(-1, c), max(0, c)
    length = min(248/(high_x-low_x), 148/s)
    ox, oy = 140-(low_x+high_x)*length/2, 90+s*length/2
    a, b = incoming/scale*length, outgoing/scale*length
    start, end = [ox-a, oy], [ox+b*c, oy-b*s]
    def half_clip(dx, dy, top):
        points = [(-16, -16 if top else 0), (16, -16 if top else 0), (16, 0 if top else 16), (-16, 0 if top else 16)]
        return ' '.join(f'{x*dx+y*dy},{-x*dy+y*dx}' for x, y in points)
    return {'path': f'M {ox-length} {oy} L {start[0]} {oy} Q {ox} {oy} {end[0]} {end[1]} L {ox+length*c} {oy-length*s}',
            'guide': f'M {ox-length} {oy} L {ox} {oy} L {ox+length*c} {oy-length*s}',
            'control': f'M {start[0]} {oy} L {ox} {oy} L {end[0]} {end[1]}',
            'x': start, 'y': end,
            'handles': {
                'x': {'matrix': [-1, 0, 0, -1, *start], 'min': -a, 'max': length-a, 'clip': half_clip(-1, 0, False)},
                'y': {'matrix': [c, -s, s, c, *end], 'min': -b, 'max': length-b, 'clip': half_clip(c, -s, True)},
            },
            'incoming': incoming, 'outgoing': outgoing, 'scale': scale, 'length': length}

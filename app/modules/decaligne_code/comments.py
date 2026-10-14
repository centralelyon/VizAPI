"""Map annotations, stored independently from topology undo checkpoints.

Bounds use the core's world coordinates so JSON round trips survive camera refits.
Only public responses contain SVG rectangles; filtering and indexing live here.
"""
import json
import math
import time
import uuid
from fastapi import HTTPException

SIDES = ('left', 'right', 'bottomLeft', 'bottomRight')


def rectangle(start, end):
    if len(start) != 2 or len(end) != 2 or any(type(v) not in (int, float) or not math.isfinite(v) for v in [*start, *end]):
        raise HTTPException(422, 'Two finite comment points are required')
    x, y = min(start[0], end[0]), min(start[1], end[1])
    right, bottom = max(start[0], end[0]), max(start[1], end[1])
    return dict(x=x, y=y, width=right-x, height=bottom-y, right=right)


def world_bounds(region, state):
    t = state['transform']
    return [t['x']+(region['x']-600)/t['scale'], t['y']+(400-region['y']-region['height'])/t['scale'],
            t['x']+(region['right']-600)/t['scale'], t['y']+(400-region['y'])/t['scale']]


def display(item, state):
    t = state['transform']
    x1, y1, x2, y2 = item['bounds']
    region = rectangle([(x1-t['x'])*t['scale']+600, 400-(y1-t['y'])*t['scale']],
                       [(x2-t['x'])*t['scale']+600, 400-(y2-t['y'])*t['scale']])
    return {**item, 'region': region}


def records(conn, sid, state):
    rows = conn.execute('SELECT data FROM map_comments WHERE session_id=? ORDER BY created, id', (sid,)).fetchall()
    items = []
    for index, row in enumerate(rows, 1):
        item = json.loads(row[0])
        legacy = 'bounds' not in item or 'index' not in item or 'resolved' not in item
        # Read annotations made by the previous version without losing them.
        if 'bounds' not in item:
            item['bounds'] = world_bounds(item.pop('region'), state)
        item.setdefault('index', index)
        item.setdefault('resolved', False)
        if legacy:
            conn.execute('UPDATE map_comments SET data=? WHERE session_id=? AND id=?', (json.dumps(item), sid, item['id']))
        items.append(item)
    return items


def exported(conn, sid, state):
    counter = conn.execute('SELECT next_index FROM map_comment_counters WHERE session_id=?', (sid,)).fetchone()
    return {'version': 1, 'coordinateSystem': 'map-world', 'items': records(conn, sid, state), 'nextIndex': counter[0] if counter else 1}


def seed(conn, sid, data):
    if data is None:
        return
    if data.get('version') != 1 or data.get('coordinateSystem') != 'map-world':
        raise HTTPException(422, 'Unsupported comment JSON format')
    used = set()
    indices = set()
    for item in data.get('items', []):
        bounds = item.get('bounds', [])
        if len(bounds) != 4 or any(type(v) not in (float, int) or not math.isfinite(v) for v in bounds) or bounds[0] >= bounds[2] or bounds[1] >= bounds[3]:
            raise HTTPException(422, 'Invalid comment bounds')
        index = item.get('index')
        if item.get('side') not in SIDES or not isinstance(item.get('text'), str) or not item['text'].strip() or type(index) is not int or index < 1 or index in indices:
            raise HTTPException(422, 'Invalid comment metadata')
        key = item.get('id') or uuid.uuid4().hex
        if key in used:
            raise HTTPException(422, 'Duplicate comment ID')
        used.add(key); indices.add(index)
        saved = dict(id=key, index=index, side=item['side'], text=item['text'], resolved=bool(item.get('resolved')), bounds=bounds)
        conn.execute('INSERT INTO map_comments VALUES (?, ?, ?, ?)', (key, sid, time.time(), json.dumps(saved)))

    next_index = max(max(indices, default=0)+1, data.get('nextIndex', 1))
    conn.execute('INSERT OR REPLACE INTO map_comment_counters VALUES (?, ?)', (sid, next_index))


def operate(sessions, body):
    action = body.get('action', 'list')
    sid = body.get('sessionId')
    with sessions.connect() as conn:
        # One transaction covers index allocation and mutation, including multiple workers.
        if action != 'list':
            conn.execute('BEGIN IMMEDIATE')
        row = conn.execute('SELECT state FROM edit_sessions WHERE id=?', (sid,)).fetchone()
        if row is None:
            raise HTTPException(404, 'Comment session does not exist')
        state = json.loads(row[0])
        items = records(conn, sid, state)
        if action == 'add':
            region = rectangle(body.get('start', []), body.get('end', []))
            text, side = body.get('text', '').strip(), body.get('side')
            if not text or side not in SIDES or not region['width'] or not region['height']:
                raise HTTPException(422, 'Comment text, window and a non-empty region are required')
            counter = conn.execute('SELECT next_index FROM map_comment_counters WHERE session_id=?', (sid,)).fetchone()
            index = max(max((c['index'] for c in items), default=0)+1, counter[0] if counter else 1)
            saved = dict(id=uuid.uuid4().hex, index=index,
                         side=side, text=text, resolved=False, bounds=world_bounds(region, state))
            conn.execute('INSERT INTO map_comments VALUES (?, ?, ?, ?)', (saved['id'], sid, time.time(), json.dumps(saved)))
            # Keep numbering monotonic after deletions, without changing edit revision.
            conn.execute('INSERT OR REPLACE INTO map_comment_counters VALUES (?, ?)', (sid, index+1))
        elif action in ('delete', 'resolve'):
            saved = next((c for c in items if c['id'] == body.get('id')), None)
            if saved is None:
                raise HTTPException(404, 'Comment does not exist')
            if action == 'delete':
                conn.execute('DELETE FROM map_comments WHERE session_id=? AND id=?', (sid, saved['id']))
            else:
                saved['resolved'] = bool(body.get('resolved'))
                conn.execute('UPDATE map_comments SET data=? WHERE session_id=? AND id=?', (json.dumps(saved), sid, saved['id']))
        elif action != 'list':
            raise HTTPException(422, 'Unknown comment action')
        items = records(conn, sid, state)
        query, side = body.get('query', '').strip().casefold(), body.get('sideFilter', 'all')
        visible = [c['id'] for c in items if (side == 'all' or c['side'] == side) and (not query or query in c['text'].casefold())]
        return {'comments': [display(c, state) for c in items], 'visibleIds': visible}

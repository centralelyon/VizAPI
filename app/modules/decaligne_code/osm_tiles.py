"""Visible Web Mercator tiles, calculated in server-owned map coordinates."""
import math


def layout(body):
    view, data = body['view'], body['map']
    size = body.get('size', {'width':1200,'height':800})
    pixels = min(size['width']/view['w'],size['height']/view['h'])
    width,height = size['width']/pixels,size['height']/pixels
    left,top = view['x']+(view['w']-width)/2,view['y']+(view['h']-height)/2
    visible = body.get('visibleRange') or [0,1,0,1]
    x0,x1 = left+width*visible[0],left+width*visible[1]
    y0,y1 = top+height*(visible[2] if len(visible)>2 else 0),top+height*(visible[3] if len(visible)>3 else 1)
    zoom = max(0,min(19,int(math.floor(math.log2(data['scale']*pixels/256)+.5))))
    count=2**zoom; tile_size=data['scale']/count
    tx=lambda x:(data['origin'][0]+x/data['scale'])*count
    ty=lambda y:(data['origin'][1]+y/data['scale'])*count
    tiles=[]
    for x in range(math.floor(tx(x0))-1,math.ceil(tx(x1))+1):
        for y in range(max(0,math.floor(ty(y0))-1),min(count,math.ceil(ty(y1))+1)):
            tiles.append({'id':f'{zoom}/{x}/{y}','href':f'https://tile.openstreetmap.org/{zoom}/{x%count}/{y}.png',
                          'x':(x/count-data['origin'][0])*data['scale'],'y':(y/count-data['origin'][1])*data['scale'],
                          'width':tile_size,'height':tile_size})
    return {'tiles':tiles,'background':{'x':left,'y':top,'width':width,'height':height},
            'attribution':{'x':x0+8/pixels,'y':y0+20/pixels,'scale':1/pixels}}

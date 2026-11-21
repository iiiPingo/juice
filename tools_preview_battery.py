from PIL import Image, ImageDraw
import numpy as np
SS=8
def X(b,u): return b[0]+u*b[2]
def Y(b,v): return b[1]+v*b[2]
def L(b,d): return d*b[2]
def rr(d,b,x0,y0,x1,y1,r):
    d.rounded_rectangle([X(b,x0),Y(b,y0),X(b,x1),Y(b,y1)], radius=min(L(b,r),(X(b,x1)-X(b,x0))/2,(Y(b,y1)-Y(b,y0))/2), fill=255)
def el(d,b,cx,cy,rx,ry): d.ellipse([X(b,cx-rx),Y(b,cy-ry),X(b,cx+rx),Y(b,cy+ry)], fill=255)
def glyph(kind,b,body,holes):
    if kind=='mouse':
        rr(body,b,.20,.03,.80,.97,.30); rr(holes,b,.465,0,.535,.40,0); rr(holes,b,.10,.40,.90,.47,0)
    elif kind=='pad':
        rr(body,b,.03,.22,.97,.64,.18); el(body,b,.23,.64,.19,.25); el(body,b,.77,.64,.19,.25)
        rr(holes,b,.14,.40,.36,.47,0); rr(holes,b,.215,.325,.285,.545,0); el(holes,b,.68,.37,.055,.055); el(holes,b,.78,.48,.055,.055)
    elif kind in('phones','headset'):
        mic=kind=='headset'; bw=.10; top=.06+bw/2
        x0=X(b,.10+bw/2); y0=Y(b,top); w=L(b,.80-bw); h=L(b,.95-top)
        body.arc([x0-L(b,bw)/2,y0-L(b,bw)/2,x0+w+L(b,bw)/2,y0+h+L(b,bw)/2],180,360,fill=255,width=int(L(b,bw)))
        cb=.82 if mic else .94
        rr(body,b,.04,.46,.30,cb,.09); rr(body,b,.70,.46,.96,cb,.09)
        if mic:
            P=[(.17,.80),(.18,.95),(.30,.95),(.46,.94)]; pts=[]
            for i in range(21):
                t=i/20; q=[(1-t)**3*P[0][k]+3*(1-t)**2*t*P[1][k]+3*(1-t)*t*t*P[2][k]+t**3*P[3][k] for k in (0,1)]
                pts.append((X(b,q[0]),Y(b,q[1])))
            body.line(pts,fill=255,width=int(L(b,.075)),joint='curve'); r=L(b,.075)/2
            for q in (pts[0],pts[-1]): body.ellipse([q[0]-r,q[1]-r,q[0]+r,q[1]+r],fill=255)
    elif kind=='buds':
        el(body,b,.29,.25,.19,.19); rr(body,b,.33,.24,.47,.95,.07); el(body,b,.71,.25,.19,.19); rr(body,b,.53,.24,.67,.95,.07)
        el(holes,b,.24,.22,.055,.055); el(holes,b,.76,.22,.055,.055)
def icon(kind,pct,fg,fill,s):
    img=Image.new('RGBA',(s,s),(0,0,0,0)); px=img.load()
    t=2 if s>=32 else 1; bw=max(6,round(s*.33))
    if (bw-2*t)%2: bw+=1
    bx=s-bw; nubH=max(1,round(s*.08)); by=max(nubH,round(s*.14)); nubW=max(2,(bw+1)//2)
    if (bw-nubW)%2: nubW+=1
    def R(x0,y0,x1,y1,c):
        for y in range(y0,y1):
            for x in range(x0,x1): px[x,y]=c+(255,)
    R(bx+(bw-nubW)//2,by-nubH,bx+(bw-nubW)//2+nubW,by,fg); R(bx,by,bx+bw,by+t,fg); R(bx,s-t,bx+bw,s,fg); R(bx,by,bx+t,s,fg); R(bx+bw-t,by,bx+bw,s,fg)
    gap=1 if s>=20 else 0
    ix0,ix1,iy0,iy1=bx+t+gap,bx+bw-t-gap,by+t+gap,s-t-gap
    fh=round((iy1-iy0)*pct/100)
    if pct>0 and fh==0: fh=1
    R(ix0,iy1-fh,ix1,iy1,fill)
    area=bx-max(1,s//16); side=min(area,s); gy=(s-side)//2; gx=(area-side)//2
    B=Image.new('L',(s*SS,s*SS),0); H=Image.new('L',(s*SS,s*SS),0)
    glyph(kind,(gx*SS,gy*SS,side*SS),ImageDraw.Draw(B),ImageDraw.Draw(H))
    B=np.array(B.resize((s,s),Image.LANCZOS)).astype(int); H=np.array(H.resize((s,s),Image.LANCZOS)).astype(int)
    g=Image.new('RGBA',(s,s),fg+(0,)); g.putalpha(Image.fromarray((B*(255-H)//255).astype('uint8')))
    return Image.alpha_composite(img,g)
rows=[('buds',70,'normal'),('phones',55,'normal'),('headset',35,'warn'),('mouse',15,'low'),('pad',80,'chg')]
cols={'dark':{'bg':(32,32,32),'fg':(255,255,255),'normal':(255,255,255),'warn':(255,193,59),'low':(255,99,90),'chg':(108,216,108)},
      'light':{'bg':(238,238,238),'fg':(26,26,26),'normal':(26,26,26),'warn':(179,107,0),'low':(196,43,28),'chg':(15,123,15)}}
sizes=[16,20,24,32]; SC=5
W=sum(s*SC+16 for s in sizes)*2+40; Hh=len(rows)*(32*SC+16)+30
out=Image.new('RGB',(W,Hh),(80,80,80)); x=16
for theme in ('dark','light'):
    c=cols[theme]
    for s in sizes:
        for i,(k,p,tone) in enumerate(rows):
            tile=Image.alpha_composite(Image.new('RGBA',(s,s),c['bg']+(255,)),icon(k,p,c['fg'],c[tone],s)).resize((s*SC,s*SC),Image.NEAREST)
            out.paste(tile.convert('RGB'),(x,16+i*(32*SC+16)))
        x+=s*SC+16
    x+=20
out.save('/tmp/icons_preview.png')
# realistic 1:1 taskbar strip at 24px (150% scaling) dark
strip=Image.new('RGB',(5*30+20,40),(32,32,32))
for i,(k,p,tone) in enumerate(rows):
    ic=icon(k,p,cols['dark']['fg'],cols['dark'][tone],24); strip.paste(ic,(10+i*30,8),ic)
strip.resize((strip.width*3,strip.height*3),Image.NEAREST).save('/tmp/strip.png')

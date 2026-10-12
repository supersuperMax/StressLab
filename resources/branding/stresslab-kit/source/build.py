import math, os, io
import cairosvg
from PIL import Image
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen

INK='#1B1F24'; HOT='#FF5A36'; PAPER='#F4F5F7'
OUT='out'; f2=lambda v: f'{v:.2f}'.rstrip('0').rstrip('.')
def svg(w,h,title,body): return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {f2(w)} {f2(h)}" width="{f2(w)}" height="{f2(h)}" role="img" aria-labelledby="title">'
                                f'<title id="title">StressLab {title}</title>\n{body}\n</svg>\n')
def write(path,s):
    os.makedirs(os.path.dirname(path),exist_ok=True); open(path,'w').write(s)
def png(svgstr,size,path=None,h=None):
    data=cairosvg.svg2png(bytestring=svgstr.encode(),output_width=size,output_height=h or size if h is None else h)
    if path: os.makedirs(os.path.dirname(path),exist_ok=True); open(path,'wb').write(data)
    return data

# ---- symbol geometry. One tile = 'quarter model' of a plate with a central hole.
# Grid 256: tiles `size`, gaps `gap`, hole radius r centred at (128,128), outer corner radius R.
# Hot tile (top-right) lifted outward by `lift` along the diagonal; whole mark shifted by -lift/2,+lift/2 to stay centred.
def tile(qx,qy,lift,size,gap,r,R,sh):
    c=128; g=gap/2; ox=c-g-size; ix=c-g; t=math.sqrt(r*r-g*g)
    X=lambda v: c+qx*(v-c)-qx*lift+sh[0]; Y=lambda v: c+qy*(v-c)-qy*lift+sh[1]
    sw=lambda s: 1-s if qx*qy<0 else s
    return (f'M {f2(X(ox+R))} {f2(Y(ox))} H {f2(X(ix))} V {f2(Y(c-t))} A {r} {r} 0 0 {sw(0)} {f2(X(c-t))} {f2(Y(ix))} '
            f'H {f2(X(ox))} V {f2(Y(ox+R))} A {R} {R} 0 0 {sw(1)} {f2(X(ox+R))} {f2(Y(ox))} Z')
CUTS={  # regular, small-size cut (<=32px), reversed (thinner light shapes = wider gaps)
 'regular': dict(size=98,gap=12,r=50,R=18,lift=8),
 'small':   dict(size=92,gap=26,r=58,R=20,lift=14),
 'reversed':dict(size=97,gap=14,r=51,R=18,lift=8),
}
def symbol_paths(base,hot,cut='regular'):
    p=CUTS[cut]; sh=(-p['lift']/2,p['lift']/2); a=dict(size=p['size'],gap=p['gap'],r=p['r'],R=p['R'],sh=sh)
    d=' '.join(tile(qx,qy,0,**a) for qx,qy in [(1,1),(1,-1),(-1,-1)])
    return f'<path fill="{base}" d="{d}"/>\n<path fill="{hot}" d="{tile(-1,1,p["lift"],**a)}"/>'

# ---- wordmark: Inter Display SemiBold "Stress" + Regular "Lab" (OFL), outlined
FD='/usr/share/fonts/opentype/inter/'
def word(parts,x0,baseline,cap,track=-0.012):
    x=x0; out=[]
    for txt,fn,col in parts:
        ft=TTFont(FD+fn); gs=ft.getGlyphSet(); cm=ft.getBestCmap(); k=cap/ft['OS/2'].sCapHeight; upm=ft['head'].unitsPerEm; ds=[]
        for ch in txt:
            pen=SVGPathPen(gs); gs[cm[ord(ch)]].draw(TransformPen(pen,(k,0,0,-k,x,baseline))); ds.append(pen.getCommands())
            x+=gs[cm[ord(ch)]].width*k+track*upm*k
        out.append(f'<path fill="{col}" d="{" ".join(ds)}"/>')
    return '\n'.join(out), x
CAP=92
def wordmark(c1,c2,x0=0,baseline=0):
    return word([('Stress','InterDisplay-SemiBold.otf',c1),('Lab','InterDisplay-Regular.otf',c2)],x0,baseline,CAP)

SCHEMES={ # name: (tile colour, hot colour, wordmark Stress, wordmark Lab, cut, background for preview)
 'color':    (INK,HOT,INK,INK,'regular'),
 'reversed': (PAPER,HOT,'#FFFFFF','#FFFFFF','reversed'),
 'black':    ('#000000','#000000','#000000','#000000','regular'),
 'white':    ('#FFFFFF','#FFFFFF','#FFFFFF','#FFFFFF','reversed'),
}
def build_logos():
    for nm,(b,h,w1,w2,cut) in SCHEMES.items():
        sym=symbol_paths(b,h,cut)
        write(f'{OUT}/logo/svg/stresslab-symbol-{nm}.svg',svg(256,256,f'symbol ({nm})',sym))
        # symbol occupies 20..236 -> crop: horizontal lockup height 256, wordmark cap centred on symbol
        wm,xe=wordmark(w1,w2,x0=256+40,baseline=128+CAP/2)
        W=xe+20
        write(f'{OUT}/logo/svg/stresslab-horizontal-{nm}.svg',svg(W,256,f'horizontal logo ({nm})',f'<g id="symbol">{sym}</g>\n<g id="wordmark">{wm}</g>'))
        # stacked: symbol on top, wordmark centred below
        wm0,xw=wordmark(w1,w2,0,0); ww=xw
        H=256+56+CAP+24; Wst=max(256,ww)+40
        wm2,_=wordmark(w1,w2,x0=(Wst-ww)/2,baseline=256+56+CAP-20)
        write(f'{OUT}/logo/svg/stresslab-stacked-{nm}.svg',svg(Wst,H-20,f'stacked logo ({nm})',
              f'<g id="symbol" transform="translate({f2((Wst-256)/2)} 0)">{sym}</g>\n<g id="wordmark">{wm2}</g>'))
        wm3,xe3=wordmark(w1,w2,x0=20,baseline=128+CAP/2)
        write(f'{OUT}/logo/svg/stresslab-wordmark-{nm}.svg',svg(xe3+20,256,f'wordmark ({nm})',wm3))
    write(f'{OUT}/logo/svg/stresslab-symbol-small-color.svg',svg(256,256,'symbol small-size cut',symbol_paths(INK,HOT,'small')))

# ---- app icons
def tile_icon(canvas, tile_box, radius, cut, squircle=False, fill=0.64):
    x,y,s=tile_box
    if squircle:  # superellipse-like continuous corners (n=5) for macOS
        n=5; pts=[]
        for i in range(256):
            t=2*math.pi*i/256; ct,st=math.cos(t),math.sin(t)
            pts.append((x+s/2+s/2*math.copysign(abs(ct)**(2/n),ct), y+s/2+s/2*math.copysign(abs(st)**(2/n),st)))
        bg=f'<path fill="{INK}" d="M '+' L '.join(f'{px:.2f} {py:.2f}' for px,py in pts)+' Z"/>'
    else:
        bg=f'<rect x="{f2(x)}" y="{f2(y)}" width="{f2(s)}" height="{f2(s)}" rx="{f2(radius)}" fill="{INK}"/>'
    k=s*0.64/256*256/216  # visible mark (216 of 256) fills ~64% of the tile
    k=s*fill/216
    off_x=x+s/2-128*k; off_y=y+s/2-128*k - s*0.01  # optical nudge up 1%
    return svg(canvas,canvas,'app icon',bg+f'\n<g transform="translate({f2(off_x)} {f2(off_y)}) scale({k:.5f})">{symbol_paths(PAPER,HOT,cut)}</g>')
def build_icons():
    # macOS: Apple grid on 1024 canvas, 824 body, 100 margin, continuous corners
    mac=lambda cut: tile_icon(1024,(100,100,824),185,cut,squircle=True,fill=0.72 if cut=='small' else 0.64)
    write(f'{OUT}/icons/macos/stresslab-macos.svg',mac('regular'))
    iconset=f'{OUT}/icons/macos/StressLab.iconset'
    imgs={}
    for sz in [16,32,64,128,256,512,1024]:
        imgs[sz]=Image.open(io.BytesIO(png(mac('small' if sz<=32 else 'reversed'),sz)))
    for base in [16,32,128,256,512]:
        imgs[base].save(f'{iconset}/icon_{base}x{base}.png') if os.makedirs(iconset,exist_ok=True) is None else None
        imgs[base*2].save(f'{iconset}/icon_{base}x{base}@2x.png')
    imgs[1024].save(f'{OUT}/icons/macos/StressLab.icns',format='ICNS',append_images=[imgs[s] for s in [16,32,64,128,256,512]])
    # Windows + Linux: rounded square nearly full-bleed (Win11/Fluent & freedesktop style)
    std=lambda cut: tile_icon(256,(8,8,240),48,cut,fill=0.74 if cut=='small' else 0.64)
    write(f'{OUT}/icons/windows/stresslab-windows.svg',std('reversed'))
    win=[Image.open(io.BytesIO(png(std('small' if s<=32 else 'reversed'),s))) for s in [16,20,24,32,40,48,64,96,128,256]]
    win[-1].save(f'{OUT}/icons/windows/StressLab.ico',format='ICO',sizes=[(i.width,i.height) for i in win],append_images=win[:-1])
    for s,i in zip([16,20,24,32,40,48,64,96,128,256],win): i.save(f'{OUT}/icons/windows/png/stresslab-{s}.png') if os.makedirs(f'{OUT}/icons/windows/png',exist_ok=True) is None else None
    # Linux hicolor theme
    for s in [16,22,24,32,48,64,128,256,512]:
        png(std('small' if s<=32 else 'reversed'),s,f'{OUT}/icons/linux/hicolor/{s}x{s}/apps/stresslab.png')
    write(f'{OUT}/icons/linux/hicolor/scalable/apps/stresslab.svg',std('reversed'))
    write(f'{OUT}/icons/linux/hicolor/symbolic/apps/stresslab-symbolic.svg',svg(256,256,'symbolic',symbol_paths('#000000','#000000','small')).replace('viewBox="0 0 256 256" width="256" height="256"','viewBox="0 0 256 256" width="16" height="16"'))
    write(f'{OUT}/icons/linux/stresslab.desktop','[Desktop Entry]\nType=Application\nName=StressLab\nComment=Finite element analysis\nExec=stresslab %F\nIcon=stresslab\nCategories=Science;Engineering;Education;\nTerminal=false\n')
    return imgs

def build_web():
    std=lambda cut: tile_icon(256,(8,8,240),48,cut,fill=0.74 if cut=='small' else 0.64)
    full=lambda fill: tile_icon(256,(0,0,256),0,'reversed',fill=fill)  # full-bleed for iOS / maskable
    d=f'{OUT}/web'
    write(f'{d}/favicon.svg',std('small'))
    ims=[]
    for s in (16,32,48):
        png(std('small' if s<=32 else 'reversed'),s,f'{d}/favicon-{s}.png'); ims.append(Image.open(f'{d}/favicon-{s}.png'))
    ims[-1].save(f'{d}/favicon.ico',format='ICO',sizes=[(16,16),(32,32),(48,48)],append_images=ims[:-1])
    png(full(0.62),180,f'{d}/apple-touch-icon.png')
    png(std('reversed'),192,f'{d}/icon-192.png'); png(std('reversed'),512,f'{d}/icon-512.png')
    png(full(0.52),512,f'{d}/maskable-512.png')
    for f in ('stresslab-favicon.svg','stresslab-app-icon.svg'):
        p=f'{d}/{f}'; os.path.exists(p) and os.remove(p)

if __name__=='__main__':
    build_logos(); build_icons(); build_web(); print('ok')

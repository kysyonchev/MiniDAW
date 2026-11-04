import numpy as np, math
import PIL
S=4096; K=S/1024
def sq(n=5.0, size=824):
    pts=[]; r=size/2*K; c=S/2
    for i in range(720):
        t=2*math.pi*i/720; ct,st=math.cos(t),math.sin(t)
        pts.append((c+r*np.sign(ct)*abs(ct)**(2/n), c+r*np.sign(st)*abs(st)**(2/n)))
    return pts
mask=PIL.Image.new("L",(S,S),0); PIL.ImageDraw.Draw(mask).polygon(sq(),fill=255)
# background gradient
y=np.linspace(0,1,S)[:,None]; x=np.linspace(0,1,S)[None,:]
top=np.array([34,37,56]); bot=np.array([10,11,18])
g=(top[None,None,:]*(1-y[...,None])+bot[None,None,:]*y[...,None])*np.ones((S,S,1))
# soft radial lift behind the M
d=np.sqrt((x-.5)**2+(y-.46)**2); g+= (np.clip(1-d/0.45,0,1)**2)[...,None]*np.array([20,18,30])
bg=PIL.Image.fromarray(np.clip(g,0,255).astype("uint8")).convert("RGBA")
bg.putalpha(mask)
canvas=PIL.Image.new("RGBA",(S,S),(0,0,0,0))
# drop shadow
sh=PIL.Image.new("RGBA",(S,S),(0,0,0,0)); sa=PIL.Image.new("L",(S,S),0)
m2=mask.copy(); sa.paste(m2.point(lambda v:int(v*0.55)),(0,int(10*K)))
sh.putalpha(sa.filter(PIL.ImageFilter.GaussianBlur(14*K)))
canvas=PIL.Image.alpha_composite(canvas,sh); canvas=PIL.Image.alpha_composite(canvas,bg)
# the M: one continuous stroke
P=[(300,700),(300,330),(512,590),(724,330),(724,700)]
def P_(p): return (p[0]*K,p[1]*K)
layer=PIL.Image.new("RGBA",(S,S),(0,0,0,0)); dl=PIL.ImageDraw.Draw(layer)
w=int(76*K); col=(244,241,234,255)
pts=[P_(p) for p in P]
dl.line(pts,fill=col,width=w,joint="curve")
for p in (pts[0],pts[-1],pts[1],pts[3]):
    r=w/2; dl.ellipse((p[0]-r,p[1]-r,p[0]+r,p[1]+r),fill=col)
canvas=PIL.Image.alpha_composite(canvas,layer)
# record dot sitting in the valley
cx,cy=512*K,372*K; r=58*K
glow=PIL.Image.new("RGBA",(S,S),(0,0,0,0)); PIL.ImageDraw.Draw(glow).ellipse((cx-r*1.5,cy-r*1.5,cx+r*1.5,cy+r*1.5),fill=(255,90,60,150))
canvas=PIL.Image.alpha_composite(canvas,glow.filter(PIL.ImageFilter.GaussianBlur(40*K)))
dot=PIL.Image.new("RGBA",(S,S),(0,0,0,0)); PIL.ImageDraw.Draw(dot).ellipse((cx-r,cy-r,cx+r,cy+r),fill=(255,88,58,255))
canvas=PIL.Image.alpha_composite(canvas,dot)
icon=canvas.resize((1024,1024),PIL.Image.LANCZOS); icon.save("icon_1024.png")
icon.resize((512,512),PIL.Image.LANCZOS).save("icon_512.png")
# iconset + icns
import os; os.makedirs("MiniDAW.iconset",exist_ok=True)
for s in (16,32,128,256,512):
    icon.resize((s,s),PIL.Image.LANCZOS).save(f"MiniDAW.iconset/icon_{s}x{s}.png")
    icon.resize((s*2,s*2),PIL.Image.LANCZOS).save(f"MiniDAW.iconset/icon_{s}x{s}@2x.png")
icon.save("MiniDAW.icns",sizes=[(16,16),(32,32),(64,64),(128,128),(256,256),(512,512),(1024,1024)])
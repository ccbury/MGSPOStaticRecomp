// Shared GPU compute rasterizer. Each dispatch owns disjoint pixels of one primitive;
// ordered dispatches preserve packed framebuffer, stencil, blending and depth semantics.
struct Vertex { float4 position; float4 colour; float4 uvFog; float4 spec; };
cbuffer Draw : register(b0) {
  uint4 registers[64];
  uint4 sizes;
  int4 clip;
  uint4 features;
  uint4 modes;
  Vertex vertices[3];
  uint4 bounds;
};
RWStructuredBuffer<uint> Colour : register(u0);
RWStructuredBuffer<uint> Depth : register(u1);
StructuredBuffer<uint> Texels : register(t0);
uint reg(uint op) { return registers[op / 4][op % 4]; }
bool enabled(uint bit) { return (features.x & bit) != 0; }
uint4 unpack(uint p) {
  if (sizes.z == 0) return uint4((p&31)*255/31,((p>>5)&63)*255/63,((p>>11)&31)*255/31,255);
  if (sizes.z == 1) return uint4((p&31)*255/31,((p>>5)&31)*255/31,((p>>10)&31)*255/31,(p&32768)?255:0);
  if (sizes.z == 2) return uint4(p&15,(p>>4)&15,(p>>8)&15,(p>>12)&15)*17;
  return uint4(p&255,(p>>8)&255,(p>>16)&255,p>>24);
}
uint pack(uint4 c) {
  if (sizes.z == 0) return (c.r>>3)|((c.g>>2)<<5)|((c.b>>3)<<11);
  if (sizes.z == 1) return (c.r>>3)|((c.g>>3)<<5)|((c.b>>3)<<10)|((c.a>>7)<<15);
  if (sizes.z == 2) return (c.r>>4)|((c.g>>4)<<4)|((c.b>>4)<<8)|((c.a>>4)<<12);
  return c.r|(c.g<<8)|(c.b<<16)|(c.a<<24);
}
bool compare(uint op, uint a, uint b) {
  switch(op) {
    case 0:return false; case 1:return true; case 2:return a==b; case 3:return a!=b;
    case 4:return a<b; case 5:return a<=b; case 6:return a>b; default:return a>=b;
  }
}
float edge(float2 a, float2 b, float2 p) { return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x); }
bool topLeft(float2 a,float2 b) { return b.y<a.y || (b.y==a.y && b.x>a.x); }
int4 bytes(uint p) { return int4(p&255,(p>>8)&255,(p>>16)&255,p>>24); }
uint texel(int2 p) {
  p.x=(modes.z&1)?clamp(p.x,0,(int)modes.x-1):(p.x&((int)modes.x-1));
  p.y=(modes.z&2)?clamp(p.y,0,(int)modes.y-1):(p.y&((int)modes.y-1));
  return Texels[p.y*modes.x+p.x];
}
int4 sampleTexture(float2 uv) {
  float2 p=uv*float2(modes.xy);
  if (!(modes.z&4)) return bytes(texel((int2)floor(p)));
  p-=0.5;
  int2 base=(int2)floor(p); float2 f=p-floor(p);
  return (int4)(lerp(lerp((float4)bytes(texel(base)),(float4)bytes(texel(base+int2(1,0))),f.x),
                     lerp((float4)bytes(texel(base+int2(0,1))),(float4)bytes(texel(base+1)),f.x),f.y)+0.5);
}
int4 combine(int4 c,int4 t) {
  bool alpha=(modes.z&8)!=0;
  if(!alpha)t.a=255;
  int3 env=bytes(reg(0xCA)).rgb;
  switch(reg(0xC9)&7) {
    case 0: c.rgb=c.rgb*t.rgb/255; if(alpha)c.a=c.a*t.a/255; break;
    case 1: c.rgb=alpha?(t.rgb*t.a+c.rgb*(255-t.a))/255:t.rgb; break;
    case 2: c.rgb=(c.rgb*(255-t.rgb)+env*t.rgb)/255; if(alpha)c.a=c.a*t.a/255; break;
    case 3: c.rgb=t.rgb; if(alpha)c.a=t.a; break;
    default:c.rgb+=t.rgb; if(alpha)c.a=c.a*t.a/255; break;
  }
  if(modes.z&16)c.rgb*=2;
  return clamp(c,0,255);
}
uint stencilOp(uint op,uint value,uint ref) {
  switch(op) {
    case 1:return 0;case 2:return ref;case 3:return (~value)&255;
    case 4:return min(255,value+1);case 5:return value?value-1:0;default:return value;
  }
}
int factor(uint f,bool src,uint ch,int4 s,int4 d) {
  switch(f) {
    case 0:return src?d[ch]:s[ch];case 1:return 255-(src?d[ch]:s[ch]);
    case 2:return s.a;case 3:return 255-s.a;case 4:return d.a;case 5:return 255-d.a;
    case 6:return min(255,s.a*2);case 7:return 255-min(255,s.a*2);
    case 8:return min(255,d.a*2);case 9:return 255-min(255,d.a*2);
    default:return ch==3?s.a:((reg(src?0xE0:0xE1)>>(ch*8))&255);
  }
}
uint logicOp(uint op,uint s,uint d) {
  switch(op) {
    case 0:return 0;case 1:return s&d;case 2:return s&~d;case 3:return s;
    case 4:return ~s&d;case 5:return d;case 6:return s^d;case 7:return s|d;
    case 8:return ~(s|d);case 9:return ~(s^d);case 10:return ~d;case 11:return s|~d;
    case 12:return ~s;case 13:return ~s|d;case 14:return ~(s&d);default:return ~0u;
  }
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
  if(id.x>=bounds.z || id.y>=bounds.w)return;
  uint2 pixel=bounds.xy+id.xy;
  if(any((int2)pixel<clip.xy)||any((int2)pixel>clip.zw))return;
  Vertex a=vertices[0], b=vertices[1], c=vertices[2];
  float2 p=(float2)pixel+0.5;
  float z,fog; float4 col; float3 spec; float2 uv;
  if(modes.w==1) {
    float2 low=min(a.position.xy,b.position.xy),high=max(a.position.xy,b.position.xy);
    if(any(p<low)||any(p>=high))return;
    float2 t=(p-a.position.xy)/(b.position.xy-a.position.xy);
    uv=float2(lerp(a.uvFog.x,b.uvFog.x,t.x),lerp(a.uvFog.y,b.uvFog.y,t.y));
    z=b.position.z;col=b.colour;spec=b.spec.xyz;fog=b.uvFog.z;
  } else {
    float area=edge(a.position.xy,b.position.xy,c.position.xy);
    if(area<=0)return;
    float3 w=float3(edge(b.position.xy,c.position.xy,p),edge(c.position.xy,a.position.xy,p),edge(a.position.xy,b.position.xy,p));
    if(any(w<0)|| (w.x==0&&!topLeft(b.position.xy,c.position.xy))||
       (w.y==0&&!topLeft(c.position.xy,a.position.xy))||(w.z==0&&!topLeft(a.position.xy,b.position.xy)))return;
    w/=area;
    z=dot(w,float3(a.position.z,b.position.z,c.position.z));
    col=a.colour*w.x+b.colour*w.y+c.colour*w.z;
    spec=a.spec.xyz*w.x+b.spec.xyz*w.y+c.spec.xyz*w.z;
    fog=dot(w,float3(a.uvFog.z,b.uvFog.z,c.uvFog.z));
    float3 pw=w*float3(a.position.w,b.position.w,c.position.w);
    float sum=pw.x+pw.y+pw.z;
    uv=sum!=0?(a.uvFog.xy*pw.x+b.uvFog.xy*pw.y+c.uvFog.xy*pw.z)/sum:float2(0,0);
  }
  if(enabled(8192)&&(z<(reg(0xD6)&65535)||z>(reg(0xD7)&65535)))return;
  z=clamp(z,0,65535);
  uint zi=(uint)(z+0.5), index=pixel.y*sizes.x+pixel.x;
  uint di=(enabled(64)||enabled(4096))?pixel.y*sizes.y+pixel.x:0;
  uint old=Colour[index],keep=sizes.w; int4 rgba=clamp((int4)(col*255+0.5),0,255);
  if(enabled(512)) {
    uint mask=(enabled(1024)?0:pack(uint4(255,255,255,0)))|(enabled(2048)?0:pack(uint4(0,0,0,255)));
    if(enabled(1024)||enabled(2048))Colour[index]=(old&mask)|(pack(rgba)&~mask);
    if(enabled(4096))Depth[di]=zi;
    return;
  }
  if(enabled(1))rgba=combine(rgba,sampleTexture(uv));
  if(enabled(4))rgba.rgb=clamp(rgba.rgb+(int3)(spec*255),0,255);
  if(enabled(2))rgba.rgb=(int3)(rgba.rgb*saturate(fog)+bytes(reg(0xCF)).rgb*(1-saturate(fog))+0.5);
  if(enabled(16)) {
    uint mask=reg(0xDA)&0xFFFFFF, value=(rgba.r|(rgba.g<<8)|(rgba.b<<16))&mask;
    if(!compare(reg(0xD8)&3,value,reg(0xD9)&mask))return;
  }
  if(enabled(8)&&!compare(reg(0xDB)&7,rgba.a&((reg(0xDB)>>16)&255),(reg(0xDB)>>8)&((reg(0xDB)>>16)&255)))return;
  int4 dst=(int4)unpack(old);
  uint ns=rgba.a;
  bool zpass=!enabled(64)||compare(reg(0xDE)&7,zi,Depth[di]);
  if(enabled(32)) {
    uint sv=sizes.z==0?0:dst.a,ref=(reg(0xDC)>>8)&255,mask=(reg(0xDC)>>16)&255;
    bool spass=compare(reg(0xDC)&7,sv&mask,ref&mask);
    uint op=!spass?(reg(0xDD)&255):!zpass?((reg(0xDD)>>8)&255):((reg(0xDD)>>16)&255);
    ns=stencilOp(op,sv,ref);
    if(!spass||!zpass) {
      uint maskKeep=keep|~pack(uint4(0,0,0,255));
      dst.a=ns;Colour[index]=(old&maskKeep)|(pack(dst)&~maskKeep);return;
    }
  } else if(!zpass)return;
  if(enabled(128)) {
    int4 src=rgba;
    for(uint ch=0;ch<3;++ch) {
      int s=src[ch]*factor(reg(0xDF)&15,true,ch,src,dst)/255;
      int d=dst[ch]*factor((reg(0xDF)>>4)&15,false,ch,src,dst)/255;
      uint op=(reg(0xDF)>>8)&7;
      int v=op==0?s+d:op==1?s-d:op==2?d-s:op==3?min(src[ch],dst[ch]):op==4?max(src[ch],dst[ch]):abs(src[ch]-dst[ch]);
      rgba[ch]=clamp(v,0,255);
    }
  }
  if(enabled(32))rgba.a=ns;
  uint outValue=pack(rgba);
  if(enabled(256)) {
    uint raw=rgba.r|(rgba.g<<8)|(rgba.b<<16)|(rgba.a<<24);
    uint d=dst.r|(dst.g<<8)|(dst.b<<16)|(dst.a<<24);
    outValue=pack(bytes(logicOp(reg(0xE6)&15,raw,d)));
  }
  Colour[index]=(old&keep)|(outValue&~keep);
  if(enabled(64)&&!(reg(0xE7)&1))Depth[di]=zi;
}

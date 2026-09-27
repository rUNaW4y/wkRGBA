sampler2D image : register(s0);
sampler2D palette : register(s1);
sampler2D backdrop : register(s2);
sampler2D artwork : register(s3);
sampler2D terrain : register(s4);
float4 mapUv : register(c1); // xy scale, zw offset
float4 artBounds : register(c2); // xy artwork extent in atlas coordinates
float4 main(float2 uv:TEXCOORD0):COLOR0 {
  float idx=tex2D(image,uv).r;
  float2 palUv=float2(idx*(255.0/256.0)+(0.5/256.0),0.5);
  float4 native=tex2D(palette,palUv);
  float2 artUv=uv*mapUv.xy+mapUv.zw;
  float4 art=tex2D(artwork,artUv);
  float inside=step(0.0,artUv.x)*step(0.0,artUv.y)*step(artUv.x,artBounds.x)*step(artUv.y,artBounds.y);
  float owner=tex2D(terrain,uv).r;
  float rgbaOwner=step(0.75,owner)*inside;
  float blackOwner=step(0.25,owner)*(1.0-step(0.75,owner));
  float bgidx=tex2D(backdrop,uv).r;
  float4 bg=tex2D(palette,float2(bgidx*(255.0/256.0)+(0.5/256.0),0.5));
  float3 rgba=lerp(bg.rgb,art.rgb,art.a);
  float3 foreground=lerp(native.rgb,float3(0.0,0.0,0.0),blackOwner);
  return float4(lerp(foreground,rgba,rgbaOwner*step(127.5/255.0,art.a)),1.0);
}


sampler Frame : register(s0);     // $basetexture = _rt_FullFrameFB
// c0  1/width, 1/height, edge threshold, strength
const float4 c0 : register(c0);

struct PS_INPUT
{
	float2 uv : TEXCOORD0;
};

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 main(const PS_INPUT i) : COLOR
{
	float2 px = c0.xy;
	float3 cM = tex2D(Frame, i.uv).rgb;
	float lM = luma(cM);
	float lN = luma(tex2D(Frame, i.uv + float2(0.0, -px.y)).rgb);
	float lS = luma(tex2D(Frame, i.uv + float2(0.0,  px.y)).rgb);
	float lW = luma(tex2D(Frame, i.uv + float2(-px.x, 0.0)).rgb);
	float lE = luma(tex2D(Frame, i.uv + float2( px.x, 0.0)).rgb);

	float lo = min(lM, min(min(lN, lS), min(lW, lE)));
	float hi = max(lM, max(max(lN, lS), max(lW, lE)));
	float range = hi - lo;
	float edge = saturate((range - c0.z) / max(c0.z, 1e-3));   // 0 below threshold

	// gradient to edge tangent
	float2 g = float2(lE - lW, lS - lN);
	float2 t = float2(-g.y, g.x);
	float tl = length(t);
	t = (tl > 1e-5) ? t / tl : float2(1.0, 0.0);
	float stretch = clamp(1.0 / max(abs(t.x), abs(t.y)), 1.0, 2.0);
	float2 d = t * px * stretch;

	float3 a = tex2D(Frame, i.uv + d * 0.5).rgb + tex2D(Frame, i.uv - d * 0.5).rgb;
	float3 b = tex2D(Frame, i.uv + d * 1.5).rgb + tex2D(Frame, i.uv - d * 1.5).rgb;
	float3 along = a * 0.3 + b * 0.2;

	float3 col = lerp(cM, along, edge * c0.w);
	return float4(col, 1.0);
}

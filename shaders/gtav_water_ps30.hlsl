sampler BumpSampler  : register(s0);   // $basetexture  waterbump2 (linear read)
sampler NoiseSampler : register(s1);   // $texture1     waternoise (linear read)
sampler FrameSampler : register(s2);   // $texture2     _rt_FullFrameFB

// c0   time, sun dir xyz
// c1   horizon rgb (fog colour), spec
// c2   deep water rgb, refraction mix
// c3   fog start, fog end
// c10  eye
const float4 c0  : register(c0);
const float4 c1  : register(c1);
const float4 c2  : register(c2);
const float4 c3  : register(c3);
const float4 cEye : register(c10);

static const float3 ZENITH = float3(0.0545, 0.1441, 0.4342);  // (66,106,176)^2.2
static const float3 SUNCOL = float3(1.0, 0.93, 0.80);

struct PS_INPUT
{
	float2 vUv     : TEXCOORD0;
	float3 vWorld  : TEXCOORD1;
	float4 vScreen : TEXCOORD2;
};

float2 slopes(float2 uv)
{
	float4 t = tex2D(BumpSampler, uv);
	return float2(t.r, t.a) * 2.0 - 1.0;
}

float3 skyColour(float3 d, float3 sun)
{
	float t = saturate(d.z * 2.0);
	float s = t * t * (3.0 - 2.0 * t);
	float3 sky = lerp(c1.rgb, ZENITH, s);
	float g = saturate(dot(d, sun));
	float glare = pow(g, 8.0) * 0.30 + pow(g, 64.0) * 0.55 + pow(g, 256.0) * 0.65;
	return sky + glare * float3(1.0, 0.82, 0.60);
}

float4 main(const PS_INPUT i) : COLOR
{
	float time = c0.x;
	float3 sun = normalize(c0.yzw);

	float3 toEye = cEye.xyz - i.vWorld;
	float dist = length(toEye);
	float3 V = toEye / max(dist, 1e-3);

	// waves, uv is world / 2048
	float2 uv = i.vUv;
	float2 s1 = slopes(uv * 12.0 + float2(0.021, 0.013) * time);
	float2 s2 = slopes(uv * float2(-5.1, 4.7) + float2(-0.009, 0.016) * time);
	float noise = tex2D(NoiseSampler, uv * 0.9 + float2(0.003, -0.002) * time).g;
	float2 sl = s1 * 0.6 + s2 * 0.8;
	float strength = lerp(0.55, 0.12, saturate(dist / 30000.0)) * (0.55 + noise);
	float3 N = normalize(float3(sl * strength, 1.0));

	float NdV = saturate(dot(N, V));
	float F = 0.02 + 0.98 * pow(1.0 - NdV, 5.0);
	float3 R = reflect(-V, N);
	R.z = abs(R.z);                     // never reflect the underside of the sky
	float3 refl = skyColour(R, sun);

	float rs = saturate(dot(R, sun));
	float3 spec = SUNCOL * c1.w * (pow(rs, 900.0) * 6.0 + pow(rs, 90.0) * 0.35);

	float2 suv = i.vScreen.xy / i.vScreen.w * float2(0.5, -0.5) + 0.5;
	suv += N.xy * (0.035 / (1.0 + dist / 600.0));
	float3 behind = tex2D(FrameSampler, suv).rgb;
	float3 body = lerp(behind, c2.rgb, c2.w);

	float3 col = lerp(body, refl, F) + spec;

	float fog = saturate((dist - c3.x) / max(c3.y - c3.x, 1.0));
	col = lerp(col, c1.rgb, fog);

	return float4(col, 1.0);
}

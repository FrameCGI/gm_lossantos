float4x4 g_ModelViewProj : register(c4);
float4x3 g_Model         : register(c58);

struct VS_INPUT
{
	float4 vPos : POSITION;
	float2 vUv  : TEXCOORD0;
};

struct VS_OUTPUT
{
	float4 vProj   : POSITION;
	float2 vUv     : TEXCOORD0;   // absolute world xy / 2048
	float3 vWorld  : TEXCOORD1;
	float4 vScreen : TEXCOORD2;
};

VS_OUTPUT main(const VS_INPUT i)
{
	VS_OUTPUT o;
	float4 p = float4(i.vPos.xyz, 1.0);
	o.vProj = mul(p, g_ModelViewProj);
	o.vWorld = mul(p, g_Model);
	o.vUv = i.vUv;
	// clip pos for the screen space refraction
	o.vScreen = o.vProj;
	return o;
}

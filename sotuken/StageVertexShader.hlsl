#include "ShaderHeaders.hlsli"

Stage StageVS(float4 pos : POSITION, float2 uv : TEXCOORD)
{
	Stage output;
	output.svpos = pos;
	output.uv = uv;
	return output;
}
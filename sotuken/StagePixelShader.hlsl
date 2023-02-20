#include "ShaderHeaders.hlsli"

float4 StagePS( Stage input ) : SV_TARGET
{
	return float4(input.uv, 1.0f, 1.0f);
}
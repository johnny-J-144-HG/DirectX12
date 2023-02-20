#include "ShaderHeaders.hlsli"

float4 BasicPS(Output input) : SV_TARGET{
	float3 light = normalize(float3(1,-1,1));//光の向かうベクトル(平行光線)
	float3 lightColor = float3(1,1,1);//ライトのカラー(1,1,1で真っ白)

	//ディフューズ計算
	float diffuseB = saturate(dot(-light, input.normal));

	//光の反射ベクトル
	float3 refLight = normalize(reflect(light, input.normal.xyz));
	float specularB = pow(saturate(dot(refLight, -input.ray)), specular.a);

	//スフィアマップ用UV
	float2 sphereMapUV = input.vnormal.xy;
	sphereMapUV = (sphereMapUV + float2(1, -1)) * float2(0.5, -0.5);

	float4 texColor =  tex.Sample(smp, input.uv); //テクスチャカラー

	return max(diffuseB
		* diffuse//ディフューズ色
		* texColor//テクスチャカラー
		* sph.Sample(smp, sphereMapUV)//スフィアマップ(乗算)
		+ spa.Sample(smp, sphereMapUV) * texColor//スフィアマップ(加算)
		+ float4(specularB * specular.rgb, 1)//スペキュラー
		, float4(texColor * ambient, 1));//アンビエント

}
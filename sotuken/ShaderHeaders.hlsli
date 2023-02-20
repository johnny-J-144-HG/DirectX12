// 
Texture2D<float4> tex : register(t0);
Texture2D<float4> sph : register(t1);
Texture2D<float4> spa : register(t2);

SamplerState smp : register(s0);

//定数バッファー0
cbuffer SceneBuffer : register(b0) {
	matrix world;//ワールド変換行列
	matrix view;//ビュー行列
	matrix proj;//プロジェクション行列
	float3 eye; //視点
};

//定数バッファ1
//マテリアル用
cbuffer Material : register(b1) {
	float4 diffuse;//ディフューズ色
	float4 specular;//スペキュラ
	float3 ambient;//アンビエント
};

//モデル描画
struct Output {
	float4 pos:POSITION;		// 頂点座標
	float4 svpos : SV_POSITION; // システム用頂点座標
	float4 normal : NORMAL0;	// 法線ベクトル
	float4 vnormal : NORMAL1;	// ビュー変換後の法線ベクトル
	float2 uv : TEXCOORD;		// UV 値
	float3 ray:VECTOR;//ベクトル
};

//ステージ描画用
struct Stage {
	float4 svpos:SV_POSITION;//システム用頂点座標
	float2 uv:TEXCOORD;//UV値
};
// 
Texture2D<float4> tex : register(t0);
Texture2D<float4> sph : register(t1);
Texture2D<float4> spa : register(t2);

SamplerState smp : register(s0);

//�萔�o�b�t�@�[0
cbuffer SceneBuffer : register(b0) {
	matrix world;//���[���h�ϊ��s��
	matrix view;//�r���[�s��
	matrix proj;//�v���W�F�N�V�����s��
	float3 eye; //���_
};

//�萔�o�b�t�@1
//�}�e���A���p
cbuffer Material : register(b1) {
	float4 diffuse;//�f�B�t���[�Y�F
	float4 specular;//�X�y�L����
	float3 ambient;//�A���r�G���g
};

//���f���`��
struct Output {
	float4 pos:POSITION;		// ���_���W
	float4 svpos : SV_POSITION; // �V�X�e���p���_���W
	float4 normal : NORMAL0;	// �@���x�N�g��
	float4 vnormal : NORMAL1;	// �r���[�ϊ���̖@���x�N�g��
	float2 uv : TEXCOORD;		// UV �l
	float3 ray:VECTOR;//�x�N�g��
};

//�X�e�[�W�`��p
struct Stage {
	float4 svpos:SV_POSITION;//�V�X�e���p���_���W
	float2 uv:TEXCOORD;//UV�l
};
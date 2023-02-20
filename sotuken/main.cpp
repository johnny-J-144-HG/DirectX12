//コンスタントバッファで行列を転送
#include<Windows.h>
#include<tchar.h>
#include<d3d12.h>
#include<dxgi1_6.h>
#include<DirectXMath.h>
#include<vector>
#include<dinput.h>
#include<d3dcompiler.h>
#include<DirectXTex.h>
#include<d3dx12.h>


#ifdef _DEBUG
#include<iostream>
#endif

#pragma comment(lib, "DirectXTex.lib")
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")


using namespace std;
using namespace DirectX;

///@brief コンソール画面にフォーマット付き文字列を表示
///@param format フォーマット(%dとか%fとかの)
///@param 可変長引数
///@remarksこの関数はデバッグ用です。デバッグ時にしか動作しません
void DebugOutputFormatString(const char* format, ...) {
#ifdef _DEBUG
	va_list valist;
	va_start(valist, format);
	printf(format, valist);
	va_end(valist);
#endif
}

//面倒だけど書かなあかんやつ
LRESULT WindowProcedure(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	if (msg == WM_DESTROY) {//ウィンドウが破棄されたら呼ばれます
		PostQuitMessage(0);//OSに対して「もうこのアプリは終わるんや」と伝える
		return 0;
	}
	return DefWindowProc(hwnd, msg, wparam, lparam);//規定の処理を行う
}

const unsigned int window_width = 1280;
const unsigned int window_height = 720;

IDXGIFactory4* _dxgiFactory = nullptr;
ID3D12Device* _dev = nullptr;
ID3D12CommandAllocator* _cmdAllocator = nullptr;
ID3D12GraphicsCommandList* _cmdList = nullptr;
ID3D12CommandQueue* _cmdQueue = nullptr;
IDXGISwapChain4* _swapchain = nullptr;

///モデルのパスとテクスチャのパスから合成パスを得る
///@param modelPath アプリケーションから見たpmdモデルのパス
///@param texPath PMDモデルから見たテクスチャのパス
///@return アプリケーションから見たテクスチャのパス
std::string GetTexturePathFromModelAndTexPath(const std::string& modelPath, const char* texPath) {
	//ファイルのフォルダ区切りは\と/の二種類が使用される可能性があり
	//ともかく末尾の\か/を得られればいいので、双方のrfindをとり比較する
	//int型に代入しているのは見つからなかった場合はrfindがepos(-1→0xffffffff)を返すため
	int pathIndex1 = modelPath.rfind('/');
	int pathIndex2 = modelPath.rfind('\\');
	auto pathIndex = max(pathIndex1, pathIndex2);
	auto folderPath = modelPath.substr(0, pathIndex + 1);
	return folderPath + texPath;
}

///ファイル名から拡張子を取得する
///@param path 対象のパス文字列
///@return 拡張子
string GetExtension(const std::string& path) {
	auto idx = path.rfind('.');
	return path.substr(idx + 1, path.length() - idx - 1);
}

///ファイル名から拡張子を取得する(ワイド文字版)
///@param path 対象のパス文字列
///@return 拡張子
wstring GetExtension(const std::wstring& path) {
	auto idx = path.rfind(L'.');
	return path.substr(idx + 1, path.length() - idx - 1);
}

///テクスチャのパスをセパレータ文字で分離する
///@param path 対象のパス文字列
///@param splitter 区切り文字
///@return 分離前後の文字列ペア
pair<string, string> SplitFileName(const std::string& path, const char splitter = '*') {
	auto idx = path.find(splitter);
	pair<string, string> ret;
	ret.first = path.substr(0, idx);
	ret.second = path.substr(idx + 1, path.length() - idx - 1);
	return ret;
}

///string(マルチバイト文字列)からwstring(ワイド文字列)を得る
///@param str マルチバイト文字列
///@return 変換されたワイド文字列
wstring GetWideStringFromString(const std::string& str) {
	//呼び出し1回目(文字列数を得る)
	auto num1 = MultiByteToWideChar(CP_ACP,
		MB_PRECOMPOSED | MB_ERR_INVALID_CHARS,
		str.c_str(), -1, nullptr, 0);

	std::wstring wstr;//stringのwchar_t版
	wstr.resize(num1);//得られた文字列数でリサイズ

	//呼び出し2回目(確保済みのwstrに変換文字列をコピー)
	auto num2 = MultiByteToWideChar(CP_ACP,
		MB_PRECOMPOSED | MB_ERR_INVALID_CHARS,
		str.c_str(), -1, &wstr[0], num1);

	assert(num1 == num2);//一応チェック
	return wstr;
}

ID3D12Resource* LoadTextureFromFile(std::string& texPath) {
	//WICテクスチャのロード
	TexMetadata metadata = {};
	ScratchImage scratchImg = {};

	auto result = LoadFromWICFile(
		GetWideStringFromString(texPath).c_str(),
		WIC_FLAGS_NONE,
		&metadata,
		scratchImg);

	//テクスチャの指定がない(""の場合などの)時に nullptr を返す
	if (FAILED(result)){
		return nullptr;
	}

	auto img = scratchImg.GetImage(0,0,0); //生データ抽出

	//WriteToSubresourceで転送する用のヒープ設定
	D3D12_HEAP_PROPERTIES texHeapProp = {};
	texHeapProp.Type = D3D12_HEAP_TYPE_CUSTOM;//特殊な設定なのでdefaultでもuploadでもなく
	texHeapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;//ライトバックで
	texHeapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;//転送がL0つまりCPU側から直で
	texHeapProp.CreationNodeMask = 0;//単一アダプタのため0
	texHeapProp.VisibleNodeMask = 0;//単一アダプタのため0

	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.Format = metadata.format;
	resDesc.Width = static_cast<UINT>(metadata.width);//幅
	resDesc.Height = static_cast<UINT>(metadata.height);//高さ
	resDesc.DepthOrArraySize = static_cast<UINT16>(metadata.arraySize);
	resDesc.SampleDesc.Count = 1;//通常テクスチャなのでアンチエイリアシングしない
	resDesc.SampleDesc.Quality = 0;//クオリティは最低
	resDesc.MipLevels = static_cast<UINT16>(metadata.mipLevels);//ミップマップしないのでミップ数は１つ
	resDesc.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(metadata.dimension);
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;//レイアウトについては決定しない
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;//とくにフラグなし

	//バッファー作成
	ID3D12Resource* texbuff = nullptr;
	result = _dev->CreateCommittedResource(
		&texHeapProp,
		D3D12_HEAP_FLAG_NONE,
		&resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		nullptr,
		IID_PPV_ARGS(&texbuff));

	if (FAILED(result)) {
		return nullptr;
	}

	result = texbuff->WriteToSubresource(
		0,
		nullptr,
		img->pixels,
		img->rowPitch,
		img->slicePitch);

	if (FAILED(result)) {
		return nullptr;
	}

	return texbuff;
}

ID3D12Resource* CreateWhiteTexture() {
	D3D12_HEAP_PROPERTIES texHeapProp = {};
	texHeapProp.Type = D3D12_HEAP_TYPE_CUSTOM;//特殊な設定なのでdefaultでもuploadでもなく
	texHeapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;//ライトバックで
	texHeapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;//転送がL0つまりCPU側から直で
	texHeapProp.CreationNodeMask = 0;//単一アダプタのため0
	texHeapProp.VisibleNodeMask = 0;//単一アダプタのため0

	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	resDesc.Width = 4;//幅
	resDesc.Height = 4;//高さ
	resDesc.DepthOrArraySize = 1;
	resDesc.SampleDesc.Count = 1;
	resDesc.SampleDesc.Quality = 0;//
	resDesc.MipLevels = 1;//
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;//レイアウトについては決定しない
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;//とくにフラグなし

	ID3D12Resource* whiteBuff = nullptr;
	auto result = _dev->CreateCommittedResource(
		&texHeapProp,
		D3D12_HEAP_FLAG_NONE,//特に指定なし
		&resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		nullptr,
		IID_PPV_ARGS(&whiteBuff)
	);
	if (FAILED(result)) {
		return nullptr;
	}
	std::vector<unsigned char> data(4 * 4 * 4);
	std::fill(data.begin(), data.end(), 0xff);

	result = whiteBuff->WriteToSubresource(0, nullptr, data.data(), 4 * 4, static_cast<UINT>(data.size()));
	return whiteBuff;
}

ID3D12Resource* CreateBlackTexture() {
	D3D12_HEAP_PROPERTIES texHeapProp = {};
	texHeapProp.Type = D3D12_HEAP_TYPE_CUSTOM;//特殊な設定なのでdefaultでもuploadでもなく
	texHeapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;//ライトバックで
	texHeapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;//転送がL0つまりCPU側から直で
	texHeapProp.CreationNodeMask = 0;//単一アダプタのため0
	texHeapProp.VisibleNodeMask = 0;//単一アダプタのため0

	D3D12_RESOURCE_DESC resDesc = {};
	resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	resDesc.Width = 4;//幅
	resDesc.Height = 4;//高さ
	resDesc.DepthOrArraySize = 1;
	resDesc.SampleDesc.Count = 1;
	resDesc.SampleDesc.Quality = 0;//
	resDesc.MipLevels = 1;//
	resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;//レイアウトについては決定しない
	resDesc.Flags = D3D12_RESOURCE_FLAG_NONE;//とくにフラグなし

	ID3D12Resource* blackBuff = nullptr;
	auto result = _dev->CreateCommittedResource(
		&texHeapProp,
		D3D12_HEAP_FLAG_NONE,//特に指定なし
		&resDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		nullptr,
		IID_PPV_ARGS(&blackBuff)
	);
	if (FAILED(result)) {
		return nullptr;
	}
	std::vector<unsigned char> data(4 * 4 * 4);
	std::fill(data.begin(), data.end(), 0x00);

	result = blackBuff->WriteToSubresource(0, nullptr, data.data(), 4 * 4, static_cast<UINT>(data.size()));
	return blackBuff;
}

///アライメントに揃えたサイズを返す
///@param size 元のサイズ
///@param alignment アライメントサイズ
///@return アライメントをそろえたサイズ
size_t
AlignmentedSize(size_t size, size_t alignment) {
	return size + alignment - size % alignment;
}

void EnableDebugLayer() {
	ID3D12Debug* debugLayer = nullptr;
	auto result = D3D12GetDebugInterface(IID_PPV_ARGS(&debugLayer));
	debugLayer->EnableDebugLayer();
	debugLayer->Release();
}

#ifdef _DEBUG
int main() {
#else
#include<Windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#endif
	DebugOutputFormatString("Show window test.");
	HINSTANCE hInst = GetModuleHandle(nullptr);
	//ウィンドウクラス生成＆登録
	WNDCLASSEX w = {};
	w.cbSize = sizeof(WNDCLASSEX);
	w.lpfnWndProc = (WNDPROC)WindowProcedure;//コールバック関数の指定
	w.lpszClassName = _T("DirectXTest");//アプリケーションクラス名(適当でいいです)
	w.hInstance = GetModuleHandle(0);//ハンドルの取得
	RegisterClassEx(&w);//アプリケーションクラス(こういうの作るからよろしくってOSに予告する)

	RECT wrc = { 0,0, window_width, window_height };//ウィンドウサイズを決める
	AdjustWindowRect(&wrc, WS_OVERLAPPEDWINDOW, false);//ウィンドウのサイズはちょっと面倒なので関数を使って補正する
	//ウィンドウオブジェクトの生成
	HWND hwnd = CreateWindow(w.lpszClassName,//クラス名指定
		_T("卒研"),//タイトルバーの文字
		WS_OVERLAPPEDWINDOW,//タイトルバーと境界線があるウィンドウです
		CW_USEDEFAULT,//表示X座標はOSにお任せします
		CW_USEDEFAULT,//表示Y座標はOSにお任せします
		wrc.right - wrc.left,//ウィンドウ幅
		wrc.bottom - wrc.top,//ウィンドウ高
		nullptr,//親ウィンドウハンドル
		nullptr,//メニューハンドル
		w.hInstance,//呼び出しアプリケーションハンドル
		nullptr);//追加パラメータ

#ifdef _DEBUG
	//デバッグレイヤーをオンに
	EnableDebugLayer();
#endif
	//DirectX12まわり初期化
	//フィーチャレベル列挙
	D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_12_1,
		D3D_FEATURE_LEVEL_12_0,
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
	};
	auto result = CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&_dxgiFactory));
	std::vector <IDXGIAdapter*> adapters;
	IDXGIAdapter* tmpAdapter = nullptr;
	for (int i = 0; _dxgiFactory->EnumAdapters(i, &tmpAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
		adapters.push_back(tmpAdapter);
	}
	for (auto adpt : adapters) {
		DXGI_ADAPTER_DESC adesc = {};
		adpt->GetDesc(&adesc);
		std::wstring strDesc = adesc.Description;
		if (strDesc.find(L"NVIDIA") != std::string::npos) {
			tmpAdapter = adpt;
			break;
		}
	}

	//Direct3Dデバイスの初期化
	D3D_FEATURE_LEVEL featureLevel;
	for (auto l : levels) {
		if (D3D12CreateDevice(tmpAdapter, l, IID_PPV_ARGS(&_dev)) == S_OK) {
			featureLevel = l;
			break;
		}
	}

	//DirectInputの初期化
	static LPDIRECTINPUT8A g_pInputInterface;
	result = DirectInput8Create(hInst, DIRECTINPUT_VERSION, IID_IDirectInput8, (void**)&g_pInputInterface, nullptr);

	static LPDIRECTINPUTDEVICE8A g_pinputDevice;
	result = g_pInputInterface->CreateDevice(GUID_SysKeyboard, &g_pinputDevice, nullptr);

	result = g_pinputDevice->SetDataFormat(&c_dfDIKeyboard);
	result = g_pinputDevice->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);

	result = g_pinputDevice->Acquire();


	result = _dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_cmdAllocator));
	result = _dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _cmdAllocator, nullptr, IID_PPV_ARGS(&_cmdList));

	D3D12_COMMAND_QUEUE_DESC cmdQueueDesc = {};
	cmdQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;//タイムアウトなし
	cmdQueueDesc.NodeMask = 0;
	cmdQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;//プライオリティ特に指定なし
	cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;//ここはコマンドリストと合わせてください
	result = _dev->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(&_cmdQueue));//コマンドキュー生成

	DXGI_SWAP_CHAIN_DESC1 swapchainDesc = {};
	swapchainDesc.Width = window_width;
	swapchainDesc.Height = window_height;
	swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapchainDesc.Stereo = false;
	swapchainDesc.SampleDesc.Count = 1;
	swapchainDesc.SampleDesc.Quality = 0;
	swapchainDesc.BufferUsage = DXGI_USAGE_BACK_BUFFER;
	swapchainDesc.BufferCount = 2;
	swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;


	result = _dxgiFactory->CreateSwapChainForHwnd(_cmdQueue,
		hwnd,
		&swapchainDesc,
		nullptr,
		nullptr,
		(IDXGISwapChain1**)&_swapchain);

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;//レンダーターゲットビューなので当然RTV
	heapDesc.NodeMask = 0;
	heapDesc.NumDescriptors = 2;//表裏の２つ
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;//特に指定なし
	ID3D12DescriptorHeap* rtvHeaps = nullptr;
	result = _dev->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeaps));
	DXGI_SWAP_CHAIN_DESC swcDesc = {};
	result = _swapchain->GetDesc(&swcDesc);
	std::vector<ID3D12Resource*> _backBuffers(swcDesc.BufferCount);
	D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeaps->GetCPUDescriptorHandleForHeapStart();

	//SRGBレンダーターゲットビュー設定
	D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;


	for (int i = 0; i < swcDesc.BufferCount; ++i) {
		result = _swapchain->GetBuffer(i, IID_PPV_ARGS(&_backBuffers[i]));
		rtvDesc.Format = _backBuffers[i]->GetDesc().Format;
		_dev->CreateRenderTargetView(_backBuffers[i], &rtvDesc, rtvH);
		rtvH.ptr += _dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	//深度バッファ作成
	//深度バッファの仕様
	D3D12_RESOURCE_DESC depthResDesc = {};
	depthResDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;//2次元のテクスチャデータとして
	depthResDesc.Width = window_width;							//幅と高さはレンダーターゲットと同じ
	depthResDesc.Height = window_height;						//上に同じ
	depthResDesc.DepthOrArraySize = 1;							//テクスチャ配列でもないし3Dテクスチャでもない
	depthResDesc.Format = DXGI_FORMAT_D32_FLOAT;					//深度値書き込み用フォーマット
	depthResDesc.SampleDesc.Count = 1;							//サンプルは1ピクセル当たり1つ
	depthResDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;//このバッファは深度ステンシルとして使用します
	depthResDesc.MipLevels = 1;
	depthResDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthResDesc.Alignment = 0;


	//デプス用ヒーププロパティ
	D3D12_HEAP_PROPERTIES depthHeapProp = {};
	depthHeapProp.Type = D3D12_HEAP_TYPE_DEFAULT;//DEFAULTだから後はUNKNOWNでよし
	depthHeapProp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	depthHeapProp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

	//このクリアバリューが重要な意味を持つ
	D3D12_CLEAR_VALUE _depthClearValue = {};
	_depthClearValue.DepthStencil.Depth = 1.0f;//深さ１(最大値)でクリア
	_depthClearValue.Format = DXGI_FORMAT_D32_FLOAT;//32bit深度値としてクリア

	ID3D12Resource* depthBuffer = nullptr;
	result = _dev->CreateCommittedResource(
		&depthHeapProp,
		D3D12_HEAP_FLAG_NONE,
		&depthResDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE, //デプス書き込みに使用
		&_depthClearValue,
		IID_PPV_ARGS(&depthBuffer));

	//深度のためのデスクリプタヒープ作成
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};//深度に使うよという事がわかればいい
	dsvHeapDesc.NumDescriptors = 1;//深度ビュー1つのみ
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;//デプスステンシルビューとして使う
	ID3D12DescriptorHeap* dsvHeap = nullptr;
	result = _dev->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&dsvHeap));

	//深度ビュー作成
	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;//デプス値に32bit使用
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;//2Dテクスチャ
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;//フラグは特になし
	_dev->CreateDepthStencilView(depthBuffer, &dsvDesc, dsvHeap->GetCPUDescriptorHandleForHeapStart());


	ID3D12Fence* _fence = nullptr;
	UINT64 _fenceVal = 0;
	result = _dev->CreateFence(_fenceVal, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence));

	ShowWindow(hwnd, SW_SHOW);//ウィンドウ表示

	auto WhiteTex = CreateWhiteTexture();
	auto BlackTex = CreateBlackTexture();


	//PMDヘッダ構造体
	struct PMDHeader {
		float version; //例：00 00 80 3F == 1.00
		char model_name[20];//モデル名
		char comment[256];//モデルコメント
	};
	char signature[3];
	PMDHeader pmdheader = {};

	std::string strModelPath = "model/ボコボコにしてやんよ.pmd";
	FILE* fp;
	errno_t	error = fopen_s(&fp, strModelPath.c_str() ,"rb");
	fread(signature, sizeof(signature), 1, fp);
	fread(&pmdheader, sizeof(pmdheader), 1, fp);

	unsigned int vertNum;//頂点数
	fread(&vertNum, sizeof(vertNum), 1, fp);

	constexpr unsigned int pmdvertex_size = 38;//頂点1つあたりのサイズ
	std::vector<unsigned char> vertices(vertNum * pmdvertex_size);//バッファ確保
	fread(vertices.data(), vertices.size(), 1, fp);//一気に読み込み

	unsigned int indicesNum;//インデックス数
	fread(&indicesNum, sizeof(indicesNum), 1, fp);

#pragma pack(1)//ここから1バイトパッキング…アライメントは発生しない
	//PMDマテリアル構造体
	struct PMDMaterial {
		XMFLOAT3 diffuse; //ディフューズ色
		float alpha; // ディフューズα
		float specularity;//スペキュラの強さ(乗算値)
		XMFLOAT3 specular; //スペキュラ色
		XMFLOAT3 ambient; //アンビエント色
		unsigned char toonIdx; //トゥーン番号(後述)
		unsigned char edgeFlg;//マテリアル毎の輪郭線フラグ
		//2バイトのパディングが発生！！
		unsigned int indicesNum; //このマテリアルが割り当たるインデックス数
		char texFilePath[20]; //テクスチャファイル名(プラスアルファ…後述)
	};//70バイトのはず…でもパディングが発生するため72バイト
#pragma pack()//1バイトパッキング解除(デフォルトに戻す)

	//シェーダ側に投げられるマテリアルデータ
	struct MaterialForHlsl {
		XMFLOAT3 diffuse; //ディフューズ色
		float alpha; // ディフューズα
		XMFLOAT3 specular; //スペキュラ色
		float specularity;//スペキュラの強さ(乗算値)
		XMFLOAT3 ambient; //アンビエント色
	};
	//それ以外のマテリアルデータ
	struct AdditionalMaterial {
		std::string texPath;//テクスチャファイルパス
		int toonIdx; //トゥーン番号
		bool edgeFlg;//マテリアル毎の輪郭線フラグ
	};
	//まとめたもの
	struct Material {
		unsigned int indicesNum;//インデックス数
		MaterialForHlsl material;
		AdditionalMaterial additional;
	};

	//UPLOAD(確保は可能)

	D3D12_HEAP_PROPERTIES C_heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	D3D12_RESOURCE_DESC C_resDesc = CD3DX12_RESOURCE_DESC::Buffer(vertices.size());
	ID3D12Resource* vertBuff = nullptr;
	result = _dev->CreateCommittedResource(
		&C_heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&C_resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&vertBuff));

	unsigned char* vertMap = nullptr;
	result = vertBuff->Map(0, nullptr, (void**)&vertMap);
	std::copy(vertices.begin(), vertices.end(), vertMap);
	vertBuff->Unmap(0, nullptr);

	D3D12_VERTEX_BUFFER_VIEW vbView = {};
	vbView.BufferLocation = vertBuff->GetGPUVirtualAddress();//バッファの仮想アドレス
	vbView.SizeInBytes = vertices.size();//全バイト数
	vbView.StrideInBytes = pmdvertex_size;//1頂点あたりのバイト数

	std::vector<unsigned short> indices(indicesNum);

	fread(indices.data(), indices.size() * sizeof(indices[0]), 1, fp);

	unsigned int materialNum;//マテリアル数
	fread(&materialNum, sizeof(materialNum), 1, fp);
		
	std::vector<Material> materials(materialNum);
	std::vector<ID3D12Resource*> textureResources(materialNum);
	std::vector<ID3D12Resource*> sphResources(materialNum);
	std::vector<ID3D12Resource*> spaResources(materialNum);


	{
		std::vector<PMDMaterial> pmdMaterials(materialNum);
		fread(pmdMaterials.data(), pmdMaterials.size() * sizeof(PMDMaterial), 1, fp);
		//コピー
		for (int i = 0; i < pmdMaterials.size(); ++i) {
			materials[i].indicesNum = pmdMaterials[i].indicesNum;
			materials[i].material.diffuse = pmdMaterials[i].diffuse;
			materials[i].material.alpha = pmdMaterials[i].alpha;
			materials[i].material.specular = pmdMaterials[i].specular;
			materials[i].material.specularity = pmdMaterials[i].specularity;
			materials[i].material.ambient = pmdMaterials[i].ambient;
		}

		for (int i = 0; i < pmdMaterials.size(); i++){
			if (strlen(pmdMaterials[i].texFilePath) == 0 ){
				textureResources[i] = nullptr;
			}

			string texFileName = pmdMaterials[i].texFilePath;
			string sphFileName = "";
			string spaFileName = "";
			if (count(texFileName.begin(), texFileName.end(), '*') > 0) {//スプリッタがある
				auto namepair = SplitFileName(texFileName);
				if (GetExtension(namepair.first) == "sph") {
					texFileName = namepair.second;
					sphFileName = namepair.first;
				}
				else if (GetExtension(namepair.first) == "spa") {
					texFileName = namepair.second;
					spaFileName = namepair.first;
				}
				else {
					texFileName = namepair.first;
					if (GetExtension(namepair.second) == "sph") {
						sphFileName = namepair.second;
					}
					else if (GetExtension(namepair.second) == "spa") {
						spaFileName = namepair.second;
					}
				}
			}
			else {
				if (GetExtension(pmdMaterials[i].texFilePath) == "sph") {
					sphFileName = pmdMaterials[i].texFilePath;
					texFileName = "";
				}
				else if (GetExtension(pmdMaterials[i].texFilePath) == "spa") {
					spaFileName = pmdMaterials[i].texFilePath;
					texFileName = "";
				}
				else {
					texFileName = pmdMaterials[i].texFilePath;
				}
			}

			//モデルとテクスチャパスからアプリケーションからのテクスチャパスを得る
			if (texFileName != "") {
				auto texFilePath = GetTexturePathFromModelAndTexPath(strModelPath, texFileName.c_str());
				textureResources[i] = LoadTextureFromFile(texFilePath);
			}
			if (sphFileName != "") {
				auto sphFilePath = GetTexturePathFromModelAndTexPath(strModelPath, sphFileName.c_str());
				sphResources[i] = LoadTextureFromFile(sphFilePath);
			}
			if (spaFileName != "") {
				auto spaFilePath = GetTexturePathFromModelAndTexPath(strModelPath, spaFileName.c_str());
				spaResources[i] = LoadTextureFromFile(spaFilePath);
			}
		}
	}

	fclose(fp);

	ID3D12Resource* idxBuff = nullptr;
	//設定は、バッファのサイズ以外頂点バッファの設定を使いまわして
	//OKだと思います。

	C_resDesc = CD3DX12_RESOURCE_DESC::Buffer(indices.size() * sizeof(indices[0]));
	result = _dev->CreateCommittedResource(
		&C_heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&C_resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&idxBuff));

	//作ったバッファにインデックスデータをコピー
	unsigned short* mappedIdx = nullptr;
	idxBuff->Map(0, nullptr, (void**)&mappedIdx);
	std::copy(indices.begin(), indices.end(), mappedIdx);
	idxBuff->Unmap(0, nullptr);

	//インデックスバッファビューを作成
	D3D12_INDEX_BUFFER_VIEW ibView = {};
	ibView.BufferLocation = idxBuff->GetGPUVirtualAddress();
	ibView.Format = DXGI_FORMAT_R16_UINT;
	ibView.SizeInBytes = indices.size() * sizeof(indices[0]);

	//マテリアルバッファーを作成
	auto materialBuffSize = sizeof(MaterialForHlsl);
	materialBuffSize = (materialBuffSize + 0xff) & ~0xff;

	ID3D12Resource* materialBuff = nullptr;
	C_resDesc = CD3DX12_RESOURCE_DESC::Buffer(materialBuffSize * materialNum);//もったいないが、仕方ない
	result = _dev->CreateCommittedResource(
		&C_heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&C_resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&materialBuff)
	);

	//マップマテリアルにデータをコピー
	char* mapMaterial = nullptr;

	result = materialBuff->Map(0, nullptr, (void**)&mapMaterial);

	for (auto& m : materials) {
		*((MaterialForHlsl*)mapMaterial) = m.material;	//データをコピー
		mapMaterial += materialBuffSize;				//次のアライメント位置まで進める(256の倍数)
	}
	materialBuff->Unmap(0, nullptr);

	//マテリアル用ディスクリプタヒープ
	ID3D12DescriptorHeap* materialDescHeap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC materialDescHeapDesc = {};
	materialDescHeapDesc.NumDescriptors = materialNum * 4;//マテリアル数ぶん(定数1つ、テクスチャ3つ)
	materialDescHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	materialDescHeapDesc.NodeMask = 0;
	materialDescHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

	//生成
	result = _dev->CreateDescriptorHeap(
		&materialDescHeapDesc,
		IID_PPV_ARGS(&materialDescHeap)
	);

	D3D12_CONSTANT_BUFFER_VIEW_DESC matCBVDesc = {};
	matCBVDesc.BufferLocation = materialBuff->GetGPUVirtualAddress();	//バッファーアドレス
	matCBVDesc.SizeInBytes = materialBuffSize;							//マテリアルの256アライメントサイズ

	////通常テクスチャビュー作成
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;//後述
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;//2Dテクスチャ
	srvDesc.Texture2D.MipLevels = 1;//ミップマップは使用しないので1

	//先頭を記録
	auto matDescHeapH = materialDescHeap->GetCPUDescriptorHandleForHeapStart();
	auto inc = _dev->GetDescriptorHandleIncrementSize(
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	for (int i = 0; i < materialNum; ++i) {
		//マテリアル用定数バッファービュー
		_dev->CreateConstantBufferView(&matCBVDesc, matDescHeapH);
		matDescHeapH.ptr += inc;			
		matCBVDesc.BufferLocation += materialBuffSize;

		//シェーダーリソースビュー
		if (textureResources[i] == nullptr){
			srvDesc.Format = WhiteTex->GetDesc().Format;
			_dev->CreateShaderResourceView(
				WhiteTex,
				&srvDesc,
				matDescHeapH);

		} else {
			srvDesc.Format = textureResources[i]->GetDesc().Format;
			_dev->CreateShaderResourceView(
				textureResources[i],
				&srvDesc,
				matDescHeapH);

		}

		matDescHeapH.ptr += inc;

		if (sphResources[i] == nullptr) {
			srvDesc.Format = WhiteTex->GetDesc().Format;
			_dev->CreateShaderResourceView(WhiteTex, &srvDesc, matDescHeapH);
		}
		else {
			srvDesc.Format = sphResources[i]->GetDesc().Format;
			_dev->CreateShaderResourceView(sphResources[i], &srvDesc, matDescHeapH);
		}
		matDescHeapH.ptr += inc;

		if(spaResources[i] == nullptr) {
			srvDesc.Format = BlackTex->GetDesc().Format;
			_dev->CreateShaderResourceView(BlackTex, &srvDesc, matDescHeapH);
		} else {
			srvDesc.Format = spaResources[i]->GetDesc().Format;
			_dev->CreateShaderResourceView(spaResources[i], &srvDesc, matDescHeapH);
		}
		matDescHeapH.ptr += inc;		
	}

	ID3DBlob* _vsBlob = nullptr;
	ID3DBlob* _psBlob = nullptr;

	ID3DBlob* errorBlob = nullptr;
	result = D3DCompileFromFile(L"BasicVertexShader.hlsl",
		nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		"BasicVS", "vs_5_0",
		D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
		0, &_vsBlob, &errorBlob);
	if (FAILED(result)) {
		if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
			::OutputDebugStringA("ファイルが見当たりません");
		}
		else {
			std::string errstr;
			errstr.resize(errorBlob->GetBufferSize());
			std::copy_n((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize(), errstr.begin());
			errstr += "\n";
			OutputDebugStringA(errstr.c_str());
		}
		exit(1);//行儀悪いかな…
	}
	result = D3DCompileFromFile(L"BasicPixelShader.hlsl",
		nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		"BasicPS", "ps_5_0",
		D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
		0, &_psBlob, &errorBlob);
	if (FAILED(result)) {
		if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
			::OutputDebugStringA("ファイルが見当たりません");
		}
		else {
			std::string errstr;
			errstr.resize(errorBlob->GetBufferSize());
			std::copy_n((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize(), errstr.begin());
			errstr += "\n";
			OutputDebugStringA(errstr.c_str());
		}
		exit(1);//行儀悪いかな…
	}
	D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
		{//　座標情報
			"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
		{//  法線情報
			"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
		{//　uv情報
			"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{//  ボーン番号情報
			"BONE_NO",0,DXGI_FORMAT_R16G16_UINT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
		{//  ウェイト情報
			"WEIGHT",0,DXGI_FORMAT_R8_UINT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },		
		{  //輪郭線フラグ情報
			"EDGE_FLG",0,DXGI_FORMAT_R8_UINT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
			
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC gpipeline = {};
	gpipeline.pRootSignature = nullptr;
	
	gpipeline.VS = CD3DX12_SHADER_BYTECODE(_vsBlob);
	/*
	gpipeline.VS.pShaderBytecode = _vsBlob->GetBufferPointer();
	gpipeline.VS.BytecodeLength = _vsBlob->GetBufferSize();
	*/
	gpipeline.PS.pShaderBytecode = _psBlob->GetBufferPointer();
	gpipeline.PS.BytecodeLength = _psBlob->GetBufferSize();

	gpipeline.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;//中身は0xffffffff

	//
	gpipeline.BlendState.AlphaToCoverageEnable = false;
	gpipeline.BlendState.IndependentBlendEnable = false;

	D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlendDesc = {};

	//ひとまず加算や乗算やαブレンディングは使用しない
	renderTargetBlendDesc.BlendEnable = false;
	renderTargetBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	//ひとまず論理演算は使用しない
	renderTargetBlendDesc.LogicOpEnable = false;

	gpipeline.BlendState.RenderTarget[0] = renderTargetBlendDesc;

	gpipeline.RasterizerState.MultisampleEnable = false;//まだアンチェリは使わない
	gpipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;//カリングしない
	gpipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;//中身を塗りつぶす
	gpipeline.RasterizerState.DepthClipEnable = true;//深度方向のクリッピングは有効に

	gpipeline.RasterizerState.FrontCounterClockwise = false;
	gpipeline.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	gpipeline.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	gpipeline.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	gpipeline.RasterizerState.AntialiasedLineEnable = false;
	gpipeline.RasterizerState.ForcedSampleCount = 0;
	gpipeline.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;


	gpipeline.DepthStencilState.DepthEnable = true;//深度バッファを使うぞ
	gpipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;//全て書き込み
	gpipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;//小さい方を採用
	gpipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	gpipeline.DepthStencilState.StencilEnable = false;

	gpipeline.InputLayout.pInputElementDescs = inputLayout;//レイアウト先頭アドレス
	gpipeline.InputLayout.NumElements = _countof(inputLayout);//レイアウト配列数

	gpipeline.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;//ストリップ時のカットなし
	gpipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;//三角形で構成

	gpipeline.NumRenderTargets = 1;//今は１つのみ
	gpipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;//0～1に正規化されたRGBA

	gpipeline.SampleDesc.Count = 1;//サンプリングは1ピクセルにつき１
	gpipeline.SampleDesc.Quality = 0;//クオリティは最低

	ID3D12RootSignature* rootsignature = nullptr;
	D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
	rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	D3D12_DESCRIPTOR_RANGE descTblRange[3] = {};

	//定数ひとつ目(座標変換用)
	descTblRange[0].NumDescriptors = 1;//定数ひとつ
	descTblRange[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;//種別は定数
	descTblRange[0].BaseShaderRegister = 0;//0番スロットから
	descTblRange[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	//定数ふたつめ(マテリアル用)
	descTblRange[1].NumDescriptors = 1;//デスクリプタヒープはたくさんあるが一度に使うのは１つ
	descTblRange[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;//種別は定数
	descTblRange[1].BaseShaderRegister = 1;//1番スロットから
	descTblRange[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	//テクスチャ1つ目(↑のマテリアルとペア)
	descTblRange[2].NumDescriptors = 4;//テクスチャ４つ(基本とsphとspaとトゥーン)
	descTblRange[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;//種別はテクスチャ
	descTblRange[2].BaseShaderRegister = 0;//0番スロットから
	descTblRange[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER rootparam[2] = {};
	rootparam[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootparam[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;	//ピクセルシェーダーから見える
	rootparam[0].DescriptorTable.pDescriptorRanges = &descTblRange[0];
	rootparam[0].DescriptorTable.NumDescriptorRanges = 1;

	rootparam[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootparam[1].DescriptorTable.pDescriptorRanges = &descTblRange[1];
	rootparam[1].DescriptorTable.NumDescriptorRanges = 2;
	rootparam[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	rootSignatureDesc.pParameters = rootparam;	//ルートパラメーターの先頭アドレス
	rootSignatureDesc.NumParameters = 2;		//ルートパラメーター数

	D3D12_STATIC_SAMPLER_DESC sampleDesc = {};
	sampleDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampleDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampleDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampleDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
	sampleDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampleDesc.MaxLOD = D3D12_FLOAT32_MAX;
	sampleDesc.MinLOD = 0.0f;
	sampleDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	sampleDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;

	rootSignatureDesc.pStaticSamplers = &sampleDesc;
	rootSignatureDesc.NumStaticSamplers = 1;

	ID3DBlob* rootSigBlob = nullptr;
	result = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rootSigBlob, &errorBlob);
	result = _dev->CreateRootSignature(0, rootSigBlob->GetBufferPointer(), rootSigBlob->GetBufferSize(), IID_PPV_ARGS(&rootsignature));
	rootSigBlob->Release();

	gpipeline.pRootSignature = rootsignature;
	ID3D12PipelineState* _pipelinestate = nullptr;
	result = _dev->CreateGraphicsPipelineState(&gpipeline, IID_PPV_ARGS(&_pipelinestate));

	//シェーダ側に渡すための基本的な行列データ
	struct SceneMatrix {
		XMMATRIX world;
		XMMATRIX view;
		XMMATRIX proj;
		XMFLOAT3 eye;
	};

	//定数バッファー作成
	auto worldMat = XMMatrixIdentity();
	XMFLOAT3 eye(0, 15, -25);
	XMFLOAT3 target(0, 10, 0);
	XMFLOAT3 up(0, 1, 0);
	auto viewMat = XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), XMLoadFloat3(&up));
	auto projMat = XMMatrixPerspectiveFovLH(XM_PIDIV4,//画角は90°
		static_cast<float>(window_width) / static_cast<float>(window_height),//アス比
		1.0f,//近い方
		100.0f//遠い方
	);

	ID3D12Resource* constBuff = nullptr;

	D3D12_HEAP_PROPERTIES constHeapProp = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	D3D12_RESOURCE_DESC constResDesc = CD3DX12_RESOURCE_DESC::Buffer((sizeof(SceneMatrix) + 0xff) & ~0xff);

	result = _dev->CreateCommittedResource(
		&constHeapProp,
		D3D12_HEAP_FLAG_NONE,
		&constResDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&constBuff)
	);

	SceneMatrix* mapScene;  //
	result = constBuff->Map(0, nullptr, (void**)&mapScene);  //
	//行列の内容をコピー
	mapScene->world = worldMat;
	mapScene->view = viewMat;
	mapScene->proj = projMat;
	mapScene->eye = eye;

	ID3D12DescriptorHeap* basicDescHeap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc = {};
	//シェーダーから見えるように
	descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	//マスクは0
	descHeapDesc.NodeMask = 0;
	//SRV１つとCBV１つ
	descHeapDesc.NumDescriptors = 1;
	//シェーダーリソースビュー用
	descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

	//生成
	result = _dev->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&basicDescHeap));

	auto basicHeapHandle = basicDescHeap->GetCPUDescriptorHandleForHeapStart();

	////デスクリプタの先頭ハンドルを取得しておく
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
	cbvDesc.BufferLocation = constBuff->GetGPUVirtualAddress();
	cbvDesc.SizeInBytes = static_cast<UINT>(constBuff->GetDesc().Width);
	//定数バッファビューの作成
	_dev->CreateConstantBufferView(&cbvDesc, basicHeapHandle);




	//ステージ用ソースコード
	/*：必要なもの
	* ・頂点(床情報系)
	* ・インデックス
	* ・頂点バッファー＆ビュー
	* ・インデックスバッファー＆ビュー
	* ・インプット情報
	* ・ステージ用パイプライン＆ルートシグネチャーオブジェクト
	* ・座標変換用ワールド行列
	* ・テクスチャ用バッファー＆ビュー
	*/

	/**/
	struct simplePolygonVertex
	{
		XMFLOAT3 pos;
		XMFLOAT2 uv;
	};

	simplePolygonVertex StageVertices[] =
	{
		{{-1.0f, 0.0f ,0.1f }, {0.0f, 0.0f}},//左奥
		{{ 1.0f, 0.0f ,0.1f }, {1.0f, 0.0f}},//右奥
		{{-1.0f, -1.0f ,0.1f }, {0.0f, 1.0f}},//左手前
		{{ 1.0f, -1.0f ,0.1f }, {1.0f, 1.0f}} //右手前
	};

	unsigned short stageIndices[] = { 0,1,2, 2,1,3 }; //インデックス

	//頂点バッファー＆ビュー

	D3D12_HEAP_PROPERTIES Stage_heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	D3D12_RESOURCE_DESC Stage_resDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(StageVertices));
	ID3D12Resource* stageVertBuff = nullptr;
	result = _dev->CreateCommittedResource(
		&Stage_heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&Stage_resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&stageVertBuff));

	simplePolygonVertex* stageVertMap = nullptr;
	result = stageVertBuff->Map(0, nullptr, (void**)&stageVertMap);
	std::copy(std::begin(StageVertices), std::end(StageVertices), stageVertMap);
	stageVertBuff->Unmap(0, nullptr);

	D3D12_VERTEX_BUFFER_VIEW stageVbView = {};

	stageVbView.BufferLocation = stageVertBuff->GetGPUVirtualAddress();
	stageVbView.SizeInBytes = sizeof(StageVertices);
	stageVbView.StrideInBytes = sizeof(StageVertices[0]);

	//インデックスバッファー＆ビュー

	ID3D12Resource* stageIdxBuff = nullptr;

	Stage_resDesc.Width = sizeof(stageIndices);
	//= CD3DX12_RESOURCE_DESC::Buffer(sizeof(stageIndices));
	result = _dev->CreateCommittedResource(
		&Stage_heapProperties,
		D3D12_HEAP_FLAG_NONE,
		&Stage_resDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&stageIdxBuff));

	unsigned short* mappedStageIdx = nullptr;
	stageIdxBuff->Map(0, nullptr, (void**)&mappedStageIdx);
	std::copy(std::begin(stageIndices), std::end(stageIndices), mappedStageIdx);
	stageIdxBuff->Unmap(0, nullptr);

	D3D12_INDEX_BUFFER_VIEW stageIbView = {};

	stageIbView.BufferLocation = stageIdxBuff->GetGPUVirtualAddress();
	stageIbView.Format = DXGI_FORMAT_R16_UINT;
	stageIbView.SizeInBytes = sizeof(stageIndices);

	ID3D10Blob* _stageVsBlob = nullptr;
	ID3D10Blob* _stagePsBlob = nullptr;

	result = D3DCompileFromFile(L"StageVertexShader.hlsl",
		nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		"StageVS", "vs_5_0",
		D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
		0, &_stageVsBlob, &errorBlob);
	if (FAILED(result)) {
		if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
			::OutputDebugStringA("ファイルが見当たりません");
		}
		else {
			std::string errstr;
			errstr.resize(errorBlob->GetBufferSize());
			std::copy_n((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize(), errstr.begin());
			errstr += "\n";
			OutputDebugStringA(errstr.c_str());
		}
		exit(1);//行儀悪いかな…
	}

	result = D3DCompileFromFile(L"StagePixelShader.hlsl",
		nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
		"StagePS", "ps_5_0",
		D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
		0, &_stagePsBlob, &errorBlob);
	if (FAILED(result)) {
		if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
			::OutputDebugStringA("ファイルが見当たりません");
		}
		else {
			std::string errstr;
			errstr.resize(errorBlob->GetBufferSize());
			std::copy_n((char*)errorBlob->GetBufferPointer(), errorBlob->GetBufferSize(), errstr.begin());
			errstr += "\n";
			OutputDebugStringA(errstr.c_str());
		}
		exit(1);//行儀悪いかな…
	}

	D3D12_INPUT_ELEMENT_DESC stageInputLayout[] = {
		{ "POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
		{ "TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
	};

	D3D12_GRAPHICS_PIPELINE_STATE_DESC stage_Gpipeline = {};

	

	stage_Gpipeline.VS = CD3DX12_SHADER_BYTECODE(_stageVsBlob);
	stage_Gpipeline.PS = CD3DX12_SHADER_BYTECODE(_stagePsBlob);

	stage_Gpipeline.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;//中身は0xffffffff

	stage_Gpipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	stage_Gpipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

	stage_Gpipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

	stage_Gpipeline.DepthStencilState.DepthEnable = true;//深度バッファを使うぞ
	stage_Gpipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;//全て書き込み
	stage_Gpipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;//小さい方を採用
	stage_Gpipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	
	stage_Gpipeline.DepthStencilState.StencilEnable = false;

	stage_Gpipeline.InputLayout.pInputElementDescs = stageInputLayout;
	stage_Gpipeline.InputLayout.NumElements = _countof(stageInputLayout);

	stage_Gpipeline.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
	stage_Gpipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

	stage_Gpipeline.NumRenderTargets = 1;
	stage_Gpipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

	stage_Gpipeline.SampleDesc.Count = 1;//サンプリングは1ピクセルにつき１
	stage_Gpipeline.SampleDesc.Quality = 0;//クオリティは最低
	
	ID3D12RootSignature* stageRootsignature = nullptr;
	D3D12_ROOT_SIGNATURE_DESC stageRootSignatureDesc = {};
	stageRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
/*
	D3D12_DESCRIPTOR_RANGE stageDescTblRange = {};
	stageDescTblRange.NumDescriptors = 1;//テクスチャひとつ
	stageDescTblRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;//種別はテクスチャ
	stageDescTblRange.BaseShaderRegister = 0;//0番スロットから
	stageDescTblRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;


	D3D12_ROOT_PARAMETER stageRootparam = {};
	stageRootparam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	stageRootparam.DescriptorTable.pDescriptorRanges = &stageDescTblRange;//デスクリプタレンジのアドレス
	stageRootparam.DescriptorTable.NumDescriptorRanges = 1;//デスクリプタレンジ数
	stageRootparam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;//ピクセルシェーダから見える

	stageRootSignatureDesc.pParameters = &stageRootparam;//ルートパラメータの先頭アドレス
	stageRootSignatureDesc.NumParameters = 1;//ルートパラメータ数

	D3D12_STATIC_SAMPLER_DESC samplerDesc = {};
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;//横繰り返し
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;//縦繰り返し
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;//奥行繰り返し
	samplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;//ボーダーの時は黒
	samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;//補間しない(ニアレストネイバー)
	samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;//ミップマップ最大値
	samplerDesc.MinLOD = 0.0f;//ミップマップ最小値
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;//オーバーサンプリングの際リサンプリングしない？
	samplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;//ピクセルシェーダからのみ可視

	stageRootSignatureDesc.pStaticSamplers = &samplerDesc;
	stageRootSignatureDesc.NumStaticSamplers = 1;
	*/
	ID3DBlob* stageRootSigBlob = nullptr;
	result = D3D12SerializeRootSignature(&stageRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &stageRootSigBlob, &errorBlob);
	result = _dev->CreateRootSignature(
		0,
		stageRootSigBlob->GetBufferPointer(),
		stageRootSigBlob->GetBufferSize(),
		IID_PPV_ARGS(&stageRootsignature)
	);
	stageRootSigBlob->Release();


	stage_Gpipeline.pRootSignature = stageRootsignature;

	ID3D12PipelineState* _stagePipelinestate = nullptr;

	result = _dev->CreateGraphicsPipelineState(&stage_Gpipeline, IID_PPV_ARGS(&_stagePipelinestate));

	//弾用ソースコード
	/*：必要なもの
	* ・頂点
	* ・インデックス
	* ・頂点バッファー＆ビュー
	* ・インデックスバッファー＆ビュー
	* ・インプット情報
	* ・ステージ用パイプライン＆ルートシグネチャーオブジェクト
	* ・座標変換用ワールド行列
	*/









	D3D12_VIEWPORT viewport = {};
	viewport.Width = window_width;//出力先の幅(ピクセル数)
	viewport.Height = window_height;//出力先の高さ(ピクセル数)
	viewport.TopLeftX = 0;//出力先の左上座標X
	viewport.TopLeftY = 0;//出力先の左上座標Y
	viewport.MaxDepth = 1.0f;//深度最大値
	viewport.MinDepth = 0.0f;//深度最小値

	D3D12_RECT scissorrect = {};
	scissorrect.top = 0;//切り抜き上座標
	scissorrect.left = 0;//切り抜き左座標
	scissorrect.right = scissorrect.left + window_width;//切り抜き右座標
	scissorrect.bottom = scissorrect.top + window_height;//切り抜き下座標

	MSG msg = {};
	unsigned int frame = 0;
	float angle = XM_PI;
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	BYTE keyState[256];
	result = g_pinputDevice->GetDeviceState(256, keyState);

	while (keyState[DIK_ESCAPE] != 0x80) {
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		//もうアプリケーションが終わるって時にmessageがWM_QUITになる
		if (msg.message == WM_QUIT) {
			break;
		}

		//キーボード入力検知
		result = g_pinputDevice->GetDeviceState(256, keyState);
		if (SUCCEEDED(result)){

			//右回転
			if (keyState[DIK_LEFT] & 0x80) {
				angle += 0.05f;
			}

			//左回転
			if (keyState[DIK_RIGHT] & 0x80) {
				angle -= 0.05f;
			}

			//右移動
			if (keyState[DIK_A] & 0x80) {
				x += 0.05f;
			}

			//左移動
			if (keyState[DIK_D] & 0x80) {
				x -= 0.05f;
			}

			//上昇
			if (keyState[DIK_UP] & 0x80) {
				target.y += 0.05f;
			}

			//下降
			if (keyState[DIK_DOWN] & 0x80) {
				target.y -= 0.05f;
			}

			//前進
			if (keyState[DIK_W] & 0x80) {
				z += 0.05f;
			}

			//後退
			if (keyState[DIK_S] & 0x80) {
				z -= 0.05f;
			}

			
		} else if (result == DIERR_INPUTLOST) {
			g_pinputDevice->Acquire();
		}
		

		worldMat = XMMatrixRotationY(angle) * XMMatrixTranslation(x,y,z);
		//worldMat = XMMatrixTranslation(0,0,0);
		viewMat = XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), XMLoadFloat3(&up));
		mapScene->world = worldMat;
		mapScene->view = viewMat;
		mapScene->proj = projMat;


		//DirectX処理
		//バックバッファのインデックスを取得
		auto bbIdx = _swapchain->GetCurrentBackBufferIndex();

		D3D12_RESOURCE_BARRIER BarrierDesc = {};
		BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		BarrierDesc.Transition.pResource = _backBuffers[bbIdx];
		BarrierDesc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		_cmdList->ResourceBarrier(1, &BarrierDesc);		
		

		//レンダーターゲット＆デプスを指定
		auto rtvH = rtvHeaps->GetCPUDescriptorHandleForHeapStart();
		rtvH.ptr += bbIdx * _dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		auto dsvH = dsvHeap->GetCPUDescriptorHandleForHeapStart();
		_cmdList->ResourceBarrier(1, &BarrierDesc);
		_cmdList->OMSetRenderTargets(1, &rtvH, false, &dsvH);

		//画面クリア

		float clearColor[] = { 0.0f, 0.0f, 1.0f ,1.0f };//白色
		_cmdList->ClearRenderTargetView(rtvH, clearColor, 0, nullptr);
		_cmdList->ClearDepthStencilView(dsvH, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		++frame;
		_cmdList->RSSetViewports(1, &viewport);
		_cmdList->RSSetScissorRects(1, &scissorrect);

		//

		{

			_cmdList->SetPipelineState(_stagePipelinestate);
			_cmdList->SetGraphicsRootSignature(stageRootsignature);
			_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			_cmdList->IASetVertexBuffers(0, 1, &stageVbView);
			_cmdList->IASetIndexBuffer(&stageIbView);

			_cmdList->DrawIndexedInstanced(6, 1, 0, 0, 0);

			BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			_cmdList->ResourceBarrier(1, &BarrierDesc);


			//モデル描画
			_cmdList->SetPipelineState(_pipelinestate);
			auto heapHandle = basicDescHeap->GetGPUDescriptorHandleForHeapStart();
			_cmdList->SetGraphicsRootSignature(rootsignature);
			_cmdList->SetDescriptorHeaps(1, &basicDescHeap);
			_cmdList->SetGraphicsRootDescriptorTable(0, heapHandle);


			_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			_cmdList->IASetVertexBuffers(0, 1, &vbView);
			_cmdList->IASetIndexBuffer(&ibView);

			//マテリアル
			_cmdList->SetDescriptorHeaps(1, &materialDescHeap);

			auto materialH = materialDescHeap->GetGPUDescriptorHandleForHeapStart();
			unsigned int idxOffset = 0;

			auto cbvsrvIncSize = _dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) * 4;

			for (auto& m : materials) {
				_cmdList->SetGraphicsRootDescriptorTable(1, materialH);
				_cmdList->DrawIndexedInstanced(m.indicesNum, 1, idxOffset, 0, 0);

				//ヒープポインタとインデックスを次に進める
				materialH.ptr += cbvsrvIncSize;  //				
				idxOffset += m.indicesNum;

			}

			BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			_cmdList->ResourceBarrier(1, &BarrierDesc);
		}

		//命令のクローズ
		_cmdList->Close();



		//コマンドリストの実行
		ID3D12CommandList* cmdlists[] = { _cmdList };
		_cmdQueue->ExecuteCommandLists(1, cmdlists);
		////待ち
		_cmdQueue->Signal(_fence, ++_fenceVal);

		if (_fence->GetCompletedValue() != _fenceVal) {
			auto event = CreateEvent(nullptr, false, false, nullptr);
			_fence->SetEventOnCompletion(_fenceVal, event);
			WaitForSingleObject(event, INFINITE);
			CloseHandle(event);
		}


		//フリップ
		_swapchain->Present(1, 0);
		_cmdAllocator->Reset();//キューをクリア
		_cmdList->Reset(_cmdAllocator, _pipelinestate);//再びコマンドリストをためる準備

	}
	//もうクラス使わんから登録解除してや

	g_pinputDevice->Unacquire();
	g_pinputDevice->Release();
	g_pInputInterface->Release();
	UnregisterClass(w.lpszClassName, w.hInstance);
	return 0;
}
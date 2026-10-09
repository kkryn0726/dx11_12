//*********************************************************
// main.cpp —— D3D11 最小三角形示例（与 D3D12HelloTriangle 对照）
//
// 与 D3D12HelloTriangle 一一对照的五个核心差异：
//  1. 设备与交换链一步创建：D3D11CreateDeviceAndSwapChain
//     （D3D12 需要 Factory -> Adapter -> Device -> Queue -> SwapChain 分步创建）
//  2. 立即模式渲染：没有 CommandQueue / Allocator / CommandList，
//     每个 Draw 调用直接沿驱动提交路径执行
//  3. 没有 PSO：输入布局、着色器等是独立 state 对象，渲染前分别绑定
//     （D3D12 在创建时把全部状态固化进一个不可变 PSO）
//  4. 没有 ResourceBarrier：运行时对绑定状态做隐式跟踪
//  5. 没有 Fence：Present 内部完成帧同步，CPU 无需等待
//*********************************************************

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

//------ 全局 D3D 对象（教学示例用裸指针，退出时显式 Release；
//------ 对照 D3D12 示例中的 ComPtr：ComPtr 只管理 CPU 引用计数，
//------ GPU 生命周期在 D3D12 中仍需 Fence 自行保证，D3D11 由运行时兜底）-----
static ID3D11Device*           g_device      = nullptr;
static ID3D11DeviceContext*    g_context     = nullptr;   // 对照 D3D12：CommandQueue + CommandList 合体
static IDXGISwapChain*         g_swapChain   = nullptr;
static ID3D11RenderTargetView* g_rtv         = nullptr;   // 对照 D3D12：无需 DescriptorHeap，RTV 直接挂在对象上
static ID3D11Buffer*           g_vertexBuffer = nullptr;
static ID3D11VertexShader*     g_vs          = nullptr;
static ID3D11PixelShader*      g_ps          = nullptr;
static ID3D11InputLayout*      g_inputLayout = nullptr;

struct Vertex
{
    float pos[3];
    float color[4];
};

// 着色器源码直接内嵌字符串，运行时用 D3DCompile 编译（免文件路径依赖）
// 对照 D3D12 示例：同样的 VSMain/PSMain、同样的 vs_5_0/ps_5_0 target
static const char* g_shaderSource = R"(
struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

PSInput VSMain(float4 position : POSITION, float4 color : COLOR)
{
    PSInput result;
    result.position = position;
    result.color    = color;
    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
)";

//--------------------------------------------------------------------------
// 初始化：设备 + 交换链 + 着色器 + 顶点缓冲 + 渲染目标视图
//--------------------------------------------------------------------------
HRESULT InitD3D(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const UINT width  = rc.right - rc.left;
    const UINT height = rc.bottom - rc.top;
    const float aspectRatio = static_cast<float>(width) / static_cast<float>(height);

    // 交换链描述：与 D3D12 示例相同的 FLIP_DISCARD 翻转模型、2 个缓冲
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount       = 2;
    sd.BufferDesc.Width  = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow     = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed         = TRUE;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    // 调试层：对照 D3D12 的 EnableDebugLayer——D3D11 只需创建设备时加一个 flag。
    // 需要"图形工具"可选功能；未安装则回退到非调试设备。
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Feature Level 请求 11.1 / 11.0，实际获得的级别写回 g_featureLevel
    const D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;

    // 一步创建 Device + Context + SwapChain（对照 D3D12 的 LoadPipeline 全过程）
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,                       // 默认适配器（对照 D3D12：手动枚举 Adapter）
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        &sd,
        &g_swapChain,
        &g_device,
        &featureLevel,
        &g_context);

#ifdef _DEBUG
    if (FAILED(hr))
    {
        // 调试层组件（Graphics Tools）未安装时回退，保证程序可运行
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags,
            featureLevels, _countof(featureLevels), D3D11_SDK_VERSION, &sd,
            &g_swapChain, &g_device, &featureLevel, &g_context);
    }
#endif
    if (FAILED(hr))
        return hr;

    // 编译着色器（对照 D3D12 示例的 D3DCompileFromFile，这里是内嵌源码）
    UINT compileFlags = 0;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ID3DBlob* vsBlob  = nullptr;
    ID3DBlob* psBlob  = nullptr;
    ID3DBlob* errBlob = nullptr;

    hr = D3DCompile(g_shaderSource, std::strlen(g_shaderSource), nullptr,
                    nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0,
                    &vsBlob, &errBlob);
    if (FAILED(hr))
    {
        if (errBlob)
        {
            OutputDebugStringA(reinterpret_cast<const char*>(errBlob->GetBufferPointer()));
            errBlob->Release();
        }
        return hr;
    }

    hr = D3DCompile(g_shaderSource, std::strlen(g_shaderSource), nullptr,
                    nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0,
                    &psBlob, &errBlob);
    if (FAILED(hr))
    {
        vsBlob->Release();
        if (errBlob)
        {
            OutputDebugStringA(reinterpret_cast<const char*>(errBlob->GetBufferPointer()));
            errBlob->Release();
        }
        return hr;
    }

    g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps);

    // 输入布局：对照 D3D12 PSO 中的 InputLayout 字段——D3D11 里是独立对象，
    // 由 IA 阶段单独绑定（元素定义与 HelloTriangle 完全一致）
    const D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,   0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g_device->CreateInputLayout(layout, _countof(layout),
                                vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                &g_inputLayout);
    vsBlob->Release();

    // 顶点缓冲：三角形顶点带颜色（与 HelloTriangle 相同的形状与宽高比修正）
    // 对照 D3D12：这里 USAGE_DEFAULT + pSysMem 初始数据一步到位，
    // 相当于 D3D12 里 Default 堆 + 初始拷贝，不需要手动 Upload 堆 + Map/memcpy
    Vertex triangleVertices[] =
    {
        { {  0.0f,  0.25f * aspectRatio, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
        { {  0.25f, -0.25f * aspectRatio, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
        { { -0.25f, -0.25f * aspectRatio, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } },
    };

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth      = sizeof(triangleVertices);
    bd.Usage          = D3D11_USAGE_DEFAULT;
    bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initData = { triangleVertices };
    g_device->CreateBuffer(&bd, &initData, &g_vertexBuffer);

    // 渲染目标视图：对照 D3D12——无需 DescriptorHeap 与句柄偏移，
    // 直接 GetBuffer 拿后台缓冲再 CreateRenderTargetView
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
    backBuffer->Release();

    // 视口：D3D11 中 viewport/scissor 是持久 state，设置一次即可
    // （对照 D3D12：每帧都要在命令列表里 RSSetViewports）
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    g_context->RSSetViewports(1, &vp);

    return S_OK;
}

//--------------------------------------------------------------------------
// 每帧渲染：对照 D3D12 的 PopulateCommandList + Execute + Present + Fence，
// 这里没有任何录制/提交/同步的显式动作
//--------------------------------------------------------------------------
void Render()
{
    const float clearColor[4] = { 0.0f, 0.2f, 0.4f, 1.0f };   // 与 HelloTriangle 同色，方便对照

    g_context->ClearRenderTargetView(g_rtv, clearColor);       // 对照 D3D12：无需先 barrier PRESENT -> RENDER_TARGET
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);         // 对照 D3D12：OMSetRenderTargets(RTV 句柄)
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->IASetInputLayout(g_inputLayout);
    g_context->VSSetShader(g_vs, nullptr, 0);
    g_context->PSSetShader(g_ps, nullptr, 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = g_vertexBuffer;
    g_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

    g_context->Draw(3, 0);                                     // 对照 D3D12：DrawInstanced(3, 1, 0, 0)

    g_swapChain->Present(1, 0);                                // 垂直同步；对照 D3D12：无需 RT -> PRESENT barrier，无 Fence 等待
}

//--------------------------------------------------------------------------
// 释放所有 D3D 对象（对照 D3D12 示例：OnDestroy 前必须 Fence 等待 GPU，
// D3D11 运行时内部引用计数 + 释放时自动同步，无需手动等待）
//--------------------------------------------------------------------------
void Cleanup()
{
    if (g_context)    { g_context->ClearState(); g_context->Release(); }
    if (g_swapChain)  g_swapChain->Release();
    if (g_rtv)        g_rtv->Release();
    if (g_vertexBuffer) g_vertexBuffer->Release();
    if (g_inputLayout) g_inputLayout->Release();
    if (g_vs)         g_vs->Release();
    if (g_ps)         g_ps->Release();
    if (g_device)     g_device->Release();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE,
                    _In_ PWSTR, _In_ int nCmdShow)
{
    // 注册窗口类并创建窗口（与 HelloTriangle 的 Win32Application 等价，精简为一个函数）
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"D3D11TriangleWindowClass";
    RegisterClassEx(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindow(wc.lpszClassName, L"D3D11 Hello Triangle",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             nullptr, nullptr, hInstance, nullptr);
    if (!hwnd)
        return -1;

    if (FAILED(InitD3D(hwnd)))
    {
        Cleanup();
        return -1;
    }

    ShowWindow(hwnd, nCmdShow);

    // 消息循环：无消息时渲染一帧（对照 HelloTriangle 的 Win32Application::Run）
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            Render();
        }
    }

    Cleanup();
    return static_cast<int>(msg.wParam);
}

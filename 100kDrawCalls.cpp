#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <execution>
#include <dxgi1_4.h>
#include <d3d12.h>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "d3dx12.h"
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#define VHR(hr) if (FAILED(hr)) { assert(0); }
#define SAFE_RELEASE(obj) if ((obj)) { (obj)->Release(); (obj) = nullptr; }

#define k_DemoName "100k Draw Calls in Parallel"
#define k_DemoResolutionX 1280
#define k_DemoResolutionY 720

struct Demo
{
    ID3D12Device* device;
    ID3D12CommandQueue* cmdQueue;
    ID3D12CommandAllocator* cmdAlloc[2];
    ID3D12GraphicsCommandList* cmdList;
    IDXGISwapChain3* swapChain;
    ID3D12DescriptorHeap* swapBufferHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE swapBufferHeapStart;
    ID3D12Resource* swapBuffers[4];
    ID3D12Fence* frameFence;
    HANDLE frameFenceEvent;
    HWND window;
    uint32_t descriptorSize;
    uint32_t descriptorSizeRtv;
    uint32_t frameIndex;
    uint32_t backBufferIndex;
    uint64_t frameCount;
    ID3D12PipelineState* pso;
    ID3D12RootSignature* rootSig;
};

// returns [0.0f, 1.0f)
static inline float
Randomf()
{
    const uint32_t exponent = 127;
    const uint32_t significand = (uint32_t)(rand() & 0x7fff); // get 15 random bits
    const uint32_t result = (exponent << 23) | (significand << 8);
    return *(float*)&result - 1.0f;
}

static inline float
Randomf(float begin, float end)
{
    assert(begin < end);
    return begin + (end - begin) * Randomf();
}

static std::vector<uint8_t>
LoadFile(const char* fileName)
{
    FILE* file = fopen(fileName, "rb");
    assert(file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    assert(size != -1);
    std::vector<uint8_t> content(size);
    fseek(file, 0, SEEK_SET);
    fread(&content[0], 1, content.size(), file);
    fclose(file);
    return content;
}

static void
InitializeDx12(Demo& demo)
{
    IDXGIFactory4* factory;
#ifdef _DEBUG
    VHR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)));
#else
    VHR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
#endif

#ifdef _DEBUG
    {
        ID3D12Debug* dbg;
        D3D12GetDebugInterface(IID_PPV_ARGS(&dbg));
        if (dbg)
        {
            dbg->EnableDebugLayer();
            ID3D12Debug1* dbg1;
            dbg->QueryInterface(IID_PPV_ARGS(&dbg1));
            if (dbg1)
                dbg1->SetEnableGPUBasedValidation(TRUE);
            SAFE_RELEASE(dbg);
            SAFE_RELEASE(dbg1);
        }
    }
#endif
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&demo.device))))
    {
        // #TODO: Add MessageBox
        return;
    }

    D3D12_COMMAND_QUEUE_DESC cmdQueueDesc = {};
    cmdQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    cmdQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    cmdQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    VHR(demo.device->CreateCommandQueue(&cmdQueueDesc, IID_PPV_ARGS(&demo.cmdQueue)));

    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 4;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = demo.window;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    swapChainDesc.Windowed = TRUE;

    IDXGISwapChain* tempSwapChain;
    VHR(factory->CreateSwapChain(demo.cmdQueue, &swapChainDesc, &tempSwapChain));
    VHR(tempSwapChain->QueryInterface(IID_PPV_ARGS(&demo.swapChain)));
    SAFE_RELEASE(tempSwapChain);
    SAFE_RELEASE(factory);

    for (uint32_t i = 0; i < 2; ++i)
        VHR(demo.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&demo.cmdAlloc[i])));

    demo.descriptorSize = demo.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    demo.descriptorSizeRtv = demo.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* swap buffers */ {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 4;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        VHR(demo.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&demo.swapBufferHeap)));
        demo.swapBufferHeapStart = demo.swapBufferHeap->GetCPUDescriptorHandleForHeapStart();

        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(demo.swapBufferHeapStart);

        for (uint32_t i = 0; i < 4; ++i)
        {
            VHR(demo.swapChain->GetBuffer(i, IID_PPV_ARGS(&demo.swapBuffers[i])));

            demo.device->CreateRenderTargetView(demo.swapBuffers[i], nullptr, handle);
            handle.Offset(demo.descriptorSizeRtv);
        }
    }

    VHR(demo.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, demo.cmdAlloc[0], nullptr, IID_PPV_ARGS(&demo.cmdList)));
    VHR(demo.cmdList->Close());

    VHR(demo.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&demo.frameFence)));
    demo.frameFenceEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
}

static void
Shutdown(Demo& demo)
{
    SAFE_RELEASE(demo.cmdList);
    SAFE_RELEASE(demo.cmdAlloc[0]);
    SAFE_RELEASE(demo.cmdAlloc[1]);
    SAFE_RELEASE(demo.swapBufferHeap);
    for (int i = 0; i < 4; ++i)
        SAFE_RELEASE(demo.swapBuffers[i]);
    CloseHandle(demo.frameFenceEvent);
    SAFE_RELEASE(demo.frameFence);
    SAFE_RELEASE(demo.swapChain);
    SAFE_RELEASE(demo.cmdQueue);
    SAFE_RELEASE(demo.device);
}

static void
Present(Demo& demo)
{
    demo.swapChain->Present(0, 0);
    demo.cmdQueue->Signal(demo.frameFence, ++demo.frameCount);

    const uint64_t deviceFrameCount = demo.frameFence->GetCompletedValue();

    if ((demo.frameCount - deviceFrameCount) >= 2)
    {
        demo.frameFence->SetEventOnCompletion(deviceFrameCount + 1, demo.frameFenceEvent);
        WaitForSingleObject(demo.frameFenceEvent, INFINITE);
    }

    demo.frameIndex = !demo.frameIndex;
    demo.backBufferIndex = demo.swapChain->GetCurrentBackBufferIndex();
}

static void
Flush(Demo& demo)
{
    demo.cmdQueue->Signal(demo.frameFence, ++demo.frameCount);
    demo.frameFence->SetEventOnCompletion(demo.frameCount, demo.frameFenceEvent);
    WaitForSingleObject(demo.frameFenceEvent, INFINITE);
}

static double
GetTime()
{
    static LARGE_INTEGER startCounter;
    static LARGE_INTEGER frequency;
    if (startCounter.QuadPart == 0)
    {
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&startCounter);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart - startCounter.QuadPart) / (double)frequency.QuadPart;
}

static void
UpdateFrameTime(HWND window, double& o_Time, double& o_DeltaTime)
{
    static double lastTime = -1.0;
    static double lastFpsTime = 0.0;
    static unsigned frameCount = 0;

    if (lastTime < 0.0)
    {
        lastTime = GetTime();
        lastFpsTime = lastTime;
    }

    o_Time = GetTime();
    o_DeltaTime = o_Time - lastTime;
    lastTime = o_Time;

    if ((o_Time - lastFpsTime) >= 1.0)
    {
        const double fps = frameCount / (o_Time - lastFpsTime);
        const double ms = (1.0 / fps) * 1000.0;
        char text[256];
        snprintf(text, sizeof(text), "[%.1f fps  %.3f ms] %s", fps, ms, k_DemoName);
        SetWindowText(window, text);
        lastFpsTime = o_Time;
        frameCount = 0;
    }
    frameCount++;
}

static LRESULT CALLBACK
ProcessWindowMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            PostQuitMessage(0);
            return 0;
        }
        break;
    }
    return DefWindowProc(window, message, wparam, lparam);
}

static void
InitializeWindow(Demo& demo)
{
    WNDCLASS winclass = {};
    winclass.lpfnWndProc = ProcessWindowMessage;
    winclass.hInstance = GetModuleHandle(nullptr);
    winclass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    winclass.lpszClassName = k_DemoName;
    if (!RegisterClass(&winclass))
        assert(0);

    RECT rect = { 0, 0, k_DemoResolutionX, k_DemoResolutionY };
    if (!AdjustWindowRect(&rect, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX, 0))
        assert(0);

    demo.window = CreateWindowEx(
        0, k_DemoName, k_DemoName, WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, nullptr, 0);
    assert(demo.window);
}

static void
Draw(Demo& demo)
{
    ID3D12CommandAllocator* cmdAlloc = demo.cmdAlloc[demo.frameIndex];
    ID3D12GraphicsCommandList* cl = demo.cmdList;

    cmdAlloc->Reset();
    cl->Reset(cmdAlloc, nullptr);

    cl->RSSetViewports(1, &CD3DX12_VIEWPORT(0.0f, 0.0f, (float)k_DemoResolutionX, (float)k_DemoResolutionY));
    cl->RSSetScissorRects(1, &CD3DX12_RECT(0, 0, k_DemoResolutionX, k_DemoResolutionY));

    cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(demo.swapBuffers[demo.backBufferIndex],
                                                                 D3D12_RESOURCE_STATE_PRESENT,
                                                                 D3D12_RESOURCE_STATE_RENDER_TARGET));

    D3D12_CPU_DESCRIPTOR_HANDLE backBufferDescriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(demo.swapBufferHeapStart,
                                                                                     demo.backBufferIndex,
                                                                                     demo.descriptorSizeRtv);
    const float clearColor[4] = { 0.0f, 0.2f, 0.4f, 1.0f };

    cl->OMSetRenderTargets(1, &backBufferDescriptor, 0, nullptr);
    cl->ClearRenderTargetView(backBufferDescriptor, clearColor, 0, nullptr);

    cl->SetPipelineState(demo.pso);
    cl->SetGraphicsRootSignature(demo.rootSig);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

    for (uint32_t i = 0; i < 100000; ++i)
    {
        float p[2] = { Randomf(-0.7f, 0.7f), Randomf(-0.7f, 0.7f) };
        cl->SetGraphicsRoot32BitConstants(0, 2, p, 0);
        cl->DrawInstanced(1, 1, 0, 0);
    }
    cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(demo.swapBuffers[demo.backBufferIndex],
                                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                 D3D12_RESOURCE_STATE_PRESENT));
    VHR(cl->Close());

    demo.cmdQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&cl);
}

static void
Initialize(Demo& demo)
{
    /* pso */ {
        std::vector<uint8_t> vsCode = LoadFile("VsTransform.cso");
        std::vector<uint8_t> psCode = LoadFile("PsShade.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.VS = { vsCode.data(), vsCode.size() };
        psoDesc.PS = { psCode.data(), psCode.size() };
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.SampleMask = 0xffffffff;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        VHR(demo.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&demo.pso)));
        VHR(demo.device->CreateRootSignature(0, vsCode.data(), vsCode.size(), IID_PPV_ARGS(&demo.rootSig)));
    }
}

int CALLBACK
WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SetProcessDPIAware();

    Demo demo = {};
    InitializeWindow(demo);
    InitializeDx12(demo);
    Initialize(demo);

    for (;;)
    {
        MSG msg = {};
        if (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
        {
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                break;
        }
        else
        {
            double time, deltaTime;
            UpdateFrameTime(demo.window, time, deltaTime);
            Draw(demo);
            Present(demo);
        }
    }

    return 0;
}
// vim: set ts=4 sw=4 expandtab:

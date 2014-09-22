#include "precompiled.h"
//
// Copyright (c) 2012-2014 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// SwapChain11.cpp: Implements a back-end specific class for the D3D11 swap chain.

#include "libGLESv2/renderer/d3d/d3d11/SwapChain11.h"

#include "libGLESv2/renderer/d3d/d3d11/renderer11_utils.h"
#include "libGLESv2/renderer/d3d/d3d11/formatutils11.h"
#include "libGLESv2/renderer/d3d/d3d11/Renderer11.h"
#if defined(ANGLE_PLATFORM_WINRT)
#include "common/winrtutils.h"
#include "common/winrtangle.h"
#include "common/winrtangleutils.h"
#include "libGLESv2/renderer/d3d/d3d11/shaders/compiled/winrt/passthrough2d11vs.h"
#include "libGLESv2/renderer/d3d/d3d11/shaders/compiled/winrt/passthroughrgba2d11ps.h"
#if !defined(ANGLE_PLATFORM_WP8)
#include <windows.ui.xaml.media.dxinterop.h>
#endif // #if !defined(ANGLE_PLATFORM_WP8)
using namespace Microsoft::WRL;
using namespace ABI::Windows::UI::Core;
using namespace Windows::Foundation;
#else
#include "libGLESv2/renderer/d3d/d3d11/shaders/compiled/passthrough2d11vs.h"
#include "libGLESv2/renderer/d3d/d3d11/shaders/compiled/passthroughrgba2d11ps.h"
#endif // #if defined(ANGLE_PLATFORM_WINRT)

namespace rx
{

SwapChain11::SwapChain11(Renderer11 *renderer, EGLNativeWindowType window, HANDLE shareHandle,
                         GLenum backBufferFormat, GLenum depthBufferFormat)
    : mRenderer(renderer), SwapChain(window, shareHandle, backBufferFormat, depthBufferFormat)
{
    mSwapChain = NULL;
    mBackBufferTexture = NULL;
    mBackBufferRTView = NULL;
#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    mOffscreenTexture = NULL;
    mOffscreenRTView = NULL;
    mOffscreenSRView = NULL;
#endif
    mDepthStencilTexture = NULL;
    mDepthStencilDSView = NULL;
    mDepthStencilSRView = NULL;
    mQuadVB = NULL;
    mPassThroughSampler = NULL;
    mPassThroughIL = NULL;
    mPassThroughVS = NULL;
    mPassThroughPS = NULL;
    mWidth = -1;
    mHeight = -1;
    mSwapInterval = 0;
    mAppCreatedShareHandle = mShareHandle != NULL;
    mPassThroughResourcesInit = false;
}

SwapChain11::~SwapChain11()
{
    release();
}

void SwapChain11::release()
{
    SafeRelease(mSwapChain);
    SafeRelease(mBackBufferTexture);
    SafeRelease(mBackBufferRTView);
#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    SafeRelease(mOffscreenTexture);
    SafeRelease(mOffscreenRTView);
    SafeRelease(mOffscreenSRView);
#endif
    SafeRelease(mDepthStencilTexture);
    SafeRelease(mDepthStencilDSView);
    SafeRelease(mDepthStencilSRView);
    SafeRelease(mQuadVB);
    SafeRelease(mPassThroughSampler);
    SafeRelease(mPassThroughIL);
    SafeRelease(mPassThroughVS);
    SafeRelease(mPassThroughPS);

    if (!mAppCreatedShareHandle)
    {
        mShareHandle = NULL;
    }
}

void SwapChain11::releaseOffscreenTexture()
{
#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    SafeRelease(mOffscreenTexture);
    SafeRelease(mOffscreenRTView);
    SafeRelease(mOffscreenSRView);
#endif
    SafeRelease(mDepthStencilTexture);
    SafeRelease(mDepthStencilDSView);
    SafeRelease(mDepthStencilSRView);
}

EGLint SwapChain11::resetOffscreenTexture(int backbufferWidth, int backbufferHeight)
{
    ID3D11Device *device = mRenderer->getDevice();

    ASSERT(device != NULL);

    // D3D11 does not allow zero size textures
    ASSERT(backbufferWidth >= 1);
    ASSERT(backbufferHeight >= 1);

    HRESULT result = E_FAIL;

#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    // Preserve the render target content
    ID3D11Texture2D *previousOffscreenTexture = mOffscreenTexture;
    if (previousOffscreenTexture)
    {
        previousOffscreenTexture->AddRef();
    }
    const int previousWidth = mWidth;
    const int previousHeight = mHeight;
#endif

    releaseOffscreenTexture();

#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER

    const d3d11::TextureFormat &backbufferFormatInfo = d3d11::GetTextureFormatInfo(mBackBufferFormat);

    // If the app passed in a share handle, open the resource
    // See EGL_ANGLE_d3d_share_handle_client_buffer
    if (mAppCreatedShareHandle)
    {
        ID3D11Resource *tempResource11;
        HRESULT result = device->OpenSharedResource(mShareHandle, __uuidof(ID3D11Resource), (void**)&tempResource11);

        if (FAILED(result))
        {
            ERR("Failed to open the swap chain pbuffer share handle: %08lX", result);
            release();
            return EGL_BAD_PARAMETER;
        }

        result = tempResource11->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&mOffscreenTexture);
        SafeRelease(tempResource11);

        if (FAILED(result))
        {
            ERR("Failed to query texture2d interface in pbuffer share handle: %08lX", result);
            release();
            return EGL_BAD_PARAMETER;
        }

        // Validate offscreen texture parameters
        D3D11_TEXTURE2D_DESC offscreenTextureDesc = {0};
        mOffscreenTexture->GetDesc(&offscreenTextureDesc);

        if (offscreenTextureDesc.Width != (UINT)backbufferWidth ||
            offscreenTextureDesc.Height != (UINT)backbufferHeight ||
            offscreenTextureDesc.Format != backbufferFormatInfo.texFormat ||
            offscreenTextureDesc.MipLevels != 1 ||
            offscreenTextureDesc.ArraySize != 1)
        {
            ERR("Invalid texture parameters in the shared offscreen texture pbuffer");
            release();
            return EGL_BAD_PARAMETER;
        }
    }
    else
    {
        const bool useSharedResource = !mWindow && mRenderer->getShareHandleSupport();

        D3D11_TEXTURE2D_DESC offscreenTextureDesc = {0};
        offscreenTextureDesc.Width = backbufferWidth;
        offscreenTextureDesc.Height = backbufferHeight;
        offscreenTextureDesc.Format = backbufferFormatInfo.texFormat;
        offscreenTextureDesc.MipLevels = 1;
        offscreenTextureDesc.ArraySize = 1;
        offscreenTextureDesc.SampleDesc.Count = 1;
        offscreenTextureDesc.SampleDesc.Quality = 0;
        offscreenTextureDesc.Usage = D3D11_USAGE_DEFAULT;
        offscreenTextureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        offscreenTextureDesc.CPUAccessFlags = 0;
        offscreenTextureDesc.MiscFlags = useSharedResource ? D3D11_RESOURCE_MISC_SHARED : 0;

        HRESULT result = device->CreateTexture2D(&offscreenTextureDesc, NULL, &mOffscreenTexture);

        if (FAILED(result))
        {
            ERR("Could not create offscreen texture: %08lX", result);
            release();

            if (d3d11::isDeviceLostError(result))
            {
                return EGL_CONTEXT_LOST;
            }
            else
            {
                return EGL_BAD_ALLOC;
            }
        }

        d3d11::SetDebugName(mOffscreenTexture, "Offscreen back buffer texture");

        // EGL_ANGLE_surface_d3d_texture_2d_share_handle requires that we store a share handle for the client
        if (useSharedResource)
        {
            IDXGIResource *offscreenTextureResource = NULL;
            result = mOffscreenTexture->QueryInterface(__uuidof(IDXGIResource), (void**)&offscreenTextureResource);

            // Fall back to no share handle on failure
            if (FAILED(result))
            {
                ERR("Could not query offscreen texture resource: %08lX", result);
            }
            else
            {
                result = offscreenTextureResource->GetSharedHandle(&mShareHandle);
                SafeRelease(offscreenTextureResource);

                if (FAILED(result))
                {
                    mShareHandle = NULL;
                    ERR("Could not get offscreen texture shared handle: %08lX", result);
                }
            }
        }
    }


    D3D11_RENDER_TARGET_VIEW_DESC offscreenRTVDesc;
    offscreenRTVDesc.Format = backbufferFormatInfo.rtvFormat;
    offscreenRTVDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    offscreenRTVDesc.Texture2D.MipSlice = 0;

    result = device->CreateRenderTargetView(mOffscreenTexture, &offscreenRTVDesc, &mOffscreenRTView);
    ASSERT(SUCCEEDED(result));
    d3d11::SetDebugName(mOffscreenRTView, "Offscreen back buffer render target");

    D3D11_SHADER_RESOURCE_VIEW_DESC offscreenSRVDesc;
    offscreenSRVDesc.Format = backbufferFormatInfo.srvFormat;
    offscreenSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    offscreenSRVDesc.Texture2D.MostDetailedMip = 0;
    offscreenSRVDesc.Texture2D.MipLevels = -1;

    result = device->CreateShaderResourceView(mOffscreenTexture, &offscreenSRVDesc, &mOffscreenSRView);
    ASSERT(SUCCEEDED(result));
    d3d11::SetDebugName(mOffscreenSRView, "Offscreen back buffer shader resource");

#endif

    const d3d11::TextureFormat &depthBufferFormatInfo = d3d11::GetTextureFormatInfo(mDepthBufferFormat);

    if (mDepthBufferFormat != GL_NONE)
    {
        D3D11_TEXTURE2D_DESC depthStencilTextureDesc;
        depthStencilTextureDesc.Width = backbufferWidth;
        depthStencilTextureDesc.Height = backbufferHeight;
        depthStencilTextureDesc.Format = depthBufferFormatInfo.texFormat;
        depthStencilTextureDesc.MipLevels = 1;
        depthStencilTextureDesc.ArraySize = 1;
        depthStencilTextureDesc.SampleDesc.Count = 1;
        depthStencilTextureDesc.SampleDesc.Quality = 0;
        depthStencilTextureDesc.Usage = D3D11_USAGE_DEFAULT;
        depthStencilTextureDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        depthStencilTextureDesc.CPUAccessFlags = 0;
        depthStencilTextureDesc.MiscFlags = 0;

        result = device->CreateTexture2D(&depthStencilTextureDesc, NULL, &mDepthStencilTexture);
        if (FAILED(result))
        {
            ERR("Could not create depthstencil surface for new swap chain: 0x%08X", result);
            release();

            if (d3d11::isDeviceLostError(result))
            {
                return EGL_CONTEXT_LOST;
            }
            else
            {
                return EGL_BAD_ALLOC;
            }
        }
        d3d11::SetDebugName(mDepthStencilTexture, "Offscreen depth stencil texture");

        D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilDesc;
        depthStencilDesc.Format = depthBufferFormatInfo.dsvFormat;
        depthStencilDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        depthStencilDesc.Flags = 0;
        depthStencilDesc.Texture2D.MipSlice = 0;

        result = device->CreateDepthStencilView(mDepthStencilTexture, &depthStencilDesc, &mDepthStencilDSView);
        ASSERT(SUCCEEDED(result));
        d3d11::SetDebugName(mDepthStencilDSView, "Offscreen depth stencil view");

        D3D11_SHADER_RESOURCE_VIEW_DESC depthStencilSRVDesc;
        depthStencilSRVDesc.Format = depthBufferFormatInfo.srvFormat;
        depthStencilSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        depthStencilSRVDesc.Texture2D.MostDetailedMip = 0;
        depthStencilSRVDesc.Texture2D.MipLevels = -1;

        result = device->CreateShaderResourceView(mDepthStencilTexture, &depthStencilSRVDesc, &mDepthStencilSRView);
        ASSERT(SUCCEEDED(result));
        d3d11::SetDebugName(mDepthStencilSRView, "Offscreen depth stencil shader resource");
    }

    mWidth = backbufferWidth;
    mHeight = backbufferHeight;

#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    if (previousOffscreenTexture != NULL)
    {
        D3D11_BOX sourceBox = {0};
        sourceBox.left = 0;
        sourceBox.right = std::min(previousWidth, mWidth);
        sourceBox.top = std::max(previousHeight - mHeight, 0);
        sourceBox.bottom = previousHeight;
        sourceBox.front = 0;
        sourceBox.back = 1;

        ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
        const int yoffset = std::max(mHeight - previousHeight, 0);
        deviceContext->CopySubresourceRegion(mOffscreenTexture, 0, 0, yoffset, 0, previousOffscreenTexture, 0, &sourceBox);

        SafeRelease(previousOffscreenTexture);

        if (mSwapChain)
        {
            swapRect(0, 0, mWidth, mHeight);
        }
    }

#endif

    return EGL_SUCCESS;
}

EGLint SwapChain11::resize(EGLint backbufferWidth, EGLint backbufferHeight)
{
    ID3D11Device *device = mRenderer->getDevice();

    if (device == NULL)
    {
        return EGL_BAD_ACCESS;
    }

    // EGL allows creating a surface with 0x0 dimension, however, DXGI does not like 0x0 swapchains
    if (backbufferWidth < 1 || backbufferHeight < 1)
    {
        return EGL_SUCCESS;
    }

    // Can only call resize if we have already created our swap buffer and resources
    ASSERT(mSwapChain && mBackBufferTexture && mBackBufferRTView);

    SafeRelease(mBackBufferTexture);
    SafeRelease(mBackBufferRTView);

    // Resize swap chain
    DXGI_SWAP_CHAIN_DESC desc;
    mSwapChain->GetDesc(&desc);
    const d3d11::TextureFormat &backbufferFormatInfo = d3d11::GetTextureFormatInfo(mBackBufferFormat);
    HRESULT result = mSwapChain->ResizeBuffers(desc.BufferCount, backbufferWidth, backbufferHeight, backbufferFormatInfo.texFormat, 0);

    if (FAILED(result))
    {
        ERR("Error resizing swap chain buffers: 0x%08X", result);
        release();

        if (d3d11::isDeviceLostError(result))
        {
            return EGL_CONTEXT_LOST;
        }
        else
        {
            return EGL_BAD_ALLOC;
        }
    }

#if defined(ANGLE_PLATFORM_WP8)
    ComPtr<IWinrtEglWindow> iWinRTWindow;
    result = winrtangleutils::getIWinRTWindow(mWindow, &iWinRTWindow);
    if(SUCCEEDED(result)) 
    {
        if(winrtangleutils::hasIPhoneXamlWindow(iWinRTWindow))
        {
            ComPtr<IWinPhone8XamlD3DWindow> iPhoneWindow;
            result = winrtangleutils::getIPhoneXamlWindow(mWindow, &iPhoneWindow);
            ASSERT(SUCCEEDED(result));
                
            ComPtr<ID3D11Texture2D> backBuffer;
            result = winrtangleutils::getBackBuffer(iPhoneWindow, &backBuffer);
            ASSERT(SUCCEEDED(result));
            mBackBufferTexture = backBuffer.Get();
            mBackBufferTexture->AddRef();
            d3d11::SetDebugName(mBackBufferTexture, "Back buffer texture");

            ComPtr<ID3D11RenderTargetView> rtv;
            result = winrtangleutils::getID3D11RenderTargetView(iPhoneWindow, &rtv);
            ASSERT(SUCCEEDED(result));
            mBackBufferRTView = rtv.Get();
            mBackBufferRTView->AddRef();
            d3d11::SetDebugName(mBackBufferRTView, "Back buffer render target");
        }
    }
#endif

    result = mSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&mBackBufferTexture);
    ASSERT(SUCCEEDED(result));
    if (SUCCEEDED(result))
    {
        d3d11::SetDebugName(mBackBufferTexture, "Back buffer texture");
    }

    result = device->CreateRenderTargetView(mBackBufferTexture, NULL, &mBackBufferRTView);
    ASSERT(SUCCEEDED(result));
    if (SUCCEEDED(result))
    {
        d3d11::SetDebugName(mBackBufferRTView, "Back buffer render target");
    }

    return resetOffscreenTexture(backbufferWidth, backbufferHeight);
}

EGLint SwapChain11::reset(int backbufferWidth, int backbufferHeight, EGLint swapInterval)
{
    ID3D11Device *device = mRenderer->getDevice();

    if (device == NULL)
    {
        return EGL_BAD_ACCESS;
    }

    // Release specific resources to free up memory for the new render target, while the
    // old render target still exists for the purpose of preserving its contents.
    SafeRelease(mSwapChain);
    SafeRelease(mBackBufferTexture);
    SafeRelease(mBackBufferRTView);

    mSwapInterval = static_cast<unsigned int>(swapInterval);
    if (mSwapInterval > 4)
    {
        // IDXGISwapChain::Present documentation states that valid sync intervals are in the [0,4] range
        return EGL_BAD_PARAMETER;
    }

    // EGL allows creating a surface with 0x0 dimension, however, DXGI does not like 0x0 swapchains
    if (backbufferWidth < 1 || backbufferHeight < 1)
    {
        releaseOffscreenTexture();
        return EGL_SUCCESS;
    }

    if (mWindow)
    {
#if !defined(ANGLE_PLATFORM_WINRT)
        const d3d11::TextureFormat &backbufferFormatInfo = d3d11::GetTextureFormatInfo(mBackBufferFormat);

        IDXGIFactory *factory = mRenderer->getDxgiFactory();

        DXGI_SWAP_CHAIN_DESC swapChainDesc = {0};
        swapChainDesc.BufferDesc.Width = backbufferWidth;
        swapChainDesc.BufferDesc.Height = backbufferHeight;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.BufferDesc.Format = backbufferFormatInfo.texFormat;
        swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = 1;
        swapChainDesc.OutputWindow = mWindow;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swapChainDesc.Flags = 0;

        HRESULT result = factory->CreateSwapChain(device, &swapChainDesc, &mSwapChain);

#elif defined(ANGLE_PLATFORM_WP8)
        ComPtr<IWinrtEglWindow> iWinRTWindow;
        HRESULT result = winrtangleutils::getIWinRTWindow(mWindow, &iWinRTWindow);
        if(SUCCEEDED(result)) 
        {
            if(winrtangleutils::hasIPhoneXamlWindow(iWinRTWindow))
            {
                ComPtr<IWinPhone8XamlD3DWindow> iPhoneWindow;
                result = winrtangleutils::getIPhoneXamlWindow(mWindow, &iPhoneWindow);
                ASSERT(SUCCEEDED(result));
                
                ComPtr<ID3D11Texture2D> backBuffer;
                result = winrtangleutils::getBackBuffer(iPhoneWindow, &backBuffer);
                ASSERT(SUCCEEDED(result));
                mBackBufferTexture = backBuffer.Get();
                mBackBufferTexture->AddRef();

                ComPtr<ID3D11RenderTargetView> rtv;
                result = winrtangleutils::getID3D11RenderTargetView(iPhoneWindow, &rtv);
                ASSERT(SUCCEEDED(result));
                mBackBufferRTView = rtv.Get();
                mBackBufferRTView->AddRef();
            }
            else
            {
                IUnknown* iWindow = iWinRTWindow->GetWindowInterface().Get();
                IDXGIFactory2 *factory = mRenderer->getDxgiFactory();
                DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {0};
                swapChainDesc.Width = static_cast<UINT>(backbufferWidth); // Match the size of the window.
                swapChainDesc.Height = static_cast<UINT>(backbufferHeight);
                swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // This is the most common swap chain format.
                swapChainDesc.Stereo = false;
                swapChainDesc.SampleDesc.Count = 1; // Don't use multi-sampling.
                swapChainDesc.SampleDesc.Quality = 0;
                swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                swapChainDesc.BufferCount = 1; // On phone, only single buffering is supported.
                swapChainDesc.Scaling = DXGI_SCALING_STRETCH; // On phone, only stretch and aspect-ratio stretch scaling are allowed.
                swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; // On phone, no swap effects are supported.
                swapChainDesc.Flags = 0;

                result = factory->CreateSwapChainForCoreWindow(
                    device,
                    iWindow,
                    &swapChainDesc,
                    nullptr, // Allow on all displays.
                    &mSwapChain
                    );
            }
        }
#elif defined(ANGLE_PLATFORM_WINRT)
        ComPtr<IWinrtEglWindow> iWinRTWindow;
        HRESULT result = mWindow.As(&iWinRTWindow);
        if(SUCCEEDED(result))
        {
            IUnknown* iWindow = iWinRTWindow->GetWindowInterface().Get();
            bool isPanel = winrt::isSwapChainBackgroundPanel(iWindow);
            IDXGIFactory2 *factory = mRenderer->getDxgiFactory();
            DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {0};
            swapChainDesc.Width = backbufferWidth;
            swapChainDesc.Height = backbufferHeight;
            swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            swapChainDesc.Stereo = FALSE;
            swapChainDesc.SampleDesc.Count = 1;
            swapChainDesc.SampleDesc.Quality = 0;
            swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapChainDesc.BufferCount = 2;
            swapChainDesc.Flags = 0;
            swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; //must be used for winrt
            swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
            swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

            if (isPanel)
            {
                ComPtr<ISwapChainBackgroundPanelNative> panelNative;
                result = iWinRTWindow->GetWindowInterface().As(&panelNative);
                if SUCCEEDED(result)
                {
                    result = factory->CreateSwapChainForComposition(device, &swapChainDesc, nullptr, &mSwapChain);
                    if SUCCEEDED(result)
                    {
                        panelNative->SetSwapChain(mSwapChain);
                    }
                }
#if (NTDDI_VERSION >= NTDDI_WINBLUE)
                else
                {
                    ComPtr<ISwapChainPanelNative> panelNative;
                    result = iWinRTWindow->GetWindowInterface().As(&panelNative);
                    if SUCCEEDED(result)
                    {
                        result = factory->CreateSwapChainForComposition(device, &swapChainDesc, nullptr, &mSwapChain);
                        if SUCCEEDED(result)
                        {
                            panelNative->SetSwapChain(mSwapChain);
                        }
                    }

                }
#endif
                if SUCCEEDED(result)
                {
                    //mSwapChain->SetRotation(DXGI_MODE_ROTATION_ROTATE90);
                }

            }
            else
            {
                result = factory->CreateSwapChainForCoreWindow(device, iWindow, &swapChainDesc, nullptr, &mSwapChain);

                // TODO: fix this!!!
                //mSwapChain->SetRotation(DXGI_MODE_ROTATION_ROTATE270);
            }
            if(mSwapChain)
                iWinRTWindow->SetAngleSwapChain(mSwapChain);
        }
#endif // #if !defined(ANGLE_PLATFORM_WINRT)

        if (FAILED(result))
        {
            ERR("Could not create additional swap chains or offscreen surfaces: %08lX", result);
            release();

            if (d3d11::isDeviceLostError(result))
            {
                return EGL_CONTEXT_LOST;
            }
            else
            {
                return EGL_BAD_ALLOC;
            }
        }

        if(mSwapChain)
        {
            result = mSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&mBackBufferTexture);
            ASSERT(SUCCEEDED(result));
            d3d11::SetDebugName(mBackBufferTexture, "Back buffer texture");

            result = device->CreateRenderTargetView(mBackBufferTexture, NULL, &mBackBufferRTView);
            ASSERT(SUCCEEDED(result));
            d3d11::SetDebugName(mBackBufferRTView, "Back buffer render target");
        }
    }

    // If we are resizing the swap chain, we don't wish to recreate all the static resources
    if (!mPassThroughResourcesInit)
    {
        mPassThroughResourcesInit = true;
        initPassThroughResources();
    }

    return resetOffscreenTexture(backbufferWidth, backbufferHeight);
}

void SwapChain11::initPassThroughResources()
{
#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    ID3D11Device *device = mRenderer->getDevice();

    ASSERT(device != NULL);

    // Make sure our resources are all not allocated, when we create
    ASSERT(mQuadVB == NULL && mPassThroughSampler == NULL);
    ASSERT(mPassThroughIL == NULL && mPassThroughVS == NULL && mPassThroughPS == NULL);

    D3D11_BUFFER_DESC vbDesc;
    vbDesc.ByteWidth = sizeof(d3d11::PositionTexCoordVertex) * 4;
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vbDesc.MiscFlags = 0;
    vbDesc.StructureByteStride = 0;

    HRESULT result = device->CreateBuffer(&vbDesc, NULL, &mQuadVB);
    ASSERT(SUCCEEDED(result));
    d3d11::SetDebugName(mQuadVB, "Swap chain quad vertex buffer");

    D3D11_SAMPLER_DESC samplerDesc;
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 0;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 0.0f;
    samplerDesc.BorderColor[1] = 0.0f;
    samplerDesc.BorderColor[2] = 0.0f;
    samplerDesc.BorderColor[3] = 0.0f;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    result = device->CreateSamplerState(&samplerDesc, &mPassThroughSampler);
    ASSERT(SUCCEEDED(result));
    d3d11::SetDebugName(mPassThroughSampler, "Swap chain pass through sampler");

    D3D11_INPUT_ELEMENT_DESC quadLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    result = device->CreateInputLayout(quadLayout, 2, g_VS_Passthrough2D, sizeof(g_VS_Passthrough2D), &mPassThroughIL);
    ASSERT(SUCCEEDED(result));
    d3d11::SetDebugName(mPassThroughIL, "Swap chain pass through layout");

    result = device->CreateVertexShader(g_VS_Passthrough2D, sizeof(g_VS_Passthrough2D), NULL, &mPassThroughVS);
    ASSERT(SUCCEEDED(result));
    d3d11::SetDebugName(mPassThroughVS, "Swap chain pass through vertex shader");

    result = device->CreatePixelShader(g_PS_PassthroughRGBA2D, sizeof(g_PS_PassthroughRGBA2D), NULL, &mPassThroughPS);
    ASSERT(SUCCEEDED(result));
    d3d11::SetDebugName(mPassThroughPS, "Swap chain pass through pixel shader");
#endif
}

// parameters should be validated/clamped by caller
EGLint SwapChain11::swapRect(EGLint x, EGLint y, EGLint width, EGLint height)
{
    HRESULT result = S_OK;

#if defined(ANGLE_PLATFORM_WP8)
    ComPtr<IWinrtEglWindow> iWinRTWindow;
    result = winrtangleutils::getIWinRTWindow(mWindow, &iWinRTWindow);
    if(SUCCEEDED(result)) 
    {
        if(winrtangleutils::hasIPhoneXamlWindow(iWinRTWindow))
        {
            ComPtr<IWinPhone8XamlD3DWindow> iPhoneWindow;
            result = winrtangleutils::getIPhoneXamlWindow(mWindow, &iPhoneWindow);
            ASSERT(SUCCEEDED(result));
                
            ComPtr<ID3D11Texture2D> backBuffer;
            result = winrtangleutils::getBackBuffer(iPhoneWindow, &backBuffer);
            ASSERT(SUCCEEDED(result));
            if(backBuffer.Get() != mBackBufferTexture)
            {
                mBackBufferTexture = backBuffer.Get();
                d3d11::SetDebugName(mBackBufferTexture, "Back buffer texture");

                ComPtr<ID3D11RenderTargetView> rtv;
                result = winrtangleutils::getID3D11RenderTargetView(iPhoneWindow, &rtv);
                ASSERT(SUCCEEDED(result));
                mBackBufferRTView = rtv.Get();
                d3d11::SetDebugName(mBackBufferRTView, "Back buffer render target");
            }
        }
    }
#else

    if (!mSwapChain)
    {
        return EGL_SUCCESS;
    }  
#endif

#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    // When rendering directly to the backbuffer, we must swap the whole buffer.
    if (!(x == 0 && y == 0 && width == mWidth && height == mHeight))
    {
        ERR("When ANGLE_ENABLE_RENDER_TO_BACK_BUFFER is defined, swapRect can only be called on the entire backbuffer.");
        ASSERT(false);
        return EGL_FALSE;
    }
#endif

    ID3D11Device *device = mRenderer->getDevice();
    ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

#ifndef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    // Set vertices
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    result = deviceContext->Map(mQuadVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (FAILED(result))
    {
        return EGL_BAD_ACCESS;
    }

    d3d11::PositionTexCoordVertex *vertices = static_cast<d3d11::PositionTexCoordVertex*>(mappedResource.pData);

    // Create a quad in homogeneous coordinates
    float x1 = (x / float(mWidth)) * 2.0f - 1.0f;
    float y1 = (y / float(mHeight)) * 2.0f - 1.0f;
    float x2 = ((x + width) / float(mWidth)) * 2.0f - 1.0f;
    float y2 = ((y + height) / float(mHeight)) * 2.0f - 1.0f;

    float u1 = x / float(mWidth);
    float v1 = y / float(mHeight);
    float u2 = (x + width) / float(mWidth);
    float v2 = (y + height) / float(mHeight);

    d3d11::SetPositionTexCoordVertex(&vertices[0], x1, y1, u1, v1);
    d3d11::SetPositionTexCoordVertex(&vertices[1], x1, y2, u1, v2);
    d3d11::SetPositionTexCoordVertex(&vertices[2], x2, y1, u2, v1);
    d3d11::SetPositionTexCoordVertex(&vertices[3], x2, y2, u2, v2);

    deviceContext->Unmap(mQuadVB, 0);

    static UINT stride = sizeof(d3d11::PositionTexCoordVertex);
    static UINT startIdx = 0;
    deviceContext->IASetVertexBuffers(0, 1, &mQuadVB, &stride, &startIdx);

    // Apply state
    deviceContext->OMSetDepthStencilState(NULL, 0xFFFFFFFF);

    static const float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    deviceContext->OMSetBlendState(NULL, blendFactor, 0xFFFFFFF);

    deviceContext->RSSetState(NULL);

    // Apply shaders
    deviceContext->IASetInputLayout(mPassThroughIL);
    deviceContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    deviceContext->VSSetShader(mPassThroughVS, NULL, 0);
    deviceContext->PSSetShader(mPassThroughPS, NULL, 0);
    deviceContext->GSSetShader(NULL, NULL, 0);

    // Apply render targets
    mRenderer->setOneTimeRenderTarget(mBackBufferRTView);

    // Set the viewport
    D3D11_VIEWPORT viewport;
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = mWidth;
    viewport.Height = mHeight;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    deviceContext->RSSetViewports(1, &viewport);

    // Apply textures
    deviceContext->PSSetShaderResources(0, 1, &mOffscreenSRView);
    deviceContext->PSSetSamplers(0, 1, &mPassThroughSampler);

    // Draw
    deviceContext->Draw(4, 0);

#endif

    if(mSwapChain)
#ifdef ANGLE_FORCE_VSYNC_OFF
        result = mSwapChain->Present(0, 0);
#else
        result = mSwapChain->Present(mSwapInterval, 0);
#endif

    if (result == DXGI_ERROR_DEVICE_REMOVED)
    {
        HRESULT removedReason = device->GetDeviceRemovedReason();
        UNUSED_TRACE_VARIABLE(removedReason);
        ERR("Present failed: the D3D11 device was removed: 0x%08X", removedReason);
        return EGL_CONTEXT_LOST;
    }
    else if (result == DXGI_ERROR_DEVICE_RESET)
    {
        ERR("Present failed: the D3D11 device was reset from a bad command.");
        return EGL_CONTEXT_LOST;
    }
    else if (FAILED(result))
    {
        ERR("Present failed with error code 0x%08X", result);
    }

    // Unbind
    static ID3D11ShaderResourceView *const nullSRV = NULL;
    deviceContext->PSSetShaderResources(0, 1, &nullSRV);

    mRenderer->unapplyRenderTargets();
    mRenderer->markAllStateDirty();

    return EGL_SUCCESS;
}

ID3D11Texture2D *SwapChain11::getOffscreenTexture()
{
#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    return mBackBufferTexture;
#else
    return mOffscreenTexture;
#endif
}

ID3D11RenderTargetView *SwapChain11::getRenderTarget()
{
#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    return mBackBufferRTView;
#else
    return mOffscreenRTView;
#endif
}

ID3D11ShaderResourceView *SwapChain11::getRenderTargetShaderResource()
{
#ifdef ANGLE_ENABLE_RENDER_TO_BACK_BUFFER
    return NULL;
#else
    return mOffscreenSRView;
#endif
}

ID3D11DepthStencilView *SwapChain11::getDepthStencil()
{
    return mDepthStencilDSView;
}

ID3D11ShaderResourceView * SwapChain11::getDepthStencilShaderResource()
{
    return mDepthStencilSRView;
}

ID3D11Texture2D *SwapChain11::getDepthStencilTexture()
{
    return mDepthStencilTexture;
}

SwapChain11 *SwapChain11::makeSwapChain11(SwapChain *swapChain)
{
    ASSERT(HAS_DYNAMIC_TYPE(rx::SwapChain11*, swapChain));
    return static_cast<rx::SwapChain11*>(swapChain);
}

void SwapChain11::recreate()
{
    // possibly should use this method instead of reset
}

}

#pragma once
#include <RHICore/Resource.hpp>
#include <Core/ThreadPool.hpp>
namespace Foundation::RenderCore
{
    class StreamingPool
    {
        ThreadPool mThreadPool;
    };
    class StreamingTexture
    {
        RHIDeviceScopedObjectHandle<RHITexture> mTexture;
    };
}
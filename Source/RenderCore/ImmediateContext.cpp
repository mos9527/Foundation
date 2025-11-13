namespace Foundation::RenderCore
{
    ImmediateContext::ImmediateContext(RHIDeviceQueueType type, RHIDevice* device) : mDevice(device)
    {
        mQueue = device->GetDeviceQueue(type);
        mCommandPool = device->CreateCommandPool({.queue = mQueue, .type = RHICommandPoolType::Persistent});
        mCommandList = mCommandPool->CreateCommandList();
    }
    void ImmediateContext::Submit(RHIDeviceFence* completionFence)
    {
        mQueue->Submit({{{.cmdLists = {{mCommandList.Get()}}}}}, completionFence);
    }
} // namespace Foundation::RenderCore

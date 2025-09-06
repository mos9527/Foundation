[shader("compute")]
[numthreads(64, 32, 1)]
void csMain(uint2 dispatchThreadId : SV_DispatchThreadID)
{
}

[shader("compute")]
[numthreads(32, 16, 1)]
void csMain2(uint2 dispatchThreadId: SV_DispatchThreadID)
{
}

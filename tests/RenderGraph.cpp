#include <Core/Allocator/DefaultAllocator.hpp>
#include <Core/Platform/Logging.hpp>
#include <Renderer/RenderGraph/Resource.hpp>
using namespace Foundation::Core;
using namespace Foundation::Renderer;
DefaultAllocator g_Allocator;
void test_storage_lifetime() {
    struct TestResource {
        int value;
        TestResource() {}
        TestResource(int v) : value(v) {
            LOG_DEBUG(RenderGraph, info, "TestResource created with value: {}", value);
        }        
        ~TestResource() {
            LOG_DEBUG(RenderGraph, info, "TestResource destroyed with value: {}", value);
        }
    };
    RGResourceStorage<TestResource> storage(&g_Allocator);
    auto producer_lambda = [&storage](int value) {
        auto handle = storage.Emplace(value);
        return handle;
    };
    auto res = producer_lambda(42);    
    auto consumer_lambda = [](auto handle) {
        LOG_DEBUG(RenderGraph, info, "Consumer received handle with value: {}", handle.GetResource().value);
    };
    consumer_lambda(res);
}
#include <spdlog/sinks/stdout_color_sinks.h>
int main() {
    GetLoggingSink()->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    ///
    test_storage_lifetime();
    StlVector<int> vec(&g_Allocator);
    return 0;
}

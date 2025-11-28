#pragma once
#include <RHICore/Common.hpp>
#include <Core/Allocator.hpp>
namespace Foundation::RenderCore {
    using namespace Core;
    /**
     * @brief Runtime reflection data for a shader module.
     */
    class Shader {
        Allocator* mAllocator;        
        /**
         * @brief Parse SPIR-V shader code and populate reflection data.        
         * 
         * See also:
         *     https://github.com/zeux/niagara/blob/master/src/shaders.cpp
         *     https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.pdf
         *     https://www.khronos.org/spirv/visualizer/
         *     https://shader-slang.org/slang-playground/
         */
        void ParseSPIRV(Span<const char> bytecode);
        void Sort();
    public:
        struct Entrypoint {
            String name;
            RHIShaderStage stage{};
            // Applies to Compute, Task and Mesh shaders
            Tuple<uint32_t, uint32_t, uint32_t> groupLocalSize{};
        };
        Vector<Entrypoint> mEntrypoints;
        struct Binding {
            String name;
            uint32_t descriptorSet;
            uint32_t binding;
        };
        Vector<Binding> mBindings;
        struct PushConstant {
            String name;
            // TODO: add size, offset, type info?
            // This would require us to parse all OpType.. instructions however.
            // Caller is also expected to know the layout of push constants - TODO for now.                      
        };
        Vector<PushConstant> mPushConstants;
        struct SpecializationConstant
        {
            uint32_t id;
            String name;
        };
        Vector<SpecializationConstant> mSpecializationConstants;
        Shader(Span<const char> bytecode, Allocator* alloc);

        [[nodiscard]] String DbgDumpShaderInfo() const;
    };
}

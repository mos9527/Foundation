#pragma once
#include <Core/Core.hpp>
#include <RHICore/Common.hpp>

namespace Foundation::Rendering {
    class ShaderReflection {
        Core::Allocator* m_allocator;        
        /**
         * @brief Parse SPIR-V shader code and populate reflection data.        
         * References:
         *     https://github.com/zeux/niagara/blob/master/src/shaders.cpp
         *     https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.pdf
         *     https://www.khronos.org/spirv/visualizer/
         *     https://shader-slang.org/slang-playground/
         */
        void ParseSPIRV(Core::StlSpan<const char> bytecode);
        void Sort();
    public:
        struct Entrypoint {
            std::string name;
            RHI::RHIShaderStage stage{};
            // Compute shader specific
            std::tuple<uint32_t, uint32_t, uint32_t> local_size{};
        };
        Core::StlVector<Entrypoint> m_entrypoints;        
        struct Binding {
            std::string name;
            uint32_t descriptorSet;
            uint32_t binding;
        };
        Core::StlVector<Binding> m_bindings;
        struct PushConstant {
            std::string name;
            // TODO: add size, offset, type info?
            // This would require us to parse all OpType.. instructions however.
            // Caller is also expected to know the layout of push constants - TODO for now.                      
        };
        Core::StlVector<PushConstant> m_pushConstants;
        ShaderReflection(Core::StlSpan<const char> bytecode, Core::Allocator* alloc);

        std::string DbgDumpShaderInfo() const;
    };
}

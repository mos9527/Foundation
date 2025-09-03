#pragma once
#include <Core/Container/Common.hpp>
#include <Core/Allocator/Allocator.hpp>

#include <RHICore/Common.hpp>
namespace Foundation {
    class ShaderReflection {
        Core::Allocator* m_allocator;        
        /// <summary>
        /// Parse SPIR-V shader code and populate reflection data.        
        /// References:
        ///     https://github.com/zeux/niagara/blob/master/src/shaders.cpp
        ///     https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.pdf
        ///     https://www.khronos.org/spirv/visualizer/
        ///     https://shader-slang.org/slang-playground/
        /// </summary>        
        void ParseSPIRV(Core::StlSpan<const char> bytecode);
        void Sort();
    public:
        struct Entrypoint {
            std::string name;
            RHI::RHIShaderStage stage{};
        };
        Entrypoint m_entrypoint;        
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

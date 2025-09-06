#include "ShaderReflection.hpp"
#include <spirv/unified1/spirv.hpp>
using namespace Foundation::Rendering;
using namespace Foundation::Core;
void ShaderReflection::ParseSPIRV(StlSpan<const char> bytecode)
{
    /* 2.3 Physical Layout of a SPIR-V Module and Instruction */
    // SPIRV shader bytecodes are always 32-bits words.    
    // NOTE: Pointer aligment can be tricky here - though with STL allocators
    // every standard types are aligned to at least alignof(void*)
    // So it's generally fine to cast like this.
    const uint32_t* code = reinterpret_cast<const uint32_t*>(bytecode.data());
    const uint32_t* end  = reinterpret_cast<const uint32_t*>(bytecode.data() + bytecode.size());
    // 0: Magic Number
    CHECK(code[0] == spv::MagicNumber);
    // 1: Version
    // 2: Generator Magic
    // 3: Bound
    uint32_t Bound = code[3];
    struct Element {
        uint32_t Opcode;
        uint32_t StorageClass;
        uint32_t Type;
        uint32_t DescriptorSet;
        uint32_t Binding;
        /* -- debug -- */
        std::string Name;
        uint32_t EpIndex;
    };
    StlVector<Element> ID(Bound, m_allocator);
    // 4: Reserved
    // 5: First Instruction
    const uint32_t* ins = code + 5;
    while (ins < end) {
        uint16_t WordCount = ins[0] >> 16;
        CHECK(WordCount && "malformed SPIRV shader! (WordCount=0)");
        uint16_t Opcode = ins[0] & 0xFFFF;
        switch (Opcode)
        {
        /* 3.32.5 Mode-Setting Instructions */
        case spv::OpEntryPoint:
        {
            Entrypoint ep;
            // 1: Execution Model
            switch (spv::ExecutionModel(ins[1]))
            {
            case spv::ExecutionModelVertex:
                ep.stage = RHI::RHIShaderStageBits::Vertex; break;
            case spv::ExecutionModelFragment:
                ep.stage = RHI::RHIShaderStageBits::Fragment; break;
            case spv::ExecutionModelGLCompute:
                ep.stage = RHI::RHIShaderStageBits::Compute; break;
            }
            // 2: Entry Point OpFunction ID
            ID[ins[2]].Opcode = Opcode;
            ID[ins[2]].EpIndex = static_cast<uint32_t>(m_entrypoints.size());
            // 3: Name
            ep.name = std::string(reinterpret_cast<const char*>(ins + 3));
            m_entrypoints.push_back(ep);
            break;
        }
        case spv::OpExecutionMode:
        {
            // 1: Entry Point OpFunction ID
            uint32_t id = ins[1];
            // 2: Execution Mode
            switch (spv::ExecutionMode(ins[2]))
            {
            case spv::ExecutionModeLocalSize:
                if (ID[id].EpIndex < m_entrypoints.size()) {
                    m_entrypoints[ID[id].EpIndex].local_size = { ins[3], ins[4], ins[5] };
                }
                break;
            default:
                break;
            }
            break;
        }
        case spv::OpName:
        {
            // 1: Target ID
            uint32_t id = ins[1];
            // 2: Name
            ID[id].Name = std::string(reinterpret_cast<const char*>(ins + 2));
            break;
        }
        /* 3.32.3 Annotation Instructions */
        case spv::OpDecorate:
        {
            // 1: ID
            uint32_t id = ins[1];
            // 2: Decoration
            switch (spv::Decoration(ins[2]))
            {
            case spv::DecorationDescriptorSet:
                ID[id].DescriptorSet = ins[3]; break;
            case spv::DecorationBinding:
                ID[id].Binding = ins[3]; break;
            default:
                break;
            }
            break;
        }
        /* 3.32.6 Type-Declaration Instructions */
        case spv::OpTypePointer: // e.g. Push Constants        
        {            
            uint32_t id = ins[1];
            ID[id].Opcode = Opcode;
            ID[id].StorageClass = ins[2];            
            ID[id].Type = ins[3];
            break;
        }
        /* 3.32.8 Memory Instruction */
        case spv::OpVariable:
        {
            uint32_t id = ins[2];         
            ID[id].Opcode = Opcode;
            ID[id].Type = ins[1];
            ID[id].StorageClass = ins[3];
            break;
        }
        default:
            break;
        }
        ins += WordCount;
    }
    CHECK(ins == end && "malformed SPIRV shader! (Incomplete read)");
    for (auto& Element : ID) {
        if (Element.Opcode == spv::OpVariable) {
            switch (spv::StorageClass(Element.StorageClass)) {
            case spv::StorageClassUniform:
            case spv::StorageClassUniformConstant:            
            case spv::StorageClassStorageBuffer:
                m_bindings.push_back({ .name = Element.Name, .descriptorSet = Element.DescriptorSet, .binding = Element.Binding });
                break;
            }           
            switch (spv::StorageClass(Element.StorageClass)) {
            case spv::StorageClassPushConstant:
            {
                m_pushConstants.push_back({ .name = Element.Name });
                break;
            }
            }
        }
    }
}

void ShaderReflection::Sort()
{
    std::sort(m_bindings.begin(), m_bindings.end(), [](const Binding& lhs, const Binding& rhs) {
        std::pair<uint32_t, uint32_t>
            k1 = { lhs.descriptorSet, lhs.binding },
            k2 = { rhs.descriptorSet, rhs.binding };
        return k1 < k2;
    });
}

ShaderReflection::ShaderReflection(Core::StlSpan<const char> bytecode, Allocator* alloc)
    : m_allocator(alloc), m_entrypoints(alloc), m_bindings(alloc), m_pushConstants(alloc)
{    
    ParseSPIRV(bytecode);
    Sort();
}



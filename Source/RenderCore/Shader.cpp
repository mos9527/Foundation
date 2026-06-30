using namespace Foundation::RenderCore;
using namespace Foundation::Core;
void Shader::ParseSPIRV(const Span<const char> bytecode)
{
    // https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.pdf
    /* 2.3 Physical Layout of a SPIR-V Module and Instruction */
    // SPIRV shader bytecodes are always 32-bits words.    
    // NOTE: Pointer alignment can be tricky here - though with STL allocators
    // every standard types are aligned to at least alignof(void*)
    // So it's generally fine to cast like this.
    const auto* code = reinterpret_cast<const uint32_t*>(bytecode.data());
    const auto* end  = reinterpret_cast<const uint32_t*>(bytecode.data() + bytecode.size());
    // 0: Magic Number
    CHECK(code[0] == spv::MagicNumber);
    // 1: Version
    // 2: Generator Magic
    // 3: Bound
    const uint32_t Bound = code[3];
    struct Element {
        uint32_t opcode{};
        uint32_t storageClass{};
        uint32_t type{};
        uint32_t descriptorSet{};
        uint32_t binding{};
        Optional<uint32_t> specID{};
        /* -- debug -- */
        String name;
        uint32_t epIndex{};
    };
    Vector<Element> ID(Bound, mAllocator);
    // 4: Reserved
    // 5: First Instruction
    const uint32_t* ins = code + 5;
    while (ins < end) {
        const uint16_t WordCount = ins[0] >> 16;
        CHECK(WordCount && "malformed SPIRV shader! (WordCount=0)");
        switch (uint16_t Opcode = ins[0] & 0xFFFF)
        {
        /* 3.32.5 Mode-Setting Instructions */
        case spv::OpEntryPoint:
        {
            Entrypoint ep;
            // 1: Execution Model
            switch (static_cast<spv::ExecutionModel>(ins[1]))
            {
            case spv::ExecutionModelVertex:
                ep.stage = RHIShaderStageBits::Vertex; break;
            case spv::ExecutionModelFragment:
                ep.stage = RHIShaderStageBits::Fragment; break;
            case spv::ExecutionModelGLCompute:
                ep.stage = RHIShaderStageBits::Compute; break;
            case spv::ExecutionModelMeshEXT:
                ep.stage = RHIShaderStageBits::Mesh; break;
            case spv::ExecutionModelTaskEXT:
                ep.stage = RHIShaderStageBits::Task; break;
            case spv::ExecutionModelRayGenerationKHR:
                ep.stage = RHIShaderStageBits::RayGeneration; break;
            case spv::ExecutionModelAnyHitKHR:
                ep.stage = RHIShaderStageBits::RayAnyHit; break;
            case spv::ExecutionModelClosestHitKHR:
                ep.stage = RHIShaderStageBits::RayClosestHit; break;
            case spv::ExecutionModelMissKHR:
                ep.stage = RHIShaderStageBits::RayMiss; break;
            case spv::ExecutionModelIntersectionKHR:
                ep.stage = RHIShaderStageBits::RayIntersection; break;
            default:
                break;
            }
            // 2: Entry Point OpFunction ID
            ID[ins[2]].opcode = Opcode;
            ID[ins[2]].epIndex = static_cast<uint32_t>(mEntrypoints.size());
            // 3: Name
            ep.name = String(reinterpret_cast<const char*>(ins + 3));
            mEntrypoints.push_back(ep);
            break;
        }
        case spv::OpExecutionMode:
        {
            // 1: Entry Point OpFunction ID
            uint32_t id = ins[1];
            // 2: Execution Mode
            switch (static_cast<spv::ExecutionMode>(ins[2]))
            {
            case spv::ExecutionModeLocalSize:
                if (ID[id].epIndex < mEntrypoints.size()) {
                    mEntrypoints[ID[id].epIndex].groupLocalSize = { ins[3], ins[4], ins[5] };
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
            ID[id].name = String(reinterpret_cast<const char*>(ins + 2));
            break;
        }
        /* 3.32.3 Annotation Instructions */
        case spv::OpDecorate:
        {
            // 1: ID
            uint32_t id = ins[1];
            // 2: Decoration
            switch (static_cast<spv::Decoration>(ins[2]))
            {
            case spv::DecorationSpecId:
                ID[id].specID = ins[3]; break;
            case spv::DecorationDescriptorSet:
                ID[id].descriptorSet = ins[3]; break;
            case spv::DecorationBinding:
                ID[id].binding = ins[3]; break;
            default:
                break;
            }
            break;
        }
        /* 3.32.6 Type-Declaration Instructions */
        case spv::OpTypePointer: // e.g. Push Constants        
        {            
            uint32_t id = ins[1];
            ID[id].opcode = Opcode;
            ID[id].storageClass = ins[2];            
            ID[id].type = ins[3];
            break;
        }
        /* 3.32.8 Memory Instruction */
        case spv::OpVariable:
        {
            uint32_t id = ins[2];         
            ID[id].opcode = Opcode;
            ID[id].type = ins[1];
            ID[id].storageClass = ins[3];
            break;
        }
        default:
            break;
        }
        ins += WordCount;
    }
    CHECK(ins == end && "malformed SPIRV shader! (Incomplete read)");
    for (auto& element : ID) {
        if (element.specID.has_value())
        {
            mSpecializationConstants.push_back({.id = element.specID.value(), .name = element.name });
        }
        if (element.opcode == spv::OpVariable) {
            switch (static_cast<spv::StorageClass>(element.storageClass)) {
            case spv::StorageClassUniform:
            case spv::StorageClassUniformConstant:            
            case spv::StorageClassStorageBuffer:
                mBindings.push_back({ .name = element.name, .descriptorSet = element.descriptorSet, .binding = element.binding });
                break;
            default:
                break;
            }           
            switch (static_cast<spv::StorageClass>(element.storageClass)) {
            case spv::StorageClassPushConstant:
            {
                mPushConstants.push_back({ .name = element.name });
                break;
            }
            default:
                break;
            }
        }
    }
}

void Shader::Sort()
{
    Ranges::sort(mBindings, [](const Binding& lhs, const Binding& rhs) {
        return std::tie(lhs.descriptorSet, lhs.binding) < std::tie(rhs.descriptorSet, rhs.binding);
    });
}

Shader::Shader(Span<const char> bytecode, Allocator* alloc)
    : mAllocator(alloc), mEntrypoints(alloc), mBindings(alloc), mPushConstants(alloc), mSpecializationConstants(alloc)
{    
    ParseSPIRV(bytecode);
    Sort();
}

String Shader::DbgDumpShaderInfo() const
{
    String out;
    fmt::format_to(std::back_inserter(out), "Entry Point: {}\n", mEntrypoints.size());
    for (const auto& ep : mEntrypoints)
        fmt::format_to(std::back_inserter(out), "   Name: {}, Stage: {}\n", ep.name, ep.stage);
    fmt::format_to(std::back_inserter(out), "Push Constants: {}\n", mPushConstants.size());
    for (const auto& pc : mPushConstants)
        fmt::format_to(std::back_inserter(out), "   Push Constant: {}\n", pc.name);
    fmt::format_to(std::back_inserter(out), "Bindings: {}\n", mBindings.size());
    for (const auto& var : mBindings)
        fmt::format_to(std::back_inserter(out), "   Binding: {} (set={}, binding={})\n", var.name, var.descriptorSet, var.binding);
    out.pop_back();
    return out;
}

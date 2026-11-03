#include "Shader.hpp"
#include <spirv/unified1/spirv.hpp>
#include <cstring>
#include <limits>

using namespace Foundation::RenderCore;
using namespace Foundation::Core;
void Shader::ParseSPIRV(const Span<const char> bytecode)
{
    CHECK_MSG(bytecode.size() >= 5 * sizeof(uint32_t) && bytecode.size() % sizeof(uint32_t) == 0,
              "Malformed SPIR-V shader header or bytecode size");
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
    CHECK_MSG(Bound > 0, "Malformed SPIR-V ID bound");
    struct Element {
        uint32_t opcode{};
        uint32_t storageClass{};
        uint32_t type{};
        uint32_t descriptorSet{};
        uint32_t binding{};
        size_t descriptorSetWordOffset{};
        size_t bindingWordOffset{};
        uint32_t arrayLength{};
        uint32_t imageDimension{};
        uint32_t imageSampled{};
        bool bufferBlock{};
        Optional<uint64_t> constant{};
        Optional<uint32_t> specID{};
        /* -- debug -- */
        String name;
        uint32_t epIndex{};
    };
    Vector<Element> ID(Bound, mAllocator);
    auto readString = [](const uint32_t* begin, const uint32_t* limit)
    {
        const char* text = reinterpret_cast<const char*>(begin);
        const char* terminator = static_cast<const char*>(
            std::memchr(text, 0, static_cast<size_t>(limit - begin) * sizeof(uint32_t)));
        CHECK_MSG(terminator, "Unterminated string in SPIR-V instruction");
        return String(text, static_cast<size_t>(terminator - text));
    };
    // 4: Reserved
    // 5: First Instruction
    const uint32_t* ins = code + 5;
    while (ins < end) {
        const uint16_t WordCount = ins[0] >> 16;
        CHECK_MSG(WordCount && WordCount <= static_cast<size_t>(end - ins),
                  "Malformed SPIR-V instruction length");
        switch (uint16_t Opcode = ins[0] & 0xFFFF)
        {
        /* 3.32.5 Mode-Setting Instructions */
        case spv::OpEntryPoint:
        {
            CHECK(WordCount >= 4 && ins[2] < Bound);
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
            ep.name = readString(ins + 3, ins + WordCount);
            mEntrypoints.push_back(ep);
            break;
        }
        case spv::OpExecutionMode:
        {
            CHECK(WordCount >= 3 && ins[1] < Bound);
            // 1: Entry Point OpFunction ID
            uint32_t id = ins[1];
            // 2: Execution Mode
            switch (static_cast<spv::ExecutionMode>(ins[2]))
            {
            case spv::ExecutionModeLocalSize:
                CHECK(WordCount >= 6);
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
            CHECK(WordCount >= 3 && ins[1] < Bound);
            // 1: Target ID
            uint32_t id = ins[1];
            // 2: Name
            ID[id].name = readString(ins + 2, ins + WordCount);
            break;
        }
        /* 3.32.3 Annotation Instructions */
        case spv::OpDecorate:
        {
            CHECK(WordCount >= 3 && ins[1] < Bound);
            // 1: ID
            uint32_t id = ins[1];
            // 2: Decoration
            switch (static_cast<spv::Decoration>(ins[2]))
            {
            case spv::DecorationSpecId:
                CHECK(WordCount == 4);
                ID[id].specID = ins[3];
                break;
            case spv::DecorationDescriptorSet:
                CHECK(WordCount == 4 && !ID[id].descriptorSetWordOffset);
                ID[id].descriptorSet = ins[3];
                ID[id].descriptorSetWordOffset = static_cast<size_t>(ins + 3 - code);
                break;
            case spv::DecorationBinding:
                CHECK(WordCount == 4 && !ID[id].bindingWordOffset);
                ID[id].binding = ins[3];
                ID[id].bindingWordOffset = static_cast<size_t>(ins + 3 - code);
                break;
            case spv::DecorationBufferBlock:
                ID[id].bufferBlock = true;
                break;
            default:
                break;
            }
            break;
        }
        /* 3.32.6 Type-Declaration Instructions */
        case spv::OpDecorationGroup:
        case spv::OpTypeStruct:
        case spv::OpTypeSampler:
        case spv::OpTypeInt:
        case spv::OpTypeAccelerationStructureKHR:
        {
            CHECK(WordCount >= 2 && ins[1] < Bound);
            ID[ins[1]].opcode = Opcode;
            break;
        }
        case spv::OpTypeImage:
        {
            CHECK(WordCount >= 9 && ins[1] < Bound);
            auto& element = ID[ins[1]];
            element.opcode = Opcode;
            element.imageDimension = ins[3];
            element.imageSampled = ins[7];
            break;
        }
        case spv::OpTypeArray:
        case spv::OpTypeRuntimeArray:
        {
            CHECK(WordCount >= 3 && ins[1] < Bound && ins[2] < Bound);
            auto& element = ID[ins[1]];
            element.opcode = Opcode;
            element.type = ins[2];
            if (Opcode == spv::OpTypeArray)
            {
                CHECK(WordCount == 4 && ins[3] < Bound);
                element.arrayLength = ins[3];
            }
            break;
        }
        case spv::OpTypePointer: // e.g. Push Constants
        {
            CHECK(WordCount == 4 && ins[1] < Bound && ins[3] < Bound);
            uint32_t id = ins[1];
            ID[id].opcode = Opcode;
            ID[id].storageClass = ins[2];
            ID[id].type = ins[3];
            break;
        }
        case spv::OpConstant:
        {
            CHECK(WordCount >= 4 && ins[1] < Bound && ins[2] < Bound);
            auto& element = ID[ins[2]];
            element.opcode = Opcode;
            element.type = ins[1];
            if (WordCount == 4)
                element.constant = ins[3];
            else if (WordCount == 5)
                element.constant = uint64_t(ins[3]) | (uint64_t(ins[4]) << 32);
            break;
        }
        /* 3.32.8 Memory Instruction */
        case spv::OpVariable:
        {
            CHECK(WordCount >= 4 && ins[1] < Bound && ins[2] < Bound);
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
    for (uint32_t id = 0; id < Bound; id++) {
        auto& element = ID[id];
        CHECK_MSG(element.opcode != spv::OpDecorationGroup ||
                      (!element.descriptorSetWordOffset && !element.bindingWordOffset && !element.bufferBlock),
                  "Grouped descriptor decorations are not supported by shader binding relocation");
        if (element.specID.has_value())
        {
            mSpecializationConstants.push_back({.id = element.specID.Get(), .name = element.name});
        }
        if (element.opcode == spv::OpVariable) {
            switch (static_cast<spv::StorageClass>(element.storageClass)) {
            case spv::StorageClassUniform:
            case spv::StorageClassUniformConstant:
            case spv::StorageClassStorageBuffer:
            {
                CHECK_MSG(element.descriptorSetWordOffset && element.bindingWordOffset,
                          "Descriptor {} requires direct DescriptorSet and Binding decorations", element.name);
                CHECK(element.type < Bound && ID[element.type].opcode == spv::OpTypePointer);
                uint32_t type = ID[element.type].type;
                uint64_t count = 1;
                uint32_t arrayDepth = 0;
                while (ID[type].opcode == spv::OpTypeArray || ID[type].opcode == spv::OpTypeRuntimeArray)
                {
                    CHECK(arrayDepth++ < Bound);
                    if (ID[type].opcode == spv::OpTypeRuntimeArray)
                    {
                        count = 0;
                    }
                    else
                    {
                        auto const& length = ID[ID[type].arrayLength];
                        CHECK_MSG(length.constant.has_value() && ID[length.type].opcode == spv::OpTypeInt,
                                  "Descriptor {} requires a literal integer array length", element.name);
                        uint64_t size = length.constant.Get();
                        CHECK(size > 0 && size <= std::numeric_limits<uint32_t>::max());
                        count *= size;
                        CHECK(count <= std::numeric_limits<uint32_t>::max());
                    }
                    type = ID[type].type;
                }
                RHIDescriptorType descriptorType{};
                if (element.storageClass == spv::StorageClassStorageBuffer)
                {
                    descriptorType = RHIDescriptorType::StorageBuffer;
                }
                else if (element.storageClass == spv::StorageClassUniform)
                {
                    descriptorType = ID[type].bufferBlock
                        ? RHIDescriptorType::StorageBuffer : RHIDescriptorType::UniformBuffer;
                }
                else
                {
                    switch (ID[type].opcode)
                    {
                    case spv::OpTypeSampler:
                        descriptorType = RHIDescriptorType::Sampler;
                        break;
                    case spv::OpTypeImage:
                        CHECK_MSG(ID[type].imageDimension != spv::DimBuffer &&
                                      ID[type].imageDimension != spv::DimSubpassData &&
                                      (ID[type].imageSampled == 1 || ID[type].imageSampled == 2),
                                  "Unsupported descriptor image type for {}", element.name);
                        descriptorType = ID[type].imageSampled == 2
                            ? RHIDescriptorType::StorageImage : RHIDescriptorType::SampledImage;
                        break;
                    case spv::OpTypeAccelerationStructureKHR:
                        descriptorType = RHIDescriptorType::AccelerationStructure;
                        break;
                    default:
                        CHECK_MSG(false, "Unsupported descriptor type for {}", element.name);
                    }
                }
                mBindings.push_back({.name = element.name,
                                     .descriptorSet = element.descriptorSet,
                                     .binding = element.binding,
                                     .type = descriptorType,
                                     .count = static_cast<uint32_t>(count),
                                     .id = id,
                                     .descriptorSetWordOffset = element.descriptorSetWordOffset,
                                     .bindingWordOffset = element.bindingWordOffset});
                break;
            }
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

void Shader::RemapBindings(Span<char> bytecode, const Map<String, Pair<uint32_t, uint32_t>>& locations)
{
    CHECK(bytecode.size() >= 5 * sizeof(uint32_t) && bytecode.size() % sizeof(uint32_t) == 0);
    for (auto& binding : mBindings)
    {
        auto it = locations.find(binding.name);
        CHECK_MSG(it != locations.end(), "No relocated binding was assigned to {}", binding.name);
        auto patch = [&](size_t wordOffset, spv::Decoration decoration, uint32_t value)
        {
            CHECK(wordOffset >= 3 && wordOffset < bytecode.size() / sizeof(uint32_t));
            uint32_t instruction[3];
            std::memcpy(instruction, bytecode.data() + (wordOffset - 3) * sizeof(uint32_t), sizeof(instruction));
            CHECK_MSG(instruction[0] == ((4u << 16) | spv::OpDecorate) &&
                          instruction[1] == binding.id && instruction[2] == decoration,
                      "SPIR-V binding decoration does not match reflection for {}", binding.name);
            std::memcpy(bytecode.data() + wordOffset * sizeof(uint32_t), &value, sizeof(value));
        };
        auto const& [set, slot] = it->second;
        patch(binding.descriptorSetWordOffset, spv::DecorationDescriptorSet, set);
        patch(binding.bindingWordOffset, spv::DecorationBinding, slot);
        binding.descriptorSet = set;
        binding.binding = slot;
    }
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
        fmt::format_to(std::back_inserter(out), "   Binding: {} (set={}, binding={}, type={}, count={})\n",
                       var.name, var.descriptorSet, var.binding, var.type, var.count);
    out.pop_back();
    return out;
}

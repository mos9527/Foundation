#include <Platform/Logging.hpp>
#include "ShaderReflection.hpp"
using namespace Foundation;

static const char* DbgGetShaderStageName(RHI::RHIShaderStage stage) {
    switch (RHI::RHIShaderStageBits(stage)) {
        case RHI::RHIShaderStageBits::Vertex: return "Vertex";
        case RHI::RHIShaderStageBits::Fragment: return "Fragment";
        case RHI::RHIShaderStageBits::Compute: return "Compute";
        default: return "Unknown";
    }
}

std::string ShaderReflection::DbgDumpShaderInfo() const
{
    std::string out;
    fmt::format_to(std::back_inserter(out), "Entry Point: {}", m_entrypoint.name);
    fmt::format_to(std::back_inserter(out), " (Stage: {})\n", DbgGetShaderStageName(m_entrypoint.stage));
    for (const auto& pc : m_pushConstants) {
        fmt::format_to(std::back_inserter(out), "  Push Constant: {}\n", pc.name);
    }
    for (const auto& var : m_bindings) {
        fmt::format_to(std::back_inserter(out), "  Binding: {} (set={}, binding={})\n", var.name, var.descriptorSet, var.binding);
    }
    return out;
}

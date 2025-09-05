#include "ShaderReflection.hpp"
#include <fmt/format.h>
using namespace Foundation::Rendering;

std::string ShaderReflection::DbgDumpShaderInfo() const
{
    std::string out;
    fmt::format_to(std::back_inserter(out), "Entry Point: {}", m_entrypoint.name);
    fmt::format_to(std::back_inserter(out), " (Stage: {})\n", m_entrypoint.stage);
    fmt::format_to(std::back_inserter(out), "Push Constants: {}\n", m_pushConstants.size());
    for (const auto& pc : m_pushConstants) {
        fmt::format_to(std::back_inserter(out), "  Push Constant: {}\n", pc.name);
    }
    fmt::format_to(std::back_inserter(out), "Bindings: {}\n", m_bindings.size());
    for (const auto& var : m_bindings) {
        fmt::format_to(std::back_inserter(out), "  Binding: {} (set={}, binding={})\n", var.name, var.descriptorSet, var.binding);
    }
    out.pop_back();
    return out;
}

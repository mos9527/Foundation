#include "ShaderReflection.hpp"
#include <fmt/format.h>
using namespace Foundation::Rendering;

String ShaderReflection::DbgDumpShaderInfo() const
{
    String out;
    fmt::format_to(std::back_inserter(out), "Entry Point: {}\n", m_entrypoints.size());
    for (const auto& ep : m_entrypoints)
        fmt::format_to(std::back_inserter(out), "   Name: {}, Stage: {}\n", ep.name, ep.stage);
    fmt::format_to(std::back_inserter(out), "Push Constants: {}\n", m_pushConstants.size());
    for (const auto& pc : m_pushConstants)
        fmt::format_to(std::back_inserter(out), "   Push Constant: {}\n", pc.name);
    fmt::format_to(std::back_inserter(out), "Bindings: {}\n", m_bindings.size());
    for (const auto& var : m_bindings)
        fmt::format_to(std::back_inserter(out), "   Binding: {} (set={}, binding={})\n", var.name, var.descriptorSet, var.binding);
    out.pop_back();
    return out;
}

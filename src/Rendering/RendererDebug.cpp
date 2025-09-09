#include "Renderer.hpp"
using namespace Foundation::Rendering;
String Renderer::DbgDumpGraphviz() const {
    String out;
    fmt::format_to(std::back_inserter(out), "digraph G {{\n");
    fmt::format_to(std::back_inserter(out), "    rankdir=TB;\n");
    auto& graph = m_setup->graph;
    auto& passes = m_setup->trackedPasses;
    auto& resources = m_setup->trackedResources;
    for (auto& pass : passes) {
        fmt::format_to(
            std::back_inserter(out),
            "    \"{}@{}\" [ shape=box style=filled fillcolor=\"{}\" ];\n",
            pass.name,
            pass.handle,
            pass.queue == RHIDeviceQueueType::Graphics ? "#d0e0f0" : "#f0d0e0");
    }
    // Dependencies
    for (PassHandle u = 0; u < m_setup->graph.size(); u++) {
        for (auto [v, w] : graph[u]) {
            fmt::format_to(
                std::back_inserter(out),
                "    \"{}@{}\" -> \"{}@{}\" [label=\"{}\"];\n",
                passes[u].name, u,
                passes[v].name, v,
                resources[w].name);
        }
    }
    fmt::format_to(std::back_inserter(out), "}}\n");
    out.pop_back();
    return out;
}

String Renderer::DbgDumpActivePasses() const {
    String out;
    for (const auto& idx : m_setup->execution) {
        auto& pass = m_setup->trackedPasses[idx];
        fmt::format_to(
            std::back_inserter(out), "{}: {}, depth={}, ord={}, queue={}, has_cross_queue_dependent={}, write_backbuffer={}\n",
            pass.handle,
            pass.name,
            pass.depth,
            pass.ord,
            pass.queue,
            pass.has_cross_queue_dependent,
            pass.write_backbuffer
        );
    }
    out.pop_back();
    return out;
}

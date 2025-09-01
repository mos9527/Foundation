#include "Renderer.hpp"

std::string Renderer::DbgDumpGraphviz() const {
    std::string out;
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
            pass.type == PassType::Graphics ? "#d0e0f0" : "#f0d0e0");
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
    // Cross-queue sync points
    for (auto& pass : passes) {
        auto const& consumer = pass.firstConsumerPass;
        if (consumer.has_value()) {
            auto const& cpass = passes[consumer.value()];
            if (cpass.type != pass.type)
                fmt::format_to(
                    std::back_inserter(out),
                    "    \"{}@{}\" -> \"{}@{}\" [label=\"<x-queue syncs>\" style=dotted];\n",
                    cpass.name, cpass.handle,
                    pass.name, pass.handle);
        }
    }
    fmt::format_to(std::back_inserter(out), "}}\n");
    return out;
}

std::string Renderer::DbgDumpActivePasses() const {
    std::string out;
    for (const auto& idx : m_setup->activePasses) {
        auto& pass = m_setup->trackedPasses[idx];
        fmt::format_to(std::back_inserter(out), "{}: {}, dep={}, ord={}\n", pass.handle, pass.name, pass.depth, pass.ord);
    }
    return out;
}

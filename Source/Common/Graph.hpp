#pragma once
#include "../pch.hpp"
#include "../Common/Allocator.hpp"
/* adjacency list DAG */
template<typename T> class DAG {
	template<typename T> using Allocator = DefaultAllocator<T>;
public:
	using vertex = T;
	using tree_type = unordered_set<vertex, DefaultAllocator<vertex>>;
	template<typename Elem> using table_type = unordered_map<vertex, Elem, Allocator<std::pair<const vertex, Elem>>>;
	// removes a vertex and all its connecting links. O(V + E)
	void remove_vertex(vertex V) {
		auto graphT = transpose();
		for (auto V0 : graphT.graph[V])
			remove_edge(V0, V);
		for (auto V1 : graph[V])
			remove_edge(V, V1);
		return;
	}
	// removes an edge. O(1) ~ O(n) 
	void remove_edge(vertex lhs, vertex rhs) {
		auto it = graph[lhs].find(rhs);
		if (it != graph[lhs].end())
			graph[lhs].erase(it);
	}
	// adds an edge. O(1) ~ O(n)
	void add_edge(vertex lhs, vertex rhs) {
		graph[lhs].insert(rhs);
	}
	// topsort. returned vector size would be 0 if cyclic. O(V + E)
	std::vector<vertex> topological_sort() {
		std::vector<vertex> sorted;
		table_type<bool> stacked, visited;
		bool has_cycles = false; // invalid DAG
		auto inner = [&](auto& inner, vertex V) -> void {
			if (stacked[V]) has_cycles = true;
			if (visited[V] || has_cycles) return;
			stacked[V] = true;
			for (auto vtx : graph[V])
				inner(inner, vtx);
			stacked[V] = false;
			visited[V] = true;
			sorted.push_back(V);
		};
		for (auto& [vtx, tree] : graph)
			inner(inner, vtx);
		if (has_cycles) sorted.clear();
		std::reverse(sorted.begin(), sorted.end());
		return sorted;
	}
	// generate a transpose of this graph. O(V + E)
	DAG<vertex> transpose() {
		DAG<vertex> graphT;
		for (auto& [V0, tree] : graph) {
			for (auto V1 : tree) {
				graphT.graph[V1].insert(V0);
			}
		}
		return graphT;
	}	
	std::unordered_map<vertex, uint> get_depths(std::vector<vertex>& topsorted) {
		std::unordered_map<vertex, uint> depths;
		for (vertex v : topsorted) {
			for (vertex next : graph[v]) {
				depths[next] = std::max(depths[next], depths[v] + 1);
			}
		}
		return depths;
	}
	std::unordered_map<vertex, uint> get_depths(auto comp = std::max) {
		auto topsorted = topological_sort();
		return get_depths(topsorted, comp);
	}
	// all edeges within this graph. ordering is not guaranteed
	std::vector<std::pair<vertex, vertex>> unordered_edges() {
		std::vector<std::pair<vertex, vertex>> list;
		for (auto& [V0, tree] : graph) {
			for (auto V1 : tree)
				list.push_back({ V0,V1 });
		}
		return list;
	}
	// all vertices within this graph. ordering is not guaranteed
	std::vector<vertex> unordered_vertices() {
		std::vector<vertex> list;
		for (auto& [V0, tree] : graph) list.push_back(V0);
		return list;
	}
	// check if two graphs are equivalent
	bool operator==(DAG<vertex>& other) {
		for (auto& [V0, tree] : graph) {
			auto& othert = other.graph[V0];
			if (other.graph[V0] != tree) return false;
		}
		return true;
	}
	// reset
	void reset() {
		graph.clear();
	}
	table_type<tree_type> & get_graph() { return graph; }
protected:
	table_type<tree_type> graph;
};
#pragma once
#include "../Common/Allocator.hpp"

/* adjacency list DAG */
template<typename T, typename vertex, typename _set_type, typename _table_type> class DAG {
public:
	using DAG_type = DAG<T, vertex, _set_type, _table_type>;
	using vertex_type = vertex;
	using set_type = _set_type;
	using table_type = _table_type;

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
		set_type stacked, visited;
		bool has_cycles = false; // invalid DAG
		auto inner = [&](auto& inner, vertex V) -> void {
			if (stacked.contains(V)) has_cycles = true;
			if (visited.contains(V) || has_cycles) return;
			stacked.insert(V);
			for (auto& vtx : graph[V])
				inner(inner, vtx);
			stacked.erase(V);
			visited.insert(V);
			sorted.push_back(V);
		};
		for (auto& [vtx, set] : graph)
			inner(inner, vtx);
		if (has_cycles) sorted.clear();
		std::reverse(sorted.begin(), sorted.end());
		return sorted;
	}
	// generate a transpose of this graph. O(V + E)
	DAG_type transpose() {
		DAG_type graphT;
		for (auto& [V0, tree] : graph) {
			for (auto V1 : tree) {
				graphT.graph[V1].insert(V0);
			}
		}
		return graphT;
	}	
	std::map<vertex, uint> get_depths(std::vector<vertex>& topsorted) {
		std::map<vertex, uint> depths;
		for (vertex v : topsorted) {
			for (vertex next : graph[v]) {
				depths[next] = std::max(depths[next], depths[v] + 1);
			}
		}
		return depths;
	}
	// check if two graphs are equivalent
	bool operator==(DAG_type& other) {
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
	table_type & get_graph() { return graph; }
protected:
	table_type graph;
};

template<typename T, typename vertex> using basic_DAG = DAG<T, vertex, std::set<vertex>, std::map<vertex, std::set<vertex>>>;
template<typename T, typename vertex> using unordered_DAG = DAG<T, vertex, std::unordered_set<vertex>, std::unordered_map<vertex, std::unordered_set<vertex>>>;
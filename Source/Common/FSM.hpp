#pragma once
#include "../pch.hpp"
namespace FSM {
	// #0. Variant based FSM
	// from https://www.cppstories.com/2023/finite-state-machines-variant-cpp/
	// This is pretty cool with the code you write! But the compiler emits code that's N^2 to
	// the number of states in symbol count. And with debug build that could be A LOT.	
	// see https://godbolt.org/z/rKd4f6sxz for how this could've been used.
	// [This is not yet used anywhere, but it looked nice so I'd keep it.]
	template<typename ...States> using VFSMState = std::variant<States...>;
	template<typename VFSMStateT> struct VFSM {
		using State = VFSMStateT;
		template<typename T1, typename T2> static State Transition(const T1&, const T2&) {
			// the compiler still generates code for all of the variant's types.
			// those cases are not explictly stated. throw a exception for them.
			throw std::logic_error{"bad transtition logic"};
		}
	private:
		State state;
	public:
		void transition(const State& new_state) {
			state = std::visit(
				[](const auto& t1, const auto& t2) {
					return Transition(t1, t2);
				}, state, new_state
			);
		}
		const State& get_state() { return state; }
	};

	// #1. Enum-based FSM.
	// xxx it's late. take this to tommorow.
	// xxx it's already 'tomorrow'.
}
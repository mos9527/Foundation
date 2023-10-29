#pragma once
typedef uint64_t handle_type;
// A FIFO queue for mananging allocation & freeing of handle values
// so that they're always within a certain range, enabling O(1) lookup
// w/o excessive constant overhead
class handle_queue {
	std::vector<handle_type> freeHandles;
public:
	handle_queue() = default;
	void setup(handle_type max_handle) {
		freeHandles.resize(max_handle);
		std::iota(freeHandles.begin(), freeHandles.end(), 0);		
		std::reverse(freeHandles.begin(), freeHandles.end());
	}
	void push(handle_type one) {
		freeHandles.push_back(one);
	}
	handle_type pop(){
		handle_type one = freeHandles.back();
		freeHandles.pop_back();
		return one;
	}
};
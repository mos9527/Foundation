#pragma once
typedef uint64_t handle;
// A FIFO queue for mananging allocation & freeing of handle values
// so that they're always within a certain range, enabling O(1) lookup
// w/o excessive constant overhead
class handle_queue {
	std::vector<handle> freeHandles;
public:
	handle_queue() = default;
	void setup(handle max_handle) {
		freeHandles.resize(max_handle);
		std::iota(freeHandles.begin(), freeHandles.end(), 0);		
		std::reverse(freeHandles.begin(), freeHandles.end());
	}
	void push(handle one) {
		freeHandles.push_back(one);
	}
	handle pop(){
		handle one = freeHandles.back();
		freeHandles.pop_back();
		return one;
	}
};
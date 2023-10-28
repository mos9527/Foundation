#pragma once
typedef size_t handle_type;
// A FIFO queue for mananging allocation & freeing of handle values
// so that they're always within a certain range, enabling O(1) lookup
// w/o excessive constant overhead
class HandleQueue {
	std::vector<handle_type> freeHandles;
public:
	HandleQueue() = default;
	void Setup(handle_type max_handle) {
		freeHandles.resize(max_handle);
		std::iota(freeHandles.begin(), freeHandles.end(), 0);		
		std::reverse(freeHandles.begin(), freeHandles.end());
	}
	void Return(handle_type one) {
		freeHandles.push_back(one);
	}
	handle_type Pop(){
		handle_type one = freeHandles.back();
		freeHandles.pop_back();
		return one;
	}
};
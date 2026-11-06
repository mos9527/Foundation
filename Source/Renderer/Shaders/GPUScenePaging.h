#pragma once

// Shared CPU/shader virtual-address layout. These describe address granularity, not budgets.
// Primitive offsets are bytes; scene-table offsets are elements (independent of sizeof(T)).
#define GS_PRIMITIVE_PAGE_SHIFT 16u
#define GS_TABLE_PAGE_SHIFT 10u
#define GS_PAGE_TABLE_LEAF_SHIFT 10u
#define GS_PAGE_TABLE_LEAF_ENTRIES (1u << GS_PAGE_TABLE_LEAF_SHIFT)

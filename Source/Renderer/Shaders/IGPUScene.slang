#include "GPUScenePaging.h"

// GPUScene buffer bindings are stable, two-level address tables, not payload buffers.
// Root/leaf entries are uint64 device addresses. New entries are published without moving
// existing allocations. Address-table bindings remain read-only even when writing payloads;
// the render graph must still declare those writes (BindBufferUnordered).
// Callers must validate counts/offsets: unmapped pages and out-of-range accesses are invalid.
public uint64_t GSResolvePageAddress(ByteAddressBuffer pages, uint pageIndex)
{
    uint64_t leafAddress = pages.Load<uint64_t>((pageIndex >> GS_PAGE_TABLE_LEAF_SHIFT) * 8u);
    Ptr<uint64_t, Access.Read> leaf = Ptr<uint64_t, Access.Read>(leafAddress);
    return leaf[pageIndex & (GS_PAGE_TABLE_LEAF_ENTRIES - 1u)];
}

// Absolute virtual element indices, including GSOffsetCount.offset when applicable.
// Each table is typed on the CPU; T must match that table's element type.
public uint64_t GSResolveElementAddress<T>(ByteAddressBuffer pages, uint index)
{
    return GSResolvePageAddress(pages, index >> GS_TABLE_PAGE_SHIFT) +
        uint64_t(index & ((1u << GS_TABLE_PAGE_SHIFT) - 1u)) * uint64_t(sizeof(T));
}

public T GSLoad<T>(ByteAddressBuffer pages, uint index)
{
    return *Ptr<T, Access.Read, AddressSpace.Device, ScalarDataLayout>(GSResolveElementAddress<T>(pages, index));
}

public void GSStore<T>(ByteAddressBuffer pages, uint index, T value)
{
    *Ptr<T, Access.ReadWrite, AddressSpace.Device, ScalarDataLayout>(GSResolveElementAddress<T>(pages, index)) = value;
}

// Absolute virtual byte offsets, as stored in GSMesh/GSCurveSet/GSInstance headers.
public uint64_t GSResolvePrimitiveAddress(ByteAddressBuffer pages, uint byteOffset)
{
    return GSResolvePageAddress(pages, byteOffset >> GS_PRIMITIVE_PAGE_SHIFT) +
        uint64_t(byteOffset & ((1u << GS_PRIMITIVE_PAGE_SHIFT) - 1u));
}

// The entire accessed value must belong to one live allocation. An allocation may span
// logical pages, but its backing storage is contiguous (also required by BLAS builds),
// so packed values crossing a logical page boundary need no special-case reconstruction.
// ScalarDataLayout preserves the packed CPU structures, including half/uint8 mesh data.
public T GSLoadPrimitive<T>(ByteAddressBuffer pages, uint byteOffset)
{
    return *Ptr<T, Access.Read, AddressSpace.Device, ScalarDataLayout>(GSResolvePrimitiveAddress(pages, byteOffset));
}

// Select the load, not the buffer object: per-instance selection must not require
// variable storage-buffer pointers or read an unmapped page in the other pool.
public T GSLoadPrimitive<T>(ByteAddressBuffer staticPages, ByteAddressBuffer dynamicPages, bool isDynamic,
                            uint byteOffset)
{
    if (isDynamic)
        return GSLoadPrimitive<T>(dynamicPages, byteOffset);
    return GSLoadPrimitive<T>(staticPages, byteOffset);
}

public void GSStorePrimitive<T>(ByteAddressBuffer pages, uint byteOffset, T value)
{
    *Ptr<T, Access.ReadWrite, AddressSpace.Device, ScalarDataLayout>(GSResolvePrimitiveAddress(pages, byteOffset)) = value;
}

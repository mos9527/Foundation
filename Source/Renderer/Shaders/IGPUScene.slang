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

// Checked inspection for externally supplied indices. Status: 0=valid, 1=root OOB,
// 2=null/misaligned leaf, 3=unmapped payload page. This cannot validate the lifetime
// of an arbitrary nonzero/corrupt device address; scene ownership still must be correct.
public struct GSAddressInfo
{
    public uint status;
    public uint rootIndex;
    public uint leafSlot;
    public uint64_t leafAddress;
    public uint64_t pageAddress;
    public uint64_t address;
};

public GSAddressInfo GSInspectPageAddress(ByteAddressBuffer pages, uint pageIndex)
{
    GSAddressInfo info;
    info.status = 1u;
    info.rootIndex = pageIndex >> GS_PAGE_TABLE_LEAF_SHIFT;
    info.leafSlot = pageIndex & (GS_PAGE_TABLE_LEAF_ENTRIES - 1u);
    info.leafAddress = 0;
    info.pageAddress = 0;
    info.address = 0;
    uint rootBytes;
    pages.GetDimensions(rootBytes);
    if (info.rootIndex >= rootBytes / 8u)
        return info;
    info.leafAddress = pages.Load<uint64_t>(info.rootIndex * 8u);
    info.status = 2u;
    if (info.leafAddress == 0 || (info.leafAddress & uint64_t(7)) != 0)
        return info;
    info.pageAddress = Ptr<uint64_t, Access.Read>(info.leafAddress)[info.leafSlot];
    info.status = 3u;
    if (info.pageAddress == 0)
        return info;
    info.address = info.pageAddress;
    info.status = 0u;
    return info;
}

public GSAddressInfo GSInspectElementAddress<T>(ByteAddressBuffer pages, uint index)
{
    GSAddressInfo info = GSInspectPageAddress(pages, index >> GS_TABLE_PAGE_SHIFT);
    if (info.status == 0u)
        info.address += uint64_t(index & ((1u << GS_TABLE_PAGE_SHIFT) - 1u)) * uint64_t(sizeof(T));
    return info;
}

public GSAddressInfo GSInspectPrimitiveAddress(ByteAddressBuffer pages, uint byteOffset)
{
    GSAddressInfo info = GSInspectPageAddress(pages, byteOffset >> GS_PRIMITIVE_PAGE_SHIFT);
    if (info.status == 0u)
        info.address += uint64_t(byteOffset & ((1u << GS_PRIMITIVE_PAGE_SHIFT) - 1u));
    return info;
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

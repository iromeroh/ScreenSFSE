// Compiled WITHOUT the forced SFSE PCH include (see CMakeLists.txt): CINTERFACE must be defined before any COM
// header so d3d12.h declares the C vtable structs whose member offsets are the slot numbers.
#define CINTERFACE
#include <windows.h>
#include <d3d12.h>

#include <cstddef>

#include "d3d12_vtbl.h"

#define SLOT(vtbl, method) static_cast<int>(offsetof(vtbl, method) / sizeof(void*))

namespace d3d12vt
{
const Indices& indices()
{
    static const Indices value{
        SLOT(ID3D12Device10Vtbl, CreateCommittedResource),
        SLOT(ID3D12Device10Vtbl, CreatePlacedResource),
        SLOT(ID3D12Device10Vtbl, CreateReservedResource),
        SLOT(ID3D12Device10Vtbl, CreateCommittedResource1),
        SLOT(ID3D12Device10Vtbl, CreateCommittedResource2),
        SLOT(ID3D12Device10Vtbl, CreatePlacedResource1),
        SLOT(ID3D12Device10Vtbl, CreateCommittedResource3),
        SLOT(ID3D12Device10Vtbl, CreatePlacedResource2),
        SLOT(ID3D12CommandQueueVtbl, ExecuteCommandLists),
    };
    return value;
}
}

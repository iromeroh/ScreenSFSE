#pragma once

// Vtable slot numbers of the D3D12 methods ScreenSFSE hooks. They are computed from the SDK's C-style
// declarations (d3d12_vtbl.cpp is compiled with CINTERFACE), so they cannot drift from d3d12.h.
namespace d3d12vt
{
struct Indices
{
    int createCommittedResource;
    int createPlacedResource;
    int createReservedResource;
    int createCommittedResource1;   // ID3D12Device4
    int createCommittedResource2;   // ID3D12Device8
    int createPlacedResource1;      // ID3D12Device8
    int createCommittedResource3;   // ID3D12Device10
    int createPlacedResource2;      // ID3D12Device10
    int executeCommandLists;        // ID3D12CommandQueue
};

const Indices& indices();
}

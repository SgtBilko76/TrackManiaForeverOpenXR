#pragma once

// Minimal d3d9on12.h for mingw-w64, which does not ship this header. Only the
// Direct3DCreate9On12 entry point used by d3d9_proxy.cpp is declared.

#include <d3d9.h>

#define MAX_D3D9ON12_QUEUES 2

typedef struct _D3D9ON12_ARGS {
    BOOL Enable9On12;
    IUnknown* pD3D12Device;
    IUnknown* ppD3D12Queues[MAX_D3D9ON12_QUEUES];
    UINT NumQueues;
    UINT NodeMask;
} D3D9ON12_ARGS;

typedef IDirect3D9*(WINAPI* PFN_Direct3DCreate9On12)(
    UINT SDKVersion, D3D9ON12_ARGS* pOverrideList, UINT NumOverrideEntries);

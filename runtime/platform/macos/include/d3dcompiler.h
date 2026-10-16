// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The HLSL compiler entry points on macOS. D3DCompile translates HLSL (shader model 5.x) to
// SPIR-V with glslang's HLSL front end; the blob holds the SPIR-V module and its entry point and
// stage, which the Metal translation layer converts to MSL with SPIRV-Cross when a pipeline is
// created. D3DReflect reflects that SPIR-V.

#include <windows.h>
#include <d3dcommon.h>

#define D3DCOMPILE_DEBUG (1u << 0)
#define D3DCOMPILE_SKIP_VALIDATION (1u << 1)
#define D3DCOMPILE_SKIP_OPTIMIZATION (1u << 2)
#define D3DCOMPILE_PACK_MATRIX_ROW_MAJOR (1u << 3)
#define D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR (1u << 4)
#define D3DCOMPILE_PARTIAL_PRECISION (1u << 5)
#define D3DCOMPILE_AVOID_FLOW_CONTROL (1u << 9)
#define D3DCOMPILE_PREFER_FLOW_CONTROL (1u << 10)
#define D3DCOMPILE_ENABLE_STRICTNESS (1u << 11)
#define D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY (1u << 12)
#define D3DCOMPILE_IEEE_STRICTNESS (1u << 13)
#define D3DCOMPILE_OPTIMIZATION_LEVEL0 (1u << 14)
#define D3DCOMPILE_OPTIMIZATION_LEVEL1 0u
#define D3DCOMPILE_OPTIMIZATION_LEVEL2 ((1u << 14) | (1u << 15))
#define D3DCOMPILE_OPTIMIZATION_LEVEL3 (1u << 15)
#define D3DCOMPILE_WARNINGS_ARE_ERRORS (1u << 18)
#define D3DCOMPILE_RESOURCES_MAY_ALIAS (1u << 19)
#define D3DCOMPILE_ALL_RESOURCES_BOUND (1u << 21)
#define D3D_COMPILE_STANDARD_FILE_INCLUDE (reinterpret_cast<ID3DInclude*>(static_cast<UINT_PTR>(1)))

HRESULT D3DCompile(LPCVOID source, SIZE_T source_size, LPCSTR source_name, const D3D_SHADER_MACRO* defines,
    ID3DInclude* include, LPCSTR entry_point, LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code,
    ID3DBlob** errors) noexcept;
HRESULT D3DReflect(LPCVOID data, SIZE_T size, REFIID iid, void** reflector) noexcept;
HRESULT D3DCreateBlob(SIZE_T size, ID3DBlob** blob) noexcept;

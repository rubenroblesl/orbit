// SPDX-License-Identifier: GPL-3.0-only
//
// DXGI formats to Metal pixel and vertex formats.
//
// Apple GPUs have no 24-bit depth formats: D24_UNORM_S8_UINT (and its typeless family) becomes
// Depth32Float_Stencil8, which keeps at least 24 bits of depth precision. Typeless color formats
// take the UNORM (or FLOAT) member of their family; views reinterpret them.

#include "metal12.h"

namespace galaxy::metal12 {

FormatInfo format_info(DXGI_FORMAT format, bool depth_resource) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return {MTLPixelFormatRGBA8Unorm, 4};
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return {MTLPixelFormatRGBA8Unorm_sRGB, 4};
    case DXGI_FORMAT_R8G8B8A8_UINT: return {MTLPixelFormatRGBA8Uint, 4};
    case DXGI_FORMAT_R8G8B8A8_SNORM: return {MTLPixelFormatRGBA8Snorm, 4};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return {MTLPixelFormatBGRA8Unorm, 4};
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return {MTLPixelFormatBGRA8Unorm_sRGB, 4};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return {MTLPixelFormatRGB10A2Unorm, 4};
    case DXGI_FORMAT_R11G11B10_FLOAT: return {MTLPixelFormatRG11B10Float, 4};
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return {MTLPixelFormatRGBA16Float, 8};
    case DXGI_FORMAT_R16G16B16A16_UNORM: return {MTLPixelFormatRGBA16Unorm, 8};
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return {MTLPixelFormatRGBA32Float, 16};
    case DXGI_FORMAT_R32G32B32A32_UINT: return {MTLPixelFormatRGBA32Uint, 16};
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT: return {MTLPixelFormatRG16Float, 4};
    case DXGI_FORMAT_R16G16_UNORM: return {MTLPixelFormatRG16Unorm, 4};
    case DXGI_FORMAT_R32G32_FLOAT: return {MTLPixelFormatRG32Float, 8};
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM: return {MTLPixelFormatRG8Unorm, 2};
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM: return {MTLPixelFormatR8Unorm, 1};
    case DXGI_FORMAT_R8_UINT: return {MTLPixelFormatR8Uint, 1};
    case DXGI_FORMAT_A8_UNORM: return {MTLPixelFormatA8Unorm, 1};
    case DXGI_FORMAT_R16_TYPELESS:
        return depth_resource ? FormatInfo{MTLPixelFormatDepth16Unorm, 2, 1, true} : FormatInfo{MTLPixelFormatR16Unorm, 2};
    case DXGI_FORMAT_R16_UNORM: return {MTLPixelFormatR16Unorm, 2};
    case DXGI_FORMAT_R16_FLOAT: return {MTLPixelFormatR16Float, 2};
    case DXGI_FORMAT_R16_UINT: return {MTLPixelFormatR16Uint, 2};
    case DXGI_FORMAT_D16_UNORM: return {MTLPixelFormatDepth16Unorm, 2, 1, true};
    case DXGI_FORMAT_R32_TYPELESS:
        return depth_resource ? FormatInfo{MTLPixelFormatDepth32Float, 4, 1, true} : FormatInfo{MTLPixelFormatR32Float, 4};
    case DXGI_FORMAT_R32_FLOAT: return {MTLPixelFormatR32Float, 4};
    case DXGI_FORMAT_R32_UINT: return {MTLPixelFormatR32Uint, 4};
    case DXGI_FORMAT_D32_FLOAT: return {MTLPixelFormatDepth32Float, 4, 1, true};
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return {MTLPixelFormatDepth32Float_Stencil8, 8, 1, true, true};
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return {MTLPixelFormatDepth32Float_Stencil8, 8, 1, true, true};
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return {MTLPixelFormatX32_Stencil8, 8, 1, false, true};
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM: return {MTLPixelFormatBC1_RGBA, 8, 4};
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM: return {MTLPixelFormatBC2_RGBA, 16, 4};
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM: return {MTLPixelFormatBC3_RGBA, 16, 4};
    // Apple GPUs on macOS sample BC1-BC7 natively (custom texture packs ship BC7).
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM: return {MTLPixelFormatBC7_RGBAUnorm, 16, 4};
    case DXGI_FORMAT_B5G6R5_UNORM: return {MTLPixelFormatB5G6R5Unorm, 2};
    case DXGI_FORMAT_B5G5R5A1_UNORM: return {MTLPixelFormatBGR5A1Unorm, 2};
    case DXGI_FORMAT_B4G4R4A4_UNORM: return {MTLPixelFormatABGR4Unorm, 2};
    default: return {};
    }
}

MTLVertexFormat vertex_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return MTLVertexFormatFloat4;
    case DXGI_FORMAT_R32G32B32_FLOAT: return MTLVertexFormatFloat3;
    case DXGI_FORMAT_R32G32_FLOAT: return MTLVertexFormatFloat2;
    case DXGI_FORMAT_R32_FLOAT: return MTLVertexFormatFloat;
    case DXGI_FORMAT_R32G32B32A32_UINT: return MTLVertexFormatUInt4;
    case DXGI_FORMAT_R32G32B32_UINT: return MTLVertexFormatUInt3;
    case DXGI_FORMAT_R32G32_UINT: return MTLVertexFormatUInt2;
    case DXGI_FORMAT_R32_UINT: return MTLVertexFormatUInt;
    case DXGI_FORMAT_R32G32B32A32_SINT: return MTLVertexFormatInt4;
    case DXGI_FORMAT_R32G32_SINT: return MTLVertexFormatInt2;
    case DXGI_FORMAT_R32_SINT: return MTLVertexFormatInt;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return MTLVertexFormatHalf4;
    case DXGI_FORMAT_R16G16_FLOAT: return MTLVertexFormatHalf2;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return MTLVertexFormatUShort4Normalized;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return MTLVertexFormatShort4Normalized;
    case DXGI_FORMAT_R16G16B16A16_UINT: return MTLVertexFormatUShort4;
    case DXGI_FORMAT_R16G16B16A16_SINT: return MTLVertexFormatShort4;
    case DXGI_FORMAT_R16G16_UNORM: return MTLVertexFormatUShort2Normalized;
    case DXGI_FORMAT_R16G16_SNORM: return MTLVertexFormatShort2Normalized;
    case DXGI_FORMAT_R16G16_UINT: return MTLVertexFormatUShort2;
    case DXGI_FORMAT_R16G16_SINT: return MTLVertexFormatShort2;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return MTLVertexFormatChar4Normalized;
    case DXGI_FORMAT_R8G8B8A8_UINT: return MTLVertexFormatUChar4;
    case DXGI_FORMAT_R8G8B8A8_SINT: return MTLVertexFormatChar4;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return MTLVertexFormatUChar4Normalized_BGRA;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return MTLVertexFormatUInt1010102Normalized;
    case DXGI_FORMAT_R8G8_UNORM: return MTLVertexFormatUChar2Normalized;
    case DXGI_FORMAT_R8G8_UINT: return MTLVertexFormatUChar2;
    case DXGI_FORMAT_R16_FLOAT: return MTLVertexFormatHalf;
    case DXGI_FORMAT_R16_UINT: return MTLVertexFormatUShort;
    case DXGI_FORMAT_R8_UNORM: return MTLVertexFormatUCharNormalized;
    case DXGI_FORMAT_R8_UINT: return MTLVertexFormatUChar;
    default: return MTLVertexFormatInvalid;
    }
}

std::string msl_fetch_expression(DXGI_FORMAT format) {
    // P is a `const device uchar*` at the element; integer formats keep their integer type.
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "float4(*(const device packed_float4*)P)";
    case DXGI_FORMAT_R32G32B32_FLOAT: return "float3(*(const device packed_float3*)P)";
    case DXGI_FORMAT_R32G32_FLOAT: return "float2(*(const device packed_float2*)P)";
    case DXGI_FORMAT_R32_FLOAT: return "*(const device float*)P";
    case DXGI_FORMAT_R32G32B32A32_UINT: return "uint4(*(const device packed_uint4*)P)";
    case DXGI_FORMAT_R32G32B32_UINT: return "uint3(*(const device packed_uint3*)P)";
    case DXGI_FORMAT_R32G32_UINT: return "uint2(*(const device packed_uint2*)P)";
    case DXGI_FORMAT_R32_UINT: return "*(const device uint*)P";
    case DXGI_FORMAT_R32G32B32A32_SINT: return "int4(*(const device packed_int4*)P)";
    case DXGI_FORMAT_R32G32_SINT: return "int2(*(const device packed_int2*)P)";
    case DXGI_FORMAT_R32_SINT: return "*(const device int*)P";
    case DXGI_FORMAT_R8G8B8A8_UINT: return "uint4(*(const device uchar4*)P)";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "float4(*(const device uchar4*)P) / 255.0";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "float4(*(const device uchar4*)P).zyxw / 255.0";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "float4(*(const device packed_half4*)P)";
    case DXGI_FORMAT_R16G16_FLOAT: return "float2(*(const device packed_half2*)P)";
    case DXGI_FORMAT_R16G16B16A16_UINT: return "uint4(*(const device packed_ushort4*)P)";
    case DXGI_FORMAT_R16G16_UINT: return "uint2(*(const device packed_ushort2*)P)";
    default: return {};
    }
}

}  // namespace galaxy::metal12

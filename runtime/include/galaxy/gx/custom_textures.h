#pragma once

// Custom (replacement) textures in Dolphin's format: a texture pack's files are named after the
// game texture they replace, `tex1_<w>x<h>[_m]_<hash>[_<tlut hash>]_<format>[_arb].dds|png`,
// where the hash is XXH64 (seed 0) of the texture's level-0 guest bytes. Packs made for Dolphin
// load unchanged, from the same layout: <directory>/RMGE01 or <directory>/RMG, plus any folder
// that holds an RMGE01.txt, RMG.txt or all.txt marker.
//
// "Load Custom Textures" indexes the pack when it is enabled; a texture file is read at the first
// use of the texture it replaces. "Prefetch Custom Textures" reads every file into memory on
// background threads right away, so first uses never wait on the disk.

#include "galaxy/gx/gx_bitfields.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace galaxy::gx {

// The parts of a Dolphin texture name (VideoCommon/TextureInfo.cpp CalculateTextureName).
struct CustomTextureName {
    std::string base;     // "tex1_64x64" or "tex1_64x64_m"
    std::string texture;  // 16 hex digits
    std::string tlut;     // "_" + 16 hex digits for indexed formats, else empty
    std::string format;   // GX format number
    [[nodiscard]] std::string full() const { return base + "_" + texture + tlut + "_" + format; }
    [[nodiscard]] std::string any_tlut() const { return base + "_" + texture + "_$_" + format; }
    [[nodiscard]] std::string any_texture() const { return base + "_$" + tlut + "_" + format; }
};

// `level0` is the block-aligned level-0 texel data as stored in guest memory; `palette` the
// format's whole TMEM palette (indexed formats only). `mipmapped` is the sampler's mip filter
// (TEX_MODE0 min filter low bits != 0), not whether the data has levels.
[[nodiscard]] CustomTextureName dolphin_texture_name(TexFormat format, unsigned width, unsigned height,
    bool mipmapped, std::span<const std::uint8_t> level0, std::span<const std::uint8_t> palette);

// A loaded replacement. Uncompressed files are converted to RGBA8; BC formats stay compressed.
enum class CustomTextureFormat : std::uint8_t { RGBA8, BC1, BC2, BC3, BC7 };

struct CustomTextureLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_bytes = 0;  // one row of pixels, or of 4x4 blocks
    std::uint32_t rows = 0;       // pixel rows, or block rows
    std::size_t offset = 0;       // into CustomTextureData::bytes
};

struct CustomTextureData {
    CustomTextureFormat format = CustomTextureFormat::RGBA8;
    std::vector<CustomTextureLevel> levels;
    std::vector<std::uint8_t> bytes;
    bool arbitrary_mipmaps = false;  // the file name carries _arb
    [[nodiscard]] std::size_t size_bytes() const noexcept { return bytes.size(); }
};

// Reads a DDS (BC1/BC2/BC3/BC7, 32-bit RGBA/BGRA, legacy or DX10 header, with its mip chain) or a
// PNG file, plus `<name>_mip<N>` sibling files for the levels a single-level file lacks.
[[nodiscard]] std::shared_ptr<const CustomTextureData> load_custom_texture(
    const std::filesystem::path& path, bool arbitrary_mipmaps, std::string& error);

struct CustomTextureSettings {
    bool enabled = false;
    bool prefetch = false;
    std::string directory;  // empty: <local app data>/Nebula/Load/Textures
    bool operator==(const CustomTextureSettings&) const = default;
};

class CustomTextureLibrary {
public:
    CustomTextureLibrary() = default;
    CustomTextureLibrary(const CustomTextureLibrary&) = delete;
    CustomTextureLibrary& operator=(const CustomTextureLibrary&) = delete;

    // Applies settings: indexes the pack again and restarts the prefetch when they change.
    // Returns whether anything changed (the caller then drops textures it already replaced).
    bool configure(const CustomTextureSettings& settings);
    [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_acquire); }

    // The replacement for a texture name (exact name, then Dolphin's `$` wildcards), loading it
    // now when it has not been read yet; null when the pack has none or the file is unusable.
    [[nodiscard]] std::shared_ptr<const CustomTextureData> find(const CustomTextureName& name);

    struct Stats {
        std::size_t indexed = 0;
        std::size_t prefetched = 0;
        std::uint64_t prefetched_bytes = 0;
        std::size_t replaced = 0;  // find() hits
    };
    [[nodiscard]] Stats stats();

private:
    struct Item {
        std::filesystem::path path;
        bool arbitrary_mipmaps = false;
        std::shared_ptr<const CustomTextureData> data;  // kept when prefetching
        bool failed = false;
    };
    void stop_prefetch();
    void prefetch_worker(std::uint64_t generation);
    [[nodiscard]] std::shared_ptr<const CustomTextureData> load_item(const std::string& key, bool keep);

    std::mutex mutex_;
    bool configured_ = false;
    CustomTextureSettings settings_;
    std::unordered_map<std::string, Item> items_;
    std::vector<std::string> prefetch_order_;
    std::atomic<std::size_t> prefetch_next_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> active_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::vector<std::thread> prefetch_threads_;
    Stats stats_;
};

// The process-wide library (never destroyed: prefetch threads may outlive static destruction).
[[nodiscard]] CustomTextureLibrary& custom_texture_library();

// Settings from the render configuration (render_config.h custom_textures*).
[[nodiscard]] CustomTextureSettings custom_texture_settings_from_config();

}  // namespace galaxy::gx

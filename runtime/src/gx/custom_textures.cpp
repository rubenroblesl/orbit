#include "galaxy/gx/custom_textures.h"

#include "galaxy/gx/render_config.h"
#include "galaxy/product.h"
#include "galaxy/windows_power.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Shlobj.h>

#define XXH_INLINE_ALL
#include "xxhash.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_ASSERT(x) ((void)0)
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>

namespace galaxy::gx {

namespace {

std::string hex64(std::uint64_t value) {
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size <= 0) return false;
    bytes.resize(static_cast<std::size_t>(size));
    in.seekg(0);
    return static_cast<bool>(in.read(reinterpret_cast<char*>(bytes.data()), size));
}

std::uint32_t u32_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    return value;  // DDS is little endian, as is every host
}

constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return std::uint32_t(std::uint8_t(a)) | (std::uint32_t(std::uint8_t(b)) << 8u) |
        (std::uint32_t(std::uint8_t(c)) << 16u) | (std::uint32_t(std::uint8_t(d)) << 24u);
}

std::uint32_t block_bytes(CustomTextureFormat format) {
    switch (format) {
    case CustomTextureFormat::BC1: return 8u;
    case CustomTextureFormat::BC2:
    case CustomTextureFormat::BC3:
    case CustomTextureFormat::BC7: return 16u;
    default: return 0u;
    }
}

// Appends one level of `width` x `height` texels taken from `source` (`row_bytes` x `rows`).
void add_level(CustomTextureData& data, std::uint32_t width, std::uint32_t height, std::uint32_t row_bytes,
    std::uint32_t rows, const std::uint8_t* source) {
    CustomTextureLevel level;
    level.width = width;
    level.height = height;
    level.row_bytes = row_bytes;
    level.rows = rows;
    level.offset = data.bytes.size();
    data.bytes.insert(data.bytes.end(), source, source + std::size_t{row_bytes} * rows);
    data.levels.push_back(level);
}

enum class DdsPixels : std::uint8_t { Unsupported, Block, RGBA, BGRA, BGRX };

// DDS: DDS_HEADER (+ DDS_HEADER_DXT10). Levels follow one another, largest first.
bool parse_dds(const std::vector<std::uint8_t>& file, CustomTextureData& data, std::string& error) {
    if (file.size() < 128u || u32_at(file, 0) != fourcc('D', 'D', 'S', ' ') || u32_at(file, 4) != 124u) {
        error = "not a DDS file";
        return false;
    }
    const std::uint32_t flags = u32_at(file, 8);
    const std::uint32_t height = u32_at(file, 12);
    const std::uint32_t width = u32_at(file, 16);
    const std::uint32_t mip_count = (flags & 0x20000u) != 0u ? std::max(1u, u32_at(file, 28)) : 1u;
    const std::uint32_t pf_flags = u32_at(file, 80);
    const std::uint32_t pf_fourcc = u32_at(file, 84);
    const std::uint32_t pf_bits = u32_at(file, 88);
    const std::uint32_t r_mask = u32_at(file, 92);
    const std::uint32_t a_mask = u32_at(file, 104);
    std::size_t offset = 128u;
    DdsPixels pixels = DdsPixels::Unsupported;
    if ((pf_flags & 0x4u) != 0u) {  // DDPF_FOURCC
        if (pf_fourcc == fourcc('D', 'X', 'T', '1')) {
            data.format = CustomTextureFormat::BC1;
            pixels = DdsPixels::Block;
        } else if (pf_fourcc == fourcc('D', 'X', 'T', '2') || pf_fourcc == fourcc('D', 'X', 'T', '3')) {
            data.format = CustomTextureFormat::BC2;
            pixels = DdsPixels::Block;
        } else if (pf_fourcc == fourcc('D', 'X', 'T', '4') || pf_fourcc == fourcc('D', 'X', 'T', '5')) {
            data.format = CustomTextureFormat::BC3;
            pixels = DdsPixels::Block;
        } else if (pf_fourcc == fourcc('D', 'X', '1', '0')) {
            if (file.size() < 148u) {
                error = "truncated DX10 header";
                return false;
            }
            offset = 148u;
            if (u32_at(file, 132) != 3u || u32_at(file, 140) > 1u) {
                error = "only 2D textures are supported";
                return false;
            }
            switch (u32_at(file, 128)) {  // DXGI_FORMAT
            case 70: case 71: case 72: data.format = CustomTextureFormat::BC1; pixels = DdsPixels::Block; break;
            case 73: case 74: case 75: data.format = CustomTextureFormat::BC2; pixels = DdsPixels::Block; break;
            case 76: case 77: case 78: data.format = CustomTextureFormat::BC3; pixels = DdsPixels::Block; break;
            case 97: case 98: case 99: data.format = CustomTextureFormat::BC7; pixels = DdsPixels::Block; break;
            case 27: case 28: case 29: pixels = DdsPixels::RGBA; break;
            case 87: case 90: case 91: pixels = DdsPixels::BGRA; break;
            case 88: case 92: case 93: pixels = DdsPixels::BGRX; break;
            default: break;
            }
        }
    } else if ((pf_flags & 0x40u) != 0u && pf_bits == 32u) {  // DDPF_RGB
        const bool alpha = (pf_flags & 0x1u) != 0u && a_mask != 0u;
        if (r_mask == 0x00ff0000u) pixels = alpha ? DdsPixels::BGRA : DdsPixels::BGRX;
        else if (r_mask == 0x000000ffu) pixels = DdsPixels::RGBA;
    }
    if (pixels == DdsPixels::Unsupported || width == 0u || height == 0u) {
        error = "unsupported DDS pixel format (BC1, BC2, BC3, BC7 and 32-bit RGBA are supported)";
        return false;
    }
    std::vector<std::uint8_t> converted;
    for (std::uint32_t level = 0; level < mip_count; ++level) {
        const std::uint32_t lw = std::max(1u, width >> level);
        const std::uint32_t lh = std::max(1u, height >> level);
        std::uint32_t row_bytes = 0;
        std::uint32_t rows = 0;
        if (pixels == DdsPixels::Block) {
            row_bytes = std::max(1u, (lw + 3u) / 4u) * block_bytes(data.format);
            rows = std::max(1u, (lh + 3u) / 4u);
        } else {
            row_bytes = lw * 4u;
            rows = lh;
        }
        const std::size_t size = std::size_t{row_bytes} * rows;
        if (offset + size > file.size()) {
            if (level == 0u) {
                error = "truncated texel data";
                return false;
            }
            break;  // keep the complete levels
        }
        const std::uint8_t* source = file.data() + offset;
        if (pixels == DdsPixels::BGRA || pixels == DdsPixels::BGRX) {
            converted.assign(source, source + size);
            for (std::size_t i = 0; i < size; i += 4u) {
                std::swap(converted[i], converted[i + 2u]);
                if (pixels == DdsPixels::BGRX) converted[i + 3u] = 0xffu;
            }
            source = converted.data();
        }
        if (pixels != DdsPixels::Block) data.format = CustomTextureFormat::RGBA8;
        add_level(data, lw, lh, row_bytes, rows, source);
        offset += size;
        if (lw == 1u && lh == 1u) break;
    }
    return true;
}

bool parse_png(const std::vector<std::uint8_t>& file, CustomTextureData& data, std::string& error) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        error = std::string("PNG decode failed: ") + stbi_failure_reason();
        return false;
    }
    data.format = CustomTextureFormat::RGBA8;
    add_level(data, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
        static_cast<std::uint32_t>(width) * 4u, static_cast<std::uint32_t>(height), pixels);
    stbi_image_free(pixels);
    return true;
}

bool parse_texture_file(const std::filesystem::path& path, CustomTextureData& data, std::string& error) {
    std::vector<std::uint8_t> file;
    if (!read_file(path, file)) {
        error = "cannot read the file";
        return false;
    }
    const std::string extension = lower(path.extension().string());
    if (extension == ".dds") return parse_dds(file, data, error);
    if (extension == ".png") return parse_png(file, data, error);
    error = "unsupported file type";
    return false;
}

std::filesystem::path default_directory() {
    wchar_t app_data[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, app_data))) return {};
    return std::filesystem::path(app_data) / galaxy::kAppDataFolder / L"Load" / L"Textures";
}

// Dolphin's GetTextureDirectoriesWithGameId for RMGE01: the RMGE01 folder (else RMG), plus the
// top folder of every RMGE01.txt / RMG.txt / all.txt marker below the root.
std::vector<std::filesystem::path> texture_directories(const std::filesystem::path& root) {
    std::set<std::filesystem::path> directories;
    std::error_code error;
    if (std::filesystem::is_directory(root / "RMGE01", error)) {
        directories.insert(root / "RMGE01");
    } else if (std::filesystem::is_directory(root / "RMG", error)) {
        directories.insert(root / "RMG");
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error) || lower(it->path().extension().string()) != ".txt") continue;
        const std::string stem = it->path().stem().string();
        if (stem != "RMGE01" && stem != "RMG" && stem != "all") continue;
        const std::filesystem::path relative = std::filesystem::relative(it->path(), root, error);
        if (!error && !relative.empty()) directories.insert(root / *relative.begin());
    }
    return {directories.begin(), directories.end()};
}

}  // namespace

CustomTextureName dolphin_texture_name(TexFormat format, unsigned width, unsigned height, bool mipmapped,
    std::span<const std::uint8_t> level0, std::span<const std::uint8_t> palette) {
    // Indexed formats hash only the palette range the texels use.
    std::size_t tlut_size = 0;
    switch (format) {
    case TexFormat::C4: tlut_size = 16u * 2u; break;
    case TexFormat::C8: tlut_size = 256u * 2u; break;
    case TexFormat::C14X2: tlut_size = 16384u * 2u; break;
    default: break;
    }
    const std::uint8_t* tlut = palette.data();
    if (tlut_size != 0u) {
        std::uint32_t min = 0xffffu;
        std::uint32_t max = 0u;
        if (format == TexFormat::C4) {
            for (const std::uint8_t byte : level0) {
                const std::uint32_t low = byte & 0xfu;
                const std::uint32_t high = byte >> 4u;
                min = std::min({min, low, high});
                max = std::max({max, low, high});
            }
        } else if (format == TexFormat::C8) {
            for (const std::uint8_t byte : level0) {
                min = std::min(min, std::uint32_t{byte});
                max = std::max(max, std::uint32_t{byte});
            }
        } else {
            // Dolphin byte-swaps a single byte here (Common::swap16 of a u8): the value is the
            // even byte shifted left by 8. Kept so C14X2 names match its packs.
            for (std::size_t i = 0; i < level0.size(); i += 2u) {
                const std::uint32_t value = (std::uint32_t{level0[i]} << 8u) & 0x3fffu;
                min = std::min(min, value);
                max = std::max(max, value);
            }
        }
        if (max < min) min = max = 0u;
        tlut_size = 2u * (max + 1u - min);
        tlut += 2u * min;
        if (palette.size() < 2u * min + tlut_size) tlut_size = 0u;
    }

    CustomTextureName name;
    name.base = "tex1_" + std::to_string(width) + "x" + std::to_string(height) + (mipmapped ? "_m" : "");
    name.texture = hex64(XXH64(level0.data(), level0.size(), 0));
    if (tlut_size != 0u) name.tlut = "_" + hex64(XXH64(tlut, tlut_size, 0));
    name.format = std::to_string(static_cast<unsigned>(format));
    return name;
}

std::shared_ptr<const CustomTextureData> load_custom_texture(
    const std::filesystem::path& path, bool arbitrary_mipmaps, std::string& error) {
    auto data = std::make_shared<CustomTextureData>();
    data->arbitrary_mipmaps = arbitrary_mipmaps;
    if (!parse_texture_file(path, *data, error)) return nullptr;
    // Lower levels in <name>_mip<N> files next to the level-0 file.
    if (data->levels.size() == 1u) {
        const std::filesystem::path stem = path.parent_path() / path.stem();
        const std::string extension = path.extension().string();
        for (unsigned level = 1; level < 16u; ++level) {
            const CustomTextureLevel& previous = data->levels.back();
            if (previous.width == 1u && previous.height == 1u) break;
            const std::filesystem::path mip = stem.string() + "_mip" + std::to_string(level) + extension;
            std::error_code exists_error;
            if (!std::filesystem::is_regular_file(mip, exists_error)) break;
            CustomTextureData level_data;
            std::string level_error;
            if (!parse_texture_file(mip, level_data, level_error) || level_data.format != data->format) {
                error = mip.filename().string() + ": " + (level_error.empty() ? "format differs from level 0" : level_error);
                return nullptr;
            }
            const CustomTextureLevel& first = level_data.levels.front();
            if (first.width != std::max(1u, data->levels.front().width >> level) ||
                first.height != std::max(1u, data->levels.front().height >> level)) {
                error = mip.filename().string() + ": wrong mip size";
                return nullptr;
            }
            add_level(*data, first.width, first.height, first.row_bytes, first.rows,
                level_data.bytes.data() + first.offset);
        }
    }
    return data;
}

bool CustomTextureLibrary::configure(const CustomTextureSettings& settings) {
    {
        std::lock_guard lock(mutex_);
        if (configured_ && settings == settings_) return false;
    }
    stop_prefetch();
    std::lock_guard lock(mutex_);
    configured_ = true;
    settings_ = settings;
    items_.clear();
    prefetch_order_.clear();
    stats_ = {};
    const std::uint64_t generation = generation_.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    if (!settings.enabled) {
        active_.store(false, std::memory_order_release);
        return true;
    }
    const std::filesystem::path root =
        settings.directory.empty() ? default_directory() : std::filesystem::path(settings.directory);
    std::size_t duplicates = 0;
    std::size_t unreadable = 0;
    for (const std::filesystem::path& directory : texture_directories(root)) {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
            const std::string extension = lower(it->path().extension().string());
            if ((extension == ".dds" || extension == ".png") && it->is_regular_file(error)) files.push_back(it->path());
        }
        if (error) ++unreadable;
        // Dolphin sorts the search and keeps the first file of each name.
        std::sort(files.begin(), files.end());
        for (const std::filesystem::path& file : files) {
            std::string name = file.stem().string();
            if (name.rfind("tex1_", 0) != 0u) continue;
            const std::size_t arb = name.rfind("_arb");
            const bool arbitrary = arb != std::string::npos;
            if (arbitrary) name.erase(arb, 4u);
            const auto [it, inserted] = items_.try_emplace(name, Item{file, arbitrary, nullptr, false});
            if (inserted) {
                prefetch_order_.push_back(name);
            } else {
                ++duplicates;
            }
        }
    }
    stats_.indexed = items_.size();
    active_.store(!items_.empty(), std::memory_order_release);
    std::cerr << "[custom-textures] " << items_.size() << " textures for RMGE01 in " << root.string();
    if (duplicates != 0u) std::cerr << " (" << duplicates << " duplicate names: the first path wins, as in Dolphin)";
    if (unreadable != 0u) std::cerr << " (" << unreadable << " folders could not be read)";
    std::cerr << (items_.empty() ? "; nothing to replace\n" : "\n");
    if (settings.prefetch && !items_.empty()) {
        stop_.store(false, std::memory_order_release);
        prefetch_next_.store(0u, std::memory_order_release);
        const unsigned workers = std::clamp(std::thread::hardware_concurrency() / 2u, 2u, 6u);
        for (unsigned i = 0; i < workers; ++i) {
            prefetch_threads_.emplace_back([this, generation] { prefetch_worker(generation); });
        }
    }
    return true;
}

void CustomTextureLibrary::stop_prefetch() {
    stop_.store(true, std::memory_order_release);
    for (std::thread& thread : prefetch_threads_) {
        if (thread.joinable()) thread.join();
    }
    prefetch_threads_.clear();
}

void CustomTextureLibrary::prefetch_worker(std::uint64_t generation) {
    galaxy::host::name_current_thread(L"Nebula Custom Texture Prefetch");
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        if (stop_.load(std::memory_order_acquire) || generation_.load(std::memory_order_acquire) != generation) return;
        const std::size_t index = prefetch_next_.fetch_add(1u, std::memory_order_acq_rel);
        std::string key;
        {
            std::lock_guard lock(mutex_);
            if (index >= prefetch_order_.size()) break;
            key = prefetch_order_[index];
        }
        (void)load_item(key, true);
    }
    // The last worker to finish reports the pack.
    std::lock_guard lock(mutex_);
    if (prefetch_next_.load(std::memory_order_acquire) == prefetch_order_.size() + prefetch_threads_.size()) {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cerr << "[custom-textures] prefetched " << stats_.prefetched << " textures ("
                  << (stats_.prefetched_bytes >> 20u) << " MB) in " << seconds << " s\n";
    }
}

std::shared_ptr<const CustomTextureData> CustomTextureLibrary::load_item(const std::string& key, bool keep) {
    std::filesystem::path path;
    bool arbitrary = false;
    {
        std::lock_guard lock(mutex_);
        const auto it = items_.find(key);
        if (it == items_.end() || it->second.failed) return nullptr;
        if (it->second.data) return it->second.data;
        path = it->second.path;
        arbitrary = it->second.arbitrary_mipmaps;
    }
    std::string error;
    std::shared_ptr<const CustomTextureData> data = load_custom_texture(path, arbitrary, error);
    std::lock_guard lock(mutex_);
    const auto it = items_.find(key);
    if (it == items_.end()) return data;  // reconfigured meanwhile
    if (!data) {
        if (!it->second.failed) {
            std::cerr << "[custom-textures] " << path.filename().string() << ": " << error << '\n';
        }
        it->second.failed = true;
        return nullptr;
    }
    if (keep && !it->second.data) {
        it->second.data = data;
        ++stats_.prefetched;
        stats_.prefetched_bytes += data->size_bytes();
    }
    return it->second.data ? it->second.data : data;
}

std::shared_ptr<const CustomTextureData> CustomTextureLibrary::find(const CustomTextureName& name) {
    if (!active()) return nullptr;
    std::string key;
    bool keep = false;
    {
        std::lock_guard lock(mutex_);
        for (const std::string& candidate : {name.full(), name.any_tlut(), name.any_texture()}) {
            if (items_.find(candidate) != items_.end()) {
                key = candidate;
                break;
            }
        }
        if (key.empty()) return nullptr;
        keep = settings_.prefetch;
    }
    std::shared_ptr<const CustomTextureData> data = load_item(key, keep);
    if (data) {
        std::lock_guard lock(mutex_);
        ++stats_.replaced;
    }
    return data;
}

CustomTextureLibrary::Stats CustomTextureLibrary::stats() {
    std::lock_guard lock(mutex_);
    return stats_;
}

CustomTextureLibrary& custom_texture_library() {
    static CustomTextureLibrary* library = new CustomTextureLibrary();
    return *library;
}

CustomTextureSettings custom_texture_settings_from_config() {
    const RenderConfig config = get_render_config();
    CustomTextureSettings settings;
    settings.enabled = config.custom_textures;
    settings.prefetch = config.custom_textures && config.prefetch_custom_textures;
    settings.directory = config.custom_textures_dir.data();
    return settings;
}

}  // namespace galaxy::gx

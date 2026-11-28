#pragma once

#include <array>
#include <functional>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace flash
{
using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

enum class Category { timeline, scripts, images, shapes, sounds, text, fonts, sprites, binary, other };
std::wstring_view category_name(Category category);
std::string_view tag_name(unsigned code);

struct Tag
{
    unsigned code;
    Category category;
    std::size_t header_offset;
    std::size_t offset;
    std::size_t size;
    std::optional<std::size_t> parent;
    std::optional<unsigned> character_id;
    std::string name;
    std::size_t next_sibling;
};

struct Header
{
    char compression;
    unsigned version;
    std::size_t stored_size;
    std::size_t expanded_size;
    std::int32_t left, right, top, bottom;
    double frames_per_second;
    unsigned frame_count;
};

class Movie
{
public:
    static Movie open(const std::filesystem::path& path);
    static Movie parse(ByteView bytes);
    static Movie import_xml(ByteView xml);

    const Header& header() const noexcept { return header_; }
    const std::vector<Tag>& tags() const noexcept { return tags_; }
    ByteView data(std::size_t index) const;
    std::optional<std::size_t> character(unsigned id) const;
    const std::optional<std::string>& document_class() const noexcept { return document_class_; }
    ByteView uncompressed() const noexcept { return bytes_; }
    Bytes compressed() const;
    std::string export_xml() const;
    Movie replace_tag(std::size_t index, ByteView replacement, std::optional<unsigned> new_code = {}) const;
    Movie insert_root_tag(std::size_t before, unsigned code, ByteView payload) const;
    Movie replace_binary(std::size_t index, ByteView replacement) const;
    Movie replace_text(std::size_t index, std::string_view utf8) const;
    Movie replace_image(std::size_t index, ByteView encoded) const;
    Movie replace_sound(std::size_t index, const std::filesystem::path& audio) const;

private:
    static Movie decode(ByteView bytes, Bytes&& owned = {});
    void read_tags(std::size_t start, std::size_t end, std::optional<std::size_t> parent, unsigned depth);
    void resolve_names();

    Header header_{};
    std::size_t tag_offset_{};
    Bytes bytes_;
    std::vector<Tag> tags_;
    std::unordered_map<unsigned, std::size_t> characters_;
    std::optional<std::string> document_class_;
};

struct Asset
{
    std::wstring extension;
    Bytes bytes;
};

struct FramePreview
{
    Asset image;
    unsigned omitted{};
    bool viewport_changed = false;
};

enum class AnimationFormat { gif, apng, svg };

struct SpriteAnimation
{
    Asset image;
    unsigned frames_with_omissions = 0;
};

class TimelinePreview
{
public:
    TimelinePreview(std::shared_ptr<const Movie> movie, std::optional<std::size_t> sprite = {});
    ~TimelinePreview();
    FramePreview render(unsigned frame);

private:
    struct State;
    std::unique_ptr<State> state_;
};

struct ExportReport
{
    std::size_t files = 0;
    std::size_t failed = 0;
    std::vector<std::string> errors;
};

unsigned frame_count(const Movie& movie, std::optional<std::size_t> sprite = {});
double playback_fps(const Movie& movie) noexcept;
std::wstring_view animation_extension(AnimationFormat format) noexcept;
FramePreview render_frame(const Movie& movie, unsigned frame, std::optional<std::size_t> sprite = {});
void render_sprite_frames(const Movie& movie, std::size_t sprite, const std::function<void(unsigned, FramePreview)>& accept);
SpriteAnimation export_sprite_animation(const Movie& movie, std::size_t sprite, AnimationFormat format, double scale = 1);
ExportReport export_all(const Movie& movie, const std::filesystem::path& destination);
ExportReport export_visuals(const Movie& movie, const std::filesystem::path& destination, std::wstring_view extension);
ExportReport export_project(const Movie& movie, const std::filesystem::path& destination);

ExportReport export_html5(const Movie& movie, const std::filesystem::path& destination, std::string_view title,
    const std::filesystem::path& source = {}, const std::filesystem::path& workspace = {});
std::string html5_classes(const Movie& movie);
struct WebWorkspaceReport
{
    std::size_t converted{}, partial{}, failed{};
};
using WebMovieOverrides = std::vector<std::pair<std::filesystem::path, std::shared_ptr<const Movie>>>;
struct WebWorkspaceMovie
{
    std::filesystem::path path;
    std::vector<std::string> routes;
    std::shared_ptr<const Movie> edited;
};
struct WebWorkspacePlan
{
    std::filesystem::path root, project, public_root;
    std::vector<WebWorkspaceMovie> movies;
    std::vector<std::filesystem::path> public_files;
    std::string warnings;
};
WebWorkspacePlan plan_html5_workspace(const std::filesystem::path& root, const WebMovieOverrides& open_movies = {});
WebWorkspaceReport export_html5_workspace(const WebWorkspacePlan& plan, const std::filesystem::path& destination,
    std::span<const std::size_t> selection, const std::filesystem::path& preferred = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {});
WebWorkspaceReport export_html5_workspace(const std::filesystem::path& root, const std::filesystem::path& destination,
    const std::filesystem::path& preferred = {}, const WebMovieOverrides& open_movies = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {});

std::optional<Asset> extract(const Movie& movie, std::size_t index);
bool is_visual_tag(unsigned code) noexcept;
Asset visual_asset(const Movie& movie, std::size_t index);
Asset convert_image(const Asset& image, std::wstring_view extension, double scale = 1);
std::string extract_text(const Movie& movie, std::size_t index);
Asset text_svg(const Movie& movie, std::size_t index);
Asset font_svg(const Movie& movie, std::size_t index);
struct FontPreview
{
    std::string name, full_name, copyright;
    std::vector<wchar_t> characters;
    unsigned units = 1024;
    std::optional<std::array<int, 3>> metrics;
    bool bold{}, italic{}, cff{};
};
FontPreview font_preview(const Movie& movie, std::size_t index);
std::vector<std::string> font_outlines(const Movie& movie, std::size_t index, std::span<const std::size_t> glyphs);
std::string binary_swf_summary(ByteView data);
std::string binary_preview(const Movie& movie, std::size_t index);
bool is_bitmap_tag(unsigned code) noexcept;
bool has_avm1_actions(const Movie& movie, std::size_t index);
std::string disassemble(const Movie& movie, std::size_t index);
std::string decompile(const Movie& movie, std::size_t index);
std::string hex_dump(ByteView bytes, std::size_t limit = 4096);
std::string inspect(const Movie& movie, std::size_t index);
std::string summarize(const Movie& movie);
Bytes read_file(const std::filesystem::path& path);
void write_file(const std::filesystem::path& path, ByteView bytes);
void write_text(const std::filesystem::path& path, std::string_view text);
}

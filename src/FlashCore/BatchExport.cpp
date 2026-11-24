#include "Swf.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <thread>

namespace flash
{
namespace
{
ExportReport export_tags(const Movie& movie, const std::filesystem::path& destination, std::wstring_view visual_format)
{
    std::filesystem::create_directories(destination);
    if (!visual_format.empty() && !std::filesystem::is_empty(destination))
        throw std::runtime_error("Choisissez un dossier vide pour exporter les visuels.");
    ExportReport report;
    std::atomic_size_t next{0};
    std::mutex report_mutex;
    auto export_tags = [&]
    {
        std::size_t files = 0;
        for (auto index = next++; index < movie.tags().size(); index = next++)
        {
            const auto& tag = movie.tags()[index];
            if (tag.category == Category::timeline)
                continue;
            if (!visual_format.empty() && (!is_visual_tag(tag.code) ||
                (visual_format == L".svg" && tag.category == Category::images)))
                continue;
            try
            {
                const auto type = tag_name(tag.code);
                const auto base = std::format(L"{:06}-{}", index, std::wstring(type.begin(), type.end()));
                if (!visual_format.empty())
                {
                    const auto asset = convert_image(visual_asset(movie, index), visual_format);
                    write_file(destination / (base + asset.extension), asset.bytes);
                    ++files;
                    continue;
                }
                if (const auto asset = extract(movie, index))
                {
                    write_file(destination / (base + asset->extension), asset->bytes);
                    ++files;
                }
                if (tag.code == 11 || tag.code == 33)
                {
                    write_file(destination / (base + L".svg"), text_svg(movie, index).bytes);
                    ++files;
                }
                if (tag.code == 72 || tag.code == 82 || has_avm1_actions(movie, index))
                {
                    write_text(destination / (base + L".instructions.txt"), disassemble(movie, index));
                    ++files;
                    write_text(destination / (base + L".as"), decompile(movie, index));
                    ++files;
                }
            }
            catch (const std::exception& error)
            {
                const std::scoped_lock lock(report_mutex);
                ++report.failed;
                if (report.errors.size() < 20)
                    report.errors.push_back(std::format("{} (tag {}) : {}", tag_name(tag.code), index, error.what()));
            }
        }
        const std::scoped_lock lock(report_mutex);
        report.files += files;
    };

    const auto worker_count = std::clamp(std::thread::hardware_concurrency(), 1u, 4u);
    {
        std::vector<std::jthread> workers;
        for (unsigned i = 1; i < worker_count; ++i)
            workers.emplace_back(export_tags);
        export_tags();
    }
    return report;
}
}

ExportReport export_all(const Movie& movie, const std::filesystem::path& destination)
{
    return export_tags(movie, destination, {});
}

ExportReport export_visuals(const Movie& movie, const std::filesystem::path& destination, std::wstring_view extension)
{
    if (extension != L".svg" && extension != L".png" && extension != L".bmp" && extension != L".jpg" && extension != L".tiff")
        throw std::runtime_error("Formats d’export par lot : .svg, .png, .bmp, .jpg ou .tiff.");
    return export_tags(movie, destination, extension);
}
}

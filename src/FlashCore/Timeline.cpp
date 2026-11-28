#include "Binary.hpp"
#include "ShapeEditing.hpp"
#include "WebExport.hpp"

#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <utility>

namespace flash
{
namespace
{
struct Position
{
    double a = 1, b = 0, c = 0, d = 1, x = 0, y = 0;
    bool operator==(const Position&) const = default;
};

Position operator*(const Position& l, const Position& r)
{
    return {l.a * r.a + l.c * r.b, l.b * r.a + l.d * r.b,
        l.a * r.c + l.c * r.d, l.b * r.c + l.d * r.d,
        l.a * r.x + l.c * r.y + l.x, l.b * r.x + l.d * r.y + l.y};
}

Position position(Matrix m) { return {m.scale_x, m.skew_y, m.skew_x, m.scale_y, static_cast<double>(m.x), static_cast<double>(m.y)}; }

std::string transform(const Position& p)
{
    return std::format("matrix({:g} {:g} {:g} {:g} {:g} {:g})", p.a, p.b, p.c, p.d, p.x, p.y);
}

struct Bounds
{
    double x1{}, y1{}, x2{}, y2{};
    bool valid = false;

    bool operator==(const Bounds&) const = default;

    void add(double x, double y)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
            throw std::runtime_error("Transformation de scène hors limites.");
        if (!valid)
            *this = {x, y, x, y, true};
        x1 = std::min(x1, x);
        y1 = std::min(y1, y);
        x2 = std::max(x2, x);
        y2 = std::max(y2, y);
    }

    void add(const Bounds& shape, const Position& p)
    {
        for (const auto x : {shape.x1, shape.x2})
            for (const auto y : {shape.y1, shape.y2})
                add(p.a * x + p.c * y + p.x, p.b * x + p.d * y + p.y);
    }
};

struct ColorTransform
{
    std::array<int, 4> multiplier{256, 256, 256, 256};
    std::array<int, 4> addition{};
    bool operator==(const ColorTransform&) const = default;

    double opacity() const { return std::clamp(multiplier[3] / 256.0 + addition[3] / 255.0, 0.0, 1.0); }
};

struct Instance
{
    unsigned id{}, placed{}, clip_end{}, ratio{};
    Position transform;
    double opacity = 1.0;
    ColorTransform color;
    bool unsupported = false, visible = true;
    std::string name;
    std::string filters;
    unsigned blend{};
    bool operator==(const Instance&) const = default;
};

struct Display
{
    std::map<unsigned, Instance> layers;
    std::optional<std::array<unsigned, 3>> background;
};

ColorTransform read_color_transform(Reader& input, bool alpha)
{
    BitReader bits(input);
    const auto has_addition = bits.unsigned_bits(1), has_multiplier = bits.unsigned_bits(1);
    const auto width = bits.unsigned_bits(4);
    ColorTransform color;
    const auto channels = alpha ? 4 : 3;
    if (has_multiplier)
        for (int channel = 0; channel < channels; ++channel)
            color.multiplier[channel] = bits.signed_bits(width);
    if (has_addition)
        for (int channel = 0; channel < channels; ++channel)
            color.addition[channel] = bits.signed_bits(width);
    return color;
}

std::string read_filters(Reader& input, bool* omitted)
{
    std::string filters;
    const auto emit = [&](std::string filter)
    {
        if (!filters.empty())
            filters += ',';
        filters += std::move(filter);
    };
    const auto fixed = [&] { return std::bit_cast<std::int32_t>(input.u32()) / 65536.0; };
    const auto scalar = [&] { return static_cast<double>(std::bit_cast<float>(input.u32())); };
    const auto count = input.u8();
    for (unsigned i = 0; i < count; ++i)
    {
        const auto kind = input.u8();
        if (omitted)
            switch (kind)
            {
            case 0: case 1: case 2:
            {
                std::array<unsigned, 4> color{};
                if (kind != 1)
                    for (auto& channel : color)
                        channel = input.u8();
                const auto x = fixed(), y = fixed();
                const auto angle = kind == 0 ? fixed() : 0;
                const auto distance = kind == 0 ? fixed() : 0;
                const auto strength = kind == 1 ? 1.0 : input.u16() / 256.0;
                const auto flags = input.u8();
                if (x < 0 || y < 0 || x > 255 || y > 255 || std::abs(distance) > 255)
                {
                    *omitted = true;
                    continue;
                }
                if (kind == 1)
                    emit(std::format("[1,{:g},{:g},{}]", x, y, flags >> 3));
                else
                    emit(std::format("[{},{:g},{:g},{:g},{:g},\"#{:02x}{:02x}{:02x}\",{:g},{},{}]",
                        kind, x, y, distance * std::cos(angle), distance * std::sin(angle), color[0], color[1], color[2],
                        color[3] * strength / 255.0, flags & 0xe0, flags & 31));
                continue;
            }
            case 6:
            {
                std::string matrix = "[6";
                bool finite = true;
                for (unsigned term = 0; term < 20; ++term)
                {
                    const auto value = scalar();
                    finite &= std::isfinite(value);
                    matrix += std::format(",{:g}", term % 5 == 4 ? value / 255.0 : value);
                }
                if (finite)
                    emit(matrix + ']');
                else
                    *omitted = true;
                continue;
            }
            case 5:
            {
                const auto columns = input.u8(), rows = input.u8();
                const auto divisor = scalar(), bias = scalar();
                std::vector<double> kernel(columns * rows);
                bool finite = std::isfinite(divisor) && std::isfinite(bias);
                for (auto& coefficient : kernel)
                {
                    coefficient = scalar();
                    finite &= std::isfinite(coefficient);
                }
                input.skip(3);
                const auto alpha = input.u8();
                const auto flags = input.u8();
                if (!finite || !divisor || !columns || !rows || columns > 15 || rows > 15 || (!(flags & 2) && alpha))
                {
                    *omitted = true;
                    continue;
                }
                auto filter = std::format("[5,{},{},{:g},{:g},{},[", columns, rows, divisor, bias / 255.0, flags);
                for (auto coefficient = kernel.rbegin(); coefficient != kernel.rend(); ++coefficient)
                {
                    if (coefficient != kernel.rbegin())
                        filter += ',';
                    filter += std::format("{:g}", *coefficient);
                }
                emit(filter + "]]");
                continue;
            }
            default: *omitted = true; break;
            }
        switch (kind)
        {
        case 0: input.skip(23); break;
        case 1: input.skip(9); break;
        case 2: input.skip(15); break;
        case 3: input.skip(27); break;
        case 4: case 7:
        {
            const auto colors = input.u8();
            input.skip(5 * colors + 19);
            break;
        }
        case 5:
        {
            const auto columns = input.u8(), rows = input.u8();
            input.skip(4 * columns * rows + 13);
            break;
        }
        case 6: input.skip(80); break;
        default: throw std::runtime_error("Filtre SWF inconnu.");
        }
    }
    return filters;
}

std::string_view blend_name(unsigned mode)
{
    constexpr std::string_view names[]{"", "", "isolate", "multiply", "screen", "lighten", "darken",
        "difference", "plus-lighter", "", "", "", "", "overlay", "hard-light"};
    return mode < std::size(names) ? names[mode] : std::string_view{};
}

void place_object(Display& display, unsigned code, ByteView payload, unsigned frame, bool* omitted = nullptr)
{
    Reader input(payload);
    if (code == 4)
    {
        Instance item;
        item.id = input.u16();
        const auto depth = input.u16();
        item.placed = frame;
        item.transform = position(read_matrix(input));
        if (input.remaining())
            item.color = read_color_transform(input, false);
        display.layers[depth] = item;
        return;
    }

    const auto flags = input.u8();
    const auto extra = code == 70 || code == 94 ? input.u8() : 0;
    const auto depth = input.u16();
    const auto previous = display.layers.find(depth);
    const auto move = (flags & 1) != 0;
    Instance item = move && previous != display.layers.end() ? previous->second : Instance{};
    // A bitmap embedded by character ID starts with the ID, even when HasImage is set.
    if (extra & 8)
        input.cstring();
    if (flags & 2)
    {
        const auto id = input.u16();
        if (id != item.id)
            item.ratio = 0;
        item.id = id;
        item.placed = frame;
    }
    if (flags & 4)
        item.transform = position(read_matrix(input));
    if (flags & 8)
    {
        item.color = read_color_transform(input, true);
        item.opacity = item.color.opacity();
    }
    if (flags & 16)
        item.ratio = input.u16();
    if (flags & 32)
        item.name = input.cstring();
    if (flags & 64)
        item.clip_end = input.u16();
    if (!item.id)
        return;
    if ((extra & 32) || omitted)
    {
        if (extra & 1)
            item.filters = read_filters(input, omitted);
        if (extra & 2)
        {
            item.blend = input.u8();
            if (omitted && item.blend > 1 && blend_name(item.blend).empty())
                *omitted = true;
        }
        if (extra & 4)
            input.skip(1);
    }
    if (extra & 32)
        item.visible = input.u8() != 0;
    if (omitted && (extra & 64))
        *omitted = true;
    if (extra & 67)
        item.unsupported = true;
    display.layers[depth] = item;
}

Display display_at(const Movie& movie, std::optional<std::size_t> sprite, unsigned frame)
{
    Display display;
    unsigned current = 1;
    for (std::size_t i = sprite ? *sprite + 1 : 0; i < movie.tags().size(); i = movie.tags()[i].next_sibling)
    {
        const auto code = movie.tags()[i].code;
        switch (code)
        {
        case 0: return display;
        case 1: if (current++ == frame) return display; break;
        case 4: case 26: case 70: case 94:
            place_object(display, code, movie.data(i), current);
            break;
        case 5: case 28:
        {
            Reader input(movie.data(i));
            if (code == 5)
                input.skip(2);
            display.layers.erase(input.u16());
            break;
        }
        case 9:
        {
            Reader input(movie.data(i));
            display.background = {input.u8(), input.u8(), input.u8()};
            break;
        }
        default: break;
        }
    }
    return display;
}

struct Shape
{
    Bounds bounds;
    std::string definition;
    std::string paths;
};

class Scene
{
public:
    explicit Scene(const Movie& movie) : movie_(movie) {}
    FramePreview render(unsigned frame, std::optional<std::size_t> sprite, Bounds* viewport = nullptr);
    Bounds measure(unsigned frame, std::size_t sprite)
    {
        draw(frame, sprite);
        return bounds_;
    }
private:
    Display draw(unsigned frame, std::optional<std::size_t> sprite);
    const Shape* shape(std::size_t index);
    void layers(const Display& display, unsigned frame, const Position& parent, unsigned depth);
    void add(std::string& output, std::string_view piece);

    const Movie& movie_;
    std::map<std::size_t, std::optional<Shape>> shapes_;
    std::map<std::pair<std::size_t, unsigned>, Display> sprites_;
    std::set<std::size_t> defined_;
    std::size_t shape_bytes_ = 0, cached_layers_ = 0;
    Bounds bounds_;
    std::string definitions_, objects_;
    unsigned omissions_ = 0, visited_ = 0, mask_ = 0;
};

void Scene::add(std::string& output, std::string_view piece)
{
    constexpr std::size_t max_svg_size = 16 * 1024 * 1024;
    if (piece.size() > max_svg_size - definitions_.size() - objects_.size())
        throw std::runtime_error("L’image SVG de cette scène dépasse 16 Mio.");
    output += piece;
}

const Shape* Scene::shape(std::size_t index)
{
    auto [found, created] = shapes_.try_emplace(index);
    if (created)
    {
        try
        {
            const auto code = movie_.tags()[index].code;
            if (movie_.tags()[index].category == Category::images)
            {
                const auto bitmap = embedded_bitmap(movie_, index);
                const auto width = bitmap.width * 20.0, height = bitmap.height * 20.0;
                const auto id = std::format("image-tag-{}", index);
                const auto definition = std::format("<image id=\"{}\" width=\"{:g}\" height=\"{:g}\" "
                    "xlink:href=\"{}\"/>\n", id, width, height, bitmap.data_uri);
                found->second = Shape{{0, 0, width, height, true}, definition,
                    std::format("<use xlink:href=\"#{}\"/>\n", id)};
            }
            else
            {
                const auto asset = code == 11 || code == 33 ? std::optional{text_svg(movie_, index)} : extract(movie_, index);
                if (!asset || asset->extension != L".svg")
                    return nullptr;
                Reader input(movie_.data(index));
                input.skip(2);
                BitReader bits(input);
                const auto width = bits.unsigned_bits(5);
                const double x1 = bits.signed_bits(width), x2 = bits.signed_bits(width);
                const double y1 = bits.signed_bits(width), y2 = bits.signed_bits(width);
                std::string_view svg(reinterpret_cast<const char*>(asset->bytes.data()), asset->bytes.size());
                svg = svg.substr(svg.find('>') + 1);
                svg = svg.substr(0, svg.rfind("</svg>"));
                std::string definition;
                if (const auto end = svg.find("</defs>"); end != std::string_view::npos)
                {
                    const auto start = svg.find("<defs>") + 6;
                    definition = svg.substr(start, end - start);
                    svg.remove_prefix(end + 7);
                }
                found->second = Shape{{x1, y1, x2, y2, true}, std::move(definition), std::string(svg)};
            }
            if (const auto& item = found->second)
                shape_bytes_ += item->definition.size() + item->paths.size();
        }
        catch (const std::runtime_error&)
        {
            // An unsupported definition does not prevent the other layers from rendering.
            found->second.reset();
        }
    }
    if (const auto& item = found->second; item && !item->definition.empty() && defined_.insert(index).second)
        add(definitions_, item->definition);
    return found->second ? &*found->second : nullptr;
}

void Scene::layers(const Display& display, unsigned frame, const Position& parent, unsigned depth)
{
    if (depth > 24)
    {
        ++omissions_;
        return;
    }
    struct Clip { unsigned end, id; };
    std::vector<Clip> clips;
    for (const auto& [at, item] : display.layers)
    {
        if (!item.visible)
            continue;
        if (++visited_ > 12'000)
            throw std::runtime_error("Trop d’instances dans cette image.");
        std::erase_if(clips, [at](const Clip& clip) { return clip.end < at; });
        const auto index = movie_.character(item.id);
        if (!index || item.opacity == 0)
            continue;
        const auto code = movie_.tags()[*index].code;
        const auto vector = code == 2 || code == 22 || code == 32 || code == 83 || code == 11 || code == 33 ||
            movie_.tags()[*index].category == Category::images;
        const auto outline = vector ? shape(*index) : nullptr;
        if (item.clip_end > at)
        {
            if (outline)
            {
                const auto id = ++mask_;
                add(definitions_, std::format("<clipPath id=\"mask-{}\"><g transform=\"{}\">", id, transform(item.transform)));
                add(definitions_, outline->paths);
                add(definitions_, "</g></clipPath>\n");
                clips.push_back({item.clip_end, id});
            }
            else
                ++omissions_;
            continue;
        }
        if (!outline && code != 39)
        {
            ++omissions_;
            continue;
        }
        if (item.unsupported)
            ++omissions_;
        for (const auto& clip : clips)
            add(objects_, std::format("<g clip-path=\"url(#mask-{})\">", clip.id));
        add(objects_, std::format("<g transform=\"{}\" opacity=\"{:g}\">\n", transform(item.transform), item.opacity));
        const auto world = parent * item.transform;
        if (outline)
        {
            bounds_.add(outline->bounds, world);
            add(objects_, outline->paths);
        }
        else
        {
            const auto count = std::max(1u, flash::frame_count(movie_, *index));
            const auto local = (frame - item.placed) % count + 1;
            auto [child, created] = sprites_.try_emplace({*index, local});
            if (created)
            {
                child->second = display_at(movie_, *index, local);
                cached_layers_ += child->second.layers.size();
            }
            layers(child->second, local, world, depth + 1);
        }
        add(objects_, "</g>\n");
        for (std::size_t i = 0; i < clips.size(); ++i)
            add(objects_, "</g>");
    }
}

Display Scene::draw(unsigned frame, std::optional<std::size_t> sprite)
{
    if (!frame || frame > std::max(1u, flash::frame_count(movie_, sprite)))
        throw std::runtime_error("Numéro d’image hors limites.");
    if (shape_bytes_ > 24 * 1024 * 1024 || shapes_.size() > 512)
    {
        shapes_.clear();
        shape_bytes_ = 0;
    }
    if (cached_layers_ > 32'000 || sprites_.size() > 256)
    {
        sprites_.clear();
        cached_layers_ = 0;
    }
    defined_.clear();
    definitions_.clear();
    objects_.clear();
    bounds_ = {};
    omissions_ = visited_ = mask_ = 0;
    auto display = display_at(movie_, sprite, frame);
    layers(display, frame, {}, 0);
    if (!sprite)
    {
        const auto& h = movie_.header();
        bounds_ = {static_cast<double>(h.left), static_cast<double>(h.top),
            static_cast<double>(h.right), static_cast<double>(h.bottom), true};
    }
    return display;
}

FramePreview Scene::render(unsigned frame, std::optional<std::size_t> sprite, Bounds* viewport)
{
    const auto display = draw(frame, sprite);
    bool viewport_changed = false;
    if (sprite && viewport)
    {
        const auto previous = *viewport;
        if (bounds_.valid)
        {
            viewport->add(bounds_.x1, bounds_.y1);
            viewport->add(bounds_.x2, bounds_.y2);
        }
        viewport_changed = *viewport != previous;
        bounds_ = *viewport;
    }
    const auto width = std::max(20.0, bounds_.x2 - bounds_.x1), height = std::max(20.0, bounds_.y2 - bounds_.y1);
    const auto scale = std::min({1.0, 4096.0 * 20 / width, 4096.0 * 20 / height});
    auto svg = std::format("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "width=\"{:g}\" height=\"{:g}\" "
        "viewBox=\"{:g} {:g} {:g} {:g}\">\n", width * scale / 20, height * scale / 20,
        bounds_.x1, bounds_.y1, width, height);
    if (!definitions_.empty())
        svg += "<defs>\n" + definitions_ + "</defs>\n";
    if (!sprite && display.background)
    {
        const auto color = *display.background;
        svg += std::format("<rect x=\"{:g}\" y=\"{:g}\" width=\"{:g}\" height=\"{:g}\" fill=\"#{:02X}{:02X}{:02X}\"/>\n",
            bounds_.x1, bounds_.y1, width, height, color[0], color[1], color[2]);
    }
    svg += objects_;
    svg += "</svg>\n";
    return {{L".svg", Bytes(svg.begin(), svg.end())}, omissions_, viewport_changed};
}
}

struct TimelinePreview::State
{
    State(std::shared_ptr<const Movie> movie, std::optional<std::size_t> sprite)
        : movie(std::move(movie)), scene(*this->movie), sprite(sprite) {}

    std::shared_ptr<const Movie> movie;
    Scene scene;
    std::optional<std::size_t> sprite;
    Bounds viewport;
    bool primed = false;
};

TimelinePreview::TimelinePreview(std::shared_ptr<const Movie> movie, std::optional<std::size_t> sprite)
    : state_(std::make_unique<State>(std::move(movie), sprite)) {}

TimelinePreview::~TimelinePreview() = default;

FramePreview TimelinePreview::render(unsigned frame)
{
    if (!state_->primed && state_->sprite)
    {
        state_->primed = true;
        const auto count = frame_count(*state_->movie, state_->sprite);
        if (count > 1 && count <= 256)
        {
            const std::set samples{1u, (count + 1) / 2, count};
            for (const auto sample : samples)
                if (sample != frame)
                    try { state_->scene.render(sample, state_->sprite, &state_->viewport); }
                    catch (const std::runtime_error&) {}
        }
    }
    return state_->scene.render(frame, state_->sprite, &state_->viewport);
}

unsigned frame_count(const Movie& movie, std::optional<std::size_t> sprite)
{
    if (!sprite)
        return movie.header().frame_count;
    if (movie.tags().at(*sprite).code != 39)
        throw std::runtime_error("Le tag sélectionné n’est pas un sprite.");
    return Reader(movie.data(*sprite).subspan(2)).u16();
}

double playback_fps(const Movie& movie) noexcept
{
    const auto fps = movie.header().frames_per_second;
    return fps > 0 ? fps : 24.0;
}

FramePreview render_frame(const Movie& movie, unsigned frame, std::optional<std::size_t> sprite)
{
    return Scene(movie).render(frame, sprite);
}

void render_sprite_frames(const Movie& movie, std::size_t sprite, const std::function<void(unsigned, FramePreview)>& accept)
{
    const auto count = (std::max)(1u, frame_count(movie, sprite));
    Scene scene(movie);
    Bounds viewport;
    for (unsigned i = 1; i <= count; ++i)
    {
        const auto bounds = scene.measure(i, sprite);
        if (bounds.valid)
        {
            viewport.add(bounds.x1, bounds.y1);
            viewport.add(bounds.x2, bounds.y2);
        }
    }
    for (unsigned i = 1; i <= count; ++i)
        accept(i, scene.render(i, sprite, &viewport));
}

namespace web
{
namespace
{
void item_json(std::string& text, unsigned depth, const Instance& item)
{
    const auto& p = item.transform;
    text += std::format("[{},{},[{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g}],{:g},{},{},{},{}",
        depth, item.id, p.a, p.b, p.c, p.d, p.x, p.y, item.opacity, json_string(item.name),
        item.clip_end, item.visible ? "true" : "false", item.placed);
    if (item.color != ColorTransform{})
        text += std::format(",[{},{},{},{},{},{},{},{}]",
            item.color.multiplier[0], item.color.multiplier[1], item.color.multiplier[2], item.color.multiplier[3],
            item.color.addition[0], item.color.addition[1], item.color.addition[2], item.color.addition[3]);
    else if (!item.filters.empty() || item.blend > 1 || item.ratio)
        text += ",null";
    if (!item.filters.empty() || item.blend > 1 || item.ratio)
        text += ",[" + item.filters + "]," + json_string(blend_name(item.blend));
    if (item.ratio)
        text += ',' + std::to_string(item.ratio);
    text += ']';
}

template<class Range, class Emit>
void array_json(std::string& text, const Range& values, Emit emit)
{
    text += '[';
    bool comma = false;
    for (const auto& value : values)
    {
        if (std::exchange(comma, true))
            text += ',';
        emit(text, value);
    }
    text += ']';
}

void merge_frame_action(const Movie& movie, std::size_t index, std::string& code, ExportReport& report)
{
    try { code += frame_action(movie.data(index)); }
    catch (const std::exception& error)
    {
        omission(report, std::format("Script du tag {} : {}", index, error.what()));
    }
}

std::string store_action(std::vector<std::string>& actions, std::string code)
{
    if (code.empty())
        return "null";
    actions.push_back("clip => {\n" + code + "}");
    return std::to_string(actions.size() - 1);
}

std::string buttons_json(const Movie& movie, std::size_t index, std::vector<std::string>& actions, ExportReport& report)
{
    const auto modern = movie.tags()[index].code == 34;
    Reader input(movie.data(index));
    input.skip(2);
    unsigned offset = 0;
    if (modern)
    {
        input.skip(1);
        offset = input.u16();
    }
    std::array<std::map<unsigned, Instance>, 4> states;
    while (const auto flags = input.u8())
    {
        Instance item;
        item.id = input.u16();
        item.placed = 1;
        const auto depth = input.u16();
        item.transform = position(read_matrix(input));
        if (modern)
        {
            item.color = read_color_transform(input, true);
            item.opacity = item.color.opacity();
            bool omitted = false;
            if (flags & 16)
                item.filters = read_filters(input, &omitted);
            if (flags & 32)
            {
                item.blend = input.u8();
                omitted |= item.blend > 1 && blend_name(item.blend).empty();
            }
            if (omitted)
                omission(report, std::format("Bouton {} : filtre ou mode de fusion à porter.", index));
        }
        for (unsigned state = 0; state < states.size(); ++state)
            if (flags & (1u << state))
                states[state][depth] = item;
    }
    std::array<std::string, 8> handlers;
    std::map<unsigned, std::string> keys;
    if (!modern)
    {
        try { handlers[0] = frame_action(input.take(input.remaining())); }
        catch (const std::exception& error) { omission(report, std::format("Bouton {} : {}", index, error.what())); }
    }
    else if (offset)
    {
        // The offset is relative to the start of the ActionOffset field.
        if (3 + offset >= movie.data(index).size())
            throw std::runtime_error("Position des événements de bouton hors limites.");
        Reader events(movie.data(index).subspan(3 + offset));
        for (unsigned count = 0;; ++count)
        {
            if (count >= 1024)
                throw std::runtime_error("Trop d’événements dans le bouton.");
            const auto size = events.u16();
            if (size && size < 4)
                throw std::runtime_error("Taille d’événement SWF invalide.");
            Reader event(events.take(size ? size - 2 : events.remaining()));
            const auto flags = event.u8();
            const auto key = event.u8();
            try
            {
                const auto code = frame_action(event.take(event.remaining()));
                constexpr std::array<std::uint8_t, 8> supported{8, 4, 1, 2, 64, 16, 32, 128};
                for (std::size_t i = 0; i < supported.size(); ++i)
                    if (flags & supported[i])
                        handlers[i] += code;
                if (key & 1)
                    handlers[4] += code;
                if (key >> 1)
                    keys[key >> 1] += code;
            }
            catch (const std::exception& error)
            {
                omission(report, std::format("Bouton {} : {}", index, error.what()));
            }
            if (!size)
                break;
        }
    }
    std::string json = "{\"states\":";
    array_json(json, states, [](std::string& output, const auto& state)
    {
        array_json(output, state, [](std::string& result, const auto& layer)
        {
            item_json(result, layer.first, layer.second);
        });
    });
    json += ",\"handlers\":";
    array_json(json, handlers, [&](std::string& output, const std::string& handler)
    {
        output += store_action(actions, handler);
    });
    json += ",\"keys\":{";
    bool comma = false;
    for (const auto& [key, code] : keys)
    {
        if (std::exchange(comma, true)) json += ',';
        json += json_string(std::to_string(key)) + ':' + store_action(actions, code);
    }
    return json + "}}";
}

std::string timeline_json(const Movie& movie, std::optional<std::size_t> sprite, std::vector<std::string>& actions,
    ExportReport& report, const std::set<unsigned>& visual, const ScriptAsset* script, const std::filesystem::path& destination,
    std::map<unsigned, std::set<unsigned>>& morph_ratios)
{
    Display display, previous;
    std::string frames = "[";
    std::map<std::string, unsigned> labels;
    std::string action;
    std::vector<std::array<unsigned, 6>> sounds;
    struct Stream
    {
        unsigned settings{}, first{}, last{};
        std::optional<std::size_t> tag;
        double offset{};
        Bytes samples;
    };
    Stream stream;
    std::string tracks = "[";
    const auto save_stream = [&]
    {
        if (stream.samples.empty())
            return;
        const auto mp3 = stream.settings >> 4 == 2;
        const auto filename = std::format("stream-{}{}", *stream.tag, mp3 ? ".mp3" : ".wav");
        if (mp3)
            write_file(destination / "assets" / filename, stream.samples);
        else
            write_file(destination / "assets" / filename, pcm_audio(stream.settings, stream.samples).bytes);
        ++report.files;
        if (tracks.size() > 1)
            tracks += ',';
        tracks += std::format("[{},{},{},{:g}]", stream.first, stream.last, json_string("assets/" + filename), stream.offset);
    };
    std::set<unsigned> unsupported;
    unsigned frame = 1;
    const auto flush = [&]
    {
        if (frame > 1)
            frames += ',';
        frames += "[[";
        bool comma = false;
        for (const auto& [depth, item] : display.layers)
        {
            const auto old = previous.layers.find(depth);
            if (old != previous.layers.end() && old->second == item)
                continue;
            if (std::exchange(comma, true))
                frames += ',';
            item_json(frames, depth, item);
            if (item.ratio)
                if (const auto index = movie.character(item.id))
                {
                    const auto code = movie.tags()[*index].code;
                    if (code == 46 || code == 84)
                        morph_ratios[item.id].insert(item.ratio);
                }
            if (!visual.contains(item.id))
                unsupported.insert(item.id);
        }
        frames += "],[";
        comma = false;
        for (const auto& [depth, item] : previous.layers)
            if (!display.layers.contains(depth))
            {
                if (std::exchange(comma, true))
                    frames += ',';
                frames += std::to_string(depth);
            }
        frames += "],";
        if (display.background != previous.background && display.background)
        {
            const auto& color = *display.background;
            frames += std::format("\"#{:02x}{:02x}{:02x}\"", color[0], color[1], color[2]);
        }
        else
            frames += "null";
        frames += ',' + store_action(actions, std::exchange(action, {})) + ',';
        array_json(frames, sounds, [](std::string& text, const auto& sound)
        {
            array_json(text, sound, [](std::string& output, unsigned value) { output += std::to_string(value); });
        });
        frames += ']';
        sounds.clear();
        previous = display;
        ++frame;
        if (frames.size() > 64 * 1024 * 1024)
            throw std::runtime_error("Timeline HTML5 supérieure à 64 Mio.");
    };
    for (std::size_t i = sprite ? *sprite + 1 : 0; i < movie.tags().size(); i = movie.tags()[i].next_sibling)
    {
        const auto tag = movie.tags()[i].code;
        if (!tag)
            break;
        if (tag == 1)
        {
            flush();
            continue;
        }
        try
        {
            Reader input(movie.data(i));
            switch (tag)
            {
            case 4: case 26: case 70: case 94:
            {
                const auto flags = tag == 4 ? 0 : input.u8();
                bool omitted = false;
                place_object(display, tag, movie.data(i), frame, &omitted);
                if (flags & 128)
                    omission(report, std::format("Placement {} : actions attachées au clip à porter.", i));
                if (omitted)
                    omission(report, std::format("Placement {} : filtre, mode de fusion ou fond opaque à porter.", i));
                break;
            }
            case 5: case 28:
                if (tag == 5)
                    input.skip(2);
                display.layers.erase(input.u16());
                break;
            case 9: display.background = {input.u8(), input.u8(), input.u8()}; break;
            case 12: merge_frame_action(movie, i, action, report); break;
            case 15:
            {
                const auto id = input.u16();
                const auto flags = input.u8();
                const auto from = flags & 1 ? input.u32() : 0u;
                const auto to = flags & 2 ? input.u32() : 0u;
                const auto loops = flags & 4 ? input.u16() : 1u;
                sounds.push_back({id, from, to, loops, bool(flags & 16), bool(flags & 32)});
                if (flags & 8)
                    omission(report, std::format("Son {} : enveloppe audio à porter.", i));
                break;
            }
            case 43:
                labels.insert_or_assign(input.cstring(), frame);
                break;
            case 86:
            {
                for (const bool scenes : {true, false})
                {
                    const auto count = input.v32();
                    if (count > input.remaining() / 2)
                        throw std::runtime_error("Nombre d’étiquettes de la timeline invalide.");
                    for (unsigned entry = 0; entry < count; ++entry)
                    {
                        const auto position = input.v32();
                        auto name = input.cstring();
                        if (!scenes && position < 65535)
                            labels.insert_or_assign(std::move(name), position + 1);
                    }
                }
                break;
            }
            case 18: case 45:
                save_stream();
                stream = {};
                input.skip(1);
                stream.tag = i;
                stream.settings = input.u8();
                input.skip(2);
                if (const auto codec = stream.settings >> 4; codec != 0 && codec != 2 && codec != 3)
                    omission(report, std::format("Tag {} : codec de streaming {} à porter.", i, codec));
                break;
            case 19:
            {
                if (!stream.tag)
                    break;
                const auto codec = stream.settings >> 4;
                if (codec != 0 && codec != 2 && codec != 3)
                    break;
                if (codec == 2)
                {
                    input.skip(2);
                    const auto skip = std::bit_cast<std::int16_t>(input.u16());
                    constexpr unsigned rates[]{5512, 11025, 22050, 44100};
                    if (!stream.first)
                        stream.offset = static_cast<double>(skip) / rates[(stream.settings >> 2) & 3];
                }
                const auto bytes = input.take(input.remaining());
                if (!bytes.empty())
                {
                    if (bytes.size() > 64 * 1024 * 1024 - stream.samples.size())
                        throw std::runtime_error("Piste audio supérieure à 64 Mio.");
                    if (!stream.first)
                        stream.first = frame;
                    stream.last = frame;
                    append(stream.samples, bytes);
                }
                break;
            }
            case 89:
                omission(report, std::format("Tag {} : lecture audio nommée à porter.", i));
                break;
            default: break;
            }
        }
        catch (const std::exception& error) { omission(report, std::format("Timeline, tag {} : {}", i, error.what())); }
    }
    try { save_stream(); }
    catch (const std::exception& error) { omission(report, std::format("Piste audio {} : {}", stream.tag.value_or(0), error.what())); }
    if (frame == 1)
        flush();
    for (const auto id : unsupported)
        omission(report, std::format("Ressource #{} : aucun visuel converti dans cette timeline.", id));
    std::string label_json = "{";
    for (const auto& [name, number] : labels)
    {
        if (label_json.size() > 1)
            label_json += ',';
        label_json += json_string(name) + ':' + std::to_string(number);
    }
    const auto legacy = script ? store_action(actions, script->legacy_initializer) : "null";
    return "{\"frames\":" + frames + "],\"labels\":" + label_json + "},\"legacyInitialize\":" + legacy +
        ",\"type\":" + (script && !script->type.empty() ? json_string(script->type) : "null") +
        ",\"streams\":" + tracks + "]}";
}
}

std::string export_timeline(const Movie& movie, const std::filesystem::path& destination, ExportReport& report,
    std::vector<std::string>& actions, const std::map<unsigned, ScriptAsset>& scripts,
    const std::function<std::string(const Asset&)>& share_visual)
{
    std::filesystem::create_directories(destination / "assets");
    std::string assets = "{", audios = "{", audio_timing = "{", buttons = "{", clips = "{";
    std::set<unsigned> visual;
    std::vector<std::pair<unsigned, std::size_t>> sprites;
    const auto entry = [](std::string& object, unsigned id, std::string value)
    {
        if (object.size() > 1)
            object += ',';
        object += json_string(std::to_string(id)) + ':' + value;
    };
    std::map<unsigned, std::set<unsigned>> morph_ratios;
    for (std::size_t index = 0; index < movie.tags().size(); ++index)
    {
        const auto& tag = movie.tags()[index];
        if (!tag.character_id || movie.character(*tag.character_id) != index)
            continue;
        const auto id = *tag.character_id;
        try
        {
            if (tag.code == 39)
            {
                visual.insert(id);
                sprites.emplace_back(id, index);
                continue;
            }
            if (tag.code == 7 || tag.code == 34)
            {
                auto button = buttons_json(movie, index, actions, report);
                if (const auto script = scripts.find(id); script != scripts.end() && !script->second.type.empty())
                {
                    button.pop_back();
                    button += ",\"type\":" + json_string(script->second.type) + '}';
                }
                entry(buttons, id, std::move(button));
                visual.insert(id);
                continue;
            }
            if (tag.code == 37)
            {
                entry(assets, id, text_field_json(movie, index));
                visual.insert(id);
                continue;
            }
            auto asset = tag.code == 11 || tag.code == 33 ? std::optional{text_svg(movie, index)} :
                is_editable_shape(tag.code) ? std::optional{shape_svg(read_shape_drawing(tag.code, movie.data(index)), &movie)} :
                extract(movie, index);
            if (!asset && (tag.code == 10 || tag.code == 48 || tag.code == 75))
            {
                if (tag.code != 10)
                {
                    Reader font(movie.data(index));
                    font.skip(4);
                    font.skip(font.u8());
                    if (!font.u16())
                        continue;
                }
                asset = font_svg(movie, index);
            }
            if (!asset)
            {
                if (tag.category == Category::sounds || tag.category == Category::shapes || tag.code == 60)
                    omission(report, std::format("Ressource #{} ({}) à porter.", id, tag_name(tag.code)));
                continue;
            }
            const auto file = std::format(L"asset-{}{}", id, asset->extension);
            const auto displayed = tag.category == Category::shapes || tag.category == Category::images ||
                tag.code == 11 || tag.code == 33;
            std::string path;
            if (displayed && share_visual)
                path = share_visual(*asset);
            else
            {
                write_file(destination / "assets" / file, asset->bytes);
                ++report.files;
                path = "assets/" + std::filesystem::path(file).generic_string();
            }
            if (tag.code == 14)
            {
                entry(audios, id, json_string(path));
                Reader input(movie.data(index));
                input.skip(2);
                const auto flags = input.u8();
                const auto samples = input.u32();
                constexpr unsigned rates[]{5512, 11025, 22050, 44100};
                const auto priming = flags >> 4 == 2 ? static_cast<std::int16_t>(input.u16()) : 0;
                entry(audio_timing, id, std::format("[{},{},{}]", rates[(flags >> 2) & 3], samples, priming));
            }
            else if (displayed)
            {
                std::string view;
                if (tag.category == Category::images)
                {
                    const auto [width, height] = bitmap_dimensions(*asset);
                    view = std::format(",\"bounds\":[0,0,{},{}]", width * 20u, height * 20u);
                }
                else
                {
                    Reader input(movie.data(index));
                    input.skip(2);
                    BitReader box(input);
                    const auto width = box.unsigned_bits(5);
                    const auto x1 = box.signed_bits(width), x2 = box.signed_bits(width);
                    const auto y1 = box.signed_bits(width), y2 = box.signed_bits(width);
                    view = std::format(",\"bounds\":[{},{},{},{}]", x1, y1,
                        std::max<std::int64_t>(20, std::int64_t{x2} - x1), std::max<std::int64_t>(20, std::int64_t{y2} - y1));
                }
                std::string type;
                if (tag.category == Category::images)
                    if (const auto found = scripts.find(id); found != scripts.end() && !found->second.type.empty())
                        type = ",\"type\":" + json_string(found->second.type);
                entry(assets, id, "{\"url\":" + json_string(path) + view + type + '}');
                visual.insert(id);
            }
        }
        catch (const std::exception& error) { omission(report, std::format("Ressource #{} : {}", id, error.what())); }
    }
    const auto add_timeline = [&](unsigned id, std::optional<std::size_t> sprite)
    {
        const auto script = scripts.find(id);
        entry(clips, id, timeline_json(movie, sprite, actions, report, visual,
            script == scripts.end() ? nullptr : &script->second, destination, morph_ratios));
    };
    add_timeline(0, {});
    for (const auto& [id, index] : sprites)
        add_timeline(id, index);
    for (const auto& [id, ratios] : morph_ratios)
    {
        const auto index = *movie.character(id);
        for (const auto ratio : ratios)
            try
            {
                const auto drawing = read_morph_drawing(movie.tags()[index].code, movie.data(index), ratio);
                const auto asset = shape_svg(drawing, &movie);
                const auto key = std::format("{}@{}", id, ratio);
                auto path = "assets/asset-" + key + ".svg";
                if (share_visual)
                    path = share_visual(asset);
                else
                {
                    write_file(destination / path, asset.bytes);
                    ++report.files;
                }
                const auto& [x1, x2, y1, y2] = drawing.bounds;
                assets += ',' + json_string(key) + std::format(":{{\"url\":{},\"bounds\":[{},{},{},{}]}}",
                    json_string(path), x1, y1, std::max<std::int64_t>(20, std::int64_t{x2} - x1),
                    std::max<std::int64_t>(20, std::int64_t{y2} - y1));
            }
            catch (const std::exception& error)
            {
                omission(report, std::format("Forme interpolée #{} (ratio {}) : {}.", id, ratio, error.what()));
            }
    }
    std::map<std::string, unsigned> exports;
    if (!actions.empty())
        for (std::size_t i = 0; i < movie.tags().size(); ++i)
            if (movie.tags()[i].code == 56)
            {
                try
                {
                    Reader input(movie.data(i));
                    const auto count = input.u16();
                    for (unsigned symbol = 0; symbol < count; ++symbol)
                    {
                        const auto id = input.u16();
                        exports.try_emplace(input.cstring(), id);
                    }
                }
                catch (const std::runtime_error& error)
                {
                    omission(report, std::format("ExportAssets {} : {}", i, error.what()));
                }
            }
    std::string legacy_symbols = "{";
    for (const auto& [name, id] : exports)
    {
        if (legacy_symbols.size() > 1) legacy_symbols += ',';
        legacy_symbols += json_string(name) + ':' + std::to_string(id);
    }
    const auto& h = movie.header();
    return std::format("{{\"fps\":{:g},\"viewBox\":[{},{},{},{}],\"assets\":{},\"audio\":{},\"audioTiming\":{},\"buttons\":{},\"clips\":{},\"legacySymbols\":{},\"byteSize\":{}}}",
        playback_fps(movie), h.left, h.top, std::max<std::int64_t>(20, std::int64_t{h.right} - h.left),
        std::max<std::int64_t>(20, std::int64_t{h.bottom} - h.top), assets + '}', audios + '}', audio_timing + '}', buttons + '}',
        clips + '}', legacy_symbols + '}', h.expanded_size);
}
}
}

#include "Workspace.hpp"
#include "UiStyle.hpp"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>

namespace studio
{
using namespace ui;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Storage::Streams;

namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_cached_frames = 48;
constexpr std::size_t max_cached_bytes = 16 * 1024 * 1024;
}

struct Workspace::FramePlayback
{
    struct Frame
    {
        SvgImageSource image;
        unsigned omitted;
    };

    FramePlayback(std::shared_ptr<const flash::Movie> document, std::optional<std::size_t> tag)
        : movie(std::move(document)), sprite(tag), renderer(movie, sprite), count(std::max(1u, flash::frame_count(*movie, sprite))),
          fps(flash::playback_fps(*movie)) {}

    std::shared_ptr<const flash::Movie> movie;
    std::optional<std::size_t> sprite;
    flash::TimelinePreview renderer;
    std::map<unsigned, Frame> cache;
    const unsigned count;
    const double fps;
    std::size_t cached_bytes = 0;
    Clock::time_point started;
    std::uint64_t revision = 0;
    unsigned origin = 1, requested = 1, displayed = 0;
    std::optional<unsigned> failed;
    bool playing = false;
};

void Workspace::build_frame_player()
{
    frame_bar_ = Grid();
    frame_bar_.Background(brush(base));
    frame_bar_.Padding(pad(14, 2, 14, 10));
    frame_bar_.RowSpacing(4);
    row(frame_bar_);
    row(frame_bar_);

    frame_seek_ = Slider();
    frame_seek_.Minimum(1);
    frame_seek_.StepFrequency(1);
    frame_seek_.IsThumbToolTipEnabled(false);
    Automation::AutomationProperties::SetName(frame_seek_, tr(L"Position dans la timeline"));
    frame_seek_.ValueChanged([this](auto&&, const RangeBaseValueChangedEventArgs& args)
    {
        if (!refreshing_ && playback_ && std::isfinite(args.NewValue()))
            seek_frame(static_cast<unsigned>(std::lround(args.NewValue())));
    });
    place(frame_bar_, frame_seek_, 0);

    Grid transport;
    transport.ColumnSpacing(9);
    column(transport, 1, GridUnitType::Auto);
    column(transport, 1, GridUnitType::Star);
    column(transport, 1, GridUnitType::Auto);
    auto buttons = horizontal(4);
    frame_previous_ = Button();
    frame_previous_.Content(glyph(L'\uE892', 13));
    Automation::AutomationProperties::SetName(frame_previous_, tr(L"Image précédente"));
    ToolTipService::SetToolTip(frame_previous_, box_value(tr(L"Image précédente")));
    frame_previous_.Click([this](auto&&, auto&&) { if (playback_ && playback_->requested > 1) seek_frame(playback_->requested - 1); });
    frame_play_ = Button();
    frame_play_.Content(box_value(tr(L"Lire")));
    frame_play_.MinWidth(68);
    frame_play_.Click([this](auto&&, auto&&) { if (playback_) set_playing(!playback_->playing); });
    frame_next_ = Button();
    frame_next_.Content(glyph(L'\uE893', 13));
    Automation::AutomationProperties::SetName(frame_next_, tr(L"Image suivante"));
    ToolTipService::SetToolTip(frame_next_, box_value(tr(L"Image suivante")));
    frame_next_.Click([this](auto&&, auto&&) { if (playback_) seek_frame(playback_->requested + 1); });
    buttons.Children().Append(frame_previous_);
    buttons.Children().Append(frame_play_);
    buttons.Children().Append(frame_next_);
    place(transport, buttons, 0);
    frame_counter_ = label(L"", 12, secondary);
    frame_counter_.VerticalAlignment(VerticalAlignment::Center);
    place(transport, frame_counter_, 0, 1);
    frame_loop_ = CheckBox();
    frame_loop_.Content(box_value(tr(L"Boucle")));
    frame_loop_.MinWidth(60);
    frame_loop_.IsChecked(true);
    place(transport, frame_loop_, 0, 2);
    place(frame_bar_, transport, 1);
    frame_bar_.Visibility(Visibility::Collapsed);
    row(inspector_);
    place(inspector_, frame_bar_, 4);

    frame_timer_ = DispatcherTimer();
    frame_timer_.Tick([this](auto&&, auto&&) { tick_frames(); });
}

void Workspace::open_frame_player()
{
    if (playback_ && playback_->movie == movie_ && playback_->sprite == selected_)
        return;
    close_frame_player();
    const auto version = ++preview_version_;
    try
    {
        playback_ = std::make_shared<FramePlayback>(movie_, selected_);
        if (audio_.Visibility() == Visibility::Visible)
        {
            audio_.Source(nullptr);
            audio_.Visibility(Visibility::Collapsed);
        }
        output_.Visibility(Visibility::Collapsed);
        code_view_.Visibility(Visibility::Collapsed);
        preview_error_.Visibility(Visibility::Collapsed);
        image_.Source(nullptr);
        picture_view_.Visibility(Visibility::Collapsed);
        progress_.Visibility(Visibility::Visible);
        progress_.IsActive(true);
        {
            const UpdatingSelection editing(refreshing_);
            frame_seek_.Maximum(playback_->count);
            frame_seek_.Value(1);
        }
        frame_seek_.IsEnabled(playback_->count > 1);
        frame_play_.IsEnabled(playback_->count > 1);
        frame_play_.Content(box_value(tr(L"Lire")));
        frame_loop_.IsEnabled(playback_->count > 1);
        frame_previous_.IsEnabled(false);
        frame_next_.IsEnabled(playback_->count > 1);
        frame_counter_.Text(std::format(L"1 / {}", playback_->count));
        const auto milliseconds = std::clamp(500.0 / playback_->fps, 4.0, 25.0);
        frame_timer_.Interval(std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::duration<double, std::milli>(milliseconds)));
        request_frame();
    }
    catch (const hresult_error& error) { preview_problem(version, error.message()); }
    catch (const std::exception& error) { preview_problem(version, widen(error.what())); }
}

void Workspace::close_frame_player()
{
    if (playback_)
    {
        frame_timer_.Stop();
        playback_.reset();
    }
}

void Workspace::set_playing(bool playing)
{
    auto& playback = *playback_;
    if (playing && playback.count <= 1)
        return;
    playback.playing = playing;
    frame_play_.Content(box_value(playing ? L"Pause" : tr(L"Lire")));
    if (playing)
    {
        playback.failed.reset();
        playback.origin = playback.requested < playback.count ? playback.requested : 1;
        playback.requested = playback.origin;
        playback.started = Clock::now();
        frame_timer_.Start();
        request_frame();
    }
    else
    {
        frame_timer_.Stop();
        if (playback.displayed)
            playback.requested = playback.displayed;
        ++playback.revision;
    }
}

void Workspace::tick_frames()
{
    if (!playback_ || !playback_->playing)
        return;
    auto& playback = *playback_;
    const auto elapsed = std::chrono::duration<double>(Clock::now() - playback.started).count();
    const auto index = playback.origin - 1 + static_cast<std::uint64_t>(elapsed * playback.fps);
    if (index >= playback.count && !frame_loop_.IsChecked().Value())
    {
        playback.playing = false;
        playback.requested = playback.count;
        frame_timer_.Stop();
        frame_play_.Content(box_value(tr(L"Lire")));
    }
    else
        playback.requested = 1 + static_cast<unsigned>(index % playback.count);
    request_frame();
}

void Workspace::seek_frame(unsigned frame)
{
    if (!playback_)
        return;
    auto& playback = *playback_;
    if (playback.playing)
        set_playing(false);
    else
        ++playback.revision;
    playback.failed.reset();
    playback.requested = std::clamp(frame, 1u, playback.count);
    {
        const UpdatingSelection editing(refreshing_);
        frame_seek_.Value(playback.requested);
    }
    frame_previous_.IsEnabled(playback.requested > 1);
    frame_next_.IsEnabled(playback.requested < playback.count);
    request_frame();
}

unsigned Workspace::displayed_frame() const
{
    return playback_ ? std::max(playback_->displayed, 1u) : 1u;
}

void Workspace::request_frame()
{
    if (closed_ || !playback_)
        return;
    auto& playback = *playback_;
    const auto frame = playback.requested;
    if (frame == playback.displayed || playback.failed == frame)
        return;
    if (playback.cache.contains(frame))
        show_frame(playback, frame);
    else if (!frame_work_running_)
    {
        frame_work_running_ = true;
        load_frame(playback_, frame, playback.revision);
    }
}

void Workspace::show_frame(FramePlayback& playback, unsigned frame)
{
    const auto& data = playback.cache.at(frame);
    const auto first = !playback.displayed;
    image_.Source(data.image);
    picture_view_.Visibility(Visibility::Visible);
    if (first)
        picture_view_.ChangeView(0.0, 0.0, 1.0f, true);
    playback.displayed = frame;
    preview_error_.Visibility(Visibility::Collapsed);
    progress_.IsActive(false);
    progress_.Visibility(Visibility::Collapsed);
    frame_counter_.Text(std::format(L"{} / {}", frame, playback.count));
    detail_meta_.Text(format_message(L"{:g} i/s{}", playback.fps,
        data.omitted ? format_message(L"  ·  {} éléments ou effets omis", data.omitted) : L""));
    if (playback.playing || playback.requested == frame)
    {
        const UpdatingSelection editing(refreshing_);
        frame_seek_.Value(frame);
        frame_previous_.IsEnabled(frame > 1);
        frame_next_.IsEnabled(frame < playback.count);
    }
}

fire_and_forget Workspace::load_frame(std::shared_ptr<FramePlayback> playback, unsigned frame, std::uint64_t revision)
{
    const auto lifetime = shared_from_this();
    const apartment_context ui;
    std::wstring failure;
    bool viewport_changed = false;
    const auto still_wanted = [&]
    {
        return !closed_ && playback_ == playback && playback->revision == revision;
    };
    try
    {
        co_await resume_background();
        std::optional<flash::FramePreview> result;
        try
        {
            result = playback->renderer.render(frame);
            viewport_changed = result->viewport_changed;
        }
        catch (const std::exception& error) { failure = widen(error.what()); }
        co_await ui;
        if (result && still_wanted())
        {
            InMemoryRandomAccessStream stream;
            DataWriter writer(stream);
            writer.WriteBytes(result->image.bytes);
            co_await writer.StoreAsync();
            writer.DetachStream();
            stream.Seek(0);
            SvgImageSource picture;
            if (co_await picture.SetSourceAsync(stream) != SvgImageSourceLoadStatus::Success)
                throw hresult_error(E_FAIL, tr(L"Impossible d’afficher cette image du sprite."));
            if (still_wanted())
            {
                const auto bytes = result->image.bytes.size();
                if (viewport_changed || playback->cache.size() >= max_cached_frames ||
                    bytes > max_cached_bytes - std::min(playback->cached_bytes, max_cached_bytes))
                {
                    playback->cache.clear();
                    playback->cached_bytes = 0;
                }
                viewport_changed = false;
                const auto inserted = playback->cache.try_emplace(frame, FramePlayback::Frame{picture, result->omitted}).second;
                if (inserted)
                    playback->cached_bytes += bytes;
                if (playback->requested == frame || (playback->playing && playback->requested != playback->displayed))
                    show_frame(*playback, frame);
            }
        }
    }
    catch (const hresult_error& error) { failure = error.message(); }
    catch (const std::exception& error) { failure = widen(error.what()); }
    if (GetCurrentThreadId() != ui_thread_ || closed_)
        co_return;
    frame_work_running_ = false;
    if (playback_ == playback && viewport_changed)
    {
        playback->cache.clear();
        playback->cached_bytes = 0;
    }
    if (!failure.empty() && still_wanted())
    {
        set_playing(false);
        playback->requested = frame;
        playback->failed = frame;
        preview_problem(preview_version_, failure);
    }
    request_frame();
}
}

#pragma once

#include "../FlashCore/Swf.hpp"
#include "../FlashCore/Abc.hpp"
#include "../FlashCore/Search.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>

namespace studio
{
enum class View { details, source, instructions, hex, frame };
enum class Mode { resources, scripts, tags };
enum class Export { asset, raw, source, instructions, pcode, graph };
enum class AbcEdit { method, simplify, pool_string, import };
enum class CodeReferenceKind { call, constructor, type };
using CodeReference = std::function<std::function<void()>(std::wstring_view, std::wstring_view, CodeReferenceKind)>;

class Workspace : public std::enable_shared_from_this<Workspace>
{
public:
    void start(const std::filesystem::path& initial_file, std::optional<std::size_t> initial_tag = {});

private:
    struct Row
    {
        std::wstring title;
        std::wstring search_key;
    };
    struct AbcGroup
    {
        std::string name;
        std::vector<unsigned> methods;
    };
    struct ScriptEntry
    {
        std::size_t tag;
        std::optional<std::size_t> type;
        std::shared_ptr<const flash::AbcModule> module;
        std::wstring package, name, qualified, search_key;
    };
    struct ScriptSelection
    {
        std::size_t tag;
        std::wstring qualified;
        std::optional<unsigned> method;
    };
    struct ScriptTab
    {
        ScriptSelection script;
        View view = View::source;
        std::size_t line = 1;
        bool pinned = false;
        winrt::Microsoft::UI::Xaml::Controls::TabViewItem item{nullptr};
    };
    struct Package
    {
        std::wstring caption;
        std::map<std::wstring, std::size_t> children;
        std::vector<std::size_t> entries;
        bool contains_selection = false;
    };
    struct ScriptCatalog
    {
        std::vector<ScriptEntry> entries;
        std::size_t errors{};
    };
    struct OpenDocument
    {
        std::filesystem::path path;
        std::shared_ptr<const flash::Movie> movie;
        std::vector<Row> rows;
        winrt::Microsoft::UI::Xaml::Controls::TabViewItem tab{nullptr};
        Mode mode = Mode::resources;
        View view = View::details;
        std::optional<std::size_t> tag;
        std::optional<ScriptSelection> script;
        std::wstring filter;
        std::vector<ScriptTab> script_tabs;
        std::optional<ScriptCatalog> catalog;
    };
    struct FramePlayback;
    struct ScriptTarget
    {
        std::optional<std::size_t> tag, type;
        std::wstring qualified;
        std::optional<unsigned> method;
        std::optional<std::size_t> line;
        View view = View::source;
    };
    static std::vector<Row> index_rows(const flash::Movie& movie);
    void build();
    void build_modes(const winrt::Microsoft::UI::Xaml::Controls::Grid& header);
    void build_script_browser(const winrt::Microsoft::UI::Xaml::Controls::Grid& sidebar);
    void build_outline(const winrt::Microsoft::UI::Xaml::Controls::Grid& preview);
    void build_gallery();
    void switch_mode(Mode mode);
    void populate_categories();
    std::optional<flash::Category> category_filter() const;
    void update_layout();
    void reset_explorer();
    void refresh_scripts();
    void fill_package(const winrt::Microsoft::UI::Xaml::Controls::TreeViewNode& node, std::size_t index);
    winrt::Microsoft::UI::Xaml::Controls::TreeViewNode package_node(std::size_t index);
    void select_script(std::size_t index, std::optional<unsigned> method = {}, View view = View::source);
    void jump_to_member();
    void jump_to_code_line(std::size_t line);
    CodeReference code_references(std::wstring_view source);
    void open_script_target(ScriptTarget target);
    void resolve_script_target();
    void open_search_hit(const flash::SearchHit& hit);
    winrt::fire_and_forget show_global_search(std::optional<flash::SearchArea> area = {});
    winrt::fire_and_forget inspect_constant(std::size_t tag, unsigned index);
    void refresh_gallery(bool keep_page = false);
    winrt::fire_and_forget index_scripts();
    winrt::fire_and_forget load_thumbnails(std::vector<std::pair<std::size_t, winrt::Microsoft::UI::Xaml::Controls::Image>> tiles);
    void open_dialog();
    void open_folder_dialog();
    void build_folder_panel(const winrt::Microsoft::UI::Xaml::Controls::Grid& main);
    void toggle_folder_panel();
    void filter_folder();
    void sync_folder_selection(bool scroll = true);
    void follow_swf_reference(std::wstring_view reference);
    winrt::Windows::Foundation::IAsyncAction open_folder(std::filesystem::path path);
    void register_shortcuts();
    bool run_shortcut(std::wstring_view action);
    winrt::fire_and_forget show_about();
    void build_documents();
    void build_script_tabs();
    void refresh_script_tabs();
    void remember_script_tab();
    void track_script_tab(const ScriptEntry& entry);
    void activate_script_tab(const ScriptTab& tab);
    void toggle_script_pin(std::size_t tag, std::wstring_view qualified);
    void update_script_pin();
    ScriptTab* current_script_tab();
    void show_recent();
    void remember_recent(const std::filesystem::path& path);
    void add_document(std::filesystem::path path, std::shared_ptr<const flash::Movie> movie, std::vector<Row> rows,
        std::optional<std::size_t> initial_tag);
    void activate_document(std::size_t index);
    void close_document(std::size_t index);
    void update_document();
    void refresh_tags();
    void show_page(std::size_t page);
    void select_tag(std::optional<std::size_t> index, std::optional<View> desired_view = {});
    void set_view(View view);
    void build_frame_player();
    void open_frame_player();
    void close_frame_player();
    void set_playing(bool playing);
    void tick_frames();
    void seek_frame(unsigned frame);
    void request_frame();
    void show_frame(FramePlayback& playback, unsigned frame);
    unsigned displayed_frame() const;
    winrt::fire_and_forget load_frame(std::shared_ptr<FramePlayback> playback, unsigned frame, std::uint64_t revision);
    void configure_abc(std::shared_ptr<const flash::AbcModule> module, unsigned method);
    void show_abc_methods(std::size_t group, std::optional<unsigned> selected);
    void show_code(std::string_view code, bool instructions);
    void hide_code();
    void open_code_preview();
    void code_preview_message(std::wstring_view action, const winrt::Windows::Data::Json::JsonObject& message);
    winrt::fire_and_forget initialize_code_view();
    void update_abc_visibility();
    void preview_problem(std::uint64_t version, std::wstring_view message);
    void status(std::wstring_view message, bool error = false);
    winrt::fire_and_forget save_movie();
    winrt::Windows::Foundation::IAsyncAction open_file(std::filesystem::path path, std::optional<std::size_t> initial_tag = {});
    winrt::Windows::Foundation::IAsyncAction prompt_deobfuscation(std::size_t methods);
    winrt::Windows::Foundation::IAsyncAction deobfuscate_swf(bool whole_document);
    winrt::fire_and_forget show_selection();
    winrt::fire_and_forget export_tag(Export format);
    winrt::fire_and_forget export_all(bool project = false, std::wstring visual_format = {});
    winrt::fire_and_forget choose_visual_batch();
    winrt::fire_and_forget export_image();
    winrt::fire_and_forget export_animation(flash::AnimationFormat format);
    winrt::Windows::Foundation::IAsyncOperation<double> choose_export_scale();
    winrt::fire_and_forget edit_abc(AbcEdit operation);
    struct SourceEdit;
    struct ShapeEdit;
    static std::filesystem::path editor_assets();
    winrt::fire_and_forget edit_shape();
    winrt::fire_and_forget save_shape(std::shared_ptr<ShapeEdit> session, winrt::hstring message);
    void close_shape_editor();
    void build_source_editor(const winrt::Microsoft::UI::Xaml::Controls::Grid& preview);
    winrt::fire_and_forget create_class();
    winrt::fire_and_forget edit_class();
    winrt::fire_and_forget begin_class_source(ScriptEntry script);
    winrt::fire_and_forget save_class(std::string source);
    void set_source_editing(bool editing);
    void open_editor_source();
    void end_source_edit();
    void set_editor_state(std::wstring_view message, bool error = false);
    void post_editor_action(std::wstring_view action);
    bool find_class_compiler(bool prompt);
    std::optional<std::filesystem::path> swf_copy_path(const std::filesystem::path& original, std::wstring_view suffix,
        const winrt::Microsoft::UI::Xaml::Window& owner = nullptr);
    winrt::fire_and_forget edit_tag();
    winrt::fire_and_forget drop(winrt::Microsoft::UI::Xaml::DragEventArgs args);

    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid root_{nullptr}, inspector_{nullptr}, abc_tools_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border empty_{nullptr}, workarea_{nullptr}, listing_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock file_name_{nullptr}, file_info_{nullptr},
        status_{nullptr}, status_icon_{nullptr}, list_count_{nullptr}, detail_title_{nullptr},
        detail_meta_{nullptr}, empty_list_{nullptr}, preview_error_{nullptr}, page_label_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox search_{nullptr}, output_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer picture_view_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid source_editor_{nullptr}, source_actions_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::WebView2 source_web_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button source_create_{nullptr}, source_edit_{nullptr}, source_save_{nullptr},
        source_cancel_{nullptr}, script_pin_{nullptr}, shape_edit_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock source_feedback_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox abc_owner_{nullptr}, abc_method_list_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem animation_export_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid frame_bar_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Slider frame_seek_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock frame_counter_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button frame_play_{nullptr}, frame_previous_{nullptr}, frame_next_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::CheckBox frame_loop_{nullptr};
    winrt::Microsoft::UI::Xaml::DispatcherTimer frame_timer_{nullptr};
    std::shared_ptr<FramePlayback> playback_;
    bool frame_work_running_ = false;
    winrt::Microsoft::UI::Xaml::Controls::ListView categories_{nullptr}, tags_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button open_{nullptr}, save_{nullptr}, replace_{nullptr},
        previous_page_{nullptr}, next_page_{nullptr}, global_search_{nullptr}, document_class_{nullptr}, deobfuscate_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::DropDownButton export_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::DropDownButton recent_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::SplitView folder_pane_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button folder_button_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox folder_search_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ListView folder_list_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock folder_title_{nullptr}, folder_count_{nullptr};
    struct FolderIndex;
    std::shared_ptr<const FolderIndex> folder_;
    std::vector<std::size_t> folder_visible_;
    winrt::Microsoft::UI::Xaml::Controls::TabView documents_bar_{nullptr}, script_tabs_bar_{nullptr};
    std::vector<OpenDocument> documents_;
    std::optional<std::size_t> active_document_;
    winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem export_source_{nullptr}, export_assembly_{nullptr},
        export_asset_{nullptr}, export_raw_{nullptr}, export_pcode_{nullptr}, export_graph_{nullptr}, convert_image_{nullptr},
        edit_pcode_{nullptr}, edit_simplified_{nullptr}, edit_class_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Image image_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::MediaPlayerElement audio_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ProgressRing progress_{nullptr};
    std::array<winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton, 5> tabs_{};
    std::array<winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton, 3> mode_tabs_{};
    winrt::Microsoft::UI::Xaml::Controls::Grid gallery_{nullptr}, script_navigation_{nullptr}, outline_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock sidebar_title_{nullptr}, script_status_{nullptr},
        gallery_title_{nullptr}, gallery_count_{nullptr}, gallery_page_label_{nullptr}, outline_count_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox script_search_{nullptr}, gallery_search_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TreeView scripts_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ListView members_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::GridView tiles_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button gallery_previous_{nullptr}, gallery_next_{nullptr}, gallery_back_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ColumnDefinition outline_column_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton toggle_outline_{nullptr};
    std::shared_ptr<const flash::Movie> movie_;
    std::shared_ptr<const flash::AbcModule> abc_;
    std::vector<AbcGroup> abc_groups_;
    std::optional<unsigned> abc_method_;
    std::vector<ScriptEntry> script_entries_;
    std::optional<ScriptSelection> edited_script_;
    std::optional<ScriptTarget> pending_script_target_;
    std::optional<std::size_t> next_code_line_;
    std::wstring global_search_term_;
    flash::SearchOptions global_search_options_;
    bool global_search_open_ = false;
    std::vector<flash::Category> category_filters_;
    std::vector<Package> packages_;
    bool expand_filtered_packages_ = false;
    std::vector<std::size_t> visible_assets_, member_lines_;
    std::optional<std::size_t> selected_script_;
    std::optional<std::size_t> previous_script_, previous_asset_;
    std::optional<unsigned> previous_method_;
    std::optional<flash::Category> previous_category_;
    View previous_script_view_ = View::source;
    std::shared_ptr<std::atomic_bool> thumbnails_cancel_;
    std::size_t gallery_page_ = 0, script_errors_ = 0;
    bool scripts_indexed_ = false, scripts_loading_ = false;
    Mode mode_ = Mode::resources;
    std::filesystem::path file_;
    std::vector<Row> indexed_rows_;
    std::vector<std::size_t> matches_;
    std::size_t page_ = 0, page_start_ = 0, page_end_ = 0;
    std::optional<std::size_t> selected_;
    std::uint64_t preview_version_ = 0;
    std::uint32_t ui_thread_ = 0;
    View view_ = View::details;
    bool busy_ = false, refreshing_ = false, closed_ = false;
    double export_scale_ = 1;
    std::filesystem::path class_compiler_;
    std::shared_ptr<SourceEdit> source_session_;
    std::shared_ptr<ShapeEdit> shape_session_;
    std::uint64_t source_revision_ = 0;
    bool source_preparing_ = false, source_ready_ = false, code_initializing_ = false, code_unavailable_ = false;
    bool code_visible_ = false, code_instructions_ = false;
    std::wstring code_text_, code_id_;
    std::size_t code_line_ = 1;
    CodeReference preview_reference_;
};
}

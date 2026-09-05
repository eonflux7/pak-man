#include "pakman/archive.hpp"

#include <Windows.h>
#include <ShObjIdl.h>
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static ID3D11Device* device = nullptr;
static ID3D11DeviceContext* context = nullptr;
static IDXGISwapChain* swap_chain = nullptr;
static ID3D11RenderTargetView* render_target = nullptr;

static std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr); return s;
}

static std::string utf8(const fs::path& path) {
    auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

static fs::path dialog(HWND owner, bool folder, bool save = false) {
    IFileDialog* d = nullptr;
    HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&d));
    if (FAILED(hr)) return {};
    DWORD opts{}; d->GetOptions(&opts);
    if (folder) d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    else d->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    COMDLG_FILTERSPEC filters[] = {{L"Commandos PAK archive", L"*.pak"}, {L"All files", L"*.*"}};
    if (!folder) { d->SetFileTypes(2, filters); d->SetDefaultExtension(L"pak"); }
    fs::path result;
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(d->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) { result = p; CoTaskMemFree(p); }
            item->Release();
        }
    }
    d->Release(); return result;
}

static void error_box(HWND hwnd, const std::exception& e) {
    auto message = std::string(e.what());
    int n = MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), text.data(), n);
    MessageBoxW(hwnd, text.c_str(), L"pak-man error", MB_OK | MB_ICONERROR);
}

struct BrowserRow {
    bool directory{};
    std::string name;
    std::string path;
    std::string type;
    std::size_t entry{};
    std::vector<std::size_t> members;
    std::uint64_t original{};
    std::uint64_t stored{};
};

static std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static std::string file_type(std::string_view name) {
    auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 == name.size()) return "File";
    std::string ext(name.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return ext + " file";
}

static ImU32 type_color(std::string_view type) {
    if (type == "DDS file" || type == "PNG file" || type == "RAW file") return IM_COL32(87, 166, 255, 255);
    if (type == "TXT file" || type == "FBS file" || type == "BDD file") return IM_COL32(103, 205, 135, 255);
    if (type == "RWS file" || type == "RPC file" || type == "ANM file") return IM_COL32(190, 130, 255, 255);
    return IM_COL32(180, 184, 192, 255);
}

static void draw_file_icon(ImVec2 p, bool directory, std::string_view type) {
    auto* draw = ImGui::GetWindowDrawList();
    if (directory) {
        const ImU32 edge = IM_COL32(205, 151, 45, 255), fill = IM_COL32(242, 184, 62, 255);
        draw->AddRectFilled({p.x + 1, p.y + 5}, {p.x + 18, p.y + 16}, fill, 2.0f);
        draw->AddRectFilled({p.x + 2, p.y + 2}, {p.x + 9, p.y + 7}, fill, 2.0f);
        draw->AddRect({p.x + 1, p.y + 5}, {p.x + 18, p.y + 16}, edge, 2.0f);
    } else {
        ImU32 color = type_color(type);
        draw->AddRectFilled({p.x + 3, p.y + 1}, {p.x + 16, p.y + 17}, IM_COL32(225, 228, 235, 255), 1.5f);
        draw->AddTriangleFilled({p.x + 11, p.y + 1}, {p.x + 16, p.y + 6}, {p.x + 11, p.y + 6}, IM_COL32(150, 155, 165, 255));
        draw->AddRectFilled({p.x + 3, p.y + 13}, {p.x + 16, p.y + 17}, color, 1.5f);
    }
}

static void create_render_target() {
    ID3D11Texture2D* back = nullptr; swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    device->CreateRenderTargetView(back, nullptr, &render_target); back->Release();
}
static void cleanup_render_target() { if (render_target) { render_target->Release(); render_target = nullptr; } }
static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC desc{}; desc.BufferCount = 2; desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.OutputWindow = hwnd; desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE; desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level{};
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &desc, &swap_chain, &device, &level, &context) != S_OK) return false;
    create_render_target(); return true;
}
static void cleanup_device() {
    cleanup_render_target(); if (swap_chain) swap_chain->Release(); if (context) context->Release(); if (device) device->Release();
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (device && wp != SIZE_MINIMIZED) { cleanup_render_target(); swap_chain->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0); create_render_target(); }
        return 0;
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, instance, nullptr, nullptr, nullptr, nullptr, L"pak-man", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"pak-man", WS_OVERLAPPEDWINDOW,
                              100, 100, 1200, 760, nullptr, nullptr, wc.hInstance, nullptr);
    if (!create_device(hwnd)) { cleanup_device(); UnregisterClassW(wc.lpszClassName, instance); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.FrameRounding = 6.0f;

    wchar_t windows_directory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windows_directory, MAX_PATH)) {
        auto font_path = utf8(fs::path(windows_directory) / L"Fonts" / L"consola.ttf");
        io.Fonts->AddFontFromFileTTF(font_path.c_str(), 14.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
    }
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(device, context);

    std::unique_ptr<pakman::Archive> archive;
    std::vector<unsigned char> selected;
    char filter[256]{};
    std::string status = "Ready";
    bool compressed_create = false;
    int create_platform = 0;
    std::string current_directory;
    std::vector<std::string> back_history;
    std::vector<std::string> forward_history;
    int selection_anchor = -1;
    int sort_column = 0;
    ImGuiSortDirection sort_direction = ImGuiSortDirection_Ascending;
    auto clear_selection = [&] { std::fill(selected.begin(), selected.end(), 0); selection_anchor = -1; };
    auto navigate = [&](std::string destination, bool record = true) {
        if (destination == current_directory) return;
        if (record) { back_history.push_back(current_directory); forward_history.clear(); }
        current_directory = std::move(destination);
        clear_selection();
        filter[0] = '\0';
    };
    auto open_archive = [&](const fs::path& p) {
        try {
            archive = std::make_unique<pakman::Archive>(pakman::Archive::open(p));
            selected.assign(archive->entries().size(), false);
            current_directory.clear(); back_history.clear(); forward_history.clear(); selection_anchor = -1;
            status = "Opened " + utf8(p);
        }
        catch (std::exception const& e) { error_box(hwnd, e); }
    };
    if (command_line && *command_line) {
        std::wstring arg = command_line; if (arg.size() > 1 && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
        if (fs::exists(arg)) open_archive(arg);
    }

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        auto viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos); ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("pak-man", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::Button("Open PAK...")) { auto p = dialog(hwnd, false); if (!p.empty()) open_archive(p); }
        ImGui::SameLine();
        if (ImGui::Button("Create PAK...")) {
            auto source = dialog(hwnd, true);
            if (!source.empty()) {
                auto target = dialog(hwnd, false, true);
                if (!target.empty()) try {
                    status = "Creating archive...";
                    pakman::create_archive(source, target,
                        {.type = compressed_create ? pakman::ArchiveType::compressed : pakman::ArchiveType::stored,
                         .platform = static_cast<pakman::Platform>(create_platform + 1),
                         .overwrite = true});
                    status = "Created " + utf8(target); open_archive(target);
                } catch (std::exception const& e) { error_box(hwnd, e); status = "Create failed"; }
            }
        }
        ImGui::SameLine(); ImGui::Checkbox("Compressed PAKC", &compressed_create);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90.0f);
        ImGui::Combo("Platform", &create_platform, "PC\0PS2\0Xbox\0");
        std::size_t selected_count = 0;
        if (archive) {
            ImGui::SameLine();
            if (ImGui::Button("Verify")) try { auto r = archive->verify(true); status = "Valid: " + std::to_string(r.compressed_blocks) + " compressed blocks checked"; } catch (std::exception const& e) { error_box(hwnd, e); }
            ImGui::SameLine();
            if (ImGui::Button("Extract all...")) {
                auto folder = dialog(hwnd, true);
                if (!folder.empty()) try { archive->extract(folder); status = "Extracted to " + utf8(folder); } catch (std::exception const& e) { error_box(hwnd, e); status = "Extraction failed"; }
            }
            ImGui::SameLine();
            if (ImGui::Button("Extract selected...")) {
                std::vector<std::size_t> ids; for (std::size_t i = 0; i < selected.size(); ++i) if (selected[i]) ids.push_back(i);
                if (!ids.empty()) { auto folder = dialog(hwnd, true); if (!folder.empty()) try { archive->extract(folder, ids); status = "Extracted selected files"; } catch (std::exception const& e) { error_box(hwnd, e); } }
            }
            ImGui::Separator();
            auto archive_path = utf8(archive->path());
            auto platform = pakman::platform_name(static_cast<pakman::Platform>(archive->platform()));
            ImGui::Text("%s | v%u %s | %zu entries | %s", pakman::type_name(archive->type()).c_str(), archive->version(), platform.c_str(), archive->entries().size(), archive_path.c_str());
            // Explorer-style navigation bar.
            ImGui::BeginDisabled(back_history.empty());
            if (ImGui::Button("<")) { forward_history.push_back(current_directory); auto target = back_history.back(); back_history.pop_back(); current_directory = std::move(target); clear_selection(); }
            ImGui::EndDisabled(); ImGui::SameLine();
            ImGui::BeginDisabled(forward_history.empty());
            if (ImGui::Button(">")) { back_history.push_back(current_directory); auto target = forward_history.back(); forward_history.pop_back(); current_directory = std::move(target); clear_selection(); }
            ImGui::EndDisabled(); ImGui::SameLine();
            ImGui::BeginDisabled(current_directory.empty());
            if (ImGui::Button("Up")) { auto slash = current_directory.find_last_of('/'); navigate(slash == std::string::npos ? "" : current_directory.substr(0, slash)); }
            ImGui::EndDisabled(); ImGui::SameLine();
            if (ImGui::SmallButton("Archive")) navigate("");
            std::string breadcrumb;
            std::size_t component_start = 0;
            while (component_start < current_directory.size()) {
                auto slash = current_directory.find('/', component_start);
                auto component = current_directory.substr(component_start, slash - component_start);
                if (!breadcrumb.empty()) breadcrumb += '/'; breadcrumb += component;
                ImGui::SameLine(); ImGui::TextUnformatted(">"); ImGui::SameLine();
                ImGui::PushID(breadcrumb.c_str()); if (ImGui::SmallButton(component.c_str())) navigate(breadcrumb); ImGui::PopID();
                if (slash == std::string::npos) break; component_start = slash + 1;
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##filter", "Search this folder...", filter, sizeof(filter));

            // Materialize only immediate children of the current directory.
            std::vector<BrowserRow> rows;
            std::map<std::string, std::size_t> directory_rows;
            std::string prefix = current_directory.empty() ? "" : current_directory + '/';
            std::string prefix_lower = lower(prefix);
            for (std::size_t i = 0; i < archive->entries().size(); ++i) {
                auto const& e = archive->entries()[i]; auto full_lower = lower(e.path_utf8);
                if (!prefix_lower.empty() && !full_lower.starts_with(prefix_lower)) continue;
                auto rest = e.path_utf8.substr(prefix.size());
                auto slash = rest.find('/');
                if (slash != std::string::npos) {
                    auto name = rest.substr(0, slash); auto key = lower(name);
                    auto found = directory_rows.find(key);
                    if (found == directory_rows.end()) {
                        BrowserRow row; row.directory = true; row.name = name; row.path = prefix + name; row.type = "Folder";
                        rows.push_back(std::move(row)); directory_rows.emplace(key, rows.size() - 1); found = directory_rows.find(key);
                    }
                    auto& row = rows[found->second]; row.members.push_back(i); row.original += e.original_size; row.stored += e.stored_size;
                } else {
                    BrowserRow row; row.name = rest; row.path = e.path_utf8; row.type = file_type(rest); row.entry = i;
                    row.members.push_back(i); row.original = e.original_size; row.stored = e.stored_size; rows.push_back(std::move(row));
                }
            }
            std::string needle = lower(filter);
            std::erase_if(rows, [&](auto const& row) { return !needle.empty() && lower(row.name + " " + row.type).find(needle) == std::string::npos; });
            auto compare_rows = [&](BrowserRow const& a, BrowserRow const& b) {
                if (a.directory != b.directory) return a.directory > b.directory;
                int cmp = 0;
                if (sort_column == 1) cmp = lower(a.type).compare(lower(b.type));
                else if (sort_column == 2) cmp = a.original < b.original ? -1 : a.original > b.original ? 1 : 0;
                else if (sort_column == 3) cmp = a.stored < b.stored ? -1 : a.stored > b.stored ? 1 : 0;
                else cmp = lower(a.name).compare(lower(b.name));
                if (!cmp) cmp = lower(a.name).compare(lower(b.name));
                return sort_direction == ImGuiSortDirection_Descending ? cmp > 0 : cmp < 0;
            };
            std::sort(rows.begin(), rows.end(), compare_rows);

            ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                    ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable;
            if (ImGui::BeginTable("entries", 5, flags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0, 0);
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110, 1);
                ImGui::TableSetupColumn("Original size", ImGuiTableColumnFlags_WidthFixed, 110, 2);
                ImGui::TableSetupColumn("Stored size", ImGuiTableColumnFlags_WidthFixed, 110, 3);
                ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 70, 4);
                ImGui::TableHeadersRow();
                if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount) {
                    int new_column = static_cast<int>(specs->Specs[0].ColumnUserID);
                    auto new_direction = specs->Specs[0].SortDirection;
                    if (new_column != sort_column || new_direction != sort_direction) { sort_column = new_column; sort_direction = new_direction; selection_anchor = -1; std::sort(rows.begin(), rows.end(), compare_rows); }
                    specs->SpecsDirty = false;
                }
                auto row_selected = [&](BrowserRow const& row) { return !row.members.empty() && std::all_of(row.members.begin(), row.members.end(), [&](auto i) { return selected[i] != 0; }); };
                auto set_row = [&](BrowserRow const& row, bool value) { for (auto i : row.members) selected[i] = value; };
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) for (auto const& row : rows) set_row(row, true);
                for (std::size_t i = 0; i < rows.size(); ++i) {
                    auto const& row = rows[i]; bool is_selected = row_selected(row);
                    ImGui::TableNextRow(0, ImGui::GetTextLineHeightWithSpacing() + 5.0f); ImGui::TableSetColumnIndex(0);
                    if (row.directory) ImGui::PushID(row.path.c_str()); else ImGui::PushID(static_cast<int>(row.entry));
                    ImVec2 p = ImGui::GetCursorScreenPos();
                    bool clicked = ImGui::Selectable("##row", is_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick, {0, ImGui::GetTextLineHeightWithSpacing() + 3.0f});
                    draw_file_icon({p.x + 5, p.y + 1}, row.directory, row.type);
                    ImGui::GetWindowDrawList()->AddText({p.x + 30, p.y + 3}, ImGui::GetColorU32(ImGuiCol_Text), row.name.c_str());
                    bool double_click = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                    if (clicked && !double_click) {
                        if (io.KeyShift && selection_anchor >= 0 && selection_anchor < static_cast<int>(rows.size())) {
                            int anchor = selection_anchor;
                            if (!io.KeyCtrl) std::fill(selected.begin(), selected.end(), 0);
                            int first = std::min(anchor, static_cast<int>(i)), last = std::max(anchor, static_cast<int>(i));
                            for (int n = first; n <= last; ++n) set_row(rows[n], true);
                        } else if (io.KeyCtrl) { set_row(row, !is_selected); selection_anchor = static_cast<int>(i); }
                        else { clear_selection(); set_row(row, true); selection_anchor = static_cast<int>(i); }
                    }
                    if (double_click && row.directory) navigate(row.path);
                    ImGui::PopID(); ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(row.type.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", static_cast<unsigned long long>(row.original));
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%llu", static_cast<unsigned long long>(row.stored));
                    ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f%%", row.original ? 100.0 * row.stored / row.original : 0.0);
                }
                ImGui::EndTable();
            }
            selected_count = std::count_if(selected.begin(), selected.end(), [](auto value) { return value != 0; });
        } else { ImGui::Spacing(); ImGui::TextWrapped("Open a Commandos: Strike Force .pak archive, or create one from a folder."); }
        ImGui::Separator(); ImGui::TextUnformatted(status.c_str());
        if (selected_count) { ImGui::SameLine(); ImGui::TextDisabled("| %zu file(s) selected", selected_count); }
        ImGui::End();
        ImGui::Render(); const float clear[4]{0.08f, 0.09f, 0.11f, 1.0f}; context->OMSetRenderTargets(1, &render_target, nullptr);
        context->ClearRenderTargetView(render_target, clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); swap_chain->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); cleanup_device(); DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, instance); CoUninitialize(); return 0;
}

#include "tray/games_dialog.h"

#include "core/error.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <format>

namespace myalien::tray {

namespace {

constexpr wchar_t kClassName[] = L"MyAlienGamesWindow";
constexpr int kClientW = 640;
constexpr int kClientH = 420;

enum Id : int {
    IDC_LBL_FILTER = 200,
    IDC_FILTER,
    IDC_LIST,
    IDC_HINT,
    IDC_FOOTER,
    IDC_ADD,
    IDC_REMOVE,
    IDC_RESCAN,
    IDC_CLOSE,
};

struct ControlDef {
    int id;
    const wchar_t* cls;
    const wchar_t* text;
    DWORD style;
    DWORD exStyle;
    int x, y, w, h;  // 96-DPI client coordinates
};

constexpr DWORD kStatic = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS;
constexpr DWORD kButton = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;

constexpr ControlDef kControls[] = {
    {IDC_LBL_FILTER, WC_STATICW, L"Filter", kStatic, 0, 12, 15, 44, 20},
    {IDC_FILTER, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, 58, 11, 220, 24},
    {IDC_HINT, WC_STATICW, L"Untick a program to never treat it as a game.", kStatic, 0, 290, 15, 338, 20},
    {IDC_LIST, WC_LISTVIEWW, L"",
     WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
     WS_EX_CLIENTEDGE, 12, 42, 616, 300},
    {IDC_FOOTER, WC_STATICW, L"", kStatic, 0, 12, 350, 616, 20},
    {IDC_ADD, WC_BUTTONW, L"Add folder…", kButton, 0, 12, 380, 100, 28},
    {IDC_REMOVE, WC_BUTTONW, L"Remove", kButton, 0, 118, 380, 90, 28},
    {IDC_RESCAN, WC_BUTTONW, L"Rescan", kButton, 0, 214, 380, 90, 28},
    {IDC_CLOSE, WC_BUTTONW, L"Close", kButton | BS_DEFPUSHBUTTON, 0, 538, 380, 90, 28},
};

bool icontains(const std::wstring& s, const std::wstring& part) {
    if (part.empty()) return true;
    auto low = [](std::wstring v) {
        std::transform(v.begin(), v.end(), v.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return v;
    };
    return low(s).find(low(part)) != std::wstring::npos;
}

bool ieq(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

}  // namespace

GamesDialog::GamesDialog(HINSTANCE instance, Callbacks callbacks)
    : instance_(instance), callbacks_(std::move(callbacks)) {}

GamesDialog::~GamesDialog() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
}

bool GamesDialog::visible() const { return hwnd_ && IsWindowVisible(hwnd_); }

void GamesDialog::show(HWND owner) {
    if (!hwnd_) create(owner);
    if (!hwnd_) return;
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
}

void GamesDialog::create(HWND owner) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    hwnd_ = CreateWindowExW(0, kClassName, L"MyAlien – Games", style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner,
                            nullptr, instance_, this);
    if (!hwnd_) return;
    dpi_ = GetDpiForWindow(hwnd_);
    for (const auto& c : kControls)
        CreateWindowExW(c.exStyle, c.cls, c.text, c.style, 0, 0, 0, 0, hwnd_,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(c.id)), instance_, nullptr);
    SendMessageW(item(IDC_FILTER), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"name or folder"));

    HWND list = item(IDC_LIST);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    const std::pair<const wchar_t*, int> columns[] = {{L"Name", 210}, {L"Source", 104}, {L"Folder or program", 290}};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(columns[i].first);
        col.cx = scale(columns[i].second);
        ListView_InsertColumn(list, i, &col);
    }
    applyFont();
    layout();
    fill();
}

void GamesDialog::applyFont() {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    HFONT fresh = CreateFontIndirectW(&ncm.lfMessageFont);
    for (const auto& c : kControls) SendMessageW(item(c.id), WM_SETFONT, reinterpret_cast<WPARAM>(fresh), FALSE);
    if (font_) DeleteObject(font_);
    font_ = fresh;
}

void GamesDialog::layout() {
    RECT r{0, 0, scale(kClientW), scale(kClientH)};
    AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE, 0, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    for (const auto& c : kControls) MoveWindow(item(c.id), scale(c.x), scale(c.y), scale(c.w), scale(c.h), FALSE);
    sizeColumns();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// Column widths follow the window's DPI (it can move to a monitor with another scale). The last
// column gets exactly the width left (after the vertical scroll bar, if any), so a long path is
// cut with "…" (full text on hover) instead of adding a horizontal scroll bar.
void GamesDialog::sizeColumns() {
    HWND list = item(IDC_LIST);
    ListView_SetColumnWidth(list, 0, scale(210));
    ListView_SetColumnWidth(list, 1, scale(104));
    RECT rc{};
    GetClientRect(list, &rc);
    const int rest = rc.right - scale(210) - scale(104);
    ListView_SetColumnWidth(list, 2, std::max(rest, scale(80)));
}

void GamesDialog::update(const games::Catalog* cat, const GameSettings& settings) {
    rows_.clear();
    if (cat) {
        for (const auto& g : cat->games) {
            const bool excluded = std::any_of(settings.excluded.begin(), settings.excluded.end(),
                                              [&](const std::wstring& e) { return ieq(e, games::gameKey(g)); });
            rows_.push_back({g, !excluded});
        }
    }
    for (const auto& f : settings.folders) {
        Row r;
        r.game.source = games::Source::Manual;
        r.game.dir = games::normalizePath(f);
        const size_t slash = r.game.dir.find_last_of(L'\\');
        r.game.name = slash == std::wstring::npos ? r.game.dir : r.game.dir.substr(slash + 1);
        rows_.push_back(std::move(r));
    }
    std::sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) {
        return CompareStringOrdinal(a.game.name.c_str(), -1, b.game.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });

    if (!cat) {
        footer_ = L"Reading the game list…";
    } else {
        footer_.clear();
        for (const auto& s : cat->sources) {
            if (!s.present || s.games == 0) continue;
            if (!footer_.empty()) footer_ += L" · ";
            footer_ += std::format(L"{} {}", toWide(games::toString(s.source)), s.games);
        }
        if (!settings.folders.empty())
            footer_ += std::format(L"{}Manual {}", footer_.empty() ? L"" : L" · ", settings.folders.size());
        if (footer_.empty()) footer_ = L"No games found";
        footer_ += std::format(L"   (read in {:.0f} ms)", cat->milliseconds);
    }
    if (hwnd_) fill();
}

void GamesDialog::fill() {
    filling_ = true;
    HWND list = item(IDC_LIST);
    wchar_t buf[256] = {};
    GetWindowTextW(item(IDC_FILTER), buf, static_cast<int>(std::size(buf)));
    const std::wstring filter = buf;

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    int n = 0;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& g = rows_[i].game;
        const std::wstring& where = g.source == games::Source::GameBar ? g.exe : g.dir;
        if (!icontains(g.name, filter) && !icontains(where, filter)) continue;
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = n;
        it.pszText = const_cast<wchar_t*>(g.name.c_str());
        it.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(list, &it);
        const std::wstring source = toWide(games::toString(g.source));
        ListView_SetItemText(list, n, 1, const_cast<wchar_t*>(source.c_str()));
        ListView_SetItemText(list, n, 2, const_cast<wchar_t*>(where.c_str()));
        ListView_SetCheckState(list, n, rows_[i].included);
        ++n;
    }
    sizeColumns();  // the vertical scroll bar may have appeared or gone
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    SetWindowTextW(item(IDC_FOOTER), footer_.c_str());
    filling_ = false;
    updateButtons();
}

const GamesDialog::Row* GamesDialog::selectedRow() const {
    HWND list = item(IDC_LIST);
    const int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (sel < 0) return nullptr;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    if (!ListView_GetItem(list, &it) || static_cast<size_t>(it.lParam) >= rows_.size()) return nullptr;
    return &rows_[static_cast<size_t>(it.lParam)];
}

void GamesDialog::updateButtons() {
    const Row* r = selectedRow();
    EnableWindow(item(IDC_REMOVE), r && r->game.source == games::Source::Manual);
}

void GamesDialog::addFolder() {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
            FILEOPENDIALOGOPTIONS opts = 0;
            dlg->GetOptions(&opts);
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dlg->SetTitle(L"Folder of a game (any program inside it counts as the game)");
            Microsoft::WRL::ComPtr<IShellItem> picked;
            PWSTR path = nullptr;
            if (SUCCEEDED(dlg->Show(hwnd_)) && SUCCEEDED(dlg->GetResult(&picked)) &&
                SUCCEEDED(picked->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                const std::wstring folder = games::normalizePath(path);
                CoTaskMemFree(path);
                if (games::isTooBroad(folder)) {
                    MessageBoxW(hwnd_,
                                (folder + L" holds more than one program. Choose the folder of one game.").c_str(),
                                L"MyAlien – Games", MB_OK | MB_ICONWARNING);
                } else if (callbacks_.addFolder) {
                    callbacks_.addFolder(folder);
                }
            }
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
}

// ---------------------------------------------------------------------------------------------
LRESULT CALLBACK GamesDialog::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<GamesDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<GamesDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT GamesDialog::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND: {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == IDC_CLOSE || id == IDCANCEL) {
                ShowWindow(hwnd_, SW_HIDE);
            } else if (id == IDC_FILTER && code == EN_CHANGE) {
                fill();
            } else if (id == IDC_ADD) {
                addFolder();
            } else if (id == IDC_REMOVE) {
                if (const Row* r = selectedRow(); r && r->game.source == games::Source::Manual && callbacks_.removeFolder)
                    callbacks_.removeFolder(r->game.dir);
            } else if (id == IDC_RESCAN) {
                footer_ = L"Reading the game list…";
                SetWindowTextW(item(IDC_FOOTER), footer_.c_str());
                if (callbacks_.rescan) callbacks_.rescan();
            }
            return 0;
        }
        case WM_NOTIFY: {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lp);
            if (hdr->idFrom == IDC_LIST && hdr->code == LVN_GETINFOTIPW) {  // full name and path on hover
                auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(const_cast<NMHDR*>(hdr));
                LVITEMW it{};
                it.mask = LVIF_PARAM;
                it.iItem = tip->iItem;
                if (ListView_GetItem(item(IDC_LIST), &it) && static_cast<size_t>(it.lParam) < rows_.size()) {
                    const auto& g = rows_[static_cast<size_t>(it.lParam)].game;
                    const std::wstring text =
                        g.name + L"\n" + (g.source == games::Source::GameBar ? g.exe : g.dir);
                    wcsncpy_s(tip->pszText, tip->cchTextMax, text.c_str(), _TRUNCATE);
                }
                return 0;
            }
            if (hdr->idFrom != IDC_LIST || hdr->code != LVN_ITEMCHANGED) break;
            const auto* nm = reinterpret_cast<const NMLISTVIEW*>(lp);
            if (nm->uChanged & LVIF_STATE) updateButtons();
            // Checkbox toggled by the user (state image 1 = unchecked, 2 = checked).
            if (filling_ || !(nm->uChanged & LVIF_STATE) ||
                ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK) == 0 || (nm->uOldState & LVIS_STATEIMAGEMASK) == 0)
                return 0;
            if (static_cast<size_t>(nm->lParam) >= rows_.size()) return 0;
            Row& row = rows_[static_cast<size_t>(nm->lParam)];
            const bool included = ((nm->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
            if (row.game.source == games::Source::Manual) {
                if (!included) {  // a manual folder is removed rather than excluded
                    filling_ = true;
                    ListView_SetCheckState(item(IDC_LIST), nm->iItem, TRUE);
                    filling_ = false;
                    MessageBoxW(hwnd_, L"Use Remove to take a folder you added off the list.", L"MyAlien – Games",
                                MB_OK | MB_ICONINFORMATION);
                }
                return 0;
            }
            row.included = included;
            if (callbacks_.setIncluded) callbacks_.setIncluded(games::gameKey(row.game), included);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* s = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, s->left, s->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            applyFont();
            layout();
            return 0;
        }
        case WM_DESTROY:
            hwnd_ = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace myalien::tray

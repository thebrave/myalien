#include "tray/icon.h"

#include "core/awcc.h"

namespace myalien::tray {

namespace {

struct Badge {
    wchar_t letter;
    COLORREF color;
};

Badge badgeFor(std::optional<uint8_t> p) {
    if (!p) return {L'?', RGB(110, 110, 110)};
    switch (*p) {
        case profile::Quiet:
        case profile::QuietLegacy: return {L'Q', RGB(40, 110, 200)};
        case profile::Balanced:
        case profile::BalancedLegacy: return {L'B', RGB(40, 150, 70)};
        case profile::BalancedPerformance:
        case profile::BalancedPerformanceLegacy: return {L'P', RGB(220, 120, 20)};
        case profile::Performance:
        case profile::PerformanceLegacy: return {L'P', RGB(220, 120, 20)};
        case profile::LowPower: return {L'L', RGB(20, 140, 140)};
        case profile::Cool: return {L'K', RGB(60, 160, 210)};
        case profile::Custom: return {L'C', RGB(130, 70, 190)};
        case profile::GMode: return {L'G', RGB(205, 40, 40)};
        default: return {L'?', RGB(110, 110, 110)};
    }
}

}  // namespace

HICON makeProfileIcon(std::optional<uint8_t> profile, int size) {
    const Badge b = badgeFor(profile);

    HDC screen = GetDC(nullptr);
    HDC colorDc = CreateCompatibleDC(screen);
    HDC maskDc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);  // monochrome: 1 = transparent
    ReleaseDC(nullptr, screen);

    HGDIOBJ oldColor = SelectObject(colorDc, color);
    HGDIOBJ oldMask = SelectObject(maskDc, mask);
    const RECT full{0, 0, size, size};
    const int radius = size / 4;

    // Mask: everything transparent except the rounded rectangle.
    FillRect(maskDc, &full, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SelectObject(maskDc, GetStockObject(BLACK_BRUSH));
    SelectObject(maskDc, GetStockObject(BLACK_PEN));
    RoundRect(maskDc, 0, 0, size, size, radius, radius);

    // Colour: badge + letter (pixels outside the shape are black, required by the AND/XOR model).
    FillRect(colorDc, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HBRUSH brush = CreateSolidBrush(b.color);
    HPEN pen = CreatePen(PS_SOLID, 1, b.color);
    HGDIOBJ oldBrush = SelectObject(colorDc, brush);
    HGDIOBJ oldPen = SelectObject(colorDc, pen);
    RoundRect(colorDc, 0, 0, size, size, radius, radius);

    HFONT font = CreateFontW(-(size * 13 / 16), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(colorDc, font);
    SetBkMode(colorDc, TRANSPARENT);
    SetTextColor(colorDc, RGB(255, 255, 255));
    RECT text = full;
    DrawTextW(colorDc, &b.letter, 1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(colorDc, oldFont);
    SelectObject(colorDc, oldBrush);
    SelectObject(colorDc, oldPen);
    SelectObject(colorDc, oldColor);
    SelectObject(maskDc, oldMask);
    DeleteObject(font);
    DeleteObject(brush);
    DeleteObject(pen);
    DeleteDC(colorDc);
    DeleteDC(maskDc);

    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

}  // namespace myalien::tray

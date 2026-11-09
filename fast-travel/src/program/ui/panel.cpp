#include "panel.hpp"

#include "assets.hpp"

namespace warp::ui {
namespace {

// ---- Palette (display values) -----------------------------------------------------------------------------------

constexpr Color kBackTop{25, 28, 35};
constexpr Color kBackBottom{14, 16, 20};
constexpr Color kEdge{70, 77, 90};
constexpr Color kDivider{43, 48, 57};
constexpr Color kGold{242, 199, 110};
constexpr Color kGoldDeep{226, 170, 64};
constexpr Color kOnGold{44, 28, 4};
constexpr Color kText{224, 226, 231};
constexpr Color kTextDim{142, 148, 158};
constexpr Color kTextFaint{96, 102, 113};
constexpr Color kRowSelected{40, 45, 55};
constexpr Color kPlaceholder{33, 37, 45};
constexpr Color kName{244, 239, 228};
constexpr Color kGreen{132, 214, 148};
constexpr Color kWarn{240, 160, 112};
constexpr Color kButton{62, 68, 80};
constexpr Color kButtonLetter{240, 241, 244};
constexpr Color kToggleOff{70, 76, 88};

// ---- Layout (units) ---------------------------------------------------------------------------------------------

constexpr float kPad = 6.0f;
constexpr float kHeaderBase = 15.0f;
constexpr float kHeaderRule = 20.0f;
constexpr float kListX = 6.0f, kListY = 24.0f, kListW = 150.0f, kRowH = 11.0f;
constexpr float kThumbX = 8.0f, kThumbInset = 1.0f;
constexpr float kRowTextX = 28.0f, kRowRight = 152.5f;
constexpr float kPicX = 164.0f, kPicY = 24.0f, kPicW = 150.0f, kPicH = 84.0f;
constexpr float kNameBase = 118.5f, kDetailBase = 126.5f;
constexpr float kFooterRule = 129.5f, kFooterMid = 134.75f;
constexpr float kRight = 314.0f;

struct Layout {
    int s;
    float u(float units) const { return units * static_cast<float>(s); }
    int px(float units) const { return static_cast<int>(units * static_cast<float>(s) + 0.5f); }
};

int pictureOf(int area) {
    static int s_pictures[kAreaCount];
    static bool s_mapped = false;
    if (!s_mapped && assets::ready()) {
        for (int i = 0; i < kAreaCount; ++i) { s_pictures[i] = assets::findPicture(kAreas[i].banc); }
        s_mapped = true;
    }
    return s_mapped && area >= 0 && area < kAreaCount ? s_pictures[area] : -1;
}

// Baseline that centres capital letters on the given middle line.
int centredBase(Font font, int scale, float middlePx) {
    return static_cast<int>(middlePx + assets::capHeight(font, scale) * 0.5f + 0.5f);
}

// Decimal text of a small non-negative number.
struct Number {
    char text[12];
    explicit Number(int value) {
        char reversed[12];
        int n = 0;
        do {
            reversed[n++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value > 0 && n < 11);
        for (int i = 0; i < n; ++i) { text[i] = reversed[n - 1 - i]; }
        text[n] = '\0';
    }
};

// Joins up to four pieces into a short line.
struct Text {
    char text[96];
    int n = 0;
    Text& add(const char* piece) {
        for (const char* c = piece; *c != '\0' && n < static_cast<int>(sizeof(text)) - 1; ++c) { text[n++] = *c; }
        text[n] = '\0';
        return *this;
    }
    Text() { text[0] = '\0'; }
};

// A round button mark with its letter; returns its width.
float buttonMark(Canvas& c, const Layout& l, float x, float middle, const char* letter) {
    const float r = l.u(4.2f);
    c.fillCircle(x + r, middle, r, kButton);
    const int w = assets::textWidth(Font::Button, l.s, letter);
    assets::drawText(c, Font::Button, l.s, static_cast<int>(x + r - w * 0.5f + 0.5f), centredBase(Font::Button, l.s, middle),
                     letter, kButtonLetter);
    return 2 * r;
}

// ---- Header -----------------------------------------------------------------------------------------------------

void paintHeader(Canvas& c, const Layout& l, const MenuView& view) {
    assets::drawText(c, Font::Title, l.s, l.px(kPad + 2), l.px(kHeaderBase), "Fast Travel", kGold);

    // Age tabs, right-aligned: [X] (Young) (Adult); the shown list's tab is filled.
    const float pillH = l.u(10), pillTop = l.u(5), pad = l.u(5), gap = l.u(3);
    const char* labels[2] = {"Young", "Adult"};
    float widths[2];
    for (int i = 0; i < 2; ++i) { widths[i] = assets::textWidth(Font::Small, l.s, labels[i]) + 2 * pad; }
    float x = l.u(kRight) - widths[0] - widths[1] - gap;
    const float middle = pillTop + pillH * 0.5f;
    buttonMark(c, l, x - l.u(12.5f), middle, "X");
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == 0) == (view.list == Age::Young);
        if (on) {
            c.fillRoundRect(x, pillTop, widths[i], pillH, pillH * 0.5f, kGoldDeep);
        } else {
            c.strokeRoundRect(x, pillTop, widths[i], pillH, pillH * 0.5f, l.u(0.4f), kEdge);
        }
        assets::drawText(c, Font::Small, l.s, static_cast<int>(x + pad + 0.5f), centredBase(Font::Small, l.s, middle),
                         labels[i], on ? kOnGold : kTextDim);
        x += widths[i] + gap;
    }
    c.fillRect(l.u(kPad), l.u(kHeaderRule), l.u(kRight - kPad), l.s >= 3 ? 1.5f : 1.0f, kDivider);
}

// ---- List -------------------------------------------------------------------------------------------------------

void paintToggle(Canvas& c, const Layout& l, float x, float middle, bool on) {
    const float w = l.u(14), h = l.u(7.5f);
    c.fillRoundRect(x, middle - h * 0.5f, w, h, h * 0.5f, on ? kGreen : kToggleOff);
    const float r = h * 0.5f - l.u(1.0f);
    c.fillCircle(on ? x + w - h * 0.5f : x + h * 0.5f, middle, r, kButtonLetter);
}

void paintList(Canvas& c, const Layout& l, const MenuView& view) {
    const bool spots = view.page == Page::Spots;
    const Area& area = kAreas[view.area];
    const int listFirst = ageFirst(view.list);
    const int count = spots ? shownSpots(view, area) : kTopRows + shownAreas(view, view.list);
    const int cursor = spots ? view.spot : view.areaRow;
    const int top = spots ? view.spotTop : view.areaTop;

    for (int row = 0; row < kVisibleRows && top + row < count; ++row) {
        const int i = top + row;
        const bool selected = i == cursor;
        const float y = l.u(kListY + row * kRowH);
        const float middle = y + l.u(kRowH * 0.5f);
        if (selected) {
            c.fillRoundRect(l.u(kListX), y, l.u(kListW), l.u(kRowH), l.u(2.5f), kRowSelected);
            c.fillRoundRect(l.u(kListX), y + l.u(2.5f), l.u(1.2f), l.u(kRowH - 5), l.u(0.6f), kGold);
        }
        const Color text = selected ? kGold : kText;
        const int base = centredBase(Font::Body, l.s, middle);
        const int right = l.px(kRowRight);
        if (spots) {
            const Number number(i + 1);
            const int w = assets::textWidth(Font::Small, l.s, number.text);
            assets::drawText(c, Font::Small, l.s, l.px(17) - w, centredBase(Font::Small, l.s, middle), number.text,
                             selected ? kGold : kTextFaint);
            assets::drawText(c, Font::Body, l.s, l.px(21), base, kSpots[shownSpot(view, area, i)].label, text,
                             right - l.px(21));
            continue;
        }
        if (i < kTopRows) {
            paintToggle(c, l, l.u(kThumbX + 1), middle, view.autosave);
            assets::drawText(c, Font::Body, l.s, l.px(kRowTextX), base, "Autosave", text);
            const char* state = view.autosave ? "On" : "Off";
            const int w = assets::textWidth(Font::Small, l.s, state);
            assets::drawText(c, Font::Small, l.s, right - w, centredBase(Font::Small, l.s, middle), state,
                             view.autosave ? kGreen : kTextDim);
            continue;
        }
        const int index = shownArea(view, listFirst, i - kTopRows);
        const Area& rowArea = kAreas[index];
        const Picture thumb = assets::thumbnail(pictureOf(index), l.s);
        const int tx = l.px(kThumbX), ty = static_cast<int>(y + l.u(kThumbInset) + 0.5f);
        if (thumb.rgb != nullptr) {
            c.drawPicture(thumb.rgb, thumb.width, thumb.height, tx, ty, l.u(1.5f));
        } else {
            c.fillRoundRect(tx, ty, l.u(16), l.u(9), l.u(1.5f), kPlaceholder);
        }
        const Number spotsCount(shownSpots(view, rowArea));
        const int w = assets::textWidth(Font::Small, l.s, spotsCount.text);
        assets::drawText(c, Font::Small, l.s, right - w, centredBase(Font::Small, l.s, middle), spotsCount.text,
                         selected ? kGold : kTextFaint);
        assets::drawText(c, Font::Body, l.s, l.px(kRowTextX), base, rowArea.title, text,
                         right - w - l.px(4) - l.px(kRowTextX));
    }

    // Scroll bar when the list is longer than the rows shown.
    if (count > kVisibleRows) {
        const float trackY = l.u(kListY + 1), trackH = l.u(kVisibleRows * kRowH - 2), x = l.u(kListX + kListW + 1.2f);
        c.fillRoundRect(x, trackY, l.u(1.2f), trackH, l.u(0.6f), kDivider);
        float thumbH = trackH * kVisibleRows / count;
        if (thumbH < l.u(8)) { thumbH = l.u(8); }
        const float travel = trackH - thumbH;
        const float pos = trackY + travel * top / static_cast<float>(count - kVisibleRows);
        c.fillRoundRect(x, pos, l.u(1.2f), thumbH, l.u(0.6f), kTextDim);
    }
}

// ---- Picture and name -------------------------------------------------------------------------------------------

void paintDetail(Canvas& c, const Layout& l, const MenuView& view) {
    const bool spots = view.page == Page::Spots;
    const bool autosaveRow = !spots && view.areaRow < kTopRows;
    const float x = l.u(kPicX), y = l.u(kPicY), w = l.u(kPicW), h = l.u(kPicH);
    const float radius = l.u(4);
    const int nameX = l.px(kPicX + 1), maxW = l.px(kPicW - 2);

    if (autosaveRow) {
        c.fillRoundRect(x, y, w, h, radius, kPlaceholder);
        const float mid = y + h * 0.5f;
        paintToggle(c, l, x + w * 0.5f - l.u(7), mid - l.u(6), view.autosave);
        const char* line = view.autosave ? "The game saves as usual" : "No automatic saves until you close the game";
        const int lw = assets::textWidth(Font::Small, l.s, line);
        assets::drawText(c, Font::Small, l.s, static_cast<int>(x + (w - lw) * 0.5f), centredBase(Font::Small, l.s, mid + l.u(8)),
                         line, kTextDim);
        assets::drawText(c, Font::AreaName, l.s, nameX, l.px(kNameBase), "Autosave", kName, maxW);
        assets::drawText(c, Font::Small, l.s, nameX, l.px(kDetailBase), "For this session", kTextDim, maxW);
        return;
    }

    const Area& area = kAreas[view.area];
    const Picture picture = assets::preview(pictureOf(view.area), l.s);
    if (picture.rgb != nullptr) {
        c.drawPicture(picture.rgb, picture.width, picture.height, static_cast<int>(x + 0.5f), static_cast<int>(y + 0.5f),
                      radius);
    } else {
        c.fillRoundRect(x, y, w, h, radius, kPlaceholder);
    }
    c.strokeRoundRect(x, y, w, h, radius, l.s >= 3 ? 1.5f : 1.0f, kEdge);

    assets::drawText(c, Font::AreaName, l.s, nameX, l.px(kNameBase), area.title, kName, maxW);
    const int spotCount = shownSpots(view, area);
    Text detail;
    if (spots) {
        detail.add("Spot ").add(Number(view.spot + 1).text).add(" of ").add(Number(spotCount).text);
    } else {
        detail.add(Number(spotCount).text).add(spotCount == 1 ? " spot" : " spots");
        detail.add(view.list == Age::Young ? "  -  young Link" : "  -  adult Link");
    }
    assets::drawText(c, Font::Small, l.s, nameX, l.px(kDetailBase), detail.text, kTextDim, maxW);
}

// ---- Footer -----------------------------------------------------------------------------------------------------

// What A does on the cursor row, while no warp status is shown.
const char* listHint(const MenuView& view, bool spots, bool autosaveRow) {
#if FAST_TRAVEL_VISITED_ONLY
    if (!spots && shownAreas(view, view.list) == 0) {
        return view.list == Age::Adult && !view.visits.adult ? "Opens when Link first grows up" : "No places visited yet";
    }
#endif
    if (view.link != KnownAge::Unknown && (view.link == KnownAge::Young) != (view.list == Age::Young)) {
        if (spots) { return "Link changes age first, at the Temple of Time"; }
        return view.link == KnownAge::Young ? "Link is young: these warps change his age"
                                            : "Link is adult: these warps change his age";
    }
    if (spots) { return "Pick a spot to warp to"; }
    if (autosaveRow) { return "Turn the game's automatic saves on or off"; }
    return "Pick an area";
}

Color statusColor(Status status) {
    if (inProgress(status) || status == Status::AutosaveOn || status == Status::AutosaveOff) { return kGreen; }
    return kWarn;
}

void paintFooter(Canvas& c, const Layout& l, const MenuView& view) {
    c.fillRect(l.u(kPad), l.u(kFooterRule), l.u(kRight - kPad), l.s >= 3 ? 1.5f : 1.0f, kDivider);
    const bool spots = view.page == Page::Spots;
    const bool autosaveRow = !spots && view.areaRow < kTopRows;
    const float middle = l.u(kFooterMid);

    struct Hint {
        const char* letter;
        const char* label;
    };
    const Hint hints[2] = {{"A", spots ? "Warp" : (autosaveRow ? "Switch" : "Open")}, {"B", spots ? "Back" : "Close"}};
    float x = l.u(kRight);
    for (int i = 1; i >= 0; --i) {
        const int w = assets::textWidth(Font::Small, l.s, hints[i].label);
        x -= w;
        assets::drawText(c, Font::Small, l.s, static_cast<int>(x + 0.5f), centredBase(Font::Small, l.s, middle),
                         hints[i].label, kTextDim);
        x -= l.u(2) + l.u(8.4f);
        buttonMark(c, l, x, middle, hints[i].letter);
        x -= l.u(7);
    }

    const bool ready = view.status == Status::Ready;
    const char* status = ready ? listHint(view, spots, autosaveRow) : statusText(view.status);
    const Color color = ready ? kTextFaint : statusColor(view.status);
    const int maxW = static_cast<int>(x - l.u(kPad + 2) - l.u(4));
    assets::drawText(c, Font::Small, l.s, l.px(kPad + 2), centredBase(Font::Small, l.s, middle), status, color, maxW);
}

}  // namespace

void paintPanel(Canvas& canvas, const MenuView& view, int scale) {
    const Layout l{scale};
    canvas.fillGradient(kBackTop, kBackBottom);
    // The panel's rim; the overlay copies only the inside of the same rounded outline onto the screen.
    canvas.strokeRoundRect(0, 0, canvas.width(), canvas.height(), panelCorner(scale), scale >= 3 ? 1.5f : 1.0f, kEdge);
    paintHeader(canvas, l, view);
    paintList(canvas, l, view);
    paintDetail(canvas, l, view);
    paintFooter(canvas, l, view);
}

}  // namespace warp::ui

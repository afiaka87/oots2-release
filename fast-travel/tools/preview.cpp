// Paints the Fast Travel panel on a PC for a few menu states, in both layouts (list only, and with empty picture
// frames), and writes each as a PPM file, to check the layout without the game. From the Fast Travel folder:
//   uv run --no-project --with pillow python tools/make_assets.py fonts <gen>/fonts.inc
//   clang++ -std=gnu++23 -Itools/host -Isrc/program -I<gen> tools/preview.cpp src/program/ui/assets.cpp
//       src/program/ui/canvas.cpp src/program/ui/panel.cpp -o preview
//   preview <out dir>
#include <cstdio>
#include <vector>

#include "ui/assets.hpp"
#include "ui/panel.hpp"
#include "ui/pictures.hpp"

// No picture file on a PC: the picture layout draws empty frames.
namespace warp::ui::pictures {
bool available() { return false; }
int find(const char*) { return -1; }
Picture thumbnail(int, int) { return {}; }
Picture preview(int, int) { return {}; }
}  // namespace warp::ui::pictures

namespace warp {
const char* statusText(Status status) {
    switch (status) {
    case Status::ChangingAge: return "Changing age...";
    case Status::SceneRunning: return "Not now: a scene is playing";
    default: return "Ready";
    }
}
}  // namespace warp

using namespace warp;

static void write(const char* dir, const char* name, int scale, const MenuView& view, bool pictures) {
    const int w = ui::panelWidth(scale, pictures), h = ui::panelHeight(scale);
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    ui::Canvas canvas(px.data(), w, h, w);
    ui::paintPanel(canvas, view, scale, pictures);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s-%s-s%d.ppm", dir, pictures ? "pictures" : "list", name, scale);
    FILE* f = std::fopen(path, "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (uint32_t p : px) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8),
                                      static_cast<unsigned char>(p >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) { return 1; }
    if (!ui::assets::init()) { std::puts("bad fonts"); return 1; }

    MenuView areas;
    areas.open = true;
    areas.list = Age::Young;
    areas.link = KnownAge::Young;
    areas.areaRow = 1;
    areas.area = 0;
    MenuView adult = areas;
    adult.list = Age::Adult;
    adult.areaRow = 6;
    adult.areaTop = 2;
    adult.area = ageFirst(Age::Adult) + 5;
    adult.status = Status::ChangingAge;
    MenuView spots = areas;
    spots.page = Page::Spots;
    spots.area = 4;
    spots.spot = 3;
    MenuView otherAge = areas;  // the adult list while Link is young: the longest list hint
    otherAge.list = Age::Adult;
    otherAge.area = ageFirst(Age::Adult);
    MenuView autosave = areas;
    autosave.areaRow = 0;
    autosave.autosave = false;
    for (int scale : {3, 2}) {
        for (bool pictures : {false, true}) {
            write(argv[1], "areas", scale, areas, pictures);
            write(argv[1], "adult", scale, adult, pictures);
            write(argv[1], "spots", scale, spots, pictures);
            write(argv[1], "autosave", scale, autosave, pictures);
            write(argv[1], "other-age", scale, otherAge, pictures);
        }
    }
    std::puts("ok");
    return 0;
}

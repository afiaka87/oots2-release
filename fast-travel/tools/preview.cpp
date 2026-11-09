// Paints the Fast Travel panel on a PC for a few menu states and writes each as a PPM file, to check the layout
// without the game:
//   clang -c src/program/ext/picojpeg.c -o picojpeg.o
//   clang++ -std=gnu++23 -Itools/host -Isrc/program tools/preview.cpp src/program/ui/*.cpp picojpeg.o -o preview
//   preview data/assets.bin <out dir>
#include <cstdio>
#include <vector>

#include "ui/assets.hpp"
#include "ui/panel.hpp"

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

static void write(const char* dir, const char* name, int scale, const MenuView& view) {
    const int w = ui::panelWidth(scale), h = ui::panelHeight(scale);
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    ui::Canvas canvas(px.data(), w, h);
    ui::paintPanel(canvas, view, scale);
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s-s%d.ppm", dir, name, scale);
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
    if (argc < 3) { return 1; }
    FILE* f = std::fopen(argv[1], "rb");
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> blob(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    std::fread(blob.data(), 1, blob.size(), f);
    std::fclose(f);
    if (!ui::assets::init(blob.data(), blob.size())) { std::puts("bad assets"); return 1; }

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
    MenuView autosave = areas;
    autosave.areaRow = 0;
    autosave.autosave = false;
    for (int scale : {3, 2}) {
        write(argv[2], "areas", scale, areas);
        write(argv[2], "adult", scale, adult);
        write(argv[2], "spots", scale, spots);
        write(argv[2], "autosave", scale, autosave);
    }
    std::puts("ok");
    return 0;
}

// Decodes JPEG files on a PC with the menu's decoder (src/program/ui/jpeg.cpp), times it, and writes the large and
// small pictures as PPM files, to compare with a reference scaler. From the Fast Travel folder:
//   clang++ -std=gnu++23 -O2 -Isrc/program tools/host/jpeg_check.cpp src/program/ui/jpeg.cpp -o jpeg_check
//   jpeg_check <out dir> <large w> <large h> <small w> <small h> <file.jpg>...
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ui/jpeg.hpp"

static size_t readFile(void* context, int64_t offset, void* buffer, size_t bytes) {
    FILE* f = static_cast<FILE*>(context);
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) { return 0; }
    return std::fread(buffer, 1, bytes, f);
}

static void writePpm(const std::string& path, const std::vector<uint8_t>& rgb, int w, int h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::printf("%s: cannot write\n", path.c_str());
        return;
    }
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::fwrite(rgb.data(), 1, rgb.size(), f);
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 7) { return 1; }
    const std::string out = argv[1];
    const int lw = std::atoi(argv[2]), lh = std::atoi(argv[3]), sw = std::atoi(argv[4]), sh = std::atoi(argv[5]);
    std::vector<uint8_t> large(static_cast<size_t>(lw) * lh * 3), small(static_cast<size_t>(sw) * sh * 3);
    std::vector<uint8_t> smallOnly(small.size());
    double fullMs = 0, smallMs = 0;
    for (int i = 6; i < argc; ++i) {
        FILE* f = std::fopen(argv[i], "rb");
        if (f == nullptr) {
            std::printf("%s: cannot open\n", argv[i]);
            continue;
        }
        const auto t0 = std::chrono::steady_clock::now();
        const char* error = warp::ui::jpeg::decode(readFile, f, {large.data(), lw, lh}, {small.data(), sw, sh});
        const auto t1 = std::chrono::steady_clock::now();
        const char* error2 = warp::ui::jpeg::decode(readFile, f, {}, {smallOnly.data(), sw, sh});
        const auto t2 = std::chrono::steady_clock::now();
        std::fclose(f);
        fullMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        smallMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
        if (error != nullptr || error2 != nullptr) {
            std::printf("%s: %s\n", argv[i], error != nullptr ? error : error2);
            continue;
        }
        std::string name = argv[i];
        name = name.substr(name.find_last_of("/\\") + 1);
        name = name.substr(0, name.find_last_of('.'));
        writePpm(out + "/" + name + "-large.ppm", large, lw, lh);
        writePpm(out + "/" + name + "-small.ppm", small, sw, sh);
        writePpm(out + "/" + name + "-small-fast.ppm", smallOnly, sw, sh);
    }
    std::printf("%d files: full decode %.2f ms each, small-only %.2f ms each\n", argc - 6, fullMs / (argc - 6),
                smallMs / (argc - 6));
    return 0;
}

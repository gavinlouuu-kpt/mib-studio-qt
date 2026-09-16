#include "../../tools/sync_tuning/intensity.h"
#include "support/assert.h"
#include <functional>

int main() {
    const auto flipped = mib::sync::orientRegion({3, 5, 4, 6}, 20, 20, true, true);
    MIB_EXPECT(flipped.x == 13 && flipped.y == 9, "flipped full-sensor coordinates");
    const auto crop = mib::sync::orientRegion({0, 3, 4, 6}, 4, 12, true, true);
    MIB_EXPECT(crop.x == 0 && crop.y == 3, "flipped crop coordinates");
    backend::playback::Frame frame;
    frame.width = 16;
    frame.height = 12;
    frame.linePitch = 20;
    frame.pixelFormat = 0x01080001;
    frame.data.assign(240, 255);
    for (size_t y = 0; y < 12; ++y)
        for (size_t x = 0; x < 16; ++x)
            frame.data[y * 20 + x] = 100;
    auto value = mib::sync::measure(frame, {0, 0, 16, 12});
    MIB_EXPECT(value.mean == 100 && value.spatialSd == 0, "constant image excludes padding");
    MIB_EXPECT(value.clippedFraction == 0 && value.darkFraction == 0, "padding is not clipping");
    for (auto band : value.bands)
        MIB_EXPECT(band == 100, "three bands cover image");

    for (size_t y = 0; y < 12; ++y)
        for (size_t x = 0; x < 16; ++x)
            frame.data[y * 20 + x] = x < 8 ? 10 : 250;
    value = mib::sync::measure(frame, {0, 0, 16, 12});
    MIB_EXPECT(value.mean == 130 && value.spatialSd == 120, "known spatial distribution");
    MIB_EXPECT(value.clippedFraction == .5 && value.darkFraction == .5,
               "inclusive intensity thresholds");
    value = mib::sync::measure(frame, {8, 2, 8, 6});
    MIB_EXPECT(value.mean == 250 && value.clippedFraction == 1, "offset sensor region");

    auto rejects = [&](const std::function<void()>& fn) {
        bool threw = false;
        try {
            fn();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        MIB_EXPECT(threw, "malformed frame/region rejected before indexing");
    };
    rejects([&] { mib::sync::measure(frame, {-1, 0, 16, 12}); });
    rejects([&] { mib::sync::measure(frame, {0, 0, 17, 12}); });
    rejects([&] { mib::sync::measure(frame, {0, 0, 16, 5}); });
    frame.pixelFormat = 0;
    rejects([&] { mib::sync::measure(frame, {0, 0, 16, 12}); });
    frame.pixelFormat = 0x01080001;
    frame.data.resize(239);
    rejects([&] { mib::sync::measure(frame, {0, 0, 16, 12}); });
    frame.linePitch = 0;
    rejects([&] { mib::sync::measure(frame, {0, 0, 16, 12}); });
    return mib::test::exitCode();
}

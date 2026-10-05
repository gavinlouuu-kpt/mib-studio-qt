#pragma once

// Bit-exact host model of the C4 U-Net that the PZ7035 PL runs (plan W3.D):
// the integer network the FINN mapping produces (W8A8, power-of-two scales),
// so desktop reprocessing gets exactly the PL's masks. Parameters come from
// the .npz that pz7035-imx426 scripts/dump_unet_params.py writes from the
// mapped graph (W0..W5 weights, T0..T5 ascending thresholds, A0..A5 output
// offsets), e.g. the promoted unet-c4-multiline-v1 release.
//
// Network (512x96 Mono8 window): e0 = conv3(x) 4 ch; e1 = conv3(pool(e0)) 8;
// b = conv3(pool(e1)) 16; d1 = conv3(concat(e1, up(b))) 8;
// d0 = conv3(concat(e0, up(d1))) 4; out = 1x1(d0) 1 ch. Every activation is
// a threshold count (number of thresholds <= the accumulator) plus the
// layer's offset. Activations NHWC, 3x3 windows flattened (ky*3+kx)*C + c,
// zero padding, the skip tensor first in a concat (FINN layout).
//
// Qt-free; part of mib_processing.

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace backend::processing {

class UnetC4 {
public:
    static constexpr int kWidth = 512;
    static constexpr int kHeight = 96;

    // Load an uncompressed .npz (np.savez). nullopt with `error` on a missing
    // array, a wrong shape, an unsupported dtype or offset, or thresholds that
    // are not ascending.
    static std::optional<UnetC4> loadNpz(const std::string& path, std::string* error);

    // codes: 96x512 CV_8SC1 input codes (raw Mono8 XOR 0x80). Returns the
    // 96x512 CV_8SC1 output codes (logit * 8); foreground is code > 0.
    cv::Mat run(const cv::Mat& codes) const;

    // Raw 96x512 Mono8 -> the foreground mask (0 / 255) the PL uses.
    cv::Mat foregroundMask(const cv::Mat& gray) const;

private:
    struct Layer {
        int inputs{0};  // MW
        int outputs{0}; // MH
        std::vector<int32_t> weights;                 // [MH][MW]
        std::vector<std::vector<int64_t>> thresholds; // [MH][steps], ascending
        int offset{0};
    };
    std::array<Layer, 6> layers_;
};

} // namespace backend::processing

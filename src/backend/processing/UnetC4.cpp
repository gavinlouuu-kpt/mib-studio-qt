#include "backend/processing/UnetC4.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace backend::processing {

namespace {

// (MW, MH) per layer: conv3 layers take 9 * C inputs; layer 5 is 1x1.
constexpr std::array<std::pair<int, int>, 6> kShapes = {{{9, 4}, {36, 8}, {72, 16}, {216, 8}, {108, 4}, {4, 1}}};
constexpr std::array<int, 6> kOffsets = {0, 0, 0, 0, 0, -128};

struct NpyArray {
    std::vector<int64_t> shape;
    std::vector<int64_t> data;
};

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

// One .npy (version 1/2/3), little-endian int64 or int32; C order, or Fortran
// order for 2-D arrays (np.save of a transposed view writes that).
NpyArray parseNpy(const uint8_t* p, size_t n) {
    if (n < 10 || std::memcmp(p, "\x93NUMPY", 6) != 0) throw std::runtime_error("not an .npy array");
    const int major = p[6];
    size_t headerLen = 0, start = 0;
    if (major == 1) {
        headerLen = le16(p + 8);
        start = 10;
    } else {
        headerLen = le32(p + 8);
        start = 12;
    }
    if (start + headerLen > n) throw std::runtime_error("truncated .npy header");
    const std::string header(reinterpret_cast<const char*>(p + start), headerLen);
    const bool fortran = header.find("'fortran_order': True") != std::string::npos;
    size_t elem = 0;
    if (header.find("'<i8'") != std::string::npos) elem = 8;
    else if (header.find("'<i4'") != std::string::npos) elem = 4;
    else throw std::runtime_error("unsupported dtype (want <i8 or <i4)");
    NpyArray a;
    const auto s0 = header.find("'shape': (");
    if (s0 == std::string::npos) throw std::runtime_error("no shape");
    const auto s1 = header.find(')', s0);
    std::string dims = header.substr(s0 + 10, s1 - s0 - 10);
    size_t count = 1;
    for (size_t pos = 0; pos < dims.size();) {
        while (pos < dims.size() && (dims[pos] == ' ' || dims[pos] == ',')) ++pos;
        if (pos >= dims.size()) break;
        size_t end = pos;
        while (end < dims.size() && std::isdigit(static_cast<unsigned char>(dims[end]))) ++end;
        const int64_t d = std::stoll(dims.substr(pos, end - pos));
        a.shape.push_back(d);
        count *= static_cast<size_t>(d);
        pos = end;
    }
    const uint8_t* data = p + start + headerLen;
    if (start + headerLen + count * elem > n) throw std::runtime_error("truncated .npy data");
    if (fortran && a.shape.size() > 2) throw std::runtime_error("Fortran-order array of rank > 2");
    a.data.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (elem == 8) {
            int64_t v;
            std::memcpy(&v, data + 8 * i, 8);
            a.data[i] = v;
        } else {
            int32_t v;
            std::memcpy(&v, data + 4 * i, 4);
            a.data[i] = v;
        }
    }
    if (fortran && a.shape.size() == 2) { // column-major on disk -> row-major
        const size_t rows = static_cast<size_t>(a.shape[0]), cols = static_cast<size_t>(a.shape[1]);
        std::vector<int64_t> rowMajor(count);
        for (size_t r = 0; r < rows; ++r)
            for (size_t c = 0; c < cols; ++c) rowMajor[r * cols + c] = a.data[c * rows + r];
        a.data.swap(rowMajor);
    }
    return a;
}

// Members of a stored (uncompressed) zip, as np.savez writes it.
std::map<std::string, NpyArray> readNpz(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    const std::vector<uint8_t> z((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::map<std::string, NpyArray> out;
    size_t pos = 0;
    while (pos + 30 <= z.size() && le32(&z[pos]) == 0x04034b50u) {
        const uint16_t flags = le16(&z[pos + 6]);
        const uint16_t method = le16(&z[pos + 8]);
        uint64_t size = le32(&z[pos + 18]);
        const uint16_t nameLen = le16(&z[pos + 26]);
        const uint16_t extraLen = le16(&z[pos + 28]);
        if (method != 0) throw std::runtime_error("compressed .npz (use np.savez, not savez_compressed)");
        if (flags & 0x08) throw std::runtime_error("zip data descriptors are not supported");
        std::string name(reinterpret_cast<const char*>(&z[pos + 30]), nameLen);
        const size_t extraStart = pos + 30 + nameLen;
        if (size == 0xFFFFFFFFu && extraLen >= 20) { // zip64 extra: header id, size, uncompressed, compressed
            uint64_t v;
            std::memcpy(&v, &z[extraStart + 4], 8);
            size = v;
        }
        const size_t data = extraStart + extraLen;
        if (data + size > z.size()) throw std::runtime_error("truncated .npz member " + name);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".npy") == 0) name.resize(name.size() - 4);
        out.emplace(name, parseNpy(&z[data], static_cast<size_t>(size)));
        pos = data + static_cast<size_t>(size);
    }
    return out;
}

// HWC int32 tensor.
struct Tensor {
    int h{0}, w{0}, c{0};
    std::vector<int32_t> v;
    Tensor(int h_, int w_, int c_) : h(h_), w(w_), c(c_), v(static_cast<size_t>(h_) * w_ * c_, 0) {}
    int32_t& at(int y, int x, int k) { return v[(static_cast<size_t>(y) * w + x) * c + k]; }
    int32_t at(int y, int x, int k) const { return v[(static_cast<size_t>(y) * w + x) * c + k]; }
};

int32_t activate(int64_t acc, const std::vector<int64_t>& t, int offset) {
    // number of thresholds <= acc (thresholds ascending)
    return static_cast<int32_t>(std::upper_bound(t.begin(), t.end(), acc) - t.begin()) + offset;
}

} // namespace

std::optional<UnetC4> UnetC4::loadNpz(const std::string& path, std::string* error) {
    try {
        const auto arrays = readNpz(path);
        UnetC4 m;
        for (int k = 0; k < 6; ++k) {
            const auto [mw, mh] = kShapes[static_cast<size_t>(k)];
            auto find = [&](const std::string& n) -> const NpyArray& {
                const auto it = arrays.find(n);
                if (it == arrays.end()) throw std::runtime_error("missing array " + n);
                return it->second;
            };
            const auto& W = find("W" + std::to_string(k));
            const auto& T = find("T" + std::to_string(k));
            const auto& A = find("A" + std::to_string(k));
            if (W.shape != std::vector<int64_t>{mh, mw}) throw std::runtime_error("W" + std::to_string(k) + " shape");
            if (T.shape.size() != 2 || T.shape[0] != mh || T.shape[1] < 1)
                throw std::runtime_error("T" + std::to_string(k) + " shape");
            if (A.data.size() != 1 || A.data[0] != kOffsets[static_cast<size_t>(k)])
                throw std::runtime_error("A" + std::to_string(k) + " offset not supported");
            Layer& L = m.layers_[static_cast<size_t>(k)];
            L.inputs = mw;
            L.outputs = mh;
            L.offset = static_cast<int>(A.data[0]);
            L.weights.assign(W.data.begin(), W.data.end());
            const size_t steps = static_cast<size_t>(T.shape[1]);
            for (int o = 0; o < mh; ++o) {
                std::vector<int64_t> t(T.data.begin() + o * steps, T.data.begin() + (o + 1) * steps);
                if (!std::is_sorted(t.begin(), t.end()))
                    throw std::runtime_error("T" + std::to_string(k) + " not ascending");
                L.thresholds.push_back(std::move(t));
            }
        }
        return m;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

namespace {

Tensor conv3(const Tensor& x, const std::vector<int32_t>& W, int mh, const std::vector<std::vector<int64_t>>& T,
             int offset) {
    Tensor out(x.h, x.w, mh);
    const int c = x.c;
    const int mw = 9 * c;
    std::vector<int32_t> window(static_cast<size_t>(mw));
    for (int y = 0; y < x.h; ++y) {
        for (int xx = 0; xx < x.w; ++xx) {
            for (int ky = 0; ky < 3; ++ky)
                for (int kx = 0; kx < 3; ++kx) {
                    const int sy = y + ky - 1, sx = xx + kx - 1;
                    const bool inside = sy >= 0 && sy < x.h && sx >= 0 && sx < x.w;
                    for (int k = 0; k < c; ++k)
                        window[static_cast<size_t>((ky * 3 + kx) * c + k)] = inside ? x.at(sy, sx, k) : 0;
                }
            for (int o = 0; o < mh; ++o) {
                const int32_t* w = &W[static_cast<size_t>(o) * mw];
                int64_t acc = 0;
                for (int i = 0; i < mw; ++i) acc += static_cast<int64_t>(w[i]) * window[static_cast<size_t>(i)];
                out.at(y, xx, o) = activate(acc, T[static_cast<size_t>(o)], offset);
            }
        }
    }
    return out;
}

Tensor pool(const Tensor& x) {
    Tensor out(x.h / 2, x.w / 2, x.c);
    for (int y = 0; y < out.h; ++y)
        for (int xx = 0; xx < out.w; ++xx)
            for (int k = 0; k < x.c; ++k)
                out.at(y, xx, k) = std::max({x.at(2 * y, 2 * xx, k), x.at(2 * y, 2 * xx + 1, k),
                                             x.at(2 * y + 1, 2 * xx, k), x.at(2 * y + 1, 2 * xx + 1, k)});
    return out;
}

// concat(skip, up2(deep)) along channels.
Tensor concatUp(const Tensor& skip, const Tensor& deep) {
    Tensor out(skip.h, skip.w, skip.c + deep.c);
    for (int y = 0; y < skip.h; ++y)
        for (int xx = 0; xx < skip.w; ++xx) {
            for (int k = 0; k < skip.c; ++k) out.at(y, xx, k) = skip.at(y, xx, k);
            for (int k = 0; k < deep.c; ++k) out.at(y, xx, skip.c + k) = deep.at(y / 2, xx / 2, k);
        }
    return out;
}

} // namespace

cv::Mat UnetC4::run(const cv::Mat& codes) const {
    if (codes.rows != kHeight || codes.cols != kWidth || codes.type() != CV_8SC1)
        throw std::invalid_argument("UnetC4::run expects 96x512 CV_8SC1 codes");
    Tensor x(kHeight, kWidth, 1);
    for (int y = 0; y < kHeight; ++y)
        for (int xx = 0; xx < kWidth; ++xx) x.at(y, xx, 0) = codes.at<int8_t>(y, xx);
    const auto& L = layers_;
    const Tensor e0 = conv3(x, L[0].weights, L[0].outputs, L[0].thresholds, L[0].offset);
    const Tensor e1 = conv3(pool(e0), L[1].weights, L[1].outputs, L[1].thresholds, L[1].offset);
    const Tensor b = conv3(pool(e1), L[2].weights, L[2].outputs, L[2].thresholds, L[2].offset);
    const Tensor d1 = conv3(concatUp(e1, b), L[3].weights, L[3].outputs, L[3].thresholds, L[3].offset);
    const Tensor d0 = conv3(concatUp(e0, d1), L[4].weights, L[4].outputs, L[4].thresholds, L[4].offset);
    cv::Mat out(kHeight, kWidth, CV_8SC1);
    const auto& head = L[5];
    for (int y = 0; y < kHeight; ++y)
        for (int xx = 0; xx < kWidth; ++xx) {
            int64_t acc = 0;
            for (int k = 0; k < head.inputs; ++k) acc += static_cast<int64_t>(head.weights[static_cast<size_t>(k)]) * d0.at(y, xx, k);
            out.at<int8_t>(y, xx) = static_cast<int8_t>(activate(acc, head.thresholds[0], head.offset));
        }
    return out;
}

cv::Mat UnetC4::foregroundMask(const cv::Mat& gray) const {
    if (gray.rows != kHeight || gray.cols != kWidth || gray.type() != CV_8UC1)
        throw std::invalid_argument("UnetC4::foregroundMask expects 96x512 Mono8");
    cv::Mat codes(kHeight, kWidth, CV_8SC1);
    for (int y = 0; y < kHeight; ++y)
        for (int xx = 0; xx < kWidth; ++xx)
            codes.at<int8_t>(y, xx) = static_cast<int8_t>(gray.at<uint8_t>(y, xx) ^ 0x80);
    const cv::Mat out = run(codes);
    cv::Mat mask(kHeight, kWidth, CV_8UC1);
    for (int y = 0; y < kHeight; ++y)
        for (int xx = 0; xx < kWidth; ++xx) mask.at<uint8_t>(y, xx) = out.at<int8_t>(y, xx) > 0 ? 255 : 0;
    return mask;
}

} // namespace backend::processing

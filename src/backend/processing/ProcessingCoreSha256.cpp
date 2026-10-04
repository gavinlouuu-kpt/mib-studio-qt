#include "backend/processing/ProcessingCoreSha256.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace backend::processing {
namespace {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

uint32_t rotateRight(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32u - bits));
}

class Sha256 {
public:
    void update(const uint8_t* bytes, size_t count) {
        totalBytes_ += count;
        for (size_t i = 0; i < count; ++i) {
            block_[blockSize_++] = bytes[i];
            if (blockSize_ == block_.size()) {
                transform(block_.data());
                blockSize_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish() {
        const uint64_t totalBits = totalBytes_ * 8u;
        block_[blockSize_++] = 0x80u;
        if (blockSize_ > 56u) {
            while (blockSize_ < 64u) block_[blockSize_++] = 0u;
            transform(block_.data());
            blockSize_ = 0;
        }
        while (blockSize_ < 56u) block_[blockSize_++] = 0u;
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[blockSize_++] = static_cast<uint8_t>(totalBits >> shift);
        }
        transform(block_.data());

        std::array<uint8_t, 32> digest{};
        for (size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<uint8_t>(state_[i] >> 24u);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16u);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8u);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        return digest;
    }

private:
    void transform(const uint8_t* block) {
        std::array<uint32_t, 64> words{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t offset = i * 4;
            words[i] = (static_cast<uint32_t>(block[offset]) << 24u) |
                       (static_cast<uint32_t>(block[offset + 1]) << 16u) |
                       (static_cast<uint32_t>(block[offset + 2]) << 8u) |
                       static_cast<uint32_t>(block[offset + 3]);
        }
        for (size_t i = 16; i < words.size(); ++i) {
            const uint32_t s0 = rotateRight(words[i - 15], 7) ^
                                rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3u);
            const uint32_t s1 = rotateRight(words[i - 2], 17) ^
                                rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10u);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (size_t i = 0; i < words.size(); ++i) {
            const uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const uint32_t choice = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + sum1 + choice + kRoundConstants[i] + words[i];
            const uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                   0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                   0x1f83d9abu, 0x5be0cd19u};
    std::array<uint8_t, 64> block_{};
    size_t blockSize_{0};
    uint64_t totalBytes_{0};
};

} // namespace

std::string processingCoreBytesSha256(const uint8_t* bytes, size_t count) {
    Sha256 sha;
    if (bytes && count > 0) sha.update(bytes, count);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint8_t byte : sha.finish()) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

std::string processingCoreFileSha256(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "cannot open processing core for hashing";
        return {};
    }
    Sha256 sha;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            sha.update(reinterpret_cast<const uint8_t*>(buffer.data()),
                       static_cast<size_t>(count));
        }
    }
    if (!input.eof()) {
        if (error) *error = "failed while hashing processing core";
        return {};
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint8_t byte : sha.finish()) output << std::setw(2) << static_cast<unsigned>(byte);
    return output.str();
}

} // namespace backend::processing

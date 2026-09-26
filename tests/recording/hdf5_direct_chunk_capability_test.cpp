// Capability guard for ADR 0006 (lossless HDF5 compression), epic PR 0.
//
// The planned writer compresses image chunks with zlib off the HDF5 thread,
// stores them with H5Dwrite_chunk into a deflate-filtered dataset, and writes
// chunks it could not compress in time raw with the deflate bit set in the
// chunk filter mask. A partially filled tail chunk is written raw (padded) so
// its frames are durable, then rewritten when the chunk fills. This test
// proves, against whatever HDF5 the lane links, that:
//   1. deflate encode+decode are available and H5Dwrite_chunk exists
//      (prints one HDF5_CAPABILITY line, including H5is_library_threadsafe);
//   2. compressed, raw and padded edge chunks mix in one dataset and read back
//      byte-identical through plain H5Dread and Hdf5Service::readImageByIndex;
//   3. (spike S1) rewriting the raw tail 9x per chunk before the compressed
//      write grows the file by < 5 % versus direct compressed writes.
#include "backend/recording/Hdf5Service.h"
#include "support/assert.h"
#include "support/tempdir.h"

#include <hdf5.h>
#include <opencv2/core.hpp>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
constexpr hsize_t kH = 96;
constexpr hsize_t kW = 512;
constexpr hsize_t kChunkFrames = 10; // ADR 0006 D2 at 512x96
constexpr size_t kFrameBytes = static_cast<size_t>(kH * kW);
constexpr size_t kChunkBytes = kChunkFrames * kFrameBytes;
const char* const kDataset = "/recorded_frames/images";

// Deterministic frames shaped like the 512x96 stream: a smooth background
// plus bounded noise, so deflate compresses them but not trivially.
std::vector<uint8_t> makeFrames(size_t frames, uint32_t seed)
{
    std::vector<uint8_t> out(frames * kFrameBytes);
    uint32_t s = seed;
    for (size_t f = 0; f < frames; ++f)
        for (hsize_t y = 0; y < kH; ++y)
            for (hsize_t x = 0; x < kW; ++x)
            {
                s = s * 1664525u + 1013904223u;
                const int base = 120 + static_cast<int>((x + 3 * y + f) % 60);
                const int noise = static_cast<int>((s >> 24) % 25) - 12;
                out[f * kFrameBytes + y * kW + x] = static_cast<uint8_t>(base + noise);
            }
    return out;
}

std::vector<uint8_t> deflateBytes(const uint8_t* data, size_t n)
{
    uLongf cap = compressBound(static_cast<uLong>(n));
    std::vector<uint8_t> out(cap);
    if (compress2(out.data(), &cap, data, static_cast<uLong>(n), 1) != Z_OK)
        return {};
    out.resize(cap);
    return out;
}

hid_t createDeflateDataset(hid_t file)
{
    hid_t lcpl = H5Pcreate(H5P_LINK_CREATE);
    H5Pset_create_intermediate_group(lcpl, 1);
    const hsize_t dims[3] = {0, kH, kW};
    const hsize_t maxDims[3] = {H5S_UNLIMITED, kH, kW};
    const hsize_t chunk[3] = {kChunkFrames, kH, kW};
    hid_t space = H5Screate_simple(3, dims, maxDims);
    hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(dcpl, 3, chunk);
    H5Pset_deflate(dcpl, 1);
    hid_t ds = H5Dcreate2(file, kDataset, H5T_NATIVE_UINT8, space, lcpl, dcpl, H5P_DEFAULT);
    H5Pclose(dcpl);
    H5Sclose(space);
    H5Pclose(lcpl);
    return ds;
}

bool setFrames(hid_t ds, hsize_t frames)
{
    const hsize_t dims[3] = {frames, kH, kW};
    return H5Dset_extent(ds, dims) >= 0;
}

// filterMask 0 = deflate applied; 0x1 = deflate skipped (raw chunk).
bool writeChunk(hid_t ds, hsize_t chunkIndex, const void* data, size_t bytes, uint32_t filterMask)
{
    const hsize_t offset[3] = {chunkIndex * kChunkFrames, 0, 0};
    return H5Dwrite_chunk(ds, H5P_DEFAULT, filterMask, offset, bytes, data) >= 0;
}

bool writeCompressed(hid_t ds, hsize_t chunkIndex, const uint8_t* frames)
{
    const auto z = deflateBytes(frames, kChunkBytes);
    return !z.empty() && z.size() < kChunkBytes && writeChunk(ds, chunkIndex, z.data(), z.size(), 0);
}

// Raw chunk holding `frames` real frames, zero-padded to the full chunk
// (HDF5 stores edge and partial chunks at full chunk size).
bool writeRawPadded(hid_t ds, hsize_t chunkIndex, const uint8_t* frames, size_t count)
{
    std::vector<uint8_t> buf(kChunkBytes, 0);
    std::memcpy(buf.data(), frames, count * kFrameBytes);
    return writeChunk(ds, chunkIndex, buf.data(), buf.size(), 0x1);
}

bool readAll(const std::filesystem::path& path, std::vector<uint8_t>& out, hsize_t& frames)
{
    hid_t file = H5Fopen(path.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0)
        return false;
    hid_t ds = H5Dopen2(file, kDataset, H5P_DEFAULT);
    hid_t space = H5Dget_space(ds);
    hsize_t dims[3] = {0, 0, 0};
    H5Sget_simple_extent_dims(space, dims, nullptr);
    frames = dims[0];
    out.assign(static_cast<size_t>(frames) * kFrameBytes, 0);
    const bool ok = H5Dread(ds, H5T_NATIVE_UINT8, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()) >= 0;
    H5Sclose(space);
    H5Dclose(ds);
    H5Fclose(file);
    return ok;
}

void reportLibrary()
{
    unsigned maj = 0, min = 0, rel = 0;
    H5get_libversion(&maj, &min, &rel);
    hbool_t threadsafe = 0;
    H5is_library_threadsafe(&threadsafe);
    unsigned int cfg = 0;
    const bool deflateAvail = H5Zfilter_avail(H5Z_FILTER_DEFLATE) > 0;
    if (deflateAvail)
        H5Zget_filter_info(H5Z_FILTER_DEFLATE, &cfg);
    const bool enc = (cfg & H5Z_FILTER_CONFIG_ENCODE_ENABLED) != 0;
    const bool dec = (cfg & H5Z_FILTER_CONFIG_DECODE_ENABLED) != 0;
    std::printf("HDF5_CAPABILITY version=%u.%u.%u threadsafe=%d deflate_encode=%d "
                "deflate_decode=%d zlib=%s\n",
                maj, min, rel, threadsafe ? 1 : 0, enc ? 1 : 0, dec ? 1 : 0, zlibVersion());
    std::fflush(stdout);
    MIB_REQUIRE(deflateAvail && enc && dec, "deflate filter must encode and decode");
}

// Chunks 0 and 2 compressed, chunk 1 raw, chunk 3 a 3-frame raw edge chunk.
void mixedChunksReadBackIdentical(const mib::test::TempDir& dir)
{
    constexpr size_t kFrames = 3 * kChunkFrames + 3;
    const auto src = makeFrames(kFrames, 7);
    const auto path = dir / "mixed.h5";
    {
        hid_t file = H5Fcreate(path.string().c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        MIB_REQUIRE(file >= 0, "create mixed.h5");
        hid_t ds = createDeflateDataset(file);
        MIB_REQUIRE(ds >= 0, "create deflate dataset");
        MIB_REQUIRE(setFrames(ds, kFrames), "extend");
        MIB_EXPECT(writeCompressed(ds, 0, src.data()), "chunk 0 compressed");
        MIB_EXPECT(writeRawPadded(ds, 1, src.data() + 1 * kChunkBytes, kChunkFrames), "chunk 1 raw");
        MIB_EXPECT(writeCompressed(ds, 2, src.data() + 2 * kChunkBytes), "chunk 2 compressed");
        MIB_EXPECT(writeRawPadded(ds, 3, src.data() + 3 * kChunkBytes, 3), "chunk 3 raw edge");
#if H5_VERSION_GE(1, 10, 5)
        const uint32_t expectMask[4] = {0, 0x1, 0, 0x1};
        for (hsize_t c = 0; c < 4; ++c)
        {
            const hsize_t offset[3] = {c * kChunkFrames, 0, 0};
            unsigned mask = 0xFF;
            haddr_t addr = 0;
            hsize_t size = 0;
            MIB_EXPECT(H5Dget_chunk_info_by_coord(ds, offset, &mask, &addr, &size) >= 0,
                       "chunk info " + std::to_string(c));
            MIB_EXPECT(mask == expectMask[c], "filter mask of chunk " + std::to_string(c));
            if (expectMask[c] == 0)
                MIB_EXPECT(size < kChunkBytes, "compressed chunk smaller than raw");
            else
                MIB_EXPECT(size == kChunkBytes, "raw chunk stored at full size");
        }
#endif
        H5Dclose(ds);
        H5Fclose(file);
    }

    std::vector<uint8_t> back;
    hsize_t frames = 0;
    MIB_REQUIRE(readAll(path, back, frames), "H5Dread mixed dataset");
    MIB_EXPECT(frames == kFrames, "extent");
    MIB_EXPECT(back == src, "H5Dread bytes identical across compressed/raw/edge chunks");

    backend::services::Hdf5Service reader;
    MIB_REQUIRE(reader.loadFile(path.string()), "Hdf5Service::loadFile");
    for (size_t i = 0; i < kFrames; ++i)
    {
        cv::Mat img;
        const bool ok = reader.readImageByIndex(kDataset, i, img);
        MIB_EXPECT(ok && img.rows == static_cast<int>(kH) && img.cols == static_cast<int>(kW) &&
                       img.isContinuous() &&
                       std::memcmp(img.data, src.data() + i * kFrameBytes, kFrameBytes) == 0,
                   "readImageByIndex frame " + std::to_string(i));
    }
    reader.closeFile();
}

// Spike S1: 9 padded raw tail rewrites per chunk, then the compressed write,
// with an H5Fflush every 10 chunks (interval flush). Must stay within 5 % of
// writing each chunk compressed once.
void tailRewritesDoNotBloatFile(const mib::test::TempDir& dir)
{
    constexpr hsize_t kChunks = 40;
    const auto src = makeFrames(kChunks * kChunkFrames, 11);
    auto build = [&](const std::filesystem::path& path, bool tailRewrites) -> bool {
        hid_t file = H5Fcreate(path.string().c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        if (file < 0)
            return false;
        hid_t ds = createDeflateDataset(file);
        bool ok = ds >= 0;
        for (hsize_t c = 0; ok && c < kChunks; ++c)
        {
            const uint8_t* chunk = src.data() + c * kChunkBytes;
            if (tailRewrites)
                for (size_t k = 1; ok && k < kChunkFrames; ++k)
                    ok = setFrames(ds, c * kChunkFrames + k) && writeRawPadded(ds, c, chunk, k);
            ok = ok && setFrames(ds, (c + 1) * kChunkFrames) && writeCompressed(ds, c, chunk);
            if (ok && c % 10 == 9)
                ok = H5Fflush(file, H5F_SCOPE_GLOBAL) >= 0;
        }
        if (ds >= 0)
            H5Dclose(ds);
        return H5Fclose(file) >= 0 && ok;
    };

    const auto direct = dir / "direct.h5";
    const auto tail = dir / "tail.h5";
    MIB_REQUIRE(build(direct, false), "direct build");
    MIB_REQUIRE(build(tail, true), "tail-rewrite build");

    const auto directBytes = std::filesystem::file_size(direct);
    const auto tailBytes = std::filesystem::file_size(tail);
    const double growth = static_cast<double>(tailBytes) / static_cast<double>(directBytes) - 1.0;
    const double ratio = static_cast<double>(src.size()) / static_cast<double>(directBytes);
    std::printf("HDF5_S1 raw_bytes=%zu direct_bytes=%llu tail_bytes=%llu growth_pct=%.2f "
                "ratio=%.2f\n",
                src.size(), static_cast<unsigned long long>(directBytes),
                static_cast<unsigned long long>(tailBytes), growth * 100.0, ratio);
    std::fflush(stdout);
    MIB_EXPECT(ratio > 1.1, "synthetic frames must actually compress");
    MIB_EXPECT(growth < 0.05, "tail rewrites must not grow the file by 5 % or more");

    std::vector<uint8_t> back;
    hsize_t frames = 0;
    MIB_REQUIRE(readAll(tail, back, frames), "read tail-rewrite file");
    MIB_EXPECT(frames == kChunks * kChunkFrames && back == src,
               "tail-rewrite file reads back byte-identical");
}
} // namespace

int main()
{
    mib::test::TempDir dir("mib_hdf5_direct_chunk");
    reportLibrary();
    mixedChunksReadBackIdentical(dir);
    tailRewritesDoNotBloatFile(dir);
    return mib::test::exitCode();
}

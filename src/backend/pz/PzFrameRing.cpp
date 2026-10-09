#include "backend/pz/PzFrameRing.h"

#include "pz_mib_abi.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <thread>

namespace backend::pz {

namespace {

constexpr uint32_t kHeadNone = 0xFFFFFFFFu;  // STORE_HEAD_SEQ before the first frame (-1)

uint64_t alignDown(uint64_t v, uint64_t a) { return v - v % a; }

std::string mib(uint64_t bytes) {
    std::ostringstream o;
    o << bytes / (1024.0 * 1024.0) << " MiB";
    return o.str();
}

} // namespace

const char* ringReadName(RingRead r) {
    switch (r) {
    case RingRead::Ok: return "ok";
    case RingRead::Unavailable: return "unavailable";
    case RingRead::OutOfRange: return "out_of_range";
    case RingRead::Overwritten: return "overwritten";
    case RingRead::Malformed: return "malformed";
    }
    return "?";
}

std::optional<uint64_t> systemRamEnd(const std::string& iomemText) {
    std::istringstream in(iomemText);
    std::string line;
    std::optional<uint64_t> end;
    while (std::getline(in, line)) {
        // Top-level lines only ("00000000-3effffff : System RAM"); nested ranges are indented.
        if (line.empty() || line[0] == ' ') continue;
        if (line.find(": System RAM") == std::string::npos) continue;
        const auto dash = line.find('-');
        const auto space = line.find(' ');
        if (dash == std::string::npos || space == std::string::npos || dash > space) continue;
        try {
            const uint64_t last = std::stoull(line.substr(dash + 1, space - dash - 1), nullptr, 16);
            end = std::max<uint64_t>(end.value_or(0), last + 1);
        } catch (...) {
        }
    }
    return end;
}

RingPlan planRing(uint32_t frames, uint64_t linuxRamEnd, uint32_t recordBytes) {
    RingPlan p;
    const uint64_t lowest = std::max<uint64_t>(alignDown(linuxRamEnd + kRingBaseAlign - 1, kRingBaseAlign), kRingFloor);
    p.maxRecords = lowest < kRingCeiling ? (kRingCeiling - lowest) / recordBytes : 0;
    if (frames == 0) {
        p.why = "the ring size is 0 frames";
        return p;
    }
    if (recordBytes < kRingSetBytes + 1) {
        p.why = "the ring record size is not valid";
        return p;
    }
    if (p.maxRecords == 0) {
        p.why = "Linux owns all the DDR below the PL area (RAM ends at " + mib(linuxRamEnd) +
                "): boot with a smaller mem= (for example mem=432M gives a 576 MiB ring)";
        return p;
    }
    if (frames > p.maxRecords) {
        p.why = "a ring of " + std::to_string(frames) + " frames needs " + mib(static_cast<uint64_t>(frames) * recordBytes) +
                " above Linux's RAM (ends at " + mib(linuxRamEnd) + "), room for " + std::to_string(p.maxRecords) +
                " frames: boot with a smaller mem= or ask for fewer frames";
        return p;
    }
    p.records = frames;
    p.bytes = static_cast<uint64_t>(frames) * recordBytes;
    p.base = alignDown(kRingCeiling - p.bytes, kRingBaseAlign);
    // Defence in depth: the arithmetic above already keeps these true.
    if (p.base < lowest || p.base < kRingFloor || p.base + p.bytes > kRingCeiling) {
        p = RingPlan{};
        p.why = "the ring does not fit between Linux's RAM and the PL area";
        return p;
    }
    p.ok = true;
    return p;
}

bool PzFrameRing::program(const RingPlan& plan, std::string* error) {
    if (!plan.ok) {
        if (error) *error = plan.why.empty() ? "the ring plan is not valid" : plan.why;
        return false;
    }
    io_.setReg(PZ_MIB_REG_STORE_BASE_LO, static_cast<uint32_t>(plan.base));
    io_.setReg(PZ_MIB_REG_STORE_BASE_HI, static_cast<uint32_t>(plan.base >> 32));
    io_.setReg(PZ_MIB_REG_STORE_RECORDS, plan.records);
    io_.setReg(PZ_MIB_REG_STORE_MODE, kStoreModeContinuous | kStoreModeRingOnly);
    const uint64_t base = io_.reg(PZ_MIB_REG_STORE_BASE_LO) | static_cast<uint64_t>(io_.reg(PZ_MIB_REG_STORE_BASE_HI)) << 32;
    if (base != plan.base || io_.reg(PZ_MIB_REG_STORE_RECORDS) != plan.records ||
        (io_.reg(PZ_MIB_REG_STORE_MODE) & (kStoreModeContinuous | kStoreModeRingOnly)) !=
            (kStoreModeContinuous | kStoreModeRingOnly)) {
        if (error) *error = "the PL did not take the ring registers (this image has no frame ring)";
        return false;
    }
    return true;
}

RingStatus PzFrameRing::status() {
    RingStatus s;
    s.records = io_.reg(PZ_MIB_REG_STORE_RECORDS);
    s.recordBytes = io_.reg(PZ_MIB_REG_STORE_RECORD_BYTES);
    s.base = io_.reg(PZ_MIB_REG_STORE_BASE_LO) | static_cast<uint64_t>(io_.reg(PZ_MIB_REG_STORE_BASE_HI)) << 32;
    s.state = io_.reg(PZ_MIB_REG_STORE_STATE);
    const uint32_t headRaw = io_.reg(PZ_MIB_REG_STORE_HEAD_SEQ);
    s.final = io_.reg(kRingRegFinalSeq);
    s.head = headRaw == kHeadNone ? -1 : static_cast<int64_t>(headRaw);
    s.fault = s.state == PZ_MIB_STORE_STATE_FAULT;
    if (s.records == 0) {
        s.why = "no frame ring is configured";
        return s;
    }
    if (s.recordBytes < kRingSetBytes + 1 || s.base < kRingFloor ||
        s.base + static_cast<uint64_t>(s.records) * s.recordBytes > kRingCeiling) {
        s.why = "the ring registers are outside the allowed DDR window";
        return s;
    }
    s.valid = true;
    s.lo = static_cast<uint64_t>(std::max<int64_t>(0, s.head + 1 - static_cast<int64_t>(s.records)));
    s.frozen = !s.fault && s.state == PZ_MIB_STORE_STATE_IDLE && s.final == static_cast<uint64_t>(s.head + 1);
    return s;
}

RingStatus PzFrameRing::awaitFrozen(std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    RingStatus s = status();
    while (s.valid && !s.frozen && !s.fault && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        s = status();
    }
    return s;
}

RingRead PzFrameRing::readFrame(uint64_t seq, RingFrame& out, std::string* why) {
    auto fail = [&](RingRead r, const std::string& message) {
        if (why) *why = message;
        return r;
    };
    const RingStatus st = status();
    if (!st.valid) return fail(RingRead::Unavailable, st.why);
    if (seq < st.lo || seq >= st.final)
        return fail(RingRead::OutOfRange, "frame " + std::to_string(seq) + " is not in the readable range [" +
                                              std::to_string(st.lo) + ", " + std::to_string(st.final) + ")");
    const uint64_t at = st.base + (seq % st.records) * static_cast<uint64_t>(st.recordBytes);

    // 1. The record set (the area is zero-padded after the records).
    std::vector<uint8_t> set(kRingSetBytes);
    if (!io_.read(at, set.data(), set.size())) return fail(RingRead::Unavailable, "the ring memory could not be read");
    std::vector<uint8_t> frameBytes;
    std::optional<FrameRecord> frame;
    std::vector<ResultRecord> results;
    std::optional<ImageRecord> mono, mask;
    size_t offset = 0;
    while (offset + PZ_MIB_RECORD_HEADER_BYTES <= set.size()) {
        uint32_t magic = 0;
        std::memcpy(&magic, set.data() + offset, 4);
        if (magic != PZ_MIB_RECORD_MAGIC) break;
        uint16_t length = 0;
        std::memcpy(&length, set.data() + offset + 6, 2);
        if (length < PZ_MIB_RECORD_HEADER_BYTES || offset + length > set.size()) return fail(RingRead::Malformed, "a record length is invalid");
        const Decoded d = decodeRecord(set.data() + offset, length);
        if (!d.ok() || !d.record) return fail(RingRead::Malformed, std::string("a record did not decode: ") + decodeErrorName(d.error));
        if (const auto* f = std::get_if<FrameRecord>(&d.record->body)) {
            if (frame) return fail(RingRead::Malformed, "two FRAME records");
            frame = *f;
            frameBytes.assign(set.begin() + static_cast<std::ptrdiff_t>(offset), set.begin() + static_cast<std::ptrdiff_t>(offset + length));
        } else if (const auto* r = std::get_if<ResultRecord>(&d.record->body)) {
            results.push_back(*r);
        } else if (const auto* i = std::get_if<ImageRecord>(&d.record->body)) {
            if (i->pixelFormat == PZ_MIB_PIXEL_FORMAT_MONO8) mono = *i;
            else if (i->pixelFormat == PZ_MIB_PIXEL_FORMAT_MASK1) mask = *i;
        }
        offset += length;
    }
    if (!frame || frameBytes.empty()) return fail(RingRead::Malformed, "the record does not start with a FRAME record");
    if (!mono) return fail(RingRead::Malformed, "the record has no MONO8 image");
    const uint32_t w = mono->width ? mono->width : frame->width, h = mono->height ? mono->height : frame->height;
    if (w == 0 || h == 0 || w % 8 != 0 || w > 4096 || h > 4096) return fail(RingRead::Malformed, "the image geometry is invalid");
    const uint64_t pixels = static_cast<uint64_t>(w) * h;
    if (mono->byteLength != pixels || static_cast<uint64_t>(mono->byteOffset) + mono->byteLength > st.recordBytes)
        return fail(RingRead::Malformed, "the MONO8 block is outside the record");
    const bool maskPresent = mask && (mask->flags & PZ_MIB_IMAGE_FLAGS_NO_RESULT) == 0;
    if (maskPresent && (mask->byteLength != pixels / 8 || static_cast<uint64_t>(mask->byteOffset) + mask->byteLength > st.recordBytes))
        return fail(RingRead::Malformed, "the MASK1 block is outside the record");
    for (const auto& r : results)
        if (r.frameId != frame->frameId) return fail(RingRead::Malformed, "a RESULT belongs to another frame");

    // 2. The blocks.
    RingFrame f;
    f.gray.resize(pixels);
    if (!io_.read(at + mono->byteOffset, f.gray.data(), f.gray.size())) return fail(RingRead::Unavailable, "the ring memory could not be read");
    f.mask.assign(pixels / 8, 0);
    if (maskPresent && !io_.read(at + mask->byteOffset, f.mask.data(), f.mask.size()))
        return fail(RingRead::Unavailable, "the ring memory could not be read");

    // 3. The checks. The header again (a cheap second check), then HEAD' against the retained window:
    //    the new frame's pixels are written before its set replaces the header, so only HEAD' proves the copy.
    std::vector<uint8_t> again(frameBytes.size());
    if (!io_.read(at, again.data(), again.size())) return fail(RingRead::Unavailable, "the ring memory could not be read");
    if (again != frameBytes) return fail(RingRead::Overwritten, "frame " + std::to_string(seq) + " was overwritten while it was copied (header changed)");
    const uint32_t headRaw = io_.reg(PZ_MIB_REG_STORE_HEAD_SEQ);
    const int64_t head2 = headRaw == kHeadNone ? -1 : static_cast<int64_t>(headRaw);
    const int64_t lo2 = std::max<int64_t>(0, head2 + 1 - static_cast<int64_t>(st.records));
    if (static_cast<int64_t>(seq) < lo2)
        return fail(RingRead::Overwritten, "frame " + std::to_string(seq) + " was overwritten while it was copied (the ring moved past it)");

    f.seq = seq;
    f.frameId = frame->frameId;
    f.timestampTicks = frame->timestamp;
    f.frameFlags = frame->flags;
    f.width = static_cast<uint16_t>(w);
    f.height = static_cast<uint16_t>(h);
    f.maskPresent = maskPresent;
    f.resultsTruncated = (frame->flags & (kFrameResultsTruncated | kFrameResultsOverflow)) != 0;
    for (const auto& r : results) {
        RingCell c;
        c.x = r.bboxX;
        c.y = r.bboxY;
        c.width = r.bboxW;
        c.height = r.bboxH;
        c.flags = r.flags;
        c.valid = (r.flags & kResultValid) != 0;
        for (size_t k = 0; k < c.payload.size() && k < r.payload.size(); ++k) c.payload[k] = r.payload[k];
        f.cells.push_back(c);
    }
    out = std::move(f);
    return RingRead::Ok;
}

std::vector<uint8_t> buildRingPacket(const RingFrame& f, uint32_t tickHz) {
    constexpr size_t kHeader = 48, kCellWords = 18;
    std::vector<uint8_t> b(kHeader + f.gray.size() + f.mask.size() + f.cells.size() * kCellWords * 4, 0);
    auto put = [&](size_t at, uint64_t v, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    std::memcpy(b.data(), "MIBR", 4);
    put(4, 1, 2);
    put(6, kHeader, 2);
    put(8, f.seq, 8);
    put(16, f.frameId, 8);
    put(24, f.timestampTicks, 8);
    put(32, tickHz, 4);
    put(36, f.frameFlags, 4);
    put(40, f.width, 2);
    put(42, f.height, 2);
    put(44, f.cells.size(), 2);
    put(46, (f.maskPresent ? 1u : 0u) | (f.resultsTruncated ? 2u : 0u), 2);
    std::copy(f.gray.begin(), f.gray.end(), b.begin() + static_cast<std::ptrdiff_t>(kHeader));
    std::copy(f.mask.begin(), f.mask.end(), b.begin() + static_cast<std::ptrdiff_t>(kHeader + f.gray.size()));
    size_t at = kHeader + f.gray.size() + f.mask.size();
    for (const auto& c : f.cells) {
        for (size_t k = 0; k < 15; ++k) put(at + 4 * k, c.payload[k], 4);
        put(at + 4 * 15, static_cast<uint32_t>(c.x) | static_cast<uint32_t>(c.y) << 16, 4);
        put(at + 4 * 16, static_cast<uint32_t>(c.width) | static_cast<uint32_t>(c.height) << 16, 4);
        put(at + 4 * 17, c.valid ? 1u : 0u, 4);
        at += kCellWords * 4;
    }
    return b;
}

} // namespace backend::pz

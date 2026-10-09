#include "backend/pz/PzRecords.h"

#include "pz_mib_abi.h" // vendored bundle: layouts are static_assert-ed there

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace backend::pz {

static_assert(sizeof(pz_mib_record_header) == PZ_MIB_RECORD_HEADER_BYTES);
static_assert(sizeof(pz_mib_frame_record) == PZ_MIB_FRAME_PAYLOAD_BYTES);
static_assert(sizeof(pz_mib_result_record) == PZ_MIB_RESULT_PAYLOAD_BYTES);
static_assert(sizeof(pz_mib_event_record) == PZ_MIB_EVENT_PAYLOAD_BYTES);
static_assert(sizeof(pz_mib_counters_record) == PZ_MIB_COUNTERS_PAYLOAD_BYTES);
static_assert(sizeof(pz_mib_preview_record) == PZ_MIB_PREVIEW_PAYLOAD_BYTES);
static_assert(sizeof(pz_mib_descriptor_record) == PZ_MIB_DESCRIPTOR_PAYLOAD_BYTES);
static_assert(sizeof(pz_mib_image_record) == PZ_MIB_IMAGE_PAYLOAD_BYTES);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "PZ records are little-endian; the decoder copies them as host structs"
#endif

namespace {

template <class T>
T load(const uint8_t* p) {
    static_assert(std::is_trivially_copyable_v<T>);
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

struct TypeSpec {
    uint8_t version;
    size_t payloadBytes;
};

std::optional<TypeSpec> typeSpec(uint8_t type) {
    switch (type) {
    case PZ_MIB_RECORD_TYPE_FRAME: return TypeSpec{PZ_MIB_FRAME_VERSION, PZ_MIB_FRAME_PAYLOAD_BYTES};
    case PZ_MIB_RECORD_TYPE_RESULT: return TypeSpec{PZ_MIB_RESULT_VERSION, PZ_MIB_RESULT_PAYLOAD_BYTES};
    case PZ_MIB_RECORD_TYPE_EVENT: return TypeSpec{PZ_MIB_EVENT_VERSION, PZ_MIB_EVENT_PAYLOAD_BYTES};
    case PZ_MIB_RECORD_TYPE_COUNTERS:
        return TypeSpec{PZ_MIB_COUNTERS_VERSION, PZ_MIB_COUNTERS_PAYLOAD_BYTES};
    case PZ_MIB_RECORD_TYPE_PREVIEW: return TypeSpec{PZ_MIB_PREVIEW_VERSION, PZ_MIB_PREVIEW_PAYLOAD_BYTES};
    case PZ_MIB_RECORD_TYPE_DESCRIPTOR:
        return TypeSpec{PZ_MIB_DESCRIPTOR_VERSION, PZ_MIB_DESCRIPTOR_PAYLOAD_BYTES};
    case PZ_MIB_RECORD_TYPE_IMAGE: return TypeSpec{PZ_MIB_IMAGE_VERSION, PZ_MIB_IMAGE_PAYLOAD_BYTES};
    default: return std::nullopt;
    }
}

const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

Decoded fail(DecodeError e, std::string detail = {}) {
    Decoded d;
    d.error = e;
    d.detail = std::move(detail);
    return d;
}

std::vector<uint8_t> finish(uint8_t type, uint8_t version, std::vector<uint8_t> body,
                            uint32_t sequence) {
    while ((PZ_MIB_RECORD_HEADER_BYTES + body.size()) % PZ_MIB_RECORD_ALIGN_BYTES) body.push_back(0);
    std::vector<uint8_t> raw(PZ_MIB_RECORD_HEADER_BYTES);
    pz_mib_record_header h{};
    h.magic = PZ_MIB_RECORD_MAGIC;
    h.type = type;
    h.version = version;
    h.length = static_cast<uint16_t>(PZ_MIB_RECORD_HEADER_BYTES + body.size());
    h.sequence = sequence;
    h.crc32 = 0;
    std::memcpy(raw.data(), &h, sizeof h);
    raw.insert(raw.end(), body.begin(), body.end());
    const uint32_t crc = recordCrc32(raw.data(), raw.size());
    std::memcpy(raw.data() + 12, &crc, 4);
    return raw;
}

} // namespace

const char* decodeErrorName(DecodeError error) {
    switch (error) {
    case DecodeError::Ok: return "OK";
    case DecodeError::BadMagic: return "BAD_MAGIC";
    case DecodeError::BadLength: return "BAD_LENGTH";
    case DecodeError::BadCrc: return "BAD_CRC";
    case DecodeError::UnknownType: return "UNKNOWN_TYPE";
    case DecodeError::BadVersion: return "BAD_VERSION";
    case DecodeError::Truncated: return "TRUNCATED";
    case DecodeError::Oversized: return "OVERSIZED";
    case DecodeError::DuplicateSequence: return "DUPLICATE_SEQUENCE";
    case DecodeError::SequenceGap: return "SEQUENCE_GAP";
    case DecodeError::StaleEpoch: return "STALE_EPOCH";
    case DecodeError::StaleGeneration: return "STALE_GENERATION";
    case DecodeError::ResultLimit: return "RESULT_LIMIT";
    case DecodeError::BadState: return "BAD_STATE";
    }
    return "UNKNOWN";
}

std::optional<uint32_t> Record::epoch() const {
    if (const auto* f = std::get_if<FrameRecord>(&body)) return f->epoch;
    if (const auto* e = std::get_if<EventRecord>(&body)) return e->epoch;
    if (const auto* p = std::get_if<PreviewRecord>(&body)) return p->epoch;
    if (const auto* d = std::get_if<DescriptorRecord>(&body)) return d->epoch;
    if (const auto* i = std::get_if<ImageRecord>(&body)) return i->epoch;
    return std::nullopt;
}

uint32_t recordCrc32(const uint8_t* data, size_t length) {
    const auto& table = crcTable();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t b = (i >= 12 && i < 16) ? 0 : data[i];
        c = table[(c ^ b) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

Decoded decodeRecord(const uint8_t* data, size_t size, std::optional<uint16_t> resultLimit) {
    constexpr size_t hb = PZ_MIB_RECORD_HEADER_BYTES;
    if (size < hb) return fail(DecodeError::Truncated, "shorter than the header");
    const auto h = load<pz_mib_record_header>(data);
    if (h.magic != PZ_MIB_RECORD_MAGIC) return fail(DecodeError::BadMagic);
    if (h.length > PZ_MIB_MAX_RECORD_BYTES) return fail(DecodeError::Oversized, "length");
    if (h.length < hb || h.length % PZ_MIB_RECORD_ALIGN_BYTES) return fail(DecodeError::BadLength);
    if (size < h.length) return fail(DecodeError::Truncated, "shorter than length");
    if (recordCrc32(data, h.length) != h.crc32) return fail(DecodeError::BadCrc);
    const auto spec = typeSpec(h.type);
    if (!spec) return fail(DecodeError::UnknownType, std::to_string(h.type));
    if (h.version > spec->version) return fail(DecodeError::BadVersion);
    if (h.length < hb + spec->payloadBytes) return fail(DecodeError::BadLength, "below the type minimum");

    const uint8_t* p = data + hb;
    Record rec;
    rec.type = static_cast<RecordType>(h.type);
    rec.version = h.version;
    rec.length = h.length;
    rec.sequence = h.sequence;
    switch (h.type) {
    case PZ_MIB_RECORD_TYPE_FRAME: {
        const auto r = load<pz_mib_frame_record>(p);
        const uint32_t limit =
            resultLimit ? *resultLimit : (r.result_limit ? r.result_limit : PZ_MIB_MAX_RESULTS_PER_FRAME);
        if (r.result_count > limit) return fail(DecodeError::ResultLimit);
        rec.body = FrameRecord{r.run_id, r.frame_id, r.timestamp, r.epoch, r.flags, r.result_count,
                               r.result_limit, r.science_profile, r.profile_version, r.width, r.height,
                               r.pixel_format};
        break;
    }
    case PZ_MIB_RECORD_TYPE_RESULT: {
        const auto r = load<pz_mib_result_record>(p);
        if (r.payload_words > PZ_MIB_MAX_RESULT_PAYLOAD_WORDS) return fail(DecodeError::Oversized, "payload words");
        if (h.length < hb + PZ_MIB_RESULT_PAYLOAD_BYTES + 4u * r.payload_words) {
            return fail(DecodeError::BadLength, "payload exceeds length");
        }
        ResultRecord out{r.frame_id, r.result_index, r.flags, r.science_profile, r.profile_version,
                         r.bbox_x, r.bbox_y, r.bbox_w, r.bbox_h, r.payload_validity, {}};
        out.payload.resize(r.payload_words);
        if (r.payload_words != 0) {
            std::memcpy(out.payload.data(), p + PZ_MIB_RESULT_PAYLOAD_BYTES, 4u * r.payload_words);
        }
        rec.body = std::move(out);
        break;
    }
    case PZ_MIB_RECORD_TYPE_EVENT: {
        const auto r = load<pz_mib_event_record>(p);
        if (r.decision > PZ_MIB_EVENT_DECISION_INHIBITED) return fail(DecodeError::BadState, "decision");
        EventRecord e{r.frame_id, r.decision_timestamp, std::nullopt, r.result_index, r.decision,
                      r.reason, r.latency_ticks, r.epoch};
        if (r.decision == PZ_MIB_EVENT_DECISION_ISSUED) e.outputTimestamp = r.output_timestamp;
        rec.body = e;
        break;
    }
    case PZ_MIB_RECORD_TYPE_COUNTERS: {
        const auto r = load<pz_mib_counters_record>(p);
        CountersRecord c{r.snapshot_seq, r.generation, r.timestamp, {}};
        std::copy(std::begin(r.counters), std::end(r.counters), c.counters.begin());
        rec.body = c;
        break;
    }
    case PZ_MIB_RECORD_TYPE_PREVIEW: {
        const auto r = load<pz_mib_preview_record>(p);
        rec.body = PreviewRecord{r.frame_id, r.timestamp, r.buffer_index, r.pixel_format, r.width,
                                 r.height, r.stride, r.byte_length, r.epoch};
        break;
    }
    case PZ_MIB_RECORD_TYPE_DESCRIPTOR: {
        const auto r = load<pz_mib_descriptor_record>(p);
        if (r.state > PZ_MIB_BUFFER_STATE_CPU_OWNED ||
            (r.kind != PZ_MIB_BUFFER_KIND_RESULT && r.kind != PZ_MIB_BUFFER_KIND_PREVIEW)) {
            return fail(DecodeError::BadState, "descriptor state/kind");
        }
        rec.body = DescriptorRecord{r.buffer_index, r.kind, r.state, r.flags, r.generation,
                                    r.byte_length, r.epoch};
        break;
    }
    case PZ_MIB_RECORD_TYPE_IMAGE: {
        const auto r = load<pz_mib_image_record>(p);
        rec.body = ImageRecord{r.frame_id, r.timestamp, r.byte_offset, r.byte_length, r.pixel_format,
                               r.flags, r.width, r.height, r.stride, r.epoch};
        break;
    }
    default: return fail(DecodeError::UnknownType);
    }
    Decoded d;
    d.record = std::move(rec);
    return d;
}

std::vector<uint8_t> encodeFrameRecord(const FrameRecord& f, uint32_t sequence) {
    pz_mib_frame_record r{};
    r.run_id = f.runId;
    r.frame_id = f.frameId;
    r.timestamp = f.timestamp;
    r.epoch = f.epoch;
    r.flags = f.flags;
    r.result_count = f.resultCount;
    r.result_limit = f.resultLimit;
    r.science_profile = f.scienceProfile;
    r.profile_version = f.profileVersion;
    r.width = f.width;
    r.height = f.height;
    r.pixel_format = f.pixelFormat;
    std::vector<uint8_t> body(sizeof r);
    std::memcpy(body.data(), &r, sizeof r);
    return finish(PZ_MIB_RECORD_TYPE_FRAME, PZ_MIB_FRAME_VERSION, std::move(body), sequence);
}

std::vector<uint8_t> encodeResultRecord(const ResultRecord& res, uint32_t sequence) {
    pz_mib_result_record r{};
    r.frame_id = res.frameId;
    r.result_index = res.resultIndex;
    r.flags = res.flags;
    r.science_profile = res.scienceProfile;
    r.profile_version = res.profileVersion;
    r.bbox_x = res.bboxX;
    r.bbox_y = res.bboxY;
    r.bbox_w = res.bboxW;
    r.bbox_h = res.bboxH;
    r.payload_validity = res.payloadValidity;
    r.payload_words = static_cast<uint16_t>(res.payload.size());
    std::vector<uint8_t> body(sizeof r + 4 * res.payload.size());
    std::memcpy(body.data(), &r, sizeof r);
    if (!res.payload.empty()) {
        std::memcpy(body.data() + sizeof r, res.payload.data(), 4 * res.payload.size());
    }
    return finish(PZ_MIB_RECORD_TYPE_RESULT, PZ_MIB_RESULT_VERSION, std::move(body), sequence);
}

std::vector<uint8_t> encodeImageRecord(const ImageRecord& img, uint32_t sequence) {
    pz_mib_image_record r{};
    r.frame_id = img.frameId;
    r.timestamp = img.timestamp;
    r.byte_offset = img.byteOffset;
    r.byte_length = img.byteLength;
    r.pixel_format = img.pixelFormat;
    r.flags = img.flags;
    r.width = img.width;
    r.height = img.height;
    r.stride = img.stride;
    r.epoch = img.epoch;
    std::vector<uint8_t> body(sizeof r);
    std::memcpy(body.data(), &r, sizeof r);
    return finish(PZ_MIB_RECORD_TYPE_IMAGE, PZ_MIB_IMAGE_VERSION, std::move(body), sequence);
}

std::vector<std::pair<size_t, size_t>> splitRecords(const uint8_t* data, size_t size) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t pos = 0;
    while (pos + PZ_MIB_RECORD_HEADER_BYTES <= size) {
        const auto length = load<uint16_t>(data + pos + 6);
        if (length < PZ_MIB_RECORD_HEADER_BYTES) break;
        out.emplace_back(pos, std::min<size_t>(length, size - pos));
        pos += length;
    }
    return out;
}

Decoded StreamDecoder::feed(const uint8_t* data, size_t size) {
    Decoded d = decodeRecord(data, size);
    if (!d.ok()) return d;
    const uint32_t seq = d.record->sequence;
    if (lastSequence_) {
        if (seq == *lastSequence_) {
            return fail(DecodeError::DuplicateSequence, std::to_string(seq));
        }
        const uint32_t expected = *lastSequence_ + 1u;
        if (seq != expected) {
            ++gaps_;
            lastSequence_ = seq;
            d.error = DecodeError::SequenceGap;
            d.detail = "expected " + std::to_string(expected) + " got " + std::to_string(seq);
            return d; // the record is still delivered
        }
    }
    lastSequence_ = seq;
    if (const auto e = d.record->epoch(); e && *e != epoch_) {
        if (*e < epoch_) {
            return fail(DecodeError::StaleEpoch, std::to_string(*e) + " < " + std::to_string(epoch_));
        }
        const auto* f = std::get_if<FrameRecord>(&d.record->body);
        if (f && (f->flags & kFrameFirstOfEpoch)) epoch_ = *e;
    }
    if (const auto* desc = std::get_if<DescriptorRecord>(&d.record->body);
        desc && desc->generation != (generation_ & 0xFFFFu)) {
        return fail(DecodeError::StaleGeneration);
    }
    return d;
}

ResultRingReader::Drain ResultRingReader::drain(std::vector<uint8_t>& out) {
    Drain drain;
    const size_t size = memory_.sizeBytes();
    const uint32_t head = memory_.head();
    const uint32_t tail = memory_.tail();
    const uint32_t fill = head - tail; // free-running counters
    if (fill == 0) return drain;
    if (fill > size) {
        drain.overrun = true;
        memory_.setTail(head);
        return drain;
    }
    const size_t start = out.size();
    out.resize(start + fill);
    const size_t offset = tail % size;
    const size_t first = std::min<size_t>(fill, size - offset);
    memory_.read(offset, out.data() + start, first);
    if (first < fill) memory_.read(0, out.data() + start + first, fill - first);
    memory_.setTail(head);
    drain.bytes = fill;
    return drain;
}

std::vector<FrameResults> FrameAssembler::add(const Record& record) {
    std::vector<FrameResults> done;
    if (const auto* f = std::get_if<FrameRecord>(&record.body)) {
        if (open_) {
            open_->incomplete = open_->results.size() < open_->frame.resultCount;
            done.push_back(std::move(*open_));
            open_.reset();
        }
        FrameResults fr;
        fr.frame = *f;
        if (f->resultCount == 0) {
            done.push_back(std::move(fr));
        } else {
            open_ = std::move(fr);
        }
        return done;
    }
    if (const auto* r = std::get_if<ResultRecord>(&record.body)) {
        if (!open_ || open_->frame.frameId != r->frameId) {
            ++orphanResults_;
            return done;
        }
        open_->results.push_back(*r);
        if (open_->results.size() >= open_->frame.resultCount) {
            done.push_back(std::move(*open_));
            open_.reset();
        }
    }
    return done;
}

std::optional<FrameResults> FrameAssembler::flush() {
    if (!open_) return std::nullopt;
    open_->incomplete = open_->results.size() < open_->frame.resultCount;
    auto out = std::move(open_);
    open_.reset();
    return out;
}

std::optional<UnetCell> decodeUnetCellsV2(const ResultRecord& r) {
    if (r.scienceProfile != kScienceProfileUnetCells || r.profileVersion != kUnetCellsProfileVersion ||
        r.payload.size() < 15) {
        return std::nullopt;
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto& w = r.payload;
    auto valid = [&](int i) { return (r.payloadValidity >> i & 1u) != 0; };
    auto q16 = [&](int i) { return valid(i) ? w[i] / 65536.0 : nan; };
    auto sq16 = [&](int i) { return valid(i) ? static_cast<int32_t>(w[i]) / 65536.0 : nan; };
    auto q8 = [&](int i) { return valid(i) ? w[i] / 256.0 : nan; };

    UnetCell c;
    c.objectId = static_cast<int>(w[0] & 0xFFFFu);
    c.reason = static_cast<UnetCellReason>(w[0] >> 16 & 15u);
    c.cutOff = (w[0] >> 20 & 1u) != 0;
    c.emodulusOutOfCoverage = (w[0] >> 23 & 1u) != 0;
    c.target = (w[0] >> 24 & 1u) != 0;
    c.bboxX = r.bboxX;
    c.bboxY = r.bboxY;
    c.bboxWidth = r.bboxW;
    c.bboxHeight = r.bboxH;
    c.contourArea = valid(1) ? w[1] / 65536.0 : 0.0;
    c.hullArea = valid(2) ? w[2] / 65536.0 : 0.0;
    c.hullPerimeter = q16(3);
    c.areaRatio = q16(4);
    if (valid(5)) {
        c.deformability = (w[5] & 0xFFFFu) / 65536.0;
        c.hullCount = static_cast<int>(w[5] >> 16 & 0xFFu);
    }
    c.cellCount = static_cast<int>(w[5] >> 24); // present for every cell
    c.brightnessMean = q16(6);
    c.centroidX = sq16(7);
    c.centroidY = sq16(8);
    c.areaUm2 = q16(9);
    c.youngsModulusKpa = q16(10);
    c.laplacianVariance = q8(11);
    c.pixelCount = static_cast<int>(w[13] & 0xFFFFu);
    c.blemishCount = static_cast<int>(w[13] >> 16);
    c.brightnessVariance = q8(14);
    return c;
}

} // namespace backend::pz

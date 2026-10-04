#pragma once

// PZ7035 PS-PL result records: decoding, stream checks, the result ring and
// the U-Net cell profile (#447 E3).
//
// The wire format is the pz7035-imx426 ABI bundle vendored in
// third_party/pz7035-abi (pinned by PROVENANCE.json; refresh with
// scripts/vendor_pz7035_abi.py). Records are 16-byte-header, little-endian,
// 8-byte aligned, CRC-32 checked; RESULT payload words are opaque to the
// platform and interpreted only by the decoder of their (science_profile,
// profile_version). The decode rules and error precedence follow the bundle's
// reference decoder (src/imx426/mib_abi.py) and are checked against its
// fixtures (processing.pz_records).
//
// Qt-free; part of mib_processing.

#include "backend/processing/ProcessingTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace backend::pz {

// pz_mib_decode_error, same numbering.
enum class DecodeError : uint8_t {
    Ok = 0,
    BadMagic = 1,
    BadLength = 2,
    BadCrc = 3,
    UnknownType = 4,
    BadVersion = 5,
    Truncated = 6,
    Oversized = 7,
    DuplicateSequence = 8,
    SequenceGap = 9,
    StaleEpoch = 10,
    StaleGeneration = 11,
    ResultLimit = 12,
    BadState = 13,
};
const char* decodeErrorName(DecodeError error);

enum class RecordType : uint8_t {
    Frame = 1,
    Result = 2,
    Event = 3,
    Counters = 4,
    Preview = 5,
    Descriptor = 6,
    Image = 7,
};

// FRAME.flags bits.
inline constexpr uint32_t kFrameEmpty = 1u << 0;
inline constexpr uint32_t kFramePartial = 1u << 1;
inline constexpr uint32_t kFrameInvalid = 1u << 2;
inline constexpr uint32_t kFrameResultsTruncated = 1u << 3;
inline constexpr uint32_t kFrameResultsOverflow = 1u << 4;
inline constexpr uint32_t kFrameFirstOfEpoch = 1u << 7;
// RESULT.flags bits.
inline constexpr uint16_t kResultTarget = 1u << 0;
inline constexpr uint16_t kResultTruncated = 1u << 1;
inline constexpr uint16_t kResultInvalid = 1u << 2;
inline constexpr uint16_t kResultValid = 1u << 3;
inline constexpr uint16_t kResultUnserved = 1u << 4;

struct FrameRecord {
    uint64_t runId{0};
    uint64_t frameId{0};
    uint64_t timestamp{0}; // ticks at SOF
    uint32_t epoch{0};
    uint32_t flags{0};
    uint16_t resultCount{0};
    uint16_t resultLimit{0};
    uint16_t scienceProfile{0};
    uint16_t profileVersion{0};
    uint16_t width{0};
    uint16_t height{0};
    uint8_t pixelFormat{0};
};

struct ResultRecord {
    uint64_t frameId{0};
    uint16_t resultIndex{0};
    uint16_t flags{0};
    uint16_t scienceProfile{0};
    uint16_t profileVersion{0};
    uint16_t bboxX{0};
    uint16_t bboxY{0};
    uint16_t bboxW{0};
    uint16_t bboxH{0};
    uint32_t payloadValidity{0};
    std::vector<uint32_t> payload; // profile-defined words
    bool target() const { return (flags & kResultTarget) != 0; }
};

struct EventRecord {
    uint64_t frameId{0};
    uint64_t decisionTimestamp{0};
    std::optional<uint64_t> outputTimestamp; // only for an ISSUED decision
    uint16_t resultIndex{0};
    uint8_t decision{0}; // 0 NONE, 1 ISSUED, 2 LATE, 3 MISSED, 4 INHIBITED
    uint8_t reason{0};
    uint32_t latencyTicks{0};
    uint32_t epoch{0};
};

struct CountersRecord {
    uint32_t snapshotSeq{0};
    uint32_t generation{0};
    uint64_t timestamp{0};
    std::array<uint32_t, 16> counters{}; // PZ_MIB_COUNTER_* order
};

struct PreviewRecord {
    uint64_t frameId{0};
    uint64_t timestamp{0};
    uint16_t bufferIndex{0};
    uint8_t pixelFormat{0};
    uint16_t width{0};
    uint16_t height{0};
    uint32_t stride{0};
    uint32_t byteLength{0};
    uint32_t epoch{0};
};

struct DescriptorRecord {
    uint16_t bufferIndex{0};
    uint8_t kind{0};  // 1 RESULT, 2 PREVIEW
    uint8_t state{0}; // 0 FREE, 1 DEVICE_OWNED, 2 READY, 3 CPU_OWNED
    uint16_t flags{0};
    uint16_t generation{0};
    uint32_t byteLength{0};
    uint32_t epoch{0};
};

struct ImageRecord {
    uint64_t frameId{0};
    uint64_t timestamp{0};
    uint32_t byteOffset{0};
    uint32_t byteLength{0};
    uint8_t pixelFormat{0};
    uint8_t flags{0};
    uint16_t width{0};
    uint16_t height{0};
    uint16_t stride{0};
    uint32_t epoch{0};
};

struct Record {
    RecordType type{RecordType::Frame};
    uint8_t version{0};
    uint16_t length{0};
    uint32_t sequence{0};
    std::variant<FrameRecord, ResultRecord, EventRecord, CountersRecord, PreviewRecord,
                 DescriptorRecord, ImageRecord>
        body;
    // Epoch carried by FRAME / EVENT / PREVIEW / DESCRIPTOR / IMAGE records.
    std::optional<uint32_t> epoch() const;
};

struct Decoded {
    DecodeError error{DecodeError::Ok};
    std::string detail;
    std::optional<Record> record; // set for Ok, and for SequenceGap (decoding continues)
    bool ok() const { return error == DecodeError::Ok; }
};

// IEEE CRC-32 over `length` bytes with the header's crc field (bytes 12..15)
// taken as zero.
uint32_t recordCrc32(const uint8_t* data, size_t length);

// Decode one record. `resultLimit` overrides FRAME.result_limit (0 there means
// MAX_RESULTS_PER_FRAME).
Decoded decodeRecord(const uint8_t* data, size_t size,
                     std::optional<uint16_t> resultLimit = std::nullopt);

// Encode FRAME / RESULT records exactly as the PL writer does (replay and
// tests): 8-byte aligned, sequence and CRC filled in.
std::vector<uint8_t> encodeFrameRecord(const FrameRecord& frame, uint32_t sequence);
std::vector<uint8_t> encodeResultRecord(const ResultRecord& result, uint32_t sequence);

// Split a drained ring buffer into records by their length fields: (offset,
// length) pairs. Stops at a length below the header size.
std::vector<std::pair<size_t, size_t>> splitRecords(const uint8_t* data, size_t size);

// Sequence, epoch and generation checks for one ring. A duplicate sequence is
// rejected; a gap is counted and reported, and the record is still returned.
class StreamDecoder {
public:
    StreamDecoder(uint32_t epoch, uint32_t generation) : epoch_(epoch), generation_(generation) {}
    Decoded feed(const uint8_t* data, size_t size);
    void setLastSequence(uint32_t sequence) { lastSequence_ = sequence; }
    uint64_t gaps() const { return gaps_; }
    uint32_t epoch() const { return epoch_; }

private:
    uint32_t epoch_;
    uint32_t generation_;
    std::optional<uint32_t> lastSequence_;
    uint64_t gaps_{0};
};

// The PS DDR result ring (RESULT_RING_* registers). HEAD (device) and TAIL
// (CPU) are free-running byte counters; offset = counter % size. The device
// admits a frame's record set whole, so [tail, head) always holds whole
// records. The memory interface keeps the reader testable without the board.
class RingMemory {
public:
    virtual ~RingMemory() = default;
    virtual size_t sizeBytes() const = 0;
    virtual uint32_t head() = 0;
    virtual uint32_t tail() = 0;
    virtual void setTail(uint32_t tail) = 0;
    // Copy n bytes at ring offset `offset` (offset + n <= sizeBytes()).
    virtual void read(size_t offset, uint8_t* dst, size_t n) = 0;
};

class ResultRingReader {
public:
    explicit ResultRingReader(RingMemory& memory) : memory_(memory) {}
    struct Drain {
        size_t bytes{0};
        // head - tail exceeded the ring: the consumer fell behind and the
        // device overwrote unread data. The reader resynchronises at head.
        bool overrun{false};
    };
    // Append [tail, head) to `out` (handling the wrap) and release it.
    Drain drain(std::vector<uint8_t>& out);

private:
    RingMemory& memory_;
};

// Groups each FRAME with its RESULT records (they follow it in the ring).
// A frame is complete when result_count RESULTs arrived, or when the next
// FRAME starts (then `incomplete` is set).
struct FrameResults {
    FrameRecord frame;
    std::vector<ResultRecord> results;
    bool incomplete{false};
};
class FrameAssembler {
public:
    // Returns the frames completed by this record (zero, one or two).
    std::vector<FrameResults> add(const Record& record);
    std::optional<FrameResults> flush();
    uint64_t orphanResults() const { return orphanResults_; }

private:
    std::optional<FrameResults> open_;
    uint64_t orphanResults_{0};
};

// ---- U-Net cell profile (science_profile 2, profile_version 2) ----------

inline constexpr uint16_t kScienceProfileUnetCells = 2;
inline constexpr uint16_t kUnetCellsProfileVersion = 2;

struct UnetCell {
    services::FilterResult result; // Contract-3 fields, coordinates in ROI 1
    int reason{0};                 // profile reason code (0 NONE ... 8 CHANNEL)
    bool emodulusOutOfCoverage{false};
    double areaUm2{std::numeric_limits<double>::quiet_NaN()};
    double hullPerimeter{std::numeric_limits<double>::quiet_NaN()};
    int hullCount{0};
};

// Decode a RESULT of profile unet_cells_v2. nullopt for any other profile or
// version (the envelope stays usable; the payload is unavailable, never zero)
// or a payload shorter than 15 words. Words the validity mask leaves out
// decode as NaN / 0.
std::optional<UnetCell> decodeUnetCellsV2(const ResultRecord& result);

} // namespace backend::pz

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <time.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <pthread.h>
#include <sched.h>
#include <xrt/xrt.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

using Json = nlohmann::json;

constexpr uint64_t kControlBase = 0xA0030000ULL;
constexpr size_t kControlSpan = 4 * 1024;
constexpr int kWidth = 512;
constexpr int kHeight = 96;
constexpr size_t kFrameBytes = static_cast<size_t>(kWidth) * kHeight;
constexpr int kWarmupFrames = 300;
constexpr int kMeasuredFrames = 5000;
constexpr double kPixelToMicron = 0.4886;

constexpr size_t kApCtrl = 0x00 / sizeof(uint32_t);
constexpr size_t kInputLow = 0x10 / sizeof(uint32_t);
constexpr size_t kInputHigh = 0x14 / sizeof(uint32_t);
constexpr size_t kBackgroundLow = 0x1c / sizeof(uint32_t);
constexpr size_t kBackgroundHigh = 0x20 / sizeof(uint32_t);
constexpr size_t kOutputLow = 0x28 / sizeof(uint32_t);
constexpr size_t kOutputHigh = 0x2c / sizeof(uint32_t);
constexpr size_t kThreshold = 0x34 / sizeof(uint32_t);
constexpr size_t kUseBackground = 0x3c / sizeof(uint32_t);

struct Stats {
    double meanUs{0.0};
    double p50Us{0.0};
    double p95Us{0.0};
    double p99Us{0.0};
    double maxUs{0.0};
    double fps{0.0};
    uint64_t digest{0};
};

struct StageDurations {
    double contourExtractionUs{0.0};
    double hierarchyPairingUs{0.0};
    double metricsFilteringUs{0.0};
    double trackingUs{0.0};
};

struct Brightness {
    double q1{0.0};
    double q2{0.0};
    double q3{0.0};
    double q4{0.0};
};

struct Validation {
    bool isValid{false};
    bool isTargetGroup{false};
    bool touchesBorder{false};
    bool hasSingleInnerContour{false};
    bool inRange{false};
    int innerContourCount{0};
    int objectId{-1};
    int objectCount{0};
    int trackId{-1};
    uint64_t trackFirstFrame{0};
    uint64_t trackLastFrame{0};
    int trackObservationCount{0};
    double bboxX{0.0};
    double bboxY{0.0};
    double bboxWidth{0.0};
    double bboxHeight{0.0};
    double centroidX{0.0};
    double centroidY{0.0};
    double area{0.0};
    double deformability{0.0};
    double areaRatio{0.0};
    double ringRatio{0.0};
    double youngsModulus{0.0};
    Brightness brightness;
};

struct Record {
    uint64_t frameIndex{0};
    Validation validation;
};

struct Analysis {
    std::vector<std::vector<cv::Point>> allContours;
    std::vector<cv::Vec4i> hierarchy;
    std::vector<std::vector<cv::Point>> filteredContours;
    std::vector<size_t> originalIndices;
    std::vector<std::vector<cv::Point>> innerContours;
    std::vector<int> innerFilteredIndices;
    std::vector<int> parentIndices;
};

struct Track {
    int id{-1};
    uint64_t firstFrame{0};
    uint64_t lastFrame{0};
    int observations{0};
    cv::Rect2d lastBbox;
    cv::Point2d lastCentroid;
    size_t outputIndex{0};
};

class Mapping {
public:
    Mapping(int fd, uint64_t physicalAddress, size_t span)
        : span_(span)
    {
        address_ = mmap(nullptr, span, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        static_cast<off_t>(physicalAddress));
        if (address_ == MAP_FAILED) {
            throw std::runtime_error("mmap failed at 0x" + std::to_string(physicalAddress)
                                     + ": " + std::strerror(errno));
        }
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping()
    {
        if (address_ != MAP_FAILED) {
            munmap(address_, span_);
        }
    }
    volatile uint32_t* words() const { return static_cast<volatile uint32_t*>(address_); }

private:
    void* address_{MAP_FAILED};
    size_t span_{0};
};

class XrtDevice {
public:
    XrtDevice()
        : handle_(xclOpen(0, nullptr, XCL_QUIET))
    {
        if (handle_ == nullptr) {
            throw std::runtime_error("xclOpen(0) failed");
        }
    }
    XrtDevice(const XrtDevice&) = delete;
    XrtDevice& operator=(const XrtDevice&) = delete;
    ~XrtDevice() { xclClose(handle_); }
    xclDeviceHandle get() const { return handle_; }

private:
    xclDeviceHandle handle_{nullptr};
};

class XrtBuffer {
public:
    XrtBuffer(xclDeviceHandle device, size_t size)
        : device_(device)
        , size_(size)
        , handle_(xclAllocBO(device, size, 0, XCL_BO_FLAGS_CACHEABLE))
    {
        if (handle_ == XRT_NULL_BO) {
            throw std::runtime_error("xclAllocBO failed");
        }
        mapped_ = static_cast<uint8_t*>(xclMapBO(device_, handle_, true));
        if (mapped_ == nullptr || mapped_ == MAP_FAILED) {
            throw std::runtime_error("xclMapBO failed");
        }
        if (xclGetBOProperties(device_, handle_, &properties_) != 0) {
            throw std::runtime_error("xclGetBOProperties failed");
        }
        if (properties_.paddr + size_ > 0x80000000ULL) {
            throw std::runtime_error("XRT BO is outside the HP0 low-DDR aperture");
        }
    }
    XrtBuffer(const XrtBuffer&) = delete;
    XrtBuffer& operator=(const XrtBuffer&) = delete;
    ~XrtBuffer()
    {
        if (mapped_ != nullptr && mapped_ != MAP_FAILED) {
            xclUnmapBO(device_, handle_, mapped_);
        }
        if (handle_ != XRT_NULL_BO) {
            xclFreeBO(device_, handle_);
        }
    }

    uint8_t* data() const { return mapped_; }
    uint64_t address() const { return properties_.paddr; }
    void sync(enum xclBOSyncDirection direction)
    {
        const int error = xclSyncBO(device_, handle_, direction, size_, 0);
        if (error != 0) {
            throw std::runtime_error("xclSyncBO failed: " + std::to_string(error));
        }
    }

private:
    xclDeviceHandle device_{nullptr};
    size_t size_{0};
    xclBufferHandle handle_{XRT_NULL_BO};
    uint8_t* mapped_{nullptr};
    xclBOProperties properties_{};
};

uint64_t monotonicNanoseconds()
{
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value) != 0) {
        throw std::runtime_error("clock_gettime failed");
    }
    return static_cast<uint64_t>(value.tv_sec) * 1'000'000'000ULL
        + static_cast<uint64_t>(value.tv_nsec);
}

void pinCurrentThread(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    const int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (error != 0) {
        throw std::runtime_error("pthread_setaffinity_np failed: "
                                 + std::to_string(error));
    }
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("failed to open " + path);
    }
    const std::streamsize size = input.tellg();
    input.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (size > 0 && !input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("failed to read " + path);
    }
    return bytes;
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary);
    if (!output || !output.write(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("failed to write " + path);
    }
}

void requireExactMask(const uint8_t* actual, const uint8_t* expected,
                      const char* phase, size_t iteration)
{
    if (std::memcmp(actual, expected, kFrameBytes) == 0) {
        return;
    }
    size_t mismatches = 0;
    for (size_t index = 0; index < kFrameBytes; ++index) {
        mismatches += actual[index] != expected[index];
    }
    throw std::runtime_error(
        std::string(phase) + " mask mismatch at iteration "
        + std::to_string(iteration) + ": " + std::to_string(mismatches)
        + " differing pixels");
}

void setAddress(volatile uint32_t* control, size_t lowRegister,
                size_t highRegister, uint64_t address)
{
    control[lowRegister] = static_cast<uint32_t>(address);
    control[highRegister] = static_cast<uint32_t>(address >> 32U);
}

void configureAccelerator(volatile uint32_t* control, uint64_t inputAddress,
                          uint64_t backgroundAddress, uint64_t outputAddress)
{
    setAddress(control, kInputLow, kInputHigh, inputAddress);
    setAddress(control, kBackgroundLow, kBackgroundHigh, backgroundAddress);
    setAddress(control, kOutputLow, kOutputHigh, outputAddress);
    control[kThreshold] = 8;
    control[kUseBackground] = 1;
    __sync_synchronize();

    if (control[kInputLow] != static_cast<uint32_t>(inputAddress)
        || control[kInputHigh] != static_cast<uint32_t>(inputAddress >> 32U)
        || control[kBackgroundLow] != static_cast<uint32_t>(backgroundAddress)
        || control[kBackgroundHigh] != static_cast<uint32_t>(backgroundAddress >> 32U)
        || control[kOutputLow] != static_cast<uint32_t>(outputAddress)
        || control[kOutputHigh] != static_cast<uint32_t>(outputAddress >> 32U)
        || control[kThreshold] != 8 || control[kUseBackground] != 1) {
        throw std::runtime_error("accelerator register readback mismatch");
    }
}

void selectFrameBuffers(volatile uint32_t* control, uint64_t inputAddress,
                        uint64_t outputAddress)
{
    setAddress(control, kInputLow, kInputHigh, inputAddress);
    setAddress(control, kOutputLow, kOutputHigh, outputAddress);
    __sync_synchronize();
}

void launchAndWait(volatile uint32_t* control)
{
    (void)control[kApCtrl];
    control[kApCtrl] = 1U;
    __sync_synchronize();
    const uint64_t deadline = monotonicNanoseconds() + 5'000'000'000ULL;
    for (;;) {
        const uint32_t status = control[kApCtrl];
        if ((status & 0x2U) != 0U) {
            __sync_synchronize();
            return;
        }
        if (monotonicNanoseconds() > deadline) {
            throw std::runtime_error("accelerator timeout");
        }
    }
}

double percentile(const std::vector<double>& sorted, double fraction)
{
    return sorted[static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1))];
}

Stats summarize(std::vector<double> durationsUs, uint64_t digest)
{
    Stats stats;
    stats.meanUs = std::accumulate(durationsUs.begin(), durationsUs.end(), 0.0)
        / static_cast<double>(durationsUs.size());
    stats.fps = 1'000'000.0 / stats.meanUs;
    stats.digest = digest;
    std::sort(durationsUs.begin(), durationsUs.end());
    stats.p50Us = percentile(durationsUs, 0.50);
    stats.p95Us = percentile(durationsUs, 0.95);
    stats.p99Us = percentile(durationsUs, 0.99);
    stats.maxUs = durationsUs.back();
    return stats;
}

Json statsJson(const Stats& stats)
{
    return {{"mean_us", stats.meanUs}, {"p50_us", stats.p50Us},
            {"p95_us", stats.p95Us}, {"p99_us", stats.p99Us},
            {"max_us", stats.maxUs}, {"fps", stats.fps}, {"digest", stats.digest}};
}

Analysis extractContours(const cv::Mat& mask)
{
    Analysis analysis;
    cv::Mat scratch = mask.clone();
    cv::findContours(scratch, analysis.allContours, analysis.hierarchy,
                     cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);
    return analysis;
}

void prepareContourHierarchy(Analysis& analysis)
{
    for (size_t i = 0; i < analysis.allContours.size(); ++i) {
        if (cv::contourArea(analysis.allContours[i]) >= 10.0) {
            analysis.filteredContours.push_back(analysis.allContours[i]);
            analysis.originalIndices.push_back(i);
        }
    }
    for (size_t i = 0; i < analysis.originalIndices.size(); ++i) {
        const size_t original = analysis.originalIndices[i];
        if (original < analysis.hierarchy.size() && analysis.hierarchy[original][3] > -1) {
            analysis.innerContours.push_back(analysis.filteredContours[i]);
            analysis.innerFilteredIndices.push_back(static_cast<int>(i));
            const int parentOriginal = analysis.hierarchy[original][3];
            int filteredParent = -1;
            for (size_t j = 0; j < analysis.originalIndices.size(); ++j) {
                if (analysis.originalIndices[j] == static_cast<size_t>(parentOriginal)) {
                    filteredParent = static_cast<int>(j);
                    break;
                }
            }
            analysis.parentIndices.push_back(filteredParent);
        }
    }
}

void populateGeometry(Validation& result, const std::vector<cv::Point>& contour)
{
    if (contour.empty()) {
        return;
    }
    const cv::Rect bbox = cv::boundingRect(contour);
    result.bboxX = bbox.x;
    result.bboxY = bbox.y;
    result.bboxWidth = bbox.width;
    result.bboxHeight = bbox.height;
    const cv::Moments moments = cv::moments(contour);
    if (std::abs(moments.m00) > std::numeric_limits<double>::epsilon()) {
        result.centroidX = moments.m10 / moments.m00;
        result.centroidY = moments.m01 / moments.m00;
    } else {
        result.centroidX = result.bboxX + result.bboxWidth * 0.5;
        result.centroidY = result.bboxY + result.bboxHeight * 0.5;
    }
}

Brightness brightnessQuantiles(const cv::Mat& original, const cv::Mat& mask, cv::Rect region = {})
{
    Brightness result;
    cv::Rect scan(0, 0, std::min(original.cols, mask.cols), std::min(original.rows, mask.rows));
    if (region.width > 0 && region.height > 0) {
        scan &= region;
    }
    std::vector<uint8_t> values;
    values.reserve(static_cast<size_t>(std::max(0, scan.width * scan.height / 4)) + 1);
    for (int y = scan.y; y < scan.y + scan.height; ++y) {
        const uint8_t* maskRow = mask.ptr<uint8_t>(y);
        const uint8_t* imageRow = original.ptr<uint8_t>(y);
        for (int x = scan.x; x < scan.x + scan.width; ++x) {
            if (maskRow[x] > 0) {
                values.push_back(imageRow[x]);
            }
        }
    }
    if (values.empty()) {
        return result;
    }
    std::sort(values.begin(), values.end());
    const size_t count = values.size();
    result.q1 = values[count / 4];
    result.q2 = values[count / 2];
    result.q3 = values[(3 * count) / 4];
    result.q4 = values[count - 1];
    return result;
}

bool touchesBorder(const std::vector<cv::Point>& contour)
{
    for (const cv::Point& point : contour) {
        if (point.x < 2 || point.x >= kWidth - 2 || point.y < 2 || point.y >= kHeight - 2) {
            return true;
        }
    }
    return false;
}

cv::Mat objectMask(const Analysis& analysis, int innerIndex, int parentIndex)
{
    cv::Mat mask(kHeight, kWidth, CV_8UC1, cv::Scalar(0));
    if (parentIndex >= 0 && parentIndex < static_cast<int>(analysis.filteredContours.size())) {
        cv::drawContours(mask, analysis.filteredContours, parentIndex, cv::Scalar(255), cv::FILLED);
        if (innerIndex >= 0 && innerIndex < static_cast<int>(analysis.filteredContours.size())) {
            cv::drawContours(mask, analysis.filteredContours, innerIndex, cv::Scalar(0), cv::FILLED);
        }
    } else if (innerIndex >= 0 && innerIndex < static_cast<int>(analysis.filteredContours.size())) {
        cv::drawContours(mask, analysis.filteredContours, innerIndex, cv::Scalar(255), cv::FILLED);
    }
    return mask;
}

Validation evaluateInner(const Analysis& analysis, size_t innerIndex, int objectId,
                         int objectCount, const cv::Mat& original)
{
    Validation result;
    result.innerContourCount = static_cast<int>(analysis.innerContours.size());
    result.hasSingleInnerContour = analysis.innerContours.size() == 1;
    result.objectId = objectId;
    result.objectCount = objectCount;
    const auto& inner = analysis.innerContours[innerIndex];
    const int parent = analysis.parentIndices[innerIndex];
    const int innerFiltered = analysis.innerFilteredIndices[innerIndex];
    const cv::Mat objMask = objectMask(analysis, innerFiltered, parent);
    const auto& geometry = parent >= 0 && parent < static_cast<int>(analysis.filteredContours.size())
        ? analysis.filteredContours[static_cast<size_t>(parent)] : inner;
    populateGeometry(result, geometry);
    result.brightness = brightnessQuantiles(
        original, objMask,
        cv::Rect(static_cast<int>(result.bboxX), static_cast<int>(result.bboxY),
                 static_cast<int>(result.bboxWidth), static_cast<int>(result.bboxHeight)));
    if (touchesBorder(inner)) {
        result.touchesBorder = true;
        return result;
    }
    const double area = cv::contourArea(inner);
    if (area <= 0.0) {
        return result;
    }
    std::vector<cv::Point> hull;
    cv::convexHull(inner, hull);
    const double hullArea = cv::contourArea(hull);
    const double perimeter = cv::arcLength(hull, true);
    result.areaRatio = hullArea / area;
    result.deformability = 1.0 - (perimeter > 0.0
        ? std::sqrt(4.0 * M_PI * hullArea) / perimeter : 0.0);
    result.area = hullArea;
    if (parent >= 0 && parent < static_cast<int>(analysis.filteredContours.size())) {
        const double outerArea = cv::contourArea(analysis.filteredContours[static_cast<size_t>(parent)]);
        result.ringRatio = outerArea > area ? std::sqrt(outerArea - area) : 0.0;
    }
    const double areaUm2 = hullArea * kPixelToMicron * kPixelToMicron;
    const bool valid = areaUm2 >= 60.0 && areaUm2 <= 290.0
        && result.ringRatio > 15.0 && result.ringRatio < 25.0
        && result.deformability >= 0.0 && result.deformability <= 0.5;
    result.inRange = valid;
    result.isValid = valid;
    result.isTargetGroup = valid && areaUm2 >= 72.0 && areaUm2 <= 191.0
        && result.deformability >= 0.0 && result.deformability <= 0.3;
    return result;
}

std::vector<Validation> filterObjects(const cv::Mat& mask, const cv::Mat& original,
                                      Analysis& analysis, StageDurations& durations)
{
    uint64_t start = monotonicNanoseconds();
    analysis = extractContours(mask);
    uint64_t stop = monotonicNanoseconds();
    durations.contourExtractionUs = static_cast<double>(stop - start) / 1000.0;

    start = monotonicNanoseconds();
    prepareContourHierarchy(analysis);
    stop = monotonicNanoseconds();
    durations.hierarchyPairingUs = static_cast<double>(stop - start) / 1000.0;

    start = monotonicNanoseconds();
    Validation empty;
    empty.innerContourCount = static_cast<int>(analysis.innerContours.size());
    empty.hasSingleInnerContour = analysis.innerContours.size() == 1;
    empty.brightness = brightnessQuantiles(original, mask);
    if (analysis.innerContours.empty()) {
        durations.metricsFilteringUs =
            static_cast<double>(monotonicNanoseconds() - start) / 1000.0;
        return {empty};
    }
    std::vector<size_t> order(analysis.innerContours.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
        const cv::Rect lhsBox = cv::boundingRect(analysis.innerContours[lhs]);
        const cv::Rect rhsBox = cv::boundingRect(analysis.innerContours[rhs]);
        return std::tie(lhsBox.x, lhsBox.y, lhs) < std::tie(rhsBox.x, rhsBox.y, rhs);
    });
    std::vector<Validation> results;
    for (size_t i = 0; i < order.size(); ++i) {
        results.push_back(evaluateInner(analysis, order[i], static_cast<int>(i + 1),
                                        static_cast<int>(order.size()), original));
    }
    durations.metricsFilteringUs =
        static_cast<double>(monotonicNanoseconds() - start) / 1000.0;
    return results;
}

double rectArea(const cv::Rect2d& rect)
{
    return std::max(0.0, rect.width) * std::max(0.0, rect.height);
}

double rectIou(const cv::Rect2d& lhs, const cv::Rect2d& rhs)
{
    const double x1 = std::max(lhs.x, rhs.x);
    const double y1 = std::max(lhs.y, rhs.y);
    const double x2 = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
    const double y2 = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
    const double intersection = std::max(0.0, x2 - x1) * std::max(0.0, y2 - y1);
    const double unionArea = rectArea(lhs) + rectArea(rhs) - intersection;
    return unionArea > 0.0 ? intersection / unionArea : 0.0;
}

cv::Rect2d bbox(const Validation& value)
{
    return {value.bboxX, value.bboxY, value.bboxWidth, value.bboxHeight};
}

int matchingTrack(const std::vector<Track>& tracks, const std::vector<bool>& matched,
                  const Validation& detection, uint64_t frameIndex)
{
    constexpr uint64_t maxFrameGap = 5;
    constexpr double minIou = 0.08;
    const cv::Rect2d detectionBox = bbox(detection);
    if (rectArea(detectionBox) <= 0.0) {
        return -1;
    }
    int best = -1;
    double bestScore = std::numeric_limits<double>::max();
    for (size_t i = 0; i < tracks.size(); ++i) {
        const Track& track = tracks[i];
        if ((i < matched.size() && matched[i]) || frameIndex <= track.lastFrame) {
            continue;
        }
        const uint64_t gap = frameIndex - track.lastFrame;
        if (gap > maxFrameGap || detection.centroidX + 2.0 < track.lastCentroid.x) {
            continue;
        }
        const double overlap = rectIou(detectionBox, track.lastBbox);
        const double dx = detection.centroidX - track.lastCentroid.x;
        const double dy = std::abs(detection.centroidY - track.lastCentroid.y);
        const double motion = std::max(24.0,
            std::max(track.lastBbox.width, track.lastBbox.height) * 1.25 + gap * 8.0);
        const double directional = std::max(
            motion, std::max(64.0, kWidth * 0.35) * static_cast<double>(gap));
        const double vertical = std::max(
            20.0, std::max(track.lastBbox.height, detectionBox.height) * 1.5);
        if (overlap < minIou && (dx > directional || dy > vertical)) {
            continue;
        }
        const double score = (1.0 - std::min(1.0, overlap))
            + std::max(0.0, dx) / std::max(1.0, directional)
            + dy / std::max(1.0, vertical) + gap * 0.05;
        if (score < bestScore) {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

void applyTrack(Record& record, const Track& track)
{
    record.validation.trackId = track.id;
    record.validation.trackFirstFrame = track.firstFrame;
    record.validation.trackLastFrame = track.lastFrame;
    record.validation.trackObservationCount = track.observations;
}

void appendRecords(std::vector<Record>& records, std::vector<Track>& tracks,
                   std::vector<Validation> detections, uint64_t frameIndex)
{
    std::vector<bool> matched(tracks.size(), false);
    for (Validation& detection : detections) {
        if (!detection.isValid) {
            records.push_back({frameIndex, std::move(detection)});
            continue;
        }
        const int index = matchingTrack(tracks, matched, detection, frameIndex);
        if (index >= 0) {
            Track& track = tracks[static_cast<size_t>(index)];
            track.lastFrame = frameIndex;
            track.lastBbox = bbox(detection);
            track.lastCentroid = {detection.centroidX, detection.centroidY};
            ++track.observations;
            matched[static_cast<size_t>(index)] = true;
            applyTrack(records[track.outputIndex], track);
            continue;
        }
        Track track;
        track.id = static_cast<int>(tracks.size()) + 1;
        track.firstFrame = frameIndex;
        track.lastFrame = frameIndex;
        track.observations = 1;
        track.lastBbox = bbox(detection);
        track.lastCentroid = {detection.centroidX, detection.centroidY};
        track.outputIndex = records.size();
        Record record{frameIndex, std::move(detection)};
        applyTrack(record, track);
        records.push_back(std::move(record));
        tracks.push_back(std::move(track));
        matched.push_back(true);
    }
}

Json contourJson(const std::vector<cv::Point>& contour)
{
    Json points = Json::array();
    for (const cv::Point& point : contour) {
        points.push_back({point.x, point.y});
    }
    return points;
}

Json validationJson(const Validation& result)
{
    return {
        {"object_id", result.objectId}, {"object_count", result.objectCount},
        {"track_id", result.trackId}, {"track_first_frame", result.trackFirstFrame},
        {"track_last_frame", result.trackLastFrame},
        {"track_observation_count", result.trackObservationCount},
        {"is_valid", result.isValid}, {"is_target_group", result.isTargetGroup},
        {"touches_border", result.touchesBorder},
        {"has_single_inner_contour", result.hasSingleInnerContour},
        {"inner_contour_count", result.innerContourCount}, {"in_range", result.inRange},
        {"bbox", {result.bboxX, result.bboxY, result.bboxWidth, result.bboxHeight}},
        {"centroid", {result.centroidX, result.centroidY}}, {"area_pixels", result.area},
        {"area_um2", result.area * kPixelToMicron * kPixelToMicron},
        {"deformability", result.deformability}, {"area_ratio", result.areaRatio},
        {"ring_ratio", result.ringRatio}, {"youngs_modulus", result.youngsModulus},
        {"brightness", {{"q1", result.brightness.q1}, {"q2", result.brightness.q2},
                         {"q3", result.brightness.q3}, {"q4", result.brightness.q4}}},
    };
}

struct ArmFrameResult {
    Analysis analysis;
    StageDurations durations;
};

ArmFrameResult processArmFrame(uint8_t* maskData, const uint8_t* originalData,
                               std::vector<Record>& records,
                               std::vector<Track>& tracks, uint64_t frameIndex)
{
    cv::Mat mask(kHeight, kWidth, CV_8UC1, maskData);
    cv::Mat original(kHeight, kWidth, CV_8UC1,
                     const_cast<uint8_t*>(originalData));
    ArmFrameResult result;
    std::vector<Validation> detections =
        filterObjects(mask, original, result.analysis, result.durations);
    const uint64_t trackingStart = monotonicNanoseconds();
    appendRecords(records, tracks, std::move(detections), frameIndex);
    result.durations.trackingUs =
        static_cast<double>(monotonicNanoseconds() - trackingStart) / 1000.0;
    return result;
}

class ArmPipelineWorker {
public:
    ArmPipelineWorker(std::vector<Record>& records, std::vector<Track>& tracks)
        : records_(records)
        , tracks_(tracks)
        , thread_(&ArmPipelineWorker::run, this)
    {
    }

    ArmPipelineWorker(const ArmPipelineWorker&) = delete;
    ArmPipelineWorker& operator=(const ArmPipelineWorker&) = delete;

    ~ArmPipelineWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void submit(uint8_t* maskData, const uint8_t* originalData,
                uint64_t frameIndex)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (jobPending_ || processing_ || resultReady_) {
            throw std::logic_error("ARM pipeline worker already has a frame");
        }
        job_ = {maskData, originalData, frameIndex};
        jobPending_ = true;
        condition_.notify_all();
    }

    ArmFrameResult take()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return resultReady_; });
        resultReady_ = false;
        if (error_) {
            std::exception_ptr error = std::exchange(error_, nullptr);
            lock.unlock();
            std::rethrow_exception(error);
        }
        return std::move(result_);
    }

private:
    struct Job {
        uint8_t* maskData{nullptr};
        const uint8_t* originalData{nullptr};
        uint64_t frameIndex{0};
    };

    void run()
    {
        try {
            pinCurrentThread(3);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = std::current_exception();
            resultReady_ = true;
            condition_.notify_all();
            return;
        }
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&] { return stop_ || jobPending_; });
                if (stop_ && !jobPending_) {
                    return;
                }
                job = job_;
                jobPending_ = false;
                processing_ = true;
            }

            ArmFrameResult result;
            std::exception_ptr error;
            try {
                result = processArmFrame(job.maskData, job.originalData,
                                         records_, tracks_, job.frameIndex);
            } catch (...) {
                error = std::current_exception();
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                result_ = std::move(result);
                error_ = std::move(error);
                processing_ = false;
                resultReady_ = true;
            }
            condition_.notify_all();
        }
    }

    std::vector<Record>& records_;
    std::vector<Track>& tracks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    Job job_;
    ArmFrameResult result_;
    std::exception_ptr error_;
    bool jobPending_{false};
    bool processing_{false};
    bool resultReady_{false};
    bool stop_{false};
    std::thread thread_;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 8) {
        std::cerr << "usage: ultra96_direct_ddr_runner <input.raw> <background.raw> "
                     "<masks.raw> <result.json> <frame-count> <ppc> <clock-mhz>\n";
        return 2;
    }
    try {
        pinCurrentThread(2);
        cv::setNumThreads(1);
        const size_t frameCount = static_cast<size_t>(std::stoul(argv[5]));
        const int pixelsPerClock = std::stoi(argv[6]);
        const int clockMhz = std::stoi(argv[7]);
        if (pixelsPerClock != 4 || clockMhz != 250) {
            throw std::runtime_error(
                "the direct-DDR image must be run as PPC 4 at 250 MHz");
        }

        const std::vector<uint8_t> inputs = readFile(argv[1]);
        const std::vector<uint8_t> background = readFile(argv[2]);
        if (frameCount == 0 || inputs.size() != frameCount * kFrameBytes
            || background.size() != kFrameBytes) {
            throw std::runtime_error("invalid input/background size");
        }
        std::vector<uint8_t> outputs(inputs.size());

        if (xclProbe() == 0) {
            throw std::runtime_error("no XRT devices found");
        }
        XrtDevice device;
        XrtBuffer backgroundBuffer(device.get(), kFrameBytes);
        std::array<std::unique_ptr<XrtBuffer>, 2> inputBuffers;
        std::array<std::unique_ptr<XrtBuffer>, 2> outputBuffers;
        for (size_t slot = 0; slot < inputBuffers.size(); ++slot) {
            inputBuffers[slot] = std::make_unique<XrtBuffer>(device.get(), kFrameBytes);
            outputBuffers[slot] = std::make_unique<XrtBuffer>(device.get(), kFrameBytes);
        }

        const int fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (fd < 0) {
            throw std::runtime_error("failed to open /dev/mem: "
                                     + std::string(std::strerror(errno)));
        }
        Mapping controlMap(fd, kControlBase, kControlSpan);
        close(fd);
        volatile uint32_t* control = controlMap.words();

        const uint64_t backgroundCopyStart = monotonicNanoseconds();
        std::memcpy(backgroundBuffer.data(), background.data(), kFrameBytes);
        const uint64_t backgroundCopyStop = monotonicNanoseconds();
        backgroundBuffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        const uint64_t backgroundSyncStop = monotonicNanoseconds();
        configureAccelerator(control, inputBuffers[0]->address(),
                             backgroundBuffer.address(), outputBuffers[0]->address());

        Json frameResults = Json::array();
        std::vector<Record> records;
        std::vector<Track> tracks;
        std::vector<double> inputCopyUs;
        std::vector<double> inputSyncUs;
        std::vector<double> bufferSelectUs;
        std::vector<double> kernelUs;
        std::vector<double> outputSyncUs;
        std::vector<double> maskArchiveCopyUs;
        std::vector<double> contourExtractionUs;
        std::vector<double> hierarchyPairingUs;
        std::vector<double> metricsFilteringUs;
        std::vector<double> trackingUs;
        std::vector<double> armPostUs;
        std::vector<double> fullBoardUs;
        uint64_t totalContours = 0;
        uint64_t totalWhitePixels = 0;

        for (size_t index = 0; index < frameCount; ++index) {
            const size_t slot = index % inputBuffers.size();
            XrtBuffer& inputBuffer = *inputBuffers[slot];
            XrtBuffer& outputBuffer = *outputBuffers[slot];

            const uint64_t fullStart = monotonicNanoseconds();
            std::memcpy(inputBuffer.data(), inputs.data() + index * kFrameBytes,
                        kFrameBytes);
            const uint64_t inputCopyStop = monotonicNanoseconds();
            inputBuffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            const uint64_t inputSyncStop = monotonicNanoseconds();
            selectFrameBuffers(control, inputBuffer.address(), outputBuffer.address());
            const uint64_t bufferSelectStop = monotonicNanoseconds();
            launchAndWait(control);
            const uint64_t kernelStop = monotonicNanoseconds();
            outputBuffer.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
            const uint64_t outputSyncStop = monotonicNanoseconds();

            ArmFrameResult arm = processArmFrame(
                outputBuffer.data(), inputs.data() + index * kFrameBytes,
                records, tracks, index);
            const uint64_t armStop = monotonicNanoseconds();

            cv::Mat mask(kHeight, kWidth, CV_8UC1, outputBuffer.data());
            Json contours = Json::array();
            for (const auto& contour : arm.analysis.allContours) {
                contours.push_back(contourJson(contour));
            }
            totalContours += contours.size();
            const int whitePixels = cv::countNonZero(mask);
            totalWhitePixels += static_cast<uint64_t>(whitePixels);
            frameResults.push_back({{"frame_index", index},
                                    {"white_pixels", whitePixels},
                                    {"contours", std::move(contours)}});

            const uint64_t archiveCopyStart = monotonicNanoseconds();
            std::memcpy(outputs.data() + index * kFrameBytes,
                        outputBuffer.data(), kFrameBytes);
            const uint64_t archiveCopyStop = monotonicNanoseconds();

            inputCopyUs.push_back(
                static_cast<double>(inputCopyStop - fullStart) / 1000.0);
            inputSyncUs.push_back(
                static_cast<double>(inputSyncStop - inputCopyStop) / 1000.0);
            bufferSelectUs.push_back(
                static_cast<double>(bufferSelectStop - inputSyncStop) / 1000.0);
            kernelUs.push_back(
                static_cast<double>(kernelStop - bufferSelectStop) / 1000.0);
            outputSyncUs.push_back(
                static_cast<double>(outputSyncStop - kernelStop) / 1000.0);
            maskArchiveCopyUs.push_back(
                static_cast<double>(archiveCopyStop - archiveCopyStart) / 1000.0);
            contourExtractionUs.push_back(arm.durations.contourExtractionUs);
            hierarchyPairingUs.push_back(arm.durations.hierarchyPairingUs);
            metricsFilteringUs.push_back(arm.durations.metricsFilteringUs);
            trackingUs.push_back(arm.durations.trackingUs);
            armPostUs.push_back(
                static_cast<double>(armStop - outputSyncStop) / 1000.0);
            fullBoardUs.push_back(
                static_cast<double>(armStop - fullStart) / 1000.0);
        }
        writeFile(argv[3], outputs);

        for (int i = 0; i < kWarmupFrames; ++i) {
            const size_t index = static_cast<size_t>(i) % frameCount;
            const size_t slot = static_cast<size_t>(i) % inputBuffers.size();
            XrtBuffer& inputBuffer = *inputBuffers[slot];
            XrtBuffer& outputBuffer = *outputBuffers[slot];
            std::memcpy(inputBuffer.data(), inputs.data() + index * kFrameBytes,
                        kFrameBytes);
            inputBuffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            selectFrameBuffers(control, inputBuffer.address(), outputBuffer.address());
            launchAndWait(control);
        }

        std::vector<double> steadyKernelUs;
        steadyKernelUs.reserve(kMeasuredFrames);
        uint64_t kernelDigest = 0;
        for (int i = 0; i < kMeasuredFrames; ++i) {
            const size_t index = static_cast<size_t>(i) % frameCount;
            const size_t slot = static_cast<size_t>(i) % inputBuffers.size();
            XrtBuffer& inputBuffer = *inputBuffers[slot];
            XrtBuffer& outputBuffer = *outputBuffers[slot];
            std::memcpy(inputBuffer.data(), inputs.data() + index * kFrameBytes,
                        kFrameBytes);
            inputBuffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            selectFrameBuffers(control, inputBuffer.address(), outputBuffer.address());
            const uint64_t start = monotonicNanoseconds();
            launchAndWait(control);
            const uint64_t stop = monotonicNanoseconds();
            steadyKernelUs.push_back(static_cast<double>(stop - start) / 1000.0);
            kernelDigest += control[kThreshold];
        }

        std::vector<double> steadyFullUs;
        std::vector<double> steadyInputCopyUs;
        std::vector<double> steadyInputSyncUs;
        std::vector<double> steadyBufferSelectUs;
        std::vector<double> steadyFullKernelUs;
        std::vector<double> steadyOutputSyncUs;
        std::vector<double> steadyContourExtractionUs;
        std::vector<double> steadyHierarchyPairingUs;
        std::vector<double> steadyMetricsFilteringUs;
        std::vector<double> steadyTrackingUs;
        std::vector<double> steadyArmPostUs;
        std::vector<double> steadyMaskVerificationUs;
        steadyFullUs.reserve(kMeasuredFrames);
        steadyInputCopyUs.reserve(kMeasuredFrames);
        steadyInputSyncUs.reserve(kMeasuredFrames);
        steadyBufferSelectUs.reserve(kMeasuredFrames);
        steadyFullKernelUs.reserve(kMeasuredFrames);
        steadyOutputSyncUs.reserve(kMeasuredFrames);
        steadyContourExtractionUs.reserve(kMeasuredFrames);
        steadyHierarchyPairingUs.reserve(kMeasuredFrames);
        steadyMetricsFilteringUs.reserve(kMeasuredFrames);
        steadyTrackingUs.reserve(kMeasuredFrames);
        steadyArmPostUs.reserve(kMeasuredFrames);
        steadyMaskVerificationUs.reserve(kMeasuredFrames);
        std::vector<Record> steadyRecords;
        std::vector<Track> steadyTracks;
        uint64_t fullDigest = 0;

        for (int i = 0; i < kMeasuredFrames; ++i) {
            const size_t index = static_cast<size_t>(i) % frameCount;
            const size_t slot = static_cast<size_t>(i) % inputBuffers.size();
            XrtBuffer& inputBuffer = *inputBuffers[slot];
            XrtBuffer& outputBuffer = *outputBuffers[slot];

            const uint64_t start = monotonicNanoseconds();
            std::memcpy(inputBuffer.data(), inputs.data() + index * kFrameBytes,
                        kFrameBytes);
            const uint64_t inputCopyStop = monotonicNanoseconds();
            inputBuffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            const uint64_t inputSyncStop = monotonicNanoseconds();
            selectFrameBuffers(control, inputBuffer.address(), outputBuffer.address());
            const uint64_t bufferSelectStop = monotonicNanoseconds();
            launchAndWait(control);
            const uint64_t kernelStop = monotonicNanoseconds();
            outputBuffer.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
            const uint64_t outputSyncStop = monotonicNanoseconds();
            ArmFrameResult arm = processArmFrame(
                outputBuffer.data(), inputs.data() + index * kFrameBytes,
                steadyRecords, steadyTracks, static_cast<uint64_t>(i));
            const uint64_t stop = monotonicNanoseconds();
            requireExactMask(outputBuffer.data(),
                             outputs.data() + index * kFrameBytes,
                             "steady serial", static_cast<size_t>(i));
            const uint64_t verificationStop = monotonicNanoseconds();

            steadyFullUs.push_back(static_cast<double>(stop - start) / 1000.0);
            steadyInputCopyUs.push_back(
                static_cast<double>(inputCopyStop - start) / 1000.0);
            steadyInputSyncUs.push_back(
                static_cast<double>(inputSyncStop - inputCopyStop) / 1000.0);
            steadyBufferSelectUs.push_back(
                static_cast<double>(bufferSelectStop - inputSyncStop) / 1000.0);
            steadyFullKernelUs.push_back(
                static_cast<double>(kernelStop - bufferSelectStop) / 1000.0);
            steadyOutputSyncUs.push_back(
                static_cast<double>(outputSyncStop - kernelStop) / 1000.0);
            steadyContourExtractionUs.push_back(arm.durations.contourExtractionUs);
            steadyHierarchyPairingUs.push_back(arm.durations.hierarchyPairingUs);
            steadyMetricsFilteringUs.push_back(arm.durations.metricsFilteringUs);
            steadyTrackingUs.push_back(arm.durations.trackingUs);
            steadyArmPostUs.push_back(
                static_cast<double>(stop - outputSyncStop) / 1000.0);
            steadyMaskVerificationUs.push_back(
                static_cast<double>(verificationStop - stop) / 1000.0);
            fullDigest += static_cast<uint64_t>(
                arm.analysis.allContours.size() + steadyRecords.size());
        }

        std::vector<Record> pipelinedRecords;
        std::vector<Track> pipelinedTracks;
        std::vector<double> pipelinedContourExtractionUs;
        std::vector<double> pipelinedHierarchyPairingUs;
        std::vector<double> pipelinedMetricsFilteringUs;
        std::vector<double> pipelinedTrackingUs;
        std::vector<double> pipelinedInputCopyUs;
        std::vector<double> pipelinedInputSyncUs;
        std::vector<double> pipelinedBufferSelectUs;
        std::vector<double> pipelinedKernelUs;
        std::vector<double> pipelinedOutputSyncUs;
        pipelinedContourExtractionUs.reserve(kMeasuredFrames);
        pipelinedHierarchyPairingUs.reserve(kMeasuredFrames);
        pipelinedMetricsFilteringUs.reserve(kMeasuredFrames);
        pipelinedTrackingUs.reserve(kMeasuredFrames);
        pipelinedInputCopyUs.reserve(kMeasuredFrames);
        pipelinedInputSyncUs.reserve(kMeasuredFrames);
        pipelinedBufferSelectUs.reserve(kMeasuredFrames);
        pipelinedKernelUs.reserve(kMeasuredFrames);
        pipelinedOutputSyncUs.reserve(kMeasuredFrames);
        ArmPipelineWorker armWorker(pipelinedRecords, pipelinedTracks);
        bool armFramePending = false;
        uint64_t pipelinedDigest = 0;

        auto collectArmResult = [&](ArmFrameResult arm) {
            pipelinedContourExtractionUs.push_back(
                arm.durations.contourExtractionUs);
            pipelinedHierarchyPairingUs.push_back(
                arm.durations.hierarchyPairingUs);
            pipelinedMetricsFilteringUs.push_back(
                arm.durations.metricsFilteringUs);
            pipelinedTrackingUs.push_back(arm.durations.trackingUs);
            pipelinedDigest += static_cast<uint64_t>(
                arm.analysis.allContours.size() + pipelinedRecords.size());
        };

        const uint64_t pipelineStart = monotonicNanoseconds();
        for (int i = 0; i < kMeasuredFrames; ++i) {
            const size_t index = static_cast<size_t>(i) % frameCount;
            const size_t slot = static_cast<size_t>(i) % inputBuffers.size();
            XrtBuffer& inputBuffer = *inputBuffers[slot];
            XrtBuffer& outputBuffer = *outputBuffers[slot];

            const uint64_t frameStart = monotonicNanoseconds();
            std::memcpy(inputBuffer.data(), inputs.data() + index * kFrameBytes,
                        kFrameBytes);
            const uint64_t inputCopyStop = monotonicNanoseconds();
            inputBuffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            const uint64_t inputSyncStop = monotonicNanoseconds();
            selectFrameBuffers(control, inputBuffer.address(), outputBuffer.address());
            const uint64_t bufferSelectStop = monotonicNanoseconds();
            launchAndWait(control);
            const uint64_t kernelStop = monotonicNanoseconds();
            outputBuffer.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
            const uint64_t outputSyncStop = monotonicNanoseconds();
            pipelinedInputCopyUs.push_back(
                static_cast<double>(inputCopyStop - frameStart) / 1000.0);
            pipelinedInputSyncUs.push_back(
                static_cast<double>(inputSyncStop - inputCopyStop) / 1000.0);
            pipelinedBufferSelectUs.push_back(
                static_cast<double>(bufferSelectStop - inputSyncStop) / 1000.0);
            pipelinedKernelUs.push_back(
                static_cast<double>(kernelStop - bufferSelectStop) / 1000.0);
            pipelinedOutputSyncUs.push_back(
                static_cast<double>(outputSyncStop - kernelStop) / 1000.0);

            if (armFramePending) {
                collectArmResult(armWorker.take());
            }
            armWorker.submit(outputBuffer.data(),
                             inputs.data() + index * kFrameBytes,
                             static_cast<uint64_t>(i));
            armFramePending = true;
        }
        if (armFramePending) {
            collectArmResult(armWorker.take());
        }
        const uint64_t pipelineStop = monotonicNanoseconds();
        const double pipelineTotalUs =
            static_cast<double>(pipelineStop - pipelineStart) / 1000.0;
        const double pipelineMeanUs =
            pipelineTotalUs / static_cast<double>(kMeasuredFrames);

        Json recordResults = Json::array();
        size_t candidateRecords = 0;
        size_t validRecords = 0;
        size_t targetRecords = 0;
        for (const Record& record : records) {
            candidateRecords += record.validation.objectId > 0;
            validRecords += record.validation.isValid;
            targetRecords += record.validation.isTargetGroup;
            recordResults.push_back({
                {"frame_index", record.frameIndex},
                {"validation", validationJson(record.validation)},
            });
        }

        const Json result{
            {"board", "Ultra96-V2"},
            {"execution",
             "physical PL mask over HP0 direct DDR plus Ultra96 ARM MIB post-processing"},
            {"transport", "XRT CMA buffer objects over S_AXI_HP0_FPD"},
            {"frame_count", frameCount},
            {"geometry", {kWidth, kHeight}},
            {"background_enabled", true},
            {"threshold", 8},
            {"pl_clock_hz", static_cast<uint64_t>(clockMhz) * 1'000'000ULL},
            {"pixels_per_clock", pixelsPerClock},
            {"buffer_addresses",
             {{"input_0", inputBuffers[0]->address()},
              {"input_1", inputBuffers[1]->address()},
              {"background", backgroundBuffer.address()},
              {"output_0", outputBuffers[0]->address()},
              {"output_1", outputBuffers[1]->address()}}},
            {"setup_timing_us",
             {{"background_copy",
               static_cast<double>(backgroundCopyStop - backgroundCopyStart)
                   / 1000.0},
              {"background_sync_to_device",
               static_cast<double>(backgroundSyncStop - backgroundCopyStop)
                   / 1000.0}}},
            {"summary",
             {{"total_white_pixels", totalWhitePixels},
              {"total_contours", totalContours},
              {"batch_records", records.size()},
              {"candidate_records", candidateRecords},
              {"valid_records", validRecords},
              {"target_records", targetRecords}}},
            {"correctness_pass_timing_us",
             {{"input_cpu_copy", statsJson(summarize(inputCopyUs, 0))},
              {"input_sync_to_device", statsJson(summarize(inputSyncUs, 0))},
              {"buffer_select_mmio", statsJson(summarize(bufferSelectUs, 0))},
              {"fpga_kernel", statsJson(summarize(kernelUs, 0))},
              {"output_sync_from_device", statsJson(summarize(outputSyncUs, 0))},
              {"contour_extraction",
               statsJson(summarize(contourExtractionUs, 0))},
              {"hierarchy_pairing",
               statsJson(summarize(hierarchyPairingUs, 0))},
              {"metrics_filtering",
               statsJson(summarize(metricsFilteringUs, 0))},
              {"tracking", statsJson(summarize(trackingUs, 0))},
              {"arm_postprocess", statsJson(summarize(armPostUs, 0))},
              {"board_end_to_end", statsJson(summarize(fullBoardUs, 0))},
              {"mask_archive_copy_not_in_end_to_end",
               statsJson(summarize(maskArchiveCopyUs, 0))}}},
            {"steady_state",
             {{"warmup_frames", kWarmupFrames},
              {"measured_frames", kMeasuredFrames},
              {"fpga_kernel",
               statsJson(summarize(std::move(steadyKernelUs), kernelDigest))},
              {"input_cpu_copy",
               statsJson(summarize(std::move(steadyInputCopyUs), 0))},
              {"input_sync_to_device",
               statsJson(summarize(std::move(steadyInputSyncUs), 0))},
              {"buffer_select_mmio",
               statsJson(summarize(std::move(steadyBufferSelectUs), 0))},
              {"full_pass_fpga_kernel",
               statsJson(summarize(std::move(steadyFullKernelUs), 0))},
              {"output_sync_from_device",
               statsJson(summarize(std::move(steadyOutputSyncUs), 0))},
              {"contour_extraction",
               statsJson(summarize(
                   std::move(steadyContourExtractionUs), 0))},
              {"hierarchy_pairing",
               statsJson(summarize(
                   std::move(steadyHierarchyPairingUs), 0))},
              {"metrics_filtering",
               statsJson(summarize(
                   std::move(steadyMetricsFilteringUs), 0))},
              {"tracking",
               statsJson(summarize(std::move(steadyTrackingUs), 0))},
              {"arm_postprocess",
               statsJson(summarize(std::move(steadyArmPostUs), 0))},
              {"mask_verification_not_in_end_to_end",
               statsJson(summarize(
                   std::move(steadyMaskVerificationUs), 0))},
              {"board_end_to_end",
               statsJson(summarize(std::move(steadyFullUs), fullDigest))},
              {"pipelined",
               {{"buffer_slots", inputBuffers.size()},
                {"total_us", pipelineTotalUs},
                {"mean_frame_us", pipelineMeanUs},
                {"fps", 1'000'000.0 / pipelineMeanUs},
                {"digest", pipelinedDigest},
                {"input_cpu_copy",
                 statsJson(summarize(
                     std::move(pipelinedInputCopyUs), 0))},
                {"input_sync_to_device",
                 statsJson(summarize(
                     std::move(pipelinedInputSyncUs), 0))},
                {"buffer_select_mmio",
                 statsJson(summarize(
                     std::move(pipelinedBufferSelectUs), 0))},
                {"fpga_kernel",
                 statsJson(summarize(
                     std::move(pipelinedKernelUs), 0))},
                {"output_sync_from_device",
                 statsJson(summarize(
                     std::move(pipelinedOutputSyncUs), 0))},
                {"contour_extraction",
                 statsJson(summarize(
                     std::move(pipelinedContourExtractionUs), 0))},
                {"hierarchy_pairing",
                 statsJson(summarize(
                     std::move(pipelinedHierarchyPairingUs), 0))},
                {"metrics_filtering",
                 statsJson(summarize(
                     std::move(pipelinedMetricsFilteringUs), 0))},
                {"tracking",
                 statsJson(summarize(
                     std::move(pipelinedTrackingUs), 0))}}}}},
            {"frames", std::move(frameResults)},
            {"records", std::move(recordResults)},
        };

        std::ofstream jsonOutput(argv[4]);
        if (!jsonOutput) {
            throw std::runtime_error("failed to open result JSON");
        }
        jsonOutput << std::setw(2) << result << '\n';
        std::cout << std::setw(2) << result.at("summary") << '\n';
        std::cout << "ULTRA96_DIRECT_DDR_EXACT_RUN frames=" << frameCount
                  << " serial_fps="
                  << result.at("steady_state")
                         .at("board_end_to_end")
                         .at("fps")
                  << " pipelined_fps="
                  << result.at("steady_state").at("pipelined").at("fps")
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

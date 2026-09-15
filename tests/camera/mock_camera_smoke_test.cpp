#include "backend/camera/mock/MockCamera.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace
{
std::filesystem::path makeTempDir()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<unsigned long long> dist;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const auto path = std::filesystem::temp_directory_path() /
                          ("mib_mock_camera_" + std::to_string(dist(gen)));
        std::error_code ec;
        if (std::filesystem::create_directories(path, ec))
        {
            return path;
        }
    }
    throw std::runtime_error("failed to create temporary directory");
}

bool writeFrame(const std::filesystem::path& path, unsigned char value)
{
    const cv::Mat image(12, 16, CV_8UC1, cv::Scalar(value));
    return cv::imwrite(path.string(), image);
}

// A decoded mock frame should be Mono8, tightly packed (linePitch == width, so
// data size == width*height), and — for our constant-fill inputs — every byte
// should equal the written value. Guards the OpenCV decode against pitch/format
// regressions, not just shape.
bool frameIsUniform(const camera::common::Frame& f, unsigned char value)
{
    if (f.pixelFormat != 0x01080001ULL) return false;
    if (f.linePitch != f.width) return false;
    if (f.data.size() != f.width * f.height) return false;
    for (unsigned char b : f.data)
    {
        if (b != value) return false;
    }
    return true;
}
} // namespace

int main()
{
    const auto frameDir = makeTempDir();
    if (!writeFrame(frameDir / "frame_002.png", 120) ||
        !writeFrame(frameDir / "frame_001.png", 60))
    {
        std::cerr << "failed to write mock camera frames\n";
        std::filesystem::remove_all(frameDir);
        return 1;
    }

    camera::mock::MockCameraOptions options;
    options.folder = frameDir;
    options.frameInterval = std::chrono::microseconds::zero();
    options.loopFiles = false;

    camera::mock::MockCamera camera(options);
    camera.applyConfig({});

    if (!camera.start() || !camera.isRunning())
    {
        std::cerr << "mock camera should start from image folder\n";
        std::filesystem::remove_all(frameDir);
        return 2;
    }

    camera::common::Frame first;
    if (!camera.grabFrame(first))
    {
        std::cerr << "mock camera should deliver first frame\n";
        std::filesystem::remove_all(frameDir);
        return 3;
    }
    if (first.width != 16 || first.height != 12 || first.data.empty())
    {
        std::cerr << "first mock frame has unexpected shape or payload\n";
        std::filesystem::remove_all(frameDir);
        return 4;
    }
    // frame_001.png (value 60) sorts first. Decoded pixels must survive intact.
    if (!frameIsUniform(first, 60))
    {
        std::cerr << "first mock frame lost its pixel values / packing\n";
        std::filesystem::remove_all(frameDir);
        return 8;
    }

    camera::common::CameraStats stats;
    if (!camera.pollStats(stats))
    {
        std::cerr << "mock camera should expose stats while running\n";
        std::filesystem::remove_all(frameDir);
        return 5;
    }

    camera::common::Frame second;
    if (!camera.grabFrame(second))
    {
        std::cerr << "mock camera should deliver second frame\n";
        std::filesystem::remove_all(frameDir);
        return 6;
    }
    if (!frameIsUniform(second, 120))
    {
        std::cerr << "second mock frame lost its pixel values / packing\n";
        std::filesystem::remove_all(frameDir);
        return 9;
    }

    camera::common::Frame third;
    if (camera.grabFrame(third) || camera.isRunning())
    {
        std::cerr << "non-looping mock camera should stop after the final frame\n";
        std::filesystem::remove_all(frameDir);
        return 7;
    }

    camera.stop();
    std::filesystem::remove_all(frameDir);

    // TIFF path: the unified OpenCV decode must handle .tif inputs too (the old
    // code only reached OpenCV for TIFF as a QImageReader fallback).
    const auto tiffDir = makeTempDir();
    if (!writeFrame(tiffDir / "frame_001.tif", 200))
    {
        std::cerr << "failed to write mock camera TIFF frame\n";
        std::filesystem::remove_all(tiffDir);
        return 10;
    }

    camera::mock::MockCameraOptions tiffOptions;
    tiffOptions.folder = tiffDir;
    tiffOptions.frameInterval = std::chrono::microseconds::zero();
    tiffOptions.loopFiles = false;

    camera::mock::MockCamera tiffCamera(tiffOptions);
    tiffCamera.applyConfig({});
    camera::common::Frame tiffFrame;
    const bool tiffOk = tiffCamera.start() && tiffCamera.grabFrame(tiffFrame) &&
                        frameIsUniform(tiffFrame, 200);
    tiffCamera.stop();
    std::filesystem::remove_all(tiffDir);
    if (!tiffOk)
    {
        std::cerr << "mock camera should decode a TIFF frame via OpenCV\n";
        return 11;
    }

    return 0;
}

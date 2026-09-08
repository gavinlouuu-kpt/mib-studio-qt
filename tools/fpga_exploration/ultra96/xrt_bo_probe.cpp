#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sys/mman.h>

#include <xrt/xrt.h>

namespace {

constexpr size_t kProbeBytes = 512U * 96U;

class Device {
public:
    Device()
        : handle_(xclOpen(0, nullptr, XCL_QUIET))
    {
        if (handle_ == nullptr) {
            throw std::runtime_error("xclOpen(0) failed");
        }
    }

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    ~Device() { xclClose(handle_); }

    xclDeviceHandle get() const { return handle_; }

private:
    xclDeviceHandle handle_{nullptr};
};

class Buffer {
public:
    Buffer(xclDeviceHandle device, size_t size)
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
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    ~Buffer()
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
    uint64_t allocatedSize() const { return properties_.size; }

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

} // namespace

int main()
{
    try {
        if (xclProbe() == 0) {
            throw std::runtime_error("no XRT devices found");
        }
        Device device;
        Buffer buffer(device.get(), kProbeBytes);
        for (size_t index = 0; index < kProbeBytes; ++index) {
            buffer.data()[index] = static_cast<uint8_t>((index * 29U + 7U) & 0xffU);
        }
        buffer.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        buffer.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
        for (size_t index = 0; index < kProbeBytes; ++index) {
            const auto expected = static_cast<uint8_t>((index * 29U + 7U) & 0xffU);
            if (buffer.data()[index] != expected) {
                throw std::runtime_error("round-trip mismatch at byte "
                                         + std::to_string(index));
            }
        }
        std::cout << "XRT_BO_PROBE_OK address=0x" << std::hex << buffer.address()
                  << std::dec << " requested_bytes=" << kProbeBytes
                  << " allocated_bytes=" << buffer.allocatedSize() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "XRT_BO_PROBE_FAILED " << error.what() << '\n';
        return 1;
    }
}

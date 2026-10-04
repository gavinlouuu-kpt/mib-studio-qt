#pragma once

// OpenCV parallelism under ThreadSanitizer.
//
// Ubuntu's OpenCV runs cv::parallel_for_ on uninstrumented TBB: a stripe
// buffer allocated on a TBB worker and freed by the caller after the join is
// reported as a race, because TSan cannot see the join (and the library's own
// memory accesses are invisible to it anyway). Tests that push full-size
// frames through blur / connected components call this first: under TSan it
// makes OpenCV run its bodies on the calling thread, so the remaining reports
// are about our threads only. Without TSan it does nothing.

#include <opencv2/core/utility.hpp>

#if defined(__SANITIZE_THREAD__)
#define MIB_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MIB_UNDER_TSAN 1
#endif
#endif

namespace mib::test {

inline void serializeOpenCvUnderTsan() {
#ifdef MIB_UNDER_TSAN
    cv::setNumThreads(0);
#endif
}

} // namespace mib::test

// Link probe for YOFO Review's review core (plan
// 2026-10-01-standalone-review-app, ADR 0014). Built only with
// MIB_BUILD_REVIEW_LINK_PROBE (the *-review-core presets): CMake resolves its
// full link line — mib_review_core, mib_processing and every static Conan
// dependency, in order — and tools/gen_review_link_manifest.py hands that line
// to the Rust bridge's build.rs on macOS and Windows. Running it is also a
// native smoke test: the statically linked review core starts and refuses a
// missing file cleanly.
#include "backend/review/ReviewSession.h"

#include <cstdio>

int main()
{
    backend::review::ReviewSession session;
    if (session.isOpen()) return 1;
    if (session.open("this-file-does-not-exist.h5")) return 2;
    std::puts("mib_review_link_probe: review core linked and running");
    return 0;
}

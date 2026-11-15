#include "effects/loading_upload.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace storm::effects;
void require(bool good, const char *what) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
int main() {
    storm::render::LoadingOpaque opaque;
    auto frame = [&](bool preparing = true, uint64_t generation = 7) {
        return LoadingUploadScope::for_frame(generation, preparing, opaque);
    };
    require(!frame().active(), "no opacity receipt grants no loading batch");
    opaque.arm(7, true, true, 255);
    require(!frame().active(), "arming is not a submission/presentation");
    opaque.submitted(7);
    require(!frame().active(), "submission is not a stable advanced presentation");
    opaque.frame(7, false, true);
    require(!frame().active(), "unadvanced frame revokes opacity");
    opaque.arm(7, true, true, 255); opaque.submitted(7); opaque.frame(7, true, false);
    require(!frame().active(), "surface generation change revokes loading batching");
    opaque.arm(7, true, true, 255); opaque.submitted(7); opaque.frame(7, true, true);
    const auto scope = frame();
    require(scope.active() && scope.accepts(7, true), "actual opaque stable current frame admits selected current scene");
    require(!scope.accepts(6, true) && !scope.accepts(7, false), "old scene and nonselected entry get no extra uploads");
    require(!frame(false).active() && !frame(true, 8).active() && !frame(true, 0).active(),
        "ready/revealed/cancelled scene and replacement generation receive no batch");
    opaque.invalidate();
    require(!frame().active(), "device loss/resize invalidation revokes receipt");

    UploadBatch ordinary(false);
    require(ordinary.allows(100, 0), "first gameplay upload permitted"); ordinary.submitted(100);
    require(ordinary.allows(100, 9000), "second gameplay upload preserves prior policy"); ordinary.submitted(100);
    require(!ordinary.allows(1, 0), "gameplay remains exactly two uploads even with pending work");
    UploadBatch bytes(true);
    bytes.submitted(UploadBatch::kLoadingBytes - 10);
    require(bytes.allows(10, 0), "exact remaining loading byte budget permitted"); bytes.submitted(10);
    require(!bytes.allows(1, 0), "next byte beyond loading budget deferred");
    UploadBatch time(true); time.submitted(1);
    require(time.allows(1, 3.999) && !time.allows(1, 4.0) && !time.allows(1, 1000),
        "loading batch stops at CPU-submit deadline between nonpreemptible uploads");
    UploadBatch count(true);
    for (unsigned i = 0; i < UploadBatch::kLoadingUploads; ++i) {
        require(count.allows(1, 0), "bounded loading count advances"); count.submitted(1);
    }
    require(!count.allows(1, 0), "hard texture count limits zero-time tiny uploads");
    UploadBatch oversized(true);
    require(oversized.allows(UploadBatch::kLoadingBytes + 1, 1000), "oversized singleton cannot deadlock readiness");
    oversized.submitted(UploadBatch::kLoadingBytes + 1);
    require(!oversized.allows(1, 0), "oversized singleton ends loading batch");
    oversized.submitted(std::numeric_limits<uint64_t>::max());
    require(oversized.bytes() == std::numeric_limits<uint64_t>::max() && !oversized.allows(1, 0),
        "byte counter cannot overflow and reopen budget");
    std::puts("PASS: production upload admission, actual opacity receipt, byte/time/count bounds, singleton progress, gameplay cap");
}

// Host-side tests for CoordinateInput.h (issue #38).
// Run with tests/native/run.sh — no board needed.

#include "../../CoordinateInput.h"

#include <cmath>
#include <cstdio>

static int failures = 0;

static void accepts(const char* text, double min_deg, double max_deg, double want) {
    double got = 12345.0;
    if (!parse_coordinate_text(text, min_deg, max_deg, got) || std::fabs(got - want) > 1e-9) {
        std::fprintf(stderr, "FAIL: \"%s\" should parse as %.6f (got %.6f)\n", text, want, got);
        failures++;
    }
}

static void rejects(const char* text, double min_deg, double max_deg) {
    double got = 12345.0;
    if (parse_coordinate_text(text, min_deg, max_deg, got) || got != 12345.0) {
        std::fprintf(stderr, "FAIL: \"%s\" should be rejected and leave the output alone\n", text);
        failures++;
    }
}

int main() {
    // Western and southern hemispheres are negative (issue #38).
    accepts("-80.1918", -180.0, 180.0, -80.1918);
    accepts("-33.8688", -90.0, 90.0, -33.8688);
    accepts("25.7617", -90.0, 90.0, 25.7617);
    accepts("+25.7617", -90.0, 90.0, 25.7617);

    // Comma-decimal keypads.
    accepts("25,7617", -90.0, 90.0, 25.7617);
    accepts("-80,1918", -180.0, 180.0, -80.1918);

    // Surrounding whitespace, and the range edges themselves.
    accepts("  12.5 ", -90.0, 90.0, 12.5);
    accepts("-90", -90.0, 90.0, -90.0);
    accepts("180", -180.0, 180.0, 180.0);

    // Out of range.
    rejects("90.0001", -90.0, 90.0);
    rejects("-180.5", -180.0, 180.0);
    rejects("inf", -180.0, 180.0);

    // Not a number, or a number with something after it.
    rejects("", -90.0, 90.0);
    rejects("abc", -90.0, 90.0);
    rejects("nan", -90.0, 90.0);
    rejects("-", -90.0, 90.0);
    rejects("12.5N", -90.0, 90.0);
    rejects("1.2.3", -90.0, 90.0);
    rejects("25,76,17", -90.0, 90.0);
    rejects("-122.419416000000000000000000000000", -180.0, 180.0);  // 35 chars: too long for the parse buffer

    if (failures) {
        std::fprintf(stderr, "coordinate input: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("coordinate input: all passed");
    return 0;
}

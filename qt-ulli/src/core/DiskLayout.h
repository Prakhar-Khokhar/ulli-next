// core/DiskLayout.h
//
// Layout arithmetic for the disk operations.
//
// The installer creates a staging partition in a free region of the
// target disk and afterwards verifies that the region still holds the
// free space that is left over. The arithmetic is platform independent
// on purpose: the layout of a region can be checked without a disk and
// without Windows.

#pragma once

#include <cstdint>
#include <vector>

namespace ulli::core {

// A partition extent on a disk: the range [offset, offset + size).
struct DiskExtent {
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

// The free space inside the region [regionOffset, regionOffset +
// regionSize) that is not covered by any of the given extents.
//
// An extent may reach into the region from either side; only the part
// that lies inside the region is counted. Extents outside the region -
// for example free space elsewhere on the same disk - therefore do not
// influence the result.
inline std::uint64_t freeSpaceInRegion(std::uint64_t regionOffset,
                                        std::uint64_t regionSize,
                                        const std::vector<DiskExtent>& extents) {
    if (regionSize == 0) {
        return 0;
    }
    const std::uint64_t regionEnd = regionOffset + regionSize;
    std::uint64_t used = 0;
    for (const DiskExtent& extent : extents) {
        const std::uint64_t start =
            extent.offset > regionOffset ? extent.offset : regionOffset;
        const std::uint64_t extentEnd = extent.offset + extent.size;
        const std::uint64_t end = extentEnd < regionEnd ? extentEnd : regionEnd;
        if (end > start) {
            used += end - start;
        }
    }
    return regionSize > used ? regionSize - used : 0;
}

// True when the measured free space still matches the expected one.
// The tolerance covers the alignment of the partition boundaries.
inline bool freeSpaceMatches(std::uint64_t measured, std::uint64_t expected,
                              std::uint64_t tolerance) {
    const std::uint64_t difference =
        measured > expected ? measured - expected : expected - measured;
    return difference <= tolerance;
}

}  // namespace ulli::core

#include "pd_party.h"

#include <string.h>

bool pd_party_swap_records(uint32_t party, int stride, int count, int a, int b, pd_read_fn read, pd_write_fn write, void* ctx) {
    if (!party || !read || !write || a == b || a < 0 || b < 0 || a >= count || b >= count || stride <= 0 || stride > 128)
        return false;
    uint8_t first[128], second[128], check[128];
    const uint32_t atA = party + (uint32_t) (a * stride), atB = party + (uint32_t) (b * stride);
    bool haveFirst = read(ctx, atA, first, (size_t) stride);
    bool haveSecond = haveFirst && read(ctx, atB, second, (size_t) stride);
    if (haveSecond && write(ctx, atA, second, (size_t) stride) && write(ctx, atB, first, (size_t) stride) &&
        read(ctx, atA, check, (size_t) stride) && !memcmp(check, second, (size_t) stride) &&
        read(ctx, atB, check, (size_t) stride) && !memcmp(check, first, (size_t) stride))
        return true;
    if (haveFirst) write(ctx, atA, first, (size_t) stride);
    if (haveSecond) write(ctx, atB, second, (size_t) stride);
    return false;
}

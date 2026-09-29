#ifndef PS5VK_DISPATCH_ENCODE_H
#define PS5VK_DISPATCH_ENCODE_H
#include "compute_commands.h"
#include "vk_pipeline.h"
/* PS5 compute profile: 36 CUs, 32 wave32 slots per CU. */
enum { PS5VK_COMPUTE_SCRATCH_WAVES = 1152, PS5VK_SCRATCH_GUARD_BYTES = 16384 };
struct ps5vk_dispatch_encoding {
    const struct ps5vk_compiled_program *program;
    struct ps5vk_compute_addresses addresses;
    uint64_t descriptor_tables[PS5VK_MAX_SETS];
    uint64_t push_constants;
    uint64_t scratch, scratch_bytes;
    uint32_t groups[3];
    uint32_t group_base[3];
    uint64_t completion_value;
};
/* Encodes an owned/mapped GPU dispatch; mapping, submission and waiting belong
 * to the native queue. Rejection never modifies the destination array. */
size_t ps5vk_dispatch_encode(uint32_t *words, size_t capacity,
                            const struct ps5vk_dispatch_encoding *dispatch);
#endif

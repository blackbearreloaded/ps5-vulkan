#ifndef PS5VK_COLOR_RUNTIME_DEFAULTS_H
#define PS5VK_COLOR_RUNTIME_DEFAULTS_H

#include <stdint.h>
#include <string.h>

#include "ps5_agc_registers.h"
#include "ps5_color_target.h"

/* The colour-target register defaults of the running system software.
 *
 * AGC keys its register-default blocks, and the number of blocks changes with
 * the system software: 128 keyed entries (79 context blocks) on 10.x, 137 (84)
 * on 12.02 and 13.x. The MRT0 block has been the same sixteen registers with
 * the same values on each of them, at a different index. It is therefore found
 * by its key and accepted by what it holds: exactly one entry with that key,
 * in the context bank, at an index the table itself covers, whose registers
 * are the sixteen a colour target is built from. A table of any other shape
 * is refused. Returns 0 and fills `out`, or a negative site code. */
static inline int ps5vk_color_runtime_defaults(
    ps5_agc_register out[PS5_COLOR_REGISTER_COUNT],
    const struct ps5_agc_register_defaults *root)
{
    static const uint32_t offsets[PS5_COLOR_REGISTER_COUNT] = {
        0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f, 0x321, 0x323,
        0x324, 0x325, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8,
    };
    const uint32_t mrt0_key = UINT32_C(0x38e92c91);
    uint32_t match = UINT32_MAX, matches = 0u, contexts = 0u;
    if (!out || !root || !root->table_cx || !root->type_index_pairs ||
        root->table_3 || !root->count || root->count > 1024u)
        return -1;
    for (uint32_t i = 0; i < root->count; ++i) {
        const struct ps5_agc_type_index *entry = &root->type_index_pairs[i];
        if (ps5_agc_type_index_bank(entry) == 0u)
            ++contexts;
        if (entry->key != mrt0_key)
            continue;
        if (ps5_agc_type_index_bank(entry) != 0u)
            return -2;
        match = ps5_agc_type_index_value(entry);
        ++matches;
    }
    if (matches != 1u || match >= contexts || !root->table_cx[match])
        return -3;
    for (uint32_t i = 0; i < PS5_COLOR_REGISTER_COUNT; ++i)
        if (root->table_cx[match][i].offset != offsets[i])
            return -4;
    memcpy(out, root->table_cx[match], sizeof(*out) * PS5_COLOR_REGISTER_COUNT);
    return 0;
}

#endif

/* The colour-target defaults are found in the register-default tables of every
 * system software measured so far, and a table of another shape is refused. */
#include "color_runtime_defaults.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const uint32_t offsets[PS5_COLOR_REGISTER_COUNT] = {
    0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f, 0x321, 0x323,
    0x324, 0x325, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8,
};

struct table {
    struct ps5_agc_register_defaults root;
    ps5_agc_register **cx;
    struct ps5_agc_type_index *pairs;
    ps5_agc_register colour[PS5_COLOR_REGISTER_COUNT];
    ps5_agc_register other[PS5_COLOR_REGISTER_COUNT];
};

/* A table like the measured ones: `count` keyed entries, the first `contexts`
 * in the context bank with indices in order, the MRT0 key at `index`. */
static void build(struct table *t, uint32_t count, uint32_t contexts, uint32_t index)
{
    memset(t, 0, sizeof(*t));
    t->cx = calloc(contexts, sizeof(*t->cx));
    t->pairs = calloc(count, sizeof(*t->pairs));
    assert(t->cx && t->pairs);
    for (uint32_t i = 0; i < PS5_COLOR_REGISTER_COUNT; ++i) {
        t->colour[i].offset = offsets[i];
        t->other[i].offset = 0x200u + i;
    }
    t->colour[4].value = 0x48u;
    t->colour[15].value = UINT32_C(0x08c6c000);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t bank = i < contexts ? 0u : 1u + (i & 1u);
        const uint32_t value = i < contexts ? i : i - contexts;
        t->pairs[i].key = UINT32_C(0x10000000) + i;
        t->pairs[i].encoded_index = value << 2 | bank;
        if (i < contexts)
            t->cx[i] = t->other;
    }
    t->pairs[index].key = UINT32_C(0x38e92c91);
    t->cx[index] = t->colour;
    t->root.table_cx = t->cx;
    t->root.table_sh = t->cx;
    t->root.table_uc = t->cx;
    t->root.type_index_pairs = t->pairs;
    t->root.count = count;
}

static void release(struct table *t)
{
    free(t->cx);
    free(t->pairs);
}

int main(void)
{
    ps5_agc_register out[PS5_COLOR_REGISTER_COUNT];
    struct table t;

    /* System software 12.02 and 13.x: 137 entries, 84 context blocks, MRT0 at 78. */
    build(&t, 137, 84, 78);
    assert(ps5vk_color_runtime_defaults(out, &t.root) == 0);
    assert(out[4].value == 0x48u && out[15].value == UINT32_C(0x08c6c000));
    assert(ps5_color_select_runtime_defaults(out, &t.root) == 0);
    release(&t);

    /* System software 10.x: 128 entries, 79 context blocks, MRT0 at 73. */
    build(&t, 128, 79, 73);
    memset(out, 0, sizeof(out));
    assert(ps5vk_color_runtime_defaults(out, &t.root) == 0);
    assert(out[0].offset == 0x318u && out[15].value == UINT32_C(0x08c6c000));
    release(&t);

    /* No table, an empty one, an absurd one, or one with a fourth bank. */
    assert(ps5vk_color_runtime_defaults(out, NULL) == -1);
    build(&t, 128, 79, 73);
    t.root.count = 0;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -1);
    t.root.count = 5000;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -1);
    t.root.count = 128;
    t.root.table_3 = t.cx;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -1);
    t.root.table_3 = NULL;

    /* The key in another bank, twice, or missing. */
    t.pairs[73].encoded_index |= 1u;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -2);
    t.pairs[73].encoded_index &= ~3u;
    t.pairs[10].key = UINT32_C(0x38e92c91);
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -3);
    t.pairs[10].key = UINT32_C(0x1000000a);
    t.pairs[73].key = 1u;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -3);
    t.pairs[73].key = UINT32_C(0x38e92c91);

    /* An index beyond the context blocks, or an empty block. */
    t.pairs[73].encoded_index = 79u << 2;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -3);
    t.pairs[73].encoded_index = 73u << 2;
    t.cx[73] = NULL;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -3);

    /* A block that is not a colour target. */
    t.cx[73] = t.other;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -4);
    t.cx[73] = t.colour;
    t.colour[9].offset = 0x326u;
    assert(ps5vk_color_runtime_defaults(out, &t.root) == -4);
    release(&t);

    puts("color runtime defaults: ok");
    return 0;
}

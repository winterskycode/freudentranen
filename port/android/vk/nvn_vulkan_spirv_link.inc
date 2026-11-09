/* TOTK: stage linking on the SPIR-V. Shaders are translated one stage at a time, so a vertex shader can lack an output
   that the fragment shader it is paired with declares as an input (Ryujinx links the stages when it translates them;
   the Metal backend appends the missing outputs to the vertex shader's MSL). Vulkan requires every fragment input to
   have a matching vertex output, and the Adreno linker rejects the pipeline otherwise. The vertex shader is patched
   instead: each missing location becomes an output of the type the fragment shader reads (spirv_add_outputs). */
/* base: 1 float, 2 signed int, 3 unsigned int (32-bit); count 1..4 */
typedef struct { uint8_t base, count; } spirv_io;

enum { SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23, SPV_OP_TYPE_POINTER = 32,
       SPV_OP_CONSTANT = 43, SPV_OP_CONSTANT_COMPOSITE = 44, SPV_OP_CONSTANT_NULL = 46, SPV_OP_FUNCTION = 54, SPV_OP_VARIABLE = 59, SPV_OP_DECORATE = 71,
       SPV_DECORATION_LOCATION = 30, SPV_DECORATION_COMPONENT = 31, SPV_STORAGE_INPUT = 1, SPV_STORAGE_OUTPUT = 3 };

/* The Location-decorated variables of a storage class: a mask of locations and, per location, the scalar or vector
   type when it is one (otherwise base 0). Variables with a Component decoration are not described (base 0). */
static uint32_t spirv_io_scan(const uint32_t *w, size_t n, uint32_t storage, spirv_io io[32]) {
    if (n < 5) return 0;
    uint32_t bound = w[3], mask = 0;
    if (bound > 1u << 22) return 0;
    int32_t *location = malloc(bound * sizeof *location); uint8_t *component = calloc(bound, 1); uint32_t *type = calloc(bound, sizeof *type);
    if (!location || !component || !type) die("SPIR-V scan allocation");
    for (uint32_t i = 0; i < bound; i++) location[i] = -1;
    /* type[id]: scalars as base | 0x10 (count 1), vectors as base | count << 4, pointers as the pointee id | 0x80000000 with storage in a side pass */
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16; if (!wc || i + wc > n) break;
        if (op == SPV_OP_DECORATE && wc >= 4 && w[i + 1] < bound) {
            if (w[i + 2] == SPV_DECORATION_LOCATION) location[w[i + 1]] = (int32_t)w[i + 3];
            if (w[i + 2] == SPV_DECORATION_COMPONENT) component[w[i + 1]] = 1;
        } else if (op == SPV_OP_TYPE_FLOAT && wc >= 3 && w[i + 1] < bound && w[i + 2] == 32) type[w[i + 1]] = 0x10 | 1;
        else if (op == SPV_OP_TYPE_INT && wc >= 4 && w[i + 1] < bound && w[i + 2] == 32) type[w[i + 1]] = 0x10 | (w[i + 3] ? 2 : 3);
        else if (op == SPV_OP_TYPE_VECTOR && wc >= 4 && w[i + 1] < bound && w[i + 2] < bound && (type[w[i + 2]] & 0xf0) == 0x10 && w[i + 3] >= 2 && w[i + 3] <= 4)
            type[w[i + 1]] = (type[w[i + 2]] & 0xf) | w[i + 3] << 4;
        else if (op == SPV_OP_TYPE_POINTER && wc >= 4 && w[i + 1] < bound && w[i + 2] == storage && w[i + 3] < bound) type[w[i + 1]] = 0x80000000u | w[i + 3];
        else if (op == SPV_OP_VARIABLE && wc >= 4 && w[i + 2] < bound && w[i + 3] == storage && (type[w[i + 1]] & 0x80000000u)) {
            uint32_t id = w[i + 2]; int32_t loc = location[id];
            if (loc >= 0 && loc < 32) {
                uint32_t t = type[type[w[i + 1]] & 0x7fffffff];
                mask |= 1u << loc;
                io[loc] = component[id] || !t ? (spirv_io){0, 0} : (spirv_io){(uint8_t)(t & 0xf), (uint8_t)(t >> 4)};
            }
        }
        i += wc;
    }
    free(location); free(component); free(type);
    return mask;
}

/* A copy of the vertex shader w with an output for every location in missing (described by io): a float vec4 starts at
   (0, 0, 0, 1), as Maxwell reads components a vertex shader leaves unwritten (translator patch 0005), others at zero. */
static uint32_t *spirv_add_outputs(const uint32_t *w, size_t n, const spirv_io io[32], uint32_t missing, size_t *out_words) {
    /* ids: the 32-bit float, int and uint scalar types; vecs: their vectors (SPIR-V forbids declaring a type twice) */
    uint32_t bound = w[3], ids[4] = {0}, vecs[4][5] = {{0}};
    size_t first_type = 0, first_function = 0, entry = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16; if (!wc || i + wc > n) die("SPIR-V patch: malformed module");
        if (op == SPV_OP_ENTRY_POINT && !entry) entry = i;
        /* OpTypeVoid .. OpTypeForwardPointer: the type section starts */
        if (!first_type && op >= 19 && op <= 39) first_type = i;
        if (!first_function && op == SPV_OP_FUNCTION) first_function = i;
        if (op == SPV_OP_TYPE_FLOAT && w[i + 2] == 32) ids[1] = w[i + 1];
        if (op == SPV_OP_TYPE_INT && w[i + 2] == 32) ids[w[i + 3] ? 2 : 3] = w[i + 1];
        if (op == SPV_OP_TYPE_VECTOR && w[i + 3] >= 2 && w[i + 3] <= 4)
            for (int base = 1; base <= 3; base++) if (ids[base] && ids[base] == w[i + 2]) vecs[base][w[i + 3]] = w[i + 1];
        i += wc;
    }
    if (!entry || !first_type || !first_function) die("SPIR-V patch: no entry point, types or function");
    uint32_t *decor = malloc(32 * 4 * sizeof *decor), *glob = malloc((32 * 24 + 16) * sizeof *glob), vars[32]; size_t nd = 0, ng = 0, nv = 0;
    uint32_t f0 = 0, f1 = 0;   /* the float constants 0 and 1, made with the first float vec4 */
    if (!decor || !glob) die("SPIR-V patch allocation");
    for (int base = 1; base <= 3; base++) if (!ids[base]) {
        int used = 0; for (int l = 0; l < 32; l++) used |= (missing >> l & 1) && io[l].base == base;
        if (!used) continue;
        ids[base] = bound++;
        if (base == 1) { glob[ng++] = 3 << 16 | SPV_OP_TYPE_FLOAT; glob[ng++] = ids[base]; glob[ng++] = 32; }
        else { glob[ng++] = 4 << 16 | SPV_OP_TYPE_INT; glob[ng++] = ids[base]; glob[ng++] = 32; glob[ng++] = base == 2; }
    }
    for (int l = 0; l < 32; l++) if (missing >> l & 1) {
        uint32_t t = ids[io[l].base];
        if (io[l].count > 1) {
            uint32_t *v = &vecs[io[l].base][io[l].count];
            if (!*v) { *v = bound++; glob[ng++] = 4 << 16 | SPV_OP_TYPE_VECTOR; glob[ng++] = *v; glob[ng++] = t; glob[ng++] = io[l].count; }
            t = *v;
        }
        uint32_t ptr = bound++, zero = bound++, var = bound++;
        glob[ng++] = 4 << 16 | SPV_OP_TYPE_POINTER; glob[ng++] = ptr; glob[ng++] = SPV_STORAGE_OUTPUT; glob[ng++] = t;
        if (io[l].base == 1 && io[l].count == 4) {
            if (!f1) { f0 = bound++; f1 = bound++;
                glob[ng++] = 4 << 16 | SPV_OP_CONSTANT; glob[ng++] = ids[1]; glob[ng++] = f0; glob[ng++] = 0;
                glob[ng++] = 4 << 16 | SPV_OP_CONSTANT; glob[ng++] = ids[1]; glob[ng++] = f1; glob[ng++] = 0x3f800000u; }
            glob[ng++] = 7 << 16 | SPV_OP_CONSTANT_COMPOSITE; glob[ng++] = t; glob[ng++] = zero; glob[ng++] = f0; glob[ng++] = f0; glob[ng++] = f0; glob[ng++] = f1;
        } else { glob[ng++] = 3 << 16 | SPV_OP_CONSTANT_NULL; glob[ng++] = t; glob[ng++] = zero; }
        glob[ng++] = 5 << 16 | SPV_OP_VARIABLE; glob[ng++] = ptr; glob[ng++] = var; glob[ng++] = SPV_STORAGE_OUTPUT; glob[ng++] = zero;
        decor[nd++] = 4 << 16 | SPV_OP_DECORATE; decor[nd++] = var; decor[nd++] = SPV_DECORATION_LOCATION; decor[nd++] = (uint32_t)l;
        vars[nv++] = var;
    }
    size_t total = n + nd + ng + nv;
    uint32_t *o = malloc(total * sizeof *o); if (!o) die("SPIR-V patch allocation");
    size_t k = 0;
    for (size_t i = 0; i < n;) {
        if (i < 5) { o[k++] = w[i++]; continue; }
        uint32_t wc = w[i] >> 16;
        if (i == first_type) { memcpy(o + k, decor, nd * sizeof *o); k += nd; }
        if (i == first_function) { memcpy(o + k, glob, ng * sizeof *o); k += ng; }
        if (i == entry) {
            o[k++] = (wc + nv) << 16 | SPV_OP_ENTRY_POINT; memcpy(o + k, w + i + 1, (wc - 1) * sizeof *o); k += wc - 1;
            memcpy(o + k, vars, nv * sizeof *o); k += nv;
        } else { memcpy(o + k, w + i, wc * sizeof *o); k += wc; }
        i += wc;
    }
    o[3] = bound; free(decor); free(glob);
    *out_words = k; return o;
}

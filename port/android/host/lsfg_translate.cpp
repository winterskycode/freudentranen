// The shader table and the DXBC translation follow lsfg-vk v1.0.0 (MIT, Copyright (c) 2025 lsfg-vk,
// https://github.com/PancakeTAS/lsfg-vk/tree/v1.0.0/src/extract). pe-parse reads the DLL's resources; DXVK's DXBC compiler
// makes the SPIR-V (vk/third_party/pe-parse, vk/third_party/dxbc). The DLL and the shaders are the player's licensed data:
// they are never part of this repository or the APK.
#include "lsfg_translate.h"
#include <pe-parse/parse.h>
#include <dxbc_modinfo.h>
#include <dxbc_module.h>
#include <dxbc_reader.h>
#include <thirdparty/spirv.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <vector>

namespace {

// name: the file the framegen library loads; resource: its RCDATA id in the DLL; bindings: the descriptors the library
// declares for it (vk/third_party/lsfg_framegen, getShader). The ids belong to one layout of the DLL: another version can
// hold other shaders under them, which translate well and then fail in the game, so the bindings are checked here.
struct Shader { const char *name; uint32_t resource, bindings; };
const Shader kShaders[] = {
    {"mipmaps", 255, 10}, {"generate", 256, 9},
    {"alpha[0]", 267, 4}, {"alpha[1]", 268, 5}, {"alpha[2]", 269, 7}, {"alpha[3]", 270, 9},
    {"beta[0]", 275, 15}, {"beta[1]", 276, 5}, {"beta[2]", 277, 5}, {"beta[3]", 278, 5}, {"beta[4]", 279, 10},
    {"gamma[0]", 257, 15}, {"gamma[1]", 259, 8}, {"gamma[2]", 260, 9}, {"gamma[3]", 261, 9}, {"gamma[4]", 262, 10},
    {"delta[0]", 257, 15}, {"delta[1]", 263, 8}, {"delta[2]", 264, 9}, {"delta[3]", 265, 9}, {"delta[4]", 266, 10},
    {"delta[5]", 258, 15}, {"delta[6]", 271, 5}, {"delta[7]", 272, 5}, {"delta[8]", 273, 5}, {"delta[9]", 274, 7},
    // performance mode
    {"p_mipmaps", 255, 10}, {"p_generate", 256, 9},
    {"p_alpha[0]", 290, 3}, {"p_alpha[1]", 291, 3}, {"p_alpha[2]", 292, 4}, {"p_alpha[3]", 293, 5},
    {"p_beta[0]", 298, 9}, {"p_beta[1]", 299, 5}, {"p_beta[2]", 300, 5}, {"p_beta[3]", 301, 5}, {"p_beta[4]", 302, 10},
    {"p_gamma[0]", 280, 11}, {"p_gamma[1]", 282, 6}, {"p_gamma[2]", 283, 5}, {"p_gamma[3]", 284, 5}, {"p_gamma[4]", 285, 8},
    {"p_delta[0]", 280, 11}, {"p_delta[1]", 286, 6}, {"p_delta[2]", 287, 5}, {"p_delta[3]", 288, 5}, {"p_delta[4]", 289, 8},
    {"p_delta[5]", 281, 10}, {"p_delta[6]", 294, 3}, {"p_delta[7]", 295, 3}, {"p_delta[8]", 296, 3}, {"p_delta[9]", 297, 6},
};
static_assert(sizeof kShaders / sizeof kShaders[0] == LSFG_SHADER_COUNT);

const char kNotDll[] = "This file is not Lossless.dll.";
const char kOtherVersion[] = "This Lossless.dll is damaged, or a version the app cannot use.";
const char kNotSaved[] = "The shaders could not be saved. The device may be out of space.";

using Resources = std::map<uint32_t, std::vector<uint8_t>>;

int on_resource(void *resources, const peparse::resource &res) {
    if (res.type == peparse::RT_RCDATA && res.buf && res.buf->bufLen > 0)
        (*static_cast<Resources *>(resources))[res.name] = std::vector<uint8_t>(res.buf->buf, res.buf->buf + res.buf->bufLen);
    return 0;
}

void md5_block(uint32_t h[4], const uint8_t *block) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const uint8_t S[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    std::memcpy(m, block, sizeof m);
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        switch (i / 16) {
            case 0: f = (b & c) | (~b & d); g = i; break;
            case 1: f = (d & b) | (~d & c); g = (5 * i + 1) % 16; break;
            case 2: f = b ^ c ^ d; g = (3 * i + 5) % 16; break;
            default: f = c ^ (b | ~d); g = (7 * i) % 16; break;
        }
        uint32_t x = a + f + K[i] + m[g]; int s = S[i / 16][i % 4];
        a = d; d = c; c = b; b += x << s | x >> (32 - s);
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

// A DXBC container's checksum at bytes 4..19: MD5 of what follows it, with the length placed differently at the end. Every
// real shader has it right. The compiler trusts its input, so a damaged shader could crash it (no exception to catch: an
// index out of range, gigabytes asked for) or translate into nonsense; this stops those before it starts.
bool dxbc_intact(const uint8_t *dxbc, size_t size) {
    const uint8_t *p = dxbc + 20; size_t n = size - 20, whole = n / 64 * 64, left = n - whole;
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, bits = static_cast<uint32_t>(n * 8), tail = bits >> 2 | 1;
    for (size_t i = 0; i < whole; i += 64) md5_block(h, p + i);
    uint8_t b[64] = {};
    if (left >= 56) {
        std::memcpy(b, p + whole, left); b[left] = 0x80; md5_block(h, b);
        std::memset(b, 0, sizeof b); std::memcpy(b, &bits, 4);
    } else { std::memcpy(b, &bits, 4); std::memcpy(b + 4, p + whole, left); b[4 + left] = 0x80; }
    std::memcpy(b + 60, &tail, 4); md5_block(h, b);
    return !std::memcmp(h, dxbc + 4, sizeof h);
}

// DXBC compute shader -> SPIR-V; bindings renumbered 0..n-1 in declaration order (what the lsfg framegen library expects).
// Throws on anything else: DxvkError (not a std::exception) from the compiler, std::exception from what it allocates.
std::vector<uint8_t> translate(const std::vector<uint8_t> &dxbc, uint32_t expected_bindings) {
    // the container's own size: a resource can be padded past it
    uint32_t size = 0;
    if (dxbc.size() >= 32) std::memcpy(&size, dxbc.data() + 24, 4);
    if (size < 32 || size > dxbc.size() || std::memcmp(dxbc.data(), "DXBC", 4) || !dxbc_intact(dxbc.data(), size)) throw dxvk::DxvkError("damaged DXBC");
    dxvk::DxbcReader reader(reinterpret_cast<const char *>(dxbc.data()), size);
    dxvk::DxbcModule module(reader);
    auto program = module.programInfo();
    if (!program || program->type() != dxvk::DxbcProgramType::ComputeShader) throw dxvk::DxvkError("not a compute shader");
    const dxvk::DxbcModuleInfo info{};
    auto code = module.compile(info, "CS");
    std::vector<uint32_t> binding_offsets;
    for (auto ins : code) {
        if (ins.opCode() == spv::OpDecorate && ins.arg(2) == spv::DecorationBinding) binding_offsets.push_back(ins.offset() + 3);
        if (ins.opCode() == spv::OpFunction) break;
    }
    if (binding_offsets.size() != expected_bindings) throw dxvk::DxvkError("not the shader the table names");
    for (size_t i = 0; i < binding_offsets.size(); i++) code.data()[binding_offsets[i]] = static_cast<uint32_t>(i);
    std::vector<uint8_t> out(code.size());
    std::copy_n(reinterpret_cast<const uint8_t *>(code.data()), code.size(), out.data());
    return out;
}

std::string run(const char *dll, const char *out_dir, lsfg_progress progress, void *user) {
    Resources resources;
    peparse::parsed_pe *pe = peparse::ParsePEFromFile(dll);
    if (!pe) return kNotDll;
    peparse::IterRsrc(pe, on_resource, &resources);
    peparse::DestructParsedPE(pe);
    if (resources.empty()) return kNotDll;
    int done = 0;
    for (const Shader &s : kShaders) {
        auto hit = resources.find(s.resource);
        if (hit == resources.end()) { std::fprintf(stderr, "[lsfg] %s: resource %u is not in the DLL\n", s.name, s.resource); return kOtherVersion; }
        std::vector<uint8_t> spirv;
        try { spirv = translate(hit->second, s.bindings); }
        catch (const dxvk::DxvkError &e) { std::fprintf(stderr, "[lsfg] %s: %s\n", s.name, e.message().c_str()); return kOtherVersion; }
        catch (const std::exception &e) { std::fprintf(stderr, "[lsfg] %s: %s\n", s.name, e.what()); return kOtherVersion; }
        std::ofstream f(std::string(out_dir) + "/" + s.name + ".spv", std::ios::binary);
        f.write(reinterpret_cast<const char *>(spirv.data()), static_cast<std::streamsize>(spirv.size()));
        f.close();
        if (!f) return kNotSaved;
        if (progress) progress(user, ++done, LSFG_SHADER_COUNT);
    }
    return "";
}

}

std::string lsfg_translate(const char *dll, const char *out_dir, lsfg_progress progress, void *user) noexcept {
    try { return run(dll, out_dir, progress, user); }
    catch (...) { return kOtherVersion; }
}

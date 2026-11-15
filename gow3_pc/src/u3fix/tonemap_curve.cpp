// Uncharted 3 1.02, 15 Nov 2026: the render settings' "improved" tonemap curve, used by PS3 and dropped by the PS4 build.
//
// The tonemap block of the render settings holds two curves: the filmic curve (+0x710 white point, +0x714..+0x728 its
// strengths, +0x72c filmic exposure bias) and the improved curve (+0x730 use-improved-curve, +0x734 improved exposure
// bias, +0x738 black point, +0x73c white point). PS3 1.19 builds its tonemap constants from the improved curve whenever
// the curve type (+0x70c) is filmic and use-improved-curve is not below one half: the Hejl / Burgess-Dawson filmic curve
// v (6.2 v + 0.5) / (v (6.2 v + 1.7) + 0.06), divided by the improved white point, of the colour after the levels scaled
// by 2^improved-exposure-bias, less the black point; the filmic fields are not used then. The PS4 build only has the
// filmic curve and never reads the four fields. No PS4 campaign level sets them, but PS3 multiplayer maps do: Museum has
// improved bias -4.1 and filmic bias -1.4, so the port showed it about 2.7 stops brighter than PS3, with the filmic
// shoulder instead of the improved one (also Ruins, Shambhala and Warzone variants, the co-op stage).
//
// The PS4 tonemap pixel shader evaluates y = c4 + (c5 x + c6) / (x^2 + c0 x + c1) with x = levels scale * colour +
// levels offset (c36..c38, c40..c42), the same form PS3 fills with either curve, so the improved curve needs only other
// constants. The constant builder (image 0xed9bd0) stores the curve (c0..c6) first and the levels (c36..c43) last; the fix
// runs right after that last store and, when the curve applies, rewrites c0, c1, c4..c6 and folds the exposure and the
// black point into the levels scale and offset, as PS3 does. It does nothing while the game's "T1 style filmic curve"
// debug switch is off (the builder then uses a fixed curve of its own instead of the render settings).
// UNCHARTED_IMPROVED_TONEMAP=0 leaves the PS4 behaviour (filmic curve only).
#include "u3fix.h"
#include "u3fix_hook.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace u3fix {

bool improved_tonemap_curve(float *constants, const unsigned char *settings, bool t1_curve) {
    uint32_t curve_type;
    float use, bias, black, white;
    std::memcpy(&curve_type, settings + 0x70c, 4);
    std::memcpy(&use, settings + 0x730, 4);
    std::memcpy(&bias, settings + 0x734, 4);
    std::memcpy(&black, settings + 0x738, 4);
    std::memcpy(&white, settings + 0x73c, 4);
    if (!t1_curve || curve_type != 1 || use < 0.5f) return false;
    // the Hejl / Burgess-Dawson curve with its x^2 term divided out: v (v + a) / (v^2 + b v + k) = 1 + ((a - b) v - k) / (...)
    const float a = 0.5f / 6.2f, b = 1.7f / 6.2f, k = 0.06f / 6.2f;
    constants[0] = b;
    constants[1] = k;
    constants[4] = 1.0f / white;
    constants[5] = (a - b) / white;
    constants[6] = -k / white;
    const float scale = std::exp2(bias);
    for (int i = 0; i < 3; i++) {
        constants[36 + i] *= scale;
        constants[40 + i] -= black;
    }
    return true;
}

namespace {

std::atomic<uint64_t> g_frames{0};
std::atomic<uint32_t> g_last{0}, g_reports{0};

// called from the stub with the constant block (r12), the render settings (rbx) and the T1 switch (r15b)
__attribute__((sysv_abi)) void tonemap_hook(float *constants, const unsigned char *settings, uint32_t t1_curve) {
    if (!constants || !settings || !improved_tonemap_curve(constants, settings, (t1_curve & 0xff) != 0)) return;
    g_frames.fetch_add(1, std::memory_order_relaxed);
    uint32_t bias_bits;
    std::memcpy(&bias_bits, settings + 0x734, 4);
    if (g_last.exchange(bias_bits, std::memory_order_relaxed) != bias_bits && g_reports.fetch_add(1) < 20) {
        float bias, black, white;
        std::memcpy(&bias, settings + 0x734, 4);
        std::memcpy(&black, settings + 0x738, 4);
        std::memcpy(&white, settings + 0x73c, 4);
        std::printf("Uncharted 3: improved tonemap curve (as on PS3): exposure bias %.3f, black %.4f, white %.3f\n",
                    bias, black, white);
        std::fflush(stdout);
    }
}

// the levels stores at the end of the constant builder: vmovaps [r12+0x90], xmm3; the offset lanes; vmovaps [r12+0xa0],
// xmm0; vmovss xmm1, [r13+0x7d8]
constexpr unsigned char kSite[] = {
    0xc4, 0xc1, 0x78, 0x29, 0x9c, 0x24, 0x90, 0x00, 0x00, 0x00, 0xc4, 0xe3, 0x79, 0x04, 0xc0, 0x15, 0xc4, 0xe3, 0x79, 0x04,
    0xc9, 0x15, 0xc4, 0xe3, 0x79, 0x04, 0xd2, 0x15, 0xc5, 0xf8, 0x15, 0xc2, 0xc5, 0xf1, 0x73, 0xd9, 0x04, 0xc5, 0xf8, 0x15,
    0xc1, 0xc4, 0xc1, 0x78, 0x29, 0x84, 0x24, 0xa0, 0x00, 0x00, 0x00, 0xc4, 0xc1, 0x7a, 0x10, 0x8d, 0xd8, 0x07, 0x00, 0x00};
constexpr unsigned kStore = 41;       // offset of vmovaps [r12+0xa0], xmm0 in kSite
constexpr unsigned kStoreLength = 10;

void put32(unsigned char *p, unsigned &n, int32_t v) { std::memcpy(p + n, &v, 4); n += 4; }

// mov [rsp+disp], r64 (store) or mov r64, [rsp+disp] (load); reg 0..15
void gpr_slot(unsigned char *p, unsigned &n, unsigned reg, int32_t disp, bool store) {
    p[n++] = static_cast<unsigned char>(0x48 | (reg >= 8 ? 4 : 0));
    p[n++] = store ? 0x89 : 0x8b;
    p[n++] = static_cast<unsigned char>(0x84 | (reg & 7) << 3);
    p[n++] = 0x24;
    put32(p, n, disp);
}

// movups [rsp+disp], xmmN (store) or movups xmmN, [rsp+disp] (load)
void xmm_slot(unsigned char *p, unsigned &n, unsigned reg, int32_t disp, bool store) {
    if (reg >= 8) p[n++] = 0x44;
    p[n++] = 0x0f;
    p[n++] = store ? 0x11 : 0x10;
    p[n++] = static_cast<unsigned char>(0x84 | (reg & 7) << 3);
    p[n++] = 0x24;
    put32(p, n, disp);
}

}  // namespace

bool install_improved_tonemap_curve(unsigned char *image, uint64_t size) {
    if (const char *env = std::getenv("UNCHARTED_IMPROVED_TONEMAP"); env && env[0] == '0') return false;   // A/B: off
    const uint64_t site = find_unique(image, size, kSite, sizeof(kSite));
    if (site == kNotFound) return false;
    unsigned char *stub = stub_page_near(image, size);
    if (!stub) return false;
    const uint64_t at = site + kStore;
    // The stub stores the levels offsets as the replaced instruction did, steps over the 128 bytes below the stack
    // pointer (PS4 code may keep values there), keeps every register the System V convention lets the handler change
    // (rax, rcx, rdx, rsi, rdi, r8-r11, xmm0-15), calls the handler on an aligned stack and jumps back.
    static const unsigned kSaved[] = {0, 1, 2, 6, 7, 8, 9, 10, 11};   // rax rcx rdx rsi rdi r8 r9 r10 r11
    unsigned n = 0;
    std::memcpy(stub + n, image + at, kStoreLength); n += kStoreLength;      // vmovaps [r12+0xa0], xmm0
    const unsigned char enter[] = {0x48, 0x8d, 0x64, 0x24, 0x80,             // lea rsp, [rsp-0x80]
                                   0x55,                                     // push rbp
                                   0x48, 0x89, 0xe5,                         // mov rbp, rsp
                                   0x48, 0x83, 0xe4, 0xf0,                   // and rsp, -16
                                   0x48, 0x81, 0xec, 0x80, 0x01, 0x00, 0x00};  // sub rsp, 0x180
    std::memcpy(stub + n, enter, sizeof enter); n += sizeof enter;
    for (unsigned i = 0; i < 16; i++) xmm_slot(stub, n, i, static_cast<int32_t>(16 * i), true);
    for (unsigned i = 0; i < 9; i++) gpr_slot(stub, n, kSaved[i], static_cast<int32_t>(0x100 + 8 * i), true);
    const unsigned char args[] = {0x4c, 0x89, 0xe7,         // mov rdi, r12
                                  0x48, 0x89, 0xde,         // mov rsi, rbx
                                  0x41, 0x0f, 0xb6, 0xd7};  // movzx edx, r15b
    std::memcpy(stub + n, args, sizeof args); n += sizeof args;
    const uint64_t handler = reinterpret_cast<uint64_t>(&tonemap_hook);
    stub[n++] = 0x48; stub[n++] = 0xb8; std::memcpy(stub + n, &handler, 8); n += 8;   // mov rax, handler
    stub[n++] = 0xff; stub[n++] = 0xd0;                                              // call rax
    for (unsigned i = 0; i < 9; i++) gpr_slot(stub, n, kSaved[i], static_cast<int32_t>(0x100 + 8 * i), false);
    for (unsigned i = 0; i < 16; i++) xmm_slot(stub, n, i, static_cast<int32_t>(16 * i), false);
    const unsigned char leave[] = {0x48, 0x89, 0xec,                                // mov rsp, rbp
                                   0x5d,                                            // pop rbp
                                   0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};  // lea rsp, [rsp+0x80]
    std::memcpy(stub + n, leave, sizeof leave); n += sizeof leave;
    stub[n++] = 0xff; stub[n++] = 0x25; std::memset(stub + n, 0, 4); n += 4;          // jmp [rip]
    const uint64_t back = reinterpret_cast<uint64_t>(image + at + kStoreLength);
    std::memcpy(stub + n, &back, 8); n += 8;
    if (!finish_stub(stub) || !jump_to(image, at, kStoreLength, stub)) return false;
    std::printf("Uncharted 3: improved tonemap curve of the render settings at the constant builder %#llx\n",
                static_cast<unsigned long long>(at));
    std::fflush(stdout);
    return true;
}

}  // namespace u3fix

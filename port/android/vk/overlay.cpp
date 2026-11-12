// In-game overlay: the quick menu, the settings screen, the button layout and the performance HUD, drawn with Dear ImGui
// (third_party/imgui, MIT) into the presented image. See overlay.h. ImGui moves the focus and reports activations; every
// row, tab and ornament is drawn here.
#ifndef IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#endif
#include "overlay.h"
#include <sys/resource.h>
#include "frame_rule.h"
#include "game_actions.h"
#include "../host/lsfg_translate.h"
#include <algorithm>
#include <dirent.h>
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <vector>

namespace {

/* The colours: ink ground, cream text, gold for the cursor, jade for values, ochre for restart-only. */
const ImU32 kInk = IM_COL32(10, 17, 20, 255), kCream = IM_COL32(238, 232, 216, 255), kMuted = IM_COL32(167, 174, 162, 255);
const ImU32 kSoft = IM_COL32(207, 201, 184, 255), kHelp = IM_COL32(214, 208, 192, 255), kGold = IM_COL32(216, 180, 92, 255);
const ImU32 kGoldText = IM_COL32(235, 203, 124, 255), kGoldFrame = IM_COL32(216, 180, 92, 178), kGoldFill = IM_COL32(216, 180, 92, 26);
const ImU32 kJade = IM_COL32(111, 230, 195, 255), kOchre = IM_COL32(229, 154, 114, 255), kOchreText = IM_COL32(240, 180, 148, 255);
const ImU32 kLine = IM_COL32(238, 232, 216, 36), kPanel = IM_COL32(12, 20, 23, 220);

/* ---- the settings: assets/settings.tsv (the startup screen reads the same table) ---- */
struct Option {
    std::string key, tab, flags, label, help, needs_key; std::vector<std::string> values, names, needs; int def = 0, cur = 0; bool touched = false;
    bool has(char f) const { return flags.find(f) != std::string::npos; }
    bool live() const { return has('L'); }
    bool page() const { return has('P'); }
    bool heading() const { return has('H'); }
};
std::vector<Option> g_options; std::vector<std::string> g_tabs;
Option *option(const char *key) { for (Option &o : g_options) if (o.key == key) return &o; return nullptr; }
/* A row shows while the setting it needs (settings.tsv: needs) has one of its values */
bool shown(const Option &o) {
    if (o.needs_key.empty()) return true;
    const Option *n = option(o.needs_key.c_str()); if (!n || n->values.empty()) return true;
    for (const std::string &v : o.needs) if (v == n->values[n->cur]) return true;
    return false;
}
std::vector<std::string> split(const std::string &s, char c) {
    std::vector<std::string> out; size_t from = 0;
    for (size_t at; (at = s.find(c, from)) != std::string::npos; from = at + 1) out.push_back(s.substr(from, at - from));
    out.push_back(s.substr(from)); return out;
}

/* ---- the button layout (host/controller_map.h order) ---- */
enum { BUTTONS = 17, SWITCH_BUTTONS = 16 };
/* where each face row's button sits on the drawing, by label */
const int kLabelPosition[4] = {1, 0, 3, 2};
/* The actions' help, the buttons an action can go on (in the order left / right steps through them) and the buttons'
   short names, from assets/actions.tsv: the startup screen shows the same table (load_actions) */
std::string g_action_help[GAME_ACTIONS], g_short[BUTTONS], g_label_short[4];
std::vector<int> g_carry;
/* the physical button the game's own layout puts an action on (host/app.c physical_of_switch) */
int action_default(int a) { return game_physical_of_switch(game_actions[a].button); }
/* The drawing of a handheld (design units, inside the 470 x 300 panel): x, y, w, h, radius, label */
struct PadButton { float x, y, w, h, r; const char *text; };
const PadButton kPad[BUTTONS] = {
    {362, 150, 28, 28, 14, ""}, {392, 120, 28, 28, 14, ""}, {332, 120, 28, 28, 14, ""}, {362, 90, 28, 28, 14, ""},
    {66, 96, 52, 52, 26, "L3"}, {300, 190, 46, 46, 23, "R3"}, {44, 34, 76, 20, 10, "L1"}, {350, 34, 76, 20, 10, "R1"},
    {44, 10, 76, 20, 10, "L2"}, {350, 10, 76, 20, 10, "R2"}, {246, 98, 30, 22, 6, "+"}, {194, 98, 30, 22, 6, "-"},
    {112, 196, 22, 22, 4, ""}, {134, 174, 22, 22, 4, ""}, {156, 196, 22, 22, 4, ""}, {134, 218, 22, 22, 4, ""}, {0, 0, 0, 0, 0, ""}};
struct Framebuffer { VkImage image; VkImageView view; VkFramebuffer fb; uint32_t w, h; };
enum { PAGE_QUICK, PAGE_SETTINGS, PAGE_CONTROLS };
struct State {
    overlay_vk vk; overlay_settings set; bool configured = false;
    char files_dir[512] = "", info[512] = "";
    bool ready = false, failed = false; VkFormat format = VK_FORMAT_UNDEFINED;
    VkRenderPass pass = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE;
    Framebuffer fbs[8] = {}; unsigned next_fb = 0;
    /* mono: the fps number (digits of one width: a steady readout) */
    ImFont *title = nullptr, *head = nullptr, *body = nullptr, *value = nullptr, *help = nullptr, *small = nullptr, *tag = nullptr, *mono = nullptr;
    std::atomic<bool> open{false};
    /* overlay_toggle from other threads, applied by the drawing thread (overlay_record) */
    std::atomic<int> toggles{0};
    int page = PAGE_QUICK, controls_back = PAGE_QUICK, tab = 0, focused = -1, pad_row = 0, rows = 0, row_focused = -1, loop_to = -1; bool focus_first = false, changed_restart = false;
    /* the bug report's progress (host/report.c) */
    char report[300] = "";
    /* the report row's choice: with a frame capture */
    bool report_capture = false;
    /* the field controls: each action's physical button */
    int8_t actions[GAME_ACTIONS] = {};
    float frame_ms[120] = {}; int frame_i = 0;
    /* HUD: the driver line and the shader counters */
    char driver[160] = ""; overlay_stats st = {};
    /* screen pixels per design unit (the design is 1280 x 720) */
    float s = 1.5f;
    pthread_mutex_t input = PTHREAD_MUTEX_INITIALIZER;
    uint32_t buttons = 0, seen = 0, edges = 0, pressed = 0; int32_t lx = 0, ly = 0;
    struct Touch { float x, y; int down; } touches[16]; unsigned ntouch = 0;
    uint64_t last_ns = 0;
    std::atomic<bool> preparing{false}; pthread_mutex_t prep_lock = PTHREAD_MUTEX_INITIALIZER; char prep_stage[160] = "", prep_detail[240] = ""; float prep_frac = -1;
} S;
enum { BTN_A = 1u << 0, BTN_B = 1u << 1, BTN_X = 1u << 2, BTN_Y = 1u << 3, BTN_L = 1u << 6, BTN_R = 1u << 7 };

uint64_t now_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
PFN_vkVoidFunction load(const char *name, void *) { return S.vk.proc(name, nullptr); }
#define VKF(name) reinterpret_cast<PFN_##name>(S.vk.proc(#name, nullptr))

/* LSFG's shaders are there: the startup screen makes them from the player's Lossless.dll (Lsfg.java does the same count) */
bool lsfg_ready() {
    const char *files = getenv("TOTK_FILES_DIR"); char dir[600]; int n = 0;
    if (!files || snprintf(dir, sizeof dir, "%s/lsfg", files) >= (int)sizeof dir) return false;
    DIR *d = opendir(dir); if (!d) return false;
    while (const dirent *e = readdir(d)) { size_t l = strlen(e->d_name); n += l > 4 && !strcmp(e->d_name + l - 4, ".spv"); }
    closedir(d);
    return n >= LSFG_SHADER_COUNT;
}

/* The table, then the starting values: the process environment (launch.env / debug.env), else each row's default.
   Values starting with lsfg are left out without the shaders, so the menu cannot pick what would only fail. */
void load_options() {
    size_t n = 0; char *text = S.set.asset ? (char *)S.set.asset("settings.tsv", &n) : nullptr;
    if (!text) { fprintf(stderr, "[overlay] assets/settings.tsv missing: no settings in the menu\n"); return; }
    char turnip[600]; const char *libdir = getenv("DM_ANDROID_LIBDIR");
    bool has_turnip = libdir && snprintf(turnip, sizeof turnip, "%s/libvulkan_freedreno.so", libdir) < (int)sizeof turnip && !access(turnip, F_OK);
    bool has_lsfg = lsfg_ready();
    for (const std::string &line : split(text, '\n')) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f = split(line, '\t');
        if (f.size() < 8) { fprintf(stderr, "[overlay] settings.tsv: a row with %zu fields\n", f.size()); continue; }
        Option o; o.key = f[0]; o.tab = f[1]; o.flags = f[2]; o.label = f[6]; o.help = f[7];
        if (o.has('S') || (o.has('D') && !has_turnip)) continue;
        if (f.size() > 8) { size_t eq = f[8].find('='); if (eq != std::string::npos) { o.needs_key = f[8].substr(0, eq); o.needs = split(f[8].substr(eq + 1), '|'); } }
        if (!o.page() && !o.heading()) {
            o.values = split(f[4], '|'); o.names = split(f[5], '|');
            if (o.values.size() != o.names.size() || o.values.empty()) { fprintf(stderr, "[overlay] settings.tsv: %s values and names differ\n", o.key.c_str()); continue; }
            if (!has_lsfg) for (size_t i = o.values.size(); i-- > 0; ) if (!o.values[i].compare(0, 4, "lsfg")) { o.values.erase(o.values.begin() + i); o.names.erase(o.names.begin() + i); }
            for (size_t i = 0; i < o.values.size(); i++) if (o.values[i] == f[3]) o.def = (int)i;
            o.cur = o.def;
            const char *v = getenv(o.key.c_str());
            if (v) for (size_t i = 0; i < o.values.size(); i++) if (o.values[i] == v) o.cur = (int)i;
        }
        bool known = false; for (const std::string &t : g_tabs) known |= t == o.tab;
        if (!known) g_tabs.push_back(o.tab);
        g_options.push_back(o);
    }
    free(text);
}
/* assets/actions.tsv (its header says the format): the help, button order and names the Controls page shows; the rest
   of each action row is checked against game_actions.h, which is what the game gets */
void load_actions() {
    size_t n = 0; char *text = S.set.asset ? (char *)S.set.asset("actions.tsv", &n) : nullptr;
    if (!text) { fprintf(stderr, "[overlay] assets/actions.tsv missing: no help on the Controls page\n"); return; }
    int a = 0;
    for (const std::string &line : split(text, '\n')) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f = split(line, '\t');
        if (f[0] == "action" && f.size() >= 6 && a < GAME_ACTIONS) {
            const game_action &g = game_actions[a];
            if (f[1] != g.name || !g.label || f[2] != g.label || atoi(f[3].c_str()) != g.button || (f[4] == "1") != (g.held != 0))
                fprintf(stderr, "[overlay] assets/actions.tsv: action %d (%s) differs from game_actions.h (%s)\n", a, f[1].c_str(), g.name);
            g_action_help[a++] = f[5];
        } else if (f[0] == "carry" && f.size() >= 2) {
            g_carry.clear(); for (const std::string &v : split(f[1], ' ')) if (!v.empty()) g_carry.push_back(atoi(v.c_str()));
        } else if (f[0] == "switch" && f.size() >= 2) {
            int b = 0; for (const std::string &v : split(f[1], ' ')) if (!v.empty() && b < 4 && atoi(v.c_str()) != game_physical_of_switch(b++))
                fprintf(stderr, "[overlay] assets/actions.tsv: switch row differs from game_actions.h\n");
        } else if (f[0] == "button" && f.size() >= 3) {
            int i = atoi(f[1].c_str()); if (i >= 0 && i < BUTTONS) { g_short[i] = f[2]; if (i < 4 && f.size() >= 4) g_label_short[i] = f[3]; }
        } else fprintf(stderr, "[overlay] assets/actions.tsv: a row not understood: %s\n", line.c_str());
    }
    if (a + 1 != GAME_ACTIONS) fprintf(stderr, "[overlay] assets/actions.tsv: %d actions, game_actions.h has %d\n", a, GAME_ACTIONS - 1);
    free(text);
}
/* The settings changed in the menu to <files>/overlay.env (the startup screen imports it). Only those: the starting values
   come from the environment, which includes developer overrides (debug.env) that must not become saved settings. */
void save_options() {
    if (!S.files_dir[0]) return;
    char path[600], tmp[620]; snprintf(path, sizeof path, "%s/overlay.env", S.files_dir); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "# written by the in-game menu; the startup screen imports and removes this file\n");
    for (const Option &o : g_options) if (o.touched && !o.heading()) fprintf(f, "%s=%s\n", o.key.c_str(), o.values[o.cur].c_str());
    fclose(f); rename(tmp, path);
}
/* The frame rate settings live (frame_rule.h, as at start), applied as the backend's own: the game's cap first
   (TOTK_UNCAPPED_HZ), then TOTK_FRAMEGEN. */
void frame_apply() {
    Option *cap = option("TOTK_FPS_CAP"), *fg = option("TOTK_FG"), *mode = option("TOTK_FG_MODE");
    if (!cap || !fg || !S.set.apply) return;
    char hz[16], framegen[32]; frame_rule(cap->values[cap->cur].c_str(), fg->values[fg->cur].c_str(), mode ? mode->values[mode->cur].c_str() : nullptr, hz, framegen);
    S.set.apply("TOTK_UNCAPPED_HZ", hz); S.set.apply("TOTK_FRAMEGEN", framegen);
}
void set_value(Option &o, int index) {
    int n = (int)o.values.size(); index = (index % n + n) % n;
    if (index == o.cur) return;
    o.cur = index; o.touched = true;
    if (o.live() && S.set.apply) S.set.apply(o.key.c_str(), o.values[o.cur].c_str());
    if (!o.live()) S.changed_restart = true;
    if (o.key == "TOTK_FPS_CAP" || o.key == "TOTK_FG" || o.key == "TOTK_FG_MODE") frame_apply();
    save_options();
}
int layout_changes() {
    int n = 0; for (int a = 0; a < GAME_ACTIONS; a++) n += game_actions[a].label && S.actions[a] != action_default(a);
    return n;
}
void layout_read() { if (S.set.controls) S.set.controls(S.actions, 0); }
void layout_write() { if (S.set.controls) S.set.controls(S.actions, 1); }

ImFont *font_asset(const char *name, float px, const ImWchar *ranges) {
    size_t n = 0; void *ttf = S.set.asset ? S.set.asset(name, &n) : nullptr;
    if (!ttf) return nullptr;
    /* the atlas takes ttf and frees it */
    ImFontConfig c; c.OversampleH = 2; c.OversampleV = 1;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(ttf, (int)n, px, &c, ranges);
}

bool init(VkFormat format) {
    if (S.ready && S.format == format) return true;
    if (S.failed || !S.configured) return false;
    if (!ImGui_ImplVulkan_LoadFunctions(load)) { S.failed = true; return false; }
    VkAttachmentDescription a = {};
    a.format = format; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // the present's own barrier follows
    a.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0; dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL; dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp = {}; rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rp.attachmentCount = 1; rp.pAttachments = &a;
    rp.subpassCount = 1; rp.pSubpasses = &sub; rp.dependencyCount = 2; rp.pDependencies = dep;
    if (VKF(vkCreateRenderPass)(S.vk.device, &rp, nullptr, &S.pass) != VK_SUCCESS) { S.failed = true; return false; }
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo pi = {}; pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 8; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    if (VKF(vkCreateDescriptorPool)(S.vk.device, &pi, nullptr, &S.pool) != VK_SUCCESS) { S.failed = true; return false; }

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    /* sizes for a 1080-line screen (1.5 pixels a design unit); other sizes scale them (overlay_record) */
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2044, 0x2190, 0x2193, 0x2212, 0x2212, 0};
    S.title = font_asset("fonts/MarcellusSC-Regular.ttf", 51, ranges);
    S.head = font_asset("fonts/MarcellusSC-Regular.ttf", 30, ranges);
    S.body = font_asset("fonts/Figtree-Regular.ttf", 27, ranges);
    S.value = font_asset("fonts/Figtree-Medium.ttf", 26, ranges);
    S.help = font_asset("fonts/Figtree-Regular.ttf", 22.5f, ranges);
    S.small = font_asset("fonts/Figtree-Regular.ttf", 20, ranges);
    S.tag = font_asset("fonts/Figtree-SemiBold.ttf", 17, ranges);
    if (!S.body) { fprintf(stderr, "[overlay] fonts missing from the APK's assets: system font\n");
        S.body = io.Fonts->AddFontFromFileTTF("/system/fonts/Roboto-Regular.ttf", 27.0f); if (!S.body) S.body = io.Fonts->AddFontDefault(); }
    /* the device's monospace font (every Android has it) */
    { ImFontConfig c; c.OversampleH = 2; c.OversampleV = 1; S.mono = io.Fonts->AddFontFromFileTTF("/system/fonts/DroidSansMono.ttf", 30, &c, ranges); }
    for (ImFont **f : {&S.title, &S.head, &S.value, &S.help, &S.small, &S.tag}) if (!*f) *f = S.body;
    if (!S.mono) S.mono = S.head;
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 0; st.ChildRounding = 0; st.FrameRounding = 0; st.WindowBorderSize = 0; st.ChildBorderSize = 0;
    st.FramePadding = ImVec2(0, 0); st.ItemSpacing = ImVec2(0, 0); st.WindowPadding = ImVec2(0, 0);
    ImVec4 *c = st.Colors, none(0, 0, 0, 0);
    c[ImGuiCol_WindowBg] = none; c[ImGuiCol_ChildBg] = none; c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(kCream);
    c[ImGuiCol_Button] = none; c[ImGuiCol_ButtonHovered] = none; c[ImGuiCol_ButtonActive] = none; c[ImGuiCol_NavCursor] = none;
    c[ImGuiCol_ScrollbarBg] = none; c[ImGuiCol_ScrollbarGrab] = none; c[ImGuiCol_PlotLines] = ImGui::ColorConvertU32ToFloat4(kJade);
    c[ImGuiCol_FrameBg] = none;

    ImGui_ImplVulkan_InitInfo ii = {};
    ii.Instance = S.vk.instance; ii.PhysicalDevice = S.vk.physical; ii.Device = S.vk.device; ii.QueueFamily = S.vk.family; ii.Queue = S.vk.queue;
    ii.DescriptorPool = S.pool; ii.RenderPass = S.pass; ii.MinImageCount = 2; ii.ImageCount = 4; ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.CheckVkResultFn = [](VkResult r) { if (r) fprintf(stderr, "[overlay] Vulkan call in the ImGui backend failed: %d\n", (int)r); };
    fprintf(stderr, "[overlay] initialising for swapchain format %d\n", (int)format);
    if (!ImGui_ImplVulkan_Init(&ii)) { S.failed = true; return false; }
    S.format = format; S.ready = true;
    return true;
}

Framebuffer *framebuffer(VkImage image, uint32_t w, uint32_t h) {
    for (Framebuffer &f : S.fbs) if (f.image == image && f.w == w && f.h == h) return &f;
    Framebuffer &f = S.fbs[S.next_fb++ % 8];
    if (f.fb) { VKF(vkDeviceWaitIdle)(S.vk.device); VKF(vkDestroyFramebuffer)(S.vk.device, f.fb, nullptr); VKF(vkDestroyImageView)(S.vk.device, f.view, nullptr); }
    VkImageViewCreateInfo vi = {}; vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = S.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (VKF(vkCreateImageView)(S.vk.device, &vi, nullptr, &f.view) != VK_SUCCESS) { f = {}; return nullptr; }
    VkFramebufferCreateInfo fi = {}; fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fi.renderPass = S.pass; fi.attachmentCount = 1; fi.pAttachments = &f.view;
    fi.width = w; fi.height = h; fi.layers = 1;
    if (VKF(vkCreateFramebuffer)(S.vk.device, &fi, nullptr, &f.fb) != VK_SUCCESS) { VKF(vkDestroyImageView)(S.vk.device, f.view, nullptr); f = {}; return nullptr; }
    f.image = image; f.w = w; f.h = h;
    return &f;
}

/* ImGui moves the focus with the d-pad / stick and activates with A; B, X, Y, L and R are this file's (presses since the
   last frame in S.pressed), so ImGui's own uses of those buttons (windowing, text input) never start. */
void feed_input() {
    ImGuiIO &io = ImGui::GetIO();
    pthread_mutex_lock(&S.input);
    uint32_t b = S.buttons, changed = b ^ S.seen; S.seen = b; S.pressed = S.edges; S.edges = 0;
    struct { uint32_t bit; ImGuiKey key; } map[] = {{BTN_A, ImGuiKey_GamepadFaceDown},
        {1u << 12, ImGuiKey_GamepadDpadLeft}, {1u << 13, ImGuiKey_GamepadDpadUp}, {1u << 14, ImGuiKey_GamepadDpadRight}, {1u << 15, ImGuiKey_GamepadDpadDown}};
    for (auto &m : map) if (changed & m.bit) io.AddKeyEvent(m.key, (b & m.bit) != 0);
    float x = S.lx / 32767.0f, y = -S.ly / 32767.0f;
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, x < -0.3f, x < 0 ? -x : 0); io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, x > 0.3f, x > 0 ? x : 0);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, y < -0.3f, y < 0 ? -y : 0); io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, y > 0.3f, y > 0 ? y : 0);
    for (unsigned i = 0; i < S.ntouch; i++) {
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen); io.AddMousePosEvent(S.touches[i].x, S.touches[i].y);
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen); io.AddMouseButtonEvent(0, S.touches[i].down != 0);
    }
    S.ntouch = 0;
    pthread_mutex_unlock(&S.input);
}
bool pressed(uint32_t bit) { return (S.pressed & bit) != 0; }
bool left_pressed() { return ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft); }
/* Lists wrap: a fresh press (not a held one) up from the first row goes to the last, and down from the last to the first. */
bool up_fresh() { return ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false); }
bool down_fresh() { return ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false); }
bool right_pressed() { return ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight); }

/* ---- drawing: design units (1280 x 720) times S.s ---- */
float u(float v) { return v * S.s; }
ImVec2 uv(float x, float y) { return ImVec2(x * S.s, y * S.s); }
ImVec2 text_size(ImFont *f, const char *t, float wrap = 0) { return f->CalcTextSizeA(f->FontSize * ImGui::GetIO().FontGlobalScale, FLT_MAX, wrap, t); }
void text(ImDrawList *dl, ImFont *f, ImVec2 p, ImU32 col, const char *t, float wrap = 0) { dl->AddText(f, f->FontSize * ImGui::GetIO().FontGlobalScale, p, col, t, nullptr, wrap); }
void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 fill, ImU32 outline = 0) {
    ImVec2 a(c.x, c.y - r), b(c.x + r, c.y), d(c.x, c.y + r), e(c.x - r, c.y);
    if (fill) dl->AddQuadFilled(a, b, d, e, fill);
    if (outline) dl->AddQuad(a, b, d, e, outline, u(1));
}
/* a gold rule with a diamond in the middle */
void ornament(ImDrawList *dl, ImVec2 p, float w) {
    float m = p.x + w / 2;
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(m - u(8), p.y), IM_COL32(216, 180, 92, 150), u(1));
    dl->AddLine(ImVec2(m + u(8), p.y), ImVec2(p.x + w, p.y), IM_COL32(216, 180, 92, 150), u(1));
    diamond(dl, ImVec2(m, p.y), u(4), kGold);
}
/* a panel: fill, hairline and gold corner brackets */
void brackets(ImDrawList *dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, kPanel); dl->AddRect(a, b, kLine, 0, 0, u(1));
    float k = u(16), t = u(2);
    dl->AddLine(a, ImVec2(a.x + k, a.y), kGold, t); dl->AddLine(a, ImVec2(a.x, a.y + k), kGold, t);
    dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x - k, a.y), kGold, t); dl->AddLine(ImVec2(b.x, a.y), ImVec2(b.x, a.y + k), kGold, t);
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x + k, b.y), kGold, t); dl->AddLine(ImVec2(a.x, b.y), ImVec2(a.x, b.y - k), kGold, t);
    dl->AddLine(b, ImVec2(b.x - k, b.y), kGold, t); dl->AddLine(b, ImVec2(b.x, b.y - k), kGold, t);
}
/* A focusable row: ImGui's button for focus and activation, drawn as the selection (a gold frame and diamond). */
struct Row { bool focused, clicked; ImVec2 a, b; };
Row row(const char *id, float w, float h, bool focus = false) {
    ImGui::PushID(id);
    /* the top: its heading too */
    if (S.loop_to == S.rows) { if (!S.rows) ImGui::SetScrollY(0); focus = true; S.loop_to = -1; }
    /* a new tab starts at its first heading */
    if (S.focus_first && !S.rows) ImGui::SetScrollY(0);
    if (focus || S.focus_first) { ImGui::SetKeyboardFocusHere(); S.focus_first = false; }
    Row r; r.a = ImGui::GetCursorScreenPos(); r.clicked = ImGui::Button("##row", ImVec2(w, h)); r.b = ImVec2(r.a.x + w, r.a.y + h);
    r.focused = ImGui::IsItemFocused();
    if (r.focused) S.row_focused = S.rows;
    S.rows++;
    ImGui::PopID();
    if (r.focused) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(r.a, r.b, kGoldFill, u(4)); dl->AddRect(r.a, r.b, kGoldFrame, u(4), 0, u(1));
        diamond(dl, ImVec2(r.a.x + u(18), (r.a.y + r.b.y) / 2), u(4.5f), kGold);
    }
    return r;
}
void rows_begin() { S.rows = 0; S.row_focused = -1; }
void rows_end() {
    if (S.rows < 2 || S.row_focused < 0) return;
    if (S.row_focused == 0 && up_fresh()) S.loop_to = S.rows - 1;
    else if (S.row_focused == S.rows - 1 && down_fresh()) S.loop_to = 0;
}
/* A touch-only button (not in the d-pad's focus order). */
bool touch_button(const char *id, ImVec2 p, ImVec2 size) {
    ImGui::SetCursorScreenPos(p);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    bool hit = ImGui::InvisibleButton(id, size);
    ImGui::PopItemFlag();
    return hit;
}
/* a footer button hint: glyph chip and words */
void hint(ImDrawList *dl, ImVec2 &p, const char *glyph, const char *what) {
    float h = u(24), w = strlen(glyph) > 1 ? text_size(S.tag, glyph).x + u(14) : h;
    ImVec2 a(p.x, p.y), b(p.x + w, p.y + h);
    if (strlen(glyph) > 1) dl->AddRectFilled(a, b, kCream, u(6)); else dl->AddCircleFilled(ImVec2(a.x + h / 2, a.y + h / 2), h / 2, kCream);
    ImVec2 g = text_size(S.tag, glyph); text(dl, S.tag, ImVec2(a.x + (w - g.x) / 2, a.y + (h - g.y) / 2), kInk, glyph);
    ImVec2 t = text_size(S.small, what); text(dl, S.small, ImVec2(b.x + u(8), a.y + (h - t.y) / 2), kSoft, what);
    p.x = b.x + u(8) + t.x + u(22);
}
void tag(ImDrawList *dl, ImVec2 right_mid, const char *t, ImU32 line, ImU32 ink, float *width) {
    ImVec2 ts = text_size(S.tag, t), a(right_mid.x - ts.x - u(16), right_mid.y - ts.y / 2 - u(3)), b(right_mid.x, right_mid.y + ts.y / 2 + u(3));
    dl->AddRect(a, b, line, u(3), 0, u(1)); text(dl, S.tag, ImVec2(a.x + u(8), a.y + u(3)), ink, t);
    *width = b.x - a.x;
}
/* "‹ value ›", right-aligned at x; returns its left edge */
float value_text(ImDrawList *dl, float x, float mid, const char *v, ImU32 col) {
    ImVec2 vs = text_size(S.value, v), cs = text_size(S.body, "›");
    float right = x, chev = right - cs.x; text(dl, S.body, ImVec2(chev, mid - cs.y / 2), kMuted, "›");
    float vx = chev - u(10) - vs.x; text(dl, S.value, ImVec2(vx, mid - vs.y / 2), col, v);
    float lx = vx - u(10) - cs.x; text(dl, S.body, ImVec2(lx, mid - cs.y / 2), kMuted, "‹");
    return lx;
}
void begin_page(const char *name) {
    ImGuiIO &io = ImGui::GetIO();
    /* the d-pad moves in the page shown */
    static int drawn = -1; if (drawn != S.page) { ImGui::SetNextWindowFocus(); drawn = S.page; }
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
}

/* ---- the quick menu ---- */
void quick_page(float w, float h, float fps, float frame_ms) {
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    ImU32 a0 = IM_COL32(7, 11, 13, 64), a1 = IM_COL32(7, 11, 13, 115), a2 = IM_COL32(7, 11, 13, 230);
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w * 0.5f, h), a0, a1, a1, a0);
    bg->AddRectFilledMultiColor(ImVec2(w * 0.5f, 0), ImVec2(w * 0.68f, h), a1, a2, a2, a1);
    bg->AddRectFilled(ImVec2(w * 0.68f, 0), ImVec2(w, h), a2);
    begin_page("quick"); rows_begin();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float x = w - u(440) + u(36), iw = u(440 - 36 - 40), y = u(40);
    text(dl, S.title, ImVec2(x, y), kGoldText, "Paused"); y += u(40);
    /* the frame rate is on the HUD already */
    ornament(dl, ImVec2(x, y + u(6)), u(220)); y += u(28);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    auto item = [&](const char *id, const char *label, float hgt, ImU32 col, bool chevron) {
        Row r = row(id, iw, u(hgt));
        ImVec2 ts = text_size(S.head, label); float mid = (r.a.y + r.b.y) / 2;
        text(dl, S.head, ImVec2(r.a.x + u(36), mid - ts.y / 2), r.focused ? kGoldText : col, label);
        if (chevron) { ImVec2 c = text_size(S.body, "›"); text(dl, S.body, ImVec2(r.b.x - u(16) - c.x, mid - c.y / 2), kMuted, "›"); }
        ImGui::SetCursorScreenPos(ImVec2(x, r.b.y + u(2)));
        return r.clicked;
    };
    auto rule = [&]() { float ry = ImGui::GetCursorScreenPos().y + u(8); dl->AddLine(ImVec2(x + u(36), ry), ImVec2(x + iw, ry), kLine, u(1)); ImGui::SetCursorScreenPos(ImVec2(x, ry + u(9))); };
    bool resume = item("resume", "Resume", 48, kCream, false);
    /* the menu opens on Resume */
    ImGui::SetItemDefaultFocus();
    if (resume) S.open = false;
    if (item("settings", "All settings", 42, kCream, true)) { S.page = PAGE_SETTINGS; S.focus_first = true; }
    if (S.set.controls && item("layout", "Controls", 42, kCream, true)) { layout_read(); S.page = PAGE_CONTROLS; S.controls_back = PAGE_QUICK; S.pad_row = 0; S.focus_first = true; }
    rule();
    for (Option &o : g_options) {
        if (!o.has('Q') || o.page()) continue;
        Row r = row(o.key.c_str(), iw, u(38));
        float mid = (r.a.y + r.b.y) / 2; ImVec2 ls = text_size(S.body, o.label.c_str());
        text(dl, S.body, ImVec2(r.a.x + u(36), mid - ls.y / 2), r.focused ? kGoldText : kCream, o.label.c_str());
        value_text(dl, r.b.x - u(16), mid, o.names[o.cur].c_str(), kJade);
        int step = r.clicked ? 1 : 0;
        if (r.focused && left_pressed()) step = -1;
        if (r.focused && right_pressed()) step = 1;
        if (r.focused && pressed(BTN_X)) set_value(o, o.def); else if (step) set_value(o, o.cur + step);
        ImGui::SetCursorScreenPos(ImVec2(x, r.b.y + u(2)));
    }
    rule();
    if (S.set.report) {
        S.set.report(0, nullptr, S.report, sizeof S.report);
        float ry = ImGui::GetCursorScreenPos().y;
        Row r = row("report", iw, u(42));
        { ImVec2 ts = text_size(S.head, "Save bug report"); text(dl, S.head, ImVec2(r.a.x + u(36), (r.a.y + r.b.y - ts.y) / 2), r.focused ? kGoldText : kCream, "Save bug report");
          if (r.focused || !S.report[0]) value_text(dl, r.b.x - u(16), (r.a.y + r.b.y) / 2, S.report_capture ? "+ frame capture" : "video, log and save", kJade);
          if (r.focused && (left_pressed() || right_pressed())) S.report_capture = !S.report_capture;
          ImGui::SetCursorScreenPos(ImVec2(x, r.b.y + u(2))); }
        /* touch: the value side picks */
        if (r.clicked && ImGui::GetIO().MouseReleased[0] && ImGui::GetIO().MousePos.x > r.a.x + iw * 0.55f) r.clicked = false, S.report_capture = !S.report_capture;
        if (r.clicked) {
            const Option *rec = option("TOTK_RECORD"); char details[900];
            snprintf(details, sizeof details, "%s\nFrame rate: %.0f fps, %.1f ms a frame\nRecording: %s", S.info, fps, frame_ms, rec && rec->cur ? "on" : "off");
            S.set.report(S.report_capture ? 2 : 1, details, S.report, sizeof S.report);
            /* the capture needs the game running into the freeze */
            if (S.report_capture) S.open = false;
        }
        /* its progress at the row's right, the folder's name once saved */
        if (S.report[0] && !r.focused) {
            const char *shown = strncmp(S.report, "Bug report saved: ", 18) ? S.report : strrchr(S.report, '/') ? strrchr(S.report, '/') + 1 : S.report;
            char t[120]; snprintf(t, sizeof t, "%s%s", shown == S.report ? "" : "saved \u00b7 ", shown);
            if (!strncmp(t, "Saving the bug report: ", 23)) memmove(t, t + 23, strlen(t + 23) + 1);
            ImVec2 ts = text_size(S.small, t); text(dl, S.small, ImVec2(x + iw - u(16) - ts.x, ry + (u(42) - ts.y) / 2), kJade, t);
        }
    }
    if (S.set.quit && item("quit", "Quit to startup screen", 42, kMuted, false)) S.set.quit();
    ImVec2 p(x, h - u(26) - u(24));
    hint(dl, p, "A", "Select"); hint(dl, p, "B", "Resume"); hint(dl, p, "‹ ›", "Change");
    if (pressed(BTN_B)) S.open = false;
    rows_end();
    ImGui::End();
}

/* ---- the full settings ---- */
void settings_page(float w, float h) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(7, 11, 13, 205));
    begin_page("settings"); rows_begin();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (g_tabs.empty()) { text(dl, S.body, uv(56, 40), kCream, "This build has no settings table."); if (pressed(BTN_B)) S.page = PAGE_QUICK; ImGui::End(); return; }
    if (S.tab >= (int)g_tabs.size()) S.tab = 0;
    // header: title, L, tabs, R; touch: Back and Resume on the right
    float x = u(56), y = u(30);
    text(dl, S.title, ImVec2(x, y - u(4)), kCream, "Settings"); x += text_size(S.title, "Settings").x + u(36);
    auto chip = [&](const char *t) { ImVec2 ts = text_size(S.tag, t); ImVec2 a(x, y + u(10)), b(x + ts.x + u(16), y + u(34));
        dl->AddRect(a, b, IM_COL32(238, 232, 216, 90), u(6), 0, u(1)); text(dl, S.tag, ImVec2(a.x + u(8), a.y + (u(24) - ts.y) / 2), kCream, t); x = b.x + u(18); };
    chip("L");
    for (int t = 0; t < (int)g_tabs.size(); t++) {
        const char *name = g_tabs[t].c_str(); ImVec2 ts = text_size(S.head, name); bool on = t == S.tab;
        if (touch_button(name, ImVec2(x - u(8), y), ImVec2(ts.x + u(16), u(44)))) { S.tab = t; S.focus_first = true; }
        text(dl, S.head, ImVec2(x, y + u(4)), on ? kGoldText : kMuted, name);
        if (on) diamond(dl, ImVec2(x + ts.x / 2, y + u(4) + ts.y + u(6)), u(3.5f), kGold);
        x += ts.x + u(28);
    }
    x -= u(10); chip("R");
    { float bx = w - u(56);
      ImVec2 rs = text_size(S.head, "Resume"), bs = text_size(S.head, "Back");
      if (touch_button("resume", ImVec2(bx - rs.x, y), ImVec2(rs.x, u(44)))) { S.page = PAGE_QUICK; S.open = false; }
      text(dl, S.head, ImVec2(bx - rs.x, y + u(4)), kGoldText, "Resume");
      if (touch_button("back", ImVec2(bx - rs.x - u(28) - bs.x, y), ImVec2(bs.x, u(44)))) S.page = PAGE_QUICK;
      text(dl, S.head, ImVec2(bx - rs.x - u(28) - bs.x, y + u(4)), kSoft, "Back"); }
    if (pressed(BTN_L)) { S.tab = (S.tab + (int)g_tabs.size() - 1) % (int)g_tabs.size(); S.focus_first = true; }
    if (pressed(BTN_R)) { S.tab = (S.tab + 1) % (int)g_tabs.size(); S.focus_first = true; }
    dl->AddLine(uv(56, 96), ImVec2(w - u(56), u(96)), kLine, u(1));

    // the rows
    float top = u(120), bottom = h - u(76), list_w = w - u(56) - u(400) - u(40) - u(56);
    auto listed = [&](const Option &o) { return o.tab == g_tabs[S.tab] && !o.heading() && shown(o); };
    if (S.focused < 0 || S.focused >= (int)g_options.size() || !listed(g_options[S.focused])) {
        S.focused = -1; for (int i = 0; i < (int)g_options.size() && S.focused < 0; i++) if (listed(g_options[i])) S.focused = i; }
    ImGui::SetCursorScreenPos(ImVec2(u(56), top));
    ImGui::BeginChild("rows", ImVec2(list_w, bottom - top), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImDrawList *rl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)g_options.size(); i++) {
        Option &o = g_options[i]; if (o.tab != g_tabs[S.tab] || !shown(o)) continue;
        if (o.heading()) {
            ImVec2 p = ImGui::GetCursorScreenPos(); float hh = u(S.rows ? 44 : 30); ImVec2 ts = text_size(S.head, o.label.c_str());
            text(rl, S.head, ImVec2(p.x + u(14), p.y + hh - u(8) - ts.y), kGoldText, o.label.c_str());
            ImGui::Dummy(ImVec2(list_w, hh)); continue;
        }
        Row r = row(o.key.c_str(), list_w, u(52));
        if (r.focused || r.clicked) S.focused = i;
        float mid = (r.a.y + r.b.y) / 2; ImVec2 ls = text_size(S.body, o.label.c_str());
        text(rl, S.body, ImVec2(r.a.x + u(36), mid - ls.y / 2), r.focused ? kGoldText : kCream, o.label.c_str());
        float right = r.b.x - u(16), left;
        if (o.page()) {
            char v[48]; int n = layout_changes(); if (r.focused || r.clicked) layout_read();
            snprintf(v, sizeof v, n ? "%d changed" : "Game defaults", n);
            ImVec2 c = text_size(S.body, "›"); text(rl, S.body, ImVec2(right - c.x, mid - c.y / 2), kMuted, "›");
            ImVec2 vs = text_size(S.value, v); left = right - c.x - u(12) - vs.x; text(rl, S.value, ImVec2(left, mid - vs.y / 2), kJade, v);
            if (r.clicked) { layout_read(); S.page = PAGE_CONTROLS; S.controls_back = PAGE_SETTINGS; S.pad_row = 0; S.focus_first = true; }
        } else {
            left = value_text(rl, right, mid, o.names[o.cur].c_str(), kJade);
            int step = r.clicked ? 1 : 0;
            if (r.focused && left_pressed()) step = -1;
            if (r.focused && right_pressed()) step = 1;
            if (r.focused && pressed(BTN_X)) set_value(o, o.def); else if (step) set_value(o, o.cur + step);
        }
        float tw = 0;
        if (o.has('X')) tag(rl, ImVec2(left - u(18), mid), "EXPERIMENTAL", IM_COL32(238, 232, 216, 110), kSoft, &tw);
        else if (!o.live() && !o.page()) tag(rl, ImVec2(left - u(18), mid), "RESTART", kOchre, kOchreText, &tw);
        ImGui::SetCursorScreenPos(ImVec2(r.a.x, r.b.y + u(2)));
    }
    if (g_tabs[S.tab] == "System" && S.info[0]) {
        ImVec2 p = ImGui::GetCursorScreenPos(); p.y += u(18);
        text(rl, S.help, ImVec2(p.x + u(36), p.y), kMuted, S.info, list_w - u(52));
        ImGui::Dummy(ImVec2(list_w, u(18) + text_size(S.help, S.info, list_w - u(52)).y));
    }
    rows_end();
    ImGui::EndChild();

    // the detail panel
    ImVec2 pa(w - u(56) - u(400), top), pb(w - u(56), bottom);
    brackets(dl, pa, pb);
    if (S.focused >= 0) {
        const Option &o = g_options[S.focused]; float px = pa.x + u(28), pw = u(400 - 56), py = pa.y + u(28);
        text(dl, S.head, ImVec2(px, py), kGoldText, o.label.c_str(), pw); py += text_size(S.head, o.label.c_str(), pw).y + u(14);
        text(dl, S.help, ImVec2(px, py), kHelp, o.help.c_str(), pw); py += text_size(S.help, o.help.c_str(), pw).y + u(16);
        /* the choices, closer together when many would run past the footer line (Render resolution has nine) */
        float step = o.names.empty() ? u(34) : std::max(u(24), std::min(u(34), (pb.y - u(64) - py) / o.names.size()));
        for (size_t i = 0; i < o.names.size() && !o.page(); i++) {
            bool on = (int)i == o.cur; ImVec2 ns = text_size(S.help, o.names[i].c_str()); float cy = py + step / 2;
            diamond(dl, ImVec2(px + u(15), cy), u(3.5f), on ? kJade : 0, on ? kJade : kSoft);
            text(dl, S.help, ImVec2(px + u(30), cy - ns.y / 2), on ? kJade : kSoft, o.names[i].c_str());
            if ((int)i == o.def && !on) { ImVec2 d = text_size(S.small, "default"); text(dl, S.small, ImVec2(px + u(30) + ns.x + u(10), cy - d.y / 2), kMuted, "default"); }
            py += step;
        }
        const char *applies = o.page() || o.live() ? "Takes effect immediately." : "Saved. Takes effect the next time the game starts.";
        dl->AddLine(ImVec2(px, pb.y - u(56)), ImVec2(pb.x - u(28), pb.y - u(56)), kLine, u(1));
        text(dl, S.help, ImVec2(px, pb.y - u(42)), o.live() || o.page() ? kMuted : kOchreText, applies, pw);
    }

    ImVec2 p(u(56), h - u(24) - u(24));
    hint(dl, p, "‹ ›", "Change"); hint(dl, p, "X", "Default"); hint(dl, p, "B", "Back");
    const char *saved = S.changed_restart ? "Changes are saved as you make them. Some take effect after a restart." : "Changes are saved as you make them.";
    ImVec2 ss = text_size(S.small, saved); text(dl, S.small, ImVec2(w - u(56) - ss.x, p.y + (u(24) - ss.y) / 2), kSoft, saved);
    if (pressed(BTN_B)) S.page = PAGE_QUICK;
    ImGui::End();
}

/* ---- the field controls: each action on a button of this device ---- */
void controls_page(float w, float h) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(7, 11, 13, 214));
    begin_page("controls"); rows_begin();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const Option *face = option("TOTK_FACE_BUTTONS"); bool by_label = face && face->values[face->cur] == "label";
    auto button = [&](int p) { return p < 0 ? "Nothing" : p < 4 && by_label ? g_label_short[p].c_str() : g_short[p].c_str(); };
    text(dl, S.title, uv(56, 26), kCream, "Controls");
    float cx = u(56) + text_size(S.title, "Controls").x + u(16);
    text(dl, S.help, ImVec2(cx, u(44)), kMuted, S.controls_back == PAGE_SETTINGS ? "Settings › Controls" : "Quick menu");
    char status[64]; int changes = layout_changes(); snprintf(status, sizeof status, changes ? "%d changed from the game's defaults" : "Using the game's default controls", changes);
    /* touch: back where the page was opened from */
    ImVec2 bs = text_size(S.head, "Back"), st = text_size(S.help, status);
    if (touch_button("back", ImVec2(w - u(56) - bs.x, u(30)), ImVec2(bs.x, u(44)))) { S.page = S.controls_back; S.focus_first = S.controls_back == PAGE_SETTINGS; }
    text(dl, S.head, ImVec2(w - u(56) - bs.x, u(34)), kSoft, "Back");
    text(dl, S.help, ImVec2(w - u(56) - bs.x - u(28) - st.x, u(44)), kJade, status);
    dl->AddLine(uv(56, 96), ImVec2(w - u(56), u(96)), kLine, u(1));
    int a = S.pad_row < 0 || S.pad_row >= GAME_ACTIONS - 1 ? 0 : S.pad_row, sel = S.actions[a];

    // the drawing: the selected action's button lit
    ImVec2 da = uv(56, 124), db = uv(526, 424);
    brackets(dl, da, db);
    dl->AddRect(ImVec2(da.x + u(30), da.y + u(62)), ImVec2(db.x - u(30), db.y - u(34)), IM_COL32(238, 232, 216, 72), u(40), 0, u(1.5f));
    for (int i = 0; i < BUTTONS; i++) {
        int at = by_label && i < 4 ? kLabelPosition[i] : i; const PadButton &b = kPad[at];
        if (b.w <= 0) continue;
        bool on = i == sel; ImVec2 pa(da.x + u(b.x), da.y + u(b.y)), pe(pa.x + u(b.w), pa.y + u(b.h));
        if (on) dl->AddRectFilled(pa, pe, IM_COL32(216, 180, 92, 90), u(b.r));
        dl->AddRect(pa, pe, on ? kGold : IM_COL32(238, 232, 216, 115), u(b.r), 0, u(1.5f));
        const char *t = i < 4 && by_label ? g_label_short[i].c_str() : b.text;
        if (t[0]) { ImVec2 ts = text_size(S.tag, t); text(dl, S.tag, ImVec2((pa.x + pe.x - ts.x) / 2, (pa.y + pe.y - ts.y) / 2), on ? kGoldText : kSoft, t); }
    }
    // the selected action
    { ImVec2 pa = uv(56, 444), pb = ImVec2(u(526), h - u(76));
      dl->AddRectFilled(pa, pb, kPanel); dl->AddRect(pa, pb, kLine, 0, 0, u(1));
      float pw = pb.x - pa.x - u(44), hx = pa.x + u(22), hy = pa.y + u(18), hh = text_size(S.head, game_actions[a].label).y;
      text(dl, S.head, ImVec2(hx, hy), kGoldText, game_actions[a].label); hx += text_size(S.head, game_actions[a].label).x + u(10);
      /* the title font has no arrow */
      ImVec2 as = text_size(S.value, "→"); text(dl, S.value, ImVec2(hx, hy + (hh - as.y) / 2), kGoldText, "→"); hx += as.x + u(10);
      text(dl, S.head, ImVec2(hx, hy), kGoldText, button(sel));
      char help[400]; snprintf(help, sizeof help, "%s Menus are not changed by this: they confirm with %s and go back with %s.", g_action_help[a].c_str(), button(1), button(0));
      text(dl, S.help, ImVec2(pa.x + u(22), hy + hh + u(10)), kHelp, help, pw); }

    // the rows
    float lx = u(566), lw = w - u(56) - lx, top = u(116);
    { float c = (lw - u(36) - u(16)) / 2; const char *cols[2] = {"ACTION", "BUTTON"};
      for (int k = 0; k < 2; k++) text(dl, S.tag, ImVec2(lx + u(36) + c * k, top), kMuted, cols[k]); }
    ImGui::SetCursorScreenPos(ImVec2(lx, top + u(24)));
    ImGui::BeginChild("actions", ImVec2(lw, h - u(76) - top - u(24)), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    ImDrawList *rl = ImGui::GetWindowDrawList();
    float col = (lw - u(36) - u(16)) / 2;
    /* the focus goes to the row of S.pad_row */
    bool changed = false, focus = S.focus_first; S.focus_first = false;
    for (int i = 0; i < GAME_ACTIONS; i++) {
        if (!game_actions[i].label) continue;
        Row r = row(game_actions[i].name, lw, u(30), focus && i == a);
        ImGui::SetCursorScreenPos(ImVec2(r.a.x, r.b.y + u(2)));
        if (r.focused || r.clicked) S.pad_row = i;
        float mid = (r.a.y + r.b.y) / 2;
        ImVec2 ls = text_size(S.help, game_actions[i].label); text(rl, S.help, ImVec2(r.a.x + u(36), mid - ls.y / 2), r.focused ? kGoldText : kCream, game_actions[i].label);
        char tv[48]; snprintf(tv, sizeof tv, "‹ %s ›", button(S.actions[i]));
        ImVec2 ts = text_size(S.help, tv); text(rl, S.help, ImVec2(r.a.x + u(36) + col, mid - ts.y / 2), kJade, tv);
        if (S.actions[i] != action_default(i)) rl->AddCircleFilled(ImVec2(r.a.x + u(36) + col + ts.x + u(10), mid), u(3), kJade);
        int step = r.clicked ? 1 : 0;
        if (r.focused && left_pressed()) step = -1;
        if (r.focused && right_pressed()) step = 1;
        /* the buttons that carry actions in order, then nothing */
        if (step) {
            int n = (int)g_carry.size(), cur = n; for (int k = 0; k < n; k++) if (g_carry[k] == S.actions[i]) cur = k;
            int next = (cur + step + n + 1) % (n + 1);
            /* the game reads no held stick clicks */
            while (game_actions[i].held && next < n && (g_carry[next] == 4 || g_carry[next] == 5))
                next = (next + step + n + 1) % (n + 1);
            S.actions[i] = (int8_t)(next == n ? -1 : g_carry[next]); changed = true;
        }
        if (r.focused && pressed(BTN_X)) { S.actions[i] = (int8_t)action_default(i); changed = true; }
    }
    ImGui::SetCursorScreenPos(ImVec2(lx, ImGui::GetCursorScreenPos().y + u(8)));
    { Row r = row("restore", lw, u(36));
      ImVec2 ts = text_size(S.head, "Restore the game's default controls"); text(rl, S.head, ImVec2(r.a.x + u(36), (r.a.y + r.b.y - ts.y) / 2), r.focused ? kGoldText : kSoft, "Restore the game's default controls");
      if (r.clicked) { for (int i = 0; i < GAME_ACTIONS; i++) S.actions[i] = (int8_t)action_default(i); changed = true; } }
    ImGui::Dummy(ImVec2(lw, u(4)));
    ImGui::EndChild();
    if (changed) layout_write();

    ImVec2 p(u(56), h - u(24) - u(24));
    hint(dl, p, "‹ ›", "Button"); hint(dl, p, "X", "Default"); hint(dl, p, "B", "Back");
    const char *note = "Changes take effect immediately and are kept for next time.";
    ImVec2 ns = text_size(S.small, note); text(dl, S.small, ImVec2(w - u(56) - ns.x, p.y + (u(24) - ns.y) / 2), kSoft, note);
    if (pressed(BTN_B)) { S.page = S.controls_back; S.focus_first = S.controls_back == PAGE_SETTINGS; }
    rows_end();
    ImGui::End();
}

/* The HUD items: each its own setting (TOTK_HUD_FPS, _FRAMETIME, _REFRESH, _TEMP, _SHADERS, _DRIVER). TOTK_HUD=off / fps / detailed in the
   environment (the profiling scripts) overrides them: off hides everything, fps shows the rate, detailed the rate and the
   frame time. */
/* TOTK_HUD_TEMP: the hotter of the hottest CPU and GPU thermal zones, read once a second on a thread of their own (a sensor read can take
   milliseconds), only while the item is on. -1: no zone of that kind can be read. */
std::atomic<int> g_temp_cpu{-1}, g_temp_gpu{-1}; std::atomic<bool> g_temp_on{false};
void *temp_thread(void *) {
    setpriority(PRIO_PROCESS, 0, 10);
    std::vector<std::string> cpu, gpu; char path[96], type[64];
    for (int i = 0; i < 256; i++) {
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/type", i);
        FILE *f = fopen(path, "r"); if (!f) continue;
        bool got = fgets(type, sizeof type, f) != nullptr; fclose(f); if (!got) continue;
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        if (!strncmp(type, "cpu", 3)) cpu.push_back(path); else if (!strncmp(type, "gpu", 3)) gpu.push_back(path);
    }
    auto hottest = [](const std::vector<std::string> &zones) { int best = -1;
        for (const std::string &z : zones) { FILE *f = fopen(z.c_str(), "r"); if (!f) continue; int v = 0; if (fscanf(f, "%d", &v) == 1 && v > 0 && v < 150000 && v / 1000 > best) best = v / 1000; fclose(f); }
        return best; };
    for (;;) { if (g_temp_on.load()) { g_temp_cpu = hottest(cpu); g_temp_gpu = hottest(gpu); } sleep(1); }
    return nullptr;
}
bool hud_item(const char *key) { const Option *o = option(key); return o && o->cur == 1; }
int hud_env() { static int env = -1; if (env < 0) { const char *e = getenv("TOTK_HUD"); env = !e ? 0 : !strcmp(e, "off") ? 1 : !strcmp(e, "fps") ? 2 : !strcmp(e, "detailed") ? 3 : 0; } return env; }
bool hud_any() { int e = hud_env(); if (e == 1) return false; if (e) return true; return hud_item("TOTK_HUD_FPS") || hud_item("TOTK_HUD_FRAMETIME") || hud_item("TOTK_HUD_REFRESH") || hud_item("TOTK_HUD_TEMP") || hud_item("TOTK_HUD_SHADERS") || hud_item("TOTK_HUD_DRIVER"); }
/* One plain line of monospace text on a translucent dark strip, in the corner chosen (TOTK_HUD_CORNER): the items on,
   left to right. Fixed-width numbers, so the line never shifts. */
void hud(float fps, float frame_ms) {
    int e = hud_env(); if (e == 1) return;
    bool f = e ? true : hud_item("TOTK_HUD_FPS"), g = e ? e == 3 : hud_item("TOTK_HUD_FRAMETIME"), sh = e ? false : hud_item("TOTK_HUD_SHADERS"), dr = e ? false : hud_item("TOTK_HUD_DRIVER"), rf = e ? false : hud_item("TOTK_HUD_REFRESH"), tp = e ? false : hud_item("TOTK_HUD_TEMP");
    g_temp_on = tp;
    if (tp) { static bool started; if (!started) { started = true; pthread_t t; if (!pthread_create(&t, nullptr, temp_thread, nullptr)) pthread_detach(t); } }
    if (!(f || g || sh || dr || rf || tp)) return;
    /* 0 bottom-left, 1 bottom-right, 2 top-left, 3 top-right */
    const Option *co = option("TOTK_HUD_CORNER"); int corner = co ? co->cur : 0;
    ImDrawList *dl = ImGui::GetForegroundDrawList(); ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImFont *fn = S.mono; float lh = text_size(fn, "0").y, gap = u(16), gw = u(120), pad = u(5);
    struct Seg { char text[192]; bool graph; float w; } segs[8]; int n = 0; float total = 0;
    auto add = [&](const char *t, bool graph) { Seg &sg = segs[n++]; snprintf(sg.text, sizeof sg.text, "%s", t); sg.graph = graph; sg.w = graph ? gw : text_size(fn, t).x; total += sg.w + gap; };
    char b[192];
    /* with frame generation on: the real and generated pictures a second, "120 fps (r60 g60)", g0 while none are made */
    if (f) { float real = S.st.real_fps, gen = std::max(fps - real, 0.0f);
        if (S.st.framegen && real > 0) snprintf(b, sizeof b, "%3.0f fps (r%2.0f g%2.0f)", fps, real, gen); else snprintf(b, sizeof b, "%3.0f fps", fps);
        add(b, false); }
    /* with frame generation: a game frame's time, split into the rest and generation's GPU time, "14.2 ms (r12.1 g2.1)" */
    if (g) { float rm = S.st.real_ms, gm = S.st.gen_ms;
        if (S.st.framegen && rm > 0) snprintf(b, sizeof b, "%5.1f ms (r%4.1f g%3.1f)", rm, rm - gm, gm); else snprintf(b, sizeof b, "%5.1f ms", frame_ms);
        add(b, false); add("", true); }
    if (rf) { if (S.st.screen_hz) snprintf(b, sizeof b, "%3u Hz", S.st.screen_hz); else snprintf(b, sizeof b, " -- Hz"); add(b, false); }
    if (tp) { int t = std::max(g_temp_cpu.load(), g_temp_gpu.load()); if (t >= 0) snprintf(b, sizeof b, "%3dC", t); else snprintf(b, sizeof b, " --C"); add(b, false); }
    /* pipelines handed to the compile thread and not built yet, the one in progress included */
    if (sh) { snprintf(b, sizeof b, "compiling %3llu", (unsigned long long)S.st.pipelines_queued); add(b, false); }
    if (dr && S.driver[0]) add(S.driver, false);
    total -= gap;
    float x = corner & 1 ? ds.x - u(6) - total : u(6), y = corner & 2 ? u(4) : ds.y - lh - u(4);
    dl->AddRectFilled(ImVec2(x - pad, y - pad + u(1)), ImVec2(x + total + pad, y + lh + pad), IM_COL32(0, 0, 0, 120));
    const ImU32 ink = IM_COL32(236, 236, 230, 255), shadow = IM_COL32(0, 0, 0, 160);
    for (int i = 0; i < n; i++) {
        /* the last 120 frames, 33 ms full height, frames over the 60 fps budget red */
        if (segs[i].graph) {
            float gh = lh - u(2), bar = gw / 120, gy = y + lh - u(1);
            for (int k = 0; k < 120; k++) {
                float m = S.frame_ms[(S.frame_i + k) % 120], v = m > 33.3f ? 1 : m / 33.3f;
                if (m > 0) dl->AddRectFilled(ImVec2(x + bar * k, gy - gh * v), ImVec2(x + bar * (k + 1) - u(0.4f), gy), m > 1000.0f / 59 ? IM_COL32(235, 95, 75, 235) : IM_COL32(215, 215, 210, 215));
            }
        } else { text(dl, fn, ImVec2(x + u(1), y + u(1)), shadow, segs[i].text); text(dl, fn, ImVec2(x, y), ink, segs[i].text); }
        x += segs[i].w + gap;
    }
}

}

extern "C" {

void overlay_configure(const overlay_vk *vk, const overlay_settings *settings) {
    S.vk = *vk; S.set = *settings; S.configured = true;
    if (settings->files_dir) snprintf(S.files_dir, sizeof S.files_dir, "%s", settings->files_dir);
    if (settings->info) snprintf(S.info, sizeof S.info, "%s", settings->info);
    if (settings->driver) snprintf(S.driver, sizeof S.driver, "%s", settings->driver);
    load_options(); load_actions();
    layout_read();
}
void overlay_forget_image(VkImage image) {
    for (Framebuffer &f : S.fbs) if (f.fb && f.image == image) {
        VKF(vkDestroyFramebuffer)(S.vk.device, f.fb, nullptr); VKF(vkDestroyImageView)(S.vk.device, f.view, nullptr); f = {}; }
}
int overlay_open(void) { return S.open.load() || S.preparing.load(); }
int overlay_wants_present(void) { return S.open.load() || S.preparing.load(); }
void overlay_prepare(int active, const char *stage, const char *detail, float fraction) {
    pthread_mutex_lock(&S.prep_lock);
    snprintf(S.prep_stage, sizeof S.prep_stage, "%s", stage ? stage : ""); snprintf(S.prep_detail, sizeof S.prep_detail, "%s", detail ? detail : ""); S.prep_frac = fraction;
    pthread_mutex_unlock(&S.prep_lock);
    S.preparing = active != 0;
}
void overlay_toggle(void) { S.toggles++; }

void overlay_pad(uint32_t buttons, int32_t lx, int32_t ly) {
    pthread_mutex_lock(&S.input); S.edges |= buttons & ~S.buttons; S.buttons = buttons; S.lx = lx; S.ly = ly; pthread_mutex_unlock(&S.input);
}
void overlay_touch(float x, float y, int down) {
    pthread_mutex_lock(&S.input);
    if (S.ntouch < 16) S.touches[S.ntouch++] = {x, y, down};
    pthread_mutex_unlock(&S.input);
}

/* the progress panel of prepare mode */
static void prepare_page(float w, float h) {
    char stage[160], detail[240]; float frac;
    pthread_mutex_lock(&S.prep_lock); memcpy(stage, S.prep_stage, sizeof stage); memcpy(detail, S.prep_detail, sizeof detail); frac = S.prep_frac; pthread_mutex_unlock(&S.prep_lock);
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("prepare", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), kInk);
    float pw = u(640), ph = u(250), x = (w - pw) / 2, y = (h - ph) / 2;
    brackets(dl, ImVec2(x, y), ImVec2(x + pw, y + ph));
    text(dl, S.title, ImVec2(x + u(36), y + u(28)), kCream, "Preparing shaders");
    ornament(dl, ImVec2(x + u(36), y + u(84)), pw - u(72));
    text(dl, S.head, ImVec2(x + u(36), y + u(104)), kGoldText, stage);
    float bx = x + u(36), by = y + u(146), bw = pw - u(72), bh = u(10);
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), kLine);
    if (frac >= 0) dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * (frac > 1 ? 1 : frac), by + bh), kGold);
    text(dl, S.help, ImVec2(bx, by + u(22)), kSoft, detail, bw);
    text(dl, S.help, ImVec2(bx, y + ph - u(40)), kMuted, "Once, so the game never compiles while you play. You can leave it running; it returns to the startup screen when done.", bw);
    ImGui::End();
}
int overlay_record(VkCommandBuffer cb, VkImage image, VkFormat format, uint32_t w, uint32_t h, float fps, float frame_ms, const overlay_stats *stats) {
    /* prepare mode: the panel only, no menu, no input */
    if (S.preparing.load()) {
        S.toggles = 0; if (!init(format)) return 0;
        Framebuffer *fb = framebuffer(image, w, h); if (!fb) return 0;
        ImGuiIO &io = ImGui::GetIO(); io.DeltaTime = 1.0f / 60.0f; io.DisplaySize = ImVec2((float)w, (float)h);
        S.s = (float)h / 720.0f; io.FontGlobalScale = S.s / 1.5f;
        ImGui_ImplVulkan_NewFrame(); ImGui::NewFrame(); ImGui::PushFont(S.body); prepare_page((float)w, (float)h); ImGui::PopFont(); ImGui::Render();
        VkRenderPassBeginInfo bi = {}; bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = S.pass; bi.framebuffer = fb->fb; bi.renderArea.extent = {w, h};
        VKF(vkCmdBeginRenderPass)(cb, &bi, VK_SUBPASS_CONTENTS_INLINE); ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb); VKF(vkCmdEndRenderPass)(cb);
        return 1;
    }
    /* Opening and closing happen here, on the thread that draws the menu (the menu changes its own page and state here
       too), and only when the menu can be drawn: the game never pauses behind a menu that is not shown. */
    if (S.toggles.exchange(0) & 1) {
        if (S.open.load() || init(format)) { S.open = !S.open.load(); S.page = PAGE_QUICK; fprintf(stderr, "[overlay] %s\n", S.open.load() ? "open (game paused)" : "closed"); }
        else fprintf(stderr, "[overlay] 【红】the menu cannot be drawn (initialisation failed): not opened\n");
    }
    bool open = S.open.load();
    if (!open) { S.frame_ms[S.frame_i] = frame_ms; S.frame_i = (S.frame_i + 1) % 120; }
    if (stats) S.st = *stats;
    if (!open && !hud_any()) {
        /* presses while playing are the game's: dropped, or the first frame of the menu would take them (B closing the
           game's own menu, or sprint, read as B: the menu would close again at once) */
        pthread_mutex_lock(&S.input); S.edges = 0; S.seen = S.buttons; pthread_mutex_unlock(&S.input);
        return 0;
    }
    if (!init(format)) return 0;
    Framebuffer *fb = framebuffer(image, w, h);
    if (!fb) return 0;
    ImGuiIO &io = ImGui::GetIO();
    uint64_t t = now_ns(); io.DeltaTime = S.last_ns && t > S.last_ns ? (float)((t - S.last_ns) / 1e9) : 1.0f / 60.0f; S.last_ns = t;
    if (io.DeltaTime <= 0 || io.DeltaTime > 0.25f) io.DeltaTime = 1.0f / 60.0f;
    io.DisplaySize = ImVec2((float)w, (float)h);
    /* the fonts are made for 1080 lines */
    S.s = (float)h / 720.0f; io.FontGlobalScale = S.s / 1.5f;
    feed_input();
    if (!open) { pthread_mutex_lock(&S.input); S.pressed = 0; pthread_mutex_unlock(&S.input); }
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(S.body);
    if (open) {
        if (S.page == PAGE_QUICK) quick_page((float)w, (float)h, fps, frame_ms);
        else if (S.page == PAGE_SETTINGS) settings_page((float)w, (float)h);
        else controls_page((float)w, (float)h);
    } else hud(fps, frame_ms);
    ImGui::PopFont();
    ImGui::Render();
    VkRenderPassBeginInfo bi = {}; bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; bi.renderPass = S.pass; bi.framebuffer = fb->fb;
    bi.renderArea.extent = {w, h};
    VKF(vkCmdBeginRenderPass)(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    VKF(vkCmdEndRenderPass)(cb);
    return 1;
}

}

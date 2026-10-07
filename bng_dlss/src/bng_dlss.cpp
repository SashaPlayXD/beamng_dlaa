// bng_dlss — Веха 6: DLAA в меню сглаживания BeamNG.
//   Lua-мод добавляет пункт «DLAA (NVIDIA)» в «Настройки -> Графика -> Сглаживание» и пишет выбор
//   в settings/bng_dlss_request.txt ("1" = DLAA, "0" = нет). Аддон читает этот файл ~2 раза в секунду.
//   Меню игры — главный выключатель; галочка в панели ReShade работает до следующего изменения в меню.
// (ниже — описание вехи 4е)
// bng_dlss — Веха 4е: чистовая версия DLAA с jitter.
//
// Что изменилось относительно 4д:
//   * Заголовки ReShade 6.8.0 (как установленный ReShade) + ImGui 1.92.5 для панели настроек.
//   * Никаких F-клавиш (F8 в BeamNG занята камерой). Всё управление — в ReShade:
//     Home -> вкладка «Дополнения» -> BeamNG DLSS -> раскрыть.
//   * Аддон пишет своё состояние в файл settings/bng_dlss_state.txt в папке пользователя BeamNG.
//     Lua-мод читает его и включает jitter ТОЛЬКО когда DLAA включён (иначе картинка дрожит).
//   * Убран нерабочий GPU-секундомер и отладочные режимы.
//
// Как это работает (кратко):
//   1. Точка встраивания — 3-я копия HDR-цвета RGBA16F в command list'е (сцена готова, пост-эффекты ещё нет).
//   2. Глубина D24S8 (reversed-Z) и вектора RG16F (UV, prev-cur) находятся по привязкам render target'ов.
//   3. DLSS (NGX) пишет результат в нашу текстуру, мы копируем её поверх цвета сцены.
//   4. NGX меняет heaps/root signature/PSO на command list'е — мы их перехватываем (vtable) и возвращаем.
//   5. Jitter задаёт Lua (Halton 2,3, 8 фаз); фазу аддон находит сам, читая вектора неподвижной земли.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <d3d12.h>
#include <nvsdk_ngx_helpers.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <psapi.h>

#pragma comment(lib, "user32.lib")   // нужен самой библиотеке NGX (GetWindowThreadProcessId)
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "nvsdk_ngx_s.lib")

using namespace reshade::api;

extern "C" __declspec(dllexport) const char *NAME = "BeamNG DLSS (bng_dlss)";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "bng_dlss v1.0: DLAA selectable in BeamNG Options -> Graphics -> Anti-aliasing. Diagnostics: Home -> Add-ons -> BeamNG DLSS.";

static void logf(const char *fmt, ...)
{
    char buf[512];
    va_list args; va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    reshade::log::message(reshade::log::level::info, buf);
}

// =====================================================================
// 1. Перехват D3D12: запоминаем heaps / root signatures / PSO каждого command list
// =====================================================================
struct cl_state
{
    UINT heap_count = 0;
    ID3D12DescriptorHeap *heaps[2] = {};
    ID3D12RootSignature *compute_rs = nullptr;
    ID3D12RootSignature *graphics_rs = nullptr;
    ID3D12PipelineState *pso = nullptr;
};
static std::mutex g_cl_mutex;
static std::unordered_map<ID3D12GraphicsCommandList *, cl_state> g_cl_state;

using PFN_SetPSO   = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
using PFN_SetHeaps = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, ID3D12DescriptorHeap *const *);
using PFN_SetRS    = void (STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12RootSignature *);
static PFN_SetPSO   o_SetPipelineState = nullptr;
static PFN_SetHeaps o_SetDescriptorHeaps = nullptr;
static PFN_SetRS    o_SetComputeRootSignature = nullptr;
static PFN_SetRS    o_SetGraphicsRootSignature = nullptr;

static void STDMETHODCALLTYPE h_SetPipelineState(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pso)
{
    { std::lock_guard<std::mutex> l(g_cl_mutex); g_cl_state[cl].pso = pso; }
    o_SetPipelineState(cl, pso);
}
static void STDMETHODCALLTYPE h_SetDescriptorHeaps(ID3D12GraphicsCommandList *cl, UINT n, ID3D12DescriptorHeap *const *heaps)
{
    {
        std::lock_guard<std::mutex> l(g_cl_mutex);
        cl_state &s = g_cl_state[cl];
        s.heap_count = n > 2 ? 2 : n;
        for (UINT i = 0; i < s.heap_count; ++i) s.heaps[i] = heaps[i];
    }
    o_SetDescriptorHeaps(cl, n, heaps);
}
static void STDMETHODCALLTYPE h_SetComputeRootSignature(ID3D12GraphicsCommandList *cl, ID3D12RootSignature *rs)
{
    { std::lock_guard<std::mutex> l(g_cl_mutex); g_cl_state[cl].compute_rs = rs; }
    o_SetComputeRootSignature(cl, rs);
}
static void STDMETHODCALLTYPE h_SetGraphicsRootSignature(ID3D12GraphicsCommandList *cl, ID3D12RootSignature *rs)
{
    { std::lock_guard<std::mutex> l(g_cl_mutex); g_cl_state[cl].graphics_rs = rs; }
    o_SetGraphicsRootSignature(cl, rs);
}

// номера методов в vtable ID3D12GraphicsCommandList (проверены по d3d12.h)
enum { VT_SetPipelineState = 25, VT_SetDescriptorHeaps = 28, VT_SetComputeRootSignature = 29, VT_SetGraphicsRootSignature = 30 };

static void *patch_vtable(void **vtable, int index, void *hook)
{
    DWORD old;
    VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    void *orig = vtable[index];
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void *), old, &old);
    return orig;
}

static void install_hooks(ID3D12GraphicsCommandList *any_list)
{
    if (o_SetPipelineState) return;
    void **vt = *reinterpret_cast<void ***>(any_list);
    o_SetPipelineState         = reinterpret_cast<PFN_SetPSO>(patch_vtable(vt, VT_SetPipelineState, reinterpret_cast<void *>(&h_SetPipelineState)));
    o_SetDescriptorHeaps       = reinterpret_cast<PFN_SetHeaps>(patch_vtable(vt, VT_SetDescriptorHeaps, reinterpret_cast<void *>(&h_SetDescriptorHeaps)));
    o_SetComputeRootSignature  = reinterpret_cast<PFN_SetRS>(patch_vtable(vt, VT_SetComputeRootSignature, reinterpret_cast<void *>(&h_SetComputeRootSignature)));
    o_SetGraphicsRootSignature = reinterpret_cast<PFN_SetRS>(patch_vtable(vt, VT_SetGraphicsRootSignature, reinterpret_cast<void *>(&h_SetGraphicsRootSignature)));
    logf("[bng_dlss] D3D12 command list hooks installed");
}

static cl_state snapshot_game_state(ID3D12GraphicsCommandList *native)
{
    std::lock_guard<std::mutex> l(g_cl_mutex);
    return g_cl_state[native];
}

// NGX сам вызывает SetDescriptorHeaps/SetPipelineState, поэтому «игровое» состояние
// снимаем ДО вызова DLSS и возвращаем именно его
static void restore_game_state(ID3D12GraphicsCommandList *native, const cl_state &s)
{
    if (s.heap_count) o_SetDescriptorHeaps(native, s.heap_count, s.heaps);
    if (s.compute_rs) o_SetComputeRootSignature(native, s.compute_rs);
    if (s.graphics_rs) o_SetGraphicsRootSignature(native, s.graphics_rs);
    if (s.pso) o_SetPipelineState(native, s.pso);
    std::lock_guard<std::mutex> l(g_cl_mutex);
    g_cl_state[native] = s;
}

// =====================================================================
// 2. Состояние аддона
// =====================================================================
static std::recursive_mutex g_mutex;
static const uint32_t HOOK_COPY_INDEX = 3;
static std::unordered_map<uint64_t, uint32_t> g_copy_count;
static std::unordered_map<uint64_t, resource_usage> g_state;
static resource g_depth = { 0 }, g_velocity = { 0 };

static device *g_dev = nullptr;
static ID3D12Device *g_d3d = nullptr;
static bool g_ngx_ok = false;
static NVSDK_NGX_Parameter *g_params = nullptr;
static NVSDK_NGX_Handle *g_dlss = nullptr;
static uint32_t g_w = 0, g_h = 0;
static resource g_out = { 0 };
static bool g_enabled = false, g_reset = true;
static uint32_t g_frames = 0, g_errors = 0, g_present_no = 0;
static NVSDK_NGX_Result g_last_result = NVSDK_NGX_Result_Success;

// пресеты модели: 0 = по умолчанию, E/F — CNN (легче), K/L/M — трансформер (L/M очень тяжёлые на RTX 30)
static const unsigned PRESETS[] = { 0, 5, 6, 11, 12, 13 };
static const char *PRESET_NAMES[] = { "Default", "E (CNN)", "F (CNN)", "K (transformer)", "L (transformer, heavy)", "M (transformer, heavy)" };
static int g_preset_idx = 3; // K

struct grave { NVSDK_NGX_Handle *h; uint32_t present_no; };
static std::vector<grave> g_graveyard;

// ---- jitter ----
static const uint32_t JITTER_PHASES = 8;
static uint32_t g_jit_counter = 0;      // растёт каждый кадр, как счётчик в Lua
static uint32_t g_jit_phase = 0;
static bool g_calibrated = false;
static bool g_auto_enabled_once = false;
static std::string g_calib_status = "waiting for a still camera...";

static float halton(uint32_t i, uint32_t b)
{
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= float(b); r += f * float(i % b); i /= b; }
    return r;
}

// ---- замер векторов движения неподвижной земли (для поиска фазы) ----
static const uint32_t RB_SLOTS = 8, RB_STRIDE = 512, RB_PIXELS = 64, CALIB_FRAMES = 16;
static resource g_rb = { 0 };
static uint32_t g_rb_n[RB_SLOTS] = {};
static bool g_rb_valid[RB_SLOTS] = {};
static uint32_t g_rb_w = 0, g_rb_h = 0;
static uint32_t g_capture_left = 0, g_last_hook_n = 0, g_last_read_n = UINT32_MAX, g_frames_since_attempt = 0;
static bool g_capture_manual = false;
struct sample { uint32_t n; double vx, vy; };
static std::vector<sample> g_samples;

static float half_to_float(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    float v;
    if (exp == 0) v = std::ldexp(float(mant), -24);
    else if (exp == 31) v = mant ? NAN : INFINITY;
    else v = std::ldexp(float(mant | 0x400), int(exp) - 25);
    return sign ? -v : v;
}

// ---- файл состояния для Lua-мода ----
static std::wstring g_state_path;
static int g_state_written = -1;

// пишет «1» (Lua должен делать jitter) или «0» (не должен)
static void write_state_file(bool jitter_on)
{
    const int v = jitter_on ? 1 : 0;
    if (v == g_state_written) return;
    if (g_state_path.empty())
    {
        const wchar_t *lad = _wgetenv(L"LOCALAPPDATA");
        if (!lad) return;
        g_state_path = std::wstring(lad) + L"\\BeamNG\\BeamNG.drive\\current\\settings\\bng_dlss_state.txt";
    }
    FILE *f = nullptr;
    if (_wfopen_s(&f, g_state_path.c_str(), L"wb") == 0 && f)
    {
        std::fputs(v ? "1" : "0", f);
        std::fclose(f);
        g_state_written = v;
    }
}

// ---- запрос из меню игры (Lua -> аддон) ----
static std::wstring g_request_path;
static int g_request_seen = -1;   // последнее прочитанное значение
static uint32_t g_request_poll = 0;

static int read_request_file()
{
    if (g_request_path.empty())
    {
        const wchar_t *lad = _wgetenv(L"LOCALAPPDATA");
        if (!lad) return -1;
        g_request_path = std::wstring(lad) + L"\\BeamNG\\BeamNG.drive\\current\\settings\\bng_dlss_request.txt";
    }
    FILE *f = nullptr;
    if (_wfopen_s(&f, g_request_path.c_str(), L"rb") != 0 || !f) return -1;
    const int c = std::fgetc(f);
    std::fclose(f);
    if (c == '1') return 1;
    if (c == '0') return 0;
    return -1;
}

static void set_enabled(bool v)
{
    if (g_enabled == v) return;
    g_enabled = v;
    g_reset = true;
    logf("[bng_dlss] DLAA %s", v ? "ON" : "OFF");
}

static bool is_screen(const resource_desc &d, format f)
{
    return d.type == resource_type::texture_2d && d.texture.format == f && d.texture.width >= 640 && d.texture.height >= 360;
}

static resource_usage known_state(resource r, resource_usage fallback)
{
    auto it = g_state.find(r.handle);
    return it != g_state.end() ? it->second : fallback;
}

// =====================================================================
// 3. NGX
// =====================================================================
static void on_init_device(device *dev)
{
    if (dev->get_api() != device_api::d3d12 || g_d3d != nullptr) return;
    g_dev = dev;
    g_d3d = reinterpret_cast<ID3D12Device *>(dev->get_native());
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID("6b1f0a52-8c3d-4e9a-b7f2-1d4c5e6f7a80", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "beamng-0.39", L".", g_d3d);
    logf("[bng_dlss] NGX init: 0x%08X", (unsigned)r);
    if (NVSDK_NGX_FAILED(r)) return;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&g_params)) || !g_params) return;
    int available = 0;
    NVSDK_NGX_Parameter_GetI(g_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    g_ngx_ok = available != 0;
    logf("[bng_dlss] DLSS available=%d", available);
    g_state_written = -1;
}

static void release_dlss()
{
    if (g_dlss) { NVSDK_NGX_D3D12_ReleaseFeature(g_dlss); g_dlss = nullptr; }
    if (g_out.handle && g_dev) { g_dev->destroy_resource(g_out); g_out = { 0 }; }
    g_w = g_h = 0;
}

static void on_destroy_device(device *dev)
{
    if (dev != g_dev) return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    release_dlss();
    if (g_rb.handle) { dev->destroy_resource(g_rb); g_rb = { 0 }; }
    for (const grave &g : g_graveyard) NVSDK_NGX_D3D12_ReleaseFeature(g.h);
    g_graveyard.clear();
    if (g_params) { NVSDK_NGX_D3D12_DestroyParameters(g_params); g_params = nullptr; }
    NVSDK_NGX_D3D12_Shutdown1(g_d3d);
    g_d3d = nullptr; g_dev = nullptr; g_ngx_ok = false;
    logf("[bng_dlss] NGX shut down");
}

// =====================================================================
// 4. Слежение за ресурсами
// =====================================================================
static void on_barrier(command_list *, uint32_t count, const resource *res, const resource_usage *, const resource_usage *new_states)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    for (uint32_t i = 0; i < count; ++i) g_state[res[i].handle] = new_states[i];
}

static void on_destroy_resource(device *, resource res)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    g_state.erase(res.handle);
    if (res == g_depth) g_depth = { 0 };
    if (res == g_velocity) g_velocity = { 0 };
}

static void on_bind_rts(command_list *cl, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
    if (count == 0 || rtvs[0].handle == 0) return;
    device *dev = cl->get_device();
    const resource rt0 = dev->get_resource_from_view(rtvs[0]);
    const resource_desc d0 = dev->get_resource_desc(rt0);
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (is_screen(d0, format::r16g16_float))
        g_velocity = rt0;
    else if (is_screen(d0, format::r16g16b16a16_float) && dsv.handle != 0)
    {
        const resource ds = dev->get_resource_from_view(dsv);
        if (is_screen(dev->get_resource_desc(ds), format::d24_unorm_s8_uint)) g_depth = ds;
    }
}

static void on_reset_command_list(command_list *cl)
{
    { std::lock_guard<std::recursive_mutex> lock(g_mutex); g_copy_count[reinterpret_cast<uint64_t>(cl)] = 0; }
    auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cl->get_native());
    install_hooks(native);
    std::lock_guard<std::mutex> l(g_cl_mutex);
    g_cl_state[native] = cl_state();
}

// =====================================================================
// 5. DLAA
// =====================================================================
static bool ensure_feature(command_list *cl, device *dev, uint32_t w, uint32_t h)
{
    if (g_dlss && g_w == w && g_h == h) return true;
    if (g_w != w || g_h != h) release_dlss();
    else if (g_dlss) { g_graveyard.push_back({ g_dlss, g_present_no }); g_dlss = nullptr; }
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, PRESETS[g_preset_idx]);

    const resource_desc od(w, h, 1, 1, format::r16g16b16a16_float, 1, memory_heap::gpu_only,
        resource_usage::unordered_access | resource_usage::copy_source | resource_usage::shader_resource);
    if (!g_out.handle && !dev->create_resource(od, nullptr, resource_usage::unordered_access, &g_out))
    {
        logf("[bng_dlss] failed to create output texture");
        return false;
    }

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = w;  cp.Feature.InHeight = h;
    cp.Feature.InTargetWidth = w;  cp.Feature.InTargetHeight = h;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
                            | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                            | NVSDK_NGX_DLSS_Feature_Flags_MVJittered; // замер 4б: движок пишет jitter в вектора
    const NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(reinterpret_cast<ID3D12GraphicsCommandList *>(cl->get_native()), 1, 1, &g_dlss, g_params, &cp);
    logf("[bng_dlss] DLAA feature %ux%u preset %s create: 0x%08X", w, h, PRESET_NAMES[g_preset_idx], (unsigned)r);
    g_last_result = r;
    if (NVSDK_NGX_FAILED(r)) { g_dlss = nullptr; return false; }
    g_w = w; g_h = h; g_reset = true;
    return true;
}

static void capture_velocity_rows(command_list *cl, device *dev, uint32_t hook_n)
{
    if (!g_rb.handle)
        dev->create_resource(resource_desc(uint64_t(RB_SLOTS) * RB_STRIDE, memory_heap::gpu_to_cpu, resource_usage::copy_dest), nullptr, resource_usage::copy_dest, &g_rb);
    if (!g_rb.handle) return;
    const resource_desc vd = dev->get_resource_desc(g_velocity);
    const uint32_t slot = hook_n % RB_SLOTS;
    const subresource_box box = { vd.texture.width / 2 - RB_PIXELS / 2, vd.texture.height * 3 / 4, 0,
                                  vd.texture.width / 2 + RB_PIXELS / 2, vd.texture.height * 3 / 4 + 1, 1 };
    const resource_usage vs = known_state(g_velocity, resource_usage::shader_resource);
    cl->barrier(g_velocity, vs, resource_usage::copy_source);
    cl->copy_texture_to_buffer(g_velocity, 0, &box, g_rb, uint64_t(slot) * RB_STRIDE, RB_PIXELS, 1);
    cl->barrier(g_velocity, resource_usage::copy_source, vs);
    g_rb_n[slot] = hook_n; g_rb_valid[slot] = true;
    g_rb_w = vd.texture.width; g_rb_h = vd.texture.height;
    g_last_hook_n = hook_n;
}

static bool on_copy_region(command_list *cl, resource src, uint32_t, const subresource_box *, resource dst, uint32_t, const subresource_box *, filter_mode)
{
    device *dev = cl->get_device();
    const resource_desc sd = dev->get_resource_desc(src);
    if (!is_screen(sd, format::r16g16b16a16_float)) return false;
    const resource_desc dd = dev->get_resource_desc(dst);
    if (!is_screen(dd, format::r16g16b16a16_float) || dd.texture.width != sd.texture.width || dd.texture.height != sd.texture.height) return false;

    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (++g_copy_count[reinterpret_cast<uint64_t>(cl)] != HOOK_COPY_INDEX) return false;

    const uint32_t hook_n = g_jit_counter++;
    const uint32_t jit_index = (hook_n + g_jit_phase) % JITTER_PHASES + 1; // как в Lua: 1..8

    if (g_capture_left > 0 && g_velocity.handle)
        capture_velocity_rows(cl, dev, hook_n);

    if (!g_enabled || !g_ngx_ok || !g_depth.handle || !g_velocity.handle || g_errors > 5) return false;
    if (!ensure_feature(cl, dev, sd.texture.width, sd.texture.height)) { ++g_errors; return false; }

    auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cl->get_native());
    const resource_usage depth_state = known_state(g_depth, resource_usage::shader_resource);
    const resource_usage vel_state = known_state(g_velocity, resource_usage::shader_resource);

    cl->barrier(src, resource_usage::copy_source, resource_usage::shader_resource);
    if (depth_state != resource_usage::shader_resource) cl->barrier(g_depth, depth_state, resource_usage::shader_resource);
    if (vel_state != resource_usage::shader_resource) cl->barrier(g_velocity, vel_state, resource_usage::shader_resource);

    const cl_state game_state = snapshot_game_state(native);

    // Замер 4б/4в: картинка сдвигается на (-(h2-0.5), -h3) px; DLSS ждёт смещение точки выборки,
    // найденные тестом знаки: X-, Y- от (h2-0.5, h3-0.5).
    const float jx = -(halton(jit_index, 2) - 0.5f);
    const float jy = -(halton(jit_index, 3) - 0.5f);

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = reinterpret_cast<ID3D12Resource *>(src.handle);
    ep.Feature.pInOutput = reinterpret_cast<ID3D12Resource *>(g_out.handle);
    ep.pInDepth = reinterpret_cast<ID3D12Resource *>(g_depth.handle);
    ep.pInMotionVectors = reinterpret_cast<ID3D12Resource *>(g_velocity.handle);
    ep.InJitterOffsetX = g_calibrated ? jx : 0.0f;
    ep.InJitterOffsetY = g_calibrated ? jy : 0.0f;
    ep.InRenderSubrectDimensions = { g_w, g_h };
    ep.InMVScaleX = static_cast<float>(g_w);
    ep.InMVScaleY = static_cast<float>(g_h);
    ep.InReset = g_reset ? 1 : 0;
    const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(native, g_dlss, g_params, &ep);
    g_reset = false;

    restore_game_state(native, game_state);

    if (vel_state != resource_usage::shader_resource) cl->barrier(g_velocity, resource_usage::shader_resource, vel_state);
    if (depth_state != resource_usage::shader_resource) cl->barrier(g_depth, resource_usage::shader_resource, depth_state);

    if (NVSDK_NGX_FAILED(r))
    {
        cl->barrier(src, resource_usage::shader_resource, resource_usage::copy_source);
        g_last_result = r;
        if (++g_errors <= 5) logf("[bng_dlss] DLAA evaluate FAILED: 0x%08X", (unsigned)r);
        return false;
    }

    cl->barrier(src, resource_usage::shader_resource, resource_usage::copy_dest);
    cl->barrier(g_out, resource_usage::unordered_access, resource_usage::copy_source);
    cl->copy_resource(g_out, src);
    cl->barrier(g_out, resource_usage::copy_source, resource_usage::unordered_access);
    cl->barrier(src, resource_usage::copy_dest, resource_usage::copy_source);

    if (++g_frames % 3600 == 1)
        logf("[bng_dlss] DLAA running, frames=%u, calibrated=%d", g_frames, g_calibrated ? 1 : 0);
    return false;
}

// =====================================================================
// 6. Калибровка jitter (раз в кадр, из present)
// =====================================================================
static void start_capture(bool manual)
{
    g_capture_left = CALIB_FRAMES;
    g_capture_manual = manual;
    g_samples.clear();
    for (bool &v : g_rb_valid) v = false;
    g_last_read_n = UINT32_MAX;
    g_frames_since_attempt = 0;
}

static void finish_capture()
{
    // модель: Lua в кадре n использует индекс (n + c) % 8 + 1; вектор земли = сдвиг(прошлый) - сдвиг(текущий)
    auto shift = [](uint32_t i, double &x, double &y) { x = -(halton(i, 2) - 0.5); y = -halton(i, 3); };
    double best_err = 1e30, energy = 0; uint32_t best_c = 0;
    for (const sample &sm : g_samples) energy += sm.vx * sm.vx + sm.vy * sm.vy;
    for (uint32_t c = 0; c < JITTER_PHASES; ++c)
    {
        double err = 0;
        for (const sample &sm : g_samples)
        {
            double cx, cy, px, py;
            shift((sm.n + c) % JITTER_PHASES + 1, cx, cy);
            shift((sm.n + JITTER_PHASES - 1 + c) % JITTER_PHASES + 1, px, py);
            err += (px - cx - sm.vx) * (px - cx - sm.vx) + (py - cy - sm.vy) * (py - cy - sm.vy);
        }
        if (err < best_err) { best_err = err; best_c = c; }
    }
    const double rms = std::sqrt(best_err / g_samples.size());
    char buf[160];
    if (energy / g_samples.size() < 0.01)
    {
        std::snprintf(buf, sizeof(buf), "no jitter seen (is the bng_dlss mod active?)");
        if (g_calibrated) { g_calibrated = false; g_reset = true; logf("[bng_dlss] jitter disappeared -> plain DLAA"); }
    }
    else if (rms > 0.05)
        std::snprintf(buf, sizeof(buf), "camera was moving (rms %.3f px), will retry", rms);
    else
    {
        if (!g_calibrated || best_c != g_jit_phase)
        {
            g_jit_phase = best_c;
            g_calibrated = true;
            g_reset = true;
            logf("[bng_dlss] CALIBRATION OK (%s): phase=%u, rms %.4f px", g_capture_manual ? "manual" : "auto", best_c, rms);
            if (!g_auto_enabled_once) { g_auto_enabled_once = true; set_enabled(true); }
        }
        std::snprintf(buf, sizeof(buf), "in sync: phase %u, error %.4f px", best_c, rms);
    }
    g_calib_status = buf;
}

static void on_present(command_queue *, swapchain *, const rect *, const rect *, uint32_t, const rect *)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    // До первой калибровки DLAA выключен, а Lua должен включить jitter, чтобы было что мерить.
    // Поэтому файл состояния говорит «1», пока калибровка не найдена или DLAA включён.
    // меню игры: читаем запрос примерно 2 раза в секунду
    if (g_request_poll++ % 30 == 0)
    {
        const int req = read_request_file();
        if (req >= 0 && req != g_request_seen)
        {
            g_request_seen = req;
            g_auto_enabled_once = true;        // выбор пользователя важнее автоматики
            logf("[bng_dlss] game menu request: DLAA %s", req ? "ON" : "OFF");
            set_enabled(req == 1);
        }
    }

    const bool want_jitter = g_enabled || !g_auto_enabled_once;
    write_state_file(want_jitter); // пишет на диск только при изменении

    // автозамер: пока не откалиброваны — раз в ~1 с; потом раз в ~10 с; при выключенном DLAA не меряем
    if (g_capture_left == 0 && g_velocity.handle && want_jitter && ++g_frames_since_attempt >= (g_calibrated ? 600u : 60u))
        start_capture(false);

    // читаем то, что GPU записал 4 кадра назад
    if (g_capture_left > 0 && g_rb.handle && g_dev && g_last_hook_n >= 4)
    {
        const uint32_t n = g_last_hook_n - 4, slot = n % RB_SLOTS;
        if (g_rb_valid[slot] && g_rb_n[slot] == n && n != g_last_read_n)
        {
            void *ptr = nullptr;
            if (g_dev->map_buffer_region(g_rb, uint64_t(slot) * RB_STRIDE, RB_PIXELS * 4, map_access::read_only, &ptr) && ptr)
            {
                const uint16_t *hv = static_cast<const uint16_t *>(ptr);
                double sx = 0, sy = 0;
                for (uint32_t i = 0; i < RB_PIXELS; ++i) { sx += half_to_float(hv[i * 2]); sy += half_to_float(hv[i * 2 + 1]); }
                g_dev->unmap_buffer_region(g_rb);
                g_samples.push_back({ n, sx / RB_PIXELS * g_rb_w, sy / RB_PIXELS * g_rb_h });
                g_last_read_n = n;
                if (--g_capture_left == 0) finish_capture();
            }
        }
    }

    ++g_present_no;
    for (size_t i = 0; i < g_graveyard.size();)
    {
        if (g_present_no - g_graveyard[i].present_no > 4)
        {
            NVSDK_NGX_D3D12_ReleaseFeature(g_graveyard[i].h);
            g_graveyard.erase(g_graveyard.begin() + i);
        }
        else ++i;
    }
}

// =====================================================================
// 7. Панель настроек в ReShade (Home -> «Дополнения» -> BeamNG DLSS)
// =====================================================================
static void draw_settings(effect_runtime *)
{
    std::lock_guard<std::recursive_mutex> lock(g_mutex);

    bool enabled = g_enabled;
    if (ImGui::Checkbox("DLAA (DLSS anti-aliasing)", &enabled))
    {
        g_auto_enabled_once = true; // пользователь решил сам — больше не включаем автоматически
        set_enabled(enabled);
    }
    ImGui::SetItemTooltip("When off, the Lua mod also stops the camera jitter (otherwise the image would shake).");

    int preset = g_preset_idx;
    if (ImGui::Combo("Model preset", &preset, PRESET_NAMES, IM_ARRAYSIZE(PRESET_NAMES)) && preset != g_preset_idx)
    {
        g_preset_idx = preset;
        if (g_dlss) { g_graveyard.push_back({ g_dlss, g_present_no }); g_dlss = nullptr; }
        g_reset = true;
        logf("[bng_dlss] preset -> %s", PRESET_NAMES[g_preset_idx]);
    }
    ImGui::SetItemTooltip("K is the best balance on RTX 30. L and M are 5-15x slower on RTX 30.");

    ImGui::Separator();
    ImGui::Text("Jitter: %s", g_calibrated ? "synchronized" : "not synchronized");
    ImGui::TextWrapped("Calibration: %s", g_calib_status.c_str());
    if (ImGui::Button("Calibrate now"))
        start_capture(true);
    ImGui::SetItemTooltip("Keep the camera still for ~1 second. Normally this happens automatically.");

    ImGui::Separator();
    ImGui::Text("NGX: %s | feature: %s | %ux%u", g_ngx_ok ? "ok" : "NOT AVAILABLE", g_dlss ? "created" : "-", g_w, g_h);
    ImGui::Text("Inputs: depth %s, motion vectors %s | frames %u | errors %u",
        g_depth.handle ? "found" : "MISSING", g_velocity.handle ? "found" : "MISSING", g_frames, g_errors);
    if (NVSDK_NGX_FAILED(g_last_result))
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Last NGX error: 0x%08X", (unsigned)g_last_result);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module)) return FALSE;
        logf("[bng_dlss] v1.0 loaded. DLAA is selectable in the game anti-aliasing menu.");
        reshade::register_event<reshade::addon_event::init_device>(on_init_device);
        reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
        reshade::register_event<reshade::addon_event::barrier>(on_barrier);
        reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_rts);
        reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
        reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_region);
        reshade::register_event<reshade::addon_event::present>(on_present);
        reshade::register_overlay(nullptr, draw_settings);
        break;
    case DLL_PROCESS_DETACH:
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}

// D3D12 screen surface: finds the placeholder screen texture when the game creates it and writes frames into it.
//
// - Hooks: vtable slots of the D3D12 runtime's device (resource creation) and command queue (ExecuteCommandLists),
//   read from a throw-away device/queue at plugin load. D3D12 devices are runtime singletons, so the game's device
//   shares the vtable; wrappers such as Streamline forward to the real objects and reach the hooks too.
// - Matching: a 2D texture with exactly the configured size, one mip, one slice and an RGBA8 format. No vanilla
//   texture is uncompressed at that size. The match is logged with the creating API and its parameters.
// - Upload: on the busiest DIRECT queue, at most `fps` times a second, a small command list copies the newest frame
//   from a ring of 3 upload buffers into every live surface and is submitted just before the game's own lists.
//   A busy ring slot skips the frame: the game thread never waits on the GPU for us.
// - Lifetime: we hold one reference per surface. When ours is the only one left the game has dropped the texture;
//   it is released once our last copy into it has completed.
#include "video_surface.h"

#include "d3d12_vtbl.h"
#include "logger.h"
#include "osd.h"
#include "test_pattern.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <d3d12.h>

namespace
{
using PFN_CreateCommittedResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreatePlacedResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
    const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateReservedResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateCommittedResource1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
    ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreateCommittedResource2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
    ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreatePlacedResource1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
    const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateCommittedResource3 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*,
    ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
using PFN_CreatePlacedResource2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64,
    const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*, REFIID,
    void**);
using PFN_ExecuteCommandLists = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

PFN_CreateCommittedResource g_createCommitted = nullptr;
PFN_CreatePlacedResource g_createPlaced = nullptr;
PFN_CreateReservedResource g_createReserved = nullptr;
PFN_CreateCommittedResource1 g_createCommitted1 = nullptr;
PFN_CreateCommittedResource2 g_createCommitted2 = nullptr;
PFN_CreatePlacedResource1 g_createPlaced1 = nullptr;
PFN_CreateCommittedResource3 g_createCommitted3 = nullptr;
PFN_CreatePlacedResource2 g_createPlaced2 = nullptr;
PFN_ExecuteCommandLists g_executeCommandLists = nullptr;

Logger* g_log = nullptr;
SurfaceConfig g_cfg;

constexpr std::size_t kRing = 3;
constexpr int kMaxNearMissLogs = 40;

struct Surface
{
    ID3D12Resource* resource{ nullptr };   // our reference
    D3D12_RESOURCE_DESC desc{};
    std::size_t channel{ 0 };    // index into g_channels
    bool simultaneous{ false };
    bool released{ false };      // the game no longer holds it; waiting for our copies to finish
    UINT64 lastFence{ 0 };
    int id{ 0 };
};

// One per screen channel (a placeholder texture size): its frame source and its upload buffers, one per ring slot,
// created when the channel's first texture appears.
struct Channel
{
    int id{ 0 };
    std::uint32_t width{ 0 };
    std::uint32_t height{ 0 };
    std::shared_ptr<FrameSource> source;
    std::uint64_t lastSeq{ 0 };
    int live{ 0 };               // its textures the game still holds
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{ 0 };
    std::array<ID3D12Resource*, kRing> upload{};
    std::array<std::uint8_t*, kRing> mapped{};
    bool ready{ false };
    bool failed{ false };
    Osd osd;                     // TV-style notice drawn over the frames until osd.until
    bool osdDrawn{ false };      // the last upload carried the notice (one clean copy removes it)
};

// A ring slot: one upload buffer per channel (Channel::upload[slot]) and the command list that copies them into the
// textures. The staging thread fills a free slot with the channels' new frames (Idle -> Staging -> Ready); the game's
// submission hook records and submits the copies (Ready -> Idle, its fence pending until the GPU is done).
enum class SlotState
{
    Idle,
    Staging,
    Ready
};

struct Slot
{
    ID3D12CommandAllocator* allocator{ nullptr };
    ID3D12GraphicsCommandList* list{ nullptr };
    UINT64 fence{ 0 };
    SlotState state{ SlotState::Idle };
    std::vector<char> fresh;   // per channel: a new frame was staged in this slot
};

struct QueueStat
{
    ID3D12CommandQueue* queue{ nullptr };
    bool direct{ false };
    std::uint32_t calls{ 0 };
};

std::mutex g_mutex;                      // surfaces, ring, queue choice
std::vector<Surface> g_surfaces;
std::atomic<int> g_liveSurfaces{ 0 };    // surfaces the game still holds
std::atomic<int> g_trackedSurfaces{ 0 }; // entries in g_surfaces (fast path for the per-submission hook)
std::atomic<int> g_nearMissLogs{ 0 };
int g_nextSurfaceId = 1;

ID3D12Device* g_device = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceValue = 0;
std::array<Slot, kRing> g_ring{};
std::size_t g_stageNext = 0;    // the slot the staging thread fills next
std::size_t g_submitNext = 0;   // the slot the hook submits next (same order)
bool g_uploadReady = false;
bool g_uploadFailed = false;
std::vector<Channel> g_channels;   // fixed at install; contents under g_mutex

std::array<QueueStat, 8> g_queues{};
ID3D12CommandQueue* g_chosenQueue = nullptr;
std::chrono::steady_clock::time_point g_queueWindowStart{};
std::chrono::steady_clock::time_point g_lastStats{};
std::uint64_t g_statUploads = 0;
std::uint64_t g_statBusy = 0;
// Staging thread: copies the frames into the upload buffers at fps, off the game's threads (the copies, 3-4 MB per
// playing channel, would stutter inside the game's ExecuteCommandLists with two screens
// playing). It sleeps on g_stagerCv while no screen texture is live.
std::thread g_stager;
std::mutex g_stagerMutex;
std::condition_variable g_stagerCv;
std::atomic<std::uint64_t> g_statStageMicros{ 0 };      // time spent copying frames (staging thread)
std::atomic<std::uint64_t> g_statStageMaxMicros{ 0 };
std::atomic<std::uint64_t> g_statHookMaxMicros{ 0 };     // longest submission in the game's hook
std::uint64_t g_statHookMicros = 0;                       // under g_mutex
std::uint64_t g_statHookCalls = 0;

std::string hex(std::uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string ptr(const void* p)
{
    return hex(reinterpret_cast<std::uintptr_t>(p));
}

void info(const std::string& m)
{
    if (g_log) {
        g_log->info(m);
    }
}

void warn(const std::string& m)
{
    if (g_log) {
        g_log->warn(m);
    }
}

bool formatMatches(DXGI_FORMAT f)
{
    for (auto v : g_cfg.formats) {
        if (static_cast<DXGI_FORMAT>(v) == f) {
            return true;
        }
    }
    return false;
}

// The channel index whose placeholder texture this is, or -1.
int channelOf(const D3D12_RESOURCE_DESC& d)
{
    if (!g_cfg.enabled || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 ||
        d.MipLevels != 1 || d.SampleDesc.Count != 1 || !formatMatches(d.Format) ||
        (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
        return -1;
    }
    for (std::size_t i = 0; i < g_channels.size(); ++i) {
        if (d.Width == g_channels[i].width && d.Height == g_channels[i].height) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool isCandidate(const D3D12_RESOURCE_DESC& d)
{
    return channelOf(d) >= 0;
}

std::string describe(const D3D12_RESOURCE_DESC& d)
{
    return std::to_string(d.Width) + "x" + std::to_string(d.Height) + " fmt=" + std::to_string(d.Format) +
           " mips=" + std::to_string(d.MipLevels) + " slices=" + std::to_string(d.DepthOrArraySize) +
           " flags=" + hex(d.Flags) + " layout=" + std::to_string(d.Layout) + " align=" + hex(d.Alignment);
}

void maybeLogNearMiss(const char* api, const D3D12_RESOURCE_DESC& d)
{
    if (!g_cfg.logNearMisses || d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return;
    }
    // render targets, depth buffers and UAVs are never a placeholder texture: the game's 1280x800 targets used up
    // the whole log budget before any screen loaded
    constexpr auto kNotATexture = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (d.Flags & kNotATexture) {
        return;
    }
    const bool close = std::any_of(g_channels.begin(), g_channels.end(), [&d](const Channel& c) {
        return d.Width == c.width || d.Height == c.height;
    });
    // any single-mip RGBA8 texture looks like a placeholder: logged once per size, whatever the size (a placeholder
    // may not appear at its own size)
    const bool rgba = d.MipLevels == 1 && d.Format >= DXGI_FORMAT_R8G8B8A8_TYPELESS && d.Format <= DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (!close && !rgba) {
        return;
    }
    {
        static std::mutex seenMutex;
        static std::set<std::uint64_t> seen;
        std::lock_guard lock(seenMutex);
        if (!seen.insert(d.Width << 20 ^ d.Height << 4 ^ d.Format).second) {
            return;
        }
    }
    if (g_nearMissLogs.fetch_add(1) < kMaxNearMissLogs) {
        info(std::string("Near miss (") + api + "): " + describe(d));
    }
}

// Called after a successful creation of a matching texture.
void registerSurface(const char* api, void* object, REFIID riid, const D3D12_RESOURCE_DESC& d, bool simultaneous,
                     const std::string& extra)
{
    ID3D12Resource* resource = nullptr;
    if (!object || FAILED(static_cast<IUnknown*>(object)->QueryInterface(IID_PPV_ARGS(&resource)))) {
        warn(std::string("Screen texture created by ") + api + " but the object is not an ID3D12Resource.");
        return;
    }
    (void)riid;
    ID3D12Device* device = nullptr;
    resource->GetDevice(IID_PPV_ARGS(&device));
    std::lock_guard lock(g_mutex);
    if (!g_device && device) {
        g_device = device;   // keeps the reference
        device = nullptr;
    }
    if (device) {
        if (device != g_device) {
            warn("Screen texture belongs to a different D3D12 device; ignored.");
            device->Release();
            resource->Release();
            return;
        }
        device->Release();
    }
    Surface s;
    s.resource = resource;   // the QueryInterface reference is ours
    s.desc = d;
    s.channel = static_cast<std::size_t>(channelOf(d));
    s.simultaneous = simultaneous;
    s.id = g_nextSurfaceId++;
    g_surfaces.push_back(s);
    Channel& ch = g_channels[s.channel];
    if (ch.live++ == 0 && ch.source) {
        ch.source->onSurfaceLive(true);
    }
    g_liveSurfaces.fetch_add(1);
    g_trackedSurfaces.fetch_add(1);
    ch.lastSeq = 0;   // a new texture needs the current frame even if it is not new
    g_stagerCv.notify_all();
    info(std::string("Screen surface #") + std::to_string(s.id) + " (channel " + std::to_string(ch.id) + ") matched via " +
         api + ": " + describe(d) + " simultaneous=" + (simultaneous ? "yes" : "no") + extra + " resource=" + ptr(resource));
}

// Adds ALLOW_SIMULTANEOUS_ACCESS to a matching texture desc when configured. Returns whether it did.
template <class Desc>
bool wantSimultaneous(const Desc& d)
{
    return g_cfg.simultaneousAccess && !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
}

// ---- creation hooks ----------------------------------------------------------------------------------------------

HRESULT STDMETHODCALLTYPE hkCreateCommittedResource(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
{
    if (!desc || !out || !isCandidate(*desc)) {
        if (desc) {
            maybeLogNearMiss("CreateCommittedResource", *desc);
        }
        return g_createCommitted(self, heap, heapFlags, desc, state, clear, riid, out);
    }
    D3D12_RESOURCE_DESC d = *desc;
    bool simultaneous = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
    HRESULT hr = E_FAIL;
    bool created = false;
    if (wantSimultaneous(d)) {
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        hr = g_createCommitted(self, heap, heapFlags, &d, state, clear, riid, out);
        if (SUCCEEDED(hr)) {
            simultaneous = true;
            created = true;
        } else {
            warn("Simultaneous-access creation failed (" + hex(static_cast<std::uint32_t>(hr)) + "); retrying as requested.");
            d = *desc;
        }
    }
    if (!created) {
        hr = g_createCommitted(self, heap, heapFlags, desc, state, clear, riid, out);
    }
    if (SUCCEEDED(hr)) {
        registerSurface("CreateCommittedResource", *out, riid, d, simultaneous,
                        " heapType=" + std::to_string(heap ? heap->Type : -1) + " initialState=" + hex(state));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreatePlacedResource(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset,
    const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid,
    void** out)
{
    if (!desc || !out || !isCandidate(*desc)) {
        if (desc) {
            maybeLogNearMiss("CreatePlacedResource", *desc);
        }
        return g_createPlaced(self, heap, offset, desc, state, clear, riid, out);
    }
    D3D12_RESOURCE_DESC d = *desc;
    bool simultaneous = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
    HRESULT hr = E_FAIL;
    bool done = false;
    if (wantSimultaneous(d)) {
        // Placed: the game sized its heap block for the original desc. Only switch if the new one fits the same block.
        D3D12_RESOURCE_DESC modified = d;
        modified.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        const auto before = self->GetResourceAllocationInfo(0, 1, desc);
        const auto after = self->GetResourceAllocationInfo(0, 1, &modified);
        if (after.SizeInBytes <= before.SizeInBytes && after.Alignment <= before.Alignment && (offset % after.Alignment) == 0) {
            hr = g_createPlaced(self, heap, offset, &modified, state, clear, riid, out);
            if (SUCCEEDED(hr)) {
                simultaneous = true;
                d = modified;
                done = true;
            } else {
                warn("Simultaneous-access placed creation failed (" + hex(static_cast<std::uint32_t>(hr)) + ").");
            }
        } else {
            warn("Simultaneous-access placed texture would not fit the game's heap block (size " +
                 std::to_string(before.SizeInBytes) + " -> " + std::to_string(after.SizeInBytes) + ", align " +
                 std::to_string(before.Alignment) + " -> " + std::to_string(after.Alignment) + ").");
        }
    }
    if (!done) {
        hr = g_createPlaced(self, heap, offset, desc, state, clear, riid, out);
    }
    if (SUCCEEDED(hr)) {
        registerSurface("CreatePlacedResource", *out, riid, d, simultaneous,
                        " heap=" + ptr(heap) + " offset=" + hex(offset) + " initialState=" + hex(state));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateReservedResource(ID3D12Device* self, const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out)
{
    if (desc && isCandidate(*desc)) {
        warn("Screen-sized texture created as a reserved (tiled) resource: " + describe(*desc) + " - not supported yet.");
    } else if (desc) {
        maybeLogNearMiss("CreateReservedResource", *desc);
    }
    return g_createReserved(self, desc, state, clear, riid, out);
}

HRESULT STDMETHODCALLTYPE hkCreateCommittedResource1(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, REFIID riid, void** out)
{
    if (!desc || !out || !isCandidate(*desc)) {
        if (desc) {
            maybeLogNearMiss("CreateCommittedResource1", *desc);
        }
        return g_createCommitted1(self, heap, heapFlags, desc, state, clear, session, riid, out);
    }
    D3D12_RESOURCE_DESC d = *desc;
    bool simultaneous = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
    HRESULT hr = E_FAIL;
    bool created = false;
    if (wantSimultaneous(d)) {
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        hr = g_createCommitted1(self, heap, heapFlags, &d, state, clear, session, riid, out);
        if (SUCCEEDED(hr)) {
            simultaneous = true;
            created = true;
        } else {
            d = *desc;
        }
    }
    if (!created) {
        hr = g_createCommitted1(self, heap, heapFlags, desc, state, clear, session, riid, out);
    }
    if (SUCCEEDED(hr)) {
        registerSurface("CreateCommittedResource1", *out, riid, d, simultaneous, " initialState=" + hex(state));
    }
    return hr;
}

D3D12_RESOURCE_DESC toDesc(const D3D12_RESOURCE_DESC1& d1)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = d1.Dimension;
    d.Alignment = d1.Alignment;
    d.Width = d1.Width;
    d.Height = d1.Height;
    d.DepthOrArraySize = d1.DepthOrArraySize;
    d.MipLevels = d1.MipLevels;
    d.Format = d1.Format;
    d.SampleDesc = d1.SampleDesc;
    d.Layout = d1.Layout;
    d.Flags = d1.Flags;
    return d;
}

HRESULT STDMETHODCALLTYPE hkCreateCommittedResource2(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, REFIID riid, void** out)
{
    if (!desc || !out || !isCandidate(toDesc(*desc))) {
        if (desc) {
            maybeLogNearMiss("CreateCommittedResource2", toDesc(*desc));
        }
        return g_createCommitted2(self, heap, heapFlags, desc, state, clear, session, riid, out);
    }
    D3D12_RESOURCE_DESC1 d = *desc;
    bool simultaneous = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
    HRESULT hr = E_FAIL;
    bool created = false;
    if (wantSimultaneous(d)) {
        d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        hr = g_createCommitted2(self, heap, heapFlags, &d, state, clear, session, riid, out);
        if (SUCCEEDED(hr)) {
            simultaneous = true;
            created = true;
        } else {
            d = *desc;
        }
    }
    if (!created) {
        hr = g_createCommitted2(self, heap, heapFlags, desc, state, clear, session, riid, out);
    }
    if (SUCCEEDED(hr)) {
        registerSurface("CreateCommittedResource2", *out, riid, toDesc(d), simultaneous, " initialState=" + hex(state));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreatePlacedResource1(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset,
    const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, REFIID riid,
    void** out)
{
    // Not modified (the heap block was sized for the original desc); copies use explicit transitions.
    const HRESULT hr = g_createPlaced1(self, heap, offset, desc, state, clear, riid, out);
    if (desc) {
        const auto d = toDesc(*desc);
        if (out && SUCCEEDED(hr) && isCandidate(d)) {
            registerSurface("CreatePlacedResource1", *out, riid, d,
                            (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0,
                            " heap=" + ptr(heap) + " offset=" + hex(offset) + " initialState=" + hex(state));
        } else if (!isCandidate(d)) {
            maybeLogNearMiss("CreatePlacedResource1", d);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreateCommittedResource3(ID3D12Device* self, const D3D12_HEAP_PROPERTIES* heap,
    D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
    const D3D12_CLEAR_VALUE* clear, ID3D12ProtectedResourceSession* session, UINT32 castCount,
    const DXGI_FORMAT* castFormats, REFIID riid, void** out)
{
    const HRESULT hr = g_createCommitted3(self, heap, heapFlags, desc, layout, clear, session, castCount, castFormats,
                                          riid, out);
    if (desc) {
        const auto d = toDesc(*desc);
        if (out && SUCCEEDED(hr) && isCandidate(d)) {
            registerSurface("CreateCommittedResource3", *out, riid, d,
                            (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0,
                            " initialLayout=" + std::to_string(layout) + " (enhanced barriers)");
        } else if (!isCandidate(d)) {
            maybeLogNearMiss("CreateCommittedResource3", d);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkCreatePlacedResource2(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset,
    const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear, UINT32 castCount,
    const DXGI_FORMAT* castFormats, REFIID riid, void** out)
{
    const HRESULT hr = g_createPlaced2(self, heap, offset, desc, layout, clear, castCount, castFormats, riid, out);
    if (desc) {
        const auto d = toDesc(*desc);
        if (out && SUCCEEDED(hr) && isCandidate(d)) {
            registerSurface("CreatePlacedResource2", *out, riid, d,
                            (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0,
                            " heap=" + ptr(heap) + " offset=" + hex(offset) + " initialLayout=" + std::to_string(layout) +
                                " (enhanced barriers)");
        } else if (!isCandidate(d)) {
            maybeLogNearMiss("CreatePlacedResource2", d);
        }
    }
    return hr;
}

// ---- upload ------------------------------------------------------------------------------------------------------

bool ensureUploadObjects()
{
    if (g_uploadReady) {
        return true;
    }
    if (g_uploadFailed || !g_device) {
        return false;
    }
    auto fail = [](const std::string& what, HRESULT hr) {
        warn("Upload setup failed at " + what + " (" + hex(static_cast<std::uint32_t>(hr)) + "); video surfaces disabled.");
        g_uploadFailed = true;
        return false;
    };
    HRESULT hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (FAILED(hr)) {
        return fail("CreateFence", hr);
    }
    for (auto& slot : g_ring) {
        hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
        if (FAILED(hr)) {
            return fail("CreateCommandAllocator", hr);
        }
        hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator, nullptr,
                                         IID_PPV_ARGS(&slot.list));
        if (FAILED(hr)) {
            return fail("CreateCommandList", hr);
        }
        slot.list->Close();
        slot.list->SetName(L"ScreenSFSE copy");
    }
    g_uploadReady = true;
    return true;
}

// The channel's upload buffers (one per ring slot), sized for its texture; created on its first frame.
bool ensureChannelUpload(Channel& ch, const D3D12_RESOURCE_DESC& texDesc)
{
    if (ch.ready) {
        return true;
    }
    if (ch.failed) {
        return false;
    }
    UINT rows = 0;
    UINT64 rowBytes = 0;
    g_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &ch.footprint, &rows, &rowBytes, &ch.bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = ch.bytes;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (std::size_t i = 0; i < kRing; ++i) {
        HRESULT hr = g_createCommitted(g_device, &heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ,
                                       nullptr, IID_PPV_ARGS(&ch.upload[i]));
        D3D12_RANGE none{ 0, 0 };
        if (SUCCEEDED(hr)) {
            hr = ch.upload[i]->Map(0, &none, reinterpret_cast<void**>(&ch.mapped[i]));
        }
        if (FAILED(hr)) {
            warn("Channel " + std::to_string(ch.id) + " upload buffer failed (" + hex(static_cast<std::uint32_t>(hr)) + ").");
            ch.failed = true;
            return false;
        }
        ch.upload[i]->SetName(L"ScreenSFSE upload");
    }
    info("Channel " + std::to_string(ch.id) + " upload ring ready: " + std::to_string(kRing) + " x " +
         std::to_string(ch.bytes) + " bytes, row pitch " + std::to_string(ch.footprint.Footprint.RowPitch));
    ch.ready = true;
    return true;
}

// Drops surfaces the game has released, once our copies into them are complete.
void retireSurfaces(UINT64 completed)
{
    for (auto it = g_surfaces.begin(); it != g_surfaces.end();) {
        if (!it->released) {
            it->resource->AddRef();
            const ULONG refs = it->resource->Release();
            if (refs <= 1) {
                it->released = true;
                g_liveSurfaces.fetch_sub(1);
                Channel& ch = g_channels[it->channel];
                if (--ch.live == 0 && ch.source) {
                    ch.source->onSurfaceLive(false);
                }
                info("Screen surface #" + std::to_string(it->id) + " (channel " + std::to_string(ch.id) + ") released by the game.");
            }
        }
        if (it->released && completed >= it->lastFence) {
            it->resource->Release();
            it = g_surfaces.erase(it);
            g_trackedSurfaces.fetch_sub(1);
            continue;
        }
        ++it;
    }
}

void chooseQueue(ID3D12CommandQueue* queue, std::chrono::steady_clock::time_point now)
{
    QueueStat* stat = nullptr;
    for (auto& q : g_queues) {
        if (q.queue == queue) {
            stat = &q;
            break;
        }
    }
    if (!stat) {
        for (auto& q : g_queues) {
            if (!q.queue) {
                q.queue = queue;
                q.direct = queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
                stat = &q;
                info(std::string("Command queue seen: ") + ptr(queue) + (q.direct ? " DIRECT" : " (not direct)"));
                break;
            }
        }
    }
    if (stat && stat->direct) {
        ++stat->calls;
    }
    if (now - g_queueWindowStart >= std::chrono::seconds(1)) {
        QueueStat* best = nullptr;
        for (auto& q : g_queues) {
            if (q.queue && q.direct && (!best || q.calls > best->calls)) {
                best = &q;
            }
        }
        if (best && best->queue != g_chosenQueue) {
            g_chosenQueue = best->queue;
            info("Copy queue: " + ptr(g_chosenQueue) + " (" + std::to_string(best->calls) + " submissions/s)");
        }
        for (auto& q : g_queues) {
            q.calls = 0;
        }
        g_queueWindowStart = now;
    }
}

// The game's submission hook: retires released textures and submits a slot the staging thread has filled. No frame
// copies here (3-4 MB per playing channel on the game's render thread stutters).
void uploadFrame(ID3D12CommandQueue* queue)
{
    std::unique_lock lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto now = start;
    chooseQueue(queue, now);
    if (queue != g_chosenQueue) {
        return;
    }
    // The hook runs on every game submission (~800/s): the bookkeeping below only when a slot is ready to go, or every
    // 100 ms (textures the game released while nothing new was staged, e.g. paused, are still let go).
    static std::chrono::steady_clock::time_point nextHousekeeping{};
    if (g_ring[g_submitNext].state != SlotState::Ready && now < nextHousekeeping) {
        return;
    }
    nextHousekeeping = now + std::chrono::milliseconds(100);
    if (!ensureUploadObjects()) {
        return;
    }
    const UINT64 completed = g_fence->GetCompletedValue();
    retireSurfaces(completed);
    if (g_liveSurfaces.load() == 0) {
        return;
    }
    // upload buffers for channels whose first texture appeared (the staging thread only fills ready channels)
    for (const auto& s : g_surfaces) {
        if (!s.released) {
            ensureChannelUpload(g_channels[s.channel], s.desc);
        }
    }
    const std::size_t slotIndex = g_submitNext;
    Slot& slot = g_ring[slotIndex];
    if (slot.state != SlotState::Ready) {
        return;
    }
    if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator, nullptr))) {
        return;
    }
    int copies = 0;
    for (auto& s : g_surfaces) {
        if (s.released || !slot.fresh[s.channel]) {
            continue;
        }
        const Channel& ch = g_channels[s.channel];
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = s.resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (!s.simultaneous) {
            barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(g_cfg.assumedState);
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            slot.list->ResourceBarrier(1, &barrier);
        }
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = s.resource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = ch.upload[slotIndex];
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = ch.footprint;
        src.PlacedFootprint.Footprint.Format = s.desc.Format;
        slot.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (!s.simultaneous) {
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            slot.list->ResourceBarrier(1, &barrier);
        }
        s.lastFence = g_fenceValue + 1;
        ++copies;
    }
    if (FAILED(slot.list->Close())) {
        return;
    }
    if (copies > 0) {
        ID3D12CommandList* lists[] = { slot.list };
        g_executeCommandLists(queue, 1, lists);
        queue->Signal(g_fence, ++g_fenceValue);
        slot.fence = g_fenceValue;
        g_statUploads += static_cast<std::uint64_t>(copies);
        static bool firstLogged = false;
        if (!firstLogged) {
            firstLogged = true;
            info("First frame uploaded on queue " + ptr(queue) + " (frames staged on ScreenSFSE's own thread).");
        }
    }
    std::fill(slot.fresh.begin(), slot.fresh.end(), 0);
    slot.state = SlotState::Idle;
    g_submitNext = (g_submitNext + 1) % kRing;
    const auto micros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
    g_statHookMicros += micros;
    ++g_statHookCalls;
    if (micros > g_statHookMaxMicros.load()) {
        g_statHookMaxMicros.store(micros);
    }
}

// Every 10 s while frames flow, from the staging thread (no file writes in the game's hook).
void logStats()
{
    std::string line;
    {
        std::lock_guard lock(g_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (now - g_lastStats < std::chrono::seconds(10)) {
            return;
        }
        g_lastStats = now;
        if (g_statUploads || g_statBusy) {
            const std::uint64_t frames = g_statUploads ? g_statUploads : 1;
            const std::uint64_t calls = g_statHookCalls ? g_statHookCalls : 1;
            line = "Stats: " + std::to_string(g_statUploads) + " frame copies, " + std::to_string(g_statBusy) +
                   " skipped (GPU busy), " + std::to_string(g_surfaces.size()) + " surface(s); staging thread " +
                   std::to_string(g_statStageMicros.exchange(0) / frames) + " us per copy (max " +
                   std::to_string(g_statStageMaxMicros.exchange(0)) + "), game hook " +
                   std::to_string(g_statHookMicros / calls) + " us per submission (max " +
                   std::to_string(g_statHookMaxMicros.exchange(0)) + ")";
        }
        g_statUploads = g_statBusy = g_statHookMicros = g_statHookCalls = 0;
    }
    if (!line.empty()) {
        info(line);
    }
}

// One frame of one channel to stage: what the staging thread needs without holding g_mutex.
struct StageJob
{
    std::size_t channel{ 0 };
    std::shared_ptr<FrameSource> source;
    std::uint8_t* dst{ nullptr };
    std::uint32_t pitch{ 0 };
    std::uint32_t width{ 0 };
    std::uint32_t height{ 0 };
    std::uint64_t lastSeq{ 0 };
    bool osdOn{ false };
    Osd osd;
    std::uint64_t seq{ 0 };
};

// The staging thread: at fps, fills the next free ring slot with every live channel's new frame (and its notice).
// Sleeps while no screen texture is live. Slots are filled and submitted in the same order; a slot is reused only
// once the hook has submitted it and the GPU has finished with it (its fence).
void stagerLoop()
{
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / g_cfg.fps));
    auto next = std::chrono::steady_clock::now();
    std::vector<StageJob> jobs;
    for (;;) {
        if (g_liveSurfaces.load() == 0) {
            // the notify comes from the texture hook without this mutex: the timeout covers a missed one
            std::unique_lock wait(g_stagerMutex);
            g_stagerCv.wait_for(wait, std::chrono::seconds(1), [] { return g_liveSurfaces.load() > 0; });
            next = std::chrono::steady_clock::now();
            continue;
        }
        // whole periods (not from the last frame), so the rate holds at fps
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (next <= now - period) {
            next = now + period;
        }
        std::this_thread::sleep_until(next);

        std::size_t slotIndex = 0;
        jobs.clear();
        {
            std::lock_guard lock(g_mutex);
            if (!g_uploadReady || !g_fence) {
                continue;
            }
            slotIndex = g_stageNext;
            Slot& slot = g_ring[slotIndex];
            if (slot.state != SlotState::Idle || g_fence->GetCompletedValue() < slot.fence) {
                ++g_statBusy;   // the hook has not submitted it yet, or the GPU still reads it
                continue;
            }
            slot.state = SlotState::Staging;
            if (slot.fresh.size() != g_channels.size()) {
                slot.fresh.assign(g_channels.size(), 0);
            }
            const auto stamp = std::chrono::steady_clock::now();
            for (std::size_t c = 0; c < g_channels.size(); ++c) {
                Channel& ch = g_channels[c];
                if (ch.live == 0 || !ch.source || !ch.ready) {
                    continue;
                }
                StageJob job;
                job.channel = c;
                job.source = ch.source;
                job.dst = ch.mapped[slotIndex] + ch.footprint.Offset;
                job.pitch = ch.footprint.Footprint.RowPitch;
                job.width = ch.width;
                job.height = ch.height;
                // while a notice shows (and once after, to clear it) the current frame is copied every tick, even when
                // it has not changed (paused, stopped): the notice is drawn over a fresh copy
                job.osdOn = ch.osd.until > stamp;
                job.osd = ch.osd;
                job.lastSeq = job.osdOn || ch.osdDrawn ? 0 : ch.lastSeq;
                jobs.push_back(std::move(job));
            }
        }
        const auto begin = std::chrono::steady_clock::now();
        for (auto& job : jobs) {
            job.seq = job.source->copyLatest(job.lastSeq, job.dst, job.pitch);
            if (job.seq != 0 && job.osdOn) {
                drawOsd(job.dst, job.pitch, job.width, job.height, job.osd);
            }
        }
        const auto micros = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
        bool any = false;
        {
            std::lock_guard lock(g_mutex);
            Slot& slot = g_ring[slotIndex];
            for (auto& job : jobs) {
                Channel& ch = g_channels[job.channel];
                if (job.seq == 0 || ch.source != job.source) {
                    continue;   // nothing new, or the source was replaced meanwhile (its first frame comes next tick)
                }
                ch.lastSeq = job.seq;
                ch.osdDrawn = job.osdOn;
                slot.fresh[job.channel] = 1;
                any = true;
            }
            slot.state = any ? SlotState::Ready : SlotState::Idle;
            if (any) {
                g_stageNext = (g_stageNext + 1) % kRing;
                g_statStageMicros.fetch_add(micros);
                if (micros > g_statStageMaxMicros.load()) {
                    g_statStageMaxMicros.store(micros);
                }
            }
        }
        jobs.clear();   // a replaced source may be released here, on this thread (its destructor joins its threads)
        logStats();
    }
}

void STDMETHODCALLTYPE hkExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    if (g_trackedSurfaces.load(std::memory_order_relaxed) > 0) {
        uploadFrame(queue);
    }
    g_executeCommandLists(queue, count, lists);
}

// ---- installation ------------------------------------------------------------------------------------------------

template <class Fn>
bool patchSlot(void** vtable, int index, Fn hook, Fn& original, const char* name)
{
    void** slot = vtable + index;
    if (*slot == reinterpret_cast<void*>(hook)) {
        return true;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        warn(std::string("VirtualProtect failed for ") + name);
        return false;
    }
    original = reinterpret_cast<Fn>(*slot);
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(hook));
    VirtualProtect(slot, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    info(std::string("Hooked ") + name + " (slot " + std::to_string(index) + ")");
    return true;
}

void** vtableOf(void* object)
{
    return *static_cast<void***>(object);
}
}

bool installVideoSurface(Logger& logger, const SurfaceConfig& config)
{
    g_log = &logger;
    g_cfg = config;
    g_channels.clear();
    for (const auto& c : g_cfg.channels) {
        Channel ch;
        ch.id = c.id;
        ch.width = c.width;
        ch.height = c.height;
        g_channels.push_back(ch);
    }
    if (!g_cfg.enabled) {
        info("Video surface disabled in the INI.");
        return true;
    }
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto createDevice = d3d12 ? reinterpret_cast<PFN_D3D12CreateDevice>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (!createDevice) {
        warn("d3d12.dll / D3D12CreateDevice not available; video surface disabled.");
        return false;
    }
    ID3D12Device* device = nullptr;
    HRESULT hr = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        warn("Probe D3D12CreateDevice failed (" + hex(static_cast<std::uint32_t>(hr)) + "); video surface disabled.");
        return false;
    }
    const auto& ix = d3d12vt::indices();
    void** base = vtableOf(device);
    bool ok = true;
    ok &= patchSlot(base, ix.createCommittedResource, &hkCreateCommittedResource, g_createCommitted, "CreateCommittedResource");
    ok &= patchSlot(base, ix.createPlacedResource, &hkCreatePlacedResource, g_createPlaced, "CreatePlacedResource");
    ok &= patchSlot(base, ix.createReservedResource, &hkCreateReservedResource, g_createReserved, "CreateReservedResource");

    // Newer creation methods only through a vtable proven to be the same one (single linear implementation).
    ID3D12Device10* device10 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10)))) {
        if (vtableOf(device10) == base) {
            ok &= patchSlot(base, ix.createCommittedResource1, &hkCreateCommittedResource1, g_createCommitted1, "CreateCommittedResource1");
            ok &= patchSlot(base, ix.createCommittedResource2, &hkCreateCommittedResource2, g_createCommitted2, "CreateCommittedResource2");
            ok &= patchSlot(base, ix.createPlacedResource1, &hkCreatePlacedResource1, g_createPlaced1, "CreatePlacedResource1");
            ok &= patchSlot(base, ix.createCommittedResource3, &hkCreateCommittedResource3, g_createCommitted3, "CreateCommittedResource3");
            ok &= patchSlot(base, ix.createPlacedResource2, &hkCreatePlacedResource2, g_createPlaced2, "CreatePlacedResource2");
        } else {
            warn("ID3D12Device10 has its own vtable; newer creation methods are not hooked.");
        }
        device10->Release();
    } else {
        info("ID3D12Device10 not available; newer creation methods are not hooked.");
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr) && queue) {
        ok &= patchSlot(vtableOf(queue), ix.executeCommandLists, &hkExecuteCommandLists, g_executeCommandLists,
                        "ID3D12CommandQueue::ExecuteCommandLists");
        queue->Release();
    } else {
        warn("Probe CreateCommandQueue failed (" + hex(static_cast<std::uint32_t>(hr)) + ").");
        ok = false;
    }
    device->Release();

    std::string list;
    for (const auto& c : g_channels) {
        list += (list.empty() ? "" : ", ") + std::to_string(c.id) + "=" + std::to_string(c.width) + "x" + std::to_string(c.height);
    }
    info("Video surface armed: channels " + list + " @ " + std::to_string(g_cfg.fps) + " fps, simultaneous=" +
         (g_cfg.simultaneousAccess ? "on" : "off") + ", assumedState=" + hex(g_cfg.assumedState));
    if (!g_stager.joinable()) {
        g_stager = std::thread(stagerLoop);   // lives as long as the game; sleeps while no screen texture is live
        g_stager.detach();
    }
    return ok;
}

void setFrameSource(int channel, std::shared_ptr<FrameSource> source)
{
    std::shared_ptr<FrameSource> old;
    {
        std::lock_guard lock(g_mutex);
        const auto it = std::find_if(g_channels.begin(), g_channels.end(), [channel](const Channel& c) { return c.id == channel; });
        if (it == g_channels.end() || source == it->source) {
            return;
        }
        old = std::move(it->source);
        it->source = std::move(source);
        it->lastSeq = 0;
        if (it->live > 0) {
            if (old) {
                old->onSurfaceLive(false);
            }
            if (it->source) {
                it->source->onSurfaceLive(true);
            }
        }
    }
    // `old` is released here, outside the lock: a source's destructor may join its threads
}

bool surfaceLive(int channel)
{
    std::lock_guard lock(g_mutex);
    const auto it = std::find_if(g_channels.begin(), g_channels.end(), [channel](const Channel& c) { return c.id == channel; });
    return it != g_channels.end() && it->live > 0;
}

void showOsd(int channel, const Osd& osd)
{
    std::lock_guard lock(g_mutex);
    const auto it = std::find_if(g_channels.begin(), g_channels.end(), [channel](const Channel& c) { return c.id == channel; });
    if (it != g_channels.end()) {
        it->osd = osd;
    }
}

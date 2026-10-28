#include "SwitifyClient.hpp"

#include <switch.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace cards::switify {
namespace {

// The service's IPC protocol (Switify's ipc_protocol.h, API 1.x): command
// numbers and the layout of what they answer.
constexpr std::uint64_t kProgramId = 0x4200000000535754ULL;
constexpr const char* kServiceName = "switify";
enum : std::uint32_t {
    CmdGetStatus = 2,
    CmdPlayPause = 10,
    CmdNext = 13,
    CmdPrevious = 14,
    CmdSetVolume = 15,
    CmdSetShuffle = 17,
    CmdSetRepeat = 18,
    CmdBrowseHeader = 20,
    CmdBrowseItem = 21,
    CmdBrowseLibrary = 31,
    CmdBrowseOpen = 33,
    CmdBrowseBack = 34,
    CmdBrowsePlay = 35,
    CmdBrowseQueue = 37,
};
enum : std::uint32_t { ServiceReady = 4, PlaybackPlaying = 2, PlaybackBuffering = 1 };
enum : std::uint32_t { FlagShuffle = 1u << 2, FlagRepeatOne = 1u << 3, FlagRepeatAll = 1u << 4, FlagHasTrack = 1u << 6 };
enum : std::uint32_t { BrowseLoading = 1 };

struct Status {
    std::uint32_t structSize, abiVersion;
    std::uint64_t sequence, monotonicMs;
    std::uint32_t serviceState, playbackState, flags, positionMs, durationMs, volumePermille, queueIndex, queueCount,
        lastResult, reserved0;
    char title[64], artist[48], album[40];
};
static_assert(sizeof(Status) == 216, "Switify status layout");

struct BrowseHeader {
    std::uint32_t structSize, abiVersion;
    std::uint64_t generation;
    std::uint32_t state, depth, itemCount, flags;
    char title[64], subtitle[64], message[56];
};
static_assert(sizeof(BrowseHeader) == 216, "Switify browse header layout");

struct BrowseItem {
    std::uint32_t structSize, abiVersion;
    std::uint64_t generation;
    std::uint32_t kind, index, durationMs, flags;
    char name[80], subtitle[64], detail[40];
};
static_assert(sizeof(BrowseItem) == 216, "Switify browse item layout");

struct BrowseRequest {
    std::uint64_t generation;
    std::uint32_t index, reserved;
    char query[128];
};
static_assert(sizeof(BrowseRequest) == 144, "Switify browse request layout");

std::mutex g_mu; // one session, shared by the worker and the UI thread
Service g_service{};

std::string text(const char* s, size_t n) { // up to the first 0, never past n
    const void* end = std::memchr(s, 0, n);
    return std::string(s, end ? static_cast<const char*>(end) - s : n);
}

bool connected() { // with g_mu held
    if (serviceIsActive(&g_service)) return true;
    return R_SUCCEEDED(smGetService(&g_service, kServiceName));
}

bool running() {
    u64 pid = 0;
    if (R_FAILED(pmdmntInitialize())) return false;
    const bool up = R_SUCCEEDED(pmdmntGetProcessId(&pid, kProgramId)) && pid != 0;
    pmdmntExit();
    return up;
}

bool simple(std::uint32_t cmd) {
    std::lock_guard<std::mutex> lk(g_mu);
    return connected() && R_SUCCEEDED(serviceDispatch(&g_service, cmd));
}

bool withU32(std::uint32_t cmd, std::uint32_t v) {
    std::lock_guard<std::mutex> lk(g_mu);
    return connected() && R_SUCCEEDED(serviceDispatchIn(&g_service, cmd, v));
}

bool browse(std::uint32_t cmd, std::uint64_t generation, std::uint32_t index) {
    BrowseRequest req{};
    req.generation = generation;
    req.index = index;
    std::lock_guard<std::mutex> lk(g_mu);
    return connected() && R_SUCCEEDED(serviceDispatchIn(&g_service, cmd, req));
}

} // namespace

bool start() {
    if (running()) return true;
    if (R_FAILED(pmshellInitialize())) return false;
    NcmProgramLocation location{};
    location.program_id = kProgramId;
    location.storageID = NcmStorageId_None;
    u64 pid = 0;
    const bool ok = R_SUCCEEDED(pmshellLaunchProgram(0, &location, &pid));
    pmshellExit();
    return ok;
}

State read(std::uint64_t known, const std::vector<Song>& keep) {
    State s;
    s.installed = true;
    if (!running()) {
        std::lock_guard<std::mutex> lk(g_mu);
        if (serviceIsActive(&g_service)) serviceClose(&g_service);
        s.status = "Starting Spotify...";
        return s;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (!connected()) {
        s.status = "Starting Spotify...";
        return s;
    }
    s.running = true;
    Status st{};
    if (R_FAILED(serviceDispatchOut(&g_service, CmdGetStatus, st)) || st.structSize != sizeof(Status)) {
        serviceClose(&g_service);
        s.running = false;
        s.status = "Spotify isn't answering";
        return s;
    }
    s.ready = st.serviceState == ServiceReady;
    if (!s.ready)
        s.status = st.serviceState <= 1 ? "Open Spotify once to sign in" : st.serviceState <= 3 ? "Connecting to Spotify..."
                                                                                                : "Spotify had a problem";
    s.playing = st.playbackState == PlaybackPlaying || st.playbackState == PlaybackBuffering;
    s.shuffle = st.flags & FlagShuffle;
    s.repeat = (st.flags & FlagRepeatOne) ? 1 : (st.flags & FlagRepeatAll) ? 2 : 0;
    s.hasTrack = st.flags & FlagHasTrack;
    s.positionMs = st.positionMs;
    s.durationMs = st.durationMs;
    s.volume = st.volumePermille;
    s.title = text(st.title, sizeof(st.title));
    s.artist = text(st.artist, sizeof(st.artist));
    s.album = text(st.album, sizeof(st.album));

    BrowseHeader h{};
    if (R_FAILED(serviceDispatchOut(&g_service, CmdBrowseHeader, h)) || h.structSize != sizeof(BrowseHeader)) return s;
    s.generation = h.generation;
    s.loading = h.state == BrowseLoading;
    s.pageTitle = text(h.title, sizeof(h.title));
    if (h.generation == known && !s.loading) { // the same page: the songs we have
        s.songs = keep;
        return s;
    }
    const std::uint32_t n = std::min<std::uint32_t>(h.itemCount, 100);
    for (std::uint32_t i = 0; i < n && !s.loading; ++i) {
        BrowseItem it{};
        if (R_FAILED(serviceDispatchInOut(&g_service, CmdBrowseItem, i, it)) || it.structSize != sizeof(BrowseItem)) break;
        Song song;
        song.name = text(it.name, sizeof(it.name));
        song.subtitle = text(it.subtitle, sizeof(it.subtitle));
        song.kind = it.kind;
        song.index = it.index;
        s.songs.push_back(std::move(song));
    }
    return s;
}

bool playPause() { return simple(CmdPlayPause); }
bool next() { return simple(CmdNext); }
bool previous() { return simple(CmdPrevious); }
bool setShuffle(bool on) { return withU32(CmdSetShuffle, on ? 1u : 0u); }
bool setRepeat(int mode) { return withU32(CmdSetRepeat, static_cast<std::uint32_t>(mode)); }
bool setVolume(std::uint32_t permille) { return withU32(CmdSetVolume, permille); }
bool showQueue() { return browse(CmdBrowseQueue, 0, 0); }
bool showLibrary() { return browse(CmdBrowseLibrary, 0, 0); }
bool open(std::uint64_t generation, std::uint32_t index) { return browse(CmdBrowseOpen, generation, index); }
bool play(std::uint64_t generation, std::uint32_t index) { return browse(CmdBrowsePlay, generation, index); }
bool back(std::uint64_t generation) { return browse(CmdBrowseBack, generation, 0); }

void close() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (serviceIsActive(&g_service)) serviceClose(&g_service);
}

} // namespace cards::switify

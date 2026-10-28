#pragma once
// Switify (Spotify for Switch) from the Music panel: talks to the "switify"
// background service (program 0x4200000000535754) the same way its Ultrahand
// overlay does, so the songs, the queue and the controls show up in CardsNX.

#include <cstdint>
#include <string>
#include <vector>

namespace cards::switify {

struct Song {
    std::string name, subtitle;
    std::uint32_t kind = 0;  // 0 section, 1 track, 2 playlist, 3 album, 4 artist
    std::uint32_t index = 0; // what BrowsePlay / BrowseOpen take
};

struct State {
    bool installed = false; // the service is on the SD card
    bool running = false;   // it answers
    bool ready = false;     // signed in and connected to Spotify
    bool playing = false, shuffle = false, hasTrack = false;
    int repeat = 0;         // 0 off, 1 one, 2 all
    std::uint32_t positionMs = 0, durationMs = 0, volume = 0; // volume: 0..1000
    std::string title, artist, album, status;
    // The page the service is browsing (the queue, a playlist, the library).
    std::uint64_t generation = 0;
    bool loading = false;
    std::string pageTitle;
    std::vector<Song> songs;
};

// Starts the service when it isn't running (as the overlay does). Slow: a worker's job.
bool start();
// One snapshot: status, and the browse page when its generation changed (`known`).
State read(std::uint64_t known, const std::vector<Song>& keep);

// Controls. Quick IPC calls; false when the service isn't there.
bool playPause();
bool next();
bool previous();
bool setShuffle(bool on);
bool setRepeat(int mode);
bool setVolume(std::uint32_t permille);
bool showQueue();
bool showLibrary();
bool open(std::uint64_t generation, std::uint32_t index);
bool play(std::uint64_t generation, std::uint32_t index);
bool back(std::uint64_t generation);
void close();

} // namespace cards::switify

// One emulated DS: melonDS's NDS plus what BananaDS adds around it (saves next to the game's name in the save
// folder, the microphone, Wi-Fi, savestates with a picture for the in-game menu).
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ARCodeFile.h"
#include "Args.h"
#include "NDSCart/CartCommon.h"
#include "config.h"
#include "mic.h"
#include "prof.h"

namespace melonDS { class NDS; class Net; class BananaRenderer; class Firmware; }

namespace bananads {
// with log.time: "[start] <what>" in the log, the start of a game timed step by step (platform.cpp)
void start_mark(const char* what);
bool rom_unmap(uint8_t* rom);  // a ROM Emu::load_rom mapped: unmapped, true (melonDS's Platform::ROMRelease)
}

namespace bananads {

// DS buttons as melonDS's key mask wants them (bit set = pressed; Emu inverts).
enum Key : uint32_t {
    KeyA = 1 << 0, KeyB = 1 << 1, KeySelect = 1 << 2, KeyStart = 1 << 3,
    KeyRight = 1 << 4, KeyLeft = 1 << 5, KeyUp = 1 << 6, KeyDown = 1 << 7,
    KeyR = 1 << 8, KeyL = 1 << 9, KeyX = 1 << 10, KeyY = 1 << 11,
};

// sched.emu_excl: the core the emulator's thread has to itself (-1: none); a thread it starts later moves itself off
// that core with sched_off_emu_core() (else it would inherit the emulator's one-core mask)
extern int g_emu_core;
// sched.emu_share (1: Gengis's workers, 2: + the 2D thread): those threads may use the emulator's core too, the
// emulator's thread at SCHED_FIFO 1 there so it takes the core back the moment it wakes (its pacing sleep is when
// the 3D frame it started at line 215 is drawn)
extern int g_emu_share;
void sched_off_emu_core();

class DirectLink;  // wifi.h

class Emu {
public:
    explicit Emu(const Config& cfg);
    ~Emu();
    // the quit's part of the destructor: the save written out, the console stopped (once; the destructor's too)
    void shutdown();

    bool load_rom(const std::string& path);  // also loads its save; false with a logged reason
    bool is_dsi() const { return dsi_; }  // the game runs on a DSi (console = auto picks it for DSi-only games)
    // a message for the player, shown on the panels when the game starts (the frontend takes it once): what's missing
    void notice(const char* line1, const char* line2);
    bool take_notice(std::string& line1, std::string& line2);
    void run_frame();                          // one DS frame (~1/59.83 s)
    void diag_sleep(long long us);             // the pacer slept us (diag-respuesta, LAN diagnostics)
    // the game's own frames: its 3D buffer swaps so far (a game that can't finish a frame in one DS frame swaps
    // every other one or less, as on a DS, while the emulator still runs 60 DS frames a second); any thread
    long swaps3d() const { return swaps3d_.load(std::memory_order_relaxed); }
    void set_keys(uint32_t pressed);
    void set_lid(bool closed);                 // closing the lid puts the game to sleep
    void touch(int x, int y);                  // bottom-screen pixels, 0..255 x 0..191
    void release_touch();

    // 256x192 XRGB8888 each (times scale() both ways), valid until the next run_frame().
    const uint32_t* top() const;
    const uint32_t* bottom() const;
    int scale() const { return scale_; }  // render.scale: 1, or 2 with Gengis Engine and the Banana 2D engine
    // The next frames drawn straight into these instead (frontend=kms: the scanout buffers), a screen at scale() each
    // with rows packed, only ever written; top() and bottom() then return them once the frame is finished. Set
    // between frames. False when the renderer can't (only the Banana 2D engine's can); null, null: back to its own.
    bool draw_into(uint32_t* top, uint32_t* bottom);
    // render.2d_out_skip: the frontend wrote into a buffer draw_into was given (overlay, copy, new buffer)
    void output_touched(const void* buf);
    // render.2d_lazy (the Banana 2D engine on its thread): the last run_frame()'s pictures may be finished by the 2D
    // thread after it returns. frame_fence() right after run_frame(), then wait_frame(fence) (any thread) before
    // reading the pictures drawn into draw_into's buffers; false: nothing to wait for
    bool set_lazy_2d(bool on);
    // kms.grid_cpu: the LCD grid table for the Banana 2D engine's 4x output (null: off); between frames, emulator's thread
    bool set_grid4(const unsigned char (*lut)[256]);
    // kms.rgb565: draw_into's buffers take RGB565 pixels (rows of 2 bytes) from the next frame on; true when they will
    // (render.scale 4, the Banana 2D engine, no grid, its self-test passed). Between frames, emulator's thread
    bool set_rgb565(bool on);
    // the pictures top()/bottom() return are RGB565 (uint16_t, as set_rgb565); pixel(): one of them as XRGB8888
    bool frame_565() const;
    static uint32_t px565(uint16_t c) {   // (as the display expands 5/6 bits to 8: the top bits repeated)
        const uint32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
        return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
    }
    bool frame_fence(uint32_t& fence) const;
    void wait_frame(uint32_t fence) const;

    int audio(int16_t* stereo, int max_frames);  // what the SPU made since the last call
    void set_audio_rate(double hz);              // the SPU's output rate (48000: the DS's speed; kms.pace=display)
    int audio_backlog() const;                   // what the SPU made and audio() hasn't read yet, in stereo frames
    // the sound's speed against the game's (audio.servo): below 1 stretches it (more samples per emulated second);
    // any thread may ask, the emulator's thread applies it between frames (never inside the SPU's mix)
    void set_audio_skew(double s) { skew_req_.store(s, std::memory_order_relaxed); }

    // Run-ahead (runahead = 1 or 2, experimental): right after run_frame(), the state kept in memory, n more frames
    // run with the same buttons (the last one's picture is the one shown: before_last() points draw_into at it,
    // after_last() may copy it), and the state put back, so the game answers a press n frames sooner than its own
    // lag. Those frames are undone: no sound, no save writes, no microphone or network traffic (hidden()), and the
    // JIT's blocks compiled meanwhile are dropped (melonDS patch 0091). Within ~2 s of a display capture the 3D frame
    // is drawn again from the state put back (the capture would otherwise carry the frame ahead's 3D into the game's
    // VRAM). False when it couldn't run: the picture is run_frame()'s then. runahead_ok(): whether it can (not with
    // local wireless, on a DSi, or with melonDS's own 3D renderer on its thread).
    bool runahead_ok() const;
    bool run_ahead(int n, const std::function<void()>& before_last, const std::function<void()>& after_last);
    bool hidden() const { return hidden_; }
    double runahead_save_ms() const { return ra_save_ms_; }  // the last snapshot's save and load, ms
    double runahead_load_ms() const { return ra_load_ms_; }
    size_t runahead_bytes() const { return ra_len_; }

    bool save_state(int slot);  // slot 0 = the quit/resume state
    bool load_state(int slot);
    void restart();  // the game from its start (the in-game menu's Restart game): a reset, booted straight into it
    // wifi.local (LAN): the network link serviced while the game is paused (the in-game menu), as melonDS's own
    // frontend does every 75 ms while paused; otherwise ENet times the peers out after 5-30 s and the LAN session
    // is lost for good. Emulator thread; serialised with the menu's save_state/load_state/restart (net_mu_)
    void service_net();
    // wifi.local: another handheld is in a local wireless game with this one (LAN's MP-active players, or another
    // player connected while the game's Wi-Fi is in a multiplayer mode). Updated on the emulator's thread after
    // each frame and each service_net; any thread may ask (the menu turns Save, Load and Restart off)
    bool mp_active() const { return mp_active_.load(std::memory_order_relaxed); }

    // ---- wifi.local from the in-game menu (Quick settings > LAN multiplayer) ----
    // The menu asks from its own thread; the emulator's thread carries it out between two frames or in the paused
    // menu's service loop (service_net), never while another thread runs the game. Joining waits up to 5 s for the
    // host (melonDS's StartClient): the state says Busy meanwhile, and the menu stays usable.
    enum class LanMode { Off, Host, Join, Auto };      // Join: looking for hosts, then in one host's session;
                                                       // Auto: joins a host of this game, or hosts (lan_auto_tick)
    // Auto's steps: waiting for the game to turn its wifi on; listening for hosts; joining one; hosting; joined; the
    // game online through Nintendo WFC (no local session then); stopped by a problem (a UDP port taken: until the
    // game's wifi goes off for 3 s and on again, or Off then Auto). Auto has no Disconnect: Off leaves
    enum class AutoPhase { Wait, Search, Joining, Host, Joined, Online, Held };
    enum class LanState { Idle, Busy, Ready, Failed };  // Idle: off, or looking; Ready: hosting, or joined
    struct LanPlayer { std::string name; bool host = false, me = false; int ping_ms = -1; };
    struct LanHost { std::string name, addr; int players = 0, max = 0; };
    struct LanStatus {
        bool built = false;          // this build has local wireless (BANANADS_LAN)
        LanMode mode = LanMode::Off;
        LanState state = LanState::Idle;
        std::string addr;            // Join: the host's address (joined, joining, or the one that failed)
        std::string msg;             // Failed: what went wrong
        int max_players = 4;         // the session's size, us included (2-16): ours (Host, Auto hosting) or the host's
        int host_players = 4;        // Host's size (wifi.local_players): a session Host or Auto starts
        std::vector<LanPlayer> players;  // the session's players, us included (Ready)
        std::vector<LanHost> hosts;      // Join, not joined: the hosts found on the network (each says so every 1 s)
        AutoPhase phase = AutoPhase::Wait;  // Auto: where it is
        bool host_untagged = false;      // Auto, joined: a host that doesn't say its game (desktop melonDS)
        bool no_net = false;             // no network (Wi-Fi off or not connected): the mode waits for one
        // Connection (wifi.local_link): router (the Wi-Fi network, as before) or direct (DirectLink, wifi.h: the
        // consoles on a cell of their own, no router; this one off its Wi-Fi network meanwhile)
        bool link_avail = false;         // the link script is there (link.script, BananaOS's wifi-link)
        bool link_direct = false;        // Connection: Direct
        int link_state = 0;              // DirectLink::State (0 down, 1 starting, 2 up, 3 stopping, 4 failed)
        std::string link_ip;             // ours on the link
        int link_freq = 0;               // MHz
    };
    LanStatus lan_status() const;             // any thread: a copy
    // max_players: Host's (0: as it was); Auto hosts with Host's. Requests are carried out in order
    void lan_request(LanMode mode, const std::string& addr = "", int max_players = 0);  // any thread
    // Connection: Direct (true) or Router; only with the mode Off (otherwise ignored). Any thread
    void lan_link_request(bool direct);
    std::string state_path(int slot) const;

    Mic& mic() { return mic_; }

    // For the tests and the differential harness (tools/difftest.cpp): the DS's state, read-only.
    const uint8_t* main_ram() const;      // 4 MB
    const uint8_t* dtcm() const;          // the ARM9's 16 KB data TCM (RetroAchievements reads it)
    const std::string& rom_path() const { return rom_path_; }
    const std::string& game() const { return game_; }
    void set_cheats(std::vector<melonDS::ARCode> codes);  // the Action Replay codes melonDS runs each frame (cheats.h)
    uint32_t main_ram_size() const;
    void cpu_regs(int cpu, uint32_t r[17]) const;  // cpu 0 = ARM9, 1 = ARM7; r[16] = CPSR
    const melonDS::NDS* nds() const { return nds_.get(); }  // (bananads-difftest's --timing reads its state)
    const Config& cfg() const { return cfg_; }

    // Platform callbacks (platform.cpp) reach the emulator through these.
    void write_save(const uint8_t* data, uint32_t len, uint32_t off, uint32_t n);
    // the game wrote the firmware (Platform::WriteFirmware): the WFC settings out to wifi.wfc_settings if changed
    void write_firmware(const melonDS::Firmware& fw);
    melonDS::Net* net() { return net_.get(); }
    void stop_requested() { stop_ = true; }
    bool stopped() const { return stop_; }

    static Emu* from(void* userdata) { return static_cast<Emu*>(userdata); }

private:
    void setup_wifi();
    void setup_local();
    void flush_save(bool now);
    void setup_slice();
    void set_clock();
    std::unique_ptr<melonDS::NDS> make_dsi(melonDS::NDSArgs&& args);
    std::optional<melonDS::DSi_NAND::NANDImage> setup_nand(const std::array<uint8_t, 0x10000>& arm7i);
    void check_secure_area();
    void set_dsi_region();
    void export_dsi_saves();
    uint64_t debug_pc_ = 0;
    const melonDS::NDSCart::CartCommon* dsi_cart_ = nullptr;  // (the game's, while make_dsi runs)
    bool dsi_nand_ = false;  // the NAND's working copy is in
    uint32_t dsi_title_cat_ = 0, dsi_title_id_ = 0;
    bool dsi_ = false;       // a DSi, not a DS
    bool dsi_bios_ = false;  // the DSi's own BIOS images, not the free ones
    bool dsi_user_nand_ = false;  // the NAND is a copy of the user's dsi_nand.bin (keeps its own region)
    bool key1_ = false;      // the free BIOS got the secure area's key from a DS bios7.bin
    bool secure_missing_ = false;
    std::vector<uint8_t> cia_tmd_;  // a .cia's title metadata (3DS format), for a NAND import
    std::mutex notice_mu_;
    std::string notice1_, notice2_;
    void update_mp_active();
    void lan_apply();      // the menu's wifi.local requests, if any, in order (emulator thread, net_mu_ held)
    void lan_refresh(bool now);  // lan_st_'s players and hosts from melonDS's LAN, twice a second (same)
#ifdef BANANADS_LAN
    void lan_stop();                                  // the session ended, discovery off (same)
    bool lan_start_host(int max_players);
    bool lan_start_find(const std::string& addr, const std::string& msg);
    bool lan_start_join(const std::string& addr);
    // Auto (emulator thread, net_mu_ held)
    void lan_auto_tick();                             // every 100 ms while Auto is on
    void auto_read_config();                          // Auto's test knobs (wifi.local_auto_*)
    void lan_read_test_modes();                       // the tests' Mode row picks (wifi.local_test_mode)
    bool auto_wifi_on() const;                        // the game's wifi, as Auto sees it
    void lan_auto_phase(AutoPhase p, const std::string& msg = "", const std::string& addr = "");
    void lan_auto_listen(const std::string& msg);     // listen 2 s + 0-1 s at random, then join or host
    bool lan_auto_join(uint32_t ip, bool tagged);
    void lan_auto_host();
    void lan_tag_update();                            // the beacon's game tag and Status (a host of any mode)
    void lan_net_check(bool now);                     // a usable network, every 1 s (same); its changes
    bool lan_net_wait(LanMode mode, const std::string& addr, int max_players);  // no network: the mode waits
    // the direct link (emulator thread, net_mu_ held)
    void lan_link_tick();                             // its status changed (DirectLink::generation)
    void lan_link_set(bool direct);                   // Connection changed (the mode Off)
    void lan_link_lost(const std::string& why);       // it failed or went: the mode Off, the reason shown
    bool lan_link_addr() const;                       // our link address still on an interface
#endif

    std::atomic<double> skew_req_{1.0};
    std::atomic<long> swaps3d_{0};
    bool lid_closed_ = false;  // the lid as set_lid last had it (emulator thread)
    uint32_t bank3d_ = 0;
    double skew_cur_ = 1.0;
    std::vector<int16_t> tap_;  // audio.tap: the mixer's 32768 Hz output (melonDS patch 0180), written at the quit
    std::string tap_path_;
    Config cfg_;
    std::unique_ptr<melonDS::NDS> nds_;
    std::unique_ptr<melonDS::Net> net_;
    Mic mic_;
    std::string rom_path_, save_path_, game_;
    bool stop_ = false;
    bool shut_ = false;  // shutdown() done
    bool local_ = false;
    std::atomic<bool> mp_active_{false};
    std::string wfc_path_;             // wifi.wfc_settings: where the WFC settings are kept ("" off)
    std::vector<uint8_t> wfc_last_;    // the settings as last loaded or saved (a write that changes nothing: no file)
    std::mutex net_mu_;  // service_net() against the menu thread's save_state/load_state/restart (LAN Begin/End)
    mutable std::mutex lan_mu_;   // lan_st_ and lan_req_ (the menu's thread and the emulator's)
    LanStatus lan_st_;
    struct LanReq { LanMode mode; std::string addr; int max_players; int link = -1; };  // link >= 0: Connection
    std::vector<LanReq> lan_req_;            // the menu's requests, not yet carried out (in order)
    void lan_apply_one(const LanReq& r);
#ifdef BANANADS_LAN
    bool lan_link_ready(const LanReq& r);   // direct: the link up for r's role; false: asked for, r waits for it
#endif
    std::atomic<bool> lan_pending_{false};
    uint64_t lan_refresh_us_ = 0;
    size_t lan_hosts_seen_ = 0;              // (the log: hosts found)
    std::string lan_players_seen_;            // (the log: who is in the session)
    char gamecode_[4] = {'#', '#', '#', '#'};  // the cart header's game code (the LAN beacon's tag)
    // Auto (emulator thread)
    bool auto_on_ = false;
    bool auto_now_ = false;                   // wifi.local_auto_start = now: no wait for the game's wifi (tests)
    std::vector<std::pair<long, bool>> auto_test_wifi_;  // wifi.local_auto_test_wifi: the game's wifi by frame (tests)
    std::vector<std::pair<long, LanMode>> lan_test_modes_;  // wifi.local_test_mode: the Mode row by frame (tests)
    bool lan_st_built_ = false;               // (setup_local ran: the network check runs)
    // the direct link (wifi.local_link = direct)
    std::unique_ptr<DirectLink> link_;        // the link script's runner (null: no script, no Connection row)
    bool link_direct_ = false;                // Connection: Direct
    bool link_used_ = false;                  // asked up once: down at the quit
    bool link_wait_ = false;                  // link_req_ waits for the link
    LanReq link_req_{LanMode::Off, "", 0};
    uint32_t link_gen_ = 0;                   // DirectLink::generation last seen
    int link_gone_ = 0;                       // checks in a row with the link up but our address gone
    bool net_ok_ = true;                      // a network interface up, connected, with an IPv4 address
    uint64_t net_check_us_ = 0;
    long auto_wifi_off_frames_ = 0;           // frames run in a row with the game's wifi off
    bool auto_wifi_was_ = false;              // (the log: the game's wifi off and on within a session)
    static constexpr long kAutoWifiGrace = 180;  // 3 s of the game's time: a wifi off that long ends an Auto host's session with nobody else in it
    static constexpr long kAutoWifiGraceShared = 1800;  // 30 s: the same for a session others are in (host or joined)
    AutoPhase auto_phase_ = AutoPhase::Wait;
    uint64_t auto_tick_us_ = 0, auto_deadline_us_ = 0, auto_host_since_us_ = 0;
    uint32_t auto_my_ip_ = 0;                 // our address, from our own beacon (0: not heard yet)
    uint32_t auto_lower_ip_ = 0, auto_lower_tick_ = 0;
    int auto_lower_count_ = 0;                // beacons in a row from a lower-address host of this game
    std::vector<std::pair<uint32_t, uint64_t>> auto_bad_;  // hosts that didn't take us, until when
    uint32_t auto_rng_ = 0;
    int scale_ = 1;
    melonDS::BananaRenderer* banana_ = nullptr;  // the renderer, when it's the Banana 2D engine's
    std::unique_ptr<uint8_t[]> save_copy_;
    uint32_t save_len_ = 0;
    uint64_t save_dirty_us_ = 0;  // when the game last wrote its save; 0 = written out
    // the save's writer (#33): the file is written and fsynced on a thread of its own, not the emulator's, where a
    // slow SD card's fsync (tens to hundreds of ms) stopped the game each time it saved
    std::thread save_thr_;
    std::mutex save_mu_;
    std::condition_variable save_cv_;
    std::vector<uint8_t> save_pending_;
    bool save_have_ = false, save_stop_ = false;
    void save_writer();
    long slice_stats_ = 0;        // core.slice_stats: log the slice's statistics every this many frames
    long slice_frames_ = 0;
    std::unique_ptr<Profiler> prof_;  // prof.hz: the emulator's thread sampled (prof.cpp), off by default
    long frames_run_ = 0;
    // run-ahead's snapshot (run_ahead)
    std::vector<uint8_t> ra_buf_;
    size_t ra_len_ = 0;
    bool hidden_ = false, ra_broken_ = false;
    bool ra_soft_threaded_ = false;  // render.renderer=soft with render.threaded: no run-ahead (emu.cpp)
    uint32_t ra_captures_ = 0;   // GPU::CaptureFrames when last seen
    long ra_capture_at_ = -100000;  // frames_run_ then
    double ra_save_ms_ = 0, ra_load_ms_ = 0;
    void note_capture();
};

}  // namespace bananads

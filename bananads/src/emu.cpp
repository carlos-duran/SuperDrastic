#include "emu.h"
#ifdef BANANADS_GENGIS
#include "gengis_renderer.h"
#endif

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <new>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <pthread.h>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "Args.h"
#include "GPU_Soft.h"
#ifdef BANANADS_2D
#include "GPU_Banana.h"
#endif
#include "NDS.h"
#include "NDSCart.h"
#include "Net.h"
#include "Net_Slirp.h"
#include "Platform.h"
#include "Savestate.h"
#include "wifi.h"

#ifdef BANANADS_LAN
#include "LAN.h"
#include "WifiAP.h"
#include "MPInterface.h"
#endif

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

namespace bananads {

static std::string basename_noext(const std::string& p) {
    size_t s = p.find_last_of('/');
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

static void mkdirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out.resize((size_t)n);
    f.seekg(0);
    return n == 0 || (bool)f.read((char*)out.data(), n);  // in one read: a 128 MB ROM byte by byte took seconds
}

// Write next to it, then rename: a save is never half written, whatever happens to the power.
static bool write_file_atomic(const std::string& path, const void* data, size_t n) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

static uint64_t now_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

Emu::Emu(const Config& cfg) : cfg_(cfg) {
    MicTuning mt;
    mt.threshold = cfg.real("mic.threshold", mt.threshold);
    mt.gate_k = cfg.real("mic.gate", mt.gate_k);
    mt.coupling_max = cfg.real("mic.coupling_max", mt.coupling_max);
    mt.hold_ms = (int)cfg.num("mic.hold_ms", mt.hold_ms);
    mic_.set_tuning(mt);
}

Emu::~Emu() {
    if (prof_) prof_->finish();  // (its report: the emulator's thread is done by now)
    flush_save(true);
#ifdef BANANADS_LAN
    lan_stop();  // at quit, before the statics go: the others hear that we leave (melonDS patch 0092)
#endif
    if (nds_) nds_->Stop();
    if (net_) net_->UnregisterInstance(0);
}

void Emu::setup_wifi() {
    std::string dns = cfg_.str("wifi.dns", "kaeru");
    if (dns == "off") return;
    uint32_t up = wfc_dns_address(dns);
    if (up == 0xFFFFFFFF) {
        Log(LogLevel::Warn, "[wifi] wifi.dns=%s isn't a preset or an address; using the system resolver\n",
            dns.c_str());
        up = 0;
    }
    net_ = std::make_unique<Net>();
    net_->RegisterInstance(0);
    Net* net = net_.get();
    auto to_ds = [net](const u8* data, int len) { net->RXEnqueue(data, len); };
    auto slirp = std::make_unique<Net_Slirp>(to_ds);
    net_->SetDriver(std::make_unique<WfcDns>(std::move(slirp), up, to_ds));
    Log(LogLevel::Info, "[wifi] on: internet through slirp, DNS %s\n", up ? dns.c_str() : "the system's");
}

// Local wireless with another handheld (Pokémon trades and battles, Mario Kart's local races, PictoChat):
// wifi.local = host on one, join:<the host's address> (or join: look for hosts, and pick one in the in-game menu's
// LAN multiplayer page) on the other, or auto on both (lan_auto_tick). Both on the same network. The menu can
// change it while the game runs (lan_request).
void Emu::setup_local() {
    const std::string mode = cfg_.str("wifi.local", "off");
#ifdef BANANADS_LAN
    {
        std::lock_guard<std::mutex> l(lan_mu_);
        lan_st_.built = true;
        lan_st_.max_players = (int)std::clamp(cfg_.num("wifi.local_players", 4), 2L, 16L);
        lan_st_.host_players = lan_st_.max_players;
    }
    lan_read_test_modes();
    lan_st_built_ = true;
    lan_net_check(true);
    if (mode == "off") return;
    const bool manual = mode == "host" || mode == "join" || mode.rfind("join:", 0) == 0;
    if (manual && lan_net_wait(mode == "host" ? LanMode::Host : LanMode::Join, mode.size() > 5 ? mode.substr(5) : "",
                               lan_status().max_players))
        ;  // (no network: it starts when there is one)
    else if (mode == "host") lan_start_host(lan_status().max_players);
    else if (mode == "auto") {
        auto_on_ = true;
        auto_read_config();
        lan_auto_phase(AutoPhase::Wait);
        Log(LogLevel::Info, "[wifi] local wireless auto: waiting for the game to turn its wifi on\n");
    }
    else if (mode == "join") lan_start_find("", "");
    else if (mode.rfind("join:", 0) == 0) lan_start_join(mode.substr(5));
    else Log(LogLevel::Warn, "[wifi] wifi.local=%s: not off, auto, host, join or join:<address>\n", mode.c_str());
    lan_refresh(true);
#else
    if (mode == "off") return;
    Log(LogLevel::Warn, "[wifi] wifi.local=%s, but this build has no local wireless (BANANADS_LAN)\n", mode.c_str());
#endif
}

Emu::LanStatus Emu::lan_status() const {
    std::lock_guard<std::mutex> l(lan_mu_);
    return lan_st_;
}

void Emu::lan_request(LanMode mode, const std::string& addr, int max_players) {
    std::lock_guard<std::mutex> l(lan_mu_);
    if (!lan_st_.built) return;
    if (max_players < 2 || max_players > 16) max_players = lan_st_.host_players;
    // kept in order, all carried out: Off then Auto before the emulator's thread gets to them still ends the session
    // first (the latest alone would be Auto, already on: nothing done, the old session kept)
    lan_req_.push_back(LanReq{mode, addr, max_players});
    // what the menu shows until the emulator's thread has done it
    if (mode == LanMode::Auto) {  // (Auto: what it does shows once the emulator's thread has it)
        if (lan_st_.mode != LanMode::Auto) lan_st_.phase = AutoPhase::Wait;
        lan_st_.mode = mode;
        lan_st_.msg.clear();
        lan_pending_.store(true, std::memory_order_release);
        return;
    }
    lan_st_.mode = mode;
    lan_st_.state = mode == LanMode::Off ? LanState::Idle : LanState::Busy;
    lan_st_.msg.clear();
    lan_st_.players.clear();
    if (mode == LanMode::Join) lan_st_.addr = addr;
    if (mode == LanMode::Host) lan_st_.max_players = lan_st_.host_players = max_players;
    lan_pending_.store(true, std::memory_order_release);
}

void Emu::lan_apply() {
    if (!lan_pending_.exchange(false, std::memory_order_acq_rel)) return;
    std::vector<LanReq> rs;
    {
        std::lock_guard<std::mutex> l(lan_mu_);
        rs.swap(lan_req_);
    }
    for (const LanReq& r : rs) lan_apply_one(r);
}

void Emu::lan_apply_one(const LanReq& r) {
#ifdef BANANADS_LAN
    // Auto has no Disconnect: Off leaves (melonDS patch 0092: the others hear it at once), Auto again starts afresh.
    // A pause until the game's wifi went off and on couldn't tell MKDS's "Create group" (its wifi off for 3 s or more,
    // in its multiplayer menu) from its main menu and straight back (under 3 s).
    if (r.mode == LanMode::Auto) {
        if (!auto_on_) {  // (already on: nothing to do; a session it hosts has Host's size, wifi.local_players)
            lan_stop();
            auto_on_ = true;
            auto_read_config();
            auto_tick_us_ = 0;
            {
                std::lock_guard<std::mutex> l(lan_mu_);
                lan_st_.addr.clear();
                lan_st_.hosts.clear();
                lan_st_.players.clear();
            }
            Log(LogLevel::Info, "[wifi] local wireless auto: on\n");
            lan_auto_phase(AutoPhase::Wait);
            lan_auto_tick();
        }
        lan_refresh(true);
        return;
    }
    auto_on_ = false;
    lan_stop();
    if (r.mode != LanMode::Off && lan_net_wait(r.mode, r.addr, r.max_players)) {
        lan_refresh(true);
        return;
    }
    switch (r.mode) {
        case LanMode::Off: {
            Log(LogLevel::Info, "[wifi] local wireless: off\n");
            std::lock_guard<std::mutex> l(lan_mu_);
            lan_st_.mode = LanMode::Off;
            lan_st_.state = LanState::Idle;
            lan_st_.addr.clear();
            lan_st_.msg.clear();
            lan_st_.players.clear();
            lan_st_.hosts.clear();
            break;
        }
        case LanMode::Host: lan_start_host(r.max_players); break;
        case LanMode::Join:
            if (r.addr.empty()) lan_start_find("", "");
            else lan_start_join(r.addr);
            break;
        case LanMode::Auto: break;
    }
    lan_refresh(true);
#else
    (void)r;
#endif
}

#ifdef BANANADS_LAN
// A network to play on: an interface other than loopback that is up and running (on the handheld, wlan0 connected:
// BananaOS keeps its address after a disconnect, but not the RUNNING flag) with an IPv4 address. Its name, or "".
static std::string usable_net() {
    struct ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return "?";  // (can't tell: taken as there, as before)
    std::string name;
    for (struct ifaddrs* i = ifs; i && name.empty(); i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && (i->ifa_flags & IFF_UP) && (i->ifa_flags & IFF_RUNNING) &&
            !(i->ifa_flags & IFF_LOOPBACK))
            name = i->ifa_name;
    freeifaddrs(ifs);
    return name;
}

// Every 1 s (and at the start): no network, no local wireless. Host, Join and Auto stay chosen (bananads.conf isn't
// touched) and wait: nothing is started, no join is tried; a session ends when the network goes (the others look
// again). When it comes back, Host hosts again, Join looks for hosts, Auto starts over when the game's wifi is on.
// Nintendo WFC goes through slirp, not this: untouched.
void Emu::lan_net_check(bool now) {
    const uint64_t t = now_us();
    if (!now && t - net_check_us_ < 1000000) return;
    net_check_us_ = t;
    const std::string net = usable_net();
    const bool ok = !net.empty();
    if (ok == net_ok_ && !now) return;
    const bool changed = ok != net_ok_;
    net_ok_ = ok;
    LanStatus st;
    {
        std::lock_guard<std::mutex> l(lan_mu_);
        lan_st_.no_net = !ok;
        st = lan_st_;
    }
    if (!changed) return;
    Log(LogLevel::Info, "[wifi] local wireless: %s%s\n", ok ? "a network again: " : "no network (Wi-Fi off or not connected)",
        ok ? net.c_str() : "");
    if (!ok) {
        const bool was = local_;
        if (auto_on_) {
            if (auto_phase_ == AutoPhase::Search || auto_phase_ == AutoPhase::Host || auto_phase_ == AutoPhase::Joined) {
                Log(LogLevel::Info, "[wifi] local wireless auto: no network: the session ends, waiting for one\n");
                lan_stop();
                lan_auto_phase(AutoPhase::Wait, "No Wi-Fi: the session ended");
            }
            return;
        }
        if (st.mode == LanMode::Off) return;
        Log(LogLevel::Info, "[wifi] local wireless %s: no network: %s, waiting for one\n", st.mode == LanMode::Host ? "host" : "join",
            st.mode == LanMode::Host ? "the session ends" : was && st.state == LanState::Ready ? "we leave the session" : "no more looking");
        lan_stop();
        std::lock_guard<std::mutex> l(lan_mu_);
        lan_st_.state = LanState::Idle;
        lan_st_.msg = was && st.state == LanState::Ready ? "No Wi-Fi: the session ended" : "";
        lan_st_.players.clear();
        lan_st_.hosts.clear();
        return;
    }
    if (auto_on_) {
        if (auto_phase_ == AutoPhase::Wait) lan_auto_phase(AutoPhase::Wait);  // (the tick starts when the game's wifi is on)
        return;
    }
    if (st.mode == LanMode::Host) lan_start_host(st.max_players);
    else if (st.mode == LanMode::Join) lan_start_find("", "");
    lan_refresh(true);
}

// Host or Join with no network: chosen, not started (1); with one, 0
bool Emu::lan_net_wait(LanMode mode, const std::string& addr, int max_players) {
    if (net_ok_) return false;
    Log(LogLevel::Info, "[wifi] local wireless %s: no network; it starts when there is one\n", mode == LanMode::Host ? "host" : "join");
    std::lock_guard<std::mutex> l(lan_mu_);
    lan_st_.mode = mode;
    lan_st_.state = LanState::Idle;
    lan_st_.msg.clear();
    lan_st_.addr = addr;
    if (mode == LanMode::Host && max_players >= 2) lan_st_.max_players = max_players;
    lan_st_.players.clear();
    lan_st_.hosts.clear();
    return true;
}

static LAN& lan_iface() {
    if (MPInterface::GetType() != MPInterface_LAN) MPInterface::Set(MPInterface_LAN);
    return static_cast<LAN&>(MPInterface::Get());
}

static std::string ipv4(uint32_t host_order) {
    char b[20];
    snprintf(b, sizeof b, "%u.%u.%u.%u", host_order >> 24, (host_order >> 16) & 255, (host_order >> 8) & 255,
             host_order & 255);
    return b;
}

// a session started while the game's Wi-Fi is already on (the menu's LAN page, the game in its multiplayer menu):
// melonDS told the LAN nothing when it powered on (MP_Begin went to no session), so the others would take this
// handheld's wifi for off. Told now, as at power-on.
static void lan_wifi_on(LAN& lan, NDS* nds) {
    if (!nds || !nds->Wifi.IsPoweredOn()) return;
    lan.Begin(0);
    Log(LogLevel::Info, "[wifi] local wireless: the game's wifi was already on; the session is told\n");
}

void Emu::lan_stop() {
    if (MPInterface::GetType() == MPInterface_LAN) {
        LAN& lan = static_cast<LAN&>(MPInterface::Get());
        lan.EndSession();    // (a host's also ends its discovery)
        lan.EndDiscovery();  // (looking for hosts: the socket closed)
    }
    local_ = false;
    lan_hosts_seen_ = 0;
    update_mp_active();
}

bool Emu::lan_start_host(int max_players) {
    LAN& lan = lan_iface();
    const std::string nick = cfg_.str("firmware.nickname", "BananaDS");
    lan.SetDiscoveryTag(gamecode_, 0);  // (melonDS patch 0092: the game, for the others' Auto)
    lan.SetDiscoveryStatus(0);
    lan.SetHostListens(false);
    const bool ok = lan.StartHost(nick.c_str(), max_players);
    Log(ok ? LogLevel::Info : LogLevel::Error, "[wifi] local wireless host: %s (%d players at most)\n",
        ok ? "ready" : "failed (UDP port 7064 taken?)", max_players);
    if (ok) {
        local_ = true;
        lan_wifi_on(lan, nds_.get());
    }
    std::lock_guard<std::mutex> l(lan_mu_);
    lan_st_.mode = LanMode::Host;
    lan_st_.state = ok ? LanState::Ready : LanState::Failed;
    lan_st_.msg = ok ? "" : "Couldn't start a session: UDP port 7064 is taken";
    lan_st_.max_players = max_players;
    lan_st_.addr.clear();
    lan_st_.players.clear();
    lan_st_.hosts.clear();
    return ok;
}

// Join mode before (or after) a session: listen for the hosts' broadcasts (each host every 1 s). A failed join or a
// lost host comes back here, with what happened in msg
bool Emu::lan_start_find(const std::string& addr, const std::string& msg) {
    LAN& lan = lan_iface();
    const bool ok = lan.StartDiscovery();
    if (ok) local_ = true;
    Log(ok ? LogLevel::Info : LogLevel::Error, "[wifi] local wireless join: %s\n",
        ok ? "looking for hosts on the network" : "can't listen for hosts (UDP port 7063 taken?)");
    std::lock_guard<std::mutex> l(lan_mu_);
    lan_st_.mode = LanMode::Join;
    lan_st_.state = !msg.empty() || !ok ? LanState::Failed : LanState::Idle;
    lan_st_.msg = !msg.empty() ? msg : ok ? "" : "Can't look for hosts: UDP port 7063 is taken";
    lan_st_.addr = addr;
    lan_st_.players.clear();
    lan_st_.hosts.clear();
    return ok;
}

bool Emu::lan_start_join(const std::string& addr) {
    LAN& lan = lan_iface();
    const std::string nick = cfg_.str("firmware.nickname", "BananaDS");
    const uint64_t t0 = now_us();
    const bool ok = lan.StartClient(nick.c_str(), addr.c_str());  // (up to 5 s)
    Log(ok ? LogLevel::Info : LogLevel::Error, "[wifi] local wireless join:%s: %s (%.1f s)\n", addr.c_str(),
        ok ? "ready" : "failed", (now_us() - t0) / 1e6);
    if (!ok) {
        lan_start_find(addr, "No answer from " + addr + ", or its session is full");
        return false;
    }
    local_ = true;
    lan_wifi_on(lan, nds_.get());
    std::lock_guard<std::mutex> l(lan_mu_);
    lan_st_.mode = LanMode::Join;
    lan_st_.state = LanState::Ready;
    lan_st_.msg.clear();
    lan_st_.addr = addr;
    lan_st_.max_players = lan.GetMaxPlayers();
    lan_st_.players.clear();
    lan_st_.hosts.clear();
    return true;
}

// ---- Auto (wifi.local = auto): two handhelds find each other as two DSes would ----
// Nothing until the game turns its wifi on (its multiplayer menu). Then listen for the hosts' beacons
// for 2 s plus 0-1 s at random, and join the lowest-address host that plays this game (melonDS patch 0092's tag;
// one hosting by hand, Host on the LAN page, before one in Auto); failing that, a host that doesn't say its game
// (desktop melonDS), only if none says it plays this one; failing that, host. Two that start together both host:
// one alone in its session that hears, twice in a row, a host of this game hosting by hand, or in Auto with a lower
// address (or, ours unknown, a lower nonce), ends its session and joins that one (a host by hand never does). Nothing
// changes while a local wireless game is on. A joined player whose host leaves listens again. A game that goes
// online through Nintendo WFC (associated with melonDS's access point, not in a multiplayer mode) gets no session:
// none is started, one alone is ended, a joined one is left (a game that looks for an access point turns its wifi
// on too, so Auto may have joined a session by then). Its frames to the access point would otherwise also reach the
// session's other players.
// Which handheld hosts the LAN session doesn't decide which one hosts the game: that stays the game's own choice.

// the tests' knobs: wifi.local_auto_start = now (as if the game's wifi were on), wifi.local_auto_test_wifi = a list
// of on@<frame> / off@<frame> (the game's wifi as a script, frames counted from the start)
void Emu::auto_read_config() {
    auto_now_ = cfg_.str("wifi.local_auto_start", "wifi") == "now";
    auto_test_wifi_.clear();
    const std::string s = cfg_.str("wifi.local_auto_test_wifi", "");
    size_t i = 0;
    while (i < s.size()) {
        const size_t j = s.find_first_of(" ,", i), e = j == std::string::npos ? s.size() : j;
        const std::string w = s.substr(i, e - i);
        const size_t at = w.find('@');
        if (at != std::string::npos) auto_test_wifi_.emplace_back(atol(w.c_str() + at + 1), w.compare(0, at, "on") == 0);
        i = e + 1;
    }
    auto_wifi_off_frames_ = 0;
}

// the tests' knob: wifi.local_test_mode = a list of <mode>@<frame> (off, auto, host, join), the LAN page's Mode row
// picked at that frame; two at one frame reach the emulator's thread together (Off then Auto at once)
void Emu::lan_read_test_modes() {
    lan_test_modes_.clear();
    const std::string s = cfg_.str("wifi.local_test_mode", "");
    size_t i = 0;
    while (i < s.size()) {
        const size_t j = s.find_first_of(" ,", i), e = j == std::string::npos ? s.size() : j;
        const std::string w = s.substr(i, e - i);
        const size_t at = w.find('@');
        if (at != std::string::npos) {
            const std::string m = w.substr(0, at);
            const LanMode lm = m == "auto" ? LanMode::Auto : m == "host" ? LanMode::Host : m == "join" ? LanMode::Join : LanMode::Off;
            lan_test_modes_.emplace_back(atol(w.c_str() + at + 1), lm);
        }
        i = e + 1;
    }
}

// the game's wifi as Auto sees it (the tests' knobs, else the DS's own)
bool Emu::auto_wifi_on() const {
    if (!auto_test_wifi_.empty()) {
        bool on = false;
        for (const auto& [f, v] : auto_test_wifi_)
            if (frames_run_ >= f) on = v;
        return on;
    }
    return auto_now_ || (nds_ && nds_->Wifi.IsPoweredOn());
}

static bool wifi_on_wfc(NDS* nds) {
    return nds && !nds->Wifi.InMPMode() && memcmp(nds->Wifi.GetBSSID(), WifiAP::APMac, 6) == 0;
}

void Emu::lan_auto_phase(AutoPhase p, const std::string& msg, const std::string& addr) {
    auto_phase_ = p;
    std::lock_guard<std::mutex> l(lan_mu_);
    lan_st_.mode = LanMode::Auto;
    lan_st_.phase = p;
    lan_st_.state = p == AutoPhase::Joining ? LanState::Busy
                  : p == AutoPhase::Host || p == AutoPhase::Joined ? LanState::Ready : LanState::Idle;
    lan_st_.msg = msg;
    if (p != AutoPhase::Joined) lan_st_.host_untagged = false;
    if (p == AutoPhase::Joining || p == AutoPhase::Joined) lan_st_.addr = addr;
    else lan_st_.addr.clear();
    if (p != AutoPhase::Search) lan_st_.hosts.clear();
    if (p != AutoPhase::Host && p != AutoPhase::Joined) lan_st_.players.clear();
}

void Emu::lan_auto_listen(const std::string& msg) {
    lan_stop();
    LAN& lan = lan_iface();
    if (!lan.StartDiscovery()) {
        Log(LogLevel::Error, "[wifi] local wireless auto: can't listen for hosts (UDP port 7063 taken?)\n");
        lan_auto_phase(AutoPhase::Held, "Can't look for hosts: UDP port 7063 is taken");
        return;
    }
    local_ = true;  // (Process() reads the beacons)
    if (!auto_rng_) auto_rng_ = lan.GetDiscoveryNonce() | 1;
    auto_rng_ ^= auto_rng_ << 13; auto_rng_ ^= auto_rng_ >> 17; auto_rng_ ^= auto_rng_ << 5;
    const uint64_t wait_ms = 2000 + auto_rng_ % 1001;
    auto_deadline_us_ = now_us() + wait_ms * 1000;
    Log(LogLevel::Info, "[wifi] local wireless auto: looking for hosts of %.4s for %.1f s\n", gamecode_,
        wait_ms / 1000.0);
    lan_auto_phase(AutoPhase::Search, msg);
}

bool Emu::lan_auto_join(uint32_t ip, bool tagged) {
    const std::string addr = ipv4(ip);
    lan_auto_phase(AutoPhase::Joining, "", addr);
    lan_stop();
    LAN& lan = lan_iface();
    const std::string nick = cfg_.str("firmware.nickname", "BananaDS");
    const uint64_t t0 = now_us();
    const bool ok = lan.StartClient(nick.c_str(), addr.c_str());  // (up to 5 s; on a home network a few ms)
    Log(ok ? LogLevel::Info : LogLevel::Warn, "[wifi] local wireless auto: join %s%s: %s (%.1f s)\n", addr.c_str(),
        tagged ? "" : " (a host that doesn't say its game)", ok ? "ready" : "failed", (now_us() - t0) / 1e6);
    if (!ok) {
        auto_bad_.emplace_back(ip, now_us() + 15000000);
        lan_auto_listen("No answer from " + addr + ", or its session is full");
        return false;
    }
    local_ = true;
    lan_wifi_on(lan, nds_.get());
    lan_auto_phase(AutoPhase::Joined, "", addr);
    std::lock_guard<std::mutex> l(lan_mu_);
    lan_st_.host_untagged = !tagged;
    lan_st_.max_players = lan.GetMaxPlayers();  // (the host's)
    return true;
}

void Emu::lan_auto_host() {
    lan_stop();
    LAN& lan = lan_iface();
    const std::string nick = cfg_.str("firmware.nickname", "BananaDS");
    const int max = lan_status().host_players;  // (Host's size, wifi.local_players)
    lan.SetDiscoveryTag(gamecode_, LAN::DiscoveryFlag_Auto);
    lan.SetDiscoveryStatus(0);
    lan.SetHostListens(true);  // (the other hosts' beacons: the step-down rule; ours: our address)
    const bool ok = lan.StartHost(nick.c_str(), max);
    Log(ok ? LogLevel::Info : LogLevel::Error, "[wifi] local wireless auto: no host of %.4s found: host: %s (%d players at most)\n",
        gamecode_, ok ? "ready" : "failed (UDP port 7064 taken?)", max);
    if (!ok) {
        lan_auto_phase(AutoPhase::Held, "Couldn't start a session: UDP port 7064 is taken");
        return;
    }
    local_ = true;
    lan_wifi_on(lan, nds_.get());
    auto_my_ip_ = 0;
    auto_lower_ip_ = 0;
    auto_lower_count_ = 0;
    auto_host_since_us_ = now_us();
    lan_auto_phase(AutoPhase::Host);
    std::lock_guard<std::mutex> l(lan_mu_);
    lan_st_.max_players = max;
}

void Emu::lan_tag_update() {
    if (auto_on_ && auto_phase_ != AutoPhase::Host) return;   // (a manual Host's too; a client's is never sent)
    LAN& lan = static_cast<LAN&>(MPInterface::Get());
    const bool playing = mp_active();
    lan.SetDiscoveryStatus(playing ? 1 : 0);
    lan.SetDiscoveryTag(gamecode_, (auto_on_ ? LAN::DiscoveryFlag_Auto : 0) | (playing ? LAN::DiscoveryFlag_Playing : 0));
}

void Emu::lan_auto_tick() {
    if (!auto_on_ || !nds_) return;
    const uint64_t t = now_us();
    if (t - auto_tick_us_ < 100000) return;
    auto_tick_us_ = t;
    const bool wifi = auto_wifi_on(), wfc = nds_->Wifi.IsPoweredOn() && wifi_on_wfc(nds_.get());
    // the game's wifi off for 3 s of its own time (frames run: the paused menu doesn't count): it has left its
    // multiplayer mode, so it isn't in a local wireless game. MKDS turns it off and on again between its own
    // screens and races, for under 2 s.
    // That ends only a host alone in its session. With others in it (a host's players, or a joined player's host),
    // the session ends after 30 s off: MKDS keeps its wifi off for longer than 3 s between looking for groups and
    // creating one, and the others' melonDS would keep a session that had ended.
    const bool wifi_gone = auto_wifi_off_frames_ >= kAutoWifiGrace;
    const bool wifi_long_gone = auto_wifi_off_frames_ >= kAutoWifiGraceShared;
    const bool alone = auto_phase_ == AutoPhase::Host && static_cast<LAN&>(MPInterface::Get()).GetNumPlayers() <= 1;
    if (wifi != auto_wifi_was_ && (auto_phase_ == AutoPhase::Host || auto_phase_ == AutoPhase::Joined))
        Log(LogLevel::Info, "[wifi] local wireless auto: the game's wifi %s\n",
            wifi ? "is on again: the session stays"
            : alone ? "went off: the session ends if it stays off for 3 s while nobody else is in it"
                    : "went off: others are in the session: it ends if it stays off for 30 s");
    auto_wifi_was_ = wifi;
    auto_bad_.erase(std::remove_if(auto_bad_.begin(), auto_bad_.end(),
                                   [t](const std::pair<uint32_t, uint64_t>& b) { return b.second <= t; }),
                    auto_bad_.end());
    switch (auto_phase_) {
        case AutoPhase::Held:  // a problem (a UDP port taken): again after the game's wifi off for 3 s, or Off then Auto
        case AutoPhase::Online:
            if (wifi_gone || (auto_phase_ == AutoPhase::Online && nds_->Wifi.InMPMode())) {
                if (auto_phase_ == AutoPhase::Online) Log(LogLevel::Info, "[wifi] local wireless auto: the game left WFC\n");
                lan_auto_phase(AutoPhase::Wait);
            }
            break;
        case AutoPhase::Wait:
            if (wfc) {
                Log(LogLevel::Info, "[wifi] local wireless auto: the game is online (Nintendo WFC): no local session\n");
                lan_auto_phase(AutoPhase::Online);
            } else if (wifi && net_ok_) {
                Log(LogLevel::Info, "[wifi] local wireless auto: %s\n",
                    auto_test_wifi_.empty() && auto_now_ ? "as if the game's wifi were on (wifi.local_auto_start = now)"
                                                          : "the game turned its wifi on");
                lan_auto_listen("");
            }
            break;
        case AutoPhase::Search: {
            if (wifi_gone) {
                lan_stop();
                Log(LogLevel::Info, "[wifi] local wireless auto: the game turned its wifi off: waiting\n");
                lan_auto_phase(AutoPhase::Wait);
                break;
            }
            if (wfc) {
                lan_stop();
                Log(LogLevel::Info, "[wifi] local wireless auto: the game is online (Nintendo WFC): no local session\n");
                lan_auto_phase(AutoPhase::Online);
                break;
            }
            if (t < auto_deadline_us_) break;
            LAN& lan = static_cast<LAN&>(MPInterface::Get());
            uint32_t best = 0, by_hand = 0, untagged = 0;
            for (const auto& [ip, d] : lan.GetDiscoveryList()) {
                if (std::any_of(auto_bad_.begin(), auto_bad_.end(), [ip = ip](const auto& b) { return b.first == ip; }))
                    continue;
                if (d.NumPlayers >= d.MaxPlayers) continue;
                LAN::DiscoveryTag tag;
                if (LAN::ReadDiscoveryTag(d, tag)) {
                    if (tag.Nonce == lan.GetDiscoveryNonce() || memcmp(tag.GameCode, gamecode_, 4) != 0) continue;
                    if (!best || ip < best) best = ip;
                    if (!(tag.Flags & LAN::DiscoveryFlag_Auto) && (!by_hand || ip < by_hand)) by_hand = ip;
                } else if (!untagged || ip < untagged)
                    untagged = ip;
            }
            if (by_hand || best) lan_auto_join(by_hand ? by_hand : best, true);
            else if (untagged) lan_auto_join(untagged, false);
            else lan_auto_host();
            break;
        }
        case AutoPhase::Host: {
            LAN& lan = static_cast<LAN&>(MPInterface::Get());
            if (alone ? wifi_gone : wifi_long_gone) {  // (the others, if any, look again)
                lan_stop();
                Log(LogLevel::Info, "[wifi] local wireless auto: the game turned its wifi off%s: the session ends, waiting\n",
                    alone ? "" : " for 30 s");
                lan_auto_phase(AutoPhase::Wait, "The game turned its wireless off: the session ended");
                break;
            }
            if (wfc && !mp_active()) {  // (the others, if any, look again)
                lan_stop();
                Log(LogLevel::Info, "[wifi] local wireless auto: the game is online (Nintendo WFC): the session ends\n");
                lan_auto_phase(AutoPhase::Online);
                break;
            }
            if (lan.GetNumPlayers() > 1 || mp_active()) { auto_lower_count_ = 0; break; }  // never with players
            // the step-down rule: another host of this game, by hand or in Auto with a lower address, heard twice in a row
            const auto list = lan.GetDiscoveryList();
            uint32_t lower = 0, lower_tick = 0, by_hand = 0, by_hand_tick = 0;
            for (const auto& [ip, d] : list) {
                LAN::DiscoveryTag tag;
                if (!LAN::ReadDiscoveryTag(d, tag)) continue;
                if (tag.Nonce == lan.GetDiscoveryNonce()) {
                    if (auto_my_ip_ != ip) {
                        auto_my_ip_ = ip;
                        Log(LogLevel::Info, "[wifi] local wireless auto: our address: %s\n", ipv4(ip).c_str());
                    }
                    continue;
                }
                if (memcmp(tag.GameCode, gamecode_, 4) != 0 || d.NumPlayers >= d.MaxPlayers) continue;
                if (!(tag.Flags & LAN::DiscoveryFlag_Auto)) {
                    if (!by_hand || ip < by_hand) { by_hand = ip; by_hand_tick = d.Tick; }
                    continue;
                }
                const bool below = auto_my_ip_ ? ip < auto_my_ip_ : tag.Nonce < lan.GetDiscoveryNonce();
                if (below && (!lower || ip < lower)) { lower = ip; lower_tick = d.Tick; }
            }
            if (by_hand) { lower = by_hand; lower_tick = by_hand_tick; }
            if (!lower) { auto_lower_count_ = 0; break; }
            if (lower != auto_lower_ip_) { auto_lower_ip_ = lower; auto_lower_count_ = 1; auto_lower_tick_ = lower_tick; }
            else if (lower_tick != auto_lower_tick_) { auto_lower_count_++; auto_lower_tick_ = lower_tick; }
            if (auto_lower_count_ >= 2) {
                Log(LogLevel::Info, "[wifi] local wireless auto: %s hosts %.4s too, %s: it hosts, we join\n",
                    ipv4(lower).c_str(), gamecode_, lower == by_hand ? "by hand" : "with a lower address");
                auto_lower_count_ = 0;
                lan_auto_join(lower, true);
            }
            break;
        }
        case AutoPhase::Joined:  // (a lost host: lan_refresh)
            if (wifi_long_gone) {
                lan_stop();
                Log(LogLevel::Info, "[wifi] local wireless auto: the game turned its wifi off for 30 s: we leave the session, waiting\n");
                lan_auto_phase(AutoPhase::Wait, "The game turned its wireless off: we left the session");
                break;
            }
            if (wfc && !mp_active()) {  // joined while the game looked for an access point: it went online
                lan_stop();
                Log(LogLevel::Info, "[wifi] local wireless auto: the game is online (Nintendo WFC): we leave the session\n");
                lan_auto_phase(AutoPhase::Online);
            }
            break;
        case AutoPhase::Joining:
            break;
    }
}
#endif

void Emu::lan_refresh(bool now) {
#ifdef BANANADS_LAN
    const uint64_t t = now_us();
    if (!now && t - lan_refresh_us_ < 500000) return;
    lan_refresh_us_ = t;
    if (!local_ || MPInterface::GetType() != MPInterface_LAN) return;
    lan_tag_update();
    LAN& lan = static_cast<LAN&>(MPInterface::Get());
    LanMode mode;
    LanState state;
    std::string addr;
    {
        std::lock_guard<std::mutex> l(lan_mu_);
        mode = lan_st_.mode;
        state = lan_st_.state;
        addr = lan_st_.addr;
    }
    std::vector<LanPlayer> players;
    bool host_gone = false;
    for (const LAN::Player& p : lan.GetPlayerList()) {
        if (p.Status == LAN::Player_Disconnected) {
            if (p.ID == 0 && ((mode == LanMode::Join && state == LanState::Ready) ||
                              (mode == LanMode::Auto && auto_phase_ == AutoPhase::Joined)))
                host_gone = true;
            continue;
        }
        LanPlayer q;
        q.name.assign(p.Name, strnlen(p.Name, sizeof p.Name));
        q.host = p.Status == LAN::Player_Host;
        q.me = p.IsLocalPlayer;
        q.ping_ms = q.me || p.Ping == 0 ? -1 : (int)p.Ping;
        players.push_back(std::move(q));
    }
    if (host_gone) {  // the host quit, or the network lost it: back to looking for hosts
        Log(LogLevel::Info, "[wifi] local wireless join:%s: the host is gone\n", addr.c_str());
        lan_stop();
        if (mode == LanMode::Auto) {  // a new host from those still here (or us)
            if (auto_wifi_on() && net_ok_) lan_auto_listen("The host left; looking again");
            else lan_auto_phase(AutoPhase::Wait, "The host left");
        } else
            lan_start_find(addr, "The host left, or the network lost it");
        return;
    }
    {  // who is in the session: in the log as people come and go
        std::string l;
        for (const LanPlayer& q : players) l += (l.empty() ? "" : ", ") + q.name + (q.host ? " (host)" : "") + (q.me ? " (us)" : "");
        if (l != lan_players_seen_) {
            lan_players_seen_ = l;
            Log(LogLevel::Info, "[wifi] local wireless: %zu in the session%s%s\n", players.size(), l.empty() ? "" : ": ", l.c_str());
        }
    }
    std::vector<LanHost> hosts;
    if ((mode == LanMode::Join && state != LanState::Ready) ||
        (mode == LanMode::Auto && auto_phase_ == AutoPhase::Search))
        for (const auto& [ip, d] : lan.GetDiscoveryList())
            hosts.push_back(LanHost{std::string(d.SessionName, strnlen(d.SessionName, sizeof d.SessionName)),
                                    ipv4(ip), d.NumPlayers, d.MaxPlayers});
    if (hosts.size() != lan_hosts_seen_) {
        lan_hosts_seen_ = hosts.size();
        std::string l;
        for (const LanHost& h : hosts)
            l += (l.empty() ? "" : ", ") + h.name + " at " + h.addr + " (" + std::to_string(h.players) + "/" +
                 std::to_string(h.max) + ")";
        Log(LogLevel::Info, "[wifi] local wireless %s: %zu host%s found%s%s\n", mode == LanMode::Auto ? "auto" : "join", hosts.size(),
            hosts.size() == 1 ? "" : "s", l.empty() ? "" : ": ", l.c_str());
    }
    std::lock_guard<std::mutex> l(lan_mu_);
    if (lan_pending_.load(std::memory_order_acquire)) return;  // (a newer request: what it shows stays)
    lan_st_.players = std::move(players);
    lan_st_.hosts = std::move(hosts);
#else
    (void)now;
#endif
}

// the DS's clock: now, or rtc.fixed
void Emu::set_clock() {
    time_t t = time(nullptr);
    tm lt;
    localtime_r(&t, &lt);
    // rtc.fixed ("YYYY-MM-DD HH:MM:SS"): the DS clock starts there instead of now, so that two runs of a game that
    // reads the clock (to seed its random numbers, say) go the same way; for tests
    int fy, fmo, fd, fh, fmi, fs;
    if (sscanf(cfg_.str("rtc.fixed", "").c_str(), "%d-%d-%d%*c%d:%d:%d", &fy, &fmo, &fd, &fh, &fmi, &fs) == 6)
        nds_->RTC.SetDateTime(fy, fmo, fd, fh, fmi, fs);
    else
        nds_->RTC.SetDateTime(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
}

void Emu::restart() {
    std::lock_guard<std::mutex> g(net_mu_);
    nds_->Reset();
    nds_->SetupDirectBoot(game_ + ".nds");
    set_clock();
    nds_->Start();
    Log(LogLevel::Info, "[emu] %s restarted\n", game_.c_str());
}

// ---- the ROM, mapped (BananaOS 0.4.0 #11) ----
// A ROM read into memory is all anonymous RAM: 256 MB for Golden Sun: Dark Dawn on a 981 MB handheld without swap,
// which the kernel's OOM killer ended. Mapped from its file (MAP_PRIVATE: a page the game writes becomes the game's
// own), a ROM page is page cache the kernel can drop and read again; the mapping is padded to a power of two with
// zero pages, as melonDS pads a ROM (so it keeps this buffer instead of copying it). A thread at the lowest priority
// reads the file once, so the game seldom waits on the card for a page.
static std::mutex g_maps_mu;
static std::vector<std::pair<uint8_t*, size_t>> g_maps;  // each mapping: where, how long

static uint8_t* map_rom(const std::string& path, size_t& file_len, size_t& map_len) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0x200 || st.st_size > (off_t)512 * 1024 * 1024) {
        close(fd);
        return nullptr;
    }
    file_len = (size_t)st.st_size;
    map_len = 0x1000;
    while (map_len < file_len) map_len <<= 1;
    void* base = mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    if (mmap(base, file_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
        munmap(base, map_len);
        close(fd);
        return nullptr;
    }
    close(fd);
    {
        std::lock_guard<std::mutex> l(g_maps_mu);
        g_maps.emplace_back((uint8_t*)base, map_len);
    }
    std::thread([path] {  // the file into the page cache, not into this process: the mapping's pages come cheap
        pthread_setname_np(pthread_self(), "bds-romread");
        setpriority(PRIO_PROCESS, 0, 19);
        int f = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (f < 0) return;
        std::vector<char> buf(1 << 20);
        while (read(f, buf.data(), buf.size()) > 0) {}
        close(f);
    }).detach();
    return (uint8_t*)base;
}

// Mapped only from the system's own card (/data's filesystem): a ROM on a card that can be pulled out while the game
// runs (the RG DS Plus's second slot) is read into memory, or the next page it needs would kill the emulator (SIGBUS).
// Where there is no /data (not BananaOS), every ROM is mapped.
static bool mappable(const std::string& path) {
    struct stat data{}, rom{};
    if (stat("/data", &data) != 0) return true;
    return stat(path.c_str(), &rom) == 0 && rom.st_dev == data.st_dev;
}

bool rom_unmap(uint8_t* p) {
    std::lock_guard<std::mutex> l(g_maps_mu);
    for (size_t i = 0; i < g_maps.size(); i++)
        if (g_maps[i].first == p) {
            munmap(p, g_maps[i].second);
            g_maps.erase(g_maps.begin() + i);
            return true;
        }
    return false;
}

bool Emu::load_rom(const std::string& path) {
    std::unique_ptr<u8[]> romdata;
    size_t romlen = 0, file_len = 0;
    if (cfg_.flag("rom.map", true) && mappable(path))
        if (uint8_t* m = map_rom(path, file_len, romlen)) {
            romdata.reset(m);  // (melonDS's ~CartCommon gives it back through Platform::ROMRelease: rom_unmap)
            Log(LogLevel::Info, "[emu] ROM mapped (%zu MB), read in the background\n", file_len >> 20);
        }
    if (!romdata) {  // rom.map=off, a removable card, or the mapping failed: read into memory, straight into the buffer
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        const std::streamoff n = f ? (std::streamoff)f.tellg() : -1;
        if (n >= 0x200) {
            romlen = (size_t)n;
            romdata = std::unique_ptr<u8[]>(new (std::nothrow) u8[romlen]);  // (not zeroed: every byte is read)
            f.seekg(0);
            if (!romdata || !f.read((char*)romdata.get(), n)) romdata.reset();
        }
    }
    if (!romdata) {
        Log(LogLevel::Error, "[emu] can't read the ROM %s\n", path.c_str());
        return false;
    }
    rom_path_ = path;
    game_ = basename_noext(path);

    std::string savedir = cfg_.str("save.dir", "");
    if (savedir.empty()) {
        size_t s = path.find_last_of('/');
        savedir = s == std::string::npos ? "." : path.substr(0, s);
    }
    if (cfg_.flag("save.per_game_folder", false)) savedir += "/" + game_;  // ROCKNIXDS #42/#56
    mkdirs(savedir);
    save_path_ = savedir + "/" + game_ + ".sav";

    std::vector<uint8_t> sav;
    NDSCart::NDSCartArgs cart_args;
    if (read_file(save_path_, sav) && !sav.empty()) {
        cart_args.SRAM = std::make_unique<u8[]>(sav.size());
        memcpy(cart_args.SRAM.get(), sav.data(), sav.size());
        cart_args.SRAMLength = (u32)sav.size();
        Log(LogLevel::Info, "[emu] save %s (%zu bytes)\n", save_path_.c_str(), sav.size());
    }

    start_mark("save read");
    auto cart = NDSCart::ParseROM(std::move(romdata), (u32)romlen, this, std::move(cart_args));
    if (!cart) {
        if (romdata && rom_unmap(romdata.get())) (void)romdata.release();  // (rejected before melonDS took it)
        Log(LogLevel::Error, "[emu] %s isn't a DS ROM melonDS can load\n", path.c_str());
        return false;
    }
    memcpy(gamecode_, cart->GetHeader().GameCode, 4);  // (the LAN beacon's tag: Auto finds hosts of this game)

    NDSArgs args;  // FreeBIOS and a generated firmware: no BIOS or firmware files needed, as with DraStic
    args.OutputSampleRate = 48000.0;
    if (!cfg_.flag("jit", true)) args.JIT = std::nullopt;
#ifdef BANANADS_JIT
    // jit.timing: block (the default) is melonDS's JIT's level of timing, with fast memory and per-block cycles;
    // exact is the interpreter's timing, cycle for cycle (the jit.* gates below compare in that mode)
    const bool exact = cfg_.str("jit.timing", "block") == "exact";
    if (args.JIT) args.JIT->FastMemory = !exact;
    if (exact) setenv("BANANAJIT_EXACT", "1", 1);
    else unsetenv("BANANAJIT_EXACT");
    // jit.fastloads=off: ARM9 loads through calls instead of inline (for bananads-difftest: both must agree)
    if (cfg_.flag("jit.fastloads", true)) unsetenv("BANANAJIT_NO_FASTLOADS");
    else setenv("BANANAJIT_NO_FASTLOADS", "1", 1);
    if (cfg_.flag("jit.faststores", true)) unsetenv("BANANAJIT_NO_FASTSTORES");
    else setenv("BANANAJIT_NO_FASTSTORES", "1", 1);
    if (cfg_.flag("jit.fastmem7", true)) unsetenv("BANANAJIT_NO_FASTMEM7");
    else setenv("BANANAJIT_NO_FASTMEM7", "1", 1);
    if (cfg_.flag("jit.link", true)) unsetenv("BANANAJIT_NO_LINK");  // jit.link=off: back to the loop each block
    else setenv("BANANAJIT_NO_LINK", "1", 1);
    // jit.mul=off: multiply and register-shift through the interpreter (for bananads-difftest: both must agree)
    if (cfg_.flag("jit.mul", true)) unsetenv("BANANAJIT_NO_MUL");
    else setenv("BANANAJIT_NO_MUL", "1", 1);
    // jit.sys=off: MRS/MSR (CPSR forms) and CLZ through the interpreter (for bananads-difftest)
    if (cfg_.flag("jit.sys", true)) unsetenv("BANANAJIT_NO_SYS");
    else setenv("BANANAJIT_NO_SYS", "1", 1);
    // jit.jumps=off: jumps to a register (BX/BLX, LDM/POP with the PC; block timing) through the interpreter
    if (cfg_.flag("jit.jumps", true)) unsetenv("BANANAJIT_NO_JUMPS");
    else setenv("BANANAJIT_NO_JUMPS", "1", 1);
    // jit.ldm=off: block transfers (LDM/STM/PUSH/POP) through the interpreter (for bananads-difftest)
    if (cfg_.flag("jit.ldm", true)) unsetenv("BANANAJIT_NO_LDM");
    else setenv("BANANAJIT_NO_LDM", "1", 1);
#endif

    auto& hdr = args.Firmware.GetHeader();
    // the DS's MAC, one per handheld and kept (wifi.h choose_wifi_mac), and the Nintendo WFC settings the game saved
    // last time (wifi.wfc_settings, melonDS's wfcsettings.bin): without them the game makes a new WFC ID (a new
    // friend code) at the first connection of every session
    {
        const std::string wfc = cfg_.str("wifi.wfc_settings", "wfcsettings.bin");
        wfc_path_ = wfc == "off" ? "" : Platform::GetLocalFilePath(wfc);
    }
    const std::string mac_file_set = cfg_.str("wifi.mac_file", "wifi-mac");
    const WifiMac wm = choose_wifi_mac(cfg_.str("wifi.mac", ""),
                                       mac_file_set == "off" ? "" : Platform::GetLocalFilePath(mac_file_set),
                                       wfc_path_);
    const auto mac = wm.mac;
    Log(LogLevel::Info, "[wifi] the DS's MAC: %s (%s)\n", format_mac(mac).c_str(), wm.source.c_str());
    memcpy(hdr.MacAddr.data(), mac.data(), 6);
    auto& user = args.Firmware.GetEffectiveUserData();
    std::string nick = cfg_.str("firmware.nickname", "BananaDS");
    user.NameLength = (u16)std::min<size_t>(nick.size(), 10);
    for (size_t i = 0; i < user.NameLength; i++) user.Nickname[i] = (char16_t)(unsigned char)nick[i];
    long lang = cfg_.num("firmware.language", -1);  // 0 ja, 1 en, 2 fr, 3 de, 4 it, 5 es
    if (lang >= 0 && lang <= 5) user.Settings = (u16)((user.Settings & ~7) | lang);
    if (!wfc_path_.empty()) {
        const int r = wfc_settings_load(args.Firmware, wfc_path_);
        if (r > 0) {
            bool id = false;
            for (const auto& ap : args.Firmware.GetAccessPoints())
                for (u8 b : ap.NintendoWFCID) id |= b != 0;
            Log(LogLevel::Info, "[wifi] WFC settings loaded from %s (%s)\n", wfc_path_.c_str(),
                id ? "with a Nintendo WFC ID" : "no Nintendo WFC ID yet");
        } else if (r == 0)
            Log(LogLevel::Info, "[wifi] no WFC settings saved yet (%s): the game makes them at its first WFC "
                "connection\n", wfc_path_.c_str());
    }
    args.Firmware.UpdateChecksums();
    wfc_last_.clear();
    if (!wfc_path_.empty() && !wfc_settings_get(args.Firmware, wfc_last_)) {
        Log(LogLevel::Warn, "[wifi] the generated firmware's access points aren't where they should be: the WFC "
            "settings won't be kept\n");
        wfc_path_.clear();
    }

    // jit.iofast=on|off sets MELONDS_IOFAST (melonDS patch 0005: the JIT slow paths' direct routes for main RAM,
    // the GX FIFO and IE/IF/IME/DISPSTAT/VCOUNT/KEYINPUT), which the NDS reads as it's created; unset, the
    // environment decides (default on; MELONDS_IOFAST_ROUTES=mask picks routes). For A/B runs, bananads-difftest
    // --a jit.iofast=on --b jit.iofast=off.
    {
        const std::string iofast = cfg_.str("jit.iofast", "");
        if (iofast == "on") setenv("MELONDS_IOFAST", "1", 1);
        else if (iofast == "off") setenv("MELONDS_IOFAST", "0", 1);
    }
    // jit.inval16=on|off sets MELONDS_JIT_INVAL16 (melonDS patch 0016: a write drops only the compiled blocks in its
    // 16 bytes, not every block of its 512; read at the first invalidation); unset, on
    {
        const std::string v = cfg_.str("jit.inval16", "");
        if (v == "on") setenv("MELONDS_JIT_INVAL16", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_INVAL16", "0", 1);
    }
    // core.dma_block=on|off sets MELONDS_DMABLOCK (melonDS patch 0018: DMA from main RAM into a VRAM page with one
    // bank copied in blocks, its timing unit by unit as before; exact; read at the first such DMA); unset, on
    {
        const std::string v = cfg_.str("core.dma_block", "");
        if (v == "on") setenv("MELONDS_DMABLOCK", "1", 1);
        else if (v == "off") setenv("MELONDS_DMABLOCK", "0", 1);
    }
    // jit.hotsmc=on|off sets MELONDS_JIT_HOTSMC (melonDS patch 0017: blocks stay out of code the game keeps rewriting,
    // so a rewrite recompiles a few instructions, not every block that had followed a branch into them); unset, on
    {
        const std::string v = cfg_.str("jit.hotsmc", "");
        if (v == "on") setenv("MELONDS_JIT_HOTSMC", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_HOTSMC", "0", 1);
    }
    // jit.hotidx=on|off sets MELONDS_JIT_HOTIDX (melonDS patch 0019: ranges with hot self-modifying code keep an
    // index of their blocks' masks, so a write there doesn't scan every block); unset, on
    {
        const std::string v = cfg_.str("jit.hotidx", "");
        if (v == "on") setenv("MELONDS_JIT_HOTIDX", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_HOTIDX", "0", 1);
    }
    // jit.codegen=new|old sets MELONDS_JIT_OLDCODEGEN (melonDS patch 0007: the AArch64 JIT's code generation
    // improvements, each also off by its own MELONDS_JIT_NO* variable), read as the JIT compiler is created; unset,
    // the environment decides (default new). For A/B runs, bananads-difftest --a jit.codegen=old --b jit.codegen=new.
    {
        const std::string codegen = cfg_.str("jit.codegen", "");
        if (codegen == "new") setenv("MELONDS_JIT_OLDCODEGEN", "0", 1);
        else if (codegen == "old") setenv("MELONDS_JIT_OLDCODEGEN", "1", 1);
    }

    // jit.slice_chain=on|off sets MELONDS_JIT_SLICECHAIN (melonDS patch 0030: when the ARM9's slice ends inside a
    // block chain and the slice loop would only keep time, the exit stub does that and the chain goes on; exact),
    // read as the JIT compiler is created; unset, the environment decides (default on; off with jit.codegen=old)
    {
        const std::string v = cfg_.str("jit.slice_chain", "");
        if (v == "on") setenv("MELONDS_JIT_SLICECHAIN", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_SLICECHAIN", "0", 1);
    }

    // jit.entry_x26=on|off: off sets MELONDS_JIT_NOENTRYX26=1, on unsets it (melonDS patch 0040: ARM_Dispatch sets the
    // fast memory base in x26, so blocks have no entry prologue; read as the JIT compiler is created; default on)
    {
        const std::string v = cfg_.str("jit.entry_x26", "");
        if (v == "on") unsetenv("MELONDS_JIT_NOENTRYX26");
        else if (v == "off") setenv("MELONDS_JIT_NOENTRYX26", "1", 1);
    }

    // jit.dyncold=on|off: off sets MELONDS_JIT_NODYNCOLD=1, on unsets it (melonDS patch 0041: a return's per-site
    // cached exit keeps its miss way in the JIT's far region; default on)
    {
        const std::string v = cfg_.str("jit.dyncold", "");
        if (v == "on") unsetenv("MELONDS_JIT_NODYNCOLD");
        else if (v == "off") setenv("MELONDS_JIT_NODYNCOLD", "1", 1);
    }

    // jit.flagstub=on|off: off sets MELONDS_JIT_NOFLAGSTUB=1, on unsets it (melonDS patch 0042: an exit's stub puts the
    // pending guest flags in RCPSR instead of three instructions in every block). Default off: no check sees a
    // mistake in ExitCheckPCF's flags (the test ROMs nor Mario Kart DS's race start, docs/PERF.md)
    {
        const std::string v = cfg_.str("jit.flagstub", "off");
        if (v == "on") unsetenv("MELONDS_JIT_NOFLAGSTUB");
        else if (v == "off") setenv("MELONDS_JIT_NOFLAGSTUB", "1", 1);
    }

    // jit.hugepages=on|off sets MELONDS_JIT_HUGEPAGE (melonDS patch 0085: the JIT's code memory aligned to 2 MB and
    // madvise(MADV_HUGEPAGE)'d, for the A55's TLBs; decided as the first JIT starts; unset, on). Emulation doesn't
    // change. The JIT's first reset logs "[jit] code memory at ..., N kB on huge pages"
    {
        const std::string v = cfg_.str("jit.hugepages", "");
        if (v == "on") setenv("MELONDS_JIT_HUGEPAGE", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_HUGEPAGE", "0", 1);
    }

    // jit.cp15nop=on|off: off sets MELONDS_JIT_NOCP15NOP=1, on unsets it (melonDS patch 0086: an ARM9 MCR to a CP15
    // cache or write buffer operation, which CP15Write ignores, compiled to its cycles instead of the interpreter's
    // call; user mode keeps the call). Exact; read as the JIT compiler is created
    {
        const std::string v = cfg_.str("jit.cp15nop", "");
        if (v == "on") unsetenv("MELONDS_JIT_NOCP15NOP");
        else if (v == "off") setenv("MELONDS_JIT_NOCP15NOP", "1", 1);
    }

    // jit.fastclz=on|off: off sets MELONDS_JIT_SOFTCLZ=1, on unsets it (melonDS patch 0088: the AArch64 emitter's
    // CountLeadingZeros by CLZ instead of a bit-by-bit loop; the JIT compiles faster, the code it emits is the same).
    // Read at the emitter's first use: one setting per process
    {
        const std::string v = cfg_.str("jit.fastclz", "");
        if (v == "on") unsetenv("MELONDS_JIT_SOFTCLZ");
        else if (v == "off") setenv("MELONDS_JIT_SOFTCLZ", "1", 1);
    }

    // jit.vramfast=on|off, jit.iodirect=on|off and render.vramtrack_skip=on|off set bits 1, 2 and 4 of
    // MELONDS_MEMFAST (melonDS patch 0010, read at the JIT's reset; default all on, exact): VRAM with one bank at
    // the 16 KB straight to the bank, OAM and palette straight to the GPU; the divider, square root,
    // IPCSYNC/IPCFIFOCNT and IME/IE/IF straight to their state, other IO registers without the ARM9Read*/ARM7Read*
    // dispatch; VRAM tracking's per-line DeriveState returns at once when nothing was remapped and its banks have
    // no dirty bit. jit.wram7fast=on|off sets bit 8 (melonDS patch 0072): the ARM7's slow loads and stores to its
    // own WRAM (0x03800000) straight to it, the JIT's check then the store, a block's region checked once. Unset,
    // the environment decides; as with jit.iofast, a difftest sets them on both --a and --b.
    {
        const char* cur = getenv("MELONDS_MEMFAST");
        unsigned long mask = cur ? strtoul(cur, nullptr, 0) & 15 : 15;
        const unsigned long before = mask;
        const std::string vram = cfg_.str("jit.vramfast", ""), io = cfg_.str("jit.iodirect", "");
        const std::string track = cfg_.str("render.vramtrack_skip", ""), wram7 = cfg_.str("jit.wram7fast", "");
        if (vram == "on") mask |= 1; else if (vram == "off") mask &= ~1ul;
        if (io == "on") mask |= 2; else if (io == "off") mask &= ~2ul;
        if (track == "on") mask |= 4; else if (track == "off") mask &= ~4ul;
        if (wram7 == "on") mask |= 8; else if (wram7 == "off") mask &= ~8ul;
        if (mask != before || (!cur && (!vram.empty() || !io.empty() || !track.empty() || !wram7.empty())))
            setenv("MELONDS_MEMFAST", std::to_string(mask).c_str(), 1);
    }

    // render.geom_batch=on|off sets MELONDS_GEOMBATCH (melonDS patch 0050: with render.geom_fast, the GX FIFO's DMA
    // and IRQ checks once per run of the geometry engine while no write is stalled, instead of after every refill
    // of its pipe; exact; read as the NDS is created; default on). Unset, the environment decides; a difftest sets
    // it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_batch", "");
        if (v == "on") setenv("MELONDS_GEOMBATCH", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMBATCH", "0", 1);
    }
    // render.gx_dmafast=on|off sets MELONDS_GXDMAFAST (melonDS patch 0060: the GX FIFO DMA's block path unpacks its
    // words into the FIFO with the command state and the FIFO's level in registers; exact; read as the NDS is created;
    // default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_dmafast", "");
        if (v == "on") setenv("MELONDS_GXDMAFAST", "1", 1);
        else if (v == "off") setenv("MELONDS_GXDMAFAST", "0", 1);
    }
    // render.geom_fifo=on|off sets MELONDS_GEOMFIFO (melonDS patch 0061: with render.geom_batch and no stalled write,
    // the geometry engine's command loop reads its pipe and the GX FIFO as one queue and puts back both rings' state
    // after the loop; exact; read as the NDS is created; default on). Unset, the environment decides; a difftest
    // sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_fifo", "");
        if (v == "on") setenv("MELONDS_GEOMFIFO", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMFIFO", "0", 1);
    }
    // render.geom_queue=on|off sets MELONDS_GEOMQUEUE (melonDS patch 0080: render.geom_fifo's loop reads its queue run
    // by run with a pointer and works out the pipe's refills after the loop from the number of entries read; exact;
    // read as the NDS is created; default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_queue", "");
        if (v == "on") setenv("MELONDS_GEOMQUEUE", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMQUEUE", "0", 1);
    }
    // render.geom_switch=on|off sets MELONDS_GEOMSWITCH (melonDS patch 0081: render.geom_queue's loop dispatches every
    // command with one switch on it, a full vertex's two entries with a path of their own; exact; read as the NDS is
    // created; default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_switch", "");
        if (v == "on") setenv("MELONDS_GEOMSWITCH", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMSWITCH", "0", 1);
    }
    // render.geom_thread=on|off sets MELONDS_GEOMTHREAD (melonDS patch 0083: a stored polygon's build on a worker
    // thread, every timing decision kept on the emulator thread; drained at VBlank, savestates and reset; exact; read
    // as the NDS is created). Default OFF: on the RG DS Plus (four A55s shared by the emulator's thread, the 2D thread
    // and three Gengis workers) the worker cost Mario Kart DS's race start ~35 frames and ~50 repeated refreshes a
    // run on the panels (2026-10-19, perf/bananads/session3/DEVICE-RESULTS.md); it only pays with a core free.
    // Unset, the environment decides (a difftest sets it on both --a and --b), else off.
    {
        const std::string v = cfg_.str("render.geom_thread", "");
        if (v == "on") setenv("MELONDS_GEOMTHREAD", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMTHREAD", "0", 1);
        else setenv("MELONDS_GEOMTHREAD", "0", 0);
    }
    // render.gx_batch=on|off sets MELONDS_GXBATCH (melonDS patch 0063, session 2's 0021: the JIT's STMs into the GX
    // FIFO hand their words to the geometry engine in one call, through 0060's loop; exact; read at the JIT's reset;
    // default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_batch", "");
        if (v == "on") setenv("MELONDS_GXBATCH", "1", 1);
        else if (v == "off") setenv("MELONDS_GXBATCH", "0", 1);
    }
    // render.gx_str=on|off sets MELONDS_GXSTR (melonDS patch 0064, session 2's 0022: the JIT's STRs to the GX FIFO
    // port and command ports with fewer layers, and a FIFO-port word that is one of its command's parameters before
    // the last straight into the FIFO; exact; read at the JIT's reset and as the NDS is created; default on). Unset,
    // the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_str", "");
        if (v == "on") setenv("MELONDS_GXSTR", "1", 1);
        else if (v == "off") setenv("MELONDS_GXSTR", "0", 1);
    }
    // render.geom_sort=on|off sets MELONDS_GEOMSORT (melonDS patch 0065: VBlank's polygon Y-sort as a stable counting
    // sort for sets of 64 polygons or more, the order std::stable_sort gives; exact; read as the NDS is created;
    // default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_sort", "");
        if (v == "on") setenv("MELONDS_GEOMSORT", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMSORT", "0", 1);
    }
    // render.gx_dmarun=on|off sets MELONDS_GXDMARUN (melonDS patch 0066: DMA::Run9 takes the GX FIFO DMA's block path
    // in a function of its own when nothing else in Run9 would run for that call; exact; read as the NDS is created;
    // default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_dmarun", "");
        if (v == "on") setenv("MELONDS_GXDMARUN", "1", 1);
        else if (v == "off") setenv("MELONDS_GXDMARUN", "0", 1);
    }
    // render.gx_dmatime=on|off sets MELONDS_GXDMATIME (melonDS patch 0082: render.gx_dmarun's burst timing takes 4
    // units at a time while none of their table entries ends the table and their sum stays below the target; exact;
    // read as the NDS is created; default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_dmatime", "");
        if (v == "on") setenv("MELONDS_GXDMATIME", "1", 1);
        else if (v == "off") setenv("MELONDS_GXDMATIME", "0", 1);
    }

    // render.geom_fast=on|off sets MELONDS_GEOMFAST (melonDS patch 0011: the 3D geometry engine's fast paths, exact;
    // read as the NDS is created; default on). Unset, the environment decides; as with jit.iofast, a difftest sets it
    // on both --a and --b.
    {
        const std::string geom = cfg_.str("render.geom_fast", "");
        if (geom == "on") setenv("MELONDS_GEOMFAST", "1", 1);
        else if (geom == "off") setenv("MELONDS_GEOMFAST", "0", 1);
    }

    start_mark("cart parsed, settings");
    setup_wifi();
    setup_local();
    start_mark("wifi");

    nds_ = std::make_unique<NDS>(std::move(args), this);
    start_mark("NDS created");
    nds_->SetNDSCart(std::move(cart));
    start_mark("cart in");
    nds_->Reset();
    start_mark("reset");
    nds_->SetupDirectBoot(game_ + ".nds");
    start_mark("direct boot");
    setup_slice();
    // core.fast_gxdma / core.fast_spu: exact shortcuts in melonDS's DMA and SPU (patches/melonds/0004), off by
    // default until measured on the handheld
    nds_->FastGXDMA = cfg_.flag("core.fast_gxdma", false);
    nds_->FastSPU = cfg_.flag("core.fast_spu", false);
    // core.fast_mix: the SPU runs only the channels whose output can change, sums with NEON, and sound DMA/capture
    // access main RAM directly (patches/melonds/0013; exact, default on)
    nds_->SPU.SetFastMix(cfg_.flag("core.fast_mix", true));
    // core.fast_samples: with core.fast_mix, a due PCM16/ADPCM channel's samples taken with its state in registers and
    // ADPCM's step from tables (patches/melonds/0070; exact)
    nds_->SPU.SetFastSamples(cfg_.flag("core.fast_samples", true));
    // core.fast_dma: 32-bit DMA from main RAM to the GX FIFO or within main RAM without the bus dispatch, and the
    // ARM9's MRAM burst timing looked up inline (patches/melonds/0013; exact, default on)
    nds_->SetFastDMA(cfg_.flag("core.fast_dma", true));
    // core.fast_sched: the scheduler keeps its next event instead of walking its list every slice (patch 0006; exact,
    // on by default since core.fast_loop needs it)
    nds_->SetFastSched(cfg_.flag("core.fast_sched", true));
    // core.fast_timers: a CPU's timers run only when one of them overflows; core.fast_loop: RunFrame's slice loop for
    // the JIT with the common cases inline, used with core.fast_sched (patch 0009; both exact, default on)
    nds_->SetFastTimers(cfg_.flag("core.fast_timers", true));
    nds_->SetFastLoop(cfg_.flag("core.fast_loop", true));
    // core.halt_skip: with both CPUs halted and nothing to wake them, the slice loop takes the times straight to the
    // next event or timer instead of slice by slice (melonDS patch 0031; exact, default on; with core.fast_loop)
    nds_->SetHaltSkip(cfg_.flag("core.halt_skip", true));
    // jit.region_pair: a CPU's PC leaving its code region checks the region it last left (ARM9: ITCM or main RAM; ARM7:
    // BIOS, main RAM or WRAM) before setting one up again (melonDS patch 0071; exact, default on; with core.fast_loop)
    nds_->SetRegionPair(cfg_.flag("jit.region_pair", true));
    // jit.region_trio (melonDS patch 0087, with jit.region_pair): two code regions kept for the swap back, not one,
    // the ARM9's BIOS and the ARM7's shared WRAM among them (an IRQ's round main RAM -> BIOS -> ITCM and back, the
    // ARM7's sound driver in shared WRAM). Exact. off: jit.region_pair's own way
    nds_->SetRegionTrio(cfg_.flag("jit.region_trio", true));

    RendererSettings rs{};
    rs.ScaleFactor = 1;
    rs.Threaded = cfg_.flag("render.threaded", true);
    // render.2d: banana (the default: the Banana 2D engine, gpu2d/) or melonds (melonDS's 2D renderer)
#ifdef BANANADS_2D
    const bool banana2d = cfg_.str("render.2d", "banana") == "banana";
    // render.2d_fast: the Banana 2D engine's NEON paths (on, the default) or the portable code (off); same pixels
    melonDS::Banana2DFast = cfg_.flag("render.2d_fast", true);
    // render.3d_share: at 2x the 2D thread reads Gengis's rows of the 3D frame instead of a 4 KB copy a line (on)
    melonDS::Banana3DShare = cfg_.flag("render.3d_share", true);
    // render.3d_wait2d: with render.3d_share, the 2D thread waits for the 3D rows, not the emulator's thread (on)
    melonDS::Banana3DWait2D = cfg_.flag("render.3d_wait2d", true);
#else
    const bool banana2d = false;
#endif
    // render.scale: 2 renders the 3D layer at 2x and gives 512x384 screens (Gengis Engine with the Banana 2D engine;
    // otherwise 1x)
    const int scale = cfg_.num("render.scale", 1) == 2 ? 2 : 1;
    scale_ = 1;
#ifdef BANANADS_GENGIS
    // render.renderer: gengis (the default: Gengis Engine, SuperDrastic's rasterizer, on render.gengis_threads
    // threads) or soft (melonDS's software renderer, render.threaded)
    if (cfg_.str("render.renderer", "gengis") == "gengis") {
        const int threads = (int)cfg_.num("render.gengis_threads", 3);
        const bool defer = cfg_.flag("render.gengis_defer", false);
        GengisRenderer3D::Help = (int)cfg_.num("render.gengis_help", 1);
        GengisRenderer3D::Nice = (int)cfg_.num("sched.gengis_nice", 0);
        // render.edge_marking, render.fog: off leaves the effect out even where the game turns it on (outlines
        // round 3D models; distance fog)
        GengisRenderer3D::SetEffects(cfg_.flag("render.edge_marking", true), cfg_.flag("render.fog", true));
#ifdef BANANADS_2D
        if (banana2d) {
            nds_->SetRenderer(std::make_unique<GengisBananaRenderer>(*nds_, threads, defer, scale));
            scale_ = scale;
        } else
#endif
            nds_->SetRenderer(std::make_unique<GengisSoftRenderer>(*nds_, threads, defer));
    } else
#endif
    {
#ifdef BANANADS_2D
        if (banana2d)
            nds_->SetRenderer(std::make_unique<BananaRenderer>(*nds_));
        else
#endif
            nds_->SetRenderer(std::make_unique<SoftRenderer>(*nds_));
        nds_->GetRenderer().SetRenderSettings(rs);
    }
#ifdef BANANADS_2D
    // render.2d_thread: on draws the Banana 2D engine's lines on a thread of its own (gpu2d/GPU_Banana.h), the same
    // pictures (the default at render.scale=2); off (the default at 1x) draws them on the emulator's thread
    // (on by default at 2x, where unit A composites four times: HeartGold at 1104 MHz 39.8 -> 49.6 fps)
    if (banana2d) banana_ = &static_cast<BananaRenderer&>(nds_->GetRenderer());
    // render.2d_ring: split (the default) puts the job ring's counters the two threads write for every job (Head;
    // Tail and the copy log's read position) on cache lines of their own, and the emulator's thread reads the 2D
    // thread's only when the ring or the log looks full (exact: the same jobs); shared is the old layout
    if (banana2d) banana_->SetSplitRing(cfg_.str("render.2d_ring", "split") != "shared");
    // render.2d_test_delay (tests only, default 0): the 2D thread sleeps this many microseconds after each job, so that
    // it falls behind and the job ring and the copy log fill: the checks then reach their full paths
    if (banana2d) banana_->SetTestDelay((u32)cfg_.num("render.2d_test_delay", 0));
    if (banana2d && cfg_.flag("render.2d_thread", scale_ == 2)) banana_->SetThreaded2D(true);
    // render.2d_kick: how many jobs (a frame has 383: 192 lines, 191 sprite lines) go to the 2D thread before it is
    // woken; a wake of the sleeping thread is a futex call on the emulator's thread (16 was the old fixed batch)
    if (banana2d) banana_->SetKickBatch((u32)cfg_.num("render.2d_kick", 64));
    // sched.2d_nice: the 2D thread's nice value (0: the default; negative puts it ahead of Gengis's workers when the cores
    // are contended). sched.emu_nice/sched.emu_cpu (the emulator's thread) and sched.gengis_nice are set beside it
    if (banana2d) banana_->SetNice((int)cfg_.num("sched.2d_nice", 0));
    // render.2d_fence_wait: the waits for the 2D thread spin on sched_yield (spin, the old way) or block on a condition
    // variable (block). Default block since 2026-10-19: on the RG DS Plus the four A55s are shared by five threads, and the
    // emulator's thread spent ~5% of Mario Kart DS's race start spinning (a prof.hz profile on the panels)
    if (banana2d) banana_->SetFenceWaitBlocking(cfg_.str("render.2d_fence_wait", "block") != "spin");
#endif

    start_mark("renderer");
    set_clock();

    if (scale != scale_)
        Log(LogLevel::Warn, "[emu] render.scale=2 needs render.renderer=gengis and render.2d=banana: rendering at 1x\n");

    nds_->Start();
    prof_ = Profiler::create(cfg_);
    Log(LogLevel::Info, "[emu] %s started (JIT %s, MAC %02x:%02x:%02x:%02x:%02x:%02x)\n", game_.c_str(),
        cfg_.flag("jit", true) ? "on" : "off", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

// core.slice: the scheduler slice, the most system cycles (33.5 MHz) the ARM9 and then the ARM7 run before they
// are interleaved again (patches/melonds/0003). 64 is melonDS's. Longer is faster and coarser: each CPU sees the
// other's writes up to a slice late. auto (or any core.slice_idle) is adaptive: core.slice_idle cycles while
// neither CPU has touched the IPC registers or WRAMCNT for core.slice_hold cycles, core.slice (64 for auto) near them.
void Emu::setup_slice() {
    // (64, melonDS's, by default: auto was the default at render.scale=2 (HeartGold at 1104 MHz 60.2 -> 64.2 fps),
    // but Golden Sun: Dark Dawn's 3D field was never drawn with it: the game's own timing goes wrong (BananaOS 0.4.0))
    const std::string s = cfg_.str("core.slice", "64");
    const bool adaptive = s == "auto";
    const long slice = adaptive ? 64 : cfg_.num("core.slice", 64);
    long idle = cfg_.num("core.slice_idle", adaptive ? 512 : slice);
    long hold = cfg_.num("core.slice_hold", 4096);
    slice_stats_ = cfg_.num("core.slice_stats", 0);
    auto ok = [](long v) { return v >= 1 && v <= 16384; };
    if (!ok(slice) || !ok(idle) || hold < 0 || hold > (1 << 24)) {
        Log(LogLevel::Warn, "[emu] core.slice=%s / slice_idle=%ld / slice_hold=%ld out of range; using 64\n", s.c_str(),
            idle, hold);
        nds_->SetSlice(64, 64, 0);
        return;
    }
    nds_->SetSlice((s32)slice, (s32)idle, (u32)hold);
    if (slice != 64 || idle != slice)
        Log(LogLevel::Info, "[emu] scheduler slice %ld, %ld while %ld cycles away from IPC\n", slice, idle, hold);
}

void Emu::service_net() {
#ifdef BANANADS_LAN
    std::lock_guard<std::mutex> g(net_mu_);
    lan_apply();
    lan_net_check(false);
    if (local_) MPInterface::Get().Process();
    update_mp_active();
    lan_auto_tick();
    lan_refresh(false);
#endif
}

void Emu::update_mp_active() {
#ifdef BANANADS_LAN
    bool on = false;
    if (local_ && nds_) {
        const LAN& lan = static_cast<const LAN&>(MPInterface::Get());
        on = lan.GetMPActiveBitmask() != 0 || (lan.GetOtherPlayersBitmask() != 0 && nds_->Wifi.InMPMode());
    }
    if (on != mp_active_.load(std::memory_order_relaxed)) {
        mp_active_.store(on, std::memory_order_relaxed);
        Log(LogLevel::Info, "[wifi] local wireless game %s\n", on ? "on: the menu's Save, Load and Restart are off"
                                                                   : "over");
    }
#endif
}

void Emu::write_firmware(const melonDS::Firmware& fw) {
    if (wfc_path_.empty()) return;
    if (fw.GetHeader().Identifier != GENERATED_FIRMWARE_IDENTIFIER) return;  // (a real firmware isn't ours to keep)
    std::vector<uint8_t> now;
    if (!wfc_settings_get(fw, now) || now == wfc_last_) return;
    if (write_file_durable(wfc_path_, now.data(), now.size())) {
        wfc_last_ = std::move(now);
        Log(LogLevel::Info, "[wifi] WFC settings saved to %s\n", wfc_path_.c_str());
    } else
        Log(LogLevel::Warn, "[wifi] couldn't save the WFC settings to %s (%s)\n", wfc_path_.c_str(), strerror(errno));
}

void Emu::run_frame() {
#ifdef BANANADS_LAN
    if (lan_pending_.load(std::memory_order_acquire)) {  // the menu's LAN page asked for something
        std::lock_guard<std::mutex> g(net_mu_);
        lan_apply();
    }
    if (local_) MPInterface::Get().Process();
#endif
    if (const double sk = skew_req_.load(std::memory_order_relaxed); sk != skew_cur_) {
        skew_cur_ = sk;
        nds_->SPU.SetOutputSkew(sk);
    }
    if (prof_) prof_->frame(frames_run_);  // prof.hz: on the emulator's thread, whichever runs the frames
    frames_run_++;
    nds_->RunFrame();
#ifdef BANANADS_LAN
    if (auto_on_) auto_wifi_off_frames_ = auto_wifi_on() ? 0 : auto_wifi_off_frames_ + 1;  // (Auto's grace)
    for (const auto& [f, m] : lan_test_modes_)
        if (f == frames_run_) lan_request(m);  // (wifi.local_test_mode: the LAN page's Mode row, for the tests)
#endif
    if (const uint32_t b = nds_->GPU.GPU3D.CurRAMBank; b != bank3d_) {  // the 3D swapped at this VBlank
        bank3d_ = b;
        swaps3d_.fetch_add(1, std::memory_order_relaxed);
    }
    flush_save(false);
    if (local_) {
        update_mp_active();
        lan_refresh(false);
    }
#ifdef BANANADS_LAN
    if (lan_st_built_ && now_us() - net_check_us_ >= 1000000) {  // a network to play on (every 1 s)
        std::lock_guard<std::mutex> g(net_mu_);
        lan_net_check(false);
    }
    if (auto_on_ && now_us() - auto_tick_us_ >= 100000) {  // Auto's steps (wifi.local = auto)
        std::lock_guard<std::mutex> g(net_mu_);
        lan_auto_tick();
    }
#endif
    if (slice_stats_ > 0 && ++slice_frames_ % slice_stats_ == 0) {
        // the share of emulated time within core.slice_hold cycles of an IPC access (where the short slice runs)
        const double near = 100.0 * (double)nds_->SliceNearIPCCycles / (560190.0 * (double)slice_stats_);
        Log(LogLevel::Info, "[emu] slice: frames %ld-%ld, %.0f IPC accesses a frame, %.1f%% of the time near one\n",
            slice_frames_ - slice_stats_ + 1, slice_frames_, (double)nds_->SliceIPCAccesses / (double)slice_stats_,
            near);
        nds_->SliceIPCAccesses = 0;
        nds_->SliceNearIPCCycles = 0;
    }
}

void Emu::set_cheats(std::vector<melonDS::ARCode> codes) { nds_->AREngine.Cheats = std::move(codes); }
void Emu::set_keys(uint32_t pressed) { nds_->SetKeyMask(~pressed & 0xFFF); }
void Emu::set_lid(bool closed) { lid_closed_ = closed; nds_->SetLidClosed(closed); }
void Emu::touch(int x, int y) {
    nds_->TouchScreen((u16)std::clamp(x, 0, 255), (u16)std::clamp(y, 0, 191));
}
void Emu::release_touch() { nds_->ReleaseScreen(); }

const uint8_t* Emu::main_ram() const { return nds_->MainRAM; }
const uint8_t* Emu::dtcm() const { return nds_->ARM9.DTCM; }
uint32_t Emu::main_ram_size() const { return nds_->MainRAMMask + 1; }
void Emu::cpu_regs(int cpu, uint32_t r[17]) const {
    const ARM& a = cpu == 0 ? (const ARM&)nds_->ARM9 : (const ARM&)nds_->ARM7;
    memcpy(r, a.R, 16 * 4);
    r[16] = a.CPSR;
}

bool Emu::set_lazy_2d(bool on) {
#ifdef BANANADS_2D
    if (!banana_) return false;
    banana_->SetLazyFrames(on);
    return true;
#else
    (void)on;
    return false;
#endif
}
bool Emu::frame_fence(uint32_t& fence) const {
#ifdef BANANADS_2D
    return banana_ && banana_->FrameFence(fence);
#else
    (void)fence;
    return false;
#endif
}
void Emu::wait_frame(uint32_t fence) const {
#ifdef BANANADS_2D
    if (banana_) banana_->WaitFence(fence);
#else
    (void)fence;
#endif
}

bool Emu::draw_into(uint32_t* top, uint32_t* bottom) {
#ifdef BANANADS_2D
    if (!banana_) return false;
    banana_->SetOutput(top, bottom);
    return true;
#else
    (void)top, (void)bottom;
    return false;
#endif
}

const uint32_t* Emu::top() const {
    void *t = nullptr, *b = nullptr;
    nds_->GPU.GetFramebuffers(&t, &b);
    return (const uint32_t*)t;
}

const uint32_t* Emu::bottom() const {
    void *t = nullptr, *b = nullptr;
    nds_->GPU.GetFramebuffers(&t, &b);
    return (const uint32_t*)b;
}

void Emu::set_audio_rate(double hz) {
    if (nds_) nds_->SPU.SetOutputSampleRate(hz);
}

int Emu::audio_backlog() const { return nds_->SPU.GetOutputSize(); }

int Emu::audio(int16_t* stereo, int max_frames) {
    int n = nds_->SPU.ReadOutput(stereo, max_frames);
    if (n > 0) mic_.note_output(stereo, n * 2);  // the echo gate wants to know what the speakers play
    return n;
}

void Emu::write_save(const uint8_t* data, uint32_t len, uint32_t off, uint32_t n) {
    // The game writes its save in many small pieces; it goes to disk a second after the last one. Only the piece
    // that changed is copied (#33): copying the whole save at every page cost Nintendogs' 256 KB save a 256 KB copy
    // per page written, frames of the emulator's thread over a whole save
    if (len != save_len_ || !save_copy_) {
        save_copy_ = std::make_unique<uint8_t[]>(len);
        save_len_ = len;
        memcpy(save_copy_.get(), data, len);
    } else if (n > 0 && off < len && n <= len - off) {
        memcpy(save_copy_.get() + off, data + off, n);
    } else {
        memcpy(save_copy_.get(), data, len);
    }
    save_dirty_us_ = now_us();
}

void Emu::save_writer() {
    for (;;) {
        std::vector<uint8_t> buf;
        {
            std::unique_lock<std::mutex> l(save_mu_);
            save_cv_.wait(l, [this] { return save_have_ || save_stop_; });
            if (!save_have_) return;  // stopping, and nothing left to write
            buf.swap(save_pending_);
            save_have_ = false;
        }
        const uint64_t t0 = now_us();
        if (write_file_atomic(save_path_, buf.data(), buf.size()))
            Log(LogLevel::Info, "[emu] saved %s (%.0f ms, beside the game)\n", save_path_.c_str(), (now_us() - t0) / 1000.0);
        else
            Log(LogLevel::Error, "[emu] couldn't write %s: %s\n", save_path_.c_str(), strerror(errno));
    }
}

// a second after the game's last write to its save, the save handed to the writer's thread; now (at quit): the
// writer finishes what it has, then the latest is written here
void Emu::flush_save(bool now) {
    if (now) {
        if (save_thr_.joinable()) {
            {
                std::lock_guard<std::mutex> l(save_mu_);
                save_stop_ = true;
            }
            save_cv_.notify_one();
            save_thr_.join();
        }
        if (!save_dirty_us_) return;
        if (write_file_atomic(save_path_, save_copy_.get(), save_len_))
            Log(LogLevel::Info, "[emu] saved %s\n", save_path_.c_str());
        else
            Log(LogLevel::Error, "[emu] couldn't write %s: %s\n", save_path_.c_str(), strerror(errno));
        save_dirty_us_ = 0;
        return;
    }
    if (!save_dirty_us_ || now_us() - save_dirty_us_ < 1000000) return;
    {
        std::lock_guard<std::mutex> l(save_mu_);
        save_pending_.assign(save_copy_.get(), save_copy_.get() + save_len_);
        save_have_ = true;
    }
    if (!save_thr_.joinable() && !save_stop_) {
        save_thr_ = std::thread([this] {
            pthread_setname_np(pthread_self(), "bds-save");
            save_writer();
        });
    }
    save_cv_.notify_one();
    save_dirty_us_ = 0;
}

std::string Emu::state_path(int slot) const {
    std::string dir = cfg_.str("state.dir", "");
    if (dir.empty()) {
        size_t s = save_path_.find_last_of('/');
        dir = s == std::string::npos ? "." : save_path_.substr(0, s);
    }
    return dir + "/" + game_ + (slot == 0 ? ".resume" : "." + std::to_string(slot)) + ".mst";
}

bool Emu::save_state(int slot) {
    std::lock_guard<std::mutex> g(net_mu_);
    Savestate st;
    if (st.Error) return false;
    nds_->DoSavestate(&st);
    if (st.Error) return false;
    std::string p = state_path(slot);
    mkdirs(p.substr(0, p.find_last_of('/')));
    if (!write_file_atomic(p, st.Buffer(), st.Length())) return false;
    // Both screens beside it, for the in-game menu's slot pictures (256x384 XRGB8888, top over bottom).
    std::vector<uint32_t> pic(256 * 384);
    const int s = scale_;  // (at 2x every other pixel of every other row)
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++) {
            pic[y * 256 + x] = top()[(y * 256 * s + x) * s];
            pic[(192 + y) * 256 + x] = bottom()[(y * 256 * s + x) * s];
        }
    write_file_atomic(p + ".pic", pic.data(), pic.size() * 4);
    Log(LogLevel::Info, "[emu] state saved: %s\n", p.c_str());
    return true;
}

bool Emu::load_state(int slot) {
    std::lock_guard<std::mutex> g(net_mu_);
    std::vector<uint8_t> buf;
    std::string p = state_path(slot);
    if (!read_file(p, buf) || buf.empty()) return false;
    Savestate st(buf.data(), (u32)buf.size(), false);
    if (st.Error || !nds_->DoSavestate(&st)) {
        Log(LogLevel::Error, "[emu] %s couldn't be loaded\n", p.c_str());
        return false;
    }
    // the lid as it is now: melonDS keeps it outside its savestates (KeyInput), and a state saved with the lid shut
    // (asleep on the charger, then a resume save at shutdown) has the game in its sleep mode, waiting for the
    // lid-open interrupt that nothing would send again. Open, this raises that interrupt; a game that isn't asleep
    // has it off.
    nds_->SetLidClosed(lid_closed_);
    Log(LogLevel::Info, "[emu] state loaded: %s\n", p.c_str());
    return true;
}

}  // namespace bananads

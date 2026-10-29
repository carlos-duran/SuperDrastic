#include "wifi.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;
using melonDS::u8;
using melonDS::u16;
using melonDS::u32;

namespace bananads {

uint32_t wfc_dns_address(const std::string& name) {
    if (name.empty() || name == "system") return 0;
    if (name == "kaeru") return 0xB23E2BD4;    // 178.62.43.212
    if (name == "altwfc") return 0xAC6858ED;   // 172.104.88.237
    if (name == "wiilink") return 0xA7EBE524;  // 167.235.229.36
    in_addr a;
    if (inet_pton(AF_INET, name.c_str(), &a) == 1) return ntohl(a.s_addr);
    return 0xFFFFFFFF;
}

std::array<uint8_t, 6> wifi_mac_from_id(const std::string& id) {
    // FNV-1a of the id: stable per device, different between devices.
    uint32_t h = 2166136261u;
    for (char c : id) h = (h ^ (uint8_t)c) * 16777619u;
    if (id.empty()) h = 0x112233;
    return {0x00, 0x09, 0xBF, (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h};
}

std::array<uint8_t, 6> wifi_mac() {
    std::ifstream f("/etc/machine-id");
    std::string id;
    std::getline(f, id);
    return wifi_mac_from_id(id);
}

// ---- the DS's MAC --------------------------------------------------------------------------------------------

bool parse_mac(const std::string& s, std::array<uint8_t, 6>& out) {
    std::array<uint8_t, 6> m{};
    size_t p = 0;
    for (int i = 0; i < 6; i++) {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
        if (i && p < s.size() && (s[p] == ':' || s[p] == '-')) p++;
        unsigned v = 0;
        int n = 0;
        while (n < 2 && p < s.size() && isxdigit((unsigned char)s[p])) {
            v = v * 16 + (unsigned)(isdigit((unsigned char)s[p]) ? s[p] - '0' : (tolower((unsigned char)s[p]) - 'a' + 10));
            p++;
            n++;
        }
        if (n != 2) return false;
        m[i] = (uint8_t)v;
    }
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
    if (p != s.size()) return false;
    if (m[0] & 1) return false;  // a multicast address can't be a station's
    bool zero = true, ones = true;
    for (uint8_t b : m) { zero &= b == 0; ones &= b == 0xFF; }
    if (zero || ones) return false;
    out = m;
    return true;
}

std::string format_mac(const std::array<uint8_t, 6>& m) {
    char b[24];
    snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

static std::string first_line(const std::string& path) {
    std::ifstream f(path);
    std::string l;
    if (f) std::getline(f, l);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
    return l;
}

static bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

bool write_file_durable(const std::string& path, const void* data, size_t len) {
    const std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const uint8_t* p = (const uint8_t*)data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n <= 0) {
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        done += (size_t)n;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    size_t s = path.find_last_of('/');
    const std::string dir = s == std::string::npos ? "." : s == 0 ? "/" : path.substr(0, s);
    int d = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d >= 0) {
        fsync(d);
        close(d);
    }
    return true;
}

// the eMMC's CID (a card that's type MMC: the handheld's own memory, not the SD card), "" if none
static std::string emmc_cid(const std::string& root) {
    const std::string dir = root + "/sys/bus/mmc/devices";
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::string best;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string base = dir + "/" + e->d_name;
        if (first_line(base + "/type") != "MMC") continue;
        std::string cid = first_line(base + "/cid");
        if (cid.size() >= 16 && (best.empty() || cid < best)) best = cid;  // (the same one every time if two)
    }
    closedir(d);
    return best;
}

static std::string wlan_mac(const std::string& root) {
    std::string a = first_line(root + "/sys/class/net/wlan0/address");
    std::array<uint8_t, 6> m;
    return parse_mac(a, m) ? format_mac(m) : "";
}

static std::array<uint8_t, 6> mac_from_seed(const std::string& seed) {
    uint32_t h = 2166136261u;
    for (char c : seed) h = (h ^ (uint8_t)c) * 16777619u;
    h = (h ^ (h >> 24)) & 0xFFFFFF;
    // not the address every BananaOS handheld had before (00:09:BF:11:22:33), nor all zeros or all ones
    if (h == 0x112233 || h == 0 || h == 0xFFFFFF) h ^= 0x5A5A5A;
    return {0x00, 0x09, 0xBF, (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h};
}

WifiMac choose_wifi_mac(const std::string& setting, const std::string& mac_file, const std::string& wfc_file,
                        const std::string& sysroot) {
    WifiMac r;
    std::array<uint8_t, 6> saved{};
    bool have_saved = false;
    if (!mac_file.empty() && exists(mac_file)) {
        const std::string l = first_line(mac_file);
        if (parse_mac(l, saved)) have_saved = true;
        else {
            Log(LogLevel::Warn, "[wifi] %s doesn't hold a MAC (\"%s\"): set aside as %s.bad\n", mac_file.c_str(),
                l.c_str(), mac_file.c_str());
            rename(mac_file.c_str(), (mac_file + ".bad").c_str());
        }
    }
    if (!setting.empty()) {
        if (parse_mac(setting, r.mac)) {
            r.source = "the wifi.mac setting";
            if (have_saved && saved != r.mac)
                Log(LogLevel::Warn, "[wifi] wifi.mac=%s isn't the saved MAC %s: a game's WFC ID made with the other "
                    "one may be refused\n", format_mac(r.mac).c_str(), format_mac(saved).c_str());
            return r;  // (not saved: without the setting the saved one comes back)
        }
        Log(LogLevel::Warn, "[wifi] wifi.mac=%s isn't a MAC (xx:xx:xx:xx:xx:xx, not multicast): ignored\n",
            setting.c_str());
    }
    if (have_saved) {
        r.mac = saved;
        r.source = "saved in " + mac_file;
        return r;
    }
    std::string how;
    if (!wfc_file.empty() && exists(wfc_file)) {
        // WFC settings saved without a MAC beside them: their WFC ID was made with the old MAC; keep it
        r.mac = wifi_mac();
        how = "the MAC before (" + wfc_file + " was there already)";
    } else if (std::string cid = emmc_cid(sysroot); !cid.empty()) {
        r.mac = mac_from_seed("bananads-mac/emmc-cid/" + cid);
        how = "made from the eMMC's CID";
    } else if (std::string w = wlan_mac(sysroot); !w.empty()) {
        r.mac = mac_from_seed("bananads-mac/wlan0/" + w);
        how = "made from wlan0's MAC";
    } else {
        uint8_t b[3] = {0, 0, 0};
        if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b) {
            uint32_t t = (uint32_t)time(nullptr) ^ (uint32_t)getpid() * 2654435761u;
            b[0] = (uint8_t)(t >> 16); b[1] = (uint8_t)(t >> 8); b[2] = (uint8_t)t;
        }
        char seed[32];
        snprintf(seed, sizeof seed, "bananads-mac/random/%02x%02x%02x", b[0], b[1], b[2]);
        r.mac = mac_from_seed(seed);
        how = "random";
    }
    if (mac_file.empty()) {
        r.source = how + ", not kept (no file)";
        return r;
    }
    std::string text = format_mac(r.mac) + "\n# the DS's MAC, BananaDS's (" + how + "); it stays this one\n";
    if (write_file_durable(mac_file, text.data(), text.size())) r.source = how + ", saved in " + mac_file;
    else {
        Log(LogLevel::Warn, "[wifi] couldn't save the MAC in %s (%s)\n", mac_file.c_str(), strerror(errno));
        r.source = how + ", not saved";
    }
    return r;
}

// ---- the Nintendo WFC settings --------------------------------------------------------------------------------

static bool wfc_layout_ok(const melonDS::Firmware& fw) {
    const u32 eap = fw.GetExtendedAccessPointOffset(), ap = fw.GetWifiAccessPointOffset();
    return eap + 3 * sizeof(melonDS::Firmware::ExtendedWifiAccessPoint) == ap &&
           ap + 3 * sizeof(melonDS::Firmware::WifiAccessPoint) <= fw.Length() &&
           eap + kWfcSettingsSize <= fw.Length();
}

bool wfc_settings_get(const melonDS::Firmware& fw, std::vector<uint8_t>& out) {
    if (!fw.Buffer() || !wfc_layout_ok(fw)) return false;
    const u8* p = fw.GetExtendedAccessPointPosition();
    out.assign(p, p + kWfcSettingsSize);
    return true;
}

int wfc_settings_load(melonDS::Firmware& fw, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::vector<uint8_t> b(kWfcSettingsSize + 1);
    const size_t n = fread(b.data(), 1, b.size(), f);
    fclose(f);
    if (n != kWfcSettingsSize) {
        Log(LogLevel::Warn, "[wifi] %s is %zu%s bytes, not %zu: set aside as %s.bad, the WFC settings start empty\n",
            path.c_str(), n, n > kWfcSettingsSize ? "+" : "", kWfcSettingsSize, path.c_str());
        rename(path.c_str(), (path + ".bad").c_str());
        return -1;
    }
    if (!fw.Buffer() || !wfc_layout_ok(fw)) {
        Log(LogLevel::Warn, "[wifi] the firmware's access points aren't where %s expects them: not loaded\n",
            path.c_str());
        return -1;
    }
    constexpr size_t kExt = 3 * sizeof(melonDS::Firmware::ExtendedWifiAccessPoint);
    constexpr size_t kAP = sizeof(melonDS::Firmware::WifiAccessPoint);
    u8* dst = fw.GetExtendedAccessPointPosition();
    int good = 0;
    for (int i = 0; i < 3; i++) {
        const u8* ap = b.data() + kExt + i * kAP;
        const u16 sum = (u16)(ap[0xFE] | ap[0xFF] << 8);
        if (melonDS::CRC16(ap, 0xFE, 0x0000) != sum) {
            Log(LogLevel::Warn, "[wifi] %s: access point %d's checksum is wrong: it starts empty\n", path.c_str(), i + 1);
            continue;
        }
        memcpy(dst + kExt + i * kAP, ap, kAP);
        good++;
    }
    memcpy(dst, b.data(), kExt);  // the DSi's extended ones, as melonDS keeps them (a DS doesn't read them)
    return good ? 1 : -1;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, v >> 16); wr16(p + 2, (uint16_t)v); }

bool WfcDns::is_dns_query(const uint8_t* f, int len) {
    if (len < 14 + 20 + 8 + 12) return false;
    if (rd16(f + 12) != 0x0800) return false;            // IPv4
    const uint8_t* ip = f + 14;
    int ihl = (ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || len < 14 + ihl + 8 + 12) return false;
    if (ip[9] != 17) return false;                        // UDP
    if (rd32(ip + 16) != kSlirpDNS) return false;
    const uint8_t* udp = ip + ihl;
    return rd16(udp + 2) == 53;
}

int WfcDns::build_reply(uint8_t* out, int cap, const uint8_t ds_mac[6], uint32_t ds_ip, uint16_t ds_port,
                        const uint8_t* dns, int dnslen) {
    int total = 14 + 20 + 8 + dnslen;
    if (dnslen <= 0 || total > cap) return 0;
    memcpy(out, ds_mac, 6);
    memcpy(out + 6, kSlirpMAC, 6);
    wr16(out + 12, 0x0800);
    uint8_t* ip = out + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    wr16(ip + 2, (uint16_t)(20 + 8 + dnslen));
    ip[8] = 64;  // TTL
    ip[9] = 17;
    wr32(ip + 12, kSlirpDNS);
    wr32(ip + 16, ds_ip);
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += rd16(ip + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    wr16(ip + 10, (uint16_t)~sum);
    uint8_t* udp = ip + 20;
    wr16(udp, 53);
    wr16(udp + 2, ds_port);
    wr16(udp + 4, (uint16_t)(8 + dnslen));
    wr16(udp + 6, 0);  // no UDP checksum (allowed on IPv4)
    memcpy(udp + 8, dns, dnslen);
    return total;
}

WfcDns::WfcDns(std::unique_ptr<melonDS::NetDriver> inner, uint32_t upstream,
               melonDS::Platform::SendPacketCallback to_ds)
    : inner_(std::move(inner)), upstream_(upstream), to_ds_(std::move(to_ds)) {
    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock_ < 0) Log(LogLevel::Error, "[wifi] DNS socket: %s; using the system resolver\n", strerror(errno));
}

WfcDns::~WfcDns() {
    if (sock_ >= 0) close(sock_);
}

int WfcDns::SendPacket(melonDS::u8* data, int len) noexcept {
    if (sock_ < 0 || upstream_ == 0 || !is_dns_query(data, len)) return inner_->SendPacket(data, len);
    const uint8_t* ip = data + 14;
    int ihl = (ip[0] & 15) * 4;
    const uint8_t* udp = ip + ihl;
    const uint8_t* dns = udp + 8;
    int dnslen = std::min<int>(rd16(udp + 4) - 8, len - 14 - ihl - 8);
    if (dnslen < 12) return len;
    Pending p;
    memcpy(p.mac, data + 6, 6);
    p.ip = rd32(ip + 12);
    p.port = rd16(udp);
    pending_[rd16(dns)] = p;
    if (pending_.size() > 64) pending_.erase(pending_.begin());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(upstream_);
    if (sendto(sock_, dns, dnslen, 0, (sockaddr*)&to, sizeof to) < 0)
        Log(LogLevel::Warn, "[wifi] DNS query to the WFC server: %s\n", strerror(errno));
    return len;
}

void WfcDns::RecvCheck() noexcept {
    inner_->RecvCheck();
    if (sock_ < 0) return;
    uint8_t buf[1500], frame[1600];
    for (;;) {
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(sock_, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 12) break;
        if (ntohl(from.sin_addr.s_addr) != upstream_) continue;  // only the server we asked
        auto it = pending_.find(rd16(buf));
        if (it == pending_.end()) continue;
        int fl2 = build_reply(frame, sizeof frame, it->second.mac, it->second.ip, it->second.port, buf, (int)n);
        pending_.erase(it);
        if (fl2) to_ds_(frame, fl2);
    }
}

// ---- the direct link ------------------------------------------------------------------------------------
extern "C" char** environ;

static uint64_t mono_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

DirectLink::DirectLink(std::string script, int channel) : script_(std::move(script)), channel_(channel) {
    thr_ = std::thread([this] { worker(); });
}

DirectLink::~DirectLink() {
    {
        std::lock_guard<std::mutex> l(mu_);
        quit_ = true;
        q_.clear();
    }
    cv_.notify_all();
    if (const pid_t p = child_.load()) kill(p, SIGTERM);  // (run() sees it end; the supervisor lives on)
    if (thr_.joinable()) thr_.join();
}

bool DirectLink::usable(const std::string& script) {
    struct stat sb;
    return !script.empty() && stat(script.c_str(), &sb) == 0 && S_ISREG(sb.st_mode) && access(script.c_str(), R_OK) == 0;
}

void DirectLink::up(const std::string& role) {
    {
        std::lock_guard<std::mutex> l(mu_);
        while (!q_.empty() && q_.back() != "down") q_.pop_back();  // (only the latest up counts)
        q_.push_back(role == "host" ? "host" : "join");
    }
    cv_.notify_all();
}

void DirectLink::down() {
    {
        std::lock_guard<std::mutex> l(mu_);
        q_.clear();
        q_.push_back("down");
        // an up still running (waiting for its cell, up to 35 s): stopped, so the down doesn't wait behind it; the
        // script's supervisor it started is told by the down
        if (busy_ && st_.state == State::Starting) {
            drop_up_ = true;
            if (const pid_t p = child_.load()) kill(p, SIGTERM);
        }
    }
    cv_.notify_all();
}

bool DirectLink::down_wait(int timeout_ms) {
    down();
    std::unique_lock<std::mutex> l(mu_);
    return cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [this] { return q_.empty() && !busy_; });
}

DirectLink::Status DirectLink::status() const {
    std::lock_guard<std::mutex> l(mu_);
    return st_;
}

void DirectLink::set(const Status& s) {
    {
        std::lock_guard<std::mutex> l(mu_);
        st_ = s;
    }
    gen_.fetch_add(1, std::memory_order_acq_rel);
}

bool DirectLink::parse_ok(const std::string& line, Status& st) {
    if (line.rfind("ok ", 0) != 0) return false;
    size_t i = 3;
    while (i < line.size()) {
        const size_t e = line.find(' ', i);
        const std::string w = line.substr(i, e == std::string::npos ? std::string::npos : e - i);
        const size_t eq = w.find('=');
        if (eq != std::string::npos) {
            const std::string k = w.substr(0, eq), v = w.substr(eq + 1);
            if (k == "role") st.role = v;
            else if (k == "ip") st.ip = v;
            else if (k == "freq") st.freq = atoi(v.c_str());
            else if (k == "state" && v != "up") return false;
        }
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return !st.ip.empty();
}

int DirectLink::run(const std::string& script, const std::vector<std::string>& args, int timeout_ms,
                    std::vector<std::string>& out, std::atomic<pid_t>* running) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    std::vector<std::string> av{"sh", script};
    av.insert(av.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : av) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    // nothing of BananaDS's own (the panels' DRM files, sockets) in the script or the supervisor it leaves running
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, dfl;
    sigemptyset(&none);
    sigemptyset(&dfl);
    sigaddset(&dfl, SIGPIPE);  // (BananaDS ignores it; the script's pipes shouldn't)
    sigaddset(&dfl, SIGCHLD);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &dfl);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    pid_t pid = 0;
    const int e = posix_spawn(&pid, "/bin/sh", &fa, &at, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(fds[1]);
    if (e != 0) {
        close(fds[0]);
        return -1;
    }
    if (running) running->store(pid);
    const uint64_t t0 = mono_ms();
    std::string buf;
    bool timed_out = false;
    for (;;) {
        const int64_t left = (int64_t)timeout_ms - (int64_t)(mono_ms() - t0);
        if (left <= 0) {
            timed_out = true;
            break;
        }
        pollfd p{fds[0], POLLIN, 0};
        const int r = poll(&p, 1, (int)std::min<int64_t>(left, 500));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        char b[512];
        const ssize_t n = read(fds[0], b, sizeof b);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;  // EOF: the script (and all that kept its output) has ended
        buf.append(b, (size_t)n);
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (running) running->store(0);
    size_t i = 0;
    while (i < buf.size()) {
        const size_t j = buf.find('\n', i);
        const std::string l = buf.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!l.empty()) out.push_back(l);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (timed_out) return -2;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void DirectLink::worker() {
    const std::string owner = std::to_string(getpid());
    for (;;) {
        std::string cmd;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return quit_ || !q_.empty(); });
            if (quit_) break;
            cmd = q_.front();
            q_.pop_front();
            busy_ = true;
            drop_up_ = false;
        }
        Status s = status();
        std::vector<std::string> out;
        if (cmd == "down") {
            if (s.state == State::Down) {  // (nothing of ours up: still asked, in case one was left behind)
                run(script_, {"down", "--nowait"}, 5000, out, &child_);
            } else {
                s.state = State::Stopping;
                set(s);
                const uint64_t t0 = mono_ms();
                const int rc = run(script_, {"down"}, 20000, out, &child_);
                Log(rc == 0 ? LogLevel::Info : LogLevel::Warn, "[wifi] direct link: down (%.1f s): %s\n",
                    (mono_ms() - t0) / 1000.0, out.empty() ? "(no answer)" : out.back().c_str());
            }
            Status d;
            d.msg = s.state == State::Failed ? s.msg : "";
            set(d);
        } else {
            Status n;
            n.state = State::Starting;
            n.role = cmd;
            n.ip = s.state == State::Up ? s.ip : "";
            n.freq = s.freq;
            set(n);
            std::vector<std::string> args{"up", cmd, "--owner", owner};
            if (channel_ >= 1 && channel_ <= 11) {
                args.push_back("--channel");
                args.push_back(std::to_string(channel_));
            }
            const uint64_t t0 = mono_ms();
            const int rc = run(script_, args, 60000, out, &child_);
            for (const std::string& l : out) Log(LogLevel::Debug, "[wifi] direct link: %s\n", l.c_str());
            Status r;
            r.role = cmd;
            const std::string last = out.empty() ? "" : out.back();
            if (drop_up_.load()) {
                Log(LogLevel::Info, "[wifi] direct link: up %s stopped by a down\n", cmd.c_str());
            } else if (rc == 0 && parse_ok(last, r)) {
                r.state = State::Up;
                r.role = cmd;
                Log(LogLevel::Info, "[wifi] direct link: up as %s at %s, %d MHz (%.1f s)\n", cmd.c_str(), r.ip.c_str(),
                    r.freq, (mono_ms() - t0) / 1000.0);
                set(r);
            } else {
                r.state = State::Failed;
                r.msg = rc == -1 ? "couldn't run " + script_
                      : rc == -2 ? "no answer from the link script in 60 s"
                      : last.rfind("error ", 0) == 0 ? last.substr(6) : last.empty() ? "the link script failed" : last;
                Log(LogLevel::Error, "[wifi] direct link: up as %s failed (%.1f s): %s\n", cmd.c_str(),
                    (mono_ms() - t0) / 1000.0, r.msg.c_str());
                set(r);
            }
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            busy_ = false;
        }
        cv_.notify_all();
    }
}

}  // namespace bananads

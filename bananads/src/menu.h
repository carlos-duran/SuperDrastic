// menu.h: the in-game menu (menu.c: SuperDrastic's, in BananaOS's colours) and what the frontend gives it. The
// frontend pauses the game (or, in a local wireless game, keeps it running: bmenu_host's live) and calls bmenu_run,
// which draws both panels and reads the input itself until the menu closes; everything it needs from the emulator and
// the handheld goes through bmenu_host.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BM_A, BM_B, BM_Y, BM_L, BM_R, BM_START, BM_UP, BM_DOWN, BM_LEFT, BM_RIGHT, BM_MENU, BM_X };
enum { BM_EV_KEY = 1, BM_EV_TOUCH = 2 };
typedef struct {
    int kind;      // BM_EV_KEY: key (BM_*) and down; BM_EV_TOUCH: a touch going down at (x, y), bottom panel pixels
    int key, down;
    int x, y;
} bmenu_ev;

// the menu's own slots 1-8 are the emulator's savestate slots 1-8; slot 9 keeps the game from before a load (undo)
enum { BM_UNDO_SLOT = 9 };

/* the Local multiplayer page (Quick settings > Local multiplayer: wifi.local, melonDS's LAN), as the frontend reports it */
enum { BM_LAN_OFF, BM_LAN_HOST, BM_LAN_JOIN, BM_LAN_AUTO, BM_LAN_MODES };   /* (the page shows Off, Auto, Host, Join) */
/* Auto's steps: waiting for the game to turn its wifi on, looking for hosts, joining one, hosting, joined, the game
 * online (Nintendo WFC: no local session), stopped by a problem (a UDP port taken: until the game's wifi goes off and
 * on, or Off then Auto) */
enum { BM_AUTO_WAIT, BM_AUTO_SEARCH, BM_AUTO_JOINING, BM_AUTO_HOST, BM_AUTO_JOINED, BM_AUTO_ONLINE, BM_AUTO_HELD };
enum { BM_LAN_IDLE, BM_LAN_BUSY, BM_LAN_READY, BM_LAN_FAILED };  /* idle: off or looking for hosts; ready: hosting or joined */
/* the direct link (Connection: Direct): coming up, up, coming down (the home Wi-Fi back), failed */
enum { BM_LINK_DOWN, BM_LINK_STARTING, BM_LINK_UP, BM_LINK_STOPPING, BM_LINK_FAILED };
#define BM_LAN_MAX 16
typedef struct {
    int mode, state;      /* BM_LAN_* */
    int max_players;      /* a host's session: how many, us included (2-16) */
    char name[32];        /* our player name (firmware.nickname, Settings > Nintendo DS > DS name) */
    char addr[48];        /* join: the host's address */
    char msg[96];         /* failed: what went wrong */
    char env[96];         /* "BANANADS_WIFI_LOCAL=host": set in the environment (bananads.env), it wins at each start */
    int nplayers;         /* in the session (ready), us included */
    struct { char name[32]; int host, me, ping; } players[BM_LAN_MAX];   /* ping in ms, -1 unknown */
    int nhosts;           /* join, looking: the hosts on the network */
    struct { char name[64]; char addr[48]; int players, max; } hosts[BM_LAN_MAX];
    int phase;            /* auto: BM_AUTO_* */
    int untagged;         /* auto, joined: the host doesn't say its game (desktop melonDS) */
    int no_net;           /* no network (Wi-Fi off or not connected): nothing starts; the mode is kept */
    int host_players;     /* Host's size (wifi.local_players, 2-16): a session Host or Auto starts */
    /* Connection (wifi.local_link): Router (this Wi-Fi network) or Direct (the consoles linked without a router,
     * BananaOS's wifi-link; this console off its Wi-Fi network, so without internet, while it is up) */
    int link_avail;       /* Direct can be picked (its script is there): the page has a Connection row */
    int link;             /* 1: Direct */
    int link_state;       /* BM_LINK_* */
    char link_ip[48];     /* up: our address on the link */
    int link_channel;     /* up: its 2.4 GHz channel (0: unknown) */
} bmenu_lan;

typedef struct {
    void* ctx;
    const char* rom;  // the game's file
    int resume_on;    // quitting writes the resume state ("keeps your spot")
    // a buffer to draw the panel's next picture in (panel 0 top, 1 bottom): its index, or -1
    int (*canvas)(void* ctx, int panel, uint32_t** px, int* pitch, int* w, int* h);
    void (*show)(void* ctx, int panel, int k);  // buffer k onto the panel
    // the game's top screen as it was when the menu opened, at the panel's size; 0: none
    int (*grab_top)(void* ctx, uint32_t* dst, int pitch, int w, int h);
    int (*poll)(void* ctx, bmenu_ev* ev);           // the next input event; 0: none waiting
    int (*battery)(void* ctx, int* charging);       // percent, or -1
    int (*save)(void* ctx, int slot);               // 0: done
    int (*load)(void* ctx, int slot);               // 0: done
    void (*state_file)(void* ctx, int slot, char* out, int n);  // the slot's file (+ ".pic": 256x384 XRGB8888)
    void (*quit)(void* ctx);                        // after the menu closes: the resume state, then the menu
    // Quit without saving: as quit, but no resume state is written (the one from before stays as it was); unset: the
    // quit question has no such choice
    void (*quit_nosave)(void* ctx);
    // the Hotkeys page: the frontend's buttons, i from 0, each its keys ("F + R") and what it does ("Save state (slot 1)");
    // 0 past the last one. Unset: no Hotkeys page
    int (*hotkey)(void* ctx, int i, const char** keys, const char** what);
    void (*restart)(void* ctx);                     // the game from its start
    void (*blow)(void* ctx, int ms);                // the game hears a blow on the mic
    int (*mic_meter)(void* ctx, float* level, float* floor_, float* bar);  // -1 no mic, 0 starting, 1 listening
    void (*sound_load)(void* ctx, int id, const int16_t* pcm, int frames, int rate, int channels);
    void (*sound_play)(void* ctx, int id);
    void (*boost)(void* ctx, int ms);               // the top CPU clock for a while (the governor)
    void (*log)(void* ctx, const char* line);
    // the emulator's own settings on the Quick settings page, after Volume, Brightness and Microphone (the shader,
    // edge marking, fog...): how many, each one's label, its values' names and the current one; opt_set applies a new
    // value at once (and keeps it for the next game). opt_count 0 or unset: none
    int (*opt_count)(void* ctx);
    const char* (*opt_label)(void* ctx, int i);
    int (*opt_values)(void* ctx, int i);
    const char* (*opt_value_name)(void* ctx, int i, int v);
    int (*opt_get)(void* ctx, int i);
    void (*opt_set)(void* ctx, int i, int v);
    // the game's cheats (Quick settings > Cheats): how many, each one's name, on or off; cheat_set turns one on or
    // off and keeps it for the next time. cheat_count unset: no Cheats row
    int (*cheat_count)(void* ctx);
    const char* (*cheat_name)(void* ctx, int i);
    int (*cheat_get)(void* ctx, int i);
    void (*cheat_set)(void* ctx, int i, int on);
    // 1: a local wireless game is on (wifi.local, another handheld in it): Save, Load and Restart game are off, as a
    // load or a restart would break it. Asked as the menu opens; unset: never
    int (*mp_lock)(void* ctx);
    // 1: the game keeps running behind the menu (a local wireless game), with no input from the player; Save, Load,
    // Restart game and Cheats are off meanwhile, whatever mp_lock says. Asked as the menu opens and every 250 ms while
    // it is open: 0 once the frontend has paused the game (the local game over), and the menu goes on as the paused
    // one. Called on the menu's thread; unset: never
    int (*live)(void* ctx);
    // Local multiplayer (Quick settings): lan_get fills st, 0 when this build has no LAN (no row). lan_set asks for a
    // mode: BM_LAN_HOST with max_players, BM_LAN_JOIN with a host's address to join it or "" to look for hosts,
    // BM_LAN_AUTO (a session it hosts has Host's size; max_players unused), BM_LAN_OFF; the mode (not the address) is
    // kept for the next game. It returns at once: lan_get says when it is done (a join can take 5 s). Mode changes only
    // outside a local wireless game (mp_lock, live), but for Off from Auto: the menu sees to it
    int (*lan_get)(void* ctx, bmenu_lan* st);
    void (*lan_set)(void* ctx, int mode, int max_players, const char* addr);
    // Connection: 1 Direct, 0 Router; kept for the next game (wifi.local_link). Only with the mode Off (the menu sees
    // to it); Direct's link comes up when Host, Join or Auto starts. Unset: no Connection row
    void (*lan_link_set)(void* ctx, int direct);
} bmenu_host;

void bmenu_init(const bmenu_host* host);  // once, before the first bmenu_run
// open the menu and run it until it closes: start 0 the main page, 1 Save, 2 Load, 3 Quick settings, 4 the quit
// question; returns 0 if it couldn't open (no fonts, no panel buffers)
int bmenu_run(int start);
void bmenu_session_start(void);  // the game started: "this session" counts from here
// a pop-up over the game's top screen (an achievement unlocked...): the frontend calls this with each finished top
// frame (XRGB8888, pitch in pixels), on the emulator's thread; it draws the current pop-up, if any, into it
int bmenu_overlay(uint32_t* px, int pitch, int w, int h);   // 1: something drawn (px NULL: would be)
int bmenu_overlay_rows(int w, int h);                       // the top rows it draws into, at most
// the FPS counter in the top panel's corner (drawn by bmenu_overlay): the frames emulated a second x 10, -1 hidden; and
// the fast-forward speed beside it (0: off, 99: as fast as it goes)
void bmenu_fps(int fps_x10, int ff);
/* the game's own frame rate (its 3D frames a second, x10) beside the emulator's, -1 when it draws no 3D */
void bmenu_fps_game(int fps_x10);
// a pop-up of the frontend's own (a screenshot saved...): shown like an achievement's, for ms; any thread
void bmenu_popup(const char* line1, const char* line2, int ms);
// the game's name as the menu shows it (the gamelist's, else the file's), for the frontend's pop-ups; looked up at the
// first call (not while the menu is open on another thread)
const char* bmenu_game(void);

#ifdef __cplusplus
}
#endif

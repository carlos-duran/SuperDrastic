# Local wireless over the network (LAN)

BananaDS plays a DS game's local wireless (tested with Mario Kart DS's local groups and races; other games are untested) between
handhelds, or a handheld and desktop melonDS 1.0/1.1, on the same Wi-Fi network, through melonDS's LAN (ENet, UDP
7064 for the game and 7063 for the hosts' broadcasts, once a second). It needs `-DBANANADS_LAN=ON` and libenet
(BananaLinux builds it so); local wireless stays off until `wifi.local` or the in-game menu asks for it. The
settings are in [SETTINGS.md](SETTINGS.md) (`wifi.local`, `wifi.local_players`, `wifi.local_link`,
`firmware.nickname`). Without a router, the consoles can also link directly (Connection: Direct, below).

Desktop melonDS's own LAN dialog warns, and so does the menu's page: LAN requires low network latency to work; do
not expect it to work through a VPN or any sort of tunnel.

## The in-game menu's LAN multiplayer page (Quick settings > LAN multiplayer)

- **Connection: Router / Direct** (only when the direct link's script is there, below; changed only with the mode
  Off, kept in `wifi.local_link`): Router plays over the Wi-Fi network the console is on; Direct links the
  consoles without a router.
- **Mode: Off / Auto / Host / Join**, changed while the game runs, without a restart, and kept in `bananads.conf`
  (`wifi.local = off | auto | host | join`; `join` looks for hosts at the start, `join:<address>` joins that host
  directly). `BANANADS_WIFI_LOCAL` / `BANANADS_WIFI_LOCAL_PLAYERS` in the environment still win at each start, and
  the page says so in red.
- **Players** (2-16, 4 by default, `wifi.local_players`): only with Host on the Mode row; greyed, "Turn Host on to
  change it", until Host is on, and while others are in its session. Auto hosts with the same count.
- **Hosts found** (Join only): the hosts heard on the network, with their name, players and address; A joins one
  ("Connecting..." for up to 5 s; the page says why a join failed, and goes on looking).
- **Disconnect** (only on the Mode row of the mode that is on, Host or Join): a host's session ends; a player goes
  back to looking for hosts. Auto has none: Off leaves at once, and Auto again starts afresh.
- A status box: what the mode is doing, the session's players with their ping, desktop melonDS's warning (with
  Auto, Host or Join on the Mode row) and the name the others see (the DS name, Settings > Nintendo DS).
- Another mode picked while others are in the session (Host with a player, Join or Auto in a host's session,
  Auto hosting players) asks first ("Disconnect from Host?"; No keeps the mode). During a local wireless game only
  Off (Auto) or Disconnect works, after a question.
- Without a network (an interface up and running with an IPv4 address, looked for every second) nothing starts:
  Quick settings' row says "No Wi-Fi" and doesn't open, the mode is kept, and it starts again when the network is
  back. A session ends when the network goes. Nintendo WFC (slirp) isn't affected. With a Connection row the page
  opens anyway, as Direct needs no network.

## Direct link (Connection: Direct)

Not yet tested on the handhelds: it is the proof of concept (two BananaOS handhelds on an ad hoc cell, ping
between them about halved) made into a menu option, and only the script's logic has been tried (against a mock
of the system's tools). With Direct, Host, Join and Auto first run `link.script` (BananaLinux:
`/usr/libexec/banana/wifi-link`; `BANANADS_LINK_SCRIPT` in the environment overrides it) as
`/bin/sh <script> up host|join --owner <BananaDS's pid> [--channel N]` on a thread of its own, wait for it (up to
60 s; the status box says "Starting the direct link... (up to 30 s)") and then use the link for the hosts'
broadcasts and ENet as on a router. The script:

- takes the Wi-Fi off the home network (BananaOS's `wifi down`, holding its `/run/banana/wifi.lock` while the link
  is up) and joins or starts an IBSS ("ad hoc") cell: SSID `bananalink`, BSSID 02:42:4E:41:00:01, 2.4 GHz channel 1
  (`wifi.local_link_channel` 1-11, or `CHANNEL=` in `/data/banana/wifi-link.conf`). Every console must use the same
  channel.
- No DHCP: the host takes 10.42.0.1/16 (another address if it is taken), a player 10.42.A.B made from its wlan0
  MAC (A, B 1-254), each checked first with an ARP probe and moved on a clash, so 4-16 consoles don't collide. A
  default route on wlan0 (no gateway) lets the broadcasts out.
- The cell is looked for before it is joined (its own scans until a fresh beacon of the BSSID: a player up to 15 s,
  a host 3 s), and a player checks every 10 s that the host (10.42.0.1) answers an ARP request; while it doesn't,
  it leaves the cell, looks for it again (35 s at most) and joins again. With both consoles busy with the game,
  mac80211's own two short scans can miss the host's cell; it then starts one of its own with the same BSSID, which
  it never merges and which may not hear the host (the first test on the handhelds: "looking for hosts" for good).
- A host that receives nothing starts its cell again every 20 s. Alone for 30 s, mac80211 scans for a cell to
  merge with, and on these handhelds (rtw88_8821cs) the host's cell is then no longer heard: a player that comes
  later starts its own and never gets the host's discovery broadcasts, though ARP and ping still pass (the second
  test on the handhelds, 2026-10-29). A player's own cell, if it started one, is adopted at the next start.
- Open cell: BananaOS's wpa_supplicant is built without IBSS RSN, so the cell is not encrypted and anyone in
  reach can join it while it is up. The console has no internet meanwhile (the page says so).
- Puts the home Wi-Fi back (only if it was up before) on Off, a host's Disconnect, Router, quitting the game,
  BananaDS crashing (a supervisor watches its pid), a sleep (BananaOS's sleep.d `05-wifi-link`, and the supervisor
  sees BananaDS stopped), its wpa_supplicant dying, or the supervisor itself being killed (a watchdog). A player's
  Disconnect keeps the link and looks for hosts again. Its log: `/data/banana/logs/wifi-link.log`.

A link that fails (no cell after 20 s, the script failing or not answering) turns the mode Off with the reason in red; a
link that goes away while on (two missed checks a second apart) does the same. A kept `wifi.local` other than
`off` with Direct doesn't start the link at a game's start: the console never loses its network without a choice
on the page. Auto with Direct, a sleep with the link up and more than two consoles are untested.

## Auto

Every console in the room on Auto (or one on Host and the rest on Auto) is enough. Nothing happens until the game
turns its wifi on (its multiplayer menu); then it listens 2 s plus up to 1 s at random and joins a
host of the same game (the discovery beacon's game tag, melonDS patch 0094; one hosting by hand first, else the
lowest address), else a desktop melonDS host if there is no tagged one, else hosts. Of two that start together,
the one with the higher address joins the other. Nothing changes during a local wireless game; a player whose host
leaves looks again; a game that goes online (Nintendo WFC) gets no session. A host alone whose game turns its wifi
off for 3 s of game time (it left its multiplayer mode; Mario Kart DS turns it off and on for under 2 s between its
own screens) ends its session and waits; with others in it, after 30 s off. A UDP port already taken stops Auto
("Auto: stopped") until the game's wifi goes off and on, or Off then Auto.

## The game around it

- While the menu has the game paused, the emulator thread services the LAN session every 60 ms, so ENet doesn't
  time the others out. During a local wireless game the menu doesn't pause it at all: the game gets neutral input
  and the sound plays at half volume, Save/Load state, Restart and Cheats are off, and a session that ends while
  the menu is open pauses the game then (`kms.menu_live = off`: the old behaviour).
- Run-ahead (`runahead`, `kms.lowlat=runahead1`/`runahead2`) doesn't run while a LAN session or discovery is on
  (`Emu::runahead_ok`), and a frame run ahead sends and takes in nothing on the LAN.
- `wfcsettings.bin` (Nintendo WFC's settings) is saved, atomically, in desktop melonDS's layout, so the WFC ID
  isn't registered again after every restart.
- Each console gets a DS MAC of its own (`wifi.mac`, else one kept in `wifi-mac`, made from the eMMC CID, wlan0 or
  at random; always 00:09:BF), so two handhelds don't clash.

## melonDS patches (patches/melonds)

- **0093** (Wifi): the end-of-window power-down rule. A Mario Kart DS parent stays awake after its beacons (1.1 put
  it to sleep, so its children's requests rarely landed), a child follows 1.1's rule (906e9eb's kept it awake when
  it had to sleep, so it could never join), WFC keeps 906e9eb's rule, dswifi's mode 2 without auto wake-up stays on
  (#2680/#2681).
- **0094** (LAN): the host waits only for the players in its DS group (one in its menu or between races no longer
  drops everyone to ~38 fps for ~16 s); players who join late learn that our wifi is on; accessors for the frontend;
  one session after another in the same run; the game tag and Status in the discovery beacon (same size and
  layout: stock melonDS shows the name as before); the others hear at once that we leave.
- **0095** (LAN): a child repeats its auth/association request every frame until the parent answers (1 s at most).
  Desktop melonDS 1.0/1.1 hosting a group sleeps between its beacons and drops a frame that came while it slept,
  so a single request reached it only by chance; a parent that stays awake answers before the first copy.
- **0096** (LAN, for testing): `BANANADS_LAN_TIMEOUT_MS` (1-100, in /data/banana/bananads.env) replaces the 25 ms
  the host waits for its players' replies to each MP command, and a child for the host (unset: 25 ms, as before).
  The value in use is logged at startup ("LAN: MP receive timeout"); with the variable set, the per-second
  "[diag] LAN last" line comes at the Info level on both sides, with the waits (count, total, longest) and the
  host's reply waits that timed out.
- **0097** (LAN, for testing, diag-respuesta): with `BANANADS_LAN_TIMEOUT_MS` set, a second per-second line,
  "[diag] LAN resp" (times in ms, mean/p95[/max]): `frames` in the window, `run` (RunFrame's time, its LAN waits
  included), `emu` (the same minus those waits), `sleep` (the pacer's, in the window, from the frontend); on the host
  `rtt` (a MP command sent to all its replies in) and `pre` (the command sent to the start of that wait); on a child
  `reply` (the host's command handed to its emulation to its reply sent).
- **0098** (LAN, for testing, diag-respuesta-temprana; no behavior change): with `BANANADS_LAN_TIMEOUT_MS` set, a
  child logs "[diag] LAN early" once a second: its reply slot (`W_TXSlotReply1`, the frame, its length, valid and
  short enough for the CMD's reply time) snapshotted when the host's CMD arrives (`arr`) and when its reception starts
  (`rx`), against what `SendMPReply` sends at the end of that reception: `ok` (the same frame, less the TX header and
  the sequence control), `blank` (no reply either time), `pay` (another payload; `pay offs`, the first differing
  802.11 offsets of up to 4 `arr` samples), `len`, `notready` (no valid slot then, one at the reply), `gone` (the
  reverse), `nosnap`; `validatarr`; `slot1 writes` (of `W_TXSlotReply1`, its latch and reset included) and `buf
  writes` (into the snapshotted frame) between the CMD's arrival and the reply, and in how many replies.
- Their other "[diag]" traces log at the Debug level only (`log.level = 0`).

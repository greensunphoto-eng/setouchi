# Claude DS line protocol (v1)

Plain ASCII text lines ending in `\n`, at most ~200 bytes. The DS is a TCP
client of either the ESP32 dongle (`192.168.4.1:7777`, which relays every line
over USB serial at 115200 baud) or, in router mode, of the bridge itself.

Fields are separated by single spaces. Names never contain spaces (the bridge
sanitizes them to `[A-Za-z0-9._-]`, max 14 chars). Unknown lines are ignored on
both sides. `-1` means "unknown".

## Bridge -> DS

| Line | Meaning |
| --- | --- |
| `HI <proto> <version>` | Answer to `HELLO`. A full sync (ST, SC/SI, PR) follows. |
| `ST k=v ...` | Status, sent every second. Keys: `ses` sessions, `wrk` working, `wai` waiting for the user, `cpu` %, `mem` %, `dsk` free GB, `unp` unpushed commits, `ctx` highest context use %, `h5` 5-hour limit %, `wk` weekly limit %, `rem` remote approval 0/1, `pend` pending requests, `clk` HH:MM. |
| `SC <shown> <total>` | Number of session rows that follow (max 5) / sessions in total. |
| `SI <idx> <state> <age_s> <ctx> <name>` | Session row. State `W` working, `A` needs you, `D` done, `I` idle. |
| `EV DONE <name>` | A session finished (DS plays the "done" chime). |
| `EV WAIT <name> <reason>` | A session needs you (`permission`, `question`, `input`). |
| `PR <id> <more> <tool> <session> <detail...>` | Permission request to show; `more` = requests queued behind it. Sent again when `more` changes. |
| `PX <id>` | Close that request (answered elsewhere, timed out, cancelled). |
| `PONG` | Answer to `PING`. |
| `WHO` | The bridge doesn't know this DS (e.g. it restarted): send `HELLO` again. |
| `ERR auth` | Router mode: wrong token; the connection is closed. |

## DS -> bridge

| Line | Meaning |
| --- | --- |
| `HELLO claude-ds <proto> [token]` | First line after connecting. The token only matters in router mode. |
| `ANS <id> <allow\|deny\|stop\|pc>` | Answer a permission request. `stop` denies and interrupts Claude; `pc` hands the question back to the normal dialog on the PC. |
| `REM <0\|1>` | Turn remote approval off/on. |
| `PING` | Keep-alive, every 5 s. The bridge drops the DS after 20 s of silence, the DS reconnects after 15 s without data. |

## ESP32 <-> bridge (serial only)

Lines starting with `#` are between the bridge and the ESP32 and are never
relayed. The ESP32 also drops `#` lines coming from the DS.

| Direction | Line | Meaning |
| --- | --- | --- |
| ESP32 -> PC | `#ESP READY <ssid> <fw> ch=<n> proto=0x<n> paired=<mac\|none> ip=<ip>` | Boot / `#INFO` answer. |
| ESP32 -> PC | `#DS UP <ip>` / `#DS DOWN` | DS TCP connection opened / closed. |
| ESP32 -> PC | `#STA JOIN\|LEAVE\|BLOCKED <mac>` | Wi-Fi station events. Unpaired stations are kicked. |
| ESP32 -> PC | `#PAIRED <mac>` / `#UNPAIRED` | Pairing changes. |
| PC -> ESP32 | `#INFO`, `#UNPAIR`, `#PING` | Status, forget the paired DS, ping (`#PONG`). |

## Claude Code side

`claude_ds_hook.py` posts every hook event to `http://127.0.0.1:8787/hook`.
`PermissionRequest` goes to `/permission`, which blocks until the DS answers
(or returns `{"decision": null}` right away when remote approval is off, the DS
is offline, the request times out, or the hook process goes away). The
statusline mode posts the statusline JSON to `/statusline` for the CONTEXT,
5H and WEEK numbers. `GET /status` shows the bridge state as JSON.

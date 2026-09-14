// aapctl - console client for the AapL2cap driver (Apple Accessory Protocol
// over L2CAP PSM 0x1001).
//
// Commands:
//   aapctl list                     print device interface paths
//   aapctl monitor                  handshake + subscribe, decode events until Ctrl+C
//   aapctl mode off|anc|transparency|adaptive
//   aapctl ca on|off                conversational awareness
//   aapctl raw <hex bytes...>       send raw packet, print replies for 2 s
//   aapctl decode <hex bytes...>    decode a captured packet offline
//
// Protocol reference: LibrePods "AAP Definitions"
// (https://github.com/kavishdevar/librepods/blob/main/docs/AAP%20Definitions.md)

#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aapl2cap_public.h"

#pragma comment(lib, "cfgmgr32.lib")

namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// AAP packets
// ---------------------------------------------------------------------------

const Bytes kHandshake        = {0x00,0x00,0x04,0x00,0x01,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
const Bytes kFeatureCaps      = {0x04,0x00,0x04,0x00,0x4D,0x00,0xFF,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
const Bytes kNotifyFilter     = {0x04,0x00,0x04,0x00,0x0F,0x00,0xFF,0xFF,0xFF,0xFF};
const Bytes kAllowOffOption   = {0x04,0x00,0x04,0x00,0x09,0x00,0x34,0x01,0x00,0x00,0x00};

constexpr uint8_t kHdr[4] = {0x04, 0x00, 0x04, 0x00};

enum Opcode : uint16_t {
    OP_BATTERY   = 0x0004,
    OP_EAR       = 0x0006,
    OP_CONTROL   = 0x0009,
    OP_METADATA  = 0x001D,
    OP_CA_EVENT  = 0x004B,
};

enum ControlId : uint8_t {
    CTL_LISTENING_MODE   = 0x0D,
    CTL_ONE_BUD_ANC      = 0x1B,
    CTL_CONV_AWARENESS   = 0x28,
    CTL_ADAPTIVE_NOISE   = 0x2E,
    CTL_ALLOW_OFF_OPTION = 0x34,
};

Bytes ControlPacket(uint8_t id, uint8_t value) {
    return {0x04,0x00,0x04,0x00,0x09,0x00, id, value, 0x00,0x00,0x00};
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

volatile bool g_stop = false;

BOOL WINAPI CtrlHandler(DWORD) {
    g_stop = true;
    return TRUE;
}

std::string Timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    s.reserve(n * 3);
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02X", p[i]);
        if (i) s += ' ';
        s += b;
    }
    return s;
}

void Log(const char* fmt, ...) {
    std::printf("[%s] ", Timestamp().c_str());
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

void Err(const char* what, DWORD gle = GetLastError()) {
    char msg[512] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, gle,
                   0, msg, sizeof(msg), nullptr);
    for (char* p = msg; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';
    std::fprintf(stderr, "error: %s (0x%08lX %s)\n", what, gle, msg);
}

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------

const char* EarState(uint8_t v) {
    switch (v) {
    case 0x00: return "In Ear";
    case 0x01: return "Out";
    case 0x02: return "In Case";
    case 0x03: return "Disconnected";
    default:   return "?";
    }
}

const char* BatteryComponent(uint8_t v) {
    switch (v) {
    case 0x01: return "Headset";
    case 0x02: return "Right";
    case 0x04: return "Left";
    case 0x08: return "Case";
    default:   return "?";
    }
}

const char* BatteryStatus(uint8_t v) {
    switch (v) {
    case 0x00: return "unknown";
    case 0x01: return "charging";
    case 0x02: return "discharging";
    case 0x04: return "disconnected";
    case 0x05: return "optimized";
    default:   return "?";
    }
}

const char* ListeningMode(uint8_t v) {
    switch (v) {
    case 0x01: return "Off";
    case 0x02: return "ANC";
    case 0x03: return "Transparency";
    case 0x04: return "Adaptive";
    default:   return "?";
    }
}

const char* OnOff12(uint8_t v) {
    switch (v) {
    case 0x01: return "on";
    case 0x02: return "off";
    default:   return "?";
    }
}

void DecodePacket(const uint8_t* p, size_t n) {
    if (n >= 4 && p[0] == 0x01 && p[1] == 0x00 && p[2] == 0x04 && p[3] == 0x00) {
        Log("HANDSHAKE ACK  %s", Hex(p, n).c_str());
        return;
    }
    if (n < 6 || std::memcmp(p, kHdr, 4) != 0) {
        Log("RAW (%zu)  %s", n, Hex(p, n).c_str());
        return;
    }

    const uint16_t op = static_cast<uint16_t>(p[4] | (p[5] << 8));
    const uint8_t* body = p + 6;
    const size_t bodyLen = n - 6;

    switch (op) {
    case OP_EAR:
        if (bodyLen >= 2) {
            Log("EAR       primary=%s secondary=%s", EarState(body[0]), EarState(body[1]));
        } else {
            Log("EAR (short)  %s", Hex(p, n).c_str());
        }
        break;

    case OP_BATTERY: {
        if (bodyLen < 1) { Log("BATTERY (short)  %s", Hex(p, n).c_str()); break; }
        const uint8_t count = body[0];
        std::string s;
        size_t off = 1;
        for (uint8_t i = 0; i < count && off + 5 <= bodyLen; ++i, off += 5) {
            char b[96];
            std::snprintf(b, sizeof(b), "%s%s=%u%% (%s)", i ? "  " : "",
                          BatteryComponent(body[off]), body[off + 2], BatteryStatus(body[off + 3]));
            s += b;
        }
        Log("BATTERY   %s%s", s.c_str(), count ? "  [first listed = primary]" : "");
        break;
    }

    case OP_CONTROL: {
        if (bodyLen < 2) { Log("CONTROL (short)  %s", Hex(p, n).c_str()); break; }
        const uint8_t id = body[0], val = body[1];
        switch (id) {
        case CTL_LISTENING_MODE:   Log("CONTROL   listening mode = %s (%02X)", ListeningMode(val), val); break;
        case CTL_CONV_AWARENESS:   Log("CONTROL   conversational awareness = %s (%02X)", OnOff12(val), val); break;
        case CTL_ONE_BUD_ANC:      Log("CONTROL   one-bud ANC = %s (%02X)", OnOff12(val), val); break;
        case CTL_ADAPTIVE_NOISE:   Log("CONTROL   adaptive noise level = %u", val); break;
        case CTL_ALLOW_OFF_OPTION: Log("CONTROL   allow-off option = %02X", val); break;
        default:                   Log("CONTROL   id=%02X value=%02X  %s", id, val, Hex(body, bodyLen).c_str()); break;
        }
        break;
    }

    case OP_CA_EVENT:
        // 04 00 04 00 4B 00 02 00 01 [level]
        if (bodyLen >= 4) {
            Log("CA EVENT  level=%u  %s", body[3], Hex(body, bodyLen).c_str());
        } else {
            Log("CA EVENT  %s", Hex(body, bodyLen).c_str());
        }
        break;

    case OP_METADATA: {
        // null-terminated UTF-8 strings (name, model, manufacturer, serial, fw ...)
        static const char* labels[] = {"name", "model", "manufacturer", "serial", "fw1", "fw2", "hw", "s6", "s7", "s8"};
        std::string s;
        size_t i = 0, field = 0;
        while (i < bodyLen) {
            size_t j = i;
            while (j < bodyLen && body[j] != 0) ++j;
            bool printable = j > i;
            for (size_t k = i; k < j && printable; ++k) if (body[k] < 0x20 || body[k] == 0x7F) printable = false;
            if (printable) {
                char b[32];
                std::snprintf(b, sizeof(b), "\n    %-12s ", field < _countof(labels) ? labels[field] : "str");
                s += b;
                s.append(reinterpret_cast<const char*>(body + i), j - i);
            } else if (j > i) {
                char b[32];
                std::snprintf(b, sizeof(b), "\n    %-12s ", "bin");
                s += b + Hex(body + i, j - i);
            }
            ++field;
            i = j + 1;
        }
        Log("METADATA  (%zu bytes)%s", bodyLen, s.c_str());
        break;
    }

    default:
        Log("OP %04X (%zu)  %s", op, n, Hex(p, n).c_str());
        break;
    }
}

// ---------------------------------------------------------------------------
// Device access
// ---------------------------------------------------------------------------

std::vector<std::wstring> EnumerateInterfaces() {
    std::vector<std::wstring> out;
    GUID guid = AAPL2CAP_DEVICE_INTERFACE;
    ULONG len = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&len, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || len < 2) return out;
    std::vector<wchar_t> buf(len);
    cr = CM_Get_Device_Interface_ListW(&guid, nullptr, buf.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS) return out;
    for (const wchar_t* p = buf.data(); *p; p += std::wcslen(p) + 1) out.emplace_back(p);
    return out;
}

class Channel {
public:
    ~Channel() { Close(); }

    bool Open() {
        auto list = EnumerateInterfaces();
        if (list.empty()) {
            std::fprintf(stderr, "error: no AapL2cap device interface found (driver not installed / AirPods not paired?)\n");
            return false;
        }
        if (list.size() > 1) std::fprintf(stderr, "note: %zu interfaces, using the first\n", list.size());
        std::wprintf(L"opening %s\n", list[0].c_str());
        h_ = CreateFileW(list[0].c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            Err("CreateFile (L2CAP open channel failed; AirPods connected?)");
            h_ = nullptr;
            return false;
        }
        readEvt_  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::printf("channel open\n");
        return true;
    }

    void Close() {
        if (h_) { CancelIo(h_); CloseHandle(h_); h_ = nullptr; }
        if (readEvt_)  { CloseHandle(readEvt_);  readEvt_ = nullptr; }
        if (writeEvt_) { CloseHandle(writeEvt_); writeEvt_ = nullptr; }
    }

    bool Write(const Bytes& b, DWORD timeoutMs = 3000) {
        OVERLAPPED ov{};
        ov.hEvent = writeEvt_;
        ResetEvent(writeEvt_);
        DWORD n = 0;
        if (!WriteFile(h_, b.data(), static_cast<DWORD>(b.size()), &n, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) { Err("WriteFile"); return false; }
            if (WaitForSingleObject(writeEvt_, timeoutMs) != WAIT_OBJECT_0) {
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &n, TRUE);
                std::fprintf(stderr, "error: write timed out\n");
                return false;
            }
            if (!GetOverlappedResult(h_, &ov, &n, FALSE)) { Err("WriteFile (overlapped)"); return false; }
        }
        Log("TX (%lu)  %s", n, Hex(b.data(), b.size()).c_str());
        return true;
    }

    // Keeps exactly one ReadFile pending. Returns true if a packet arrived.
    // `packet` is filled; false on timeout (no error) or error (sets `failed`).
    bool Read(Bytes& packet, DWORD timeoutMs, bool& failed) {
        failed = false;
        if (!pending_) {
            ov_ = {};
            ov_.hEvent = readEvt_;
            ResetEvent(readEvt_);
            DWORD n = 0;
            if (ReadFile(h_, buf_, sizeof(buf_), &n, &ov_)) {
                packet.assign(buf_, buf_ + n);
                return true;
            }
            const DWORD gle = GetLastError();
            if (gle != ERROR_IO_PENDING) {
                Err("ReadFile", gle);
                failed = true;
                return false;
            }
            pending_ = true;
        }
        if (WaitForSingleObject(readEvt_, timeoutMs) != WAIT_OBJECT_0) return false;
        DWORD n = 0;
        pending_ = false;
        if (!GetOverlappedResult(h_, &ov_, &n, FALSE)) {
            const DWORD gle = GetLastError();
            if (gle == ERROR_DEVICE_NOT_CONNECTED || gle == ERROR_NOT_READY) {
                std::fprintf(stderr, "AirPods disconnected (0x%08lX)\n", gle);
            } else {
                Err("ReadFile (overlapped)", gle);
            }
            failed = true;
            return false;
        }
        packet.assign(buf_, buf_ + n);
        return true;
    }

    void CancelPending() {
        if (pending_) {
            CancelIoEx(h_, &ov_);
            DWORD n;
            GetOverlappedResult(h_, &ov_, &n, TRUE);
            pending_ = false;
        }
    }

private:
    HANDLE h_ = nullptr;
    HANDLE readEvt_ = nullptr;
    HANDLE writeEvt_ = nullptr;
    OVERLAPPED ov_{};
    bool pending_ = false;
    uint8_t buf_[AAPL2CAP_MAX_SDU * 2] = {};
};

// Pump incoming packets for `ms` milliseconds, decoding each. Returns false on I/O failure.
bool PumpFor(Channel& ch, DWORD ms, bool (*onPacket)(const Bytes&) = nullptr) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= end || g_stop) return true;
        const DWORD left = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count());
        Bytes pkt;
        bool failed = false;
        if (ch.Read(pkt, left > 50 ? 50 : left, failed)) {
            DecodePacket(pkt.data(), pkt.size());
            if (onPacket && onPacket(pkt)) return true;
        } else if (failed) {
            return false;
        }
    }
}

// Handshake + feature caps + notification filter (+ AllowOffOption).
bool Handshake(Channel& ch) {
    if (!ch.Write(kHandshake)) return false;
    bool acked = false;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < end && !acked) {
        Bytes pkt; bool failed = false;
        if (ch.Read(pkt, 100, failed)) {
            DecodePacket(pkt.data(), pkt.size());
            if (pkt.size() >= 4 && pkt[0] == 0x01 && pkt[1] == 0x00 && pkt[2] == 0x04 && pkt[3] == 0x00) acked = true;
        } else if (failed) {
            return false;
        }
    }
    if (!acked) std::fprintf(stderr, "warning: no handshake ack within 1.5 s, continuing anyway\n");

    if (!PumpFor(ch, 300)) return false;
    if (!ch.Write(kFeatureCaps)) return false;
    if (!PumpFor(ch, 300)) return false;
    if (!ch.Write(kNotifyFilter)) return false;
    if (!PumpFor(ch, 300)) return false;
    if (!ch.Write(kAllowOffOption)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

int CmdList() {
    auto list = EnumerateInterfaces();
    if (list.empty()) {
        std::printf("no AapL2cap device interfaces present\n");
        return 1;
    }
    for (auto& s : list) std::wprintf(L"%s\n", s.c_str());
    return 0;
}

int CmdMonitor() {
    Channel ch;
    if (!ch.Open()) return 1;
    if (!Handshake(ch)) return 1;
    Log("monitoring, Ctrl+C to stop");
    while (!g_stop) {
        Bytes pkt; bool failed = false;
        if (ch.Read(pkt, 250, failed)) {
            DecodePacket(pkt.data(), pkt.size());
        } else if (failed) {
            return 1;
        }
    }
    ch.CancelPending();
    return 0;
}

uint8_t g_expectId = 0;
uint8_t g_expectVal = 0;
bool MatchControlEcho(const Bytes& p) {
    return p.size() >= 8 && std::memcmp(p.data(), kHdr, 4) == 0 && p[4] == 0x09 && p[5] == 0x00 &&
           p[6] == g_expectId && p[7] == g_expectVal;
}

int CmdControl(uint8_t id, uint8_t value, const char* what) {
    Channel ch;
    if (!ch.Open()) return 1;
    if (!Handshake(ch)) return 1;
    g_expectId = id;
    g_expectVal = value;
    if (!ch.Write(ControlPacket(id, value))) return 1;
    Log("sent %s, waiting for echo (2 s)", what);
    if (!PumpFor(ch, 2000, MatchControlEcho)) return 1;
    ch.CancelPending();
    return 0;
}

std::optional<Bytes> ParseHex(int argc, char** argv, int from) {
    Bytes out;
    std::string all;
    for (int i = from; i < argc; ++i) { all += argv[i]; all += ' '; }
    std::string cur;
    auto flush = [&]() -> bool {
        if (cur.empty()) return true;
        if (cur.size() > 2) return false;
        char* endp = nullptr;
        const long v = std::strtol(cur.c_str(), &endp, 16);
        if (*endp || v < 0 || v > 255) return false;
        out.push_back(static_cast<uint8_t>(v));
        cur.clear();
        return true;
    };
    for (char c : all) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            cur += c;
            if (cur.size() == 2 && !flush()) return std::nullopt;
        } else if (c == ' ' || c == ',' || c == ':' || c == '-') {
            if (!flush()) return std::nullopt;
        } else if (c == 'x' || c == 'X') {
            if (cur == "0") cur.clear(); else return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    if (!flush()) return std::nullopt;
    return out;
}

int CmdRaw(int argc, char** argv) {
    auto bytes = ParseHex(argc, argv, 2);
    if (!bytes || bytes->empty()) {
        std::fprintf(stderr, "usage: aapctl raw <hex bytes>   e.g. aapctl raw 04 00 04 00 09 00 0D 02 00 00 00\n");
        return 2;
    }
    Channel ch;
    if (!ch.Open()) return 1;
    if (!Handshake(ch)) return 1;
    if (!ch.Write(*bytes)) return 1;
    Log("replies for 2 s:");
    if (!PumpFor(ch, 2000)) return 1;
    ch.CancelPending();
    return 0;
}

int CmdDecode(int argc, char** argv) {
    auto bytes = ParseHex(argc, argv, 2);
    if (!bytes || bytes->empty()) {
        std::fprintf(stderr, "usage: aapctl decode <hex bytes>\n");
        return 2;
    }
    DecodePacket(bytes->data(), bytes->size());
    return 0;
}

int Usage() {
    std::fprintf(stderr,
        "aapctl - AirPods AAP over L2CAP (AapL2cap driver)\n"
        "  aapctl list\n"
        "  aapctl monitor\n"
        "  aapctl mode off|anc|transparency|adaptive\n"
        "  aapctl ca on|off\n"
        "  aapctl raw <hex bytes>\n"
        "  aapctl decode <hex bytes>     (offline: decode a captured packet)\n");
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    if (argc < 2) return Usage();
    const std::string_view cmd = argv[1];

    if (cmd == "list") return CmdList();
    if (cmd == "monitor") return CmdMonitor();

    if (cmd == "mode") {
        if (argc < 3) return Usage();
        const std::string_view m = argv[2];
        uint8_t v = 0;
        if (m == "off") v = 0x01;
        else if (m == "anc") v = 0x02;
        else if (m == "transparency") v = 0x03;
        else if (m == "adaptive") v = 0x04;
        else return Usage();
        return CmdControl(CTL_LISTENING_MODE, v, "listening mode");
    }

    if (cmd == "ca") {
        if (argc < 3) return Usage();
        const std::string_view m = argv[2];
        uint8_t v = 0;
        if (m == "on") v = 0x01;
        else if (m == "off") v = 0x02;
        else return Usage();
        return CmdControl(CTL_CONV_AWARENESS, v, "conversational awareness");
    }

    if (cmd == "raw") return CmdRaw(argc, argv);
    if (cmd == "decode") return CmdDecode(argc, argv);

    return Usage();
}

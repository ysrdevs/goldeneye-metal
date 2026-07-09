// GoldenEye recomp — online multiplayer client. See ge_online.h.

#include "ge_online.h"
#include "ge_online_protocol.h"

#include <rex/platform.h>  // REX_PLATFORM_WIN32
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xsocket.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

REXCVAR_DEFINE_BOOL(ge_online_enable, false, "Online",
                    "Route GoldenEye System Link multiplayer through the matchmaker/relay "
                    "(online play). Off = normal behaviour.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(ge_online_server, "127.0.0.1", "Online",
                      "Matchmaker/relay server address: an IP or hostname (e.g. your public "
                      "server). 127.0.0.1 = localhost.");

REXCVAR_DEFINE_INT32(ge_online_port, rex::kernel::xam::geonline::kServerPort, "Online",
                     "Matchmaker/relay server UDP port (default 31000). Must match the port the "
                     "host started the server with. A 'host:port' typed into ge_online_server "
                     "overrides this.");

namespace rex::kernel::xam {

using namespace geonline;

OnlineClient& OnlineClient::Get() {
  static OnlineClient instance;
  return instance;
}

bool OnlineClient::Enabled() { return REXCVAR_GET(ge_online_enable); }

OnlineClient::~OnlineClient() { Shutdown(); }

uint32_t OnlineClient::virtual_ip() {
  std::lock_guard<std::mutex> lock(mutex_);
  return virtual_ip_;
}

static void FillHeader(MsgHeader& h, MsgType type) {
  h.magic = kMagic;
  h.version = kProtocolVersion;
  h.type = static_cast<uint8_t>(type);
}

void OnlineClient::EnsureStarted() {
#if REX_PLATFORM_WIN32
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
      REXLOG_ERROR("[ge-online] control socket() failed: {}", WSAGetLastError());
      return;
    }
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(kServerPort);
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Resolve the configured server. Accepts "host" or "host:port" (an IP or
    // hostname; e.g. a playit.gg tunnel "xxxx.ply.gg:2721"). Default 127.0.0.1
    // and port kServerPort (31000) for localhost play.
    {
      std::string host = REXCVAR_GET(ge_online_server);
      int cfg_port = REXCVAR_GET(ge_online_port);
      uint16_t port = (cfg_port > 0 && cfg_port < 65536) ? static_cast<uint16_t>(cfg_port)
                                                         : static_cast<uint16_t>(kServerPort);
      auto colon = host.rfind(':');
      if (colon != std::string::npos) {
        int p = std::atoi(host.c_str() + colon + 1);
        if (p > 0 && p < 65536) {
          port = static_cast<uint16_t>(p);
        }
        host = host.substr(0, colon);
      }
      srv.sin_port = htons(port);
      if (!host.empty()) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
          srv.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
          freeaddrinfo(res);
        } else {
          REXLOG_ERROR("[ge-online] could not resolve server '{}', using loopback", host);
        }
      }
      REXLOG_INFO("[ge-online] server {}:{}", host, port);
    }
    if (connect(s, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) == SOCKET_ERROR) {
      REXLOG_ERROR("[ge-online] control connect() failed: {}", WSAGetLastError());
      closesocket(s);
      return;
    }
    control_sock_ = static_cast<uintptr_t>(s);
    session_id_ = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
                  (GetTickCount64() & 0xFFFFFFFFull) ^ 0x474531234ull;
    started_ = true;
    run_ = true;
  }
  recv_thread_ = std::thread([this] { RecvLoop(); });
  heartbeat_thread_ = std::thread([this] { HeartbeatLoop(); });
  {
    // Request a virtual ip right away (don't wait for the first ~5s heartbeat) so
    // XNetGetTitleXnAddr can hand the title its real vip before it builds its
    // advertised session XNADDR.
    std::lock_guard<std::mutex> lock(mutex_);
    SendRegister(hosting_);
  }
  REXLOG_INFO("[ge-online] client started, session={:016X}", session_id_);
#endif
}

void OnlineClient::Shutdown() {
#if REX_PLATFORM_WIN32
  run_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_sock_ != ~uintptr_t(0)) {
      closesocket(static_cast<SOCKET>(control_sock_));  // unblocks RecvLoop
      control_sock_ = ~uintptr_t(0);
    }
    started_ = false;
  }
  if (recv_thread_.joinable()) recv_thread_.join();
  if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
#endif
}

bool OnlineClient::SendRaw(const void* data, int len) {
#if REX_PLATFORM_WIN32
  if (control_sock_ == ~uintptr_t(0)) {
    return false;
  }
  return send(static_cast<SOCKET>(control_sock_), reinterpret_cast<const char*>(data), len, 0) ==
         len;
#else
  return false;
#endif
}

void OnlineClient::SendRegister(bool is_host) {
  RegisterMsg msg{};
  FillHeader(msg.hdr, MsgType::kRegister);
  msg.session.session_id = session_id_;
  msg.session.virtual_ip = virtual_ip_;
  msg.session.host_port = game_port_;
  msg.session.gamemode = host_gamemode_;
  msg.session.map = host_map_;
  msg.session.max_players = host_max_;
  msg.session.cur_players = host_cur_;
  msg.session.is_host = is_host ? 1 : 0;
  std::strncpy(msg.session.name, host_name_[0] ? host_name_ : "GoldenEye",
               sizeof(msg.session.name) - 1);
  SendRaw(&msg, sizeof(msg));
}

void OnlineClient::OnBind(rex::system::XSocket* match_socket, uint16_t port) {
  EnsureStarted();
  std::lock_guard<std::mutex> lock(mutex_);
  sockets_by_port_[port] = match_socket;
  game_port_ = port;  // last bound; advertised as the host port
  SendRegister(hosting_);
  REXLOG_INFO("[ge-online] peer registered (port {})", port);
}

void OnlineClient::OnCloseSocket(rex::system::XSocket* socket) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = sockets_by_port_.begin(); it != sockets_by_port_.end();) {
    if (it->second == socket) {
      it = sockets_by_port_.erase(it);
    } else {
      ++it;
    }
  }
}

void OnlineClient::RegisterHost(uint32_t gamemode, uint32_t map, uint8_t max_players,
                                uint8_t cur_players, const char* name) {
  EnsureStarted();
  std::lock_guard<std::mutex> lock(mutex_);
  hosting_ = true;
  host_gamemode_ = gamemode;
  host_map_ = map;
  host_max_ = max_players;
  host_cur_ = cur_players;
  std::strncpy(host_name_, name ? name : "GoldenEye", sizeof(host_name_) - 1);
  SendRegister(true);
  REXLOG_INFO("[ge-online] REGISTER host port={} mode={} {}/{}", game_port_, gamemode, cur_players,
              max_players);
}

void OnlineClient::Unregister() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (control_sock_ == ~uintptr_t(0)) {
    return;
  }
  RegisterMsg msg{};
  FillHeader(msg.hdr, MsgType::kUnregister);
  msg.session.session_id = session_id_;
  SendRaw(&msg, sizeof(msg));
  hosting_ = false;
}

void OnlineClient::SendRelay(uint32_t dest_vip, uint16_t port, const uint8_t* data, uint16_t len) {
#if REX_PLATFORM_WIN32
  if (len > kMaxInner) {
    len = kMaxInner;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (control_sock_ == ~uintptr_t(0)) {
    return;
  }
  uint8_t buf[sizeof(RelayMsg) + kMaxInner];
  auto* rm = reinterpret_cast<RelayMsg*>(buf);
  FillHeader(rm->hdr, MsgType::kRelay);
  rm->addr = dest_vip;  // 0 = broadcast
  rm->src_port = port;  // unified deliver-to + source port
  rm->inner_len = len;
  std::memcpy(buf + sizeof(RelayMsg), data, len);
  SendRaw(buf, static_cast<int>(sizeof(RelayMsg) + len));
#endif
}

void OnlineClient::RecvLoop() {
#if REX_PLATFORM_WIN32
  uint8_t buf[sizeof(RelayMsg) + kMaxInner + 64];
  while (run_) {
    SOCKET s;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (control_sock_ == ~uintptr_t(0)) {
        break;
      }
      s = static_cast<SOCKET>(control_sock_);
    }
    int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    if (n < static_cast<int>(sizeof(MsgHeader))) {
      if (n <= 0 && !run_) {
        break;
      }
      continue;
    }
    auto* hdr = reinterpret_cast<MsgHeader*>(buf);
    if (hdr->magic != kMagic || hdr->version != kProtocolVersion) {
      continue;
    }
    switch (static_cast<MsgType>(hdr->type)) {
      case MsgType::kRegisterAck: {
        if (n >= static_cast<int>(sizeof(RegisterMsg))) {
          auto* m = reinterpret_cast<RegisterMsg*>(buf);
          std::lock_guard<std::mutex> lock(mutex_);
          if (virtual_ip_ == 0) {
            virtual_ip_ = m->session.virtual_ip;
            REXLOG_INFO("[ge-online] assigned virtual ip {}.{}.{}.{}", (virtual_ip_ >> 24) & 0xFF,
                        (virtual_ip_ >> 16) & 0xFF, (virtual_ip_ >> 8) & 0xFF, virtual_ip_ & 0xFF);
          }
        }
        break;
      }
      case MsgType::kRelayDeliver: {
        if (n >= static_cast<int>(sizeof(RelayMsg))) {
          auto* rm = reinterpret_cast<RelayMsg*>(buf);
          uint16_t inner_len = rm->inner_len;
          if (inner_len > kMaxInner ||
              n < static_cast<int>(sizeof(RelayMsg)) + inner_len) {
            break;
          }
          rex::system::XSocket* sock = nullptr;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sockets_by_port_.find(rm->src_port);
            if (it != sockets_by_port_.end()) {
              sock = it->second;
            }
          }
          if (sock) {
            // Queue the inner packet onto the matching socket so WSARecvFrom
            // returns it with the sender's virtual ip as the source address.
            sock->QueuePacket(rm->addr, rm->src_port, buf + sizeof(RelayMsg), inner_len);
          }
        }
        break;
      }
      default:
        break;
    }
  }
#endif
}

void OnlineClient::HeartbeatLoop() {
#if REX_PLATFORM_WIN32
  while (run_) {
    for (int i = 0; i < 50 && run_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));  // ~5s
    }
    if (!run_) {
      break;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_sock_ != ~uintptr_t(0)) {
      RegisterMsg msg{};
      FillHeader(msg.hdr, MsgType::kHeartbeat);
      msg.session.session_id = session_id_;
      msg.session.virtual_ip = virtual_ip_;
      msg.session.host_port = game_port_;
      msg.session.gamemode = host_gamemode_;
      msg.session.map = host_map_;
      msg.session.max_players = host_max_;
      msg.session.cur_players = host_cur_;
      msg.session.is_host = hosting_ ? 1 : 0;
      std::strncpy(msg.session.name, host_name_[0] ? host_name_ : "GoldenEye",
                   sizeof(msg.session.name) - 1);
      SendRaw(&msg, sizeof(msg));
    }
  }
#endif
}

}  // namespace rex::kernel::xam

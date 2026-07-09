// GoldenEye recomp — online multiplayer client.
//
// Bridges the game's emulated XNet (xam_net.cpp) to the localhost matchmaker +
// relay. The game does LAN-broadcast discovery on port 1001; we route those
// broadcasts (and the unicast replies) through the server so they cross
// processes/machines, and deliver them into the match socket's packet queue.
// Each instance gets a server-assigned VIRTUAL IP used as its peer identity.
//
// Gated entirely by the `ge_online_enable` cvar.

#pragma once

#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace rex::system {
class XSocket;
}

namespace rex::kernel::xam {

class OnlineClient {
 public:
  static OnlineClient& Get();
  static bool Enabled();

  void EnsureStarted();
  void Shutdown();

  // From bind(): register this instance as a peer (assigns a virtual ip) and
  // remember the match socket so relayed packets can be queued onto it.
  void OnBind(rex::system::XSocket* match_socket, uint16_t port);
  void OnCloseSocket(rex::system::XSocket* socket);

  // Host advertises a session (from XNetQosListen).
  void RegisterHost(uint32_t gamemode, uint32_t map, uint8_t max_players, uint8_t cur_players,
                    const char* name);
  void Unregister();

  // This instance's assigned virtual ip (host order); 0 until the server acks.
  uint32_t virtual_ip();

  // Relay a game packet sent from a socket bound to `port`. dest_vip == 0 =>
  // broadcast to all peers. Delivered to the peer's socket on the same port.
  void SendRelay(uint32_t dest_vip, uint16_t port, const uint8_t* data, uint16_t len);

 private:
  OnlineClient() = default;
  ~OnlineClient();

  void RecvLoop();
  void HeartbeatLoop();
  void SendRegister(bool is_host);
  bool SendRaw(const void* data, int len);

  std::mutex mutex_;
  uintptr_t control_sock_ = ~uintptr_t(0);
  bool started_ = false;
  uint64_t session_id_ = 0;
  uint32_t virtual_ip_ = 0;
  uint16_t game_port_ = 0;

  bool hosting_ = false;
  uint32_t host_gamemode_ = 0;
  uint32_t host_map_ = 0;
  uint8_t host_max_ = 4;
  uint8_t host_cur_ = 1;
  char host_name_[32] = {};

  // All match sockets, keyed by bound port (1001 = discovery UDP, 1000 = game
  // VDP). Relayed packets are queued onto the socket matching the dest port.
  std::unordered_map<uint16_t, rex::system::XSocket*> sockets_by_port_;

  std::thread recv_thread_;
  std::thread heartbeat_thread_;
  volatile bool run_ = false;
};

}  // namespace rex::kernel::xam

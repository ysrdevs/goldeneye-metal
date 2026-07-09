// GoldenEye recomp — online multiplayer wire protocol (CLIENT copy).
//
// MUST stay byte-identical to the server-side ge_net_protocol.h copy. Plain
// UDP, little-endian, packed structs.
//
// Two jobs: (1) session DIRECTORY (hosts register, clients query), (2) packet
// RELAY so emulated LAN broadcast/unicast crosses processes/machines. Each
// instance gets a server-assigned VIRTUAL IP used as its peer identity.

#pragma once
#include <cstdint>

namespace rex::kernel::xam::geonline {

constexpr uint32_t kMagic = 0x32304547;  // "GE02" (bumped: protocol w/ relay)
constexpr uint16_t kServerPort = 31000;
constexpr uint32_t kProtocolVersion = 2;
constexpr uint32_t kSessionTimeoutSec = 15;

enum class MsgType : uint8_t {
  kRegister = 1,  // peer -> server: announce/refresh (host session OR just a peer)
  kUnregister = 2,
  kHeartbeat = 3,
  kQuery = 4,         // client -> server: list advertised host sessions
  kList = 5,          // server -> client
  kRegisterAck = 6,   // server -> peer: assigns virtual_ip (in session.virtual_ip)
  kRelay = 7,         // peer -> server: relay an inner game packet
  kRelayDeliver = 8,  // server -> peer: a relayed inner game packet
};

#pragma pack(push, 1)

struct MsgHeader {
  uint32_t magic;
  uint32_t version;
  uint8_t type;
};

struct SessionInfo {
  uint64_t session_id;
  uint32_t virtual_ip;  // server-assigned peer identity (host order); 0 until acked
  uint32_t host_ip;     // real source IP (network order); server fills it
  uint16_t host_port;   // host's game UDP port (nominal)
  uint32_t gamemode;
  uint32_t map;
  uint8_t max_players;
  uint8_t cur_players;
  uint8_t is_host;  // 1 = advertised host session, 0 = plain peer
  char name[32];
};

struct RegisterMsg {
  MsgHeader hdr;
  SessionInfo session;
};

struct QueryMsg {
  MsgHeader hdr;
};

struct ListMsg {
  MsgHeader hdr;
  uint16_t count;
  // SessionInfo entries[count] follow.
};

// Relay: kRelay (peer->server) sets addr = destination virtual IP (0 = broadcast
// to all other peers). kRelayDeliver (server->peer) sets addr = SOURCE virtual
// IP. inner_len bytes of opaque game data follow the header.
struct RelayMsg {
  MsgHeader hdr;
  uint32_t addr;      // dest vip (kRelay) or src vip (kRelayDeliver), host order
  uint16_t src_port;  // sender's nominal game port
  uint16_t inner_len;
  // uint8_t inner[inner_len] follows.
};
constexpr uint16_t kMaxInner = 1408;

#pragma pack(pop)

}  // namespace rex::kernel::xam::geonline

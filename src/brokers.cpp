
#include <chrono>
#include <iostream>

#include <caf/all.hpp>
#include <caf/io/all.hpp>

#include "caf/io/broker.hpp"

using namespace std;
using namespace caf;
using namespace caf::io;

namespace {

using ping_atom = caf::atom_constant<atom("ping")>;
using reset_atom = caf::atom_constant<atom("reset")>;
using start_atom = caf::atom_constant<atom("start")>;
using shutdown_atom = caf::atom_constant<atom("shutdown")>;

//  8 bytes sequence number + annotations
constexpr size_t message_overhead = 10;

constexpr auto interval = std::chrono::seconds(1);
constexpr auto one = chrono::duration_cast<chrono::microseconds>(interval);

} // namespace anonymous

// -----------------------------------------------------------------------------
//  ACTOR SYSTEM CONFIG
// -----------------------------------------------------------------------------

class config : public actor_system_config {
public:
  bool is_server = false;
  string host = "127.0.0.1";
  uint16_t port = 1337;
  uint32_t rate = 1000;
  uint32_t bundle = 10;
  uint32_t payload = 1024;
  config() {
    load<io::middleman>();
    set("middleman.enable-udp", true);
    set("middleman.enable-tcp", true);
    add_message_type<std::vector<char>>("std::vector<char>");
    opt_group{custom_options_, "global"}
      .add(port, "port,P", "set port")
      .add(bundle, "bundle,b", "messages sent without sleep")
      .add(host, "host,H", "set host (ignored in server mode)")
      .add(rate, "rate,r", "set number of messages per second")
      .add(payload, "payload,p", "set payload of each message in bytes "
                                 "(default: 1024 bytes)")
      .add(is_server, "server,s", "start a server");
  }
};


// -----------------------------------------------------------------------------
//  SERVER BROKER
// -----------------------------------------------------------------------------

struct statistics {
  uint32_t packets_per_interval;
  uint64_t bytes;
  uint64_t received;
  uint32_t lost;
  uint32_t next;
  // deserialization stuff
  vector<char> payload;
};

behavior server(stateful_broker<statistics>* self, uint16_t port) {
  // open local endpoint
  auto epair = self->add_udp_datagram_servant(port, nullptr, true);
  if (!epair) {
    cerr << "could not open port " << port << ": "
         << self->system().render(epair.error()) << endl;
    self->quit();
  }
  aout(self) << "broker open on port " << epair->second << endl;
  // initialize state
  auto& s = self->state;
  s.packets_per_interval = 0;
  s.bytes = 0;
  s.lost = 0;
  s.bytes = 0;
  s.next = 0;
  self->delayed_send(self, interval, reset_atom::value);
  return {
    [=](const new_datagram_msg& msg) {
      // regular data packet
      auto& s = self->state;
      // count messages that arrived
      ++s.received;
      // count bytes that arrived
      s.bytes += msg.buf.size();
      binary_deserializer bd{self->context(), msg.buf};
      uint64_t seq;
      bd(s.payload, seq);
      if (seq == s.next) {
        // expected message
        ++s.next;
      } else if (seq > s.next) {
        // skipped messages
        s.lost += (seq - s.next);
        s.next = seq + 1;
      } else {
        // previously lost message
        --s.lost;
      }
    },
    [=](reset_atom) {
      self->delayed_send(self, interval, reset_atom::value);
      auto& s = self->state;
      aout(self) << "Received " << s.received << " received, lost "
                 << (s.lost * 1.0 / s.received)
                 << " --> " << (s.bytes / (1024.0 * 1024.0) )
                 << " MBs/s" << std::endl;
      s.received = 0;
      s.bytes = 0;
      s.lost = 0;
    },
    [=](shutdown_atom) {
      self->quit();
    }
  };
}

// -----------------------------------------------------------------------------
//  CLIENT BROKER
// -----------------------------------------------------------------------------

struct c_state {
  uint32_t count;
  uint64_t seq;
  vector<char> payload;
  uint32_t packets;
  uint32_t bundle;
  caf::duration timeout;
  // vector<char> buf;
  datagram_handle servant;
};


behavior client(stateful_broker<c_state>* self, const string& h, uint16_t p,
                vector<char> payload, uint32_t packets, uint32_t bundle) {
  auto& s = self->state;
  aout(self) << "remote endpoint at " << h << ":" << p << endl;
  // create endpoint to contact server
  auto ehdl = self->add_udp_datagram_servant(h, p);
  if (!ehdl) {
    cerr << "failed to create local endpoint: "
         << self->system().render(ehdl.error()) << endl;
    self->quit();
  }
  s.servant = move(*ehdl);
  // initialize state
  s.count = 0;
  s.seq = 0;
  s.payload = std::move(payload);
  s.packets = packets;
  s.bundle = bundle;
  //s.timeout = caf::duration{(one * bundle / packets) / 2};
  s.timeout = caf::duration{one / packets / 3};
  aout(self) << "targeting " << self->state.packets << " packets/s" << endl;
  self->delayed_send(self, self->state.timeout, ping_atom::value);
  self->delayed_send(self, interval, reset_atom::value);
  self->ack_writes(s.servant, true);
  return {
    [=](ping_atom) {
      auto& s = self->state;
      /*
      if (s.seq % s.bundle == 0)
        self->delayed_send(self, s.timeout, ping_atom::value);
      else
      */
      if (s.count < s.packets) {
        // serialize into new message buffer
        binary_serializer bs{self->context(), self->wr_buf(s.servant)};
        bs(s.payload, s.seq);
        self->flush(s.servant);
        ++s.count;
        ++s.seq;
      }
    },
    [=](const datagram_sent_msg&) {
      auto& s = self->state;
      if (s.count < s.packets) {
        // serialize into new message buffer
        binary_serializer bs{self->context(), self->wr_buf(s.servant)};
        bs(s.payload, s.seq);
        self->flush(s.servant);
        ++s.count;
        ++s.seq;
      }
    },
    [=](reset_atom) {
      self->delayed_send(self, interval, reset_atom::value);
      aout(self) << "sent " << self->state.count << " packets/s" << endl;
      self->state.count = 0;
      self->send(self, ping_atom::value);
    },
    [=](shutdown_atom) {
      self->quit();
    }
  };
}

// -----------------------------------------------------------------------------
//  MAIN
// -----------------------------------------------------------------------------

void caf_main(actor_system& system, const config& cfg) {
  if (!system.config().middleman_enable_udp) {
    cerr << "please enable UDP in CAF" << endl;
    return;
  }
  if (cfg.is_server) { // server
    system.middleman().spawn_broker(server, cfg.port);
  } else { // client
    if (cfg.payload < message_overhead) {
      cerr << "Payload needs to be at least " << message_overhead
           << " bytes" << endl;
      return;
    }
    vector<char> payload(cfg.payload - message_overhead, 'a');
    system.middleman().spawn_broker(client, cfg.host, cfg.port,
                                    move(payload), cfg.rate, cfg.bundle);
  }
}

CAF_MAIN();

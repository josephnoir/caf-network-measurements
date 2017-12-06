
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
constexpr uint32_t message_overhead = 10;

constexpr auto interval = std::chrono::seconds(1);

} // namespace anonymous

// -----------------------------------------------------------------------------
//  ACTOR SYSTEM CONFIG
// -----------------------------------------------------------------------------

class config : public actor_system_config {
public:
  bool is_server = false;
  string host = "127.0.0.1";
  uint16_t port = 1338;
  uint32_t rate = 1000;
  uint32_t bundle = 1;
  uint32_t payload = 1024;
  config() {
    load<io::middleman>();
    set("middleman.enable-tcp", true);
    set("middleman.enable-udp", false);
    add_message_type<std::vector<char>>("std::vector<char>");
    opt_group{custom_options_, "global"}
      .add(port, "port,P", "set port")
      .add(bundle, "bundle,b", "broker waits for b buffers before sending again")
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

struct s_state {
  uint32_t packets_per_interval;
  uint64_t bytes;
  uint64_t received;
  uint32_t lost;
  uint32_t next;
  // deserialization stuff
  vector<char> payload;
  bool reporting;
};

behavior server(stateful_broker<s_state>* self) {
  aout(self) << "Server running, waiting for clients!" << endl;
  // initialize state
  auto& s = self->state;
  s.packets_per_interval = 0;
  s.bytes = 0;
  s.lost = 0;
  s.bytes = 0;
  s.next = 0;
  s.reporting = false;
  return {
    [=](new_connection_msg& msg) {
      if (self->state.reporting == true) {
        aout(self) << "No support for multiple enpoints." << endl;
      } else {
        aout(self) << "New client, let's start reporting." << endl;
        self->delayed_send(self, interval, reset_atom::value);
        self->configure_read(msg.handle, receive_policy::exactly(1024));
        binary_serializer bs{self->context(), self->wr_buf(msg.handle)};
        bs(start_atom::value);
        self->flush(msg.handle);
        self->state.reporting = true;
      }
    },
    [=](connection_closed_msg&) {
      aout(self) << "Client lost." << endl;
      self->state.reporting = false;
    },
    [=](const new_data_msg& msg) {
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
      auto& s = self->state;
      if (s.reporting) {
        self->delayed_send(self, interval, reset_atom::value);
        aout(self) << "Received " << s.received << " received, lost "
                   << (s.lost * 1.0 / s.received)
                   << " --> " << (s.bytes * 8 / (1024.0 * 1024.0) )
                   << " Mbits/s." << std::endl;
      } else {
        aout(self) << "Waiting for new client ... " << endl;
      }
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
  uint32_t tmp; // track messages in bundle
  uint32_t bundle;
  connection_handle servant;
};


behavior client(stateful_broker<c_state>* self,
                const string& host, uint16_t port,
                uint32_t payload, uint32_t packets, uint32_t bundle) {
  auto es = self->add_tcp_scribe(host, port);
  if (!es) {
    cerr << "Failed to create client for " << host << ":" << port
         << ": " << self->system().render(es.error()) << "." << endl;
    self->quit();
  }
  auto hdl = move(*es);
  self->configure_read(hdl, receive_policy::at_most(1024));
  auto& s = self->state;
  // initialize state
  s.count = 0;
  s.seq = 0;
  s.payload = vector<char>(payload, 'a');
  s.packets = packets;
  s.bundle = bundle;
  return {
    [=](new_data_msg& msg) {
      auto& s = self->state;
      aout(self) << "Response from server, starting to send" << endl
                 << "targeting " << self->state.packets << " packets/s." << endl;
      s.servant = msg.handle;
      self->delayed_send(self, interval, reset_atom::value);
      for (uint32_t i = 0; i < (2 * s.bundle); ++i)
        self->send(self, ping_atom::value);
      self->ack_writes(s.servant, true);
    },
    [=](ping_atom) {
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
    [=](data_transferred_msg& msg) {
      auto& s = self->state;
      ++s.tmp;
      if (s.tmp >= s.bundle) {
        while (s.count < s.packets && s.tmp > 0) {
          binary_serializer bs{self->context(), self->wr_buf(msg.handle)};
          bs(s.payload, s.seq);
          self->flush(s.servant);
          ++s.count;
          ++s.seq;
          --s.tmp;
        }
      }
    },
    [=](reset_atom) {
      self->delayed_send(self, interval, reset_atom::value);
      aout(self) << "Sent " << self->state.count << " packets/s." << endl;
      self->state.count = 0;
      for (uint32_t i = 0; i < (2 * s.bundle); ++i)
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
  if (!system.config().middleman_enable_tcp) {
    cerr << "Please enable TCP in CAF." << endl;
    return;
  }
  if (cfg.is_server) { // server
    auto es = system.middleman().spawn_server(server, cfg.port);
    if (!es) {
      cerr << "Failed to spawn server: " << system.render(es.error())
           << "." << endl;
    }
  } else { // client
    if (cfg.payload < message_overhead) {
      cerr << "Payload needs to be at least " << message_overhead
           << " bytes." << endl;
      return;
    }
    uint32_t payload = cfg.payload - message_overhead;
    system.middleman().spawn_broker(client, cfg.host, cfg.port,
                                    payload, cfg.rate, cfg.bundle);
  }
}

CAF_MAIN();

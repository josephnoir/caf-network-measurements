
#include <chrono>
#include <iostream>

#include <caf/all.hpp>
#include <caf/io/all.hpp>

using namespace caf;
using namespace std;

namespace {

using ping_atom = caf::atom_constant<atom("ping")>;
using reset_atom = caf::atom_constant<atom("reset")>;
using start_atom = caf::atom_constant<atom("start")>;
using shutdown_atom = caf::atom_constant<atom("shutdown")>;

// 82 bytes BASP header
//  2 bytes annotation
//  4 bytes sequence number
//  8 bytes timestamp
constexpr size_t message_overhead = 82 - 2 - 4 - 8;

constexpr auto interval = std::chrono::seconds(1);
constexpr auto one = chrono::duration_cast<chrono::microseconds>(interval);

} // namespace anonymous

// -----------------------------------------------------------------------------
//  ACTOR SYSTEM CONFIG
// -----------------------------------------------------------------------------

class config : public actor_system_config {
public:
  bool server = false;
  uint16_t port = 1337;
  std::string host = "127.0.0.1";
  uint32_t rate = 1000;
  uint32_t payload = 1024 - message_overhead;
  uint32_t bundle = 10;
  bool debug = false;
  bool udp = false;
  config() {
    load<io::middleman>();
    set("middleman.enable-udp", true);
    set("middleman.enable-tcp", true);
    add_message_type<std::vector<char>>("std::vector<char>");
    opt_group{custom_options_, "global"}
      .add(port, "port,P", "set port")
      .add(udp, "udp,u", "use udp (default: tcp)")
      .add(bundle, "bundle,b", "messages sent without sleep")
      .add(host, "host,H", "set host (ignored in server mode)")
      .add(rate, "rate,r", "set number of messages per second")
      .add(payload, "payload,p", "set payload of each message in bytes (default"
                                 ": 1024 bytes, overhead is 82+2+8 bytes)")
      .add(server, "server,s", "start a server")
      .add(debug, "debug,d", "print message size only");
  }
};


// -----------------------------------------------------------------------------
//  SERVER
// -----------------------------------------------------------------------------

struct statistics {
  uint32_t packets_per_interval;
  uint64_t bytes;
  uint64_t received;
  uint32_t lost;
  uint32_t next;
};

behavior measureing_server(stateful_actor<statistics>* self);

// server while idle
behavior idle_server(stateful_actor<statistics>* self) {
  //self->set_default_handler(skip);
  return {
    [=](start_atom, uint32_t num_packets) {
      // new client with data ...
      auto& s = self->state;
      s.packets_per_interval = num_packets;
      s.bytes = 0;
      s.lost = 0;
      s.bytes = 0;
      s.next = 0;
      self->become(measureing_server(self));
      return start_atom::value;
    },
    [=](shutdown_atom) {
      self->quit();
    },
  };
}

// server behavior while measuring data
behavior measureing_server(stateful_actor<statistics>* self) {
  self->delayed_send(self, interval, reset_atom::value);
  return {
    [=](const vector<char>& payload, uint32_t seq, caf::timestamp&) {
      // regular data packet
      auto& s = self->state;
      // count messages that arrived
      ++s.received;
      // count bytes that arrived
      s.bytes += payload.size() + message_overhead;
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
    },
    after(chrono::seconds(5)) >> [=] {
      self->become(idle_server(self));
    }
  };
}

// -----------------------------------------------------------------------------
//  CLIENT
// -----------------------------------------------------------------------------

struct c_state {
  uint32_t count;
  uint32_t seq;
  actor srv;
  vector<char> payload;
  uint32_t packets;
  uint32_t bundle;
  caf::duration timeout;
};

behavior sending_client(stateful_actor<c_state>* self);

behavior handshake_client(stateful_actor<c_state>* self, actor srv,
                          vector<char> payload, uint32_t packets,
                          uint32_t bundle, caf::duration timeout) {
  auto& s = self->state;
  s.count = 0;
  s.seq = 0;
  s.srv = srv;
  s.payload = std::move(payload);
  s.packets = packets;
  s.bundle = bundle;
  s.timeout = timeout;
  self->send(srv, start_atom::value, packets);
  return {
    [=](start_atom) {
      self->become(sending_client(self));
    }
  };
}

behavior sending_client(stateful_actor<c_state>* self) {
  aout(self) << "Sending " << self->state.packets << " packets/s" << endl;
  self->delayed_send(self, self->state.timeout, ping_atom::value);
  self->delayed_send(self, interval, reset_atom::value);
  return {
    [=](ping_atom) {
      auto& s = self->state;
      /*
      if (s.seq % s.bundle == 0)
        self->delayed_send(self, s.timeout, ping_atom::value);
      else
      */
      self->send(self, ping_atom::value);
      if (self->state.count < s.packets) {
        self->send(s.srv, s.payload, s.seq, caf::make_timestamp());
        ++s.count;
        ++s.seq;
      }
    },
    [=](reset_atom) {
      self->delayed_send(self, interval, reset_atom::value);
      self->state.count = 0;
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
  vector<char> payload(cfg.payload, 'a');
  auto ts = caf::make_timestamp();
  if (cfg.debug) {
    vector<char> buf;
    binary_serializer sink{system, buf};
    auto e = sink(payload, 1u, ts);
    cout << "Message will be " << (buf.size() + caf::io::basp::header_size)
         << " bytes" << endl;
  } else {
    if (cfg.server) { // server
      auto s = system.spawn<detached>(idle_server);
      auto ep = cfg.udp ? system.middleman().publish_udp(s, cfg.port)
                        : system.middleman().publish(s, cfg.port);
      if (ep) {
        cout << "started server on port " << *ep << endl;
        system.await_all_actors_done();
      } else {
        cerr << "failed to start server " << system.render(ep.error())
             << ". (" << ep.error().code() << ")" << std::endl;
        anon_send(s, shutdown_atom::value);
        return;
      }
    } else { // client
      auto es = cfg.udp ? system.middleman().remote_actor_udp(cfg.host, cfg.port)
                        : system.middleman().remote_actor(cfg.host, cfg.port);
      if (!es) {
        std::cerr << "Cannot reach server on '" << cfg.host << ":" << cfg.port
                  << "':" << system.render(es.error()) << std::endl;
        return;
      }
      system.spawn<detached>(handshake_client, std::move(*es), std::move(payload),
                             cfg.rate, cfg.bundle,
                             caf::duration{(one * cfg.bundle / cfg.rate) / 2});
      /*
      auto sleep_time = one / cfg.rate;
      scoped_actor self{system};
      std::chrono::high_resolution_clock::time_point last;
      for (;;) {
        auto current = std::chrono::high_resolution_clock::now();
        cout << chrono::duration_cast<chrono::microseconds>(current - last).count()
             << endl;
        last = current;
        std::this_thread::sleep_for(sleep_time);
      }
      */
    }
  }
}

CAF_MAIN();

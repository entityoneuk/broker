#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <caf/atom.hpp>
#include <caf/behavior.hpp>
#include <caf/config_option_adder.hpp>
#include <caf/deep_to_string.hpp>
#include <caf/downstream.hpp>
#include <caf/event_based_actor.hpp>
#include <caf/exit_reason.hpp>
#include <caf/send.hpp>
#include <caf/term.hpp>
#include <caf/uri.hpp>

#include "broker/atoms.hh"
#include "broker/configuration.hh"
#include "broker/convert.hh"
#include "broker/data.hh"
#include "broker/endpoint.hh"
#include "broker/publisher.hh"
#include "broker/status.hh"
#include "broker/subscriber.hh"
#include "broker/topic.hh"

using std::string;

using broker::data;
using broker::topic;

// -- I/O utility --------------------------------------------------------------

namespace detail {

namespace {

std::mutex ostream_mtx;

} // namespace <anonymous>

int print_impl(std::ostream& ostr, const char* x) {
  ostr << x;
  return 0;
}

int print_impl(std::ostream& ostr, const string& x) {
  ostr << x;
  return 0;
}

int print_impl(std::ostream& ostr, const caf::term& x) {
  ostr << x;
  return 0;
}

template <class T>
int print_impl(std::ostream& ostr, const T& x) {
  return print_impl(ostr, caf::deep_to_string(x));
}

template <class... Ts>
void println(std::ostream& ostr, Ts&&... xs) {
  std::unique_lock<std::mutex> guard{ostream_mtx};
  std::initializer_list<int>{print_impl(ostr, std::forward<Ts>(xs))...};
  ostr << caf::term::reset_endl;
}

} // namespace detail

namespace out {

template <class... Ts>
void println(Ts&&... xs) {
  detail::println(std::cout, std::forward<Ts>(xs)...);
}

} // namespace out

namespace err {

template <class... Ts>
void println(Ts&&... xs) {
  detail::println(std::cerr, caf::term::red, std::forward<Ts>(xs)...);
}

} // namespace err

namespace verbose {

namespace {

std::atomic<bool> enabled;

} // namespace <anonymous>

template <class... Ts>
void println(Ts&&... xs) {
  if (enabled)
    detail::println(std::cout, caf::term::blue, std::forward<Ts>(xs)...);
}

} // namespace verbose

// -- CAF setup ----------------------------------------------------------------

using namespace caf;

namespace {

// -- atom constants -----------------------------------------------------------

using ping_atom = atom_constant<atom("ping")>;

using pong_atom = atom_constant<atom("pong")>;

using relay_atom = atom_constant<atom("relay")>;

using blocking_atom = atom_constant<atom("blocking")>;

using stream_atom = atom_constant<atom("stream")>;

// -- type aliases -------------------------------------------------------------

using uri_list = std::vector<uri>;

using mode_fun = void (*)(broker::endpoint&, broker::topic);

// -- constants ----------------------------------------------------------------

constexpr size_t max_cap = std::numeric_limits<size_t>::max();

// -- program options ----------------------------------------------------------

class config : public broker::configuration {
public:
  config() {
    opt_group{custom_options_, "global"}
      .add<bool>("verbose,v", "print status and debug output")
      .add<string>("topic,t", "topic for sending/receiving messages")
      .add<atom_value>("mode,m", "set mode: 'relay', 'ping', or 'pong'")
      .add<atom_value>("impl,i", "mode: 'ping', 'pong', or 'relay'")
      .add<size_t>(
        "num-pings,n",
        "number of pings (default: 10), ignored in pong and relay mode)")
      .add<uri_list>("peers,p",
                     "list of peers we connect to on startup in "
                     "<tcp://$host:$port> notation")
      .add<uint16_t>("local-port,l",
                     "local port for publishing this endpoint at");
  }
};

// -- convenience get_or and get_if overloads for enpoint ----------------------

template <class T>
auto get_or(broker::endpoint& d, string_view key, const T& default_value)
-> decltype(caf::get_or(d.system().config(), key, default_value)) {
  return caf::get_or(d.system().config(), key, default_value);
}

template <class T>
auto get_if(broker::endpoint* d, string_view key)
-> decltype(caf::get_if<T>(&(d->system().config()), key)) {
  return caf::get_if<T>(&(d->system().config()), key);
}

// -- message creation and introspection ---------------------------------------

bool is_ping_msg(const broker::data& x) {
  auto str = caf::get_if<string>(&x);
  return str && *str == "ping";
}

bool is_pong_msg(const broker::data& x) {
  auto str = caf::get_if<string>(&x);
  return str && *str == "pong";
}

bool is_stop_msg(const broker::data& x) {
  auto str = caf::get_if<string>(&x);
  return str && *str == "stop";
}

broker::data make_ping_msg() {
  return "ping";
}

broker::data make_pong_msg() {
  return "pong";
}

broker::data make_stop_msg() {
  return "stop";
}

// -- mode implementations -----------------------------------------------------

void relay_mode(broker::endpoint& ep, broker::topic topic) {
  verbose::println("relay messages");
  auto in = ep.make_subscriber({topic});
  for (;;) {
    auto x = in.get();
    if (is_ping_msg(x.second)) {
      verbose::println(ep.system().clock().now(), " received a ping");
    } else if (is_pong_msg(x.second)) {
      verbose::println(ep.system().clock().now(), " received a pong");
    } else if (is_stop_msg(x.second)) {
      verbose::println(ep.system().clock().now(), " received stop");
      return;
    }
  }
}

void ping_mode(broker::endpoint& ep, broker::topic topic) {
  verbose::println("send pings to topic ", topic);
  std::vector<timespan> xs;
  auto n = get_or(ep, "num-pings", size_t{10});
  if (n == 0) {
    err::println("send no pings: n = 0");
    return;
  }
  auto in = ep.make_subscriber({topic});
  for (size_t i = 0; i < n; ++i) {
    bool done = false;
    auto t0 = std::chrono::system_clock::now();
    ep.publish(topic, make_ping_msg());
    do {
      auto x = in.get();
      done = is_pong_msg(x.second);
    } while (!done);
    auto t1 = std::chrono::system_clock::now();
    xs.emplace_back(t1 - t0);
    out::println(xs.back());
  }
  auto avg = std::accumulate(xs.begin(), xs.end(), timespan{}) / xs.size();
  out::println("AVG: ", avg);
}

void pong_mode(broker::endpoint& ep, broker::topic topic) {
  verbose::println("receive pings from topic ", topic);
  auto in = ep.make_subscriber({topic});
  for (;;) {
    auto x = in.get();
    if (is_ping_msg(x.second)) {
      verbose::println(ep.system().clock().now(), " received a ping");
      ep.publish(topic, make_pong_msg());
    } else if (is_stop_msg(x.second)) {
      verbose::println(ep.system().clock().now(), " received stop");
      return;
    }
  }
}

} // namespace <anonymous>

// -- main function ------------------------------------------------------------

int main(int argc, char** argv) {
  // Parse CLI parameters using our config.
  config cfg;
  cfg.parse(argc, argv);
  broker::endpoint ep{std::move(cfg)};
  // Get topic (mandatory).
  auto topic = get_if<string>(&ep, "topic");
  if (!topic) {
    err::println("no topic specified");
    return EXIT_FAILURE;
  }
  // Enable verbose output if demanded by user.
  actor verbose_logger;
  if (get_or(ep, "verbose", false)) {
    verbose::enabled = true;
    // Launch background worker when running in verbose mode.
    auto& sys = ep.system();
    auto& groups = sys.groups();
    auto g1 = groups.get_local("broker/errors");
    auto g2 = groups.get_local("broker/statuses");
    sys.spawn_in_groups({g1, g2}, [](event_based_actor* self) -> behavior {
      auto ts = [=] { return self->system().clock().now(); };
      return {
        [=](broker::atom::local, broker::error& x) {
          verbose::println(ts(), " ", x);
        },
        [=](broker::atom::local, broker::status& x) {
          verbose::println(ts(), " ", x);
        }
      };
    });
  }
  // Publish endpoint at demanded port.
  if (auto local_port = get_if<uint16_t>(&ep, "local-port")) {
    verbose::println("listen for peers on port ", *local_port);
    ep.listen({}, *local_port);
  }
  // Select function f based on the mode.
  auto mode = get_if<atom_value>(&ep, "mode");
  if (!mode) {
    err::println("no mode specified");
    return EXIT_FAILURE;
  }
  mode_fun f = nullptr;
  switch (static_cast<uint64_t>(*mode)) {
    case relay_atom::uint_value():
      f = relay_mode;
      break;
    case ping_atom::uint_value():
      f = ping_mode;
      break;
    case pong_atom::uint_value():
      f = pong_mode;
      break;
    default:
      err::println("invalid mode: ", mode);
      return EXIT_FAILURE;
  }
  // Connect to peers.
  auto peers = get_or(ep, "peers", uri_list{});
  for (auto& peer : peers) {
    auto& auth = peer.authority();
    if (peer.scheme() != "tcp") {
      err::println("unrecognized scheme (expected tcp) in: <", peer, '>');
    } else if (auth.empty()) {
      err::println("no authority component in: <", peer, '>');
    } else {
      auto host = to_string(auth.host);
      auto port = auth.port;
      verbose::println("connect to ", host, " on port ", port, " ...");
      ep.peer(host, port);
    }
  }
  f(ep, *topic);
  // Disconnect from peers.
  for (auto& peer : peers) {
    auto& auth = peer.authority();
    if (peer.scheme() == "tcp" && !auth.empty()) {
      auto host = to_string(auth.host);
      auto port = auth.port;
      verbose::println("diconnect from ", host, " on port ", port, " ...");
      ep.unpeer_nosync(host, port);
    }
  }
  // Stop utility actors.
  anon_send_exit(verbose_logger, exit_reason::user_shutdown);
}


#ifndef BROKER_DETAIL_CLONE_ACTOR_HH
#define BROKER_DETAIL_CLONE_ACTOR_HH

#include <unordered_map>
#include <vector>

#include <caf/actor.hpp>
#include <caf/stateful_actor.hpp>

#include "broker/data.hh"
#include "broker/internal_command.hh"
#include "broker/topic.hh"

namespace broker {
namespace detail {

class clone_state {
public:
  /// Allows us to apply this state as a visitor to internal commands.
  using result_type = void;

  /// Creates an uninitialized object.
  clone_state();

  /// Initializes the object.
  void init(caf::event_based_actor* ptr, std::string&& nm,
            caf::actor&& parent);

  /// Sends `x` to the master.
  void forward(internal_command&& x);

  /// Wraps `x` into a `data` object and forwards it to the master.
  template <class T>
  void forward_from(T& x) {
    forward(make_internal_command<T>(std::move(x)));
  }

  void command(internal_command& cmd);

  void operator()(none);

  void operator()(put_command&);

  void operator()(put_unique_command&);

  void operator()(erase_command&);

  void operator()(add_command&);

  void operator()(subtract_command&);

  void operator()(snapshot_command&);

  void operator()(set_command&);

  void operator()(clear_command&);

  data keys() const;

  caf::event_based_actor* self;

  std::string name;

  topic master_topic;

  caf::actor core;

  caf::actor master;

  std::unordered_map<data, data> store;

  bool is_stale;

  double stale_time;

  double unmutable_time;

  std::vector<internal_command> mutation_buffer;
};

caf::behavior clone_actor(caf::stateful_actor<clone_state>* self,
                          caf::actor core, std::string name,
                          double resync_interval, double stale_interval,
                          double mutation_buffer_interval);

} // namespace detail
} // namespace broker

#endif // BROKER_DETAIL_CLONE_ACTOR_HH
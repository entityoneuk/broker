#include "broker/detail/core_policy.hh"

#include <algorithm>

#include <caf/detail/stream_distribution_tree.hpp>
#include <caf/none.hpp>

#include "broker/core_actor.hh"
#include "broker/defaults.hh"
#include "broker/detail/filesystem.hh"
#include "broker/logger.hh"

using caf::detail::stream_distribution_tree;

using namespace caf;

namespace broker {
namespace detail {

core_policy::core_policy(caf::detail::stream_distribution_tree<core_policy>* p,
                         core_state* state, filter_type filter)
  : parent_(p), state_(state), remaining_records_(0) {
  // TODO: use filter
  BROKER_ASSERT(parent_ != nullptr);
  BROKER_ASSERT(state_ != nullptr);
  auto& cfg = state->self->system().config();
  auto meta_dir = get_or(cfg, "broker.recording-directory",
                         defaults::recording_directory);
  if (!meta_dir.empty() && detail::is_directory(meta_dir)) {
    auto file_name = meta_dir + "/messages.dat";
    recorder_ = make_generator_file_writer(file_name);
    if (recorder_ == nullptr) {
      BROKER_WARNING("cannot open recording file" << file_name);
    } else {
      BROKER_DEBUG("opened file for recording:" << file_name);
      remaining_records_ = get_or(cfg, "broker.output-generator-file-cap",
                                  defaults::output_generator_file_cap);
    }
  }
}

bool core_policy::substream_local_data() const {
  return false;
}

void core_policy::before_handle_batch(stream_slot,
                                      const strong_actor_ptr& hdl) {
  BROKER_TRACE(BROKER_ARG(hdl));
  // If there's anything in the central buffer at this point, it's
  // stuff that we're sending out ourselves (as opposed to forwarding),
  // so we flush it out to each path's own cache now to make sure the
  // subsequent flush in after_handle_batch doesn't accidentally filter
  // out messages where the outband path of previously-buffered messagesi
  // happens to match the path of the inbound data we are handling here.
  peers().selector().active_sender = nullptr;
  peers().fan_out_flush();
  peers().selector().active_sender = actor_cast<actor_addr>(hdl);
}

void core_policy::block_peer(caf::actor peer) {
  blocked_peers.emplace(std::move(peer));
}

void core_policy::unblock_peer(caf::actor peer) {
  blocked_peers.erase(peer);

  auto it = blocked_msgs.find(peer);

  if ( it == blocked_msgs.end() )
    return;

  auto pit = peer_to_ipath_.find(peer);

  if ( pit == peer_to_ipath_.end() ) {
    blocked_msgs.erase(it);
    BROKER_DEBUG("dropped batches after unblocking peer: path no longer exists" << peer);
    return;
  }

  auto& slot = pit->second;
  auto sap = actor_cast<strong_actor_ptr>(peer);

  for ( auto& batch : it->second ) {
    BROKER_DEBUG("handle blocked batch" << peer);
    before_handle_batch(slot, sap);
    handle_batch(slot, sap, batch);
    after_handle_batch(slot, sap);
  }

  blocked_msgs.erase(it);
}

static bool ends_with(const std::string& s, const std::string& ending) {
  if (ending.size() > s.size())
    return false;
  return std::equal(ending.rbegin(), ending.rend(), s.rbegin());
}

void core_policy::handle_batch(stream_slot, const strong_actor_ptr& peer,
                               message& xs) {
  BROKER_TRACE(BROKER_ARG(xs));

  if (xs.match_elements<peer_trait::batch>()) {

    auto peer_actor = caf::actor_cast<actor>(peer);
    auto it = blocked_peers.find(peer_actor);

    if ( it != blocked_peers.end() ) {
      BROKER_DEBUG("buffer batch from blocked peer" << peer);
      auto& bmsgs = blocked_msgs[peer_actor];
      bmsgs.emplace_back(std::move(xs));
      return;
    }

    auto num_workers = workers().num_paths();
    auto num_stores = stores().num_paths();
    BROKER_DEBUG("forward batch from peers;" << BROKER_ARG(num_workers)
                  << BROKER_ARG(num_stores));
    // Only received from other peers. Extract content for to local workers
    // or stores and then forward to other peers.
    for (auto& msg : xs.get_mutable_as<peer_trait::batch>(0)) {
      const topic* t;
      // Dispatch to local workers or stores messages.
      if (is_data_message(msg)) {
        auto& dm = get<data_message>(msg.content);
        t = &get_topic(dm);
        if (num_workers > 0)
          workers().push(dm);
      } else {
        auto& cm = get<command_message>(msg.content);
        t = &get_topic(cm);
        if (num_stores > 0)
          stores().push(cm);
      }
      // Check if forwarding is on.
      if (!state_->options.forward)
        continue;
      // Somewhat hacky, but don't forward data store clone messages.
      if (ends_with(t->string(), topics::clone_suffix.string()))
        continue;
      // Either decrease TTL if message has one already, or add one.
      if (--msg.ttl == 0) {
        BROKER_WARNING("dropped a message with expired TTL");
        continue;
      }
      // Forward to other peers.
      peers().push(std::move(msg));
    }
    return;
  }
  using variant_batch = std::vector<node_message::value_type>;
  if (try_handle<worker_trait::batch>(xs, "publish from local workers")
      || try_handle<store_trait::batch>(xs, "publish from local stores")
      || try_handle<variant_batch>(xs, "publish from custom actors"))
    return;
  BROKER_ERROR("unexpected batch:" << deep_to_string(xs));
}

void core_policy::after_handle_batch(stream_slot, const strong_actor_ptr&) {
  BROKER_TRACE("");
  // Make sure the content of the buffer is pushed to the outbound paths while
  // the sender filter is still active.
  peers().fan_out_flush();
  peers().selector().active_sender = nullptr;
}

void core_policy::ack_open_success(stream_slot slot,
                                   const actor_addr& rebind_from,
                                   strong_actor_ptr rebind_to) {
  BROKER_TRACE(BROKER_ARG(slot) << BROKER_ARG(rebind_from) << BROKER_ARG(rebind_to));
  if (rebind_from != rebind_to) {
    BROKER_DEBUG("rebind occurred" << BROKER_ARG(slot) << BROKER_ARG(rebind_from)
                  << BROKER_ARG(rebind_to));
    peers().filter(slot).first = actor_cast<actor_addr>(rebind_to);
  }
}

void core_policy::ack_open_failure(stream_slot slot,
                                   const actor_addr& rebind_from,
                                   strong_actor_ptr rebind_to) {
  BROKER_TRACE(BROKER_ARG(slot) << BROKER_ARG(rebind_from) << BROKER_ARG(rebind_to));
  CAF_IGNORE_UNUSED(rebind_from);
  CAF_IGNORE_UNUSED(rebind_to);
  auto i = opath_to_peer_.find(slot);
  if (i != opath_to_peer_.end()) {
    auto hdl = i->second;
    remove_peer(hdl, make_error(caf::sec::invalid_stream_state), false, false);
  }
}

void core_policy::push_to_substreams(std::vector<message> xs) {
  // Dispatch on the content of `xs`.
  for (auto& x : xs) {
    if (x.match_elements<topic, data>()) {
      x.force_unshare();
      workers().push(std::move(x.get_mutable_as<topic>(0)),
                     std::move(x.get_mutable_as<data>(1)));
    } else if (x.match_elements<topic, internal_command>()) {
      x.force_unshare();
      stores().push(std::move(x.get_mutable_as<topic>(0)),
                    std::move(x.get_mutable_as<internal_command>(1)));
    }
  }
  workers().emit_batches();
  stores().emit_batches();
}

// -- status updates to the state ----------------------------------------------

void core_policy::peer_lost(const actor& hdl) {
  BROKER_TRACE(BROKER_ARG(hdl));
  state_->emit_status<sc::peer_lost>(hdl, "lost remote peer");
  if (shutting_down())
    return;
  auto x = state_->cache.find(hdl);
  if (!x || x->retry == timeout::seconds(0))
    return;
  BROKER_INFO("will try reconnecting to" << *x << "in" << to_string(x->retry));
  state_->self->delayed_send(state_->self, x->retry, atom::peer::value,
                             atom::retry::value, *x);
}

void core_policy::peer_removed(const actor& hdl) {
  BROKER_TRACE(BROKER_ARG(hdl));
  state_->emit_status<sc::peer_removed>(hdl, "removed peering");
}

// -- callbacks for close/drop events ------------------------------------------

void core_policy::path_closed(stream_slot slot) {
  BROKER_TRACE(BROKER_ARG(slot));
  remove_cb(slot, ipath_to_peer_, peer_to_ipath_, peer_to_opath_, caf::none);
}

void core_policy::path_force_closed(stream_slot slot, error reason) {
  BROKER_TRACE(BROKER_ARG(slot) << BROKER_ARG(reason));
  remove_cb(slot, ipath_to_peer_, peer_to_ipath_, peer_to_opath_,
            std::move(reason));
}

void core_policy::path_dropped(stream_slot slot) {
  BROKER_TRACE(BROKER_ARG(slot));
  remove_cb(slot, opath_to_peer_, peer_to_opath_, peer_to_ipath_, caf::none);
}

void core_policy::path_force_dropped(stream_slot slot, error reason) {
  BROKER_TRACE(BROKER_ARG(slot) << BROKER_ARG(reason));
  remove_cb(slot, opath_to_peer_, peer_to_opath_, peer_to_ipath_,
            std::move(reason));
}

void core_policy::remove_cb(stream_slot slot, path_to_peer_map& xs,
                            peer_to_path_map& ys, peer_to_path_map& zs,
                            error reason) {
  BROKER_TRACE(BROKER_ARG(slot));
  auto i = xs.find(slot);
  if (i == xs.end()) {
    BROKER_DEBUG("no entry in xs found for slot" << slot);
    return;
  }
  auto peer_hdl = i->second;
  remove_peer(peer_hdl, std::move(reason), true, false);
}

// -- state required by the distribution tree --------------------------------

bool core_policy::shutting_down() const {
  return state_->shutting_down;
}

void core_policy::shutting_down(bool value) {
  state_->shutting_down = value;
}

// -- peer management --------------------------------------------------------

bool core_policy::has_peer(const actor& hdl) const {
  return peer_to_opath_.count(hdl) != 0 || peer_to_ipath_.count(hdl) != 0;
}

void core_policy::ack_peering(const stream<node_message>& in,
                              const actor& peer_hdl) {
  BROKER_TRACE(BROKER_ARG(peer_hdl));
  // Check whether we already receive inbound traffic from the peer. Could use
  // `CAF_ASSERT` instead, because this must'nt get called for known peers.
  if (peer_to_ipath_.count(peer_hdl) != 0) {
    BROKER_ERROR("peer already connected");
    return;
  }
  // Add inbound path for our peer.
  auto slot = parent_->add_unchecked_inbound_path(in);
  add_ipath(slot, peer_hdl);
}

bool core_policy::has_outbound_path_to(const caf::actor& peer_hdl) {
  return peer_to_opath_.count(peer_hdl) != 0;
}

bool core_policy::has_inbound_path_from(const caf::actor& peer_hdl) {
  return peer_to_ipath_.count(peer_hdl) != 0;
}

bool core_policy::remove_peer(const actor& hdl, error reason, bool silent,
                              bool graceful_removal) {
  BROKER_TRACE(BROKER_ARG(hdl));
  int performed_erases = 0;
  { // lifetime scope of first iterator pair
    auto e = peer_to_opath_.end();
    auto i = peer_to_opath_.find(hdl);
    if (i != e) {
      BROKER_DEBUG("remove outbound path to peer:" << hdl);
      ++performed_erases;
      out().remove_path(i->second, reason, silent);
      opath_to_peer_.erase(i->second);
      peer_to_opath_.erase(i);
    }
  }
  { // lifetime scope of second iterator pair
    auto e = peer_to_ipath_.end();
    auto i = peer_to_ipath_.find(hdl);
    if (i != e) {
      BROKER_DEBUG("remove inbound path to peer:" << hdl);
      ++performed_erases;
      parent_->remove_input_path(i->second, reason, silent);
      ipath_to_peer_.erase(i->second);
      peer_to_ipath_.erase(i);
    }
  }
  if (performed_erases == 0) {
    BROKER_DEBUG("no path was removed for peer:" << hdl);
    return false;
  }
  if (graceful_removal)
    peer_removed(hdl);
  else
    peer_lost(hdl);
  state_->cache.remove(hdl);
  if (shutting_down() && peer_to_opath_.empty()) {
    // Shutdown when the last peer stops listening.
    parent_->self()->quit(exit_reason::user_shutdown);
  } else {
    // See whether we can make progress without that peer in the mix.
    parent_->push();
  }
  return true;
}

/// Updates the filter of an existing peer.
bool core_policy::update_peer(const actor& hdl, filter_type filter) {
  BROKER_TRACE(BROKER_ARG(hdl) << BROKER_ARG(filter));
  auto e = peer_to_opath_.end();
  auto i = peer_to_opath_.find(hdl);
  if (i == e) {
    BROKER_DEBUG("cannot update filter on unknown peer");
    return false;
  }
  peers().filter(i->second).second = std::move(filter);
  return true;
}

// -- management of worker and storage streams -------------------------------

auto core_policy::add_worker(filter_type filter)
-> outbound_stream_slot<worker_trait::element> {
  BROKER_TRACE(BROKER_ARG(filter));
  auto slot = parent_->add_unchecked_outbound_path<worker_trait::element>();
  if (slot != invalid_stream_slot) {
    out().assign<worker_trait::manager>(slot);
    workers().set_filter(slot, std::move(filter));
  }
  return slot;
}

auto core_policy::add_store(filter_type filter)
-> outbound_stream_slot<store_trait::element> {
  CAF_LOG_TRACE(CAF_ARG(filter));
  auto slot = parent_->add_unchecked_outbound_path<store_trait::element>();
  if (slot != invalid_stream_slot) {
    out().assign<store_trait::manager>(slot);
    stores().set_filter(slot, std::move(filter));
  }
  return slot;
}

// -- selectively pushing data into the streams ------------------------------

/// Pushes data to workers without forwarding it to peers.
void core_policy::local_push(data_message x) {
  BROKER_TRACE(BROKER_ARG(x) << BROKER_ARG2("num_paths", workers().num_paths()));
  if (workers().num_paths() > 0) {
    workers().push(std::move(x));
    workers().emit_batches();
  }
}

/// Pushes data to stores without forwarding it to peers.
void core_policy::local_push(command_message x) {
  BROKER_TRACE(BROKER_ARG(x) << BROKER_ARG2("num_paths", stores().num_paths()));
  if (stores().num_paths() > 0) {
    stores().push(std::move(x));
    stores().emit_batches();
  }
}

/// Pushes data to peers only without forwarding it to local substreams.
void core_policy::remote_push(node_message msg) {
  BROKER_TRACE(BROKER_ARG(msg));
  if (recorder_ != nullptr)
    try_record(msg);
  peers().push(std::move(msg));
  peers().emit_batches();
}

/// Pushes data to peers and workers.
void core_policy::push(data_message msg) {
  BROKER_TRACE(BROKER_ARG(msg));
  remote_push(make_node_message(std::move(msg), state_->options.ttl));
  //local_push(std::move(x), std::move(y));
}

/// Pushes data to peers and stores.
void core_policy::push(command_message msg) {
  BROKER_TRACE(BROKER_ARG(msg));
  remote_push(make_node_message(std::move(msg), state_->options.ttl));
  //local_push(std::move(x), std::move(y));
}

auto core_policy::out() noexcept -> downstream_manager_type& {
  return parent_->out();
}

auto core_policy::out() const noexcept -> const downstream_manager_type& {
  return parent_->out();
}

auto core_policy::peers() noexcept -> peer_trait::manager& {
  return out().get<peer_trait::manager>();
}

auto core_policy::peers() const noexcept -> const peer_trait::manager& {
  return out().get<peer_trait::manager>();
}

auto core_policy::workers() noexcept -> worker_trait::manager& {
  return out().get<worker_trait::manager>();
}

auto core_policy::workers() const noexcept -> const worker_trait::manager& {
  return out().get<worker_trait::manager>();
}

auto core_policy::stores() noexcept -> store_trait::manager& {
  return out().get<store_trait::manager>();
}

auto core_policy::stores() const noexcept -> const store_trait::manager& {
  return out().get<store_trait::manager>();
}

scheduled_actor* core_policy::self() {
  return parent_->self();
}

const scheduled_actor* core_policy::self() const {
  return parent_->self();
}

std::vector<caf::actor> core_policy::get_peer_handles() {
  std::vector<caf::actor> peers;
  for (auto& kvp : peer_to_opath_)
    peers.emplace_back(kvp.first);
  for (auto& kvp : peer_to_ipath_)
    peers.emplace_back(kvp.first);
  auto b = peers.begin();
  auto e = peers.end();
  std::sort(b, e);
  auto p = std::unique(b, e);
  if (p != e)
    peers.erase(p, e);
  return peers;
}

core_policy::ttl core_policy::initial_ttl() const {
  return static_cast<ttl>(state_->options.ttl);
}

void core_policy::add_ipath(stream_slot slot, const actor& peer_hdl) {
  BROKER_TRACE(BROKER_ARG(slot) << BROKER_ARG(peer_hdl));
  if (slot == invalid_stream_slot) {
    BROKER_ERROR("tried to add an invalid inbound path");
    return;
  }
  if (!ipath_to_peer_.emplace(slot, peer_hdl).second) {
    BROKER_ERROR("ipath_to_peer entry already exists");
    return;
  }
  if (!peer_to_ipath_.emplace(peer_hdl, slot).second) {
    BROKER_ERROR("peer_to_ipath entry already exists");
    return;
  }
}

void core_policy::add_opath(stream_slot slot, const actor& peer_hdl) {
  BROKER_TRACE(BROKER_ARG(slot) << BROKER_ARG(peer_hdl));
  if (slot == invalid_stream_slot) {
    BROKER_ERROR("tried to add an invalid outbound path");
    return;
  }
  if (!opath_to_peer_.emplace(slot, peer_hdl).second) {
    BROKER_ERROR("opath_to_peer entry already exists");
    return;
  }
  if (!peer_to_opath_.emplace(peer_hdl, slot).second) {
    BROKER_ERROR("peer_to_opath entry already exists");
    return;
  }
}

auto core_policy::add(std::true_type, const actor& hdl) -> step1_handshake {
  auto xs = std::make_tuple(state_->filter, actor_cast<actor>(self()));
  return parent_->add_unchecked_outbound_path<node_message>(hdl, std::move(xs));
}

auto core_policy::add(std::false_type, const actor& hdl) -> step2_handshake {
  atom_value ok = ok_atom::value;
  auto xs = std::make_tuple(ok, actor_cast<actor>(self()));
  return parent_->add_unchecked_outbound_path<node_message>(hdl, std::move(xs));
}

} // namespace detail
} // namespace broker

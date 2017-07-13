#include <caf/io/middleman.hpp>

#include "broker/address.hh"
#include "broker/configuration.hh"
#include "broker/data.hh"
#include "broker/endpoint.hh"
#include "broker/internal_command.hh"
#include "broker/port.hh"
#include "broker/snapshot.hh"
#include "broker/status.hh"
#include "broker/store.hh"
#include "broker/subnet.hh"
#include "broker/time.hh"
#include "broker/topic.hh"
#include "broker/version.hh"

#include "broker/detail/filter_type.hh"

namespace broker {

configuration::configuration() {
  add_message_type<data>("broker::data");
  add_message_type<address>("broker::address");
  add_message_type<subnet>("broker::subnet");
  add_message_type<port>("broker::port");
  add_message_type<timespan>("broker::timespan");
  add_message_type<timestamp>("broker::timestamp");
  add_message_type<enum_value>("broker::enum_value");
  add_message_type<vector>("broker::vector");
  add_message_type<broker::set>("broker::set");
  add_message_type<status>("broker::status");
  add_message_type<table>("broker::table");
  add_message_type<topic>("broker::topic");
  add_message_type<std::vector<topic>>("std::vector<broker::topic>");
  add_message_type<optional<timestamp>>("broker::optional<broker::timestamp>");
  add_message_type<optional<timespan>>("broker::optional<broker::timespan>");
  add_message_type<snapshot>("broker::snapshot");
  add_message_type<internal_command>("broker::internal_command");
  add_message_type<store::stream_type::value_type>(
    "broker::store::stream_type::value_type");
  add_message_type<std::vector<store::stream_type::value_type>>(
    "std::vector<broker::store::stream_type::value_type>");
  add_message_type<endpoint::stream_type::value_type>(
    "broker::endpoint::stream_type::value_type");
  add_message_type<std::vector<endpoint::stream_type::value_type>>(
    "std::vector<broker::endpoint::stream_type::value_type>");
  load<caf::io::middleman>();
  logger_file_name = "broker_[PID]_[TIMESTAMP].log";
  logger_verbosity = caf::atom("INFO");
  logger_component_filter = "broker";

  if (auto env = getenv("BROKER_DEBUG_VERBOSE")) {
    if (*env && *env != '0') {
      logger_verbosity = caf::atom("DEBUG");
      logger_component_filter = "";
    }
  }

  if (auto env = getenv("BROKER_DEBUG_LEVEL")) {
    char level[10];
    strncpy(level, env, sizeof(level));
    level[sizeof(level) - 1] = '\0';
    logger_verbosity = caf::atom(level);
  }

  if (auto env = getenv("BROKER_DEBUG_COMPONENT_FILTER")) {
    logger_component_filter = env;
  }

  middleman_app_identifier = "broker.v" + std::to_string(version::protocol);
}

configuration::configuration(int argc, char** argv) : configuration{} {
  parse(argc, argv);
}

} // namespace broker

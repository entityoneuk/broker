#define SUITE alm.multipath

#include "broker/alm/multipath.hh"

#include "test.hh"

using broker::alm::multipath;

using namespace broker;

namespace {

using linear_path = std::vector<std::string>;

} // namespace

FIXTURE_SCOPE(multipath_tests, base_fixture)

TEST(multipaths are default constructible) {
  multipath<std::string> p;
  CHECK_EQUAL(p.id(), "");
  CHECK_EQUAL(p.nodes().size(), 0u);
  CHECK_EQUAL(caf::deep_to_string(p), R"__((""))__");
}

TEST(users can fill multipaths with emplace_node) {
  multipath<std::string> p{"a"};
  auto ac = p.emplace_node("ac").first;
  ac->emplace_node("acb");
  ac->emplace_node("aca");
  auto ab = p.emplace_node("ab").first;
  ab->emplace_node("abb");
  ab->emplace_node("aba");
  CHECK_EQUAL(
    caf::deep_to_string(p),
    R"__(("a", [("ab", [("aba"), ("abb")]), ("ac", [("aca"), ("acb")])]))__");
}

TEST(multipaths are constructible from linear paths) {
  linear_path abc{"a", "b", "c"};
  multipath<std::string> path{abc.begin(), abc.end()};
  CHECK_EQUAL(caf::deep_to_string(path), R"__(("a", [("b", [("c")])]))__");
}

TEST(multipaths are copy constructible and comparable) {
  linear_path abc{"a", "b", "c"};
  multipath<std::string> path1{abc.begin(), abc.end()};
  auto path2 = path1;
  CHECK_EQUAL(caf::deep_to_string(path1), caf::deep_to_string(path2));
  CHECK_EQUAL(path1, path2);
}

TEST(splicing an empty or equal linear path is a nop) {
  linear_path abc{"a", "b", "c"};
  multipath<std::string> path1{abc.begin(), abc.end()};
  auto path2 = path1;
  linear_path empty_path;
  CHECK(path2.splice(empty_path));
  CHECK_EQUAL(path1, path2);
  CHECK(path2.splice(abc));
  CHECK_EQUAL(path1, path2);
}

TEST(splicing merges linear paths into multipaths) {
  linear_path abc{"a", "b", "c"};
  linear_path abd{"a", "b", "d"};
  linear_path aef{"a", "e", "f"};
  linear_path aefg{"a", "e", "f", "g"};
  multipath<std::string> path{"a"};
  for (const auto& lp : {abc, abd, aef, aefg})
    CHECK(path.splice(lp));
  CHECK_EQUAL(caf::deep_to_string(path),
              R"__(("a", [("b", [("c"), ("d")]), ("e", [("f", [("g")])])]))__");
}

TEST(multipaths are serializable) {
  multipath<std::string> path{"a"};
  MESSAGE("fill the path with nodes");
  {
    auto ac = path.emplace_node("ac").first;
    ac->emplace_node("acb");
    ac->emplace_node("aca");
    auto ab = path.emplace_node("ab").first;
    ab->emplace_node("abb");
    ab->emplace_node("aba");
  }
  caf::binary_serializer::container_type buf;
  MESSAGE("serializer the path into a buffer");
  {
    caf::binary_serializer sink{sys, buf};
    CHECK_EQUAL(sink(path), caf::none);
  }
  multipath<std::string> copy{"a"};
  MESSAGE("deserializers a copy from the path from the buffer");
  {
    caf::binary_deserializer source{sys,buf};
    CHECK_EQUAL(source(copy), caf::none);
  }
  MESSAGE("after a serialization roundtrip, the path is equal to its copy");
  CHECK_EQUAL(path, copy);
  CHECK_EQUAL(caf::deep_to_string(path), caf::deep_to_string(copy));
}

FIXTURE_SCOPE_END()

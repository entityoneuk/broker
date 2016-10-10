// Copyright (C) 2013 Jarryd Beck
//
// (adapted by Matthias Vallentin, then Jon Siwek).
//
// Distributed under the Boost Software License, Version 1.0
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
//
//   The copyright notices in the Software and this entire statement, including
//   the above license grant, this restriction and the following disclaimer,
//   must be included in all copies of the Software, in whole or in part, and
//   all derivative works of the Software, unless such copies or derivative
//   works are solely in the form of machine-executable object code generated by
//   a source language processor.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
//   SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
//   FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
//   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.

#ifndef BROKER_DETAIL_VARIANT_HH
#define BROKER_DETAIL_VARIANT_HH

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <type_traits>

#include <caf/deserializer.hpp>
#include <caf/serializer.hpp>

#include "broker/detail/assert.hh"
#include "broker/detail/hash.hh"
#include "broker/detail/operators.hh"
#include "broker/detail/type_traits.hh"

namespace broker {
namespace detail {

template <class Visitor>
class delayed_visitor {
public:
  using result_type = typename remove_reference_t<Visitor>::result_type;

  delayed_visitor(Visitor v) : visitor(std::move(v)) {
  }

  template <class... Visitables>
  result_type operator()(Visitables&&... vs) {
    return visit(visitor, std::forward<Visitables>(vs)...);
  }

private:
  Visitor visitor;
};

template <class Visitor>
class delayed_visitor_wrapper {
public:
  using result_type = typename remove_reference_t<Visitor>::result_type;

  delayed_visitor_wrapper(Visitor& visitor) : visitor(visitor) {
  }

  template <class... Visitables>
  result_type operator()(Visitables&&... vs) {
    return visit(visitor, std::forward<Visitables>(vs)...);
  }

private:
  Visitor& visitor;
};

template <class Visitor, class Visitable>
class binary_visitor {
public:
  using result_type = typename remove_reference_t<Visitor>::result_type;

  binary_visitor(Visitor& arg_visitor, Visitable& arg_visitable)
    : visitor(arg_visitor), visitable(arg_visitable) {
  }

  template <class... Ts>
  result_type operator()(Ts&&... xs) {
    return visitable.template apply(visitor, std::forward<Ts>(xs)...);
  }

private:
  Visitor& visitor;
  Visitable& visitable;
};

/// A variant class modeled after C++17's variant.
/// @tparam Ts the types the variant should assume.
template <class... Ts>
class variant : totally_ordered<variant<Ts...>> {
  // Workaround for http://stackoverflow.com/q/24433658/1170277
  template <class T, class...>
  struct front {
    using type = T;
  };

  using first_type = typename front<Ts...>::type;

public:
  /// Default-construct a variant with the first type.
  variant() noexcept(std::is_nothrow_default_constructible<first_type>::value) {
    construct(first_type{});
    index_ = 0;
  }

   /// Destruct variant by invoking destructor of the active instance.
  ~variant() noexcept {
    destruct();
  }

  template <class T, class = disable_if_same_or_derived_t<variant, T>>
  variant(T&& x) {
    // A compile error here means that T is not unambiguously convertible to
    // any of the variant types.
    initializer<0, Ts...>::initialize(*this, std::forward<T>(x));
  }

  variant(const variant& other) {
    other.apply(copy_ctor(*this));
    index_ = other.index_;
  }

  variant(variant&& other) noexcept {
    other.apply(move_ctor(*this));
    index_ = other.index_;
  }

  variant& operator=(const variant& rhs) {
    rhs.apply(assigner{*this, rhs.index_});
    index_ = rhs.index_;
    return *this;
  }

  variant& operator=(variant&& rhs)
  noexcept(detail::conjunction<
             std::is_nothrow_move_assignable<Ts>...
           >::value) {
    rhs.apply(move_assigner(*this, rhs.index_));
    index_ = rhs.index_;
    return *this;
  }

  /// @returns the index of the active variant type.
  size_t index() const {
    return index_;
  }

  template <class Visitor, class... Args>
  typename remove_reference_t<Visitor>::result_type
  apply(Visitor&& visitor, Args&&... args) {
    return visit_impl(index_, storage_, std::forward<Visitor>(visitor),
                      std::forward<Args>(args)...);
  }

  template <class Visitor, class... Args>
  typename remove_reference_t<Visitor>::result_type
  apply(Visitor&& visitor, Args&&... args) const {
    return visit_impl(index_, storage_, std::forward<Visitor>(visitor),
                      std::forward<Args>(args)...);
  }

private:
  template <class T>
  struct Sizeof {
    static constexpr auto value = sizeof(T);
  };

  template <class T>
  struct Alignof {
    static constexpr auto value = alignof(T);
  };

  // Could use std::aligned_union, but GCC doesn't have it.
  using storage_type =
    aligned_storage_t<max<Sizeof, Ts...>(), max<Alignof, Ts...>()>;

  storage_type storage_;
  size_t index_;

  struct copy_ctor {
    using result_type = void;

    copy_ctor(variant& self) : self(self) {
    }

    template <class T>
    void operator()(const T& x) const
    noexcept(std::is_nothrow_copy_constructible<T>::value) {
      self.construct(x);
    }

    variant& self;
  };

  struct move_ctor {
    using result_type = void;

    move_ctor(variant& self) : self(self) {
    }

    template <class T>
    void operator()(T& rhs) const
    noexcept(std::is_nothrow_move_constructible<T>::value) {
      self.construct(std::move(rhs));
    }

    variant& self;
  };

  struct assigner {
    using result_type = void;

    template <class Rhs>
    void operator()(const Rhs& rhs) const
    noexcept(std::is_nothrow_copy_assignable<Rhs>::value &&
             std::is_nothrow_destructible<Rhs>::value &&
             std::is_nothrow_move_constructible<Rhs>::value) {
      if (self.index_ == rhs_index) {
        *reinterpret_cast<Rhs*>(&self.storage_) = rhs;
      } else {
        Rhs tmp(rhs);
        self.destruct();
        self.construct(std::move(tmp));
      }
    }

    variant& self;
    size_t rhs_index;
  };

  template <class T>
  struct container_uses_default_allocator {
    static const bool value = false;
  };

  template <class CharT, class Traits>
  struct container_uses_default_allocator<std::basic_string<CharT, Traits>> {
    static const bool value = true;
  };

  template <class T, class Compare>
  struct container_uses_default_allocator<std::set<T, Compare>> {
    static const bool value = true;
  };

  template <class Key, class T, class Compare>
  struct container_uses_default_allocator<std::map<Key, T, Compare>> {
    static const bool value = true;
  };

  struct move_assigner {
    using result_type = void;

    move_assigner(variant& self, size_t rhs_index)
      : self(self), rhs_index(rhs_index) {
    }

    template <class Rhs>
    void operator()(Rhs& rhs) const
    noexcept(std::is_nothrow_destructible<Rhs>::value &&
             std::is_nothrow_move_constructible<Rhs>{}) {
      using rhs_type = typename std::remove_const<Rhs>::type;
      if (self.index_ == rhs_index) {
        *reinterpret_cast<rhs_type*>(&self.storage_) = std::move(rhs);
      } else {
        self.destruct();
        self.construct(std::move(rhs));
      }
    }

    variant& self;
    size_t rhs_index;
  };

  struct dtor {
    using result_type = void;

    template <class T>
    void operator()(T& x) const noexcept {
      static_assert(std::is_nothrow_destructible<T>{},
                    "T must not throw in destructor");
      x.~T();
    }
  };

  template <size_t TT, class... Tail>
  struct initializer;

  template <size_t TT, class T, class... Tail>
  struct initializer<TT, T, Tail...> : public initializer<TT + 1, Tail...> {
    using base = initializer<TT + 1, Tail...>;
    using base::initialize;

    static void initialize(variant& v, T&& x) {
      v.construct(std::move(x));
      v.index_ = TT;
    }

    static void initialize(variant& v, const T& x) {
      v.construct(x);
      v.index_ = TT;
    }
  };

  template <size_t TT>
  struct initializer<TT> {
    void initialize(); // this should never match
  };

  template <class T, class Storage>
  using const_type =
    typename std::conditional<
      std::is_const<remove_reference_t<Storage>>::value, T const, T
    >::type;

  template <class T, class Storage, class Visitor, class... Args>
  static typename remove_reference_t<Visitor>::result_type
  invoke(Storage&& storage, Visitor&& visitor, Args&&... args) {
    auto x = reinterpret_cast<const_type<T, Storage>*>(&storage);
    return visitor(*x, args...);
  }

  template <class Storage, class Visitor, class... Args>
  static typename remove_reference_t<Visitor>::result_type
  visit_impl(size_t which_active, Storage&& storage,
             Visitor&& visitor, Args&&... args) {
    using result_type = typename remove_reference_t<Visitor>::result_type;
    using fn = result_type (*)(Storage&&, Visitor&&, Args&&...);
    static constexpr fn callers[sizeof...(Ts)]
      = {&invoke<Ts, Storage, Visitor, Args...>...};
    BROKER_ASSERT(static_cast<size_t>(which_active) >= 0
                  && static_cast<size_t>(which_active) < sizeof...(Ts));
    return (*callers[static_cast<size_t>(which_active)])(
      std::forward<Storage>(storage), std::forward<Visitor>(visitor),
      std::forward<Args>(args)...);
  }

  template <class T>
  void construct(T&& x) {
    using type = remove_reference_t<T>;
    new (&storage_) type(std::forward<T>(x));
  }

  void destruct() noexcept {
    apply(dtor());
  }

  struct equals {
    using result_type = bool;

    equals(const variant& self) : self(self) {
    }

    template <class Rhs>
    bool operator()(const Rhs& rhs) const {
      return *reinterpret_cast<const Rhs*>(&self.storage_) == rhs;
    }

    const variant& self;
  };

  struct less_than {
    using result_type = bool;

    less_than(const variant& self) : self(self) {
    }

    template <class Rhs>
    bool operator()(const Rhs& rhs) const {
      return *reinterpret_cast<const Rhs*>(&self.storage_) < rhs;
    }

    const variant& self;
  };

  friend bool operator==(const variant& x, const variant& y) {
    return x.index_ == y.index_ && y.apply(equals{x});
  }

  friend bool operator<(const variant& x, const variant& y) {
    if (x.index_ == y.index_)
      return y.apply(less_than{x});
    else
      return x.index_ < y.index_;
  }

  struct variant_serializer {
    using result_type = void;

    template <class T>
    result_type operator()(const T& x) const {
      sink << x;
    }

    caf::serializer& sink;
  };

  struct variant_deserializer {
    using result_type = void;

    template <class T>
    result_type operator()(T& x) const {
      self.construct(T{});
      source >> x;
    }

    variant& self;
    caf::deserializer& source;
  };

  friend void serialize(caf::serializer& sink, const variant& v) {
    sink << v.index_;
    visit(variant_serializer{sink}, v);
  }

  friend void serialize(caf::deserializer& source, variant& v) {
    source >> v.index_;
    visit(variant_deserializer{v, source}, v);
  }
};

template <class Visitor>
delayed_visitor<Visitor> visit(Visitor&& visitor) {
  return delayed_visitor<Visitor>(std::move(visitor));
}

template <class Visitor>
delayed_visitor_wrapper<Visitor> visit(Visitor& visitor) {
  return delayed_visitor_wrapper<Visitor>(visitor);
}

template <class Visitor, class Visitable>
typename remove_reference_t<Visitor>::result_type
visit(Visitor&& visitor, Visitable&& visitable) {
  return visitable.template apply(std::forward<Visitor>(visitor));
}

template <class Visitor, class V, class... Vs>
typename remove_reference_t<Visitor>::result_type
visit(Visitor&& visitor, V&& v, Vs&&... vs) {
  return visit(binary_visitor<Visitor, V>(visitor, v), vs...);
}

template <class T>
struct getter {
  using result_type = T*;

  result_type operator()(T& val) const {
    return &val;
  }

  template <class U>
  result_type operator()(const U&) const {
    return nullptr;
  }
};

class bad_variant_access : public std::exception {
public:
  bad_variant_access() = default;

  const char* what() const noexcept override {
    return "bad variant access";
  }
};

template <class T, class... Ts>
T* get_if(variant<Ts...>& v) {
  return visit(getter<T>{}, v);
}

template <class T, class... Ts>
const T* get_if(const variant<Ts...>& v) {
  return visit(getter<const T>{}, v);
}

// FIXME: return references instead of pointers
template <class T, class... Ts>
T& get(variant<Ts...>& v) {
  if (auto ptr = get_if<T>(v))
    return *ptr;
  throw bad_variant_access{};
}

template <class T, class... Ts>
const T& get(const variant<Ts...>& v) {
  if (auto ptr = get_if<const T>(v))
    return *ptr;
  throw bad_variant_access{};
}

} // namespace detail
} // namespace broker

namespace std {

template <class... Ts>
struct hash<broker::detail::variant<Ts...>> {
  using result_type = size_t;

  struct hasher {
    using result_type = size_t;

    template <class T>
    result_type operator()(const T& x) const {
      return std::hash<T>{}(x);
    }
  };

  result_type operator()(const broker::detail::variant<Ts...>& v) const {
    result_type result;
    broker::detail::hash_combine(result, v.index());
    broker::detail::hash_combine(result, visit(hasher{}, v));
    return result;
  }
};

} // namespace std

#endif // BROKER_VARIANT_HH

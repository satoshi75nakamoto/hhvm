/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Docs: https://fburl.com/fbcref_range
//

/**
 * Range abstraction using a pair of iterators. It is not
 * similar to boost's range abstraction because an API identical
 * with the former StringPiece class is required, which is used alot
 * internally. This abstraction does fulfill the needs of boost's
 * range-oriented algorithms though.
 *
 * Note: (Keep memory lifetime in mind when using this class, since it
 * does not manage the data it refers to - just like an iterator
 * would not.)
 *
 * Additional documentation is in folly/docs/Range.md
 *
 * @refcode folly/docs/examples/folly/Range.h
 * @struct folly::range
 */

#pragma once

#include <folly/Portability.h>
#include <folly/hash/SpookyHashV2.h>
#include <folly/lang/CString.h>
#include <folly/lang/Exception.h>
#include <folly/portability/Constexpr.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstring>
#include <iosfwd>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#if defined(__cpp_lib_ranges)
#include <ranges>
#endif

#if __has_include(<fmt/format.h>)
#include <fmt/format.h>
#endif

#include <folly/CpuId.h>
#include <folly/Likely.h>
#include <folly/Traits.h>
#include <folly/detail/RangeCommon.h>
#include <folly/detail/RangeSimd.h>

// Ignore shadowing warnings within this file, so includers can use -Wshadow.
FOLLY_PUSH_WARNING
FOLLY_GNU_DISABLE_WARNING("-Wshadow")

namespace folly {

/**
 * Ubiquitous helper template for knowing what's a string.
 */
template <class T>
struct IsSomeString : std::false_type {};

template <typename Alloc>
struct IsSomeString<std::basic_string<char, std::char_traits<char>, Alloc>>
    : std::true_type {};

template <class Iter>
class Range;

/**
 * Finds the first occurrence of needle in haystack. The algorithm is on
 * average faster than O(haystack.size() * needle.size()) but not as fast
 * as Boyer-Moore. On the upside, it does not do any upfront
 * preprocessing and does not allocate memory.
 */
template <
    class Iter,
    class Comp = std::equal_to<typename Range<Iter>::value_type>>
inline size_t qfind(
    const Range<Iter>& haystack, const Range<Iter>& needle, Comp eq = Comp());

/**
 * Finds the first occurrence of needle in haystack. The result is the
 * offset reported to the beginning of haystack, or string::npos if
 * needle wasn't found.
 */
template <class Iter>
size_t qfind(
    const Range<Iter>& haystack,
    const typename Range<Iter>::value_type& needle);

/**
 * Finds the last occurrence of needle in haystack. The result is the
 * offset reported to the beginning of haystack, or string::npos if
 * needle wasn't found.
 */
template <class Iter>
size_t rfind(
    const Range<Iter>& haystack,
    const typename Range<Iter>::value_type& needle);

/**
 * Finds the first occurrence of any element of needle in
 * haystack. The algorithm is O(haystack.size() * needle.size()).
 */
template <class Iter>
inline size_t qfind_first_of(
    const Range<Iter>& haystack, const Range<Iter>& needle);

/**
 * Small internal helper - returns the value just before an iterator.
 */
namespace detail {

/*
 * Use IsCharPointer<T>::type to enable const char* or char*.
 * Use IsCharPointer<T>::const_type to enable only const char*.
 */
template <class T>
struct IsCharPointer {};

template <>
struct IsCharPointer<char*> {
  using type = int;
};

template <>
struct IsCharPointer<const char*> {
  using const_type = int;
  using type = int;
};

template <class T>
struct IsUnsignedCharPointer {};

template <>
struct IsUnsignedCharPointer<unsigned char*> {
  using type = int;
};

template <>
struct IsUnsignedCharPointer<const unsigned char*> {
  using const_type = int;
  using type = int;
};

void range_is_char_type_f_(char const*);
void range_is_char_type_f_(wchar_t const*);
#if (defined(__cpp_char8_t) && __cpp_char8_t >= 201811L) || \
    FOLLY_CPLUSPLUS >= 202002
void range_is_char_type_f_(char8_t const*);
#endif
void range_is_char_type_f_(char16_t const*);
void range_is_char_type_f_(char32_t const*);
template <typename Iter>
using range_is_char_type_d_ =
    decltype(folly::detail::range_is_char_type_f_(FOLLY_DECLVAL(Iter)));
template <typename Iter>
constexpr bool range_is_char_type_v_ =
    is_detected_v<range_is_char_type_d_, Iter>;

void range_is_byte_type_f_(unsigned char const*);
void range_is_byte_type_f_(signed char const*);
void range_is_byte_type_f_(std::byte const*);
template <typename Iter>
using range_is_byte_type_d_ =
    decltype(folly::detail::range_is_byte_type_f_(FOLLY_DECLVAL(Iter)));
template <typename Iter>
constexpr bool range_is_byte_type_v_ =
    is_detected_v<range_is_byte_type_d_, Iter>;

struct range_traits_char_ {
  template <typename Value>
  using apply = std::char_traits<Value>;
};
struct range_traits_byte_ {
  template <typename Value>
  struct apply {
    FOLLY_ERASE static constexpr bool eq(Value a, Value b) { return a == b; }

    FOLLY_ERASE static constexpr int compare(
        Value const* a, Value const* b, std::size_t c) {
      return !c ? 0 : std::memcmp(a, b, c);
    }
  };
};
struct range_traits_fbck_ {
  template <typename Value>
  struct apply {
    FOLLY_ERASE static constexpr bool eq(Value a, Value b) { return a == b; }

    FOLLY_ERASE static constexpr int compare(
        Value const* a, Value const* b, std::size_t c) {
      while (c--) {
        auto&& ai = *a++;
        auto&& bi = *b++;
        if (ai < bi) {
          return -1;
        }
        if (bi < ai) {
          return +1;
        }
      }
      return 0;
    }
  };
};

template <typename Iter>
using range_traits_c_ = conditional_t<
    range_is_char_type_v_<Iter>,
    range_traits_char_,
    conditional_t< //
        range_is_byte_type_v_<Iter>,
        range_traits_byte_,
        range_traits_fbck_>>;
template <typename Iter, typename Value>
using range_traits_t_ = typename range_traits_c_<Iter>::template apply<Value>;

} // namespace detail

template <class Iter>
class Range {
 private:
  using iter_traits = std::iterator_traits<Iter>;

  template <typename Alloc>
  using string = std::basic_string<char, std::char_traits<char>, Alloc>;

 public:
  using value_type = typename iter_traits::value_type;
  using size_type = std::size_t;
  using difference_type = typename iter_traits::difference_type;
  using iterator = Iter;
  using const_iterator = Iter;
  using reference = typename iter_traits::reference;
  using const_reference = conditional_t<
      std::is_lvalue_reference_v<reference>,
      std::add_lvalue_reference_t<
          std::add_const_t<std::remove_reference_t<reference>>>,
      conditional_t<
          std::is_rvalue_reference_v<reference>,
          std::add_rvalue_reference_t<
              std::add_const_t<std::remove_reference_t<reference>>>,
          reference>>;

  /*
   * For MutableStringPiece and MutableByteRange we define StringPiece
   * and ByteRange as const_range_type (for everything else its just
   * identity). We do that to enable operations such as find with
   * args which are const.
   */
  using const_range_type = typename std::conditional<
      std::is_same<Iter, char*>::value ||
          std::is_same<Iter, unsigned char*>::value,
      Range<const value_type*>,
      Range<Iter>>::type;

  using traits_type = detail::range_traits_t_<Iter, value_type>;

  static const size_type npos;

  /**
   * Works for all iterator
   *
   *
   * @methodset Range
   */
  constexpr Range() : b_(), e_() {}

  constexpr Range(const Range&) = default;
  constexpr Range(Range&&) = default;

 public:
  /**
   * Works for all iterators
   *
   *
   * @methodset Range
   */
  constexpr Range(Iter start, Iter end) : b_(start), e_(end) {}
  /**
   *  Works only for random-access iterators
   *
   *
   * @methodset Range
   */
  constexpr Range(Iter start, size_t size) : b_(start), e_(start + size) {}

  /* implicit */ Range(std::nullptr_t) = delete;

  constexpr /* implicit */ Range(Iter str)
      : b_(str), e_(str + constexpr_strlen(str)) {
    static_assert(
        std::is_same<int, typename detail::IsCharPointer<Iter>::type>::value,
        "This constructor is only available for character ranges");
  }

  template <
      class Alloc,
      class T = Iter,
      typename detail::IsCharPointer<T>::const_type = 0>
  /* implicit */ Range(const string<Alloc>& str)
      : b_(str.data()), e_(b_ + str.size()) {}

  template <
      class Alloc,
      class T = Iter,
      typename detail::IsCharPointer<T>::const_type = 0>
  Range(const string<Alloc>& str, typename string<Alloc>::size_type startFrom) {
    if (FOLLY_UNLIKELY(startFrom > str.size())) {
      throw_exception<std::out_of_range>("index out of range");
    }
    b_ = str.data() + startFrom;
    e_ = str.data() + str.size();
  }

  template <
      class Alloc,
      class T = Iter,
      typename detail::IsCharPointer<T>::const_type = 0>
  Range(
      const string<Alloc>& str,
      typename string<Alloc>::size_type startFrom,
      typename string<Alloc>::size_type size) {
    if (FOLLY_UNLIKELY(startFrom > str.size())) {
      throw_exception<std::out_of_range>("index out of range");
    }
    b_ = str.data() + startFrom;
    if (str.size() - startFrom < size) {
      e_ = str.data() + str.size();
    } else {
      e_ = b_ + size;
    }
  }

  Range(const Range& other, size_type first, size_type length = npos)
      : Range(other.subpiece(first, length)) {}

  template <
      class Container,
      class = typename std::enable_if<
          std::is_same<Iter, typename Container::const_pointer>::value>::type,
      class = decltype(
          Iter(std::declval<Container const&>().data()),
          Iter(
              std::declval<Container const&>().data() +
              std::declval<Container const&>().size()))>
  /* implicit */ constexpr Range(Container const& container)
      : Range(container.data(), container.size()) {}

  template <
      class Container,
      class = typename std::enable_if<
          std::is_same<Iter, typename Container::const_pointer>::value>::type,
      class = decltype(
          Iter(std::declval<Container const&>().data()),
          Iter(
              std::declval<Container const&>().data() +
              std::declval<Container const&>().size()))>
  Range(Container const& container, typename Container::size_type startFrom) {
    auto const cdata = container.data();
    auto const csize = container.size();
    if (FOLLY_UNLIKELY(startFrom > csize)) {
      throw_exception<std::out_of_range>("index out of range");
    }
    b_ = cdata + startFrom;
    e_ = cdata + csize;
  }

  template <
      class Container,
      class = typename std::enable_if<
          std::is_same<Iter, typename Container::const_pointer>::value>::type,
      class = decltype(
          Iter(std::declval<Container const&>().data()),
          Iter(
              std::declval<Container const&>().data() +
              std::declval<Container const&>().size()))>
  Range(
      Container const& container,
      typename Container::size_type startFrom,
      typename Container::size_type size) {
    auto const cdata = container.data();
    auto const csize = container.size();
    if (FOLLY_UNLIKELY(startFrom > csize)) {
      throw_exception<std::out_of_range>("index out of range");
    }
    b_ = cdata + startFrom;
    if (csize - startFrom < size) {
      e_ = cdata + csize;
    } else {
      e_ = b_ + size;
    }
  }

  /**
   * @brief Allow explicit construction of ByteRange from std::string_view or
   * std::string.

   * Given that we allow implicit construction of ByteRange from
   * StringPiece, it makes sense to allow this explicit construction, and avoids
   * callers having to say ByteRange{StringPiece{str}} when they want a
   * ByteRange pointing to data in a std::string.
   *
   *
   * @methodset Range
   */
  template <
      class Container,
      class T = Iter,
      typename detail::IsUnsignedCharPointer<T>::const_type = 0,
      class = typename std::enable_if<
            std::is_same<typename Container::const_pointer, const char*>::value
          >::type,
      class = decltype(
          Iter(std::declval<Container const&>().data()),
          Iter(
              std::declval<Container const&>().data() +
              std::declval<Container const&>().size()))>
  explicit Range(const Container& str)
      : b_(reinterpret_cast<Iter>(str.data())), e_(b_ + str.size()) {}
  /**
   * @brief Allow implicit conversion from Range<const char*> (aka StringPiece)
   to
   * Range<const unsigned char*> (aka ByteRange).

   * Give both are frequently
   * used to represent ranges of bytes.  Allow explicit conversion in the other
   * direction.
   *
   * @methodset Range
   */
  template <
      class OtherIter,
      typename std::enable_if<
          (std::is_same<Iter, const unsigned char*>::value &&
           (std::is_same<OtherIter, const char*>::value ||
            std::is_same<OtherIter, char*>::value)),
          int>::type = 0>
  /* implicit */ Range(const Range<OtherIter>& other)
      : b_(reinterpret_cast<const unsigned char*>(other.begin())),
        e_(reinterpret_cast<const unsigned char*>(other.end())) {}

  template <
      class OtherIter,
      typename std::enable_if<
          (std::is_same<Iter, unsigned char*>::value &&
           std::is_same<OtherIter, char*>::value),
          int>::type = 0>
  /* implicit */ Range(const Range<OtherIter>& other)
      : b_(reinterpret_cast<unsigned char*>(other.begin())),
        e_(reinterpret_cast<unsigned char*>(other.end())) {}

  template <
      class OtherIter,
      typename std::enable_if<
          (std::is_same<Iter, const char*>::value &&
           (std::is_same<OtherIter, const unsigned char*>::value ||
            std::is_same<OtherIter, unsigned char*>::value)),
          int>::type = 0>
  explicit Range(const Range<OtherIter>& other)
      : b_(reinterpret_cast<const char*>(other.begin())),
        e_(reinterpret_cast<const char*>(other.end())) {}

  template <
      class OtherIter,
      typename std::enable_if<
          (std::is_same<Iter, char*>::value &&
           std::is_same<OtherIter, unsigned char*>::value),
          int>::type = 0>
  explicit Range(const Range<OtherIter>& other)
      : b_(reinterpret_cast<char*>(other.begin())),
        e_(reinterpret_cast<char*>(other.end())) {}

  /**
   * Allow implicit conversion from Range<From> to Range<To> if From is
   * implicitly convertible to To.
   *
   * @methodset Range
   */
  template <
      class OtherIter,
      typename std::enable_if<
          (!std::is_same<Iter, OtherIter>::value &&
           std::is_convertible<OtherIter, Iter>::value),
          int>::type = 0>
  constexpr /* implicit */ Range(const Range<OtherIter>& other)
      : b_(other.begin()), e_(other.end()) {}
  /**
   * Allow explicit conversion from Range<From> to Range<To> if From is
   * explicitly convertible to To.
   *
   * @methodset Range
   */
  template <
      class OtherIter,
      typename std::enable_if<
          (!std::is_same<Iter, OtherIter>::value &&
           !std::is_convertible<OtherIter, Iter>::value &&
           std::is_constructible<Iter, const OtherIter&>::value),
          int>::type = 0>
  constexpr explicit Range(const Range<OtherIter>& other)
      : b_(other.begin()), e_(other.end()) {}

  /**
   * Allow explicit construction of Range() from a std::array of a
   * convertible type.
   *
   * For instance, this allows constructing StringPiece from a
   * std::array<char, N> or a std::array<const char, N>
   *
   * @methodset Range
   */
  template <
      class T,
      size_t N,
      typename = typename std::enable_if<
          std::is_convertible<const T*, Iter>::value>::type>
  constexpr explicit Range(const std::array<T, N>& array)
      : b_{array.empty() ? nullptr : &array.at(0)},
        e_{array.empty() ? nullptr : &array.at(0) + N} {}
  template <
      class T,
      size_t N,
      typename =
          typename std::enable_if<std::is_convertible<T*, Iter>::value>::type>
  constexpr explicit Range(std::array<T, N>& array)
      : b_{array.empty() ? nullptr : &array.at(0)},
        e_{array.empty() ? nullptr : &array.at(0) + N} {}

  Range& operator=(const Range& rhs) & = default;
  Range& operator=(Range&& rhs) & = default;

  template <
      class Alloc,
      class T = Iter,
      typename detail::IsCharPointer<T>::const_type = 0>
  Range& operator=(string<Alloc>&& rhs) = delete;

  /**
   * Clear start and end iterators
   */
  void clear() {
    b_ = Iter();
    e_ = Iter();
  }

  /**
   * Assign start and end iterators
   */
  void assign(Iter start, Iter end) {
    b_ = start;
    e_ = end;
  }

  /**
   * Reset start and end interator based on size
   */
  void reset(Iter start, size_type size) {
    b_ = start;
    e_ = start + size;
  }

  // Works only for Range<const char*>
  template <typename Alloc>
  void reset(const string<Alloc>& str) {
    reset(str.data(), str.size());
  }

  constexpr size_type size() const {
    assert(b_ <= e_);
    return size_type(e_ - b_);
  }
  constexpr size_type walk_size() const {
    return size_type(std::distance(b_, e_));
  }
  constexpr bool empty() const { return b_ == e_; }
  constexpr Iter data() const { return b_; }
  constexpr Iter start() const { return b_; }
  constexpr Iter begin() const { return b_; }
  constexpr Iter end() const { return e_; }
  constexpr Iter cbegin() const { return b_; }
  constexpr Iter cend() const { return e_; }
  reference front() {
    assert(b_ < e_);
    return *b_;
  }
  reference back() {
    assert(b_ < e_);
    return *std::prev(e_);
  }
  const_reference front() const {
    assert(b_ < e_);
    return *b_;
  }
  const_reference back() const {
    assert(b_ < e_);
    return *std::prev(e_);
  }

 private:
  // It would be nice to be able to implicit convert to any target type
  // T for which either an (Iter, Iter) or (Iter, size_type) noexcept
  // constructor was available, and explicitly convert to any target
  // type for which those signatures were available but not noexcept.
  // The problem is that this creates ambiguity when there is also a
  // T constructor that takes a type U that is implicitly convertible
  // from Range.
  //
  // To avoid ambiguity, we need to avoid having explicit operator T
  // and implicit operator U coexist when T is constructible from U.
  // U cannot be deduced when searching for operator T (and C++ won't
  // perform an existential search for it), so we must limit the implicit
  // target types to a finite set that we can enumerate.
  //
  // At the moment the set of implicit target types consists of just
  // std::string_view (when it is available).
  struct NotStringView {};
  struct StringViewTypeChar {
    template <typename ValueType>
    using apply = std::basic_string_view<ValueType>;
  };
  struct StringViewTypeNone {
    template <typename>
    using apply = NotStringView;
  };
  template <typename ValueType>
  using StringViewTypeFunc = std::conditional_t<
      detail::range_is_char_type_v_<Iter>,
      StringViewTypeChar,
      StringViewTypeNone>;
  template <typename ValueType>
  struct StringViewType {
    using type =
        typename StringViewTypeFunc<ValueType>::template apply<ValueType>;
  };

  template <typename Target>
  struct IsConstructibleViaStringView
      : Conjunction<
            std::is_constructible<
                _t<StringViewType<value_type>>,
                Iter const&,
                size_type>,
            std::is_constructible<Target, _t<StringViewType<value_type>>>> {};

 public:
  /// explicit operator conversion to any compatible type
  ///
  /// A compatible type is one which is constructible with an iterator and a
  /// size (preferred), or a pair of iterators (fallback), passed by const-ref.
  ///
  /// Participates in overload resolution precisely when the target type is
  /// compatible. This allows std::is_constructible compile-time checks to work.
  template <
      typename Tgt,
      std::enable_if_t<
          std::is_constructible<Tgt, Iter const&, size_type>::value &&
              !IsConstructibleViaStringView<Tgt>::value,
          int> = 0>
  constexpr explicit operator Tgt() const noexcept(
      std::is_nothrow_constructible<Tgt, Iter const&, size_type>::value) {
    return Tgt(b_, walk_size());
  }
  template <
      typename Tgt,
      std::enable_if_t<
          !std::is_constructible<Tgt, Iter const&, size_type>::value &&
              std::is_constructible<Tgt, Iter const&, Iter const&>::value &&
              !IsConstructibleViaStringView<Tgt>::value,
          int> = 0>
  constexpr explicit operator Tgt() const noexcept(
      std::is_nothrow_constructible<Tgt, Iter const&, Iter const&>::value) {
    return Tgt(b_, e_);
  }

  /// implicit operator conversion to std::string_view
  template <
      typename Tgt,
      typename ValueType = value_type,
      std::enable_if_t<
          StrictConjunction<
              std::is_same<Tgt, _t<StringViewType<ValueType>>>,
              std::is_constructible<
                  _t<StringViewType<ValueType>>,
                  Iter const&,
                  size_type>>::value,
          int> = 0>
  constexpr operator Tgt() const noexcept(
      std::is_nothrow_constructible<Tgt, Iter const&, size_type>::value) {
    return Tgt(b_, walk_size());
  }

#if FMT_VERSION < 100000
  template <
      typename IterType = Iter,
      std::enable_if_t<detail::range_is_char_type_v_<IterType>, int> = 0>
  constexpr operator fmt::basic_string_view<value_type>() const noexcept {
    return _t<StringViewType<value_type>>(*this);
  }
#endif

  /// explicit non-operator conversion to any compatible type
  ///
  /// A compatible type is one which is constructible with an iterator and a
  /// size (preferred), or a pair of iterators (fallback), passed by const-ref.
  ///
  /// Participates in overload resolution precisely when the target type is
  /// compatible. This allows is_invocable compile-time checks to work.
  ///
  /// Provided in addition to the explicit operator conversion to permit passing
  /// additional arguments to the target type constructor. A canonical example
  /// of an additional argument might be an allocator, where the target type is
  /// some specialization of std::vector or std::basic_string in a context which
  /// requires a non-default-constructed allocator.
  template <typename Tgt, typename... Args>
  constexpr std::enable_if_t<
      std::is_constructible<Tgt, Iter const&, size_type>::value,
      Tgt>
  to(Args&&... args) const noexcept(
      std::is_nothrow_constructible<Tgt, Iter const&, size_type, Args&&...>::
          value) {
    return Tgt(b_, walk_size(), static_cast<Args&&>(args)...);
  }
  template <typename Tgt, typename... Args>
  constexpr std::enable_if_t<
      !std::is_constructible<Tgt, Iter const&, size_type>::value &&
          std::is_constructible<Tgt, Iter const&, Iter const&>::value,
      Tgt>
  to(Args&&... args) const noexcept(
      std::is_nothrow_constructible<Tgt, Iter const&, Iter const&, Args&&...>::
          value) {
    return Tgt(b_, e_, static_cast<Args&&>(args)...);
  }

  // Works only for Range<const char*> and Range<char*>
  std::string str() const { return to<std::string>(); }
  std::string toString() const { return to<std::string>(); }

  const_range_type castToConst() const { return const_range_type(*this); }

  int compare(const const_range_type& o) const {
    const size_type tsize = this->size();
    const size_type osize = o.size();
    const size_type msize = std::min(tsize, osize);
    int r = traits_type::compare(data(), o.data(), msize);
    if (r == 0 && tsize != osize) {
      // We check the signed bit of the subtraction and bit shift it
      // to produce either 0 or 2. The subtraction yields the
      // comparison values of either -1 or 1.
      r = (static_cast<int>((osize - tsize) >> (CHAR_BIT * sizeof(size_t) - 1))
           << 1) -
          1;
    }
    return r;
  }

  reference operator[](size_t i) {
    assert(i < size());
    return b_[i];
  }

  const_reference operator[](size_t i) const {
    assert(i < size());
    return b_[i];
  }

  reference at(size_t i) {
    if (i >= size()) {
      throw_exception<std::out_of_range>("index out of range");
    }
    return b_[i];
  }

  const_reference at(size_t i) const {
    if (i >= size()) {
      throw_exception<std::out_of_range>("index out of range");
    }
    return b_[i];
  }

  void advance(size_type n) {
    if (FOLLY_UNLIKELY(n > size())) {
      throw_exception<std::out_of_range>("index out of range");
    }
    b_ += n;
  }

  void subtract(size_type n) {
    if (FOLLY_UNLIKELY(n > size())) {
      throw_exception<std::out_of_range>("index out of range");
    }
    e_ -= n;
  }

  // Returns a window into the current range, starting at first, and spans
  // length characters (or until the end of the current range, whichever comes
  // first). Throws if first is past the end of the current range.
  Range subpiece(size_type first, size_type length = npos) const {
    if (FOLLY_UNLIKELY(first > size())) {
      throw_exception<std::out_of_range>("index out of range");
    }

    return Range(b_ + first, std::min(length, size() - first));
  }

  template <
      typename...,
      typename T = Iter,
      std::enable_if_t<detail::range_is_char_type_v_<T>, int> = 0>
  Range substr(size_type first, size_type length = npos) const {
    return subpiece(first, length);
  }

  // unchecked versions
  void uncheckedAdvance(size_type n) {
    assert(n <= size());
    b_ += n;
  }

  void uncheckedSubtract(size_type n) {
    assert(n <= size());
    e_ -= n;
  }

  Range uncheckedSubpiece(size_type first, size_type length = npos) const {
    assert(first <= size());
    return Range(b_ + first, std::min(length, size() - first));
  }

  void pop_front() {
    assert(b_ < e_);
    ++b_;
  }

  void pop_back() {
    assert(b_ < e_);
    --e_;
  }

  // string work-alike functions
  size_type find(const_range_type str) const {
    return qfind(castToConst(), str);
  }

  size_type find(const_range_type str, size_t pos) const {
    if (pos > size()) {
      return std::string::npos;
    }
    size_t ret = qfind(castToConst().subpiece(pos), str);
    return ret == npos ? ret : ret + pos;
  }

  size_type find(Iter s, size_t pos, size_t n) const {
    if (pos > size()) {
      return std::string::npos;
    }
    auto forFinding = castToConst();
    size_t ret = qfind(
        pos ? forFinding.subpiece(pos) : forFinding, const_range_type(s, n));
    return ret == npos ? ret : ret + pos;
  }

  // Works only for Range<(const) (unsigned) char*> which have Range(Iter) ctor
  size_type find(const Iter s) const {
    return qfind(castToConst(), const_range_type(s));
  }

  // Works only for Range<(const) (unsigned) char*> which have Range(Iter) ctor
  size_type find(const Iter s, size_t pos) const {
    if (pos > size()) {
      return std::string::npos;
    }
    size_type ret = qfind(castToConst().subpiece(pos), const_range_type(s));
    return ret == npos ? ret : ret + pos;
  }

  size_type find(const value_type& c) const { return qfind(castToConst(), c); }

  size_type rfind(const value_type& c) const {
    return folly::rfind(castToConst(), c);
  }

  size_type find(const value_type& c, size_t pos) const {
    if (pos > size()) {
      return std::string::npos;
    }
    size_type ret = qfind(castToConst().subpiece(pos), c);
    return ret == npos ? ret : ret + pos;
  }

  size_type find_first_of(const_range_type needles) const {
    return qfind_first_of(castToConst(), needles);
  }

  size_type find_first_of(const_range_type needles, size_t pos) const {
    if (pos > size()) {
      return std::string::npos;
    }
    size_type ret = qfind_first_of(castToConst().subpiece(pos), needles);
    return ret == npos ? ret : ret + pos;
  }

  // Works only for Range<(const) (unsigned) char*> which have Range(Iter) ctor
  size_type find_first_of(Iter needles) const {
    return find_first_of(const_range_type(needles));
  }

  // Works only for Range<(const) (unsigned) char*> which have Range(Iter) ctor
  size_type find_first_of(Iter needles, size_t pos) const {
    return find_first_of(const_range_type(needles), pos);
  }

  size_type find_first_of(Iter needles, size_t pos, size_t n) const {
    return find_first_of(const_range_type(needles, n), pos);
  }

  size_type find_first_of(const value_type& c) const { return find(c); }

  size_type find_first_of(const value_type& c, size_t pos) const {
    return find(c, pos);
  }

  /**
   * Determine whether the range contains the given subrange or item.
   *
   * Note: Call find() directly if the index is needed.
   */
  bool contains(const const_range_type& other) const {
    return find(other) != std::string::npos;
  }

  bool contains(const value_type& other) const {
    return find(other) != std::string::npos;
  }

  void swap(Range& rhs) {
    std::swap(b_, rhs.b_);
    std::swap(e_, rhs.e_);
  }

  /**
   * Does this Range start with another range?
   */
  bool startsWith(const const_range_type& other) const {
    return size() >= other.size() &&
        castToConst().subpiece(0, other.size()) == other;
  }
  bool startsWith(const value_type& c) const {
    return !empty() && front() == c;
  }

  template <class Comp>
  bool startsWith(const const_range_type& other, Comp&& eq) const {
    if (size() < other.size()) {
      return false;
    }
    auto const trunc = subpiece(0, other.size());
    return std::equal(
        trunc.begin(), trunc.end(), other.begin(), std::forward<Comp>(eq));
  }

  bool starts_with(const_range_type other) const noexcept {
    return startsWith(other);
  }
  bool starts_with(const value_type& c) const noexcept { return startsWith(c); }
  template <
      typename...,
      typename T = Iter,
      std::enable_if_t<detail::range_is_char_type_v_<T>, int> = 0>
  bool starts_with(const value_type* other) const {
    return startsWith(other);
  }

  /**
   * Does this Range end with another range?
   */
  bool endsWith(const const_range_type& other) const {
    return size() >= other.size() &&
        castToConst().subpiece(size() - other.size()) == other;
  }
  bool endsWith(const value_type& c) const { return !empty() && back() == c; }

  template <class Comp>
  bool endsWith(const const_range_type& other, Comp&& eq) const {
    if (size() < other.size()) {
      return false;
    }
    auto const trunc = subpiece(size() - other.size());
    return std::equal(
        trunc.begin(), trunc.end(), other.begin(), std::forward<Comp>(eq));
  }

  template <class Comp>
  bool equals(const const_range_type& other, Comp&& eq) const {
    return size() == other.size() &&
        std::equal(begin(), end(), other.begin(), std::forward<Comp>(eq));
  }

  bool ends_with(const_range_type other) const noexcept {
    return endsWith(other);
  }
  bool ends_with(const value_type& c) const noexcept { return endsWith(c); }
  template <
      typename...,
      typename T = Iter,
      std::enable_if_t<detail::range_is_char_type_v_<T>, int> = 0>
  bool ends_with(const value_type* other) const {
    return endsWith(other);
  }

  /**
   * Remove the items in [b, e), as long as this subrange is at the beginning
   * or end of the Range.
   *
   * Required for boost::algorithm::trim()
   */
  void erase(Iter b, Iter e) {
    if (b == b_) {
      b_ = e;
    } else if (e == e_) {
      e_ = b;
    } else {
      throw_exception<std::out_of_range>("index out of range");
    }
  }

  /**
   * Remove the given prefix and return true if the range starts with the given
   * prefix; return false otherwise.
   */
  bool removePrefix(const const_range_type& prefix) {
    return startsWith(prefix) && (b_ += prefix.size(), true);
  }
  bool removePrefix(const value_type& prefix) {
    return startsWith(prefix) && (++b_, true);
  }

  /**
   * Remove the given suffix and return true if the range ends with the given
   * suffix; return false otherwise.
   */
  bool removeSuffix(const const_range_type& suffix) {
    return endsWith(suffix) && (e_ -= suffix.size(), true);
  }
  bool removeSuffix(const value_type& suffix) {
    return endsWith(suffix) && (--e_, true);
  }

  /**
   * Replaces the content of the range, starting at position 'pos', with
   * contents of 'replacement'. Entire 'replacement' must fit into the
   * range. Returns false if 'replacements' does not fit. Example use:
   *
   * char in[] = "buffer";
   * auto msp = MutableStringPiece(input);
   * EXPECT_TRUE(msp.replaceAt(2, "tt"));
   * EXPECT_EQ(msp, "butter");
   *
   * // not enough space
   * EXPECT_FALSE(msp.replace(msp.size() - 1, "rr"));
   * EXPECT_EQ(msp, "butter"); // unchanged
   */
  bool replaceAt(size_t pos, const_range_type replacement) {
    if (size() < pos + replacement.size()) {
      return false;
    }

    std::copy(replacement.begin(), replacement.end(), begin() + pos);

    return true;
  }

  /**
   * Replaces all occurrences of 'source' with 'dest'. Returns number
   * of replacements made. Source and dest have to have the same
   * length. Throws if the lengths are different. If 'source' is a
   * pattern that is overlapping with itself, we perform sequential
   * replacement: "aaaaaaa".replaceAll("aa", "ba") --> "bababaa"
   *
   * Example use:
   *
   * char in[] = "buffer";
   * auto msp = MutableStringPiece(input);
   * EXPECT_EQ(msp.replaceAll("ff","tt"), 1);
   * EXPECT_EQ(msp, "butter");
   */
  size_t replaceAll(const_range_type source, const_range_type dest) {
    if (source.size() != dest.size()) {
      throw_exception<std::invalid_argument>(
          "replacement must have the same size as source");
    }

    if (dest.empty()) {
      return 0;
    }

    size_t pos = 0;
    size_t num_replaced = 0;
    size_type found = std::string::npos;
    while ((found = find(source, pos)) != std::string::npos) {
      replaceAt(found, dest);
      pos += source.size();
      ++num_replaced;
    }

    return num_replaced;
  }

  /**
   * Splits this `Range` `[b, e)` in the position `i` dictated by the next
   * occurrence of `delimiter`.
   *
   * Returns a new `Range` `[b, i)` and adjusts this range to start right after
   * the delimiter's position. This range will be empty if the delimiter is not
   * found. If called on an empty `Range`, both this and the returned `Range`
   * will be empty.
   *
   * Example:
   *
   *  folly::StringPiece s("sample string for split_next");
   *  auto p = s.split_step(' ');
   *
   *  // prints "string for split_next"
   *  cout << s << endl;
   *
   *  // prints "sample"
   *  cout << p << endl;
   *
   * Example 2:
   *
   *  void tokenize(StringPiece s, char delimiter) {
   *    while (!s.empty()) {
   *      cout << s.split_step(delimiter);
   *    }
   *  }
   *
   */
  Range split_step(const value_type& delimiter) {
    auto i = find(delimiter);
    Range result(b_, i == std::string::npos ? size() : i);

    b_ = result.end() == e_ ? e_ : std::next(result.end());

    return result;
  }

  Range split_step(Range delimiter) {
    auto i = find(delimiter);
    Range result(b_, i == std::string::npos ? size() : i);

    b_ = result.end() == e_
        ? e_
        : std::next(result.end(), difference_type(delimiter.size()));

    return result;
  }

  /**
   * Convenience method that calls `split_step()` and passes the result to a
   * functor, returning whatever the functor does. Any additional arguments
   * `args` passed to this function are perfectly forwarded to the functor.
   *
   * Say you have a functor with this signature:
   *
   *  Foo fn(Range r) { }
   *
   * `split_step()`'s return type will be `Foo`. It works just like:
   *
   *  auto result = fn(myRange.split_step(' '));
   *
   * A functor returning `void` is also supported.
   *
   * Example:
   *
   *  void do_some_parsing(folly::StringPiece s) {
   *    auto version = s.split_step(' ', [&](folly::StringPiece x) {
   *      if (x.empty()) {
   *        throw std::invalid_argument("empty string");
   *      }
   *      return std::strtoull(x.begin(), x.end(), 16);
   *    });
   *
   *    // ...
   *  }
   *
   *  struct Foo {
   *    void parse(folly::StringPiece s) {
   *      s.split_step(' ', parse_field, bar, 10);
   *      s.split_step('\t', parse_field, baz, 20);
   *
   *      auto const kludge = [](folly::StringPiece x, int &out, int def) {
   *        if (x == "null") {
   *          out = 0;
   *        } else {
   *          parse_field(x, out, def);
   *        }
   *      };
   *
   *      s.split_step('\t', kludge, gaz);
   *      s.split_step(' ', kludge, foo);
   *    }
   *
   *  private:
   *    int bar;
   *    int baz;
   *    int gaz;
   *    int foo;
   *
   *    static parse_field(folly::StringPiece s, int &out, int def) {
   *      try {
   *        out = folly::to<int>(s);
   *      } catch (std::exception const &) {
   *        value = def;
   *      }
   *    }
   *  };
   *
   */
  template <typename TProcess, typename... Args>
  auto split_step(
      const value_type& delimiter, TProcess&& process, Args&&... args)
      -> decltype(process(std::declval<Range>(), std::forward<Args>(args)...)) {
    return process(split_step(delimiter), std::forward<Args>(args)...);
  }

  template <typename TProcess, typename... Args>
  auto split_step(Range delimiter, TProcess&& process, Args&&... args)
      -> decltype(process(std::declval<Range>(), std::forward<Args>(args)...)) {
    return process(split_step(delimiter), std::forward<Args>(args)...);
  }

 private:
  Iter b_;
  Iter e_;
};

template <class Iter>
const typename Range<Iter>::size_type Range<Iter>::npos = std::string::npos;

template <class Iter>
void swap(Range<Iter>& lhs, Range<Iter>& rhs) {
  lhs.swap(rhs);
}

/**
 * Create a range from two iterators, with type deduction.
 */
template <class Iter>
constexpr Range<Iter> range(Iter first, Iter last) {
  return Range<Iter>(first, last);
}

/*
 * Creates a range to reference the contents of a contiguous-storage container.
 */
// Use pointers for types with '.data()' member
template <class Collection>
constexpr auto range(Collection& v) -> Range<decltype(v.data())> {
  return Range<decltype(v.data())>(v.data(), v.data() + v.size());
}
template <class Collection>
constexpr auto range(Collection const& v) -> Range<decltype(v.data())> {
  return Range<decltype(v.data())>(v.data(), v.data() + v.size());
}
template <class Collection>
constexpr auto crange(Collection const& v) -> Range<decltype(v.data())> {
  return Range<decltype(v.data())>(v.data(), v.data() + v.size());
}

template <class T, size_t n>
constexpr Range<T*> range(T (&array)[n]) {
  return Range<T*>(array, array + n);
}
template <class T, size_t n>
constexpr Range<T const*> range(T const (&array)[n]) {
  return Range<T const*>(array, array + n);
}
template <class T, size_t n>
constexpr Range<T const*> crange(T const (&array)[n]) {
  return Range<T const*>(array, array + n);
}

template <class T, size_t n>
constexpr Range<T*> range(std::array<T, n>& array) {
  return Range<T*>{array};
}
template <class T, size_t n>
constexpr Range<T const*> range(std::array<T, n> const& array) {
  return Range<T const*>{array};
}
template <class T, size_t n>
constexpr Range<T const*> crange(std::array<T, n> const& array) {
  return Range<T const*>{array};
}

template <class T>
constexpr Range<T const*> range(std::initializer_list<T> ilist) {
  return Range<T const*>(ilist.begin(), ilist.end());
}

template <class T>
constexpr Range<T const*> crange(std::initializer_list<T> ilist) {
  return Range<T const*>(ilist.begin(), ilist.end());
}

using StringPiece = Range<const char*>;
using MutableStringPiece = Range<char*>;
using ByteRange = Range<const unsigned char*>;
using MutableByteRange = Range<unsigned char*>;

template <class C>
std::basic_ostream<C>& operator<<(
    std::basic_ostream<C>& os, Range<C const*> piece) {
  using StreamSize = decltype(os.width());
  os.write(piece.start(), static_cast<StreamSize>(piece.size()));
  return os;
}

template <class C>
std::basic_ostream<C>& operator<<(std::basic_ostream<C>& os, Range<C*> piece) {
  using StreamSize = decltype(os.width());
  os.write(piece.start(), static_cast<StreamSize>(piece.size()));
  return os;
}

/**
 * Templated comparison operators
 */

template <class Iter>
inline bool operator==(const Range<Iter>& lhs, const Range<Iter>& rhs) {
  using value_type = typename Range<Iter>::value_type;
  if (lhs.size() != rhs.size()) {
    return false;
  }
  if constexpr (
      std::is_pointer_v<Iter> &&
      (std::is_integral_v<value_type> || std::is_enum_v<value_type>)) {
    auto const size = lhs.size() * sizeof(value_type);
    return 0 == size || 0 == std::memcmp(lhs.data(), rhs.data(), size);
  } else {
    for (size_t i = 0; i < lhs.size(); ++i) {
      if (!Range<Iter>::traits_type::eq(lhs[i], rhs[i])) {
        return false;
      }
    }
    return true;
  }
}

template <class Iter>
inline bool operator!=(const Range<Iter>& lhs, const Range<Iter>& rhs) {
  return !(operator==(lhs, rhs));
}

template <class Iter>
inline bool operator<(const Range<Iter>& lhs, const Range<Iter>& rhs) {
  return lhs.compare(rhs) < 0;
}

template <class Iter>
inline bool operator<=(const Range<Iter>& lhs, const Range<Iter>& rhs) {
  return lhs.compare(rhs) <= 0;
}

template <class Iter>
inline bool operator>(const Range<Iter>& lhs, const Range<Iter>& rhs) {
  return lhs.compare(rhs) > 0;
}

template <class Iter>
inline bool operator>=(const Range<Iter>& lhs, const Range<Iter>& rhs) {
  return lhs.compare(rhs) >= 0;
}

/**
 * Specializations of comparison operators for StringPiece
 */

namespace detail {

template <class A, class B>
struct ComparableAsStringPiece {
  enum {
    value = (std::is_convertible<A, StringPiece>::value &&
             std::is_same<B, StringPiece>::value) ||
        (std::is_convertible<B, StringPiece>::value &&
         std::is_same<A, StringPiece>::value)
  };
};

} // namespace detail

/**
 * operator== through conversion for Range<const char*>
 */
template <class T, class U>
std::enable_if_t<detail::ComparableAsStringPiece<T, U>::value, bool> operator==(
    const T& lhs, const U& rhs) {
  return StringPiece(lhs) == StringPiece(rhs);
}

/**
 * operator!= through conversion for Range<const char*>
 */
template <class T, class U>
std::enable_if_t<detail::ComparableAsStringPiece<T, U>::value, bool> operator!=(
    const T& lhs, const U& rhs) {
  return StringPiece(lhs) != StringPiece(rhs);
}

/**
 * operator< through conversion for Range<const char*>
 */
template <class T, class U>
std::enable_if_t<detail::ComparableAsStringPiece<T, U>::value, bool> operator<(
    const T& lhs, const U& rhs) {
  return StringPiece(lhs) < StringPiece(rhs);
}

/**
 * operator> through conversion for Range<const char*>
 */
template <class T, class U>
std::enable_if_t<detail::ComparableAsStringPiece<T, U>::value, bool> operator>(
    const T& lhs, const U& rhs) {
  return StringPiece(lhs) > StringPiece(rhs);
}

/**
 * operator< through conversion for Range<const char*>
 */
template <class T, class U>
std::enable_if_t<detail::ComparableAsStringPiece<T, U>::value, bool> operator<=(
    const T& lhs, const U& rhs) {
  return StringPiece(lhs) <= StringPiece(rhs);
}

/**
 * operator> through conversion for Range<const char*>
 */
template <class T, class U>
std::enable_if_t<detail::ComparableAsStringPiece<T, U>::value, bool> operator>=(
    const T& lhs, const U& rhs) {
  return StringPiece(lhs) >= StringPiece(rhs);
}

/**
 * Finds substrings faster than brute force by borrowing from Boyer-Moore
 */
template <class Iter, class Comp>
size_t qfind(const Range<Iter>& haystack, const Range<Iter>& needle, Comp eq) {
  // Don't use std::search, use a Boyer-Moore-like trick by comparing
  // the last characters first
  auto const nsize = needle.size();
  if (haystack.size() < nsize) {
    return std::string::npos;
  }
  if (!nsize) {
    return 0;
  }
  auto const nsize_1 = nsize - 1;
  auto const lastNeedle = needle[nsize_1];

  // Boyer-Moore skip value for the last char in the needle. Zero is
  // not a valid value; skip will be computed the first time it's
  // needed.
  std::string::size_type skip = 0;

  auto i = haystack.begin();
  auto iEnd = haystack.end() - nsize_1;

  while (i < iEnd) {
    // Boyer-Moore: match the last element in the needle
    while (!eq(i[nsize_1], lastNeedle)) {
      if (++i == iEnd) {
        // not found
        return std::string::npos;
      }
    }
    // Here we know that the last char matches
    // Continue in pedestrian mode
    for (size_t j = 0;;) {
      assert(j < nsize);
      if (!eq(i[j], needle[j])) {
        // Not found, we can skip
        // Compute the skip value lazily
        if (skip == 0) {
          skip = 1;
          while (skip <= nsize_1 && !eq(needle[nsize_1 - skip], lastNeedle)) {
            ++skip;
          }
        }
        i += skip;
        break;
      }
      // Check if done searching
      if (++j == nsize) {
        // Yay
        return size_t(i - haystack.begin());
      }
    }
  }
  return std::string::npos;
}

namespace detail {

inline size_t qfind_first_byte_of(
    const StringPiece haystack, const StringPiece needles) {
  // Let's default to the SIMD implementation. Internally, if that's not
  // available, the _nosimd version gets picked instead.
  return qfind_first_byte_of_simd(haystack, needles);
}

} // namespace detail

template <class Iter, class Comp>
size_t qfind_first_of(
    const Range<Iter>& haystack, const Range<Iter>& needles, Comp eq) {
  auto ret = std::find_first_of(
      haystack.begin(), haystack.end(), needles.begin(), needles.end(), eq);
  return ret == haystack.end() ? std::string::npos : ret - haystack.begin();
}

struct AsciiCaseSensitive {
  bool operator()(char lhs, char rhs) const { return lhs == rhs; }
};

/**
 * Check if two ascii characters are case insensitive equal.
 * The difference between the lower/upper case characters are the 6-th bit.
 * We also check they are alpha chars, in case of xor = 32.
 */
struct AsciiCaseInsensitive {
  bool operator()(char lhs, char rhs) const {
    char k = lhs ^ rhs;
    if (k == 0) {
      return true;
    }
    if (k != 32) {
      return false;
    }
    k = lhs | rhs;
    return (k >= 'a' && k <= 'z');
  }
};

template <class Iter>
size_t qfind(
    const Range<Iter>& haystack,
    const typename Range<Iter>::value_type& needle) {
  auto pos = std::find(haystack.begin(), haystack.end(), needle);
  return pos == haystack.end() ? std::string::npos : pos - haystack.data();
}

template <class Iter>
size_t rfind(
    const Range<Iter>& haystack,
    const typename Range<Iter>::value_type& needle) {
  for (auto i = haystack.size(); i-- > 0;) {
    if (haystack[i] == needle) {
      return i;
    }
  }
  return std::string::npos;
}

// specialization for StringPiece
template <>
inline size_t qfind(const Range<const char*>& haystack, const char& needle) {
  // memchr expects a not-null pointer, early return if the range is empty.
  if (haystack.empty()) {
    return std::string::npos;
  }
  auto pos = static_cast<const char*>(
      ::memchr(haystack.data(), needle, haystack.size()));
  return pos == nullptr ? std::string::npos : pos - haystack.data();
}

template <>
inline size_t rfind(const Range<const char*>& haystack, const char& needle) {
  // memchr expects a not-null pointer, early return if the range is empty.
  if (haystack.empty()) {
    return std::string::npos;
  }
  auto pos = static_cast<const char*>(
      memrchr(haystack.data(), needle, haystack.size()));
  return pos == nullptr ? std::string::npos : pos - haystack.data();
}

// specialization for ByteRange
template <>
inline size_t qfind(
    const Range<const unsigned char*>& haystack, const unsigned char& needle) {
  // memchr expects a not-null pointer, early return if the range is empty.
  if (haystack.empty()) {
    return std::string::npos;
  }
  auto pos = static_cast<const unsigned char*>(
      ::memchr(haystack.data(), needle, haystack.size()));
  return pos == nullptr ? std::string::npos : pos - haystack.data();
}

template <>
inline size_t rfind(
    const Range<const unsigned char*>& haystack, const unsigned char& needle) {
  // memchr expects a not-null pointer, early return if the range is empty.
  if (haystack.empty()) {
    return std::string::npos;
  }
  auto pos = static_cast<const unsigned char*>(
      memrchr(haystack.data(), needle, haystack.size()));
  return pos == nullptr ? std::string::npos : pos - haystack.data();
}

template <class Iter>
size_t qfind_first_of(const Range<Iter>& haystack, const Range<Iter>& needles) {
  return qfind_first_of(haystack, needles, AsciiCaseSensitive());
}

// specialization for StringPiece
template <>
inline size_t qfind_first_of(
    const Range<const char*>& haystack, const Range<const char*>& needles) {
  return detail::qfind_first_byte_of(haystack, needles);
}

// specialization for ByteRange
template <>
inline size_t qfind_first_of(
    const Range<const unsigned char*>& haystack,
    const Range<const unsigned char*>& needles) {
  return detail::qfind_first_byte_of(
      StringPiece(haystack), StringPiece(needles));
}

template <class Key, class Enable>
struct hasher;

template <class T>
struct hasher<
    folly::Range<T*>,
    std::enable_if_t<std::is_integral<T>::value, void>> {
  using folly_is_avalanching = std::true_type;

  size_t operator()(folly::Range<T*> r) const {
    // std::is_integral<T> is too restrictive, but is sufficient to
    // guarantee we can just hash all of the underlying bytes to get a
    // suitable hash of T.  Something like absl::is_uniquely_represented<T>
    // would be better.  std::is_pod is not enough, because POD types
    // can contain pointers and padding.  Also, floating point numbers
    // may be == without being bit-identical.  size_t is less than 64
    // bits on some platforms.
    return static_cast<size_t>(
        hash::SpookyHashV2::Hash64(r.begin(), r.size() * sizeof(T), 0));
  }
};

/**
 * _sp is a user-defined literal suffix to make an appropriate Range
 * specialization from a literal string.
 *
 * Modeled after C++17's `sv` suffix.
 */
inline namespace literals {
inline namespace string_piece_literals {
constexpr Range<char const*> operator""_sp(
    char const* str, size_t len) noexcept {
  return Range<char const*>(str, len);
}

#if defined(__cpp_char8_t) && __cpp_char8_t >= 201811L
constexpr Range<char8_t const*> operator""_sp(
    char8_t const* str, size_t len) noexcept {
  return Range<char8_t const*>(str, len);
}
#endif

constexpr Range<char16_t const*> operator""_sp(
    char16_t const* str, size_t len) noexcept {
  return Range<char16_t const*>(str, len);
}

constexpr Range<char32_t const*> operator""_sp(
    char32_t const* str, size_t len) noexcept {
  return Range<char32_t const*>(str, len);
}

constexpr Range<wchar_t const*> operator""_sp(
    wchar_t const* str, size_t len) noexcept {
  return Range<wchar_t const*>(str, len);
}
} // namespace string_piece_literals
} // namespace literals

} // namespace folly

// Avoid ambiguity in older fmt versions due to StringPiece's conversions.
#if FMT_VERSION >= 70000
namespace fmt {
template <>
struct formatter<folly::StringPiece> : private formatter<string_view> {
  using formatter<string_view>::parse;

#if FMT_VERSION >= 80000
  template <typename Context>
  typename Context::iterator format(folly::StringPiece s, Context& ctx) const {
    return formatter<string_view>::format({s.data(), s.size()}, ctx);
  }
#else
  template <typename Context>
  typename Context::iterator format(folly::StringPiece s, Context& ctx) {
    return formatter<string_view>::format({s.data(), s.size()}, ctx);
  }
#endif
};
} // namespace fmt
#endif

FOLLY_POP_WARNING

FOLLY_ASSUME_FBVECTOR_COMPATIBLE_1(folly::Range)

// Unfortunately it is not possible to forward declare enable_view under
// MSVC 2019.8 due to compiler bugs, so we need to include the actual
// definition if available.
#if __has_include(<range/v3/range/concepts.hpp>) && defined(_MSC_VER)
#include <range/v3/range/concepts.hpp> // @manual
#else
namespace ranges {
template <class T>
extern const bool enable_view;
} // namespace ranges
#endif

// Tell the range-v3 library that this type should satisfy
// the view concept (a lightweight, non-owning range).
namespace ranges {
template <class Iter>
inline constexpr bool enable_view<::folly::Range<Iter>> = true;
} // namespace ranges

#if defined(__cpp_lib_ranges)
template <typename T>
constexpr bool std::ranges::enable_borrowed_range<folly::Range<T>> = true;
#endif

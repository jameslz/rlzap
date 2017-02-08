#include <memory>
#include <vector>

#include <lcp/index.hpp>
#include <lcp/parse.hpp>

#include <alphabet.hpp>
#include <impl/differential_iterator.hpp>
#include <get_matchings.hpp>

#include "serialize.hpp"
#include "include/parse_coord_common.hpp"
#include "main.hpp"

using Alphabet = rlz::alphabet::lcp_32;
using Symbol   = Alphabet::Symbol;

template <typename Index, typename Length>
class Instantiate {
  ds_getter<Length::value()> getter;
public:
  Index operator()()
  {
    using SignedAlphabet = rlz::alphabet::SignLcp<Alphabet>;
    using SignSymbol     = SignedAlphabet::Symbol;
    using InputIt = Symbol*;
    using RefIt   = Symbol*;

    // Get data
    using DiffIt = rlz::differential_iterator<InputIt, SignSymbol>;
    using Dumper = rlz::utils::iterator_dumper<DiffIt>;

    auto &input      = getter.input();
    auto &reference  = getter.reference();

    auto ref_beg     = reference.data();
    auto ref_end     = reference.data() + reference.size();
    auto input_beg   = input.data();
    auto input_end   = input.data() + input.size();

    Dumper ref_dumper{DiffIt(ref_beg), DiffIt(ref_end)},
           input_dumper{DiffIt(input_beg), DiffIt(input_end)};


    auto matches = std::get<0>(
      rlz::get_relative_matches<SignedAlphabet>(ref_dumper, input_dumper)
    );

    // Build it
    using Builder = typename Index::template builder<InputIt>;
    using rlz::lcp::build::observer;
    using rlz::lcp::build::coordinator;
    auto build = std::make_shared<Builder>();
    std::vector<std::shared_ptr<observer<Alphabet, InputIt>>> v {{ build }};
    coordinator<Alphabet, InputIt, RefIt> coord(
      v.begin(), v.end(), input_beg, ref_beg
    );

    auto lit_func = [&] (std::size_t pos, std::size_t length) {
      coord.literal_evt(pos, length);
    };

    auto copy_func = [&] (std::size_t pos, std::size_t ref, std::size_t len) {
      coord.copy_evt(pos, ref, len);
    };

    auto end_func = [&] () {
      coord.end_evt();
    };

    rlz::lcp::parser parse;

    parse(
      matches.begin(), matches.end(),
      input_beg, input_end,
      ref_beg, ref_end,
      lit_func, copy_func, end_func
    );

    return build->get(reference);
  }
};

template <typename Index, typename Length>
class Serialize {
  Instantiate<Index, Length> instantiate;
public:
  Index operator()()
  {
    auto idx    = instantiate();
    auto loaded = load_unload(idx);
    loaded.set_source(ds_getter<Length::value()>{}.reference());
    return loaded;
  }
};

template <typename A, typename B, template <typename, typename> class C>
struct type_pack { };

template <typename TypePack>
struct unpack
{ };

template <typename IndexT, typename LengthT, template <typename, typename> class GetterT>
struct unpack<type_pack<IndexT, LengthT, GetterT>>
{
  using Index  = IndexT;
  using Length = LengthT;
  using Getter = GetterT<Index, Length>;
};

template <typename TypeList>
class LcpIndex : public ::testing::Test {
  using Index = typename unpack<TypeList>::Index;
  using Length = typename unpack<TypeList>::Length;
  using Getter = typename unpack<TypeList>::Getter;
public:
  Index get()
  {
    return Getter{}();
  }

  const std::vector<Symbol> &reference()
  {
    return ds_getter<Length::value()>{}.reference();
  }

  const std::vector<Symbol> &input()
  {
    return ds_getter<Length::value()>{}.input();
  }
};

using TestedIndex = rlz::lcp::index<Alphabet, std::vector<Symbol>>;

using IndexTypes = ::testing::Types<
  type_pack<TestedIndex, rlz::values::Size<    128>, Instantiate>,
  type_pack<TestedIndex, rlz::values::Size<   1024>, Instantiate>,
  type_pack<TestedIndex, rlz::values::Size<   2048>, Instantiate>,
  type_pack<TestedIndex, rlz::values::Size<   4096>, Instantiate>,
  type_pack<TestedIndex, rlz::values::Size<1048576>, Instantiate>,
  type_pack<TestedIndex, rlz::values::Size<    128>,   Serialize>,
  type_pack<TestedIndex, rlz::values::Size<   1024>,   Serialize>,
  type_pack<TestedIndex, rlz::values::Size<   2048>,   Serialize>,
  type_pack<TestedIndex, rlz::values::Size<   4096>,   Serialize>,
  type_pack<TestedIndex, rlz::values::Size<1048576>,   Serialize>
>;
TYPED_TEST_CASE(LcpIndex, IndexTypes);

TYPED_TEST(LcpIndex, Size)
{
  auto idx = this->get();
  ASSERT_EQ(idx.size(), this->input().size());
}

TYPED_TEST(LcpIndex, Access)
{
  auto idx    = this->get();
  auto &input = this->input();
  for (auto i = 0U; i < input.size(); ++i) {
    ASSERT_EQ(input[i], idx(i));
  }
}

TYPED_TEST(LcpIndex, Range)
{
  auto idx    = this->get();
  auto &input = this->input();
  std::vector<std::size_t> ranges {{ 1UL, 2UL, 4UL, 8UL, 16UL }};
  std::vector<Symbol> cont(ranges.back());
  for (auto start = 0U; start + ranges.back() < input.size(); ++start) {
    for (auto range : ranges) {
      idx(start, start + range, cont.begin());
      ASSERT_TRUE(std::equal(
        cont.begin(),
        cont.begin() + range,
        input.begin() + start
      ));
    }
  }
}

TYPED_TEST(LcpIndex, Iterator)
{
  using Index    = typename unpack<TypeParam>::Index;
  using iterator = typename Index::iter;
  auto idx    = this->get();
  auto &input = this->input();

  iterator start_it, it, end_it;

  std::vector<int> jumps {{ -16, -8, -4, -2, -1, 1, 2, 4, 8, 16 }};

  for (auto start = 0U; start < input.size(); ++start) {
    std::tie(start_it, it, end_it) = idx.iterator_position(start);

    auto phrase_len   = std::distance(start_it, end_it);
    auto displacement = std::distance(start_it, it);
    auto left         = std::distance(it, end_it);


    ASSERT_EQ(displacement + left, phrase_len);
    ASSERT_LE(displacement, start);
    ASSERT_LE(start + std::distance(it, end_it), input.size());
    ASSERT_LE(displacement, phrase_len);
    ASSERT_LE(left, phrase_len);

    ASSERT_EQ(*it, input[start]);
    
    auto phrase_start = start - displacement;
    auto phrase_end   = start + left;
    for (auto jump : jumps) {
      if ((static_cast<int>(start) + jump >= phrase_start) and (static_cast<int>(start) + jump < phrase_end)) {
        auto jumped = std::next(it, jump);
        auto obt    = *jumped;
        auto val    = input[start + jump];
        ASSERT_EQ(obt, val);
      }
    }
  }
}
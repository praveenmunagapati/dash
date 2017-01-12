#include <libdash.h>

#include "../bench.h"

#include <array>
#include <vector>
#include <algorithm>
#include <deque>
#include <iostream>
#include <iomanip>
#include <unistd.h>

using std::cout;
using std::endl;

typedef dash::util::Timer<
          dash::util::TimeMeasure::Clock
        > Timer;

#ifndef TYPE
#define TYPE int
#endif

typedef dash::TilePattern<
  1,
  dash::ROW_MAJOR,
  int
> TilePattern_t;

typedef dash::Pattern<
  1,
  dash::ROW_MAJOR,
  int
> BlockPattern_t;

typedef dash::Array<
  TYPE,
  int,
  TilePattern_t
> ArrayTiledDist_t;

typedef dash::Array<
  TYPE,
  int,
  BlockPattern_t
> ArrayBlockedDist_t;

template<typename Iter>
void init_values(Iter begin, Iter end, unsigned);

template<class ArrayType>
double test_view_gups(ArrayType & a, unsigned, unsigned);

template<class ArrayType>
double test_algo_gups(ArrayType & a, unsigned, unsigned);

void perform_test(unsigned ELEM_PER_UNIT, unsigned REPEAT);

double gups(
  /// Number of units
  unsigned N,
  /// Duration in microseconds
  double   useconds,
  /// Elements per unit
  unsigned ELEM_PER_UNIT,
  /// Number of iterations
  unsigned REPEAT)
{
  double num_derefs_per_unit =
    static_cast<double>(N * ELEM_PER_UNIT * REPEAT);
  // kilo-updates / usecs = giga-updates / sec
  return N * (num_derefs_per_unit / 1000.0f) / useconds;
}

int main(int argc, char* argv[]) {
  dash::init(&argc, &argv);

  Timer::Calibrate(0);

  std::deque<std::pair<int, int>> tests;

  tests.push_back({0          , 0}); // this prints the header
  tests.push_back({4          , 1});
#if 0
  tests.push_back({4          , 1000000});
  tests.push_back({16         , 100000});
  tests.push_back({64         , 100000});
  tests.push_back({256        , 10000});
  tests.push_back({1024       , 10000});
  tests.push_back({4096       , 1000});
  tests.push_back({4*4096     , 500});
  tests.push_back({16*4096    , 100});
  tests.push_back({64*4096    , 50});
#endif
  for (auto test: tests) {
    perform_test(test.first, test.second);
  }

  dash::finalize();

  return 0;
}

void perform_test(
  unsigned ELEM_PER_UNIT,
  unsigned REPEAT) {
  auto num_units = dash::size();
  if (ELEM_PER_UNIT == 0) {
    if (dash::myid() == 0) {
      cout << std::setw(10)
           << "units"
           << ", "
           << std::setw(10)
           << "elem/unit"
           << ", "
           << std::setw(10)
           << "iterations"
           << ", "
           << std::setw(11)
           << "lrange.view"
           << ", "
           << std::setw(11)
           << "lrange.algo"
           << endl;
    }
    return;
  }

  std::vector<unsigned> local_sizes;
  for (size_t u = 0; u < num_units; ++u) {
    local_sizes.push_back(ELEM_PER_UNIT);
  }

  ArrayBlockedDist_t arr_blocked_dist(
    // Total number of elements
    ELEM_PER_UNIT * num_units
  );

  ArrayTiledDist_t arr_tiled_dist(
    // Total number of elements
    ELEM_PER_UNIT * num_units,
    // 1-dimensional distribution
    dash::DistributionSpec<1>(
      dash::TILE(ELEM_PER_UNIT))
  );

  double t_view = test_view_gups(arr_blocked_dist, ELEM_PER_UNIT, REPEAT);
  double t_algo = test_algo_gups(arr_blocked_dist, ELEM_PER_UNIT, REPEAT);

  dash::barrier();

  if (dash::myid() == 0) {
      auto lbegin_gidx = arr_blocked_dist.pattern().global(0);
      auto lrange = dash::index(
                  //  dash::local(
                        dash::sub(
                          lbegin_gidx + 0,
                          lbegin_gidx + 2,
                          arr_blocked_dist)
                  //    )
                      );

      auto lrange_begin = dash::begin(lrange);
      auto lrange_end   = dash::end(lrange);

      std::cout << "lrange { " << *lrange_begin << ", "
                               << *lrange_end   << " }"
                << std::endl;
  }
  return;

  if (dash::myid() == 0) {
    double gups_view = gups(num_units, t_view, ELEM_PER_UNIT, REPEAT);
    double gups_algo = gups(num_units, t_algo, ELEM_PER_UNIT, REPEAT);

    cout << std::setw(10)
         << num_units
         << ", "
         << std::setw(10)
         << ELEM_PER_UNIT
         << ", "
         << std::setw(10)
         << REPEAT
         << ", "
         << std::setw(11) << std::fixed << std::setprecision(4)
         << gups_view
         << ", "
         << std::setw(11) << std::fixed << std::setprecision(4)
         << gups_algo
         << endl;
  }
}

template<typename Iter>
void init_values(
  Iter begin,
  Iter end,
  unsigned ELEM_PER_UNIT) {
  auto i = 0;
  for(auto it = begin; it != end; ++it, ++i) {
    *it = i;
  }
}

template <class ArrayType>
double test_view_gups(
  ArrayType & a,
  unsigned ELEM_PER_UNIT,
  unsigned REPEAT)
{
  typedef typename ArrayType::pattern_type pattern_t;
  typedef typename ArrayType::value_type       val_t;
  typedef typename ArrayType::index_type     index_t;
  typedef typename ArrayType::local_type     local_t;
  auto & pattern = a.pattern();

  init_values(a.lbegin(), a.lend(), ELEM_PER_UNIT);

  auto lbegin_gidx = a.pattern().global(0);

  auto a_size   = a.size();
  auto ts_start = Timer::Now();
  auto myid     = pattern.team().myid();

  for (auto i = 0; i < REPEAT; ++i) {
    for (auto lidx = 1; lidx < a.lsize(); ++lidx) {
      auto lrange       = dash::index(
                            dash::local(
                              dash::sub(
                                lbegin_gidx + 0,
                                lbegin_gidx + lidx,
                                a) ) );
      auto lrange_begin = *dash::begin(lrange);
      auto lrange_end   = *dash::end(lrange);
      if (lrange_begin > lrange_end) {
        throw std::runtime_error("invalid range");
      }
    }
  }
  return Timer::ElapsedSince(ts_start);
}

template <class ArrayType>
double test_algo_gups(
  ArrayType & a,
  unsigned ELEM_PER_UNIT,
  unsigned REPEAT)
{
  typedef typename ArrayType::pattern_type pattern_t;
  typedef typename ArrayType::value_type       val_t;
  typedef typename ArrayType::index_type     index_t;
  typedef typename ArrayType::local_type     local_t;
  auto & pattern = a.pattern();

  init_values(a.lbegin(), a.lend(), ELEM_PER_UNIT);

  auto lbegin_gidx = a.pattern().global(0);

  auto a_size   = a.size();
  auto ts_start = Timer::Now();
  auto myid     = pattern.team().myid();
  for (auto i = 0; i < REPEAT; ++i) {
    for (auto lidx = 1; lidx < a.lsize(); ++lidx) {
      auto lrange       = dash::local_index_range(
                            a.begin() + lbegin_gidx,
                            a.begin() + lbegin_gidx + lidx
                          );
      auto lrange_begin = lrange.begin;
      auto lrange_end   = lrange.end;
      if (lrange_begin > lrange_end) {
        throw std::runtime_error("invalid range");
      }
    }
  }
  return Timer::ElapsedSince(ts_start);
}



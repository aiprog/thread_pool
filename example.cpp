#include <random>
#include <numeric>
#include <chrono>
#include <cassert>

#include "thread_pool.hpp"

using namespace std;
using namespace chrono;
using iret::thread_pool;

void quick_sort(thread_pool &tp, int *beg, int *end)
{
  auto len = end - beg;
  if (len < 100000)
  {
    sort(beg, end);
    return;
  }

  int val = *beg;
  auto mid = partition(beg + 1, end, [&](int &v) { return v < val; });
  swap(*beg, *(mid - 1));

  tp.submit(quick_sort, ref(tp), beg, mid - 1);
  tp.submit(quick_sort, ref(tp), mid, end);
}

constexpr auto NUM = 100000000;

int arr[NUM];

int main()
{
  iota(begin(arr), end(arr), 1);
  shuffle(begin(arr), end(arr), mt19937(random_device()()));

  {
    thread_pool tp(6);
    quick_sort(tp, begin(arr), end(arr));

    this_thread::sleep_for(1s);
    tp.resize(10);
    this_thread::sleep_for(1s);
    tp.resize(3);
    this_thread::sleep_for(1s);

    while (tp.run_one(false) != thread_pool::run_state::empty)
      ;
  }

  for (int i = 0; i < NUM; ++i)
    assert(arr[i] == i + 1);

  return 0;
}
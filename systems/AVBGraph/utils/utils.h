//
// Created by per on 31.08.20.
//

#ifndef LIVE_GRAPH_TWO_UTILS_H
#define LIVE_GRAPH_TWO_UTILS_H

#include <assert.h>

#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "HALMemoryPool/BlockAllocator.hpp"
#include "data-structure/data_types.h"

// Default is fine-grained; AVB_COARSE selects the historical coarse mode.
#ifndef AVB_COARSE
#define FINEGRAIN
#endif

// BUG: little MATCHTXNNUM (such as 10) will cause too many txn-block, insulting
// sink and transform error

// #define DEBUG true
// #define OUTPUT true
// #define MID true

#if MID == true
#define BATCHTXNNUM 10
#else
#if DEBUG == true
#define BATCHTXNNUM 1000
#else
#define BATCHTXNNUM 10000000
#endif
#endif

#define MAXSIMULBATCH 16
#define MODPM 15
#define READ_TIMES 1
#define INITIAL_BLOCK_CAPACITY_FOR_TEST 4096
#define WRITE_THREAD 64

extern atomic<long long> count_ll;
extern atomic<int> count_x, count_y, count_vb;
extern MemoryAllocator* la;
extern thread_local int threadid;
extern int xxx, count_vb_n;

// GC watermark: the oldest epoch still needed by any reader (lagging is fine —
// it only makes merge drop fewer deleted edges, never a wrong one). Advanced by
// AllVBManager::GC(); read by VertexEdges::merge_tmpev_with_eb().
extern std::atomic<epoch_t> g_oldest_epoch;

// #define TVB_STATS  // uncomment for TVB scan statistics
// #define PR_DEBUG   // uncomment for PageRank per-iteration debug output
// #define AVB_LEGACY_RESULT_OUTPUT  // standalone algorithm result files
#ifdef TVB_STATS
extern std::atomic<long long> tvb_scan_total;
extern std::atomic<long long> tvb_scan_vertices;
extern std::atomic<long long> tvb_record_total;
#endif

bool file_exists(const string& name);
bool endsWith(const string& fullString, const string& ending);

string string_join(const string& join, const vector<string>& list);

template <typename n>
n sum(const vector<n>& v) {
  n s = 0;
  for (const auto& l : v) {
    s += l;
  }
  return s;
}

template <typename k, typename v>
unordered_map<v, k> reverse_map(unordered_map<k, v> mapping) {
  unordered_map<v, k> out;
  for (const auto& kv : mapping) {
    out.insert({kv.second, kv.first});
  }
  return out;
}

string get_filename(string path);

string get_home_dir();

void intersect_edge_block(dst_t* start_a, dst_t* end_a, dst_t* start_b,
                          dst_t* end_b, vector<dst_t>& out);

template <typename K, typename V>
unordered_set<V> get_values_from_multimap(unordered_multimap<K, V> map, K key) {
  unordered_set<V> r;
  r.reserve(map.count(key));

  auto range = map.equal_range(key);

  auto i = range.first;
  while (i != range.second) {
    r.insert(i->second);
    i++;
  }
  return r;
}

uint round_up_power_of_two(uint v);

/**
 * Source:
 * https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring
 */

// trim from start (in place)
static inline void ltrim(std::string& s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
          }));
}

// trim from end (in place)
static inline void rtrim(std::string& s) {
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](unsigned char ch) { return !std::isspace(ch); })
              .base(),
          s.end());
}

// trim from both ends (in place)
static inline void trim(std::string& s) {
  ltrim(s);
  rtrim(s);
}

class Satistical {
 public:
  long long record_cnt = 0;
  long long visible_record_cnt = 0;
  Satistical() {
    record_cnt = 0;
    visible_record_cnt = 0;
  }

  void reach_record() { record_cnt++; }
  void reach_visible() { visible_record_cnt++; }

  void reset() {
    visible_record_cnt = 0;
    record_cnt = 0;
  }
};

#endif  // LIVE_GRAPH_TWO_UTILS_H

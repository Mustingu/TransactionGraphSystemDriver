#ifndef ALLVBMANAGER_H
#define ALLVBMANAGER_H

#include <assert.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "RWSpinLock.h"
#include "Transaction/Transaction.h"
#include "VersionBlock/VertexVersionBlock/VersionBlockManager.h"
// #include "boost/asio.hpp"
#include "tbb/concurrent_unordered_map.h"
#include "tbb/concurrent_vector.h"

#define NOT_ACTIVE numeric_limits<epoch_t>::max()
// #define SPIN_LOCK

class XorShiftRandom {
 private:
  thread_local static uint64_t state;

 public:
  static void seed(uint64_t s);

  static uint32_t rand_int(uint32_t max = 100000);
};

class VBData : public VBDataInterface {
 public:
  VBData(epoch_t epoch) : epoch_(epoch), first_committed_(false), txn_num_(0) {}
  ~VBData() {
    int n = this_vb_vector_.size();
    for (int i = 0; i < n; i++) {
      if (version_block_[i].start_ != nullptr)
        delete[] version_block_[i].start_;
    }
    free(version_block_);
  }

  unsigned get_txn_size() const { return txn_num_; }
  unsigned add_txn() { return txn_num_.fetch_add(1); }
  unsigned add_txn2() { return finish_num_.fetch_add(1); }

  // Fine-grained: per-epoch intra-epoch timestamp counter
  intra_t fetch_add_intra() { return intra_counter_.fetch_add(1) + 1; }
  intra_t get_intra_counter() const { return intra_counter_.load(); }
  bool get_first_committed() const { return first_committed_; }
  epoch_t get_epoch() const { return epoch_; }
  // bool all_commit() const {
  //   return all_flag && committed_txn_num_ == BATCHTXNNUM;
  // }
  // void erase_txn(Transaction* txn) { txns_.erase(txn); }

  void registerVersionBlock(TmpVersionBlock* vb) { vb_vector_.push_back(vb); }

  unsigned deregisterTransaction(Transaction* txn);

  void VB_pushback(TmpVersionBlock* vb, unsigned threadid) {
    vb_vector[threadid].push_back(vb);
  }
  bool commitVersionBlock();
  void lock_versionblock();
  // void set_all_flag() { all_flag = true; }

  std::vector<TmpVersionBlock*> this_vb_vector_;
  tbb::concurrent_vector<TmpVersionBlock*> vb_vector_;
  std::vector<TmpVersionBlock*> vb_vector[65];

 private:
  bool first_committed_;
  VersionBlock* version_block_;

  std::atomic_flag has_trans = ATOMIC_FLAG_INIT;
  std::atomic<unsigned> txn_num_;
  std::atomic<unsigned> finish_num_;
  std::atomic<intra_t> intra_counter_{0};  // fine-grained intra-epoch counter
  // std::atomic<unsigned> committed_txn_num_;
  epoch_t epoch_;

  // TODO: remove lock:
  // 1. tbb::mutex + unordered_set;
  // 2. tbb::concurrent_unordered_set
  // tbb::concurrent_vector<Transaction*> txns_;
  tbb::concurrent_vector<VersionData*> VersionData_ptr_;
  // tbb::internal::vector_iterator<tbb::concurrent_vector<VersionBlock*>,
  //                                VersionBlock*>
  //     vb_vector_end;
  // std::vector<VersionBlock*> vb_vector_2;

  // TODO: chang to RDTSC (fastest but may cause problem) or
  // time.h::clock_gettime (a little faster than chrono)
  std::chrono::_V2::system_clock::time_point first_commit_time_;
};

class AllVBManager {
 public:
  bool start_count = false;
  AllVBManager(unsigned max_threads, bool on);
  ~AllVBManager();

  uint64_t test1();
  void test2();

  void registerTransaction(Transaction* txn);
  void registerROTransaction(Transaction* txn, int thread_id_ = -1);
  void deregisterROTransaction(int thread_id_ = -1);
  void deregisterTransaction();

  void commitVersionBlock(epoch_t cur);
  void NoMoreTxn();
  // Close the write phase and synchronously publish every remaining epoch.
  // Static evaluation uses this instead of polling an asynchronous updater,
  // whose VB transform can legitimately take longer than a short timeout.
  void FinalizeNoMoreTxn();
  void P_timess() { std::cout << timess << '\n'; }

  epoch_t getCurrentEpoch() const { return cur_; }
  epoch_t getCurrentReadEpoch() {
#ifdef SPIN_LOCK
    std::unique_lock<RWSpinLock> lock(mtx_);
#else
    std::unique_lock<std::mutex> lock(mtx_);
#endif
    return read_epoch_;
  }

  void reset_max_threads(uint max_threads);

  /**
   * Registers a thread with the transaction manager.
   * @param id the id to use out of the dense domain of 0 to max_threads
   * @throws IllegalOperation if the thread id is already taken.
   */
  void register_thread(size_t id, bool on = true);

  /**
   * Deregisters a thread with the transaction manager.
   * @param id the id to use out of the dense domain of 0 to max_threads
   * @throws IllegalOperation if the thread id is not used taken.
   */
  void deregister_thread(size_t id);
  epoch_t getMinActiveVersion();

  void GC();
  void update_min_version();
  void run_min_epoch_updater(uint interval);

  std::atomic<bool> stopped{0}, last_epoch_has_trans{0};

 private:
  volatile epoch_t cur_;
  std::atomic<bool> no_more_txn_{false};
  static const unsigned max_txn_num_invb_ =
      BATCHTXNNUM - 1;  // Maximum number of transactions in a running VB
  static const unsigned max_simul_batch_num =
      MAXSIMULBATCH;  // Maximum number of simultaneous threads
  // static const int max_query_thread_num = 20;
  std::atomic<int> is_create;

#ifdef SPIN_LOCK
  RWSpinLock mtx_;
#else
  std::mutex mtx_;
#endif
  // Epoch publication can be requested by the background updater and by a
  // static-load finalizer. Serialize those paths so a synchronous finalizer
  // cannot transform or enqueue the same VBData twice.
  std::atomic<uint64_t> timess;
  volatile epoch_t read_epoch_;
  unsigned read_epoch_cur_;
  // std::unordered_map<epoch_t, VBData*> active_txns_;
  VBData* active_txns_[max_simul_batch_num + 1];

  thread min_version_updater;
  uint max_threads;
  static thread_local size_t thread_id;
  mutex thread_registry_lock;
  vector<bool> thread_id_in_use;

  std::unique_ptr<std::atomic<epoch_t>[]> active_transactions;
  std::unique_ptr<std::atomic<epoch_t>[]> active_read_epochs;
  epoch_t min_epoch{numeric_limits<epoch_t>::min()};
  epoch_t min_read_epoch{numeric_limits<epoch_t>::min()};
  queue<VBData*> vb_que_;
  vector<epoch_t> sorted_versions;

  // boost::asio::thread_pool* pool_;

  bool check_vb_commit();

  // Add private members and methods here
};

#endif

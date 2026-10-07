#include "AllVBManager.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for_each.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>

#define MIN_VERSION_UPDATER_INTERVAL 100

// GC watermark (see declaration in utils/utils.h). Owned by the GC path here.
std::atomic<epoch_t> g_oldest_epoch{0};

void XorShiftRandom::seed(uint64_t s) {
  state = s ? s : 1;  // The PRNG state must not be zero.
}

uint32_t XorShiftRandom::rand_int(uint32_t max) {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state % max;
}
thread_local uint64_t XorShiftRandom::state =
    std::chrono::high_resolution_clock::now().time_since_epoch().count();

thread_local size_t AllVBManager::thread_id = 0;
void AllVBManager::register_thread(size_t id, bool on) {
  lock_guard<mutex> l(thread_registry_lock);
  // std::cout << "Registering thread " << id << "\n";
  if (thread_id_in_use[id] && on) {
    std::cout << "Tyring to reuse a thread id.\n" << on << '\n';
    assert(false);
  }
  thread_id_in_use[id] = true;
  thread_id = id;
}

epoch_t AllVBManager::getMinActiveVersion() { return min_epoch; }

void AllVBManager::update_min_version() {
  min_read_epoch = read_epoch_;
  epoch_t min1 = numeric_limits<epoch_t>::max();
  for (unsigned i = 0; i < max_threads; ++i) {
    min1 = std::min(
        min1, active_read_epochs[i].load(std::memory_order_acquire));
  }

  min_read_epoch = std::min(min_read_epoch, min1);

  auto last = min_epoch;
  min_epoch = (cur_ + no_more_txn_);
  epoch_t min2 = numeric_limits<epoch_t>::max();
  for (unsigned i = 0; i < max_threads; ++i) {
    min2 = std::min(
        min2, active_transactions[i].load(std::memory_order_acquire));
  }

  min_epoch = std::min(min_epoch, min2);
  if (min_epoch < last && min2 != numeric_limits<epoch_t>::max()) {
    // A lagging updater must never move the GC watermark backwards.  This
    // used to print every correction, which puts synchronous stdout I/O on a
    // transaction-manager hot path during benchmark runs.
    min_epoch = last;
  }
}

void AllVBManager::GC() {
#ifdef FINEGRAIN
  while (!vb_que_.empty() &&
         vb_que_.front()->get_epoch() < min_read_epoch) {  // fine-grained
#else
  while (!vb_que_.empty() && vb_que_.front()->get_epoch() <= min_read_epoch) {
#endif
    // std::cout << "Garbage collection " << vb_que_.front()->get_epoch() <<
    // '\n';
    delete vb_que_.front();
    vb_que_.pop();
  }
  // Advance the GC watermark. VBs strictly older than min_read_epoch have just
  // been discarded, so a deleted edge whose deletion time is < watermark has no
  // "existence" record left and no reader can need it. Relaxed on purpose: a
  // stale read in merge only drops *fewer* edges (conservative), never a wrong
  // one.
  g_oldest_epoch.store(min_read_epoch, std::memory_order_relaxed);
}
/**
 * Periodically updates the minimal epoch of all active transactions.
 *
 * @param interval The interval in which the minimal epoch is updated, in
 * microseconds.
 */
void AllVBManager::run_min_epoch_updater(uint interval) {
  unsigned last_min_epoch = 0;
  double st = 0;
  while ((!no_more_txn_ || read_epoch_ != cur_) && !stopped) {
    update_min_version();
    GC();
    if (read_epoch_ + 1 < min_epoch ||
        (no_more_txn_ && read_epoch_ < cur_)) {
      // Once the writer side is closed, publish the final active epoch too;
      // otherwise a static reader can remain at the initial read epoch.
      commitVersionBlock(no_more_txn_ ? cur_ + 1 : min_epoch);
    }
    this_thread::sleep_for(chrono::microseconds(interval));
    // if (start_count) st++;
    // auto start = std::chrono::steady_clock::now();
    // auto timeout = std::chrono::microseconds(interval);

    // while (std::chrono::steady_clock::now() - start <
    //        chrono::microseconds(interval));
  }
}

void AllVBManager::deregister_thread(size_t id) {
  lock_guard<mutex> l(thread_registry_lock);
  if (!thread_id_in_use[id]) {
    std::cout << "Trying to deregister a thread that has not been registered\n";
    assert(false);
  }
  if (active_transactions[id].load(std::memory_order_acquire) !=
      MY_NO_TRANSACTION) {
    std::cout << "Trying to deregister a thread with an active transaction\n";
    assert(false);
  }
  thread_id_in_use[id] = false;
}

void AllVBManager::reset_max_threads(uint max_threads) {
  lock_guard<mutex> l(thread_registry_lock);
  for (bool in_use : thread_id_in_use) {
    if (in_use) {
      // Thrown currently before update validation.

      std::cout
          << "Cannot change max_threads while any threads are registered\n";
      assert(false);
    }
  }
  this->max_threads = max_threads;
  active_transactions =
      std::make_unique<std::atomic<epoch_t>[]>(max_threads);
  active_read_epochs =
      std::make_unique<std::atomic<epoch_t>[]>(max_threads);
  for (unsigned i = 0; i < max_threads; ++i) {
    active_transactions[i].store(MY_NO_TRANSACTION,
                                 std::memory_order_relaxed);
    active_read_epochs[i].store(MY_NO_TRANSACTION,
                                std::memory_order_relaxed);
  }
  thread_id_in_use = vector<bool>(max_threads, false);
}

// TODO: register VB when creating in VBManager
bool VBData::commitVersionBlock() {
  // return true;
  // std::cout << "start commit with ts : " + std::to_string(epoch_) + '\n';
  // auto start_time = std::chrono::high_resolution_clock::now();

  // if (has_trans.test_and_set()) return false;
  // return true;
  int sm = 0, nm = 0, n = 0, kn = 0;
  for (int wt = 0; wt < WRITE_THREAD; wt++) {
    auto tmpn = vb_vector[wt].size();
    n += tmpn;
    for (int i = 0; i < tmpn; i++) {
      // it->Transform();
      this_vb_vector_.push_back(vb_vector[wt][i]);
      // kn += vb_vector[wt][i]->GetVersionNum();
      // sm += vb_vector[wt][i]->GetVersionNum();
    }
  };
  auto size = n * sizeof(VersionBlock);
  version_block_ = (VersionBlock*)malloc(size);

  std::atomic<int> x(0);

// std::ofstream outfile("version_block_addr.txt", std::ios::app);
// if (!outfile) {
//   std::cerr << "Unable to open file\n";
//   assert(false);
// }
// outfile << epoch_ << '\n' << n << '\n';
// for (int i = 0; i < n; i++) {
//   outfile << (version_block_ + i) << '\n';
// }
// outfile.close();
#pragma omp parallel for num_threads(30)
  for (int i = 0; i < n; i++) {
    new (((VersionBlock*)(version_block_) + i))
        VersionBlock(this_vb_vector_[i]);
    this_vb_vector_[i]->edge_block_->Transform(
        this_vb_vector_[i], ((VersionBlock*)(version_block_) + i));
  }

  // free(tmp);

  // auto end = std::chrono::high_resolution_clock::now();
  // auto duration =
  //     std::chrono::duration_cast<std::chrono::milliseconds>(end -
  //     start_time);

  // std::cout << "epoch " << epoch_ << " " << sm << " " << n << " " << kn << "
  // "
  //           << x << " ms : ";
  // // count_ll = 0;
  // if (duration.count() != 0)
  //   std::cout << " Here Waiting Elapsed time: " + to_string(duration.count())
  //   +
  //                    " ms\n\n";
  return true;
}

AllVBManager::AllVBManager(unsigned max_threads, bool on) : cur_(1) {
  active_txns_[1] = new VBData(1);
  read_epoch_ = 0;
  read_epoch_cur_ = 1;

  reset_max_threads(max_threads);
  stopped.store(false);
  if (on)
    min_version_updater = thread(&AllVBManager::run_min_epoch_updater, this,
                                 MIN_VERSION_UPDATER_INTERVAL);
  // pool_ = new boost::asio::thread_pool(8);

  // Constructor implementation
}

AllVBManager::~AllVBManager() {
  stopped.store(true);
  if (min_version_updater.joinable()) min_version_updater.join();
}

uint64_t AllVBManager::test1() {
  // bool f = false;
  // unsigned times = 0;
  // auto start = std::chrono::high_resolution_clock::now();
  // Timing interval begins here.
  epoch_t nw = cur_;
  while (nw - read_epoch_ >= max_simul_batch_num) nw = cur_;
  return nw;
  // std::cout << to_string(cur_ & 0xffffffff) + " " + to_string(read_epoch_) +
  //                  '\n';
  // std::this_thread::sleep_for(std::chrono::milliseconds(100));
  // timess++;
  //   f = true, times++;
  // Record the end of the interval.
  // if (f) {
  //   auto end = std::chrono::high_resolution_clock::now();
  // Compute elapsed time.
  //   auto duration =
  //       std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  // Report elapsed time in milliseconds.
  //   std::cout << "Find " + to_string(times) +
  //                    " times with Waiting Elapsed time: " +
  //                    to_string(duration.count()) + " ms\n\n";
  // } else {
  //   auto end = std::chrono::high_resolution_clock::now();
  // Compute elapsed time.
  //   auto duration =
  //       std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  //   if (duration.count() != 0)
  //     std::cout << " Here Waiting Elapsed time: " +
  //                      to_string(duration.count()) + " ms\n\n";
  // }
}
// has been locked by TxnManager
uint64_t x = 0;
void AllVBManager::registerTransaction(Transaction* txn) {
  // return;
  // TODO: using the max_simul_batch_num
  // assert(!txn->is_read_only());

  // #ifdef SPIN_LOCK
  //   std::unique_lock<RWSpinLock> lock(mtx_);
  // #else
  //   std::unique_lock<std::mutex> lock(mtx_);
  // #endif
  volatile uint64_t t;
  volatile unsigned cur, active_id;
  VBData* it;
  // t = cur_;
  // cur = t & 0xffffffff;
  // unsigned active_id = t >> 32;
  // while ((cur_ & 0xffffffff) - read_epoch_ > max_simul_batch_num);
  unsigned txn_num = 0;
  do {
    cur = test1();
    active_transactions[thread_id].store(cur, std::memory_order_release);
  } while (cur != cur_);

  active_id = cur & MODPM;
  it = active_txns_[active_id];
  // Add the transaction to the current VB
  int x = XorShiftRandom::rand_int(BATCHTXNNUM);
  // break;
  // txn_num = it->add_txn();

  // unsigned txn_num = 1;
  // return;
  // if (txn_num > max_txn_num_invb_) {
  //   active_transactions[thread_id] = numeric_limits<epoch_t>::max();
  //   continue;
  // }
  // std::cout << "register " + to_string(cur) + " " + to_string(txn_num) +
  // '\n';
  // if (txn_num == max_txn_num_git pull github main --allow-unrelated-historiesinvb_) {
  if (cur_ - read_epoch_ < max_simul_batch_num && !x) {
    int req = 0;
    if (is_create.compare_exchange_strong(req, 1)) {
      // auto start = std::chrono::high_resolution_clock::now();
      // Timing interval begins here.
      if (cur == cur_) {
        unsigned next_id = (active_id + 1) & MODPM;
        // it->set_all_flag();
        // std::cout << next_id << " A\n";
        // std::cout << "new VBData* " + to_string(cur) + '\n';
        active_txns_[next_id] = new VBData(cur + 1);  // new VBData(cur + 1);
        // std::cout << next_id << " B\n";
        cur_++;
        // std::cout << std::to_string(cur_) << " start\n";
        // Record the end of the interval.
        // auto end = std::chrono::high_resolution_clock::now();
        // Compute elapsed time.
        // auto duration =
        //     std::chrono::duration_cast<std::chrono::milliseconds>(end -
        //     start);
        // Report elapsed time in milliseconds.
        // std::cout << " new VBData* " + to_string(cur) +
        //                  " Elapsed time: " + to_string(duration.count()) +
        //                  "ms\n\n";
      }
      is_create = 0;
    }
  }
  txn->set_read_epoch(read_epoch_);
  txn->set_epoch(cur);
  txn->set_vb_data(it);
  // std::cout << "txn.epoch: " << txn->get_epoch() << " "
  //           << "txn.vbdata: " << txn->get_vb_data() << '\n';
}

void AllVBManager::registerROTransaction(Transaction* txn, int thread_id_) {
  if (thread_id_ == -1) thread_id_ = thread_id;
  epoch_t re;
  do {
    re = read_epoch_;
    active_read_epochs[thread_id_].store(re, std::memory_order_release);
  } while (re < min_read_epoch);
  txn->set_read_epoch(re);
#ifdef FINEGRAIN
  // Capture fine-grained read timestamp from earliest unpublished epoch.
  // active_txns_[read_epoch_cur_] is only valid to read once a VBData for
  // epoch re+1 has actually been constructed, i.e. re+1 <= cur_ (cur_ is
  // only incremented right after that VBData is created). After
  // FinalizeNoMoreTxn() synchronously commits read_epoch_ up to cur_, re+1
  // exceeds cur_ and that ring slot was never written (or was reused by an
  // earlier epoch and is stale) — dereferencing it without this guard reads
  // garbage and can segfault.
  VBData* vbdata = (re + 1 <= cur_) ? active_txns_[read_epoch_cur_] : nullptr;
  if (vbdata && vbdata->get_epoch() == re + 1) {
    txn->set_read_ts(Composite(re + 1, vbdata->get_intra_counter()));
  } else {
    txn->set_read_ts(Composite(re, INTRA_MAX));
  }
#endif
}
void AllVBManager::deregisterROTransaction(int thread_id_) {
  if (thread_id_ == -1) thread_id_ = thread_id;
  active_read_epochs[thread_id_].store(MY_NO_TRANSACTION,
                                       std::memory_order_release);
}

// has been locked by TxnManager
void AllVBManager::deregisterTransaction() {
  // std::cout << "VBData " + std::to_string(t) +
  //                  " has txn_num : " + std::to_string(it->get_txn_size()) +
  //                  '\n';
  // epoch_t txn_cur = txn->get_epoch();
  active_transactions[thread_id].store(MY_NO_TRANSACTION,
                                       std::memory_order_release);
  // auto vbd_id = txn_cur % (max_simul_batch_num + 1);

  // auto vbdata = active_txns_[txn_cur % (max_simul_batch_num + 1)];

  // auto num = vbdata->deregisterTransaction(txn);

  // if (num == BATCHTXNNUM - 1)
  //   std::cout << to_string(cur_ & 0xffffffff) + " " +
  //                    to_string(read_epoch_ + 1) + " " + to_string(txn_cur)
  //                    +
  //                    '\n';
  // if (txn_cur == read_epoch_ + 1 && num == BATCHTXNNUM - 1)
  //   commitVersionBlock(0, vbdata);
}
void AllVBManager::commitVersionBlock(epoch_t cur) {
#ifdef SPIN_LOCK
  std::unique_lock<RWSpinLock> lock(mtx_);
#else
  std::unique_lock<std::mutex> lock(mtx_);
#endif
  // std::cout << to_string(cur) + "A" + to_string(read_epoch_ + 1) + " " +
  //                  to_string(vbdata->all_commit()) + '\n';
  // if (read_epoch_ != 0) assert(false);
  // std::cout << "read_epoch_cur_: " << read_epoch_cur_ << " "
  //           << active_txns_[read_epoch_cur_] << '\n';
  auto vbdata = active_txns_[read_epoch_cur_];
  // std::cout << "commit " + to_string(cur) + " " + to_string(read_epoch_) +
  // '\n';
  while (read_epoch_ + 1 < cur) {
    // std::cout << "here " + to_string(read_epoch_) + '\n';
    // assert(false);
    // vbdata->lock_versionblock();
    vbdata->commitVersionBlock();
    // std::cout << "commit " + to_string((uint64_t)vbdata) + " " +
    //                  to_string(read_epoch_) + '\n';

    // delete vbdata;
    vb_que_.push(vbdata);
    // std::cout << "here2 " + to_string(cur) + " " + to_string(read_epoch_) +
    //                  '\n';
    // std::cout << "before: " << read_epoch_ << "  ";
    read_epoch_++;
    read_epoch_cur_ =
        read_epoch_cur_ == max_simul_batch_num - 1 ? 0 : read_epoch_cur_ + 1;

    // std::cout << "after: " << read_epoch_ << '\n';
    // if (cur == read_epoch_ + 1) cur = cur_ & 0xffffffff;
    // std::cout << "here3 " + to_string(cur) + " " + to_string(read_epoch_) +
    //                  '\n';

    // std::cout << "read_epoch_cur_: " << read_epoch_cur_ << " "
    //           << active_txns_[read_epoch_cur_] << '\n';
    vbdata = active_txns_[read_epoch_cur_];
    // std::cout << "here4 " + to_string(cur) + " " +
    //                  to_string((read_epoch_ + 1)) + " " +
    //                  to_string(vbdata->all_commit()) + '\n';
  }
}

void AllVBManager::NoMoreTxn() {
  no_more_txn_.store(true, std::memory_order_release);
}

void AllVBManager::FinalizeNoMoreTxn() {
  no_more_txn_.store(true, std::memory_order_release);

  // Static loading has already joined every writer before this hook is
  // called. Stop and join the asynchronous updater first, so it cannot race
  // with the target-epoch snapshot below. This avoids reading cur_/read_epoch_
  // concurrently while still keeping the regular online updater lock-free.
  stopped.store(true, std::memory_order_release);
  if (min_version_updater.joinable()) min_version_updater.join();

  // With writers and the updater stopped, cur_ is stable. Publish the active
  // write epoch synchronously; readers can then start from a complete static
  // snapshot instead of depending on a polling interval.
  const epoch_t target = cur_ + 1;
  if (read_epoch_ + 1 < target) commitVersionBlock(target);
}

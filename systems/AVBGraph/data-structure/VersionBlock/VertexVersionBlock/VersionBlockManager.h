#ifndef VERSIONBLOCKMANAGER_H
#define VERSIONBLOCKMANAGER_H

#include <deque>

#include "VBIterator.h"
/**
 * @brief Manager of VersionBlock
 *
 * Only used for simple control of VersionBlock
 *
 * 1. Return a VB for specified echo_t or alloc new VB when necessary
 *
 * 2. Garbage Collection
 *
 * 3. Commit a VB
 *
 * 4. Read a VB
 *
 */
class VersionBlockManager : public VBManagerInterface {
 public:
  VersionBlockManager() { vb_start = vb_end = nullptr; };
  VersionBlockManager(dst_t src);
  ~VersionBlockManager();

  // bool getVersionBlock(VersionBlock*& vb);
  // bool checkVersionBlockExist(epoch_t ts);
  // bool getVersionBlock(epoch_t ts, VersionBlock*& vb, Transaction* txn);
  // bool getTVersionBlock(epoch_t ts, TmpVersionBlock* vb);
  // bool getTVersionBlock(epoch_t ts, TmpVersionBlock& vb);
  // bool getFirstBlock(epoch_t ts, VersionBlock*& vb);
  void PopFront(unsigned num);

  void setSrc(dst_t src) { src_ = src; }
  dst_t getSrc() const { return src_; }

  void GetReadLock() { spin_rw_lock.lock_read(); }
  void GetWriteLock() { spin_rw_lock.lock(); }
  void UnLock() { spin_rw_lock.unlock(); }
  void PushVB(int& edge_num, VersionBlock* nw);
  int getDegreeVersioned(epoch_t epoch);

  epoch_t GetEndEpoch() { return end_epoch_; }
  VersionBlock* GetLastVB() { return vb_end; }

#ifdef FINEGRAIN
  // TVB active bitmask: bit i = tmp_vb[i] has uncommitted entries
  uint16_t active_tvb_mask_ = 0;
  void MarkTvbActive(epoch_t epoch)   { active_tvb_mask_ |=  (1u << (epoch & MODPM)); }
  void MarkTvbInactive(epoch_t epoch) { active_tvb_mask_ &= ~(1u << (epoch & MODPM)); }
  uint16_t GetActiveTvbMask() const   { return active_tvb_mask_; }

  // Fine-grained: VB vector index for binary search by epoch
  void PushVBIndex(epoch_t ts, VersionBlock* vb) {
    vb_index_.emplace_back(ts, vb);
  }
  VersionBlock* find_vb(epoch_t ts) {
    if (vb_index_.empty()) return nullptr;
    auto it = std::upper_bound(
        vb_index_.begin(), vb_index_.end(), ts,
        [](epoch_t v, const std::pair<epoch_t, VersionBlock*>& p) {
          return v < p.first;
        });
    return it == vb_index_.begin() ? nullptr : (--it)->second;
  }
#endif

  TmpVersionBlock tvb_array[MAXSIMULBATCH];

 private:
  // unsigned vb_r;
  // std::atomic<unsigned> vb_num, vb_id;
  // epoch_t ts_[MAXSIMULBATCH];
  tbb::spin_rw_mutex spin_rw_lock;
  dst_t src_;

  // std::deque<std::pair<epoch_t, VersionBlock*>>
  //     vb_deque_;  // VBs that has Transformed
  // VersionBlock* vb_array[MAXSIMULBATCH];
  epoch_t end_epoch_ = 0;
  VersionBlock *vb_start, *vb_end;
#ifdef FINEGRAIN
  std::vector<std::pair<epoch_t, VersionBlock*>> vb_index_;
#endif

  // void allocVersionBlock(epoch_t ts, VersionBlock*& vb, Transaction* txn);
  // void allocTVersionBlock(epoch_t ts, TmpVersionBlock*& vb, Transaction*
  // txn);
};

#endif  // VERSIONBLOCKMANAGER_H
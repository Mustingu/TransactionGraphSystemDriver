#include "VersionBlockManager.h"

VersionBlockManager::VersionBlockManager(dst_t src) : src_(src) {
  vb_start = vb_end = nullptr;
  // for (int i = 1; i < MAXSIMULBATCH; i++) {
  //   tvb_array[i].SetEdgeBlock(edge_block);
  // }
  // tvb_array[0].SetTimestamp(MAXSIMULBATCH);
  // vb_r = MAXSIMULBATCH - 1;
  // vb_num = 0;
  // Constructor implementation
}

VersionBlockManager::~VersionBlockManager() {
  // Destructor implementation
}

// bool VersionBlockManager::checkVersionBlockExist(epoch_t ts) {
//   // TODO: GC
//   return (vb_start != nullptr && vb_start->timestamp_ <= ts);
//   // return !(vb_deque_.empty() || vb_deque_.front().first > ts);
// }

// bool VersionBlockManager::getVersionBlock(epoch_t ts, VersionBlock*& vb,
//                                           Transaction* txn) {
//   auto nw_id = vb_id.load(std::memory_order_relaxed);
//   auto vb_num = nw_id >> 16, vb_r = nw_id & 0xFFFF;
//   if (vb_num == 0 || ts_[vb_r] < ts) {
//     allocVersionBlock(ts, vb, txn);
//     // count_vb++;
//     vb_num++;
//     vb_r = (vb_r + 1) & MODPM;
//     // vb_r++;
//     while (!vb_id.compare_exchange_strong(nw_id, vb_num << 16 | vb_r,
//                                           std::memory_order_relaxed,
//                                           std::memory_order_relaxed)) {
//       vb_num = (nw_id >> 16) + 1;
//     }
//     ts_[vb_r] = ts;
//     vb_array[vb_r] = vb;
//     // vb_r = vb_r + 1 & (MAXSIMULBATCH - 1);
//     return true;
//   }
//   // return false;
//   // if (vb_num == 0 || ts_[vb_r] < ts) {
//   //   // std::cout << this << " " << "src : " << src_ << " " << "ts : " <<
//   ts
//   //   << "
//   //   // "
//   //   //           << "vb_r and vb_num : " << vb_r << " " << vb_num << " "
//   //   //           << "last timestamp : "
//   //   //           << ((vb_num == 0) ? -1 : vb_array[vb_r]->timestamp_) <<
//   //   '\n';

//   //   // allocVersionBlock(ts, vb, txn);
//   //   vb_num++;
//   //   vb_r = (vb_r + 1) & (MAXSIMULBATCH - 1);
//   //   ts_[vb_r] = ts;
//   //   vb_array[vb_r] = vb;
//   //   return true;
//   // }
//   if (ts_[vb_r] == ts) {
//     return (vb = vb_array[vb_r]), true;
//   }

//   unsigned it1 = vb_r, t = 0;

//   while (t < vb_num) {
//     if (ts_[it1] < ts) break;
//     if (ts_[it1] == ts) return (vb = vb_array[it1]), true;
//     it1 = it1 - 1 & (MAXSIMULBATCH - 1);
//     t++;
//   }
//   unsigned id = vb_r + 1 & (MAXSIMULBATCH - 1), pre = vb_r;
//   t = 0;

//   if (t < vb_num)
//     while (pre != it1) {
//       vb_array[id] = vb_array[pre];
//       ts_[id] = ts_[pre];
//       id = pre;
//       pre = (pre - 1) & (MAXSIMULBATCH - 1);
//     }
//   // std::cout << "src : " << src_ << " " << "ts : " << ts << '\n';
//   allocVersionBlock(ts, vb, txn);
//   vb_r = (vb_r + 1) & (MAXSIMULBATCH - 1);
//   vb_num++;
//   while (!vb_id.compare_exchange_strong(nw_id, vb_num << 16 | vb_r,
//                                         std::memory_order_relaxed,
//                                         std::memory_order_relaxed)) {
//     vb_num = (nw_id >> 16) + 1;
//   }
//   vb_array[id] = vb;
//   ts_[id] = ts;
//   // vb_array[vb_r] = new VersionBlock(src_, ts, this);
//   return true;
// }
// bool VersionBlockManager::getTVersionBlock(epoch_t ts, TmpVersionBlock& vb) {
//   vb = tvb_array[ts & MODPM];
//   // return vb.GetTimestamp() == ts;
//   return true;
// }
// bool VersionBlockManager::getTVersionBlock(epoch_t ts, TmpVersionBlock* vb) {
//   vb = tvb_array + (ts & MODPM);
//   return vb->GetTimestamp() == ts;
// }

void VersionBlockManager::PushVB(int& edge_num, VersionBlock* nw) {
  edge_num += nw->edge_num_;
  nw->SetNextVB(vb_end, end_epoch_);
  vb_end = nw;
  end_epoch_ = nw->timestamp_;
#ifdef FINEGRAIN
  PushVBIndex(nw->timestamp_, nw);
#endif
}

int VersionBlockManager::getDegreeVersioned(epoch_t epoch) {
  int degree = 0;
  auto ts = end_epoch_;
  for (auto vb = vb_end; epoch < ts; ts = vb->last_epoch_, vb = vb->last_vb_) {
    degree -= vb->edge_num_;
  }
  return degree;
}

// bool VersionBlockManager::getFirstBlock(epoch_t ts, VersionBlock*& vb) {
//   bool f = 0;
//   // GetReadLock();
//   if (vb_start == nullptr || vb_start->timestamp_ > ts) return false;
//   // UnLock();
//   return (vb = vb_start), true;
// }

// void VersionBlockManager::allocVersionBlock(epoch_t ts, VersionBlock*& vb,
//                                             Transaction* txn) {
//   // return;
//   // new TmpVersionBlock(ts);
//   vb = new VersionBlock(src_, ts, this);
//   txn->write_lock_pushback(vb);
//   // vb->timestamp_ = ts;
//   count_vb++;
//   txn->get_vb_data()->registerVersionBlock(vb);
//   // std::cout << "vb : " << (void*)vb.get()->start_ << '\n';
// }

// void VersionBlockManager::allocTVersionBlock(epoch_t ts, TmpVersionBlock*&
// vb,
//                                              Transaction* txn) {
//   // vb = new VersionBlock(src_, ts, this);
//   // vb = reinterpret_cast<VersionBlock*>(je_calloc(1,
//   sizeof(VersionBlock))); vb = new TmpVersionBlock(ts); count_vb++;
//   txn->get_vb_data()->registerVersionBlock(reinterpret_cast<VersionBlock*>(vb));
//   // std::cout << "vb : " << (void*)vb.get()->start_ << '\n';
// }

// void VersionBlockManager::PopFront(unsigned num) {
//   // GetWriteLock();
//   auto last = vb_start;
//   vb_start = vb_start->next_vb_;
//   delete last;
//   // UnLock();
// }

// bool VersionBlockManager::getVersionBlock(VersionBlock*& vb) {
//   vb_end->SetNextVB(vb);
//   vb_end = vb;
//   return true;
// }
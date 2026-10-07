#include "VersionBlock.h"

#include <assert.h>

unsigned TmpVersionBlock::InsertVersion(dst_t* edge, epoch_t last_epoch,
                                        unsigned last_index, VersionBlock* vb,
                                        Transaction* txn, unsigned offset,
                                        unsigned mt, epoch_t le,
                                        intra_t old_intra, intra_t new_intra) {
  version_num_++;
  if (version_num_ > tmp_num) {
    if (tmp_num != 0) {
      tmp_num <<= 1;
      auto new_tmp = new VersionEntry[tmp_num];
      memcpy(new_tmp, tmp_entry_, (tmp_num >> 1) * sizeof(VersionEntry));
      delete[] tmp_entry_;
      tmp_entry_ = new_tmp;
    } else {
      tmp_num = 8;
      tmp_entry_ = new VersionEntry[tmp_num];
    }
  }
  memcpy(&tmp_entry_[version_num_ - 1].edge, edge, sizeof(dst_t) << 1);
  tmp_entry_[version_num_ - 1].merge_times = mt;
  tmp_entry_[version_num_ - 1].offset = offset;
  tmp_entry_[version_num_ - 1].txn =
      UN_READ_WRITE_MARK((uint64_t)txn) |
      (txn->is_write_only() ? 0 : READ_WRITE_TXN);
  tmp_entry_[version_num_ - 1].link.Set(last_epoch, last_index, vb);
  tmp_entry_[version_num_ - 1].last_epoch = le;
  tmp_entry_[version_num_ - 1].old_intra_c = old_intra;
  tmp_entry_[version_num_ - 1].new_intra_c = new_intra;
  return version_num_ - 1;
}

bool TmpVersionBlock::ChangeVersion(dst_t* edge, unsigned index,
                                    Transaction* txn, unsigned mt,
                                    unsigned offset) {
  auto& entry = tmp_entry_[index];
  if (txn->is_write_only()) {
    // However two txn can coexist
    entry.offset = offset;
    entry.merge_times = mt;
    if (entry.HasReadWriteTxn() ||
        UN_READ_WRITE_MARK(entry.txn) <= UN_READ_WRITE_MARK((uint64_t)txn)) {
      // new_txn is last txn in this epoch
      entry.weight = edge[1];
      entry.txn = UN_READ_WRITE_MARK((uint64_t)txn) |
                  (txn->is_write_only() ? 0 : READ_WRITE_TXN);
      // entry = (VersionEntry){edge, 0, txn};
      return true;
    } else {
      // std::cout << "without new property\n";
      // old_txn is writ_only and later on timestamps, do nothing
      return true;
    }
  } else {
    // TODO: for read-write txn
    assert(false);
    return false;
  }
}

void TmpVersionBlock::ChangeNextIndex(unsigned index, epoch_t next_epoch,
                                      unsigned next_index) {
  auto& entry = tmp_entry_[index];
  // entry.link.Set(next_epoch | (entry.link.block & TMPVB_MASK), next_index);
  entry.link.block |= next_epoch;
  entry.link.index = next_index;
  // entry.next_epoch = next_epoch;
  // entry.next_index = next_index;
}
void TmpVersionBlock::ChangeLastIndex(unsigned index, VersionBlock* vb) {
  auto& entry = tmp_entry_[index];
  entry.link.link = vb;
  entry.link.block ^= TMPVB_MASK;
}

void TmpVersionBlock::ChangeOffset(unsigned index, unsigned offset) {
  auto& entry = tmp_entry_[index];
  entry.offset = offset;
}

VersionBlock::VersionBlock(TmpVersionBlock* tvb)
    : timestamp_(tvb->timestamp_),
      version_num_(tvb->version_num_),
      start_(nullptr),
      end_(0),
      last_vb_(nullptr),
      manager_(tvb->manager_) {
        // std::memcpy(read_ts_, tvb->tmp_rs.data(), sizeof(unsigned) * (sm_rs
        // << 1));

        // new (&rw_mutex_) tbb::spin_rw_mutex();
      };

/*
void VersionBlock::Transform(TmpVersionBlock *tvb) {
  // std::cout << version_num_ << '\n';
  // return;
  // if (has_sink.test_and_set()) {
  //   // count_x++;
  //   return;
  // }
  // count_y++;

  // manager_->GetReadLock();
  manager_->PushVB(this);

  // calculate the basic information of new VersionBlock
  // read_ts_.reserve(5);
  read_ts_[0] = timestamp_;
  read_ts_[1] = 0;
  // alloc new memory for constructed versionblock
  // start_ = new dst_t[version_num_ << 1];
  // tail_ = start_ + version_num_ * (sizeof(dst_t) << 1);
  // link_start_ = new VersionLink[version_num_];
  // swap_index_ = new unsigned[version_num_];
  memset(swap_index_, 0, sizeof(unsigned) * version_num_);

  // offset used in transfer temporary version to VersionBlock
  auto offset = start_;
  auto link_offset = link_start_;

  // record for other VersionBlock::Sink
  // std::unordered_map<VersionBlock *,
  //                    std::pair<epoch_t, std::vector<unsigned *>>>
  //     sink_map;

  // copy tmp_entry_ to start
  // set VersionLink
  auto t = 0;
  for (int i = 0; i < version_num_; i++) {
    auto a = tvb->tmp_entry_[i];
    *offset = a.edge;
    *(offset + 1) = a.weight;
    offset += 2;

    auto &it = *link_offset;
    memcpy(it, a.link, sizeof(VersionLink));

    if (it->link) {
      if (manager_->checkVersionBlockExist(it->block)) {
        it->link->Sink(timestamp_, &(it->index));
      }
      // sink_map[(a.link->GetVB())].first = link_offset->block;
      // sink_map[(a.link->GetVB())].second.push_back(&(link_offset->index));
    }
    link_offset++;

    ((VersionLink *)(a.link))->index = (t++);
    ((VersionLink *)(a.link))->block = (timestamp_);
    ((VersionLink *)(a.link))->link = (this);
  };

  // manager_->UnLock();

  // sink other VersionBlock for (auto &v : sink_map) {
  //   // TODO: need to check existance of VersionBlock
  //   if (manager_->checkVersionBlockExist(v.second.first)) {
  //     v.first->Sink(timestamp_, v.second.second);
  //   }
  // }

  // tbb::concurrent_unordered_map<dst_t, VersionEntry>().swap(tmp_entry_);
  // tmp_entry_.clear();
}
*/

// void VersionBlock::Sink(epoch_t ts, unsigned *index) {
//   // Implementation of sink method

//   {
//     tbb::spin_rw_mutex::scoped_lock lock(rw_mutex_, true);
//     // assert(has_sink.test_and_set());
//     auto &read_ts_it = read_ts_.back();
//     unsigned swap_id = end_++, ts1 = read_ts_it.first;
//     // if (!has_sink)
//     //   std::cout << ts << " " << timestamp_ << " " << this << " "
//     //             << tmp_entry_.size() << " not has sink\n";

//     // assert(has_sink);
//     // char *tmp = new char[ele_size_];

//     // get the last restriction index
//     if (ts1 == ts) {
//       read_ts_it.second++;
//     } else {
//       read_ts_.push_back(std::make_pair(ts, end_));
//     }
//     if (end_ == version_num_) {
//       return;
//     }

//     unsigned &it = *index;
//     if (swap_index_[it]) {
//       Track(it);
//     }
//     if (swap_id != it) {
//       swap_index_[swap_id] = it;

//       // swap index[i] and index[i - st]
//       std::swap(start_[swap_id << 1], start_[it << 1]);
//       std::swap(start_[(swap_id << 1) + 1], start_[(it << 1) + 1]);
//       // memcpy(tmp, start_ + swap_id * ele_size_, ele_size_);
//       // memcpy(start_ + swap_id * ele_size_, start_ + it * ele_size_,
//       // ele_size_); memcpy(start_ + it * ele_size_, tmp, ele_size_); swap
//       // link_start[i] and link_start[index[i - st]] swap(link_start_[i],
//       // link_start_[it]);
//       link_start_[swap_id].val ^= link_start_[it].val;
//       link_start_[it].val ^= link_start_[swap_id].val;
//       link_start_[swap_id].val ^= link_start_[it].val;

//       it = swap_id;
//       SET_SINK_FLAG(it);
//       // adjust the input index

//       // TODO: read through version_link
//     }
//   }
// }

// void VersionBlock::Sink(epoch_t ts, std::vector<unsigned *> &index) {
//   // Implementation of sink method

//   tbb::spin_rw_mutex::scoped_lock lock(rw_mutex_, true);
//   unsigned st = read_ts_.back().second, ed;
//   if (!has_sink.test_and_set()) {
//     std::cout << ts << " " << timestamp_ << " " << this << " "
//               << " not has sink\n";
//     assert(false);
//   }
//   // char *tmp = new char[ele_size_];

//   // get the last restriction index
//   end_ += index.size();
//   ed = end_;

//   read_ts_.push_back(std::make_pair(ts, end_));

//   if (end_ == version_num_) {
//     return;
//   }

//   for (auto i = st; i < ed; i++) {
//     unsigned &it = *index[i - st];
//     if (swap_index_[it]) {
//       Track(it);
//     }
//     if (i != it) {
//       swap_index_[i] = it;

//       // swap index[i] and index[i - st]
//       // memcpy(tmp, start_ + i * ele_size_, ele_size_);
//       // memcpy(start_ + i * ele_size_, start_ + it * ele_size_, ele_size_);
//       // memcpy(start_ + it * ele_size_, tmp, ele_size_);
//       // swap link_start[i] and link_start[index[i - st]]
//       // swap(link_start_[i], link_start_[it]);
//       link_start_[i].val ^= link_start_[it].val;
//       link_start_[it].val ^= link_start_[i].val;
//       link_start_[i].val ^= link_start_[it].val;

//       it = i;
//       SET_SINK_FLAG(it);
//       // adjust the input index

//       // TODO: read through version_link
//     }
//   }
// }

void VersionBlock::SetNextVB(VersionBlock* next, epoch_t ts) {
  last_vb_ = next;
  last_epoch_ = ts;
}

// bool VersionBlock::GetReadMutex() { return rw_mutex_.lock_read(), true; }
// void VersionBlock::ReleaseReadMutex() { return rw_mutex_.unlock(); }
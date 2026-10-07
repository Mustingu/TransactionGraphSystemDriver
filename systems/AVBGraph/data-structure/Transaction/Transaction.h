#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <data-structure/data_types.h>
// #include <tbb/mutex.h>

#include <iostream>
#include <mutex>
#include <string>

#include "utils/Interface.h"
#include "utils/utils.h"

// used by VB_index
#define LINK_SINK_FALG (1 << 31)
#define IS_TRACK(x) (!(x & LINK_SINK_FALG))
#define SET_SINK_FLAG(x) (x |= LINK_SINK_FALG)
#define UNSET_SINK_FLAG(x) (x & ~LINK_SINK_FALG)

// used by VBLink
#define WRITE_LOCK_FLAG (1L << 63)
#define IS_WRITE_LOCK(x) (x & WRITE_LOCK_FLAG)
#define SET_WRITE_LOCK_FLAG(x) (x |= WRITE_LOCK_FLAG)
#define UNSET_WRITE_LOCK_FLAG(x) (x &= ~WRITE_LOCK_FLAG)

#define inf 0xffffffffffffffffL

class VBData;

// TODO: change to uint64_t, and parse when use
// union VersionLink {
//   /**
//    * NOTE: whether used shared_ptr or raw ptr or direct index
//    *    1. shared_ptr : easy to manage memory, but may caused performance
//    issue
//    *    2. raw ptr : need to manage memory by yourself
//    *    3. direct index : need to call VersionBlockManager
//    */
//   struct {
//     epoch_t block;
//     unsigned index;
//   };
//   dst_t val;

//   VersionLink(uint64_t v) : val(v) {}
//   VersionLink(epoch_t v) : block(v), index(-1) {}
//   VersionLink() : val(inf) {}
//   void P() {
//     std::cout << "  block : " << block << " index : " << index << '\n';
//   }
// };

struct VersionLink {
  epoch_t block;
  unsigned index;
  VersionBlock* link;
  VersionLink() : block(0), index(0), link(nullptr) {}
  void Set(epoch_t b, unsigned i, VersionBlock* l) {
    block = b;
    index = i;
    link = l;
  }
  void Set(epoch_t b, unsigned i) {
    block = b;
    index = i;
  }
  void clear() {
    block = index = 0;
    link = nullptr;
  }

  bool HasNextVB() { return (block & ~TMPVB_MASK); }
};

class TmpVersionData {
 private:
  // std::atomic<bool> f;
  // VersionLink link_;
  dst_t e;
  // VersionLink vb_index_;
  // VersionBlock* vb_;

 public:
  TmpVersionData() { /*count_vb++; */ }
  // epoch_t oldest_version_epoch_;
  // TmpVersionData(epoch_t link)
  //     : e(link) {
  //         // std::cout << "link_ : " << link_.block << " " << link_.index
  //         //           << "oldest : " << oldest_version_epoch_ << '\n';
  //       };
  // TmpVersionData(epoch_t link, VersionBlock* vb)
  //     : link_(link), vb_(vb) {
  //         // std::cout << "link_ : " << link_.block << " " << link_.index
  //         //           << "oldest : " << oldest_version_epoch_ << '\n';
  //       };
};

// class VersionData {
//  private:
//   VersionLink link_;
//   VersionLink vb_index_;
//   VersionBlock* vb_;
//   std::mutex lock_;

//  public:
//   epoch_t oldest_version_epoch_;
//   VersionData(uint64_t link) : link_(link), vb_index_(link) {};
//   VersionData(epoch_t link)
//       : link_(link), oldest_version_epoch_(link), vb_index_(link) {
//           // std::cout << "link_ : " << link_.block << " " << link_.index
//           //           << "oldest : " << oldest_version_epoch_ << '\n';
//         };
//   VersionLink& GetLink() { return link_; };
//   inline bool CheckEdgeBlockVersionNeed(epoch_t epoch) {
//     // std::cout << "link_ : " << link_.block << " " << link_.index
//     //           << " epoch : " << epoch << "oldest : " <<
//     oldest_version_epoch_
//     //           << '\n';
//     // if (epoch < oldest_version_epoch_) std::cout << "yes\n";
//     return epoch < oldest_version_epoch_;
//   }
//   VersionLink GetVBIndex() { return vb_index_; }
//   void ChangeIndexIndex(unsigned index) { vb_index_.index = index; }
//   void ChangeIndexBlock(unsigned block) { vb_index_.block = block; }
//   void ChangeLinkIndex(unsigned index) { link_.index = index; }
//   void ChangeLinkBlock(unsigned block) { link_.block = block; }
//   void SetLink(epoch_t link) { link_ = vb_index_ = link; };
//   void SetVB(VersionBlock* vb) { vb_ = vb; }
//   auto GetVB() { return vb_; }
//   void lock() { lock_.lock(); };
//   void unlock() { lock_.unlock(); };
//   void commit();
// };
class Transaction {
 public:
  struct insert_triple {
    dst_t* edge;
    // VersionData* versionData_ptr_;
  };
  Transaction(version_t version, bool read_only, bool write_only,
              VersionedTopologyInterface* ds)
      : version_(version),
        read_only_(read_only),
        write_only_(write_only),
        ds(ds),
        edges_to_insert(nullptr) {}
  bool is_read_only() const { return read_only_; };
  bool is_write_only() const { return write_only_; };
  void set_read_epoch(epoch_t epoch) { read_epoch_ = epoch; };
  void set_epoch(epoch_t epoch) { epoch_ = epoch; };
#ifdef FINEGRAIN
  Composite get_read_ts() const { return read_ts_; }
  void set_read_ts(Composite ts) { read_ts_ = ts; }
#endif
  void set_vb_data(VBDataInterface* vb_data) { vb_data_ = vb_data; };
  intra_t get_intra_c() {
#ifdef FINEGRAIN
    if (!intra_c_) intra_c_ = vb_data_->fetch_add_intra();
#endif
    return intra_c_;
  }
  void write_lock_pushback(TmpVersionBlock* vb) {
    // VersionData_ptr_.push_back(val);
    // version_blocks_.push_back(vb);
    vb_data_->VB_pushback(vb, threadid);
  };
  VBDataInterface* get_vb_data() { return vb_data_; };
  epoch_t get_epoch() const { return epoch_; };
  epoch_t get_read_epoch() const { return read_epoch_; };
  version_t get_version() const { return version_; };
  // bool getFirstBlock(shared_ptr<VersionBlock>& vb) const {
  //   return ds->getFirstBlock(read_epoch_, vb);
  // };

  void CheckETI() {
    if (edges_to_insert == nullptr)
      edges_to_insert = new std::vector<Transaction::insert_triple>();
  }
  void COMMIT();
  void INSERT(std::vector<insert_triple> edges_to_insert_) {
    if (insert_cnt_ == 0) one_edge_ = edges_to_insert_[0];
    insert_cnt_ += edges_to_insert_.size();
    if (insert_cnt_ > 1) {
      CheckETI();
      edges_to_insert->insert(edges_to_insert->end(),
                              edges_to_insert_.begin() + 1,
                              edges_to_insert_.end());
    }
    // std::sort(edges_to_insert.begin(), edges_to_insert.end(),
    //           [](const insert_triple& a, const insert_triple& b) {
    //             if (a.src != b.src) return a.src < b.src;
    //             return a.dst < b.dst;
    //           });
  }
  void INSERT(dst_t* edge) {
    insert_cnt_++;
    bool f = 0;
    // ds->insert_edge_block(y.src, y.dst, epoch_, y.property,
    // y.versionData_ptr_,
    //                       f, this);
    if (insert_cnt_ == 1)
      one_edge_ = (Transaction::insert_triple){edge};
    else {
      CheckETI();
      edges_to_insert->push_back({edge});
    }
    // std::sort(edges_to_insert.begin(), edges_to_insert.end(),
    //           [](const insert_triple& a, const insert_triple& b) {
    //             if (a.src != b.src) return a.src < b.src;
    //             return a.dst < b.dst;
    //           });
  }
  void execute();
  void insertedge(dst_t* edge);
  void deletedge(dst_t* edge);

  // void getReadLock() { ds->getReadLock(); }
  // void unleashReadLock() { ds->unleashReadLock(); }

  VersionedTopologyInterface* getTopology() { return ds; }

  // std::vector<VersionData*> VersionData_ptr_;
  // std::vector<VersionBlock*> version_blocks_;

  void commit();

  void AddEB(VertexEdgesInterface* eb) { EB_vec_.push_back(eb); }

 private:
  bool read_only_, write_only_;
  unsigned insert_cnt_ = 0;
  version_t version_;
  epoch_t read_epoch_, epoch_;
  intra_t intra_c_ = 0;  // fine-grained intra-epoch timestamp
#ifdef FINEGRAIN
  Composite read_ts_{0, 0};  // fine-grained read timestamp
#endif
  std::vector<Transaction::insert_triple>* edges_to_insert;
  VersionedTopologyInterface* ds;
  Transaction::insert_triple one_edge_;
  std::vector<VertexEdgesInterface*> EB_vec_;

 public:
  VBDataInterface* vb_data_;
};

#endif  // TRANSACTION_H
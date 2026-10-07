#ifndef VERTEXEDGES_H
#define VERTEXEDGES_H

#include <utils/utils.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <unordered_map>

#include "HashTable.h"
#include "RWSpinLock.h"
#include "VersionBlockManager.h"
#include "gapbs.h"
#define TMPNUM 64

/**
 * Represents a block of memory which contains edges, versions and properties.
 *
 * The block starts with edges, interleaved with versions and ends with
 * properties. The edges and versions grow upwards and the properties grow
 * downwards.
 *
 * In the unversioned case, the property value belonging to an edge has the same
 * offset in the property section. In the versioned case, the property value
 * belonging to an edge has the same offset in the property section minus all
 * versions that are before the edge in question. This allows random access to
 * edge and property in the unversioned case but requires scanning edges from
 * the beginning in the versioned case.
 *
 * We do not support multiple property versions yet. They can be supported the
 * same way as supporting edge versions but requires to clean the property
 * section on GC.
 */

// Read visibility of a PA / tmp_item entry for a scan. In FINEGRAIN the PA
// carries the newest version together with an intra-epoch timestamp, so
// visibility must be composite ((coarse, intra) < read_ts); a coarse-only check
// would leak a later intra-epoch write into an earlier reader intra. In coarse
// mode the intra is meaningless (the non-fine path passes Composite(read_ts, 0)
// and writes index = 0xFFFFFFFF), so the plain coarse check is the correct one.
static inline bool edge_visible_at(const EdgeWithIndex& e, Composite read_ts) {
#ifdef FINEGRAIN
  return e.IsVisibleAtComposite(read_ts);
#else
  return e.IsVisibleAt(read_ts.e);
#endif
}

class VertexEdges : public VertexEdgesInterface {
 public:
  VertexEdges(dst_t src, dst_t* _start, size_t _capacity,
              size_t _edges_and_versions);

  VertexEdges();
  VertexEdges(dst_t src);

  void setSrc(dst_t src);
  VertexEdges(const VertexEdges& other);

  size_t get_edges_and_versions() { return edges_and_versions; }
  VersionBlockManager& GetVBM() { return VBM; }

  // Accessors for testing / low-level access
  EdgeWithIndex* get_start() { return start; }
  unsigned get_capacity() { return capacity; }
  EdgeWithIndex* get_tmp_item() { return tmp_item; }
  unsigned get_tmp_ev() { return tmp_ev; }
  TmpVersionBlock** get_tmp_vb() { return tmp_vb; }

  // char* properties_end() { return ; }
  /**
   * Finds the correct place to add edge, version record and properties and
   * inserts them by shifthing.
   *
   * @param e
   * @param version
   * @param properties
   */

  unsigned Transform(TmpVersionBlock* tvb, VersionBlock* vb);

  void merge_tmpev_with_eb();
  bool get_edge_index2(dst_t e, EdgeWithIndex*& item_e, EdgeWithIndex*& item_p);
  bool get_edge_index(dst_t e, EdgeWithIndex*& item_e, EdgeWithIndex*& item_p,
                      unsigned& offset);
  // TODO: adjust to Versionblock
  bool insert_edge_block(dst_t* e, epoch_t epoch, Transaction* txn,
                         bool new_entry);
  bool delete_edge_block(dst_t* e, epoch_t epoch, Transaction* txn,
                         bool new_entry);

  unsigned get_degree() { return num; }
  unsigned get_degree(epoch_t epoch);
  // True only for a block that has never received versioned or buffered writes.
  // This conservative predicate intentionally does not infer safety from GC:
  // VersionBlockManager currently has no synchronized empty-history state.
  bool has_primary_only_state() {
    return tmp_ev == 0 && VBM.GetEndEpoch() == 0
#ifdef FINEGRAIN
           && VBM.GetActiveTvbMask() == 0
#endif
        ;
  }

  // Fast read for an immutable primary adjacency. The caller must hold the
  // VertexEdges read lock and establish the primary-only predicate first.
  template <typename EdgeCallback>
  void for_each_primary_edge(EdgeCallback cb) const {
    for (unsigned i = 0; i < edges_and_versions; ++i) {
      const auto* edge = start + i;
      if (!is_delete(edge->e)) cb(const_cast<EdgeWithIndex*>(edge));
    }
  }

  // unsigned get_degree();
  void build(unsigned _num, dst_t* _edges, dst_t* _properties);

  void my_print_block();

  // NOTE: build this function for test
  double getSum(dst_t src,
                std::map<std::pair<dst_t, dst_t>, std::vector<unsigned>>&
                    edge_wight_versioned,
                epoch_t epoch, bool& need_iterator, VersionBlock*& vb,
                std::vector<dst_t>& pmHM);
  /*
  bool delete_edge(dst_t e, version_t version);*/

  /**
   * Finds the upper bound for value in a sorted array.
   *
   * Ignores versions.
   *
   * @param start
   * @param end
   * @param value
   * @return a pointer to the position of the upper bound or end.
   * @return a pointer to the position of the upper bound or end.
   */
  dst_t* find_upper_bound(dst_t* start, dst_t* end, dst_t value);

  void getReadLock();
  void unleashReadLock();

  dst_t get_src();

  // Sorted edge iteration: merge sorted PA with small tmp_item buffer.
  // Calls cb(EdgeWithIndex*) for each visible dst in sorted order (dedup: PA wins).
  template <typename EdgeCallback>
  void for_each_edge_sorted(epoch_t epoch, EdgeCallback cb) {
    // tmp_item: collect visible, sort by dst
    struct Pair { dst_t d; EdgeWithIndex* p; };
    Pair tmp_buf[64]; int tn = 0;
    for (unsigned i = 0; i < tmp_ev; i++)
      if (!is_delete(tmp_item[i].e) && tmp_item[i].IsVisibleAt(epoch))
        tmp_buf[tn++] = {tmp_item[i].e, tmp_item + i};
    std::sort(tmp_buf, tmp_buf + tn, [](auto& a, auto& b){ return a.d < b.d; });
    // Two-pointer merge
    unsigned si = 0, ti = 0;
    dst_t last = ~(dst_t)0;
    while (si < edges_and_versions || ti < (unsigned)tn) {
      bool use_start = false;
      if (si >= edges_and_versions) use_start = false;
      else if (ti >= (unsigned)tn) use_start = true;
      else if (is_delete(start[si].e) || !start[si].IsVisibleAt(epoch)) { si++; continue; }
      else use_start = start[si].e < tmp_buf[ti].d;
      if (use_start) {
        if (start[si].e != last) { cb(start + si); last = start[si].e; }
        si++;
      } else {
        if (tmp_buf[ti].d != last) { cb(tmp_buf[ti].p); last = tmp_buf[ti].d; }
        ti++;
      }
    }
  }

  template <typename EdgeCallback>
  void iterate_edges(Composite read_ts, bool& need_iterator, VersionBlock*& vb,
                     EdgeCallback callback) {
    // volatile double sm = 0;
    for (auto i = tmp_item; i < tmp_item + tmp_ev; i++) {
      if (!is_delete(i->e) && edge_visible_at(*i, read_ts)) {
        callback(i);
      }
    }

    for (auto i = start; i < start + edges_and_versions; i++) {
      if (!is_delete(i->e) && edge_visible_at(*i, read_ts)) {
        callback(i);
      }
    }
#ifdef FINEGRAIN
    // TVB scan: uncommitted old versions displaced by current-epoch writes
    {
      uint16_t mask = VBM.GetActiveTvbMask();
      Composite reader_ts = read_ts;
      while (mask) {
        int idx = __builtin_ctz(mask);
        mask &= mask - 1;
        auto* tvb = tmp_vb[idx];
        if (!tvb || !tvb->tmp_entry_) continue;
        #ifdef TVB_STATS
        tvb_scan_total.fetch_add(1, std::memory_order_relaxed);
#endif
        int k;
        for (k = (int)tvb->version_num_ - 1; k >= 0; k--) {
          auto& ent = tvb->tmp_entry_[k];
          if (is_delete(ent.edge)) continue;
          Composite inv(tvb->timestamp_, ent.new_intra_c);
          if (!(reader_ts <= inv)) break;
          Composite c(ent.last_epoch, ent.old_intra_c);
          if (c < reader_ts) {
            EdgeWithIndex e_tmp;
            e_tmp.SetEdgeFG(ent.edge, ent.last_epoch, ent.old_intra_c);
            callback(&e_tmp);
          }
        }
        {
          int _n = (int)tvb->version_num_;
          int _c = (k < 0) ? _n : _n - k;
          #ifdef TVB_STATS
          tvb_record_total.fetch_add(_c, std::memory_order_relaxed);
#endif
        }
      }
    }
#endif

    // uint64_t* k = reinterpret_cast<uint64_t*>(tmp_item);
    // for (auto i = 0; i < tmp_ev; i++) {
    //   // if (!is_delete(tmp_item[i].e) && tmp_item[i].GetLinkBlock() <=
    //   // epoch) {
    //   callback(k + i);
    //   // }
    // }
    // k = reinterpret_cast<uint64_t*>(start);
    // for (auto i = 0; i < edges_and_versions; i++) {
    //   // if (!is_delete(i->e) && i->IsVisibleAt(read_ts.e)) {
    //   // callback(i);
    //   callback(k + i);
    //   // sm += 1;
    //   // }
    // }

#ifdef FINEGRAIN
    if (read_ts.e <= VBM.GetEndEpoch()) {
#else
    if (read_ts.e < VBM.GetEndEpoch()) {
#endif
      vb = VBM.GetLastVB();
      need_iterator = vb != nullptr;
    }
  }

  template <typename EdgeCallback>
  void iterate_edges_condition(Composite read_ts, bool& need_iterator,
                               VersionBlock*& vb, EdgeCallback callback) {
    // volatile double sm = 0;
    for (auto i = tmp_item; i < tmp_item + tmp_ev; i++) {
      if (!is_delete(i->e) && edge_visible_at(*i, read_ts)) {
        if (callback(i)) return;
      }
    }

    for (auto i = start; i < start + edges_and_versions; i++) {
      if (!is_delete(i->e) && edge_visible_at(*i, read_ts)) {
        if (callback(i)) return;
      }
    }
    // uint64_t* k = reinterpret_cast<uint64_t*>(tmp_item);
    // for (auto i = 0; i < tmp_ev; i++) {
    //   // if (!is_delete(tmp_item[i].e) && tmp_item[i].GetLinkBlock() <=
    //   // epoch) {
    //   callback(k + i);
    //   // }
    // }
    // k = reinterpret_cast<uint64_t*>(start);
    // for (auto i = 0; i < edges_and_versions; i++) {
    //   // if (!is_delete(i->e) && i->IsVisibleAt(read_ts.e)) {
    //   // callback(i);
    //   callback(k + i);
    //   // sm += 1;
    //   // }
    // }

#ifdef FINEGRAIN
    // TVB scan: uncommitted old versions displaced by current-epoch writes
    {
      uint16_t mask = VBM.GetActiveTvbMask();
      Composite reader_ts = read_ts;
      while (mask) {
        int idx = __builtin_ctz(mask);
        mask &= mask - 1;
        auto* tvb = tmp_vb[idx];
        if (!tvb || !tvb->tmp_entry_) continue;
        #ifdef TVB_STATS
        tvb_scan_total.fetch_add(1, std::memory_order_relaxed);
#endif
        int k;
        for (k = (int)tvb->version_num_ - 1; k >= 0; k--) {
          auto& ent = tvb->tmp_entry_[k];
          if (is_delete(ent.edge)) continue;
          Composite inv(tvb->timestamp_, ent.new_intra_c);
          if (!(reader_ts <= inv)) break;
          Composite c(ent.last_epoch, ent.old_intra_c);
          if (c < reader_ts) {
            EdgeWithIndex e_tmp;
            e_tmp.SetEdgeFG(ent.edge, ent.last_epoch, ent.old_intra_c);
            if (callback(&e_tmp)) return;
          }
        }
        {
          int _n = (int)tvb->version_num_;
          int _c = (k < 0) ? _n : _n - k;
          #ifdef TVB_STATS
          tvb_record_total.fetch_add(_c, std::memory_order_relaxed);
#endif
        }
      }
    }
#endif

#ifdef FINEGRAIN
    if (read_ts.e <= VBM.GetEndEpoch()) {
#else
    if (read_ts.e < VBM.GetEndEpoch()) {
#endif
      vb = VBM.GetLastVB();
      need_iterator = vb != nullptr;
    }
  }

  template <typename EdgeCallback>
  void iterate_edges_with_property(Composite read_ts, bool& need_iterator,
                                   VersionBlock*& vb, EdgeCallback callback) {
    // volatile double sm = 0;
    for (auto i = tmp_item; i < tmp_item + tmp_ev; i++) {
      if (!is_delete(i->e) && edge_visible_at(*i, read_ts)) {
        // if (src_ == 43 || src_ == 36) {
        // std::cout << "src: " << src_ << " e: " << i->e
        //           << " p: " << *(double*)(&(i + TMPNUM)->properties) << " "
        //           << (i->block & ~TMPVB_MASK) << " "
        //           << ((i + TMPNUM)->block & ~TMPVB_MASK) << '\n';
        // }
        callback(i, i + TMPNUM);
      }
    }

    for (auto i = start; i < start + edges_and_versions; i++) {
      if (!is_delete(i->e) && edge_visible_at(*i, read_ts)) {
        // if (src_ == 43 || src_ == 36) {
        // std::cout << "src: " << src_ << " e: " << i->e
        //           << " p: " << *(double*)(&(i + TMPNUM)->properties) << " "
        //           << (i->block & ~TMPVB_MASK) << " "
        //           << ((i + TMPNUM)->block & ~TMPVB_MASK) << '\n';
        // }
        callback(i, i + capacity);
      }
    }

#ifdef FINEGRAIN
    // TVB scan: uncommitted old versions displaced by current-epoch writes
    {
      uint16_t mask = VBM.GetActiveTvbMask();
      Composite reader_ts = read_ts;
      while (mask) {
        int idx = __builtin_ctz(mask);
        mask &= mask - 1;
        auto* tvb = tmp_vb[idx];
        if (!tvb || !tvb->tmp_entry_) continue;
        #ifdef TVB_STATS
        tvb_scan_total.fetch_add(1, std::memory_order_relaxed);
#endif
        int k;
        for (k = (int)tvb->version_num_ - 1; k >= 0; k--) {
          auto& ent = tvb->tmp_entry_[k];
          if (is_delete(ent.edge)) continue;
          Composite inv(tvb->timestamp_, ent.new_intra_c);
          if (!(reader_ts <= inv)) break;
          Composite c(ent.last_epoch, ent.old_intra_c);
          if (c < reader_ts) {
            EdgeWithIndex e_tmp, p_tmp;
            e_tmp.SetEdgeFG(ent.edge, ent.last_epoch, ent.old_intra_c);
            p_tmp.SetPropFG(ent.weight, 0, ent.new_intra_c);
            callback(&e_tmp, &p_tmp);
          }
        }
        {
          int _n = (int)tvb->version_num_;
          int _c = (k < 0) ? _n : _n - k;
          #ifdef TVB_STATS
          tvb_record_total.fetch_add(_c, std::memory_order_relaxed);
#endif
        }
      }
    }
#endif

#ifdef FINEGRAIN
    if (read_ts.e <= VBM.GetEndEpoch()) {
#else
    if (read_ts.e < VBM.GetEndEpoch()) {
#endif
      vb = VBM.GetLastVB();
      need_iterator = vb != nullptr;
    }
  }

  unsigned GetSrc() { return src_; }

 private:
  int edge_num_ = 0;
  unsigned tmp_ev = 0;
  unsigned merge_times = 0;
  unsigned edges_and_versions, num;
  unsigned capacity = 0;

  HashTable2 M;
  dst_t src_;

  tbb::spin_rw_mutex spin_rw_lock;

  VersionBlockManager VBM;

  // EdgeItem* start;
  // EdgeItem* tmp_item;
  EdgeWithIndex *start, *tmp_item;

  TmpVersionBlock* tmp_vb[MAXSIMULBATCH];
};

class GraphAlgorithms {
 public:
  template <typename EdgeCallback>
  static void for_each_edge(VertexEdges* eb, Composite read_ts,
                            EdgeCallback callback,
                            Satistical* count = nullptr) {
    if (!eb) return;
    bool need_iterator = false;
    VersionBlock* vb = nullptr;
    eb->iterate_edges(read_ts, need_iterator, vb, callback);

    if (need_iterator) {
      // std::cout << "\nyes : " << vb.get() << '\n';
      auto it = new VBIterator(vb, read_ts.e, count);
      while (!it->is_end_) {
        // std::cout << "in while : " << it->vb_.get() << '\n';
        auto start = it->vb_->start_;
        auto n = it->vb_->version_num_;
        int i = 0;
#ifdef FINEGRAIN
        if (it->vb_->timestamp_ == read_ts.e) {
          // Same-epoch VB: composite visibility with creation + invalidation
          Composite q = read_ts;
          for (i = 0; i < n; i++) {
            Composite inv(read_ts.e, (start + i + n)->GetIntraInv());
            if (!(start + i)->IsVisibleAtComposite(q, inv)) break;
            if (count != nullptr) count->reach_record();
            callback(start + i);
          }
        } else {
#endif
          for (i = 0; i < n && (start + i)->IsVisibleAt(read_ts.e); i++) {
            callback(start + i);
          }
#ifdef FINEGRAIN
        }
#endif

        it->getNext();
      }
      delete it;
    }
  }

  template <typename EdgeCallback>
  static void for_each_edge(VertexEdges* eb, Composite read_ts,
                            EdgeCallback callback) {
    if (!eb) return;
    bool need_iterator = false;
    VersionBlock* vb = nullptr;
    eb->iterate_edges(read_ts, need_iterator, vb, callback);

    if (need_iterator) {
      auto it = new VBIterator(vb, read_ts.e);
      while (!it->is_end_) {
        auto start = it->vb_->start_;
        auto n = it->vb_->version_num_;
#ifdef FINEGRAIN
        if (it->vb_->timestamp_ == read_ts.e) {
          Composite q = read_ts;
          for (int i = 0; i < n; i++) {
            Composite inv(read_ts.e, (start + i + n)->GetIntraInv());
            if (!(start + i)->IsVisibleAtComposite(q, inv)) break;
            callback(start + i);
          }
        } else {
#endif
          for (int i = 0; i < n && (start + i)->IsVisibleAt(read_ts.e); i++) {
            callback(start + i);
          }
#ifdef FINEGRAIN
        }
#endif
        it->getNext();
      }
      delete it;
    }
  }

  template <typename EdgeCallback>
  static void for_each_edge_condition(VertexEdges* eb, Composite read_ts,
                                      EdgeCallback callback) {
    if (!eb) return;
    bool need_iterator = false;
    VersionBlock* vb = nullptr;
    eb->iterate_edges_condition(read_ts, need_iterator, vb, callback);

    if (need_iterator) {
      auto it = new VBIterator(vb, read_ts.e);
      while (!it->is_end_) {
        auto start = it->vb_->start_;
        auto n = it->vb_->version_num_;
#ifdef FINEGRAIN
        if (it->vb_->timestamp_ == read_ts.e) {
          Composite q = read_ts;
          for (int i = 0; i < n; i++) {
            Composite inv(read_ts.e, (start + i + n)->GetIntraInv());
            if (!(start + i)->IsVisibleAtComposite(q, inv)) break;
            if (callback(start + i)) {
              delete it;
              return;
            }
          }
        } else {
#endif
          for (int i = 0; i < n && (start + i)->IsVisibleAt(read_ts.e); i++) {
            if (callback(start + i)) {
              delete it;
              return;
            }
          }
#ifdef FINEGRAIN
        }
#endif
        it->getNext();
      }
      delete it;
    }
  }
  template <typename EdgeCallback>
  static void for_each_edge_with_property(VertexEdges* eb, Composite read_ts,
                                          EdgeCallback callback) {
    if (!eb) return;
    bool need_iterator = false;
    VersionBlock* vb = nullptr;
    eb->iterate_edges_with_property(read_ts, need_iterator, vb, callback);

    if (need_iterator) {
      auto it = new VBIterator(vb, read_ts.e);
      while (!it->is_end_) {
        auto start = it->vb_->start_;
        auto n = it->vb_->version_num_;
#ifdef FINEGRAIN
        if (it->vb_->timestamp_ == read_ts.e) {
          // Same-epoch VB: composite visibility with creation + invalidation
          Composite q = read_ts;
          for (int i = 0; i < n; i++) {
            Composite inv(read_ts.e, (start + i + n)->GetIntraInv());
            if (!(start + i)->IsVisibleAtComposite(q, inv)) break;
            callback(start + i, start + i + n);
          }
        } else {
#endif
          for (int i = 0; i < n && (start + i)->IsVisibleAt(read_ts.e); i++) {
            callback(start + i, start + i + n);
          }
#ifdef FINEGRAIN
        }
#endif
        it->getNext();
      }
      delete it;
    }
  }
};

#endif  // VERTEXEDGES_H

#include "Transaction.h"

// void VersionData::commit() { unlock(); }

void Transaction::COMMIT() {
  // if (insert_cnt_ && one_edge_.versionData_ptr_) {
  //   one_edge_.versionData_ptr_->commit();
  // }
  // if (insert_cnt_ > 1)
  //   for (auto& it : *edges_to_insert) {
  //     if (it.versionData_ptr_ != nullptr) it.versionData_ptr_->commit();
  //     // assert(it.versionData_ptr_ != nullptr);
  //   }
  // vb_data_->add_txn2();
}

void Transaction::insertedge(dst_t* edge) {
  ds->insert_edge_block(edge, epoch_, this);
}

void Transaction::deletedge(dst_t* edge) {
  ds->delete_edge_block(edge, epoch_, this);
}

void Transaction::execute() {
  ds->insert_edge_block(one_edge_.edge, epoch_, this);
  // return;
  if (edges_to_insert)
    for (auto& it : *edges_to_insert) {
      ds->insert_edge_block(it.edge, epoch_, this);
      // assert(it.versionData_ptr_ != nullptr);
    }
}

void Transaction::commit() {
#ifdef FINEGRAIN
  // Ensure intra_c is fetched (lazy, fetched once from epoch counter)
  if (!intra_c_) intra_c_ = vb_data_->fetch_add_intra();
  // vb_data_->add_txn();
  for (auto eb : EB_vec_) {
    eb->unleashReadLock();
  }
#endif
}
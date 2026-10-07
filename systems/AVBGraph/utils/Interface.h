#ifndef UTILS_INTERFACE_H
#define UTILS_INTERFACE_H

#include <data-structure/data_types.h>
#include <memory>

class VersionBlock;
class TmpVersionBlock;
class VersionData;

class VertexEdgesInterface {
 public:
  virtual unsigned Transform(TmpVersionBlock* tvb, VersionBlock* vb) = 0;
  virtual void unleashReadLock() = 0;
};

class VBDataInterface {
 public:
  virtual void registerVersionBlock(TmpVersionBlock* vb) = 0;
  virtual void VB_pushback(TmpVersionBlock* vb, unsigned threadid) = 0;
  virtual unsigned add_txn() = 0;
  virtual intra_t fetch_add_intra() = 0;
};

class Transaction;
class VBManagerInterface {
 public:
  // virtual bool getVersionBlock(epoch_t ts, VersionBlock*& vb,
  //                              Transaction* txn) = 0;
  // virtual bool getVersionBlock(VersionBlock*& vb) = 0;

  // virtual bool checkVersionBlockExist(epoch_t ts) = 0;

  virtual void GetReadLock() = 0;
  virtual void GetWriteLock() = 0;
  virtual void UnLock() = 0;
  virtual void PushVB(int& edge_num, VersionBlock* nw) = 0;
};

class VersionedTopologyInterface {
 public:
  virtual bool insert_edge_block(dst_t* edge, epoch_t epoch,
                                 Transaction* txn) = 0;
  virtual bool delete_edge_block(dst_t* edge, epoch_t epoch,
                                 Transaction* txn) = 0;

  // virtual shared_ptr<VersionBlock> getVersionBlock(epoch_t epoch) = 0;
  // virtual bool getFirstBlock(epoch_t epoch,
  //                            std::shared_ptr<VersionBlock>& vb) = 0;
  // virtual void getReadLock() = 0;
  // virtual void unleashReadLock() = 0;
};

#endif  // UTILS_INTERFACE_H
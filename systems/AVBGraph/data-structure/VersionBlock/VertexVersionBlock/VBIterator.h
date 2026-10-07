
#ifndef VBITERATOR_H
#define VBITERATOR_H

#include <memory>

#include "VersionBlock.h"
#include "utils/utils.h"

class VBIterator {
 public:
  // read VB
  VersionBlock* vb_;
  bool has_weight_, is_end_;
  epoch_t version_;

  bool FindIndex(Satistical* count = nullptr);

  // read ptr
  EdgeWithIndex* index_;

  VBIterator();
  VBIterator(VersionBlock* vb, epoch_t version, Satistical* count = nullptr);
  ~VBIterator();

  /**
   * @brief Get the Cur VB version's number
   *
   * end condition : cannot find own version
   *
   * @return true : success get the next
   * @return false : fail to get the next because of the end of all VB
   */
  bool getCur(unsigned& num);

  /**
   * @brief Get the Next objec
   *
   * @return true
   * @return false
   */
  void getNext(Satistical* count = nullptr);
  // void release() { vb_->ReleaseReadMutex(); }
};

#endif  // VBITERATOR_H
#include "VBIterator.h"

void XH(std::vector<std::pair<epoch_t, unsigned>>::iterator& ptr,
        epoch_t version) {
  while (ptr->first > version) {
    ptr--;
  }
}
bool VBIterator::FindIndex(Satistical* count) {
  if (count == nullptr) {
    index_ = vb_->start_;
#ifdef FINEGRAIN
    if (vb_->timestamp_ == version_) {  // fine-grained
      if (index_ == nullptr) return true;
      return false;
    }
#endif
    while (index_ == nullptr || index_->block > version_) {
// if (index_) std::cout << index_->block;
#ifdef FINEGRAIN
      if (vb_->last_epoch_ < version_) {  // fine-grained
        return true;
      }
#else
      if (vb_->last_epoch_ <= version_) {
        return true;
      }
#endif
      vb_ = vb_->last_vb_;
      if (!vb_) return true;
      index_ = vb_->start_;
#ifdef FINEGRAIN
      if (vb_->timestamp_ == version_) {  // fine-grained
        if (index_ == nullptr) return true;
        return false;
      }
#endif
    }
    return false;
  }

  index_ = vb_->start_;
  count->reach_record();
  while (index_ == nullptr || index_->block > version_) {
    if (vb_->last_epoch_ <= version_) {
      return true;
    }
    vb_ = vb_->last_vb_;
    if (!vb_) return true;
    index_ = vb_->start_;
    count->reach_record();
  }
  return false;
}

// Default constructor.
VBIterator::VBIterator()
    : vb_(nullptr), version_(0), index_(0), is_end_(false) {}

VBIterator::VBIterator(VersionBlock* vb, epoch_t version, Satistical* count)
    : version_(version), is_end_(false) {
  vb_ = vb;
  // find the first version
  is_end_ = FindIndex(count);
}

// Destructor.
VBIterator::~VBIterator() {
  if (vb_ != nullptr) {
    // vb_->ReleaseReadMutex();
    vb_ = nullptr;
  }
}

// Get the current object.
bool VBIterator::getCur(unsigned& num) {
  num = *(unsigned*)index_;
  return true;
}

void VBIterator::getNext(Satistical* count) {
#ifdef FINEGRAIN
  if (vb_->last_epoch_ < version_) {  // fine-grained
#else
  if (vb_->last_epoch_ <= version_) {
#endif
    return void(is_end_ = true);
  }
  vb_ = vb_->last_vb_;
  if (!vb_) { is_end_ = true; return; }
  is_end_ = FindIndex(count);
}

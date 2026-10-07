#include "MemoryPoolLib.h"

int getCurSizeTable(int curSize2) {
  if (curSize2 < PRIMETABLE[8]) {
    for (int i = 0; i < 8; i++) {
      if (curSize2 == PRIMETABLE[i]) {
        return PRIMETABLE[i + 1];
      }
    }
  } else {
    return curSize2 * 2;
  }
  return -1;
}
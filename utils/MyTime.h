//
// Created by masitan on 11/22/24.
//

#ifndef MST_SORTLEDTON_MYTIME_H
#define MST_SORTLEDTON_MYTIME_H

#include <chrono>
#include <string>
double PrintFunctionTime(std::function<void()> function, std::string str) {
  auto start = std::chrono::high_resolution_clock::now();
  // Record the beginning of the measured interval.
  function();
  // Record the end of the measured interval.
  auto end = std::chrono::high_resolution_clock::now();
  // Compute elapsed time.
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
  // Report elapsed time in milliseconds.
  std::cout << str << " Elapsed time: " << duration.count() << "ms\n\n";
  return duration.count();
}

#endif  // MST_SORTLEDTON_MYTIME_H

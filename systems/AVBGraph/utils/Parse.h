
#ifndef PARSE_H
#define PARSE_H

#include <map>
#include <set>

#include "CLI11.hpp"

struct Args {
  std::map<std::string, std::string> PreFileMap{
      {"dota", "/home/masitan/gfe_system/gfe_data/dota-league.e"},
      {"graph24",
       "/home/masitan/gfe_system/gfe_data/graph500-24/graph500-24.e"},
      {"datagen-8_4-fb",
       "/home/masitan/gfe_system/gfe_data/datagen-8_4-fb.e"},
      {"datagen-7_9-fb",
       "/home/masitan/gfe_system/gfe_data/datagen-7_9-fb.e"},
      {"datagen-8_4-fb-weighted",
       "/home/masitan/gfe_system/gfe_data/datagen-8_4-fb-weighted.e"},
      {"wiki-Talk", "/home/masitan/gfe_system/gfe_data/wiki-Talk.e"}};
  CLI::App app{"AVB Test Program"};

  bool read_test = false;
  bool con_wr_test = false;
  bool single_read_work = false;

  bool file_input = false;
  bool permute_ = false;
  bool check_ = false;
  std::string input_file = "";
  std::string algorithm_ = "";
  unsigned limit_edge_nums = 0;
  int Spruce_wt = 64;
  int read_thread = 64;
  int bfsroot = 0;
  bool csr = false;

  bool gen_and_output = false;

  void ParseArgs(int argc, char** argv);

  bool IsFileInput() { return file_input; }
  bool TouchLimitEdgeNum(unsigned edge_num) {
    return limit_edge_nums == edge_num;
  }
};

#endif  // PARSE_H

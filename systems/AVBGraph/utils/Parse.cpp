#include "Parse.h"

void Args::ParseArgs(int argc, char** argv) {
  std::string preset_input_file_key = "";
  std::set<std::string> preset_input_file_names;
  for (const auto& pair : PreFileMap) {
    preset_input_file_names.insert(pair.first);
  }

  app.add_flag("-g,--gen-and-output", gen_and_output, "Generate and output a file");
  app.add_flag("--read", read_test, "Run the read benchmark");
  app.add_option("-r, --read-thread", read_thread, "Number of reader threads (default: 64)");
  app.add_option("--write-thread", Spruce_wt, "Number of writer threads (default: 64)");
  app.add_option("-e,--edge-limit", limit_edge_nums,
                 "Limit input edges; 0 means no limit (default)");
  app.add_flag("-p,--permute", permute_, "Shuffle input edges");
  app.add_flag("-c,--check", check_, "Check results");
  app.add_flag("--con", con_wr_test, "Run concurrent reads and writes");
  app.add_flag("-s,--single", single_read_work,
               "Run one multithreaded reader task (disabled by default)");
  app.add_option("--bfs-root", bfsroot, "BFS source vertex ID");

  app.add_option("-a,--algorithm", algorithm_, "Algorithm (supported: sum)");
  app.add_flag("--csr", csr, "Use the CSR baseline");

  auto* group = app.add_option_group("Input file (mutually exclusive options)");
  auto* prefile_opt =
      group
          ->add_option("--prefile", preset_input_file_key,
                       "Preset input file key: dota/dota-league/graph24/datagen-7_9-fb/datagen-8_4-fb/wiki-Talk")
          ->check(CLI::IsMember(
              preset_input_file_names));  // Validate the key against the preset list.
  group->add_option("-f,--file", input_file, "Input .e edge-list file");
  group->require_option(0, 1);

  app.callback([&]() {
    // Resolve the preset only when --prefile is explicitly supplied.
    if (prefile_opt->count() > 0) {
      auto it = PreFileMap.find(preset_input_file_key);
      if (it != PreFileMap.end()) {
        input_file = it->second;
      }
    }
  });

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    app.exit(e);
  }
  if (!input_file.empty()) {
    file_input = true;
  }
}

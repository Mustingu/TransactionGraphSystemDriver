#include "Parse.h"

#include <cstdlib>
#include <iostream>

namespace bench {
void Args::ParseArgs(int argc, char** argv) {
  std::string preset_input_file_key = "";
  std::set<std::string> preset_input_file_names;
  for (const auto& pair : PreFileMap) {
    preset_input_file_names.insert(pair.first);
  }

  app.add_flag("-g,--gen-and-output", gen_and_output, "Generate and output a file");
  app.add_flag("--graphalytics", graphalytics_,
               "Graphalytics-like static graph evaluation");
  app.add_flag("--common-algorithms", common_algorithms_,
               "opt-in shared PR/BFS/SSSP/WCC/LCC/CDLP kernels (static evaluation only)");
  app.add_flag("--quiet", quiet_,
               "suppress per-execution reader/progress prints (default)");
  app.add_flag("!--verbose", quiet_,
               "enable per-execution reader/progress prints for diagnostics");
  app.add_flag("!--no-collect-results", collect_results_,
               "disable graph algorithm result materialisation so processing_ms "
               "reflects pure algorithm time; correct-only runs keep this on");
  app.add_option("--repetitions", repetitions_,
                 "Static evaluation repetitions, default 1")
      ->check(CLI::PositiveNumber);
  app.add_option("--pr-iterations", pr_iterations_,
                 "PageRank iterations in static evaluation")
      ->check(CLI::PositiveNumber);
  app.add_option("--damping", pr_damping_,
                 "PageRank damping factor in static evaluation")
      ->check(CLI::Range(0.0, 1.0));
  app.add_option("--cdlp-iterations", cdlp_iterations_,
                 "CDLP max iterations in static evaluation")
      ->check(CLI::PositiveNumber);
  app.add_option("--output", output_file_,
                 "Static evaluation result output file");
  app.add_option("--edge-mode", edge_mode_,
                 "Static graph edge mode: undirected or directed")
      ->check(CLI::IsMember({"undirected", "directed"}));
  app.add_flag("--read", read_test, "Run the read benchmark");
  app.add_option("-r, --read-thread", read_thread, "Number of reader threads (default: 64)");
  app.add_option("--write-thread", Spruce_wt, "Number of writer threads (default: 64)");
  app.add_option("-e,--edge-limit", limit_edge_nums,
                 "Limit input edges; 0 means no limit (default)");
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "-p" || std::string(argv[i]) == "--permute") {
      permute_explicit_ = true;
      break;
    }
  }
  app.add_flag("-p,--permute", permute_, "Shuffle input edges (enabled by default in Graphalytics mode)");
  app.add_flag("-c,--check", check_, "Check results");
  auto* con_opt = app.add_flag("--con", con_wr_test, "Run concurrent reads and writes");
  auto* reference_schedule_opt = app.add_flag(
      "--reference-schedule", reference_schedule_,
      "Diagnostic: run a single Teseo reader on the controlling thread, matching the reference driver");
  reference_schedule_opt->needs(con_opt);
  app.add_flag("-s,--single", single_read_work,
               "Run one multithreaded reader task (disabled by default)");
  app.add_option("--root", root, "Source vertex ID for BFS or SSSP");

  app.add_option("-a,--algorithm", algorithm_, "Algorithm: pr/bfs/sssp/wcc/lcc/cdlp; static Graphalytics also accepts all or a comma-separated list, run sequentially after one load");
  app.add_option("--system", system_,
                 "Backend: avb/gtx/slt/rs/lg/teseo/csr; SLT requires bench_run_slt")
      ->check(CLI::IsMember({"avb", "AVB", "gtx", "GTX", "slt", "SLT",
                            "sortledton", "Sortledton", "rs", "RS",
                            "rapidstore", "RapidStore", "lg", "livegraph",
                            "LiveGraph", "teseo", "Teseo", "TESEO", "csr",
                            "CSR"}));

  auto* tasks_opt =
      app.add_option("--tasks", read_tasks,
                     "Algorithms for concurrent reader tasks; list length determines task count (e.g. "
                     "--tasks bfs pr sssp).");
  reference_schedule_opt->needs(tasks_opt);
  auto* repeat_opt =
      app.add_option("--repeat", repeat_count,
                     "Number of input-edge passes for concurrent writes (default: 1)");

  auto* mix_opt =
      app.add_option("--mix", mix_,
                     "Mixed-update workload: id/idm/m100 (disabled by default)");
  auto* rounds_opt =
      app.add_option("--rounds", rounds_,
                     "Update rounds; mixed defaults to 5, HotSet to ceil(5/f)")
          ->check(CLI::PositiveNumber);
  app.add_flag("--stall-abort", stall_abort_,
               "Raise SIGABRT on heartbeat STALL to produce a core dump");
  app.add_option("--wseed", wseed_, "Workload random seed (default: 42)");
  auto* hotset_opt =
      app.add_option("--hotset", hotset_,
                     "Hot-edge fraction for concurrent HotSet updates: (0,1]; requires --con")
          ->check(CLI::PositiveNumber)
          ->check(CLI::Range(0.0, 1.0));
  hotset_opt->needs(con_opt);
  hotset_opt->needs(tasks_opt);
  hotset_opt->excludes(mix_opt, repeat_opt);
  auto* hotset_file_opt =
      app.add_option("--hotset-file", hotset_file_,
                     "Candidate hot-edge file generated by hotset_gen")
          ->check(CLI::ExistingFile);
  hotset_file_opt->needs(con_opt);
  hotset_file_opt->needs(tasks_opt);
  hotset_file_opt->excludes(hotset_opt, mix_opt, repeat_opt);
  auto* hotset_update_pct_opt =
      app.add_option("--hotset-update-pct", hotset_update_pct_,
                     "HotSet update transaction percentage: 0/20/40/60/80/100; "
                     "remaining delete:insert=1:1 (default: 100)")
          ->check(CLI::IsMember({"0", "20", "40", "60", "80", "100"}));
  hotset_update_pct_opt->needs(con_opt);
  hotset_update_pct_opt->needs(tasks_opt);
  auto* progress_file_opt =
      app.add_option("--progress-file", progress_file_,
                     "Periodically write concurrent progress to a sidecar for partial-result recovery after a hang or crash");
  progress_file_opt->needs(con_opt);
  auto* graphlog_opt = app.add_option(
      "--graphlog", graphlog_,
      "Diagnostic: replay a reference GFE graphlog and run one algorithm in the 10%-90% progress window");
  graphlog_opt->needs(con_opt);
  graphlog_opt->needs(tasks_opt);
  graphlog_opt->excludes(hotset_opt, mix_opt, repeat_opt);
  auto* churn_opt = app.add_flag(
      "--churn",
      churn_,
      "Diagnostic: after loading, replace each old edge with an absent edge to simulate graphlog churn");
  churn_opt->needs(con_opt);
  churn_opt->needs(tasks_opt);
  churn_opt->needs(repeat_opt);
  churn_opt->excludes(hotset_opt, mix_opt, graphlog_opt);
  auto* split_update_opt = app.add_flag(
      "--split-update",
      split_update_,
      "Diagnostic: after loading, delete and reinsert the same edge in separate transactions");
  split_update_opt->needs(con_opt);
  split_update_opt->needs(tasks_opt);
  split_update_opt->needs(repeat_opt);
  split_update_opt->excludes(hotset_opt, mix_opt, graphlog_opt, churn_opt);
  auto* teseo_churn_opt = app.add_flag(
      "--teseo-churn",
      teseo_churn_,
      "Teseo graph-wide graphlog-like churn (default: 5*|E| logical replacements)");
  teseo_churn_opt->needs(con_opt);
  teseo_churn_opt->needs(tasks_opt);
  teseo_churn_opt->excludes(repeat_opt, hotset_opt, mix_opt, graphlog_opt,
                            churn_opt, split_update_opt);

  auto* group = app.add_option_group("Input file (mutually exclusive options)");
  auto* prefile_opt =
      group
          ->add_option("--prefile", preset_input_file_key,
                       "Preset input file key: dota/dota-league/graph24/datagen-7_9-fb/datagen-8_4-fb/wiki-Talk/edit-enwiki")
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
    std::exit(app.exit(e));
  }
  rounds_explicit_ = rounds_opt->count() > 0;
  if (hotset_update_pct_opt->count() > 0 && hotset_opt->count() == 0 &&
      hotset_file_opt->count() == 0) {
    std::cerr << "--hotset-update-pct requires --hotset or --hotset-file\n";
    std::exit(2);
  }
  root_explicit_ = app.get_option("--root")->count() > 0;
  if (!input_file.empty()) {
    file_input = true;
  }
}
}  // namespace bench

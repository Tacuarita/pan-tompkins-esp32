# C++ implementation and review

After each C++ implementation pass, run clang-tidy before handing off the work or marking the ticket complete.

1. Configure the affected host and ESP32-S3 builds to export `compile_commands.json`. Analyze the changed C++ translation units and those that include changed project headers, using the compilation database for each affected configuration. Use `--header-filter` to include the project's headers when the configuration does not already cover them.
2. Run `clang-tidy -p <build-dir> --warnings-as-errors='*' <translation-units>` with the project's check configuration when present. Fix the findings, rebuild, rerun the affected tests, and repeat clang-tidy until it reports zero findings in the analyzed project code. Keep checks enabled and resolve the underlying findings without adding suppressions.
3. Record the clang-tidy version, commands, build configurations checked, and final result in the handoff. A missing tool, missing compilation database, or analysis error blocks verification. Restore the analysis setup or report the specific blocker and leave the C++ ticket incomplete.

[LLVM clang-tidy documentation](https://clang.llvm.org/extra/clang-tidy/)

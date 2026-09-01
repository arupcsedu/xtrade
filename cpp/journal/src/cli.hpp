#ifndef AEGIS_JOURNAL_CLI_HPP
#define AEGIS_JOURNAL_CLI_HPP

namespace aegis::journal::cli {

[[nodiscard]] int inspect_main(int argc, char** argv);
[[nodiscard]] int verify_main(int argc, char** argv);
[[nodiscard]] int repair_main(int argc, char** argv);
[[nodiscard]] int export_main(int argc, char** argv);
[[nodiscard]] int replay_main(int argc, char** argv);

} // namespace aegis::journal::cli

#endif // AEGIS_JOURNAL_CLI_HPP

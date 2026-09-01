#include "../src/cli.hpp"

int main(const int argc, char** argv) {
  return aegis::journal::cli::replay_main(argc, argv);
}

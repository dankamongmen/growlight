#define DOCTEST_CONFIG_IMPLEMENT
#include "main.h"

auto main(int argc, const char **argv) -> int {
  if(!setlocale(LC_ALL, "")){
    std::cerr << "Couldn't set locale based on user preferences!" << std::endl;
    return EXIT_FAILURE;
  }
  const char* lang = getenv("LANG");
  if(lang == nullptr){
    std::cerr << "Warning: LANG wasn't defined" << std::endl;
  }else{
    std::cout << "Running with LANG=" << lang << std::endl;
  }
  const char* term = getenv("TERM");
  // ubuntu's buildd sets TERM=unknown, fuck it, handle this atrocity
  if(term == nullptr || strcmp(term, "unknown") == 0){
    std::cerr << "TERM wasn't defined, exiting with success" << std::endl;
    return EXIT_SUCCESS;
  }
  std::cout << "Running with TERM=" << term << std::endl;
  doctest::Context context;

  context.setOption("order-by", "name"); // sort the test cases by their name

  context.applyCommandLine(argc, argv);

  // overrides
  context.setOption("no-breaks", true); // don't break in the debugger when assertions fail

  int res = context.run(); // run

  if(context.shouldExit()){ // important - query flags (and --exit) rely on the user doing this
    return res;             // propagate the result of the tests
  }

  return res; // the result from doctest is propagated here as well
}

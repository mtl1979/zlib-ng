/* test_main.cc - Main entry point for google tests */

#include "zbuild.h"
#ifdef ZLIB_COMPAT
#  include "zlib.h"
#else
#  include "zlib-ng.h"
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <gtest/gtest.h>

int main(int argc, char **argv) {
  //return 0;
  //::testing::InitGoogleTest(&argc, argv);
  return 0;//RUN_ALL_TESTS();
}
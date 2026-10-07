#pragma once
#include <iostream>

#include "driver.h"
#include "ittnotify.h"
#include "utils/commandLineParser.hpp"

int main(int argc, char** argv) {
  __itt_pause();
  commandLineParser& parser = commandLineParser::get_instance();
  parser.parse("/home/masitan/gfe_system/RapidStore/config.cfg");

  wrapper::execute(parser.get_driver_config());
  return 0;
}

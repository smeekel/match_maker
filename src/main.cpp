#include "app.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

void usage()
{
  std::cerr << "matchmaker --database <path>\n"
            << "The database path may also be set with MATCHMAKER_DATABASE.\n";
}

} // namespace

int main(int argc, char** argv)
{
  std::string database;
  if( const char* env = std::getenv("MATCHMAKER_DATABASE") )
  {
    database = env;
  }
  for( int i = 1; i < argc; ++i )
  {
    const std::string arg = argv[i];
    if( arg == "--help" || arg == "-h" )
    {
      usage();
      return 0;
    }
    if( arg == "--database" && i + 1 < argc )
    {
      database = argv[++i];
      continue;
    }
    std::cerr << "unknown argument: " << arg << '\n';
    usage();
    return 2;
  }
  if( database.empty() )
  {
    usage();
    return 2;
  }
  try
  {
    matchmaker::App app(std::move(database));
    app.start();
    app.install_signals();
    app.wait();
  }
  catch( const std::exception& ex )
  {
    std::cerr << ex.what() << '\n';
    return 1;
  }
  return 0;
}

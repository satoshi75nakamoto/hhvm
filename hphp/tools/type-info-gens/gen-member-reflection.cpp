/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/tools/debug-parser/debug-parser.h"

#include "hphp/runtime/base/member-reflection.h"

#include "hphp/util/assertions.h"
#include "hphp/util/file.h"

#include <boost/program_options.hpp>

#include <folly/Format.h>
#include <folly/Singleton.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

namespace {

///////////////////////////////////////////////////////////////////////////////

using namespace debug_parser;

const std::string kProgramDescription =
  "Generate member reflection helpers from debug-info";

constexpr bool actually_run =
#if defined(NDEBUG) || defined(HHVM_ENABLE_MEMBER_REFLECTION)
  true;
#else
  false;
#endif

std::size_t size_of(const Type& type,
                    const std::unique_ptr<TypeParser>& parser) {
  return type.match<std::size_t>(
    [&](const VoidType*) -> std::size_t { always_assert(false); },
    [&](const FuncType*) -> std::size_t { always_assert(false); },
    [&](const MemberType*) -> std::size_t { always_assert(false); },

    [&](const PtrType*) { return sizeof(void*); },
    [&](const RefType*) { return sizeof(void*); },
    [&](const RValueRefType*) { return sizeof(void*); },

    [&](const ConstType* t) { return size_of(t->modified, parser); },
    [&](const VolatileType* t) { return size_of(t->modified, parser); },
    [&](const RestrictType* t) { return size_of(t->modified, parser); },

    [&](const ArrType* t) {
      return t->count ? *t->count * size_of(t->element, parser) : 0;
    },
    [&](const ObjectType* t) {
      always_assert(!t->incomplete);
      auto const obj = parser->getObject(t->key);
      return obj.size;
    }
  );
}

void generate_entry(const Object& object, std::ostream& o,
                    const std::unique_ptr<TypeParser>& parser) {
  o << folly::format(
    "  {{\n"
    "    \"{}\",\n"
    "    [](const void* base, const void* internal) -> const char* {{\n"
    "      auto const diff = reinterpret_cast<const char*>(internal) -\n"
    "                        reinterpret_cast<const char*>(base);\n"
    "      (void)diff;\n",
    object.name.name
  );

  auto const gen_range_check = [&] (const Object::Member& member,
                                    std::size_t base_off,
                                    std::size_t last_end) -> size_t {
    if (!member.offset) return 0; // static

    auto const off = base_off + *member.offset;
    auto const size = size_of(member.type, parser);

    auto const name = member.name.empty()
      ? folly::format("union@{}", off).str()
      : member.name;

    if (last_end < off) {
      o << folly::format("      // hole ({})\n", off - last_end);
    }

    o << "      " << folly::format(
      "if ({} <= diff && diff < {}) return \"{}\"; // size {}\n",
      off, off + size, name, size
    );
    return off + size;
  };

  size_t last_end = 0;
  for (auto const& base : object.bases) {
    if (!base.offset) continue;
    auto const base_object = parser->getObject(base.type.key);

    for (auto const& member : base_object.members) {
      last_end = std::max(last_end,
          gen_range_check(member, *base.offset, last_end));
    }
  }

  for (auto const& member : object.members) {
    last_end = std::max(last_end, gen_range_check(member, 0, last_end));
  }

  o << "      return nullptr;\n"
    << "    }\n"
    << "  }";
}

size_t NumThreads = 24;

void generate(const std::string& source_executable, std::ostream& o) {
  o << "#include <string>\n";
  o << "#include <unordered_map>\n\n";
  o << "#include \"hphp/util/portability.h\"\n\n";

  o << "extern \"C\" {\n\n"
    << "EXTERNALLY_VISIBLE auto "
    << HPHP::detail::kMemberReflectionTableName
    << " =\n"
       "  std::unordered_map<\n"
       "    std::string,\n"
       "    const char*(*)(const void*, const void*)\n"
       "  >\n"
       "{\n";

  if (actually_run) {
    auto reflectables = std::unordered_set<std::string> {
#define X(name) "HPHP::"#name,
      HPHP_REFLECTABLES
#undef X
    };

    auto const parser = TypeParser::make(source_executable, NumThreads);
    auto first = true;

    parser->forEachObject(
      [&](const ObjectType& type) {
        if (type.incomplete) return;
        if (type.name.linkage != ObjectTypeName::Linkage::external) return;

        // Assume the first, complete, external definition is the canonical one.
        if (!reflectables.contains(type.name.name)) return;
        reflectables.erase(type.name.name);

        if (first) {
          first = false;
        } else {
          o << ",\n";
        }
        generate_entry(parser->getObject(type.key), o, parser);
      }
    );
  }

  o << "\n};\n\n";
  o << "}";
}

///////////////////////////////////////////////////////////////////////////////

}

int main(int argc, char** argv) {
  folly::SingletonVault::singleton()->registrationComplete();
  namespace po = boost::program_options;

  po::options_description desc{"Allowed options"};
  desc.add_options()
    ("help", "produce help message")
    ("install_dir",
       po::value<std::string>(),
       "directory to put generated code in")
    ("fbcode_dir", po::value<std::string>(), "ignored")
    ("source_file",
       po::value<std::string>()->required(),
       "filename to read debug-info from")
    ("dep",
       po::value<std::vector<std::string>>(),
       "just here so we can add dependencies")
    ("output_file",
       po::value<std::string>()->required(),
       "filename of generated code")
    ("num_threads", po::value<int>(), "number of parallel threads");

  try {
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if (vm.contains("help")) {
      std::cout << kProgramDescription << "\n\n"
                << desc << std::endl;
      return 1;
    }

    if (vm.contains("num_threads")) {
      auto n = vm["num_threads"].as<int>();
      if (n > 0) {
        NumThreads = n;
      } else {
        std::cerr << "\nIllegal num_threads=" << n << "\n";
        return 1;
      }
    }

    po::notify(vm);

    auto const source_executable = vm["source_file"].as<std::string>();
    auto const output_filename = vm.contains("install_dir")
      ? folly::sformat(
          "{}{}{}",
          vm["install_dir"].as<std::string>(),
          HPHP::FileUtil::getDirSeparator(),
          vm["output_file"].as<std::string>())
      : vm["output_file"].as<std::string>();

    try {
      std::ofstream output_file{output_filename};
      generate(source_executable, output_file);
    } catch (const debug_parser::Exception& exn) {
      std::cerr << "\nError generating member reflection utilities:\n"
                << exn.what() << std::endl << std::endl;
      return 1;
    }
  } catch (const po::error& e) {
    std::cerr << e.what() << "\n\n"
              << kProgramDescription << "\n\n"
              << desc << std::endl;
    return 1;
  }

  return 0;
}

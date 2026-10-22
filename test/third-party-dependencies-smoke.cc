#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "snappy.h"
#include "re2/re2.h"

#define REQUIRE(condition) do { \
  if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " << #condition << '\n'; \
    std::exit(1); \
  } \
} while (false)

int main() {
  std::string source;
  for (int i = 0; i != 4096; ++i) source += "parkable-string-\xE4\xB8\xAD\0";
  source.append("\0binary\0", 8);
  std::vector<char> compressed(snappy::MaxCompressedLength(source.size()));
  size_t compressed_size = 0;
  snappy::RawCompress(source.data(), source.size(), compressed.data(), &compressed_size);
  size_t uncompressed_size = 0;
  REQUIRE(snappy::GetUncompressedLength(compressed.data(), compressed_size, &uncompressed_size));
  REQUIRE(uncompressed_size == source.size());
  std::string restored(uncompressed_size, '\0');
  REQUIRE(snappy::RawUncompress(compressed.data(), compressed_size, restored.data()));
  REQUIRE(restored == source);
  REQUIRE(!snappy::RawUncompress(compressed.data(), compressed_size - 1, restored.data()));

  RE2 regex("https://example\\.test/[a-z]+/[0-9]+");
  REQUIRE(regex.ok());
  REQUIRE(RE2::FullMatch("https://example.test/worker/123", regex));
  REQUIRE(!RE2::FullMatch("https://example.test/worker/123/extra", regex));
  REQUIRE(!RE2::FullMatch("https://other.test/worker/123", regex));
  RE2::Options options;
  options.set_log_errors(false);
  REQUIRE(!RE2("[", options).ok());

  std::cout << "Snappy binary/truncation and RE2 URL filtering passed\n";
}

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// Parses a flat JSON object {"key": <number>, ...} and calls fn(key, value)
// for each entry. Returns false if the file cannot be opened.
template <typename Fn>
static bool parse_flat_floats(const std::string& path, Fn fn) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  if (sz <= 0) {
    fclose(f);
    return false;
  }
  rewind(f);
  std::vector<char> buf((size_t)sz + 1, '\0');
  fread(buf.data(), 1, (size_t)sz, f);
  fclose(f);

  const char* p = buf.data();
  const char* end = p + sz;

  while (p < end) {
    while (p < end && *p != '"' && *p != '}') ++p;
    if (p >= end || *p == '}') break;
    ++p;  // skip opening '"'

    const char* key = p;
    while (p < end && *p != '"') ++p;
    int klen = (int)(p - key);
    if (p < end) ++p;  // skip closing '"'

    while (p < end && *p != ':' && *p != '}') ++p;
    if (p >= end || *p == '}') break;
    ++p;

    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
      ++p;
    if (p >= end) break;

    char* endp;
    float v = (float)strtod(p, &endp);
    if (endp == p) {
      ++p;
      continue;
    }
    p = endp;

    fn(std::string_view(key, (size_t)klen), v);
  }
  return true;
}

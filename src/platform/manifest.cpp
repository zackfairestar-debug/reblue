/**
 * @file    platform/manifest.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/manifest.h"

#include <charconv>

#include <toml++/toml.h>

#include "platform/http.h"

namespace bd::platform {
namespace {

constexpr size_t kSHA256Length = 64;

bool IsHexDigest(std::string_view s) {
  if (s.size() != kSHA256Length)
    return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

bool ReadArtifact(const toml::table &t, Artifact &out, std::string &error) {
  out.url = t["url"].value_or(std::string{});
  out.sha256 = t["sha256"].value_or(std::string{});
  out.size = static_cast<u64>(t["size"].value_or(i64{0}));
  if (out.url.empty()) {
    error = "artifact has no url";
    return false;
  }
  if (!IsHexDigest(out.sha256)) {
    error = "artifact '" + out.url + "' has no valid sha256";
    return false;
  }
  return true;
}

bool ParseRoot(std::string_view text, i64 expected, toml::table &root,
               std::string &error) {
  try {
    root = toml::parse(text);
  } catch (const toml::parse_error &e) {
    error = std::string("not valid TOML: ") + e.what();
    return false;
  }
  const i64 schema = root["schema"].value_or(i64{0});
  if (schema != expected) {
    error = "schema " + std::to_string(schema) + ", expected " +
            std::to_string(expected);
    return false;
  }
  return true;
}

// Relative to the document that named it.
std::string ResolveUrl(const std::string &base, const std::string &url) {
  if (url.empty() || url.starts_with("http://") || url.starts_with("https://"))
    return url;
  const auto scheme = base.find("://");
  if (scheme == std::string::npos)
    return url;
  if (url.front() == '/') {
    const auto host = base.find('/', scheme + 3);
    return host == std::string::npos ? base + url : base.substr(0, host) + url;
  }
  const auto slash = base.find_last_of('/');
  return slash <= scheme + 2 ? base + "/" + url
                             : base.substr(0, slash + 1) + url;
}

// Both documents come off the same client, so this is the one place a fetch
// failure turns into a message.
bool FetchText(const std::string &url, std::string &body, std::string &error) {
  const HTTPResult result = HTTP::Get(url, body);
  if (result.Succeeded())
    return true;
  error = result.error.empty() ? "HTTP " + std::to_string(result.status)
                               : result.error;
  return false;
}

} // namespace

int Version::Compare(std::string_view a, std::string_view b) {
  auto next = [](std::string_view &s) -> u32 {
    while (!s.empty() && (s.front() < '0' || s.front() > '9'))
      s.remove_prefix(1);
    u32 value = 0;
    const auto [end, ec] =
        std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc()) {
      s = {};
      return 0;
    }
    s.remove_prefix(static_cast<size_t>(end - s.data()));
    return value;
  };
  while (!a.empty() || !b.empty()) {
    const u32 lhs = next(a);
    const u32 rhs = next(b);
    if (lhs != rhs)
      return lhs < rhs ? -1 : 1;
  }
  return 0;
}

std::string_view AppManifest::PlatformKey() {
#if defined(_WIN32)
  return "win-amd64";
#elif defined(__APPLE__)
#if defined(__aarch64__)
  return "mac-arm64";
#else
  return "mac-amd64";
#endif
#elif defined(__ANDROID__)
  // No artifact is published under this key, so Android never offers updates.
  return "android-arm64";
#elif defined(__aarch64__)
  return "linux-arm64";
#else
  return "linux-amd64";
#endif
}

const Artifact *AppManifest::ArtifactForThisPlatform() const {
  const auto it = artifacts.find(std::string(PlatformKey()));
  return it == artifacts.end() ? nullptr : &it->second;
}

std::optional<AppManifest> AppManifest::Parse(std::string_view text,
                                              std::string &error) {
  toml::table root;
  if (!ParseRoot(text, kAppManifestSchema, root, error))
    return std::nullopt;

  AppManifest m;
  m.schema = kAppManifestSchema;
  m.content_url = root["content_url"].value_or(std::string{});

  const auto *app = root["app"].as_table();
  if (app == nullptr) {
    error = "no [app] table";
    return std::nullopt;
  }
  m.app_version = (*app)["version"].value_or(std::string{});
  m.notes_url = (*app)["notes_url"].value_or(std::string{});
  if (m.app_version.empty()) {
    error = "[app] has no version";
    return std::nullopt;
  }

  if (const auto *artifacts = (*app)["artifacts"].as_table()) {
    for (const auto &[key, node] : *artifacts) {
      const auto *entry = node.as_table();
      if (entry == nullptr)
        continue;
      Artifact a;
      if (!ReadArtifact(*entry, a, error))
        return std::nullopt;
      m.artifacts.emplace(std::string(key.str()), std::move(a));
    }
  }
  return m;
}

std::optional<AppManifest> AppManifest::Fetch(const std::string &url,
                                              std::string &error) {
  std::string body;
  if (!FetchText(url, body, error))
    return std::nullopt;
  auto m = Parse(body, error);
  if (m) {
    m->content_url = ResolveUrl(url, m->content_url);
    for (auto &[key, artifact] : m->artifacts)
      artifact.url = ResolveUrl(url, artifact.url);
  }
  return m;
}

std::optional<ContentManifest> ContentManifest::Parse(std::string_view text,
                                                      std::string &error) {
  toml::table root;
  if (!ParseRoot(text, kContentManifestSchema, root, error))
    return std::nullopt;

  ContentManifest m;
  m.schema = kContentManifestSchema;

  const auto *content = root["content"].as_array();
  if (content == nullptr)
    return m;

  for (const auto &node : *content) {
    const auto *entry = node.as_table();
    if (entry == nullptr)
      continue;
    ContentEntry c;
    c.id = (*entry)["id"].value_or(std::string{});
    c.version = (*entry)["version"].value_or(i64{0});
    c.min_app = (*entry)["min_app"].value_or(std::string{});
    if (c.id.empty()) {
      error = "a [[content]] entry has no id";
      return std::nullopt;
    }
    Artifact a;
    if (!ReadArtifact(*entry, a, error))
      return std::nullopt;
    c.url = std::move(a.url);
    c.sha256 = std::move(a.sha256);
    c.size = a.size;
    m.packs.push_back(std::move(c));
  }
  return m;
}

std::optional<ContentManifest> ContentManifest::Fetch(const std::string &url,
                                                      std::string &error) {
  std::string body;
  if (!FetchText(url, body, error))
    return std::nullopt;
  auto m = Parse(body, error);
  if (m) {
    for (auto &pack : m->packs)
      pack.url = ResolveUrl(url, pack.url);
  }
  return m;
}

} // namespace bd::platform

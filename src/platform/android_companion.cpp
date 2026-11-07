/**
 * @file    platform/android_companion.cpp
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/android_companion.h"

#if defined(__ANDROID__)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <jni.h>

#include "core/memory_helpers.h"
#include "platform/android_companion_tex.h"
#include "vfs/key.h"
#include "engine/game.h"
#include "engine/game_tables.h"
#include "engine/gimmicks.h"
#include "engine/iss_event.h"
#include "engine/game_task.h"
#include "engine/menus/camp_main_task.h"
#include "engine/script_man_task.h"
#include "engine/sofdec_player.h"

namespace bd::platform::android {
namespace {

using namespace bd::engine;

// One snapshot every few engine steps is plenty for a panel the page polls at
// 5 Hz, and keeps the cost off most frames.
constexpr u32 kSampleEvery = 6;
// Markers and the bag change rarely and cost a table walk, so they refresh
// on a slower beat (counted in samples) or when the stage changes.
constexpr u32 kMarkersEvery = 15;
constexpr u32 kItemsEvery = 10;
// The panel polls every 200 ms while it is on screen. Past this without a
// poll (no second screen, the panel asleep or turned off) nothing is sampled.
constexpr i64 kIdleAfterMs = 1500;

constexpr u32 kMaxNameChars = 96;
constexpr size_t kMaxMarkers = 400;
constexpr size_t kMaxEnemies = 16;

// ---------------------------------------------------------------- JSON

// Everything leaves as ASCII: anything else is a \uXXXX escape, so the string
// NewStringUTF gets is valid modified UTF-8 whatever the engine text holds.
void AppendEscapedUnit(std::string &out, u32 unit) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "\\u%04x", unit & 0xFFFF);
  out += buf;
}

void AppendCodePoint(std::string &out, u32 cp) {
  if (cp == '"' || cp == '\\') {
    out += '\\';
    out += static_cast<char>(cp);
  } else if (cp >= 0x20 && cp < 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp < 0x10000) {
    AppendEscapedUnit(out, cp);
  } else if (cp <= 0x10FFFF) {
    cp -= 0x10000;
    AppendEscapedUnit(out, 0xD800 + (cp >> 10));
    AppendEscapedUnit(out, 0xDC00 + (cp & 0x3FF));
  }
}

void AppendUtf8(std::string &out, std::string_view s) {
  out += '"';
  for (size_t i = 0; i < s.size();) {
    const u8 c = static_cast<u8>(s[i]);
    u32 cp = 0;
    size_t n = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
      cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
      n = 2;
    } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
      cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
      n = 3;
    } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
      cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) |
           ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
      n = 4;
    } else {
      cp = '?';
    }
    AppendCodePoint(out, cp);
    i += n;
  }
  out += '"';
}

// An engine wide (big-endian UTF-16) string, already in the game language.
// Control codes the text renderer interprets are dropped.
std::string WideName(u32 va) {
  std::string out;
  if (!va)
    return out;
  for (u32 i = 0; i < kMaxNameChars; ++i) {
    const auto *ch = mem::try_at<const be_u16>(va + i * 2);
    if (!ch)
      break;
    const u16 c = *ch;
    if (!c)
      break;
    if (c < 0x20)
      continue;
    AppendCodePoint(out, c);
  }
  return out;
}

// Records keep a pointer to their localized name at +0x08
// (Camp_BuildMemberStatusPanel for phenomes; assumed the same for items).
constexpr u32 kRecordName = 0x08;

std::string RecordName(u32 record) {
  return record ? WideName(mem::try_load<u32>(record + kRecordName))
                : std::string{};
}

struct Json {
  std::string s;
  bool first = true;

  void Sep() {
    if (!first)
      s += ',';
    first = false;
  }
  void Key(const char *k) {
    Sep();
    s += '"';
    s += k;
    s += "\":";
  }
  void Open(char c) {
    s += c;
    first = true;
  }
  void Close(char c) {
    s += c;
    first = false;
  }
  void Num(const char *k, long long v) {
    Key(k);
    s += std::to_string(v);
  }
  void Float(const char *k, f32 v) {
    Key(k);
    if (!std::isfinite(v))
      v = 0.0f;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    s += buf;
  }
  void Bool(const char *k, bool v) {
    Key(k);
    s += v ? "true" : "false";
  }
  void Str(const char *k, std::string_view v) {
    Key(k);
    AppendUtf8(s, v);
  }
  // Already escaped (WideName output).
  void Raw(const char *k, std::string_view escaped) {
    Key(k);
    s += '"';
    s += escaped;
    s += '"';
  }
};

// ---------------------------------------------------------------- state

std::mutex g_mutex;
std::string g_snapshot = "{}";
u64 g_serial = 0;
std::string g_lastBody;

std::atomic<i64> g_lastPollMs{0};

// Set by the game thread once the game loop runs: the guest is up and the VFS
// mounted. Before that the panel must not ask for game art, since reading the
// game files or guest memory during boot brings the process down.
std::atomic<bool> g_engineRunning{false};
std::mutex g_localeMutex;
std::string g_locale; // what Key::Localized puts in place of "_xx"

i64 NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The minimap floor texture, copied raw on the game thread when the floor
// changes and decoded on the panel's request.
struct MinimapCapture {
  u32 db = 0;
  u32 texture = 0;
  u32 format = 0;
  u32 size = 0;
  XenosFetch fetch;
  std::vector<u8> raw;
  u64 serial = 0;
  std::vector<u8> png; // cached for 'serial'
  u64 pngSerial = 0;
};
std::mutex g_minimapMutex;
MinimapCapture g_minimap;

u32 g_stepCounter = 0;
u32 g_sampleCounter = 0;

// The leader's position, written every engine step while the panel polls, so
// the map can follow the player each frame instead of at the snapshot rate.
std::mutex g_posMutex;
std::string g_pos; // "x,z,yaw" or empty off the field

std::string g_markersStem;
std::string g_markersJson; // the "markers" array, cached
std::string g_tallyJson;   // "chests"/"points" pairs, cached
std::string g_itemsJson;   // the "items" array, cached
std::unordered_map<u32, std::string> g_itemNames;
std::unordered_map<u32, std::string> g_phenomeNames;

// Names are in the language latched at boot, so they are cached for the
// session. A miss (table not loaded yet) is not cached and is tried again.
std::string CachedName(std::unordered_map<u32, std::string> &cache, u32 id,
                       u32 record) {
  std::string name = RecordName(record);
  if (!name.empty())
    cache.emplace(id, name);
  return name;
}

std::string ItemName(u32 id) {
  auto it = g_itemNames.find(id);
  if (it != g_itemNames.end())
    return it->second;
  return CachedName(g_itemNames, id, GameTables::Get().ItemRecord(id));
}

std::string PhenomeName(u32 id) {
  auto it = g_phenomeNames.find(id);
  if (it != g_phenomeNames.end())
    return it->second;
  return CachedName(g_phenomeNames, id, GameTables::Get().PhenomeRecord(id));
}

// Walking the field: the leader is the player's to steer. Game::Mode() calls
// any busy loader slot "loading", and some areas stream all the time (the
// Ancient Ruins caverns kept a slot busy while the player walked), which hid
// the map and left "Loading..." in the header for the whole visit.
bool PlayerOnField(const Game &g) {
  return g.FieldGameplayActive() && g.FieldPlayerEntity().HasControl() &&
         g.ScriptManTask().SceneState() !=
             bd::engine::kSceneStateStageTransition;
}

const char *ModeName(const Game &g) {
  if (!g.IsReady())
    return "boot";
  if (IssEvent::LiveCount() > 0 || SofdecPlayer::Playing())
    return "event";
  switch (g.Mode()) {
  case EngineMode::Battle:
    return "battle";
  case EngineMode::Loading:
    if (PlayerOnField(g))
      return CampMainTask::Get() ? "menu" : "field";
    return "loading";
  case EngineMode::FieldTransition:
    return "loading";
  case EngineMode::FieldActive:
    return CampMainTask::Get() ? "menu" : "field";
  case EngineMode::TitleOrMenu:
    return g.FieldPlayerEntity() ? "menu" : "title";
  case EngineMode::Unknown:
    break;
  }
  return "boot";
}

void AppendMember(Json &j, const Player &c, bool leader, bool acting) {
  j.Sep();
  j.Open('{');
  const u32 slot = c.SlotId();
  j.Num("slot", slot);
  j.Num("pc", slot / kSlotIdStride);
  j.Num("lv", c.Level());
  j.Num("exp", c.Exp());
  j.Num("hp", c.HP());
  j.Num("maxHp", c.MaxHP());
  j.Num("mp", c.MP());
  j.Num("maxMp", c.MaxMP());
  j.Bool("leader", leader);
  j.Bool("acting", acting);
  const auto cls = c.ActiveClass();
  j.Str("cls", cls ? ToString(*cls) : "");
  j.Num("rank", cls ? c.ClassRank(*cls) : 0);

  // Every unlocked shadow class: [class index, rank, skill points].
  j.Key("classes");
  j.Open('[');
  for (u32 i = 0; i < kCharaClassCount; ++i) {
    const auto job = static_cast<CharaClass>(i);
    if (!c.IsClassUnlocked(job))
      continue;
    j.Sep();
    j.s += '[' + std::to_string(i) + ',' + std::to_string(c.ClassRank(job)) +
           ',' + std::to_string(c.ClassSP(job)) + ']';
  }
  j.Close(']');

  j.Key("status");
  j.Open('[');
  const u32 flags = c.StatusFlags();
  for (const auto bit : kCharaStatusBits) {
    if (!HasStatus(flags, bit) || bit == CharaStatus::kUnknown20)
      continue;
    j.Sep();
    AppendUtf8(j.s, ToString(bit));
  }
  j.Close(']');

  j.Key("acc");
  j.Open('[');
  const u32 worn = std::min<u32>(c.EquipCount(), kCharaEquipCount);
  for (u32 i = 0; i < worn; ++i) {
    const u32 id = c.Equipment(i);
    j.Sep();
    if (!id) {
      j.s += "null";
      continue;
    }
    j.Open('{');
    j.Num("id", id);
    j.Raw("name", PhenomeName(id));
    j.Close('}');
  }
  j.Close(']');
  j.Close('}');
}

void RefreshMarkers(const std::string &stem) {
  g_markersStem = stem;
  g_markersJson.clear();
  g_tallyJson.clear();
  if (stem.empty())
    return;
  auto &gm = Gimmicks::Get();
  if (!gm.IsReady() || !gm.Has(stem))
    return;

  Json j;
  j.Open('[');
  size_t n = 0;
  for (const Marker &mk : gm.Markers(stem)) {
    if (!mk.trackable || mk.collected || mk.kind == GimmickKind::Nothing)
      continue;
    if (++n > kMaxMarkers)
      break;
    j.Sep();
    j.Open('{');
    j.Str("k", ToString(mk.kind));
    j.Float("x", mk.x);
    j.Float("z", mk.z);
    if (mk.kind == GimmickKind::Item && mk.value)
      j.Raw("name", ItemName(mk.value));
    else if (mk.kind == GimmickKind::Gold)
      j.Num("v", mk.value);
    j.Close('}');
  }
  j.Close(']');
  g_markersJson = std::move(j.s);

  const Tally chests = gm.Chests(stem);
  const Tally points = gm.Points(stem);
  const Tally barriers = gm.Barriers(stem);
  g_tallyJson = "\"chests\":[" + std::to_string(chests.Found()) + "," +
                std::to_string(chests.total) + "],\"points\":[" +
                std::to_string(points.Found()) + "," +
                std::to_string(points.total) + "],\"barriers\":[" +
                std::to_string(barriers.Found()) + "," +
                std::to_string(barriers.total) + "]";
}

void RefreshItems(const Game &g) {
  g_itemsJson.clear();
  const auto items = g.ItemSaveData();
  if (!items)
    return;
  Json j;
  j.Open('[');
  for (size_t i = 0; i < items.SlotCount(); ++i) {
    const auto it = items.At(i);
    if (!it.id || !it.count)
      continue;
    j.Sep();
    j.Open('{');
    j.Num("id", it.id);
    j.Raw("name", ItemName(it.id));
    j.Num("count", it.count);
    j.Close('}');
  }
  j.Close(']');
  g_itemsJson = std::move(j.s);
}

// The floor texture a MiniMapDB holds: its texture holder's table (+0x794,
// the holder at +0x78C plus 8), the table's entries, and the first entry's
// texture, as Texlist::Texture walks the same chain.
constexpr u32 kDbTable = 0x794;
constexpr u32 kTableEntries = 0x04;
constexpr u32 kEntryTexture = 0x18;
// D3DTexture::Format, the fetch constant.
constexpr u32 kTextureFetch = 0x1C;

u32 FloorTexture(const MiniMapDB &db) {
  const u32 table = mem::try_load<u32>(db.Address() + kDbTable);
  const u32 entries = table ? mem::try_load<u32>(table + kTableEntries) : 0;
  return entries ? mem::try_load<u32>(entries + kEntryTexture) : 0;
}

// Copies the floor texture's tiled mip 0 when the floor (or what it holds)
// changed. Returns the capture serial, zero when there is none.
u64 CaptureMinimap(const MiniMapDB &db) {
  const u32 texture = FloorTexture(db);
  if (!texture)
    return 0;
  XenosFetch fetch;
  for (u32 i = 0; i < 6; ++i)
    fetch.dword[i] = mem::try_load<u32>(texture + kTextureFetch + i * 4);
  // A 2D texture fetch constant: type 2.
  if ((fetch.dword[0] & 3) != 2)
    return 0;

  std::lock_guard lock(g_minimapMutex);
  if (g_minimap.serial && g_minimap.db == db.Address() &&
      g_minimap.texture == texture && g_minimap.format == fetch.dword[1] &&
      g_minimap.size == fetch.dword[2])
    return g_minimap.serial;

  const size_t bytes = XenosMip0Bytes(fetch);
  const u32 base = fetch.BaseAddress();
  const u8 *src = bytes && base ? mem::try_at<const u8>(base) : nullptr;
  // Both ends, since try_at validates an address and not a range.
  if (!src || !mem::try_at<const u8>(base + u32(bytes) - 1))
    return 0;
  g_minimap.db = db.Address();
  g_minimap.texture = texture;
  g_minimap.format = fetch.dword[1];
  g_minimap.size = fetch.dword[2];
  g_minimap.fetch = fetch;
  g_minimap.raw.assign(src, src + bytes);
  ++g_minimap.serial;
  g_minimap.png.clear();
  return g_minimap.serial;
}

std::string BuildBody() {
  const Game &g = Game::Get();
  Json j;
  j.Open('{');
  const char *mode = ModeName(g);
  j.Str("mode", mode);
  if (!g.IsReady()) {
    j.Close('}');
    return j.s;
  }

  const auto script = g.ScriptManTask().Script();
  const std::string stem = script ? script.Name() : std::string{};
  j.Str("stage", stem);
  j.Str("location", script ? script.DisplayName() : std::string{});

  const auto items = g.ItemSaveData();
  if (items)
    j.Num("gold", items.Gold());

  // Party, in formation order, and the rest of the roster.
  const auto entity = g.FieldPlayerEntity();
  const auto battle = g.BattleTask();
  const u32 actor = battle ? battle.CurrentActor().Address() : 0;
  const u32 leaderAddr = entity.Leader().Address();
  std::vector<u32> inParty;
  j.Key("party");
  j.Open('[');
  for (const auto &m : entity.Party()) {
    const Player c = m.Chara();
    if (!c)
      continue;
    inParty.push_back(m.Address());
    AppendMember(j, c, m.Address() == leaderAddr,
                 actor && actor == m.Address());
    if (inParty.size() >= 8)
      break;
  }
  j.Close(']');
  j.Key("reserve");
  j.Open('[');
  size_t rosterCount = 0;
  for (const auto &m : entity.Roster()) {
    if (++rosterCount > 16)
      break;
    if (std::find(inParty.begin(), inParty.end(), m.Address()) !=
        inParty.end())
      continue;
    // The roster holds every playable character from the start; the ones
    // still to join have no unlock bit yet, and listing them put "Marumaro
    // learns the Black Mage class" in the journal hours before he joins.
    const Player c = m.Chara();
    if (c && g.GameTask().IsSlotUnlocked(c.SlotId()))
      AppendMember(j, c, false, false);
  }
  j.Close(']');

  // Battle: the enemy line-up. The manager handle is only valid in the step
  // that captured it, which is the one that just ran.
  if (battle && std::string_view(mode) == "battle") {
    j.Key("battle");
    j.Open('{');
    j.Num("phase", battle.Phase());
    j.Key("enemies");
    j.Open('[');
    const size_t count = std::min(battle.EnemyCount(), kMaxEnemies);
    for (size_t i = 0; i < count; ++i) {
      const auto task = battle.EnemyAt(i);
      const Enemy e = task.Chara();
      if (!e)
        continue;
      j.Sep();
      j.Open('{');
      j.Num("type", e.TypeId());
      j.Num("hp", e.HP());
      j.Num("maxHp", e.MaxHP());
      j.Bool("acting", actor && actor == task.Address());
      j.Close('}');
    }
    j.Close(']');
    j.Close('}');
  }

  // Map: the leader and what is still to find on this stage.
  const Player leader = entity.Leader().Chara();
  const bool onField = leader && !stem.empty() &&
                       (std::string_view(mode) == "field" ||
                        std::string_view(mode) == "menu");
  j.Key("map");
  j.Open('{');
  j.Bool("available", onField);
  if (onField) {
    if (stem != g_markersStem || g_sampleCounter % kMarkersEvery == 0)
      RefreshMarkers(stem);
    const Vec3 p = leader.Position();
    j.Float("x", p[0]);
    j.Float("z", p[2]);
    j.Float("yaw", leader.Rotation()[1]);
    // The floor the compass is showing, with what places the player on it:
    // texture UV = (offset + world) / scale (AreaMap's toScreen), the compass
    // turned by texRot + offRot, its arrow by texRot + plyRot.
    const auto db = g.ScriptManTask().MiniMap().Floor();
    if (db && db.Ready()) {
      j.Num("tex", static_cast<long long>(CaptureMinimap(db)));
      j.Float("texW", db.TexW());
      j.Float("texH", db.TexH());
      j.Float("scaleX", db.ScaleX());
      j.Float("scaleZ", db.ScaleZ());
      j.Float("offX", db.OffsetX());
      j.Float("offZ", db.OffsetZ());
      j.Float("dispW", db.DispW());
      j.Float("dispH", db.DispH());
      j.Float("texRot", db.TexRot());
      j.Float("offRot", db.OffsetRot());
      j.Float("plyRot", db.PlyRot());
    }
    if (!g_tallyJson.empty()) {
      j.Sep();
      j.s += g_tallyJson;
    }
    j.Key("markers");
    j.s += g_markersJson.empty() ? "[]" : g_markersJson;
  }
  j.Close('}');

  if (g_itemsJson.empty() || g_sampleCounter % kItemsEvery == 0)
    RefreshItems(g);
  j.Key("items");
  j.s += g_itemsJson.empty() ? "[]" : g_itemsJson;

  j.Close('}');
  return j.s;
}

} // namespace

void CompanionSample() {
  if (!g_engineRunning.load(std::memory_order_relaxed)) {
    // The language code the VFS puts in place of "_xx", read here on the game
    // thread where guest memory is safe to touch.
    const std::string probe = vfs::Key::FromRelative("a_xx.b").Localized().string();
    std::string code;
    if (probe != "a_xx.b" && probe.size() > 3 && probe.front() == 'a' &&
        probe.compare(probe.size() - 2, 2, ".b") == 0)
      code = probe.substr(1, probe.size() - 3);
    {
      std::lock_guard lock(g_localeMutex);
      g_locale = code;
    }
    g_engineRunning.store(true, std::memory_order_release);
  }
  // Nobody is reading: no second screen, or the panel asleep or off.
  if (NowMs() - g_lastPollMs.load(std::memory_order_relaxed) > kIdleAfterMs)
    return;
  {
    // Cheap: the leader's transform only, every step.
    std::string pos;
    const Game &g = Game::Get();
    if (g.IsReady() && (g.Mode() == EngineMode::FieldActive ||
                        (g.Mode() == EngineMode::Loading && PlayerOnField(g)))) {
      const Player leader = g.FieldPlayerEntity().Leader().Chara();
      if (leader) {
        const Vec3 p = leader.Position();
        const f32 yaw = leader.Rotation()[1];
        if (std::isfinite(p[0]) && std::isfinite(p[2]) && std::isfinite(yaw)) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%.2f,%.2f,%.4f",
                        static_cast<double>(p[0]), static_cast<double>(p[2]),
                        static_cast<double>(yaw));
          pos = buf;
        }
      }
    }
    std::lock_guard lock(g_posMutex);
    g_pos = std::move(pos);
  }
  if (++g_stepCounter % kSampleEvery != 0)
    return;
  ++g_sampleCounter;

  std::string body = BuildBody();
  std::lock_guard lock(g_mutex);
  if (body == g_lastBody)
    return;
  g_lastBody = std::move(body);
  ++g_serial;
  // {"serial":N, + the rest of the body after its opening brace.
  std::string snap = "{\"serial\":" + std::to_string(g_serial);
  if (g_lastBody.size() > 2) {
    snap += ',';
    snap.append(g_lastBody, 1, std::string::npos);
  } else {
    snap += '}';
  }
  g_snapshot = std::move(snap);
}

} // namespace bd::platform::android

extern "C" {

JNIEXPORT jstring JNICALL
Java_org_reblue_android_DualScreenCompanion_nativeState(JNIEnv *env, jclass) {
  using namespace bd::platform::android;
  g_lastPollMs.store(NowMs(), std::memory_order_relaxed);
  std::string copy;
  {
    std::lock_guard lock(g_mutex);
    copy = g_snapshot;
  }
  return env->NewStringUTF(copy.c_str());
}

// The leader's live position, "x,z,yaw" (empty off the field). Polled every
// animation frame by the map, so it only copies a string.
JNIEXPORT jstring JNICALL
Java_org_reblue_android_DualScreenCompanion_nativePos(JNIEnv *env, jclass) {
  using namespace bd::platform::android;
  g_lastPollMs.store(NowMs(), std::memory_order_relaxed);
  std::string copy;
  {
    std::lock_guard lock(g_posMutex);
    copy = g_pos;
  }
  return env->NewStringUTF(copy.c_str());
}

namespace {

jbyteArray ToJava(JNIEnv *env, const std::vector<u8> &bytes) {
  if (bytes.empty())
    return nullptr;
  jbyteArray out = env->NewByteArray(static_cast<jsize>(bytes.size()));
  if (out)
    env->SetByteArrayRegion(out, 0, static_cast<jsize>(bytes.size()),
                            reinterpret_cast<const jbyte *>(bytes.data()));
  return out;
}

} // namespace

// True once the game loop runs. The panel's WebView must not exist before
// that: the guest arena is mapped MAP_FIXED at a fixed address and would land
// on whatever Chromium had already reserved there.
JNIEXPORT jboolean JNICALL
Java_org_reblue_android_DualScreenCompanion_nativeEngineReady(JNIEnv *, jclass) {
  return bd::platform::android::g_engineRunning.load(std::memory_order_acquire)
             ? JNI_TRUE
             : JNI_FALSE;
}

// A texture of the installed game as PNG, or null. Runs on a WebView thread.
JNIEXPORT jbyteArray JNICALL
Java_org_reblue_android_DualScreenCompanion_nativeGameTexture(JNIEnv *env,
                                                              jclass,
                                                              jstring path) {
  using namespace bd::platform::android;
  if (!path || !g_engineRunning.load(std::memory_order_acquire))
    return nullptr;
  const char *chars = env->GetStringUTFChars(path, nullptr);
  if (!chars)
    return nullptr;
  const std::string p(chars);
  env->ReleaseStringUTFChars(path, chars);
  std::string locale;
  {
    std::lock_guard lock(g_localeMutex);
    locale = g_locale;
  }
  return ToJava(env, GameTexturePng(p, locale));
}

// The current minimap floor as PNG, or null before one has been captured.
JNIEXPORT jbyteArray JNICALL
Java_org_reblue_android_DualScreenCompanion_nativeMinimap(JNIEnv *env,
                                                          jclass) {
  using namespace bd::platform::android;
  std::lock_guard lock(g_minimapMutex);
  if (!g_minimap.serial)
    return nullptr;
  if (g_minimap.pngSerial != g_minimap.serial) {
    DecodedImage image;
    g_minimap.png.clear();
    if (DecodeXenosTexture(g_minimap.fetch, g_minimap.raw.data(),
                           g_minimap.raw.size(), image))
      g_minimap.png = EncodePng(image);
    g_minimap.pngSerial = g_minimap.serial;
  }
  return ToJava(env, g_minimap.png);
}

} // extern "C"

#endif // __ANDROID__

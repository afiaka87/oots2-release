#include "visits.hpp"

#if FAST_TRAVEL_VISITED_ONLY

namespace warp {
namespace {

using GetBoolFn = bool (*)(void* manager, bool* out, u32 hash);
using GetString64Fn = bool (*)(void* manager, const char** out, u32 hash);
using GetFileSizeFn = u32 (*)(s64* size, FileHandle file);
using CloseFileFn = void (*)(FileHandle file);
using CreateFileFn = u32 (*)(const char* path, s64 size);
using WriteFileFn = u32 (*)(FileHandle file, s64 offset, const void* buffer, size_t size, const WriteOption& option);
using SetFileSizeFn = u32 (*)(FileHandle file, s64 size);
using ReadFileFn = u32 (*)(size_t* read, FileHandle file, s64 offset, void* buffer, size_t size);

// Set when Link first wakes as an adult in the Temple of Time, at the story's first age change (the menu's own age
// change does not set it).
constexpr u32 kGrownUpOnce = murmur3("Market_Child_Firsttime_GotoAdult");
constexpr u32 kNextArea = murmur3("Sequence_ReservedNextBanc");
constexpr u32 kNextSpawn = murmur3("Sequence_ReservedNextStartPosName");
constexpr u32 kCurrentArea = murmur3("Sequence_CurrentBanc");
constexpr int kPrefixLength = sizeof("IsVisitLocation.") - 1;
constexpr int kReadEvery = 30;     // unpaused controller updates between reads of the flags and Link's position
constexpr int kArrivalWait = 120;  // updates, after a trip's reservation clears, for the area flag to name its end
constexpr float kNearDistance = 5.0f;              // map units (about metres): a position spot this close is reached
constexpr u64 kReadGuardTicks = 20ull * 19200000;  // 20 s of system ticks
constexpr int kTextCapacity = 64;                  // a GameData String64 value with its end

struct PlaceHashes {
    u32 values[kPlaceCount];
};

constexpr PlaceHashes placeHashes() {
    PlaceHashes hashes{};
    for (int i = 0; i < kPlaceCount; ++i) { hashes.values[i] = murmur3(kPlaceFlags[i]); }
    return hashes;
}

constexpr PlaceHashes kPlaceHashes = placeHashes();
// Two of the hashes as the game's GameDataList has them.
static_assert(murmur3("IsVisitLocation.KokiriForest") == 0x2E315EB6);
static_assert(murmur3("IsVisitLocation.GerudoFortress") == 0x3D07B702);

// The record name of arriving in an area at a spawn point, as catalog.py's arrival_key: murmur3 of "<area>/<spawn>",
// with a mid-air variant ("_Air") named as its ground point.
constexpr u32 arrivalKey(const char* area, const char* spawn) {
    char text[2 * kTextCapacity + 1] = {};
    int n = 0;
    for (int i = 0; i < kTextCapacity - 1 && area[i] != '\0'; ++i) { text[n++] = area[i]; }
    text[n++] = '/';
    int length = 0;
    while (length < kTextCapacity - 1 && spawn[length] != '\0') { ++length; }
    if (length > 4 && spawn[length] == '\0' && sameText(spawn + length - 4, "_Air")) { length -= 4; }
    for (int i = 0; i < length; ++i) { text[n++] = spawn[i]; }
    return murmur3(text);
}

constexpr bool spotKeysMatch() {
    for (const Area& area : kAreas) {
        for (int spot = area.first; spot < area.first + area.count; ++spot) {
            if (kSpots[spot].spawn[0] != '\0' && arrivalKey(area.banc, kSpots[spot].spawn) != kSpotKey[spot]) {
                return false;
            }
        }
    }
    return true;
}
static_assert(spotKeysMatch(), "catalog.py and arrivalKey name every named spot the same way");
static_assert(arrivalKey("LostWood_Child", "LostWood_Entrance_Air") == arrivalKey("LostWood_Child", "LostWood_Entrance"));

constexpr void addSpot(SpotSet& set, int spot) { set.words[spot / 64] |= 1ull << (spot % 64); }

int countSpots(const SpotSet& set) {
    int n = 0;
    for (const u64 word : set.words) { n += __builtin_popcountll(word); }
    return n;
}

int areaOfSpot(int spot) {
    for (int i = 0; i < kAreaCount; ++i) {
        if (spot >= kAreas[i].first && spot < kAreas[i].first + kAreas[i].count) { return i; }
    }
    return 0;
}

// ---- The spot record ------------------------------------------------------------------------------------------

// The spots reached in the loaded game. The controller update adds to it; on the file thread a save read replaces it
// and a save write copies it.
SpinLock s_recordLock;
SpotSet s_record;
u64 s_recordReadTick = 0;  // when a save read last replaced it
u32 s_recordReads = 0;     // save reads so far

// Adds a spot to the record and logs how Link reached it. False when it was already there.
bool reachSpot(int spot, const char* how) {
    {
        LockGuard guard(s_recordLock);
        if (hasSpot(s_record, spot)) { return false; }
        addSpot(s_record, spot);
    }
    const Area& area = kAreas[areaOfSpot(spot)];
    WTLOG("spot reached (%s): %s (%s) / %s", how, area.title, ageName(area.age), kSpots[spot].label);
    return true;
}

// ---- Trips between areas --------------------------------------------------------------------------------------

// A text flag's value, or nullptr when the game has no such flag.
const char* readText(void* manager, u32 hash) {
    const char* value = nullptr;
    if (!gameFunction<GetString64Fn>(kGetString64)(manager, &value, hash)) { return nullptr; }
    return value != nullptr ? value : "";
}

void copyText(char (&to)[kTextCapacity], const char* from) {
    int i = 0;
    for (; from != nullptr && i < kTextCapacity - 1 && from[i] != '\0'; ++i) { to[i] = from[i]; }
    to[i] = '\0';
}

// A trip the game makes (through a loading zone, a warp song, an owl or a story scene): it fills the reserved next
// area and spawn point, and clears them once the destination has loaded.
struct Trip {
    char area[kTextCapacity] = {};
    char spawn[kTextCapacity] = {};
    char from[kTextCapacity] = {};  // the area Link left
    bool byFastTravel = false;      // a Fast Travel warp was under way while it was reserved
};
enum class TripState : u8 { None, Reserved, Ended };
TripState s_tripState = TripState::None;
Trip s_trip;
int s_tripWait = 0;  // updates since the reservation cleared

// The spots a finished trip reaches: its arrival point, and the near side of the loading zone Link went through.
void placeTrip(const char* current) {
    s_tripState = TripState::None;
    if (s_trip.byFastTravel) {
        WTLOG("trip to %s / %s was a Fast Travel warp: nothing new", s_trip.area, s_trip.spawn);
        return;
    }
    if (current == nullptr || !sameText(s_trip.area, current)) {
        WTLOG("trip from %s to %s / %s ended in %s: not counted", s_trip.from, s_trip.area, s_trip.spawn,
              current != nullptr ? current : "(no area flag)");
        return;
    }
    const u32 key = arrivalKey(s_trip.area, s_trip.spawn);
    int listed = 0, added = 0;
    for (int spot = 0; spot < kSpotCount; ++spot) {
        if (kSpotKey[spot] != key) { continue; }
        ++listed;
        added += reachSpot(spot, "arrived there") ? 1 : 0;
    }
    for (const Doorway& doorway : kDoorways) {
        if (doorway.arrival != key || !sameText(kAreas[areaOfSpot(doorway.spot)].banc, s_trip.from)) { continue; }
        ++listed;
        added += reachSpot(doorway.spot, "went through it from this side") ? 1 : 0;
    }
    WTLOG("trip from %s to %s / %s: %d listed spot%s, %d new", s_trip.from, s_trip.area, s_trip.spawn, listed,
          listed == 1 ? "" : "s", added);
}

void noteTrip(void* manager, bool warping) {
    const char* next = readText(manager, kNextArea);
    if (next != nullptr && next[0] != '\0') {
        if (s_tripState == TripState::Ended) { placeTrip(readText(manager, kCurrentArea)); }
        if (s_tripState == TripState::None) {
            s_trip = {};
            copyText(s_trip.from, readText(manager, kCurrentArea));
            s_tripState = TripState::Reserved;
        }
        copyText(s_trip.area, next);
        copyText(s_trip.spawn, readText(manager, kNextSpawn));
        s_trip.byFastTravel = s_trip.byFastTravel || warping;
        return;
    }
    if (s_tripState == TripState::Reserved) {
        s_tripState = TripState::Ended;
        s_tripWait = 0;
    }
    if (s_tripState != TripState::Ended) { return; }
    const char* current = readText(manager, kCurrentArea);
    if ((current == nullptr || !sameText(s_trip.area, current)) && ++s_tripWait < kArrivalWait) { return; }
    placeTrip(current);
}

// Position spots (no spawn point name) are reached by coming near them, once the area has stayed the same for a read.
u32 s_nearArea = 0;  // hash of the area at the previous read (0: none, or a trip under way)

void noteNearSpots(void* manager) {
    const char* current = s_tripState == TripState::None ? readText(manager, kCurrentArea) : nullptr;
    const u32 area = current != nullptr && current[0] != '\0' ? murmur3(current) : 0;
    const bool settled = area != 0 && area == s_nearArea;
    s_nearArea = area;
    if (!settled) { return; }
    void* player = instanceAt(kPlayerInfoSlot);
    if (player == nullptr) { return; }
    const float* at = &field<float>(player, kPlayerInfoPosition);
    for (const Area& candidate : kAreas) {
        if (!sameText(candidate.banc, current)) { continue; }
        for (int spot = candidate.first; spot < candidate.first + candidate.count; ++spot) {
            const Spot& s = kSpots[spot];
            if (s.spawn[0] != '\0') { continue; }
            const float dx = at[0] - s.position[0], dy = at[1] - s.position[1], dz = at[2] - s.position[2];
            if (dx * dx + dy * dy + dz * dz <= kNearDistance * kNearDistance) { reachSpot(spot, "Link came near"); }
        }
    }
}

// ---- Places and the menu --------------------------------------------------------------------------------------

int s_untilRead = 0;
bool s_reported = false;
Visits s_last;
u64 s_missing = 0;  // places whose flag the game does not have (each logged once)
bool s_grownUpMissing = false;
u32 s_readsLogged = 0;

void logChange(const Visits& visits) {
    WTLOG("visited places: %d of %d recorded, spots reached %d of %d; adult list %s",
          __builtin_popcountll(visits.places), kPlaceCount, countSpots(visits.spots), kSpotCount,
          visits.adult ? "open" : "closed");
    const u64 added = visits.places & ~s_last.places;
    const u64 removed = s_last.places & ~visits.places;
    for (int i = 0; i < kPlaceCount; ++i) {
        if (((added >> i) & 1) != 0) { WTLOG("visited: %s", kPlaceFlags[i] + kPrefixLength); }
        if (((removed >> i) & 1) != 0) { WTLOG("no longer visited (another save?): %s", kPlaceFlags[i] + kPrefixLength); }
    }
}

// A visited place reading unvisited again means another game: a new game (a return to the title keeps the flags), so
// the record is cleared; unless a save read replaced it moments ago (loading a slot changes the flags just after
// reading it).
void clearWhenPlacesDrop(u64 places) {
    const u64 dropped = s_last.places & ~places;
    if (!s_reported || dropped == 0) { return; }
    bool cleared = false;
    {
        LockGuard guard(s_recordLock);
        if (s_recordReads == 0 || svcGetSystemTick() - s_recordReadTick > kReadGuardTicks) {
            s_record = {};
            cleared = true;
        }
    }
    const int count = __builtin_popcountll(dropped);
    WTLOG("%d visited place%s no longer visited (a new game?): %s", count, count == 1 ? "" : "s",
          cleared ? "the spot record is cleared" : "just after a save read, so the spot record is kept");
}

// After a save read, lists the record the slot brought.
void logRecordRead() {
    SpotSet record;
    u32 reads = 0;
    {
        LockGuard guard(s_recordLock);
        record = s_record;
        reads = s_recordReads;
    }
    if (reads == s_readsLogged) { return; }
    s_readsLogged = reads;
    WTLOG("spot record now from the save: %d spots", countSpots(record));
    for (int spot = 0; spot < kSpotCount; ++spot) {
        if (!hasSpot(record, spot)) { continue; }
        const Area& area = kAreas[areaOfSpot(spot)];
        WTLOG("in the record: %s (%s) / %s", area.title, ageName(area.age), kSpots[spot].label);
    }
}

// ---- The record file beside each save slot --------------------------------------------------------------------

constexpr u32 kRecordMagic = 0x52565446;  // the bytes "FTVR"
constexpr u32 kRecordVersion = 1;
constexpr int kRecordCapacity = 253;
constexpr int kPathCapacity = 256;
constexpr char kSlotFile[] = "/progress.sav";
constexpr char kRecordFile[] = "fasttravel.sav";

// The file: magic, version, entry count, then each recorded spot's name (kSpotKey). 1 KB, written whole.
struct RecordFile {
    u32 magic;
    u32 version;
    u32 count;
    u32 keys[kRecordCapacity];
};
static_assert(sizeof(RecordFile) == 1024 && kSpotCount <= kRecordCapacity);
constexpr size_t kRecordHeader = 3 * sizeof(u32);

SpinLock s_fileLock;  // guards s_file; held while the record file is open
RecordFile s_file;

bool startsWith(const char* text, const char* prefix) {
    for (; *prefix != '\0'; ++text, ++prefix) {
        if (*text != *prefix) { return false; }
    }
    return true;
}

int pathLength(const char* path) {
    int length = 0;
    while (length < kPathCapacity && path[length] != '\0') { ++length; }
    return length;
}

// A save-folder file: its name ends in ".sav" (progress, caption, option, account and the temporary save).
bool isSaveFile(const char* path, int length) {
    return length >= 4 && length < kPathCapacity && sameText(path + length - 4, ".sav");
}

// The record's path for a save slot's progress.sav ("save:/slot_00/progress.sav" -> "save:/slot_00/fasttravel.sav");
// false for every other file, the temporary save among them.
bool recordPathFor(const char* path, int length, char (&out)[kPathCapacity]) {
    constexpr int suffix = sizeof(kSlotFile) - 1;
    if (length >= kPathCapacity || length < suffix || !sameText(path + length - suffix, kSlotFile)) { return false; }
    const int keep = length - suffix + 1;  // through the folder's '/'
    bool slot = false;
    for (int i = 0; i + 5 <= keep; ++i) { slot = slot || startsWith(path + i, "slot_"); }
    if (!slot || keep + static_cast<int>(sizeof(kRecordFile)) > kPathCapacity) { return false; }
    for (int i = 0; i < keep; ++i) { out[i] = path[i]; }
    for (size_t i = 0; i < sizeof(kRecordFile); ++i) { out[keep + i] = kRecordFile[i]; }
    return true;
}

HOOK_DEFINE_TRAMPOLINE(OpenFileHook) {
    static u32 Callback(FileHandle* out, const char* path, int mode);
};

u32 openUnhooked(FileHandle* out, const char* path, int mode) { return OpenFileHook::Orig(out, path, mode); }

// Writes the record beside a slot the game is saving; the game's own commit of the save keeps it.
void writeRecord(const char* path) {
    LockGuard file(s_fileLock);
    s_file = {kRecordMagic, kRecordVersion, 0, {}};
    int spots = 0;
    {
        LockGuard guard(s_recordLock);
        spots = countSpots(s_record);
        for (int spot = 0; spot < kSpotCount; ++spot) {
            if (hasSpot(s_record, spot)) { s_file.keys[s_file.count++] = kSpotKey[spot]; }
        }
    }
    const u32 created = gameFunction<CreateFileFn>(kCreateFile)(path, sizeof(RecordFile));
    if (created != 0 && created != kPathAlreadyExists) {
        WTLOG("spot record not saved: creating %s gave %#x", path, created);
        return;
    }
    FileHandle handle{};
    const u32 opened = openUnhooked(&handle, path, kOpenWrite);
    if (opened != 0) {
        WTLOG("spot record not saved: opening %s gave %#x", path, opened);
        return;
    }
    u32 result = gameFunction<SetFileSizeFn>(kSetFileSize)(handle, sizeof(RecordFile));
    if (result == 0) {
        result = gameFunction<WriteFileFn>(kWriteFile)(handle, 0, &s_file, sizeof(RecordFile), WriteOption{kWriteFlush});
    }
    gameFunction<CloseFileFn>(kCloseFile)(handle);
    if (result != 0) {
        WTLOG("spot record not saved: writing %s gave %#x", path, result);
        return;
    }
    WTLOG("spot record saved: %d spots, in %s", spots, path);
}

// Reads a record file into s_file; nullptr when it holds a record this version knows, otherwise why not.
const char* loadRecordFile(const char* path) {
    FileHandle handle{};
    if (openUnhooked(&handle, path, kOpenRead) != 0) { return "no record beside this slot yet"; }
    s64 size = 0;
    size_t read = 0;
    u32 result = gameFunction<GetFileSizeFn>(kGetFileSize)(&size, handle);
    if (result == 0 && size >= static_cast<s64>(kRecordHeader)) {
        const size_t wanted = size < static_cast<s64>(sizeof(RecordFile)) ? static_cast<size_t>(size) : sizeof(RecordFile);
        result = gameFunction<ReadFileFn>(kReadFile)(&read, handle, 0, &s_file, wanted);
    }
    gameFunction<CloseFileFn>(kCloseFile)(handle);
    if (result != 0 || read < kRecordHeader) { return "the record file could not be read"; }
    if (s_file.magic != kRecordMagic || s_file.version != kRecordVersion || s_file.count > kRecordCapacity ||
        read < kRecordHeader + s_file.count * sizeof(u32)) {
        return "the record file is not one this version knows";
    }
    return nullptr;
}

// Reads the record beside a slot the game is reading and makes it the loaded game's. A missing or unreadable record
// counts as empty: that slot shows no spots until Link reaches them again.
void readRecord(const char* path) {
    LockGuard file(s_fileLock);
    SpotSet spots;
    int unknown = 0;
    const char* problem = loadRecordFile(path);
    for (u32 i = 0; problem == nullptr && i < s_file.count; ++i) {
        bool known = false;
        for (int spot = 0; spot < kSpotCount; ++spot) {
            if (kSpotKey[spot] != s_file.keys[i]) { continue; }
            addSpot(spots, spot);
            known = true;
        }
        unknown += known ? 0 : 1;
    }
    {
        LockGuard guard(s_recordLock);
        s_record = spots;
        s_recordReadTick = svcGetSystemTick();
        ++s_recordReads;
    }
    if (problem != nullptr) {
        WTLOG("spot record for %s: %s; it starts with no spots shown", path, problem);
        return;
    }
    WTLOG("spot record read: %d spots from %s (%d entries not in this build's list)", countSpots(spots), path, unknown);
}

// A save-file open: logged (path, reading or writing, result); a slot's progress.sav also writes or reads the record
// beside it, after the game's own open succeeded. Kept out of line so other file opens use little stack.
[[gnu::noinline]] void noteSaveFileOpen(const char* path, int length, int mode, u32 result) {
    const bool writing = (mode & kOpenWrite) != 0;
    WTLOG("save file open: %s for %s: result %#x", path, writing ? "writing" : "reading", result);
    char record[kPathCapacity];
    if (result != 0 || !recordPathFor(path, length, record)) { return; }
    if (writing) {
        writeRecord(record);
    } else {
        readRecord(record);
    }
}

// Every game file open passes here.
u32 OpenFileHook::Callback(FileHandle* out, const char* path, int mode) {
    const u32 result = Orig(out, path, mode);
    const int length = path != nullptr ? pathLength(path) : 0;
    if (isSaveFile(path, length)) { noteSaveFileOpen(path, length, mode, result); }
    return result;
}

}  // namespace

void noteVisits(void* manager, KnownAge age, bool warping) {
    if (manager == nullptr) { return; }
    noteTrip(manager, warping);
    if (s_untilRead > 0) {
        --s_untilRead;
        return;
    }
    s_untilRead = kReadEvery - 1;
    noteNearSpots(manager);
    const auto readBool = gameFunction<GetBoolFn>(kGetBool);
    Visits visits;
    for (int i = 0; i < kPlaceCount; ++i) {
        bool visited = false;
        if (!readBool(manager, &visited, kPlaceHashes.values[i])) {
            if (((s_missing >> i) & 1) == 0) {
                s_missing |= 1ull << i;
                WTLOG("no visited flag %s: that place stays hidden", kPlaceFlags[i]);
            }
            continue;
        }
        if (visited) { visits.places |= 1ull << i; }
    }
    bool grownUp = false;
    if (!readBool(manager, &grownUp, kGrownUpOnce)) {
        grownUp = false;
        if (!s_grownUpMissing) {
            s_grownUpMissing = true;
            WTLOG("no first-adult flag: the adult list opens only while Link is adult");
        }
    }
    visits.adult = grownUp || age == KnownAge::Adult;
    clearWhenPlacesDrop(visits.places);
    logRecordRead();
    {
        LockGuard guard(s_recordLock);
        visits.spots = s_record;
    }
    if (s_reported && sameVisits(visits, s_last)) { return; }
    logChange(visits);
    s_last = visits;
    s_reported = true;
    reportVisits(visits);
}

void installSaveRecordHook() { OpenFileHook::InstallAtOffset(kOpenFile.offset); }

}  // namespace warp

#endif

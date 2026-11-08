#include "warp.hpp"

#include "menu.hpp"

#include <atomic>

namespace warp {
namespace {

using SetString64Fn = void (*)(void* manager, const char* const* value, u32 hash);
using GetString64Fn = bool (*)(void* manager, const char** out, u32 hash);
using SetVec3Fn = void (*)(void* manager, const float* value, u32 hash);
using GetBoolFn = bool (*)(void* manager, bool* out, u32 hash);
using EventRequestInitFn = void (*)(void* request);
using EventRequestFn = bool (*)(void* eventManager, void* request);
using EventRequestMemberEndFn = void (*)(void* member);

constexpr u32 kNextArea = murmur3("Sequence_ReservedNextBanc");
constexpr u32 kNextSpawn = murmur3("Sequence_ReservedNextStartPosName");
constexpr u32 kNextPosition = murmur3("Sequence_ReservedNextPosition");
constexpr u32 kNextRotation = murmur3("Sequence_ReservedNextCancelSleepRot");
constexpr u32 kIsYoung = murmur3("Sequence_IsChildhood");
constexpr u32 kCurrentArea = murmur3("Sequence_CurrentBanc");

// A one-node event: wait 30 frames. Its end runs the game's after-event sequence, which loads the reserved area.
constexpr const char* kCarrierEvent = "DefaultParentEvent";
// The game's own age changes at the Master Sword pedestal (requested in vanilla by a trigger in front of it). Each
// plays the sword scene, swaps the equipment, sets the age, reserves the other age's Temple of Time and plays its
// arrival scene there. Both need Link in the Temple of Time of his current age.
constexpr const char* kToAdultEvent = "Market_Child_Field_ChildToAdult";
constexpr const char* kToYoungEvent = "Market_Adult_Field_AdultToChild";
constexpr const char* kYoungTemple = "Market_Child";
constexpr const char* kAdultTemple = "Market_Adult";
constexpr const char* kTempleSpawn = "LightPrelude";  // the Prelude of Light arrival, inside the temple

// Frames (player controller updates while not paused) to wait for a requested event to start before giving up.
constexpr int kStartTimeoutFrames = 900;
// Paused frames a chosen warp waits for the game to resume before it is refused.
constexpr int kPausedTimeoutFrames = 120;
// A step has arrived after this many unpaused updates in a row with no event pending, the reserved next area
// empty again (the game clears it after loading) and the current area name the step's area.
constexpr int kSettleFrames = 60;
// The same with the area name still different: the step counts as arrived, and the log says so.
constexpr int kMismatchSettleFrames = 600;
// Updates (paused or not) after a step's event ends before the step is given up as not finished.
constexpr int kArriveTimeoutUpdates = 18000;
// Unpaused frames a later step of an age change keeps asking while the game is not ready.
constexpr int kRetryFrames = 600;

alignas(16) unsigned char s_request[kEventRequestSize];

enum class StepKind : u8 { Warp, AgeChange };

struct Step {
    StepKind kind = StepKind::Warp;
    const char* area = "";             // Warp: the area to load. AgeChange: the area the age event loads.
    const char* spawn = "";            // Warp: the spawn point, or "" to arrive by position
    const float* position = nullptr;   // Warp by position: the point
    const char* label = "";            // for the log
    const char* event = kCarrierEvent;  // the event requested
    bool young = false;                // AgeChange: Link's age afterwards
    Status status = Status::Warping;   // what the menu says while the step runs
};

enum class Phase : u8 { Idle, Starting, WaitStart, WaitEnd, Arriving };

constexpr int kMaxSteps = 3;

struct Job {
    Step steps[kMaxSteps];
    int count = 0;
    int current = 0;
    Phase phase = Phase::Idle;
    u64 number = 0;  // the warp's number since the game started, for the log
    int frames = 0;  // unpaused updates in this phase
    // Arriving: updates since the event ended, paused ones among them, settled updates in a row, and the timing of
    // updates (system ticks), to learn what an area load looks like from here.
    int updates = 0;
    int paused = 0;
    int settled = 0;
    u64 endTick = 0;
    u64 lastTick = 0;
    u64 longestGap = 0;
};
Job s_job;

int s_pausedFrames = 0;
u64 s_warps = 0;
KnownAge s_linkAge = KnownAge::Unknown;

// Why the game's automatic saves are held right now (read by the save request hook on any thread), or None.
enum class SaveHold : u8 { None, AgeChange, Switch };
std::atomic<u8> s_saveHold{static_cast<u8>(SaveHold::None)};

// Entries claimed in a GameData queue: the low 20 bits of its count word.
u32 queueClaimed(void* manager, uintptr_t countOffset) { return field<u32>(manager, countOffset) & 0xFFFFF; }

bool queueHasRoom(void* manager, uintptr_t capacityOffset, uintptr_t countOffset, int needed) {
    const s32 capacity = field<s32>(manager, capacityOffset);
    return capacity > 0 && static_cast<s32>(queueClaimed(manager, countOffset)) + needed <= capacity;
}

bool eventPending(void* eventManager) {
    return eventManager != nullptr && field<u64>(eventManager, kEventManagerPending) != 0;
}

// A text flag's value, or nullptr when the game has no such flag.
const char* readText(void* manager, u32 hash) {
    const char* value = nullptr;
    if (!gameFunction<GetString64Fn>(kGetString64)(manager, &value, hash)) { return nullptr; }
    return value != nullptr ? value : "";
}

// Link's age flag (true: young); false when the game has no such flag.
bool readIsYoung(void* manager, bool* young) { return gameFunction<GetBoolFn>(kGetBool)(manager, young, kIsYoung); }

// Requests an event by name exactly as the SeqOneShotRequestEvent node does. Returns the request's acceptance.
bool requestEvent(void* eventManager, const char* name) {
    void* request = s_request;
    gameFunction<EventRequestInitFn>(kEventRequestInit)(request);
    char* buffer = field<char*>(request, kEventRequestNameBuffer);
    const s32 capacity = field<s32>(request, kEventRequestNameCapacity);
    bool accepted = false;
    if (buffer == nullptr || capacity <= 1) {
        WTLOG("event request set-up gave no name buffer (%p, capacity %d); not requested", buffer, capacity);
    } else {
        int length = 0;
        for (; name[length] != '\0' && length < capacity - 1; ++length) { buffer[length] = name[length]; }
        buffer[length] = '\0';
        accepted = gameFunction<EventRequestFn>(kEventRequest)(eventManager, request);
    }
    const auto end = gameFunction<EventRequestMemberEndFn>(kEventRequestMemberEnd);
    for (const uintptr_t member : kEventRequestMembers) {
        field<uintptr_t>(request, member) = mainBase() + kEventRequestMemberVtable;
        end(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(request) + member));
    }
    return accepted;
}

void writeText(void* manager, u32 hash, const char* text) {
    gameFunction<SetString64Fn>(kSetString64)(manager, &text, hash);
}

// Clears the reserved area so that no later event loads it.
void clearReservation(void* manager) {
    if (!queueHasRoom(manager, kString64QueueCapacity, kString64QueueCount, 2)) {
        WTLOG("could not clear the reserved area: text flag queue full");
        return;
    }
    writeText(manager, kNextArea, "");
    writeText(manager, kNextSpawn, "");
}

const Step& currentStep() { return s_job.steps[s_job.current]; }

void updateSaveHold() {
    SaveHold hold = SaveHold::None;
    if (s_job.phase != Phase::Idle && s_job.current + 1 < s_job.count) {
        hold = SaveHold::AgeChange;
    } else if (!autosaveOn()) {
        hold = SaveHold::Switch;
    }
    s_saveHold.store(static_cast<u8>(hold), std::memory_order_relaxed);
}

// Starts the current step: requests its event (and, for a warp, fills the reserved next-area slots). Returns
// Status::Ready when the game accepted it, otherwise what the menu should say.
Status startStep(void* manager, void* eventManager) {
    const Step& step = currentStep();
    const int n = static_cast<int>(s_job.number);
    if (manager == nullptr || eventManager == nullptr) { return Status::StillLoading; }
    if (eventPending(eventManager)) { return Status::SceneRunning; }
    const bool warp = step.kind == StepKind::Warp;
    const bool byPosition = warp && step.spawn[0] == '\0';
    if (warp && (!queueHasRoom(manager, kString64QueueCapacity, kString64QueueCount, 2) ||
                 (byPosition && !queueHasRoom(manager, kVec3QueueCapacity, kVec3QueueCount, 2)))) {
        WTLOG("warp %d: GameData queue near full (text %u/%d, vector %u/%d)", n,
              queueClaimed(manager, kString64QueueCount), field<s32>(manager, kString64QueueCapacity),
              queueClaimed(manager, kVec3QueueCount), field<s32>(manager, kVec3QueueCapacity));
        return Status::GameBusy;
    }
    const u32 flags = field<u32>(eventManager, kEventManagerFlags);
    if (!requestEvent(eventManager, step.event)) {
        WTLOG("warp %d step %d/%d: event request for %s refused (event manager flags %#x)", n, s_job.current + 1,
              s_job.count, step.event, flags);
        return Status::Refused;
    }
    if (warp) {
        writeText(manager, kNextArea, step.area);
        writeText(manager, kNextSpawn, step.spawn);
        if (byPosition) {
            static constexpr float kNoRotation[3] = {0.0f, 0.0f, 0.0f};
            gameFunction<SetVec3Fn>(kSetVec3)(manager, step.position, kNextPosition);
            gameFunction<SetVec3Fn>(kSetVec3)(manager, kNoRotation, kNextRotation);
        }
        WTLOG("warp %d step %d/%d to '%s': %s requested (flags were %#x); reserved %s at %s%s (thread %#lx)", n,
              s_job.current + 1, s_job.count, step.label, step.event, flags, step.area,
              byPosition ? "position" : "spawn point ", byPosition ? "" : step.spawn,
              static_cast<unsigned long>(threadKey()));
        if (byPosition) {
            WTLOG("warp %d position (%.3f, %.3f, %.3f), rotation zero", n, static_cast<double>(step.position[0]),
                  static_cast<double>(step.position[1]), static_cast<double>(step.position[2]));
        }
    } else {
        WTLOG("warp %d step %d/%d: age change %s requested (flags were %#x); it should load %s with Link %s", n,
              s_job.current + 1, s_job.count, step.event, flags, step.area, step.young ? "young" : "adult");
    }
    return Status::Ready;
}

void finishJob(Status status) {
    s_job = {};
    reportWarp(status, false);
}

void beginPhase(Phase phase) {
    s_job.phase = phase;
    s_job.frames = 0;
}

// Plans a chosen warp: straight there when the spot is in Link's own age's world; otherwise to his age's Temple of
// Time, the game's age change, then there.
void planJob(const WarpRequest& request, bool young) {
    const Area& area = kAreas[request.area];
    const Spot& spot = kSpots[request.spot];
    const bool wantYoung = area.age == Age::Young;
    s_job = {};
    if (young != wantYoung) {
        Step& temple = s_job.steps[s_job.count++];
        temple.area = young ? kYoungTemple : kAdultTemple;
        temple.spawn = kTempleSpawn;
        temple.label = "Temple of Time";
        temple.status = Status::ToTemple;
        Step& change = s_job.steps[s_job.count++];
        change.kind = StepKind::AgeChange;
        change.area = wantYoung ? kYoungTemple : kAdultTemple;
        change.label = "age change";
        change.event = young ? kToAdultEvent : kToYoungEvent;
        change.young = wantYoung;
        change.status = Status::ChangingAge;
    }
    Step& to = s_job.steps[s_job.count++];
    to.area = area.banc;
    to.spawn = spot.spawn;
    to.position = spot.position;
    to.label = spot.label;
    to.status = Status::Warping;
    s_job.number = ++s_warps;
    WTLOG("warp %d planned: %s / '%s' (%s world) with Link %s: %d step%s", static_cast<int>(s_job.number), area.title,
          spot.label, ageName(area.age), young ? "young" : "adult", s_job.count, s_job.count == 1 ? "" : "s");
}

// Every controller-0 update while a step's event has ended, paused or not.
void noteArrivingUpdate(bool paused, void* manager, void* eventManager) {
    if (s_job.phase != Phase::Arriving) { return; }
    const Step& step = currentStep();
    const int n = static_cast<int>(s_job.number);
    const u64 now = svcGetSystemTick();
    if (now - s_job.lastTick > s_job.longestGap) { s_job.longestGap = now - s_job.lastTick; }
    s_job.lastTick = now;
    ++s_job.updates;
    if (paused) { ++s_job.paused; }
    if (s_job.updates >= kArriveTimeoutUpdates) {
        WTLOG("warp %d step %d/%d: not settled %d updates after its event ended; given up", n, s_job.current + 1,
              s_job.count, s_job.updates);
        finishJob(Status::DidNotFinish);
        return;
    }
    if (paused || manager == nullptr) { return; }
    const char* reserved = readText(manager, kNextArea);
    const char* current = readText(manager, kCurrentArea);
    const bool settledNow = !eventPending(eventManager) && (reserved == nullptr || reserved[0] == '\0');
    if (!settledNow) {
        s_job.settled = 0;
        return;
    }
    const bool match = current != nullptr && sameText(current, step.area);
    if (++s_job.settled < (match ? kSettleFrames : kMismatchSettleFrames)) { return; }
    WTLOG("warp %d step %d/%d: arrived: %d updates after the event ended (%d paused), %llu ticks in all, longest gap "
          "between updates %llu ticks; area now %s%s",
          n, s_job.current + 1, s_job.count, s_job.updates, s_job.paused,
          static_cast<unsigned long long>(now - s_job.endTick), static_cast<unsigned long long>(s_job.longestGap),
          current != nullptr ? current : "(no area flag)", match ? "" : " (not the step's area; accepted after a wait)");
    if (step.kind == StepKind::AgeChange) {
        bool young = !step.young;
        const bool found = readIsYoung(manager, &young);
        if (!found || young != step.young) {
            WTLOG("warp %d: age change did not happen (age flag %s); the rest is cancelled", n,
                  found ? (young ? "young" : "adult") : "not found");
            finishJob(Status::AgeDidNotChange);
            return;
        }
        WTLOG("warp %d: Link is now %s", n, young ? "young" : "adult");
    }
    if (++s_job.current >= s_job.count) {
        WTLOG("warp %d: finished; menu ready", n);
        finishJob(Status::Ready);
        return;
    }
    beginPhase(Phase::Starting);
    reportWarp(currentStep().status, false);
}

// One unpaused update of the job's current step (other than arriving, handled above).
void runJob(void* manager, void* eventManager) {
    const int n = static_cast<int>(s_job.number);
    switch (s_job.phase) {
    case Phase::Idle:
    case Phase::Arriving: return;
    case Phase::Starting: {
        // A later step of an age change gets here (a warp chosen in the menu starts its first step as it is
        // chosen); the game may still be finishing the previous one, so a refusal is retried a while.
        const Status status = startStep(manager, eventManager);
        if (status == Status::Ready) {
            beginPhase(Phase::WaitStart);
            return;
        }
        if (s_job.frames == 0) {
            WTLOG("warp %d step %d/%d not started yet: %s; retrying", n, s_job.current + 1, s_job.count,
                  statusText(status));
        }
        if (++s_job.frames >= kRetryFrames) {
            WTLOG("warp %d step %d/%d not started after %d frames: %s; the rest is cancelled", n, s_job.current + 1,
                  s_job.count, kRetryFrames, statusText(status));
            finishJob(status);
        }
        return;
    }
    case Phase::WaitStart:
        ++s_job.frames;
        if (eventPending(eventManager)) {
            WTLOG("warp %d step %d/%d: %s started after %d frames", n, s_job.current + 1, s_job.count,
                  currentStep().event, s_job.frames);
            beginPhase(Phase::WaitEnd);
        } else if (s_job.frames >= kStartTimeoutFrames) {
            WTLOG("warp %d step %d/%d: %s did not start within %d frames%s", n, s_job.current + 1, s_job.count,
                  currentStep().event, kStartTimeoutFrames,
                  currentStep().kind == StepKind::Warp ? "; reserved area cleared" : "");
            if (currentStep().kind == StepKind::Warp && manager != nullptr) { clearReservation(manager); }
            finishJob(Status::DidNotStart);
        }
        return;
    case Phase::WaitEnd:
        ++s_job.frames;
        if (!eventPending(eventManager)) {
            const char* reserved = manager != nullptr ? readText(manager, kNextArea) : nullptr;
            const char* current = manager != nullptr ? readText(manager, kCurrentArea) : nullptr;
            WTLOG("warp %d step %d/%d: event ended after %d frames; reserved next area '%s', current area '%s'", n,
                  s_job.current + 1, s_job.count, s_job.frames, reserved != nullptr ? reserved : "(none)",
                  current != nullptr ? current : "(none)");
            beginPhase(Phase::Arriving);
            s_job.updates = s_job.paused = s_job.settled = 0;
            s_job.endTick = s_job.lastTick = svcGetSystemTick();
            s_job.longestGap = 0;
        }
        return;
    }
}

void noteLinkAge(void* manager) {
    KnownAge age = KnownAge::Unknown;
    bool young = false;
    if (manager != nullptr && readIsYoung(manager, &young)) {
        age = young ? KnownAge::Young : KnownAge::Adult;
    }
    if (age == s_linkAge) { return; }
    s_linkAge = age;
    reportLinkAge(age);
}

bool validRequest(const WarpRequest& request) {
    if (request.area < 0 || request.area >= kAreaCount) { return false; }
    const Area& area = kAreas[request.area];
    return request.spot >= area.first && request.spot < area.first + area.count;
}

// Starts a warp chosen in the menu. Its first step is asked for at once; a refusal is shown, not retried.
Status beginWarp(const WarpRequest& request, void* manager, void* eventManager) {
    if (manager == nullptr || eventManager == nullptr) { return Status::StillLoading; }
    if (s_job.phase != Phase::Idle) { return Status::AlreadyWarping; }
    if (eventPending(eventManager)) { return Status::SceneRunning; }
    bool young = false;
    if (!readIsYoung(manager, &young)) { return Status::AgeUnknown; }
    planJob(request, young);
    beginPhase(Phase::Starting);
    const Status status = startStep(manager, eventManager);
    if (status != Status::Ready) {
        s_job = {};
        return status;
    }
    beginPhase(Phase::WaitStart);
    return currentStep().status;
}

void logWarpNotStarted(const WarpRequest& request, const char* why) {
    WTLOG("warp to %s / '%s' not started: %s", kAreas[request.area].title, kSpots[request.spot].label, why);
}

// A warp chosen in the menu.
void serviceRequest(void* manager, void* eventManager) {
    const WarpRequest request = takeWarpRequest();
    if (!validRequest(request)) { return; }
    const Status status = beginWarp(request, manager, eventManager);
    if (status == Status::AlreadyWarping) {  // the menu refuses this too; keep the running warp's status
        logWarpNotStarted(request, "another warp is running");
        return;
    }
    if (!inProgress(status)) { logWarpNotStarted(request, statusText(status)); }
    reportWarp(status, inProgress(status));
}

}  // namespace

const char* autosaveHeldBecause() {
    switch (static_cast<SaveHold>(s_saveHold.load(std::memory_order_relaxed))) {
    case SaveHold::None: return nullptr;
    case SaveHold::AgeChange: return "mid-way through a warp with an age change";
    case SaveHold::Switch: return "autosave switched off in the menu";
    }
    return nullptr;
}

void serviceWarps() {
    s_pausedFrames = 0;
    void* manager = instanceAt(kGameDataManagerSlot);
    void* eventManager = instanceAt(kEventManagerSlot);
    noteLinkAge(manager);
    noteArrivingUpdate(false, manager, eventManager);
    runJob(manager, eventManager);
    serviceRequest(manager, eventManager);
    updateSaveHold();
}

void serviceWarpsWhilePaused() {
    noteArrivingUpdate(true, nullptr, nullptr);
    updateSaveHold();
    if (!warpRequested()) {
        s_pausedFrames = 0;
        return;
    }
    if (++s_pausedFrames < kPausedTimeoutFrames) { return; }
    s_pausedFrames = 0;
    const WarpRequest request = takeWarpRequest();
    if (validRequest(request)) { logWarpNotStarted(request, "the game stayed paused"); }
    reportWarp(Status::Paused, false);
}

}  // namespace warp

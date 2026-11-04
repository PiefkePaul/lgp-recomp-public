// battle-throw: a throw of a Poké Ball in a wild battle shows the Pokémon
// drawn into the ball (Fang-4b, docs/analysis/55-fangen-im-kampf.md). The
// game's client still handles the throw command, but its display functions
// are empty in LGP: "throw start" only marks, "throw wait" is done at once.
// The mod plays the battle's own recall (the sequence a switch uses, camera
// on the target) at throw start, holds throw wait for wait_frames calls and,
// when the ball did not hold, sends the Pokémon out again (the send-out
// start and update, a ball at its place opens) before the battle goes on.
//
// File battle-throw.cfg (roles from our analysis, addresses private):
//   hook throw_start VA      x0 = battle view, x1 = position; runs first
//   hook throw_wait VA       x0 = battle view; 1 = done
//   hook throw_handler VA    optional: the client handler of the throw
//                            command, the success flag is read at its entry
//                            (else at throw_start's)
//   value recall_start VA    recall(view, position, 0)
//   value sendout_start VA   sendout_start(view, w1 = position, w2 = sendout_side,
//                            w3 = sendout_team (default 0)): sets the slot up and
//                            its state to 0, so that the update loads the model
//                            again; a switch calls it after the recall wait.
//                            Without it the update finds the state the recall
//                            wait left, loads nothing and the next sequence
//                            command meets the unloaded model (Fang-15)
//   value sendout_side N     w2 of sendout_start: a side other than the
//                            player's (else it lands in the own team display)
//   value sendout_update VA  sendout(view): 0 while it runs, 1 when done
//   expr throw_success SIZE EXPR  the success flag, SIZE (1, 2, 4) bytes at
//                            EXPR (as in LGP_HOOK_DUMP, on the entry registers)
//   value wait_frames N      calls of throw_wait held (default 120)
//   value recall_wait VA     recall_wait(view), 1 when done; called
//                            on every throw_wait after the hold until it reports
//                            1, as long as it takes, as the game does (a warning
//                            after 600 calls; going on early would load the
//                            model before it is unloaded). The
//                            game calls it after every recall: it waits for the
//                            sequence and unloads the slot's model. Without it
//                            the send-out loads the model a second time, and
//                            the end of the battle waits forever for the
//                            resources to be free (Fang-14, H1)
//
// The ball on the ground (optional, all of these or none; Fang-4e). After the
// recall the mod builds the game's ball model itself, puts it where the
// Pokémon was and plays its catch animations: fall, one shake per shake of
// the throw (at most 3), then success or failure. The animations are in the
// ball's package, which the battle loads, but their file is never loaded in
// a battle. The end of the recall sequence clears the manager's list, model
// included (Fang-4g/4h), so the mod preloads model and animation file after
// the recall and drives the load with res_poll, which the game calls only
// while a sequence loads.
//   value ball_alloc_slot N   the heap allocates virtually: x0 = heap, w1 = size,
//                            w2 = alignment -> x0 through [[heap] + N]
//   expr heap_stack 0 EXPR    optional: the thread's heap stack (16 entries,
//                            `value heap_stack_count` the offset of its u32
//                            counter, default 0x80), e.g. from tp; the heap is
//                            pushed around the allocation and constructor, as
//                            the game does
//   value ball_create VA      constructor (object, view, heap); the mod then
//                            writes the header: +8 heap, +0x10 object,
//                            +0x18 size, +0x20 alignment
//                            and calls AddRef: vtable at object +
//                            ball_ref_offset (0x48), entry ball_addref_slot
//                            (0x10), x0 = object + 0x48, once more before
//                            list_add, which takes one reference over; the
//                            removal is deinit, list_remove, then Release
//                            (entry ball_release_slot, 0x18), which also gives
//                            the ball's resources back (Fang-15); the log line
//                            `ball removed` names each step, with `value
//                            ball_refcount_offset N` also the u32 at object + N
//                            before and after each (diagnosis, Fang-16)
//   value ball_init VA        (object, heap, model resource, w3 = ball_kind)
//   value ball_anim_attach VA (object, heap, animation resource)
//   value ball_anim_play VA   (object, name object, w2 = first frame)
//   value ball_list_add VA    (list, heap, &object, w3 = 0x40)
//   value ball_list_remove VA (list, &object)
//   value ball_deinit VA      (object)
//   value res_preload VA      (manager, package path, file path, heap, w4 = kind
//                            of the job: res_preload_kind_model (0) for the
//                            model, res_preload_kind_anim (1) for an animation
//                            cfg; Fang-4i: a wrong kind reads the file wrongly),
//                            x5 = res_preload_x5 (0; the game passes 4)
//   value res_model VA        (manager, path) -> resource or 0
//   value res_anim VA         (manager, path) -> resource or 0
//   value res_poll VA         (manager) -> w0 1 when all is loaded; called
//                            before every res_anim and before res_clear. The
//                            ball is built only after it reports 1 (Fang-18)
//   value res_clear VA        (manager): drops the manager's load jobs. The game
//                            calls it at the end of every sequence; ours come
//                            after the recall's, so without it the jobs stay and
//                            the end of the battle waits forever (white,
//                            Fang-16). Called once the ball is removed (or left
//                            out after its preload), after res_poll reports 1
//   value seq_busy_offset N   optional: res_clear only while the u32 at view + N
//                            is 0 (no battle sequence runs)
//   value ball_free_slot N    the buffer goes back through [[heap] + N] (heap,
//                            buffer, size, alignment) after res_clear; without it
//                            the end of the battle stays white as well (Fang-16,
//                            PC K17/K18)
//   value model_preload 0     diagnosis: the model is not preloaded (only the
//                            animation file)
//   value path_ball_model VA  (view, w1 = ball_slot) -> path object at x8
//   value path_from_text VA   (text) -> path object at x8
//   value path_package VA     optional: the same for the package path (else
//                            path_from_text)
//   expr res_manager 0 EXPR   these three on throw_start's entry registers
//   expr view_heap 0 EXPR     (x0 = view), e.g. x0>0xb0 for a pointer there
//   expr model_list 0 EXPR
//   expr ball_pos 0 EXPR      three floats: where the ball lies (not needed with
//                            slot_position)
//   value slot_position VA    optional, instead of ball_pos: slot_position(view,
//                            w1 = ball_target_slot (default 1, the opponent),
//                            &xyz, &rotation) as the send-out places its ball
//                            (Fang-17); ball_raise_milli N lifts the ball by
//                            N/1000 on y (its radius: else its centre lies on
//                            the ground, half sunk). The position is written
//                            again on every throw_wait while the ball plays
//                            (`value ball_place_every_frame 0`: only once;
//                            diagnosis, Fang-18: the placement may reset the
//                            motion)
//   expr ball_pos_add 0 EXPR  optional: three floats added to it (a model's
//                            world position is +0xb0 plus +0xf0, Fang-4k)
//   expr throw_shakes SIZE EXPR  read where throw_success is read
//   value recall_seq_text VA  optional (Fang-22): the path text of the recall's
//                            sequence in the game's rodata. For the time of
//                            its own recall the mod writes there the path of
//                            the throw's sequence (the game reads it at run
//                            time: the path is copied and the file opened
//                            within the call, Fang-22 C) and puts the text
//                            back right after. Normal switches keep the
//                            original.
//   path throw_seq_success 0 TEXT  the sequence for a caught Pokémon (fall,
//                            shakes, success: the game builds and plays the
//                            ball itself, data mod files)
//   path throw_seq_fail0..3 0 TEXT  for a ball that breaks free after 0-3
//                            shakes (throw_shakes); a missing one takes the
//                            nearest fewer, else more shakes. No text for an
//                            outcome: the plain recall. Texts at most as long
//                            as the original; excludes value ball_create.
//                            wait_frames must cover the longest sequence
//   value throw_partner_slot N  optional (Fang-40): the slot of the view's
//                            vtable that tells the partner (Pikachu or Eevee
//                            of the save) apart: the mod calls it on
//                            throw_start's x0 with x1 = 0, bit 0 of the
//                            result set means the partner. Unreadable: not
//                            the partner
//   path throw_seq_partner_success|fail0..3 0 TEXT  the throw's sequences for
//                            the partner (e.g. onto the trainer's shoulder);
//                            the same fallback as throw_seq_*, a partner
//                            without any of them plays throw_seq_*. Need
//                            value throw_partner_slot and recall_seq_text
//   path keep_sendout_seq_partner 0 TEXT  keep_sendout_seq for the partner
//   expr keep_busy SIZE EXPR  a miss without the recall wait (Fang-29): the
//                            recall wait unloads the Pokémon's model and the
//                            send-out loads it again (about 45 frames of empty
//                            ground). With this line the mod waits instead
//                            until this value (the sequence controller's
//                            state, on throw_wait's entry registers) is 0,
//                            runs `acts keep_sendout` once and waits for 0
//                            again; recall_wait and the send-out calls are
//                            left out, so the model is loaded once
//   acts keep_sendout 0 ACTS  what runs then: LGP_HOOK_ACT items without the
//                            door (set:EXPR=V[/SIZE], call:FN(xN=V;…)), comma
//                            separated, on throw_wait's entry registers: the
//                            slot fields and the send-out sequence (ee630)
//   path keep_sendout_seq 0 TEXT  with expr keep_busy, instead of or after the
//                            acts: the mod plays this sequence (the send-out,
//                            ee630) the way it plays throw_seq_*: the text at
//                            recall_seq_text for the time of a recall_start
//                            call with throw_start's x0/x1. Needs value
//                            recall_seq_text; whether recall_start leaves the
//                            model loaded is checked on the game only
//   expr trainer_battle SIZE EXPR  optional: on throw_start's entry registers,
//                            the battle's kind (0 wild, Fang-32); not 0 or
//                            unreadable: the mod leaves the throw to the game
//                            (in a trainer battle the game itself bounces the
//                            ball off, command 0x60, "Don't be a thief!")
//   acts throw_prepare 0 ACTS  optional: LGP_HOOK_ACT items as for keep_sendout,
//                            run on throw_start's entry registers before the
//                            throw's sequence starts
//   acts throw_started 0 ACTS  optional, the same right after the throw's
//                            recall_start call and on every held throw_wait
//                            call: recall_start writes the Pokémon's own ball
//                            into the ball slot's kind, which 0x4e reads for
//                            the model (Fang-32b: set:x0+OFF=x2/4 with the
//                            thrown item; x2 is throw_start's entry x2 there)
//   value caught_release N   with the sequences: a caught ball frees
//                            throw_wait after N calls, without the recall
//                            wait, so the game shows "caught" while the
//                            sequence (hold range, ball) still runs and the
//                            camera stays on the ball (Fang-27 A7). The
//                            sequence must end before the next one (EXP,
//                            battle end); N > 0, only with throw_seq_success
//   hook ball_hold_init VA   optional (Fang-42), with the throw's sequences:
//                            the ball that a 0x4e without a lifetime task
//                            (range 0) builds stays until the battle's fade
//                            out, and the mod removes it itself. This door is
//                            the ball's init (x0 = ball); from our throw on
//                            the mod notes x0 of every call there (at most
//                            8), with `value ball_hold_lr VA` only calls
//                            returning to VA (the 0x4e handler)
//   hook ball_hold_deinit VA the game's ball_deinit (x0 = ball): a noted ball
//                            the game removes itself (a 0x4e with its task)
//                            is forgotten
//   hook ball_hold_release VA  runs once as the battle's end unloads, before
//                            it waits for its models: before its original the
//                            mod removes the noted balls as the game does,
//                            value ball_deinit (ball), then value
//                            ball_list_remove (list, &ball; the pointer on
//                            the stack). Without it the end waits for ever
//                            (white)
//   expr ball_hold_list 0 EXPR  the model list, on throw_start's entry
//                            registers (x0 = view)
//   hook catch_store VA      optional (Fang-48), the store of a caught Pokémon
//                            (x1 = a pointer to its data): the catch process
//                            that wild-battles leads a battle catch into
//                            keeps its default Poké Ball. On the first call
//                            returning to `value catch_store_lr VA` after a
//                            caught throw, the mod calls `value ball_setter
//                            VA` (x0 = [x1], x1 = the ball, throw_start's x2)
//                            before the original. A break-out, a new throw
//                            and the store itself clear it, so a later Go
//                            catch keeps the game's own ball
//   path capture_package 0 TEXT   the package and the animation file
//   path capture_anim 0 TEXT
//   value ball_slot, ball_kind (0), ball_pos_offset (0xb0), ball_dirty_offset
//   (0xc0, a byte set to 1 after the position), name_word2 (0x3f, the third
//   word of a name object), load_frames (10), fall_frames, shake_frames,
//   end_frames (30 each)
// The path objects are not
// released (their text objects are counted, Fang-4i: one count each stays):
// they live in the battle's heap until the battle ends (guess: the heap goes
// with the battle).
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "lgp/lifted/cpu_state.hpp"
#include "lgp/runtime/guest_mem.hpp"
#include "lgp/runtime/log.hpp"
#include "mod_impl.hpp"

namespace lgp::mods::detail {

namespace {

std::atomic<bool> g_on{false};

struct Config {
    uint64_t recall_start = 0;
    uint64_t sendout_update = 0;
    uint64_t sendout_start = 0, sendout_side = 0, sendout_team = 0;
    HookDump success;  // the address of the flag
    unsigned success_size = 1;
    bool success_at_handler = false;
    uint32_t wait_frames = 120;
    uint32_t caught_release = 0;  // 0: a caught ball waits for the recall like a miss
    bool keep = false;            // expr keep_busy: a miss keeps the model (Fang-29)
    HookDump keep_busy;
    unsigned keep_busy_size = 1;
    std::vector<HookAct> keep_sendout;
    std::string keep_seq;         // path keep_sendout_seq: played through recall_start
    bool has_trainer = false;     // expr trainer_battle
    HookDump trainer;
    unsigned trainer_size = 4;
    std::vector<HookAct> throw_prepare;
    std::vector<HookAct> throw_started;
    uint64_t recall_wait = 0;
    // The ball on the ground.
    bool ball = false;
    uint64_t alloc_slot = 0, create = 0, init = 0, attach = 0, play = 0, list_add = 0, list_remove = 0, deinit = 0;
    uint64_t preload = 0, res_poll = 0, res_model = 0, res_anim = 0, path_model = 0, path_text = 0, path_package = 0;
    HookDump manager, heap, list, pos, pos_add, shakes, heap_stack;
    bool has_pos_add = false;
    bool has_heap_stack = false;
    bool no_model_preload = false;  // value model_preload 0 (diagnosis, Fang-4h)
    uint64_t kind_model = 0, kind_anim = 1;  // res_preload's w4
    uint64_t preload_x5 = 0;                 // and x5 (the game passes 4, meaning unknown)
    uint64_t ref_offset = 0x48, addref_slot = 0x10, release_slot = 0x18;
    std::optional<uint64_t> refcount_offset;  // diagnosis: logged around the removal
    uint64_t res_clear = 0;
    uint64_t slot_position = 0, target_slot = 1;
    float raise = 0.0f;
    bool place_every_frame = true;
    std::optional<uint64_t> seq_busy_offset, free_slot;
    uint64_t heap_stack_count = 0x80;
    unsigned shakes_size = 1;
    bool has_shakes = false;
    std::string package, anim;
    uint64_t slot = 0, kind = 0, pos_offset = 0xb0, dirty_offset = 0xc0, name_word2 = 0x3f;
    uint32_t load_frames = 10, fall_frames = 30, shake_frames = 30, end_frames = 30;
    // The throw's own sequences (Fang-22): 0 success, 1 + k break free after k shakes.
    uint64_t seq_text = 0;
    std::string seq[5];
    // The partner's own sequences (Fang-40), chosen through throw_partner_slot.
    uint64_t partner_slot = 0;
    std::string partner_seq[5];
    std::string partner_keep_seq;
    // The held ball (Fang-42).
    bool hold = false;
    uint64_t hold_lr = 0, hold_deinit = 0, hold_list_remove = 0;
    HookDump hold_list;
    // The thrown ball stored with the catch (Fang-48).
    bool store_ball = false;
    uint64_t store_lr = 0, ball_setter = 0;
} g_config;

constexpr uint32_t kRecallWaitWarn = 600;  // calls of the recall wait before a warning

enum Phase : uint32_t { kIdle, kHold, kRecallWait, kBallLoad, kBallAnim, kBallClear, kSendOut, kKeepWait, kKeepPlay };
std::atomic<uint32_t> g_phase{kIdle};
std::atomic<uint32_t> g_left{0};
std::atomic<bool> g_success{true};
std::atomic<uint32_t> g_shakes{3};
std::atomic<uint64_t> g_position{0};  // throw_start's x1, for the send-out start
std::atomic<uint64_t> g_start_x0{0};  // throw_start's x0, for keep_sendout_seq
lgp::CpuState g_start_entry{};         // throw_start's entry registers, for throw_started
std::atomic<bool> g_sendout_started{false};
std::atomic<bool> g_partner{false};  // this throw's Pokémon is the partner (throw_partner_slot)
// The recall's original path text while the throw's is written there (only
// the battle thread touches them).
std::string g_seq_original;
bool g_seq_written = false;

// The balls held past their sequence (Fang-42): noted from our throw on
// until the battle's end removes them.
constexpr size_t kHoldMax = 8;
std::atomic<uint64_t> g_held[kHoldMax];
std::atomic<bool> g_hold_armed{false};
std::atomic<uint64_t> g_hold_list{0};

// The ball of a caught throw until the catch is stored (Fang-48), 0: none.
std::atomic<uint64_t> g_caught_ball{0};

// One throw's ball (only the battle thread touches it).
constexpr uint64_t kBufSize = 0x400;
constexpr uint64_t kObjectSize = 0x2c0;
constexpr uint64_t kPackageText = 0x000, kAnimText = 0x0c0, kPackagePath = 0x180, kAnimPath = 0x1b0,
                   kModelPath = 0x1e0, kName = 0x210, kNameText = 0x230, kObjectRef = 0x330, kSlotPos = 0x340,
                   kSlotRot = 0x350, kPlayResult = 0x380;
struct Ball {
    uint64_t view = 0, manager = 0, heap = 0, list = 0, buf = 0, object = 0;
    float pos[3] = {};
    uint32_t tries = 0;
    unsigned step = 0, steps = 0;  // fall, shakes, end
    bool preloaded = false;        // load jobs queued: res_clear before the end
} g_ball;
bool read_success(const lgp::CpuState& entry) {
    uint64_t addr = 0;
    if (!eval_hook_expr(g_config.success, entry, 0, &addr) ||
        !rt::access_allowed(addr, g_config.success_size, rt::kPageRead))
        return true;  // unreadable: as before, nothing comes back
    switch (g_config.success_size) {
    case 1: return rt::load<uint8_t>(addr) != 0;
    case 2: return rt::load<uint16_t>(addr) != 0;
    default: return rt::load<uint32_t>(addr) != 0;
    }
}

// The value of a configured expr (size 1, 2 or 4) on these registers; false if unreadable.
bool read_expr(const HookDump& expr, unsigned size, const lgp::CpuState& entry, uint32_t* out) {
    uint64_t addr = 0;
    if (!eval_hook_expr(expr, entry, 0, &addr) || !rt::access_allowed(addr, size, rt::kPageRead))
        return false;
    *out = size == 1 ? rt::load<uint8_t>(addr) : size == 2 ? rt::load<uint16_t>(addr) : rt::load<uint32_t>(addr);
    return true;
}

// Whether the keep path's state can be read on these registers.
bool keep_readable(const lgp::CpuState& entry) {
    uint64_t addr = 0;
    return eval_hook_expr(g_config.keep_busy, entry, 0, &addr) &&
           rt::access_allowed(addr, g_config.keep_busy_size, rt::kPageRead);
}

// The sequence controller's state for the keep path; unreadable counts as busy.
bool keep_busy(const lgp::CpuState& entry) {
    uint64_t addr = 0;
    if (!eval_hook_expr(g_config.keep_busy, entry, 0, &addr) ||
        !rt::access_allowed(addr, g_config.keep_busy_size, rt::kPageRead))
        return true;
    switch (g_config.keep_busy_size) {
    case 1: return rt::load<uint8_t>(addr) != 0;
    case 2: return rt::load<uint16_t>(addr) != 0;
    default: return rt::load<uint32_t>(addr) != 0;
    }
}

uint32_t read_shakes(const lgp::CpuState& entry) {
    uint64_t addr = 0;
    if (!g_config.has_shakes || !eval_hook_expr(g_config.shakes, entry, 0, &addr) ||
        !rt::access_allowed(addr, g_config.shakes_size, rt::kPageRead))
        return 3;
    const uint32_t n = g_config.shakes_size == 1   ? rt::load<uint8_t>(addr)
                       : g_config.shakes_size == 2 ? rt::load<uint16_t>(addr)
                                                   : rt::load<uint32_t>(addr);
    return n > 3 ? 3 : n;
}

// x0 of fn(args…, x8 = out) on ctx's stack; every register put back. False
// if the guest halted (the halt stays).
bool call(lgp::CpuState* ctx, uint64_t fn, std::initializer_list<uint64_t> args, uint64_t* ret, uint64_t x8 = 0) {
    const lgp::CpuState saved = *ctx;
    unsigned i = 0;
    for (uint64_t a : args)
        ctx->x[i++] = a;
    ctx->x[8] = x8;
    const bool done = call_guest(ctx, fn);
    const uint64_t r = ctx->x[0];
    const uint32_t halt = ctx->halt;
    const uint64_t halt_info = ctx->halt_info;
    *ctx = saved;
    if (!done) {
        ctx->halt = halt;
        ctx->halt_info = halt_info;
        return false;
    }
    *ret = r;
    return true;
}

bool call3(lgp::CpuState* ctx, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t* ret) {
    return call(ctx, fn, {x0, x1, x2}, ret);
}

// size bytes from the heap through its allocation slot; 0 when it has none.
bool heap_alloc(lgp::CpuState* ctx, uint64_t heap, uint64_t size, uint64_t* out) {
    *out = 0;
    const uint64_t vtable = rt::load<uint64_t>(heap);
    if (vtable == 0 || !rt::access_allowed(vtable + g_config.alloc_slot, 8, rt::kPageRead))
        return true;
    return call(ctx, rt::load<uint64_t>(vtable + g_config.alloc_slot), {heap, size, 16}, out);
}

// The game's heap stack around an allocation: push the heap (false when the
// stack is full or unreadable, the ball is then left out), pop it again.
bool heap_push(const lgp::CpuState& entry, uint64_t heap, uint64_t* stack) {
    *stack = 0;
    if (!g_config.has_heap_stack)
        return true;
    uint64_t at = 0;
    if (!eval_hook_expr(g_config.heap_stack, entry, 0, &at) ||
        !rt::access_allowed(at + g_config.heap_stack_count, 4, rt::kPageWrite))
        return false;
    const uint32_t n = rt::load<uint32_t>(at + g_config.heap_stack_count);
    if (n >= 16 || !rt::access_allowed(at + 8 * n, 8, rt::kPageWrite))
        return false;
    rt::store<uint64_t>(at + 8 * n, heap);
    rt::store<uint32_t>(at + g_config.heap_stack_count, n + 1);
    *stack = at;
    return true;
}

void heap_pop(uint64_t stack) {
    if (stack == 0)
        return;
    const uint32_t n = rt::load<uint32_t>(stack + g_config.heap_stack_count);
    if (n > 0)
        rt::store<uint32_t>(stack + g_config.heap_stack_count, n - 1);
}

// A call through the object's reference counter: vtable at object +
// ref_offset, entry `slot`, x0 = object + ref_offset.
bool ref_call(lgp::CpuState* ctx, uint64_t object, uint64_t slot) {
    const uint64_t counter = object + g_config.ref_offset;
    if (!rt::access_allowed(counter, 8, rt::kPageRead)) {
        rt::log(rt::LogLevel::Warn, "battle-throw: ball reference counter unreadable, ball left out");
        return false;
    }
    const uint64_t vtable = rt::load<uint64_t>(counter);
    if (vtable == 0 || !rt::access_allowed(vtable + slot, 8, rt::kPageRead)) {
        rt::log(rt::LogLevel::Warn, "battle-throw: ball reference counter without vtable, ball left out");
        return false;
    }
    uint64_t r = 0;
    return call(ctx, rt::load<uint64_t>(vtable + slot), {counter}, &r);
}

bool eval_ptr(const HookDump& e, const lgp::CpuState& entry, uint64_t* out) {
    return eval_hook_expr(e, entry, 0, out) && *out != 0;
}

void store_text(uint64_t at, const std::string& text) {
    for (size_t i = 0; i < text.size(); ++i)
        rt::store<uint8_t>(at + i, static_cast<uint8_t>(text[i]));
    rt::store<uint8_t>(at + text.size(), 0);
}

// Plays animation `name` on the ball from frame 0: the name object (FNV-1a-64
// of the name, its text, name_word2, 0) and the text in the throw's buffer.
bool play_anim(lgp::CpuState* ctx, const char* name) {
    const Ball& b = g_ball;
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const char* p = name; *p != '\0'; ++p)
        hash = (hash ^ static_cast<uint8_t>(*p)) * 0x100000001b3ull;
    const uint64_t text = b.buf + kNameText;
    store_text(text, name);
    rt::store<uint64_t>(b.buf + kName, hash);
    rt::store<uint64_t>(b.buf + kName + 8, text);
    rt::store<uint64_t>(b.buf + kName + 0x10, g_config.name_word2);
    rt::store<uint64_t>(b.buf + kName + 0x18, 0);
    // x8: the result of the transition search lands there (Fang-19: before,
    // x8 was 0); logged with the model's scene component (+0x78), which the
    // list's update needs (diagnosis).
    for (uint64_t i = 0; i < 0x40; i += 8)
        rt::store<uint64_t>(b.buf + kPlayResult + i, 0);
    uint64_t r = 0;
    if (!call(ctx, g_config.play, {b.object, b.buf + kName, 0}, &r, b.buf + kPlayResult))
        return false;
    rt::log(rt::LogLevel::Info, "battle-throw: play %s: result %016" PRIx64 " %016" PRIx64 ", scene %s", name,
            rt::load<uint64_t>(b.buf + kPlayResult), rt::load<uint64_t>(b.buf + kPlayResult + 8),
            rt::load<uint64_t>(b.object + 0x78) != 0 ? "set" : "0");
    return true;
}

// At throw start: where the ball goes and with what; buffer, path objects and
// preload follow after the recall (ball_build). False with the ball left out
// (the recall still plays).
bool ball_prepare(lgp::CpuState* ctx) {
    const Config& c = g_config;
    const lgp::CpuState entry = *ctx;
    Ball b;
    b.view = entry.x[0];
    if (!eval_ptr(c.manager, entry, &b.manager) || !eval_ptr(c.heap, entry, &b.heap) ||
        !eval_ptr(c.list, entry, &b.list)) {
        rt::log(rt::LogLevel::Warn, "battle-throw: no manager, heap or list at throw start, ball left out");
        return false;
    }
    if (c.slot_position != 0) {  // asked after the recall, in ball_build
        g_ball = b;
        return true;
    }
    uint64_t pos = 0;
    if (!eval_hook_expr(c.pos, entry, 0, &pos) || !rt::access_allowed(pos, 12, rt::kPageRead)) {
        rt::log(rt::LogLevel::Warn, "battle-throw: ball position unreadable, ball left out");
        return false;
    }
    for (int i = 0; i < 3; ++i)
        b.pos[i] = rt::load<float>(pos + 4 * i);
    if (c.has_pos_add) {  // a model's world position: +0xb0 plus its offset field (Fang-4k)
        uint64_t add = 0;
        if (!eval_hook_expr(c.pos_add, entry, 0, &add) || !rt::access_allowed(add, 12, rt::kPageRead)) {
            rt::log(rt::LogLevel::Warn, "battle-throw: ball position offset unreadable, ball left out");
            return false;
        }
        for (int i = 0; i < 3; ++i)
            b.pos[i] += rt::load<float>(add + 4 * i);
    }
    g_ball = b;
    return true;
}

// The ball's position and the byte that has the view take it.
void place_ball() {
    const Ball& b = g_ball;
    for (int i = 0; i < 3; ++i)
        rt::store<float>(b.object + g_config.pos_offset + 4 * i, b.pos[i]);
    rt::store<uint8_t>(b.object + g_config.dirty_offset, 1);
}

// The model, once it and its animation file are loaded: false while they
// load (try again on the next call), *gone when the ball is left out. Both are
// preloaded here, after the recall, whose end clears the manager's list.
bool ball_build(lgp::CpuState* ctx, bool* gone) {
    const Config& c = g_config;
    Ball& b = g_ball;
    *gone = false;
    uint64_t r = 0;
    if (b.tries == 0) {
        // Everything the load jobs point at is made only now, after the
        // recall (suspicion, Fang-4h: memory taken during the recall goes with
        // its end), in the view's heap on top of the heap stack like the
        // allocation of the ball.
        if (!heap_alloc(ctx, b.heap, kBufSize, &b.buf) || b.buf == 0) {
            if (ctx->halt == kHaltNone)
                rt::log(rt::LogLevel::Warn, "battle-throw: no buffer from the heap, ball left out");
            b.buf = 0;
            return *gone = true, false;
        }
        store_text(b.buf + kPackageText, c.package);
        store_text(b.buf + kAnimText, c.anim);
        uint64_t stack = 0;
        if (!heap_push(*ctx, b.heap, &stack)) {
            rt::log(rt::LogLevel::Warn, "battle-throw: heap stack full or unreadable, ball left out");
            return *gone = true, false;
        }
        const bool queued = call(ctx, c.path_package != 0 ? c.path_package : c.path_text, {b.buf + kPackageText},
                                 &r, b.buf + kPackagePath) &&
                            call(ctx, c.path_text, {b.buf + kAnimText}, &r, b.buf + kAnimPath) &&
                            call(ctx, c.path_model, {b.view, c.slot}, &r, b.buf + kModelPath) &&
                            (c.no_model_preload ||
                             call(ctx, c.preload,
                                  {b.manager, b.buf + kPackagePath, b.buf + kModelPath, b.heap, c.kind_model, c.preload_x5}, &r)) &&
                            call(ctx, c.preload,
                                 {b.manager, b.buf + kPackagePath, b.buf + kAnimPath, b.heap, c.kind_anim, c.preload_x5}, &r);
        heap_pop(stack);
        b.preloaded = true;  // even if only the first job went in
        if (!queued)
            return *gone = true, false;
    }
    // With res_poll the ball waits until it reports 1 for all jobs, as the
    // game does: the animation's state machine (the to_… transitions) is
    // built only in the job's second stage, and a resource there before that
    // takes the names silently (Fang-18).
    uint64_t anim = 0, model = 0, polled = 1;
    if ((c.res_poll != 0 && !call(ctx, c.res_poll, {b.manager}, &polled)) ||
        !call(ctx, c.res_anim, {b.manager, b.buf + kAnimPath}, &anim) ||
        !call(ctx, c.res_model, {b.manager, b.buf + kModelPath}, &model))
        return *gone = true, false;
    if (anim == 0 || model == 0 || (polled & 0xff) == 0) {
        if (++b.tries >= c.load_frames) {
            rt::log(rt::LogLevel::Warn, "battle-throw: %s not loaded after %u calls, ball left out",
                    anim == 0 && model == 0 ? "ball model and animation file"
                    : anim == 0             ? "animation file"
                    : model == 0            ? "ball model"
                                            : "everything (res_poll)",
                    c.load_frames);
            *gone = true;
        }
        return false;
    }
    uint64_t stack = 0;
    if (!heap_push(*ctx, b.heap, &stack)) {
        rt::log(rt::LogLevel::Warn, "battle-throw: heap stack full or unreadable, ball left out");
        return *gone = true, false;
    }
    const bool made = heap_alloc(ctx, b.heap, kObjectSize, &b.object) && b.object != 0 &&
                      call(ctx, c.create, {b.object, b.view, b.heap}, &r);
    if (made) {
        rt::store<uint64_t>(b.object + 8, b.heap);
        rt::store<uint64_t>(b.object + 0x10, b.object);
        rt::store<uint64_t>(b.object + 0x18, kObjectSize);
        rt::store<uint64_t>(b.object + 0x20, 16);
    }
    if (ctx->halt != kHaltNone)
        return *gone = true, false;
    heap_pop(stack);
    if (!made) {
        rt::log(rt::LogLevel::Warn, "battle-throw: no ball object from the heap, ball left out");
        return *gone = true, false;
    }
    // The game's AddRef after the header (Fang-4h b1r-t), and one more for
    // the list: list_add takes a reference over and list_remove frees it, so
    // with one reference the ball would be gone before deinit (Fang-15).
    if (!ref_call(ctx, b.object, c.addref_slot))
        return *gone = true, false;
    rt::store<uint64_t>(b.buf + kObjectRef, b.object);
    if (!call(ctx, c.init, {b.object, b.heap, model, c.kind}, &r) ||
        !call(ctx, c.attach, {b.object, b.heap, anim}, &r) || !ref_call(ctx, b.object, c.addref_slot) ||
        !call(ctx, c.list_add, {b.list, b.heap, b.buf + kObjectRef, 0x40}, &r))
        return *gone = true, false;
    if (c.slot_position != 0) {  // the opponent's place as the send-out takes it (Fang-17)
        if (!call(ctx, c.slot_position, {b.view, c.target_slot, b.buf + kSlotPos, b.buf + kSlotRot}, &r))
            return *gone = true, false;
        for (int i = 0; i < 3; ++i)
            b.pos[i] = rt::load<float>(b.buf + kSlotPos + 4 * i);
    }
    b.pos[1] += c.raise;
    place_ball();
    b.steps = 2 + g_shakes.load(std::memory_order_relaxed);
    b.step = 0;
    rt::log(rt::LogLevel::Info, "battle-throw: ball at %.1f %.1f %.1f, %u shakes", b.pos[0], b.pos[1], b.pos[2],
            b.steps - 2);
    return play_anim(ctx, "to_capture01_fall");
}

// The throw's buffer back to the heap (also when the ball was left out
// before its preload). False if the guest halted.
bool free_buffer(lgp::CpuState* ctx) {
    Ball& b = g_ball;
    if (b.buf == 0 || !g_config.free_slot)
        return true;
    const uint64_t vtable = rt::load<uint64_t>(b.heap);
    uint64_t r = 0;
    if (vtable != 0 && rt::access_allowed(vtable + *g_config.free_slot, 8, rt::kPageRead) &&
        !call(ctx, rt::load<uint64_t>(vtable + *g_config.free_slot), {b.heap, b.buf, kBufSize, 16}, &r))
        return false;
    b.buf = 0;
    return true;
}

// After the ball: the load jobs of our preload dropped (res_clear, once no
// sequence runs and res_poll reports 1), then the buffer back to the heap.
// False while waiting (or halted).
bool ball_clear(lgp::CpuState* ctx) {
    const Config& c = g_config;
    Ball& b = g_ball;
    uint64_t r = 0;
    if (!b.preloaded)
        return free_buffer(ctx);
    const uint32_t calls = g_left.load(std::memory_order_relaxed) + 1;
    g_left.store(calls, std::memory_order_relaxed);
    if (calls == kRecallWaitWarn)
        rt::log(rt::LogLevel::Warn, "battle-throw: load jobs not cleared after %u calls, still waiting", calls);
    if (c.seq_busy_offset) {
        const uint64_t at = ctx->x[0] + *c.seq_busy_offset;
        if (rt::access_allowed(at, 4, rt::kPageRead) && rt::load<uint32_t>(at) != 0)
            return false;
    }
    if (c.res_poll != 0) {
        if (!call(ctx, c.res_poll, {b.manager}, &r))
            return false;
        if ((r & 0xff) == 0)
            return false;
    }
    if (!call(ctx, c.res_clear, {b.manager}, &r))
        return false;
    b.preloaded = false;
    const bool had_buffer = b.buf != 0;
    if (!free_buffer(ctx))
        return false;
    rt::log(rt::LogLevel::Info, "battle-throw: load jobs cleared after %u calls%s", calls,
            had_buffer ? ", buffer freed" : "");
    return true;
}

uint32_t step_frames(unsigned step) {
    const Ball& b = g_ball;
    return step == 0 ? g_config.fall_frames : step + 1 < b.steps ? g_config.shake_frames : g_config.end_frames;
}

bool ball_step(lgp::CpuState* ctx, unsigned step) {
    static const char* const kShakes[] = {"to_capture01_yure01", "to_capture01_yure02", "to_capture01_yure03"};
    if (step + 1 < g_ball.steps)
        return play_anim(ctx, kShakes[step - 1]);
    return play_anim(ctx, g_success.load(std::memory_order_relaxed) ? "to_capture01_success" : "to_capture01_failure");
}

// As the game removes a ball: deinit, out of the list (its reference), then
// our own Release, which frees the ball and its resources (Fang-15).
void ball_remove(lgp::CpuState* ctx) {
    const uint64_t object = g_ball.object;
    std::string counts;  // the reference count before and after each step (diagnosis)
    auto count = [&] {
        if (!g_config.refcount_offset)
            return;
        const uint64_t at = object + *g_config.refcount_offset;
        counts += rt::access_allowed(at, 4, rt::kPageRead) ? std::to_string(rt::load<uint32_t>(at)) : std::string("?");
        counts += ' ';
    };
    uint64_t r = 0;
    rt::store<uint64_t>(g_ball.buf + kObjectRef, object);  // list_add emptied the slot
    count();
    const bool deinit = call(ctx, g_config.deinit, {object}, &r);
    count();
    const bool removed = deinit && call(ctx, g_config.list_remove, {g_ball.list, g_ball.buf + kObjectRef}, &r);
    count();
    const bool released = removed && ref_call(ctx, object, g_config.release_slot);
    count();
    rt::log(rt::LogLevel::Info, "battle-throw: ball removed: deinit %s, list_remove %s, release %s%s%s",
            deinit ? "done" : "failed", removed ? "done" : "skipped", released ? "done" : "skipped",
            counts.empty() ? "" : ", count ", counts.c_str());
    g_ball.object = 0;
}
// The sequence text for this outcome: a missing break-free text takes the
// nearest one with fewer shakes, then with more. Empty: the plain recall.
const std::string& seq_for(const std::string (&seq)[5], bool success, uint32_t shakes) {
    if (success)
        return seq[0];
    for (uint32_t k = shakes + 1; k-- > 0;)
        if (!seq[1 + k].empty())
            return seq[1 + k];
    for (uint32_t k = shakes + 1; k < 4; ++k)
        if (!seq[1 + k].empty())
            return seq[1 + k];
    static const std::string none;
    return none;
}

// Whether the view's Pokémon is the partner: the vtable slot on x0 = view,
// x1 = 0, bit 0 of the result (Fang-40). False when off or unreadable.
bool is_partner(lgp::CpuState* ctx, uint64_t view) {
    if (g_config.partner_slot == 0)
        return false;
    const uint64_t vtable = rt::access_allowed(view, 8, rt::kPageRead) ? rt::load<uint64_t>(view) : 0;
    if (vtable == 0 || !rt::access_allowed(vtable + g_config.partner_slot, 8, rt::kPageRead)) {
        rt::log(rt::LogLevel::Warn, "battle-throw: throw_partner_slot unreadable, not the partner");
        return false;
    }
    uint64_t r = 0;
    if (!call3(ctx, rt::load<uint64_t>(vtable + g_config.partner_slot), view, 0, 0, &r))
        return false;
    return (r & 1) != 0;
}

// Host pointer to n guest bytes at va, read-only pages too (the text is in
// rodata), or nullptr.
uint8_t* text_bytes(uint64_t va, uint64_t n) {
    if (n == 0 || !rt::access_allowed(va, static_cast<unsigned>(n), rt::kPageRead))
        return nullptr;
    return rt::g_view.base + (va - rt::g_view.guest_base);
}

// Writes the throw's sequence path over the recall's (Fang-22). False: the
// recall plays as it is.
bool seq_write(const std::string& text) {
    if (text.empty() || g_config.seq_text == 0)
        return false;
    if (g_seq_original.empty()) {  // read once: the original text, up to its NUL
        std::string t;
        for (uint64_t at = g_config.seq_text; t.size() < 0x100; ++at) {
            const uint8_t* p = text_bytes(at, 1);
            if (p == nullptr) {
                t.clear();
                break;
            }
            if (*p == 0)
                break;
            t += static_cast<char>(*p);
        }
        if (t.empty() || t.size() >= 0x100) {
            rt::log(rt::LogLevel::Warn, "battle-throw: no path text at recall_seq_text, plain recall");
            return false;
        }
        g_seq_original = t;
    }
    if (text.size() > g_seq_original.size()) {
        rt::log(rt::LogLevel::Warn, "battle-throw: %s is longer than the recall's path (%zu characters), plain recall",
                text.c_str(), g_seq_original.size());
        return false;
    }
    uint8_t* p = text_bytes(g_config.seq_text, g_seq_original.size() + 1);
    if (p == nullptr)
        return false;
    std::memset(p, 0, g_seq_original.size() + 1);
    std::memcpy(p, text.data(), text.size());
    g_seq_written = true;
    rt::log(rt::LogLevel::Info, "battle-throw: recall plays %s", text.c_str());
    return true;
}

void seq_restore() {
    if (!g_seq_written)
        return;
    if (uint8_t* p = text_bytes(g_config.seq_text, g_seq_original.size() + 1)) {
        std::memcpy(p, g_seq_original.data(), g_seq_original.size());
        p[g_seq_original.size()] = 0;
    }
    g_seq_written = false;
}

void on_handler(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    if (g_on.load(std::memory_order_relaxed) && g_phase.load(std::memory_order_relaxed) == kIdle) {
        g_success.store(read_success(*ctx), std::memory_order_relaxed);
        g_shakes.store(read_shakes(*ctx), std::memory_order_relaxed);
    }
    original(ctx);
}

// The ball's init: notes the ball while our throw is armed.
void on_hold_init(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const uint64_t ball = ctx->x[0];
    if (g_on.load(std::memory_order_relaxed) && g_hold_armed.load(std::memory_order_relaxed) && ball != 0 &&
        (g_config.hold_lr == 0 || ctx->x[30] == g_config.hold_lr)) {
        bool noted = false;
        for (std::atomic<uint64_t>& h : g_held) {
            uint64_t free = 0;
            if (h.load(std::memory_order_relaxed) == ball) {
                noted = true;
                break;
            }
            if (h.compare_exchange_strong(free, ball, std::memory_order_relaxed)) {
                noted = true;
                rt::log(rt::LogLevel::Info, "battle-throw: ball noted, removed at the battle's end if still there");
                break;
            }
        }
        if (!noted)
            rt::log(rt::LogLevel::Warn, "battle-throw: more than %zu balls, this one is not held", kHoldMax);
    }
    original(ctx);
}

// The game's ball_deinit: a noted ball the game removes itself is forgotten.
void on_hold_deinit(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    for (std::atomic<uint64_t>& h : g_held) {
        uint64_t ball = ctx->x[0];
        if (ball != 0 && h.compare_exchange_strong(ball, 0, std::memory_order_relaxed)) {
            rt::log(rt::LogLevel::Info, "battle-throw: the game removed a noted ball itself");
            break;
        }
    }
    original(ctx);
}

// The battle's end: the noted balls removed as the game removes a ball
// (deinit, then out of the model list), before the end waits for its
// models. Also when the mod was switched off in the battle.
void on_hold_release(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    g_hold_armed.store(false, std::memory_order_relaxed);
    const uint64_t list = g_hold_list.exchange(0, std::memory_order_relaxed);
    unsigned removed = 0;
    for (std::atomic<uint64_t>& h : g_held) {
        const uint64_t ball = h.exchange(0, std::memory_order_relaxed);
        if (ball == 0)
            continue;
        uint64_t r = 0;
        if (!call(ctx, g_config.hold_deinit, {ball}, &r))
            return;
        // list_remove takes the address of a pointer to the ball: a slot
        // below the stack, the call's frame below that.
        const uint64_t sp = ctx->sp;
        const uint64_t slot = (sp - 16) & ~uint64_t{15};
        if (list == 0 || !rt::access_allowed(slot, 8, rt::kPageWrite)) {
            rt::log(rt::LogLevel::Warn, "battle-throw: held ball deinited but not out of the list (%s)",
                    list == 0 ? "no list" : "stack not writable");
            continue;
        }
        rt::store<uint64_t>(slot, ball);
        ctx->sp = slot - 16;
        const bool done = call(ctx, g_config.hold_list_remove, {list, slot}, &r);
        ctx->sp = sp;
        if (!done)
            return;
        ++removed;
    }
    if (removed > 0)
        rt::log(rt::LogLevel::Info, "battle-throw: %u held ball%s removed before the battle's end", removed,
                removed == 1 ? "" : "s");
    original(ctx);
}

// The store of a caught Pokémon: the ball of our caught throw goes into its
// data before the original stores it (Fang-48). Only the first store
// returning to catch_store_lr after the catch; a Go catch finds nothing noted.
void on_catch_store(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    if (g_on.load(std::memory_order_relaxed) && ctx->x[30] == g_config.store_lr) {
        const uint64_t ball = g_caught_ball.exchange(0, std::memory_order_relaxed);
        if (ball != 0) {
            if (!rt::access_allowed(ctx->x[1], 8, rt::kPageRead)) {
                rt::log(rt::LogLevel::Warn, "battle-throw: catch_store x1 unreadable, the catch keeps the game's ball");
            } else {
                uint64_t r = 0;
                if (!call(ctx, g_config.ball_setter, {rt::load<uint64_t>(ctx->x[1]), ball}, &r))
                    return;
                rt::log(rt::LogLevel::Info, "battle-throw: caught ball %" PRIu64 " stored", ball);
            }
        }
    }
    original(ctx);
}

// A caught ball whose sequence keeps it in view past the throw wait.
bool early_release() {
    return g_config.caught_release != 0 && g_success.load(std::memory_order_relaxed);
}

// acts throw_started on throw_start's entry registers (x0-x28; sp and the
// frame stay the current call's, the old frame may be gone); ctx is left as
// it was. False if the guest halted.
bool run_started(lgp::CpuState* ctx, bool log) {
    if (g_config.throw_started.empty())
        return true;
    const CpuState saved = *ctx;
    std::copy(g_start_entry.x, g_start_entry.x + 29, ctx->x);
    uint64_t ret = 0;
    run_hook_acts(g_config.throw_started, false, ctx, g_start_entry, 1, &ret, log);
    if (ctx->halt != kHaltNone)
        return false;
    *ctx = saved;
    return true;
}

void on_start(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    if (!g_on.load(std::memory_order_relaxed)) {
        original(ctx);
        return;
    }
    g_caught_ball.store(0, std::memory_order_relaxed);  // a new throw: an older catch is not ours any more
    if (g_config.has_trainer) {
        uint32_t kind = 0;
        const bool readable = read_expr(g_config.trainer, g_config.trainer_size, *ctx, &kind);
        if (!readable || kind != 0) {
            rt::log(readable ? rt::LogLevel::Info : rt::LogLevel::Warn,
                    readable ? "battle-throw: not a wild battle (kind %u), the game's own throw"
                             : "battle-throw: trainer_battle unreadable, the game's own throw",
                    kind);
            original(ctx);
            return;
        }
    }
    if (!g_config.success_at_handler) {
        g_success.store(read_success(*ctx), std::memory_order_relaxed);
        g_shakes.store(read_shakes(*ctx), std::memory_order_relaxed);
    }
    const bool ball = g_config.ball && ball_prepare(ctx);
    if (ctx->halt != kHaltNone)
        return;
    g_ball.view = ball ? g_ball.view : 0;
    g_position.store(ctx->x[1], std::memory_order_relaxed);
    g_start_x0.store(ctx->x[0], std::memory_order_relaxed);
    g_sendout_started.store(false, std::memory_order_relaxed);
    if (!g_config.throw_prepare.empty()) {
        const CpuState entry = *ctx;
        uint64_t ret = 0;
        run_hook_acts(g_config.throw_prepare, false, ctx, entry, 1, &ret);
        if (ctx->halt != kHaltNone)
            return;
        *ctx = entry;
    }
    const bool partner = is_partner(ctx, ctx->x[0]);
    if (ctx->halt != kHaltNone)
        return;
    g_partner.store(partner, std::memory_order_relaxed);
    g_start_entry = *ctx;
    const uint64_t thrown = ctx->x[2];  // the thrown item; its number is the ball (Fang-32b)
    const bool success = g_success.load(std::memory_order_relaxed);
    const uint32_t shakes = g_shakes.load(std::memory_order_relaxed);
    const std::string* text = &seq_for(g_config.seq, success, shakes);
    if (partner) {
        const std::string& own = seq_for(g_config.partner_seq, success, shakes);
        if (!own.empty())
            text = &own;
        rt::log(rt::LogLevel::Info, "battle-throw: the partner throws%s", own.empty() ? ", no partner sequence" : "");
    }
    seq_write(*text);
    uint64_t r = 0;
    const bool played = call3(ctx, g_config.recall_start, ctx->x[0], ctx->x[1], 0, &r);
    seq_restore();  // seq_path copies the text and seq_play opens the file within the call (Fang-22 C)
    if (!played)
        return;
    if (!run_started(ctx, true))
        return;
    if (g_config.hold) {  // from now on the throw's ball is noted at its init
        uint64_t list = 0;
        if (eval_ptr(g_config.hold_list, g_start_entry, &list)) {
            g_hold_list.store(list, std::memory_order_relaxed);
            g_hold_armed.store(true, std::memory_order_relaxed);
        } else {
            rt::log(rt::LogLevel::Warn, "battle-throw: ball_hold_list unreadable, the ball is not held");
        }
    }
    if (g_config.store_ball && g_success.load(std::memory_order_relaxed)) {
        if (thrown != 0 && thrown <= 0xff)
            g_caught_ball.store(thrown, std::memory_order_relaxed);
        else
            rt::log(rt::LogLevel::Warn, "battle-throw: thrown item %" PRIu64 " is no ball, the catch keeps the game's ball",
                    thrown);
    }
    g_left.store(early_release() ? g_config.caught_release : g_config.wait_frames, std::memory_order_relaxed);
    g_phase.store(kHold, std::memory_order_relaxed);
    rt::log(rt::LogLevel::Info, "battle-throw: recall played, %s", g_success.load() ? "caught" : "breaks free");
    original(ctx);
}

// What follows the recall: the ball on the ground, else the end (caught) or
// the send-out (a miss).
Phase after_recall() {
    if (g_ball.view != 0)
        return kBallLoad;
    return g_success.load(std::memory_order_relaxed) ? kIdle : kSendOut;
}

void on_wait(lgp::CpuState* ctx, lgp::LiftedFn original, void*) {
    const uint32_t phase = g_phase.load(std::memory_order_relaxed);
    if (phase == kHold) {
        if (const uint32_t left = g_left.load(std::memory_order_relaxed); left > 0) {
            g_left.store(left - 1, std::memory_order_relaxed);
            if (!run_started(ctx, false))  // logged once after the recall
                return;
            ctx->x[0] = 0;  // not done
            return;
        }
        if (early_release()) {
            g_phase.store(kIdle, std::memory_order_relaxed);
            rt::log(rt::LogLevel::Info, "battle-throw: caught, released before the sequence ends");
            original(ctx);
            return;
        }
        bool keep = g_config.keep && !g_success.load(std::memory_order_relaxed) && g_ball.view == 0;
        if (keep && !keep_readable(*ctx)) {  // a wrong expr would wait for ever: the plain recall instead
            rt::log(rt::LogLevel::Warn, "battle-throw: keep_busy unreadable, recall wait and send-out as before");
            keep = false;
        }
        if (keep) {
            g_phase.store(kKeepWait, std::memory_order_relaxed);
            g_left.store(0, std::memory_order_relaxed);
        } else {
            g_phase.store(kRecallWait, std::memory_order_relaxed);
            g_left.store(0, std::memory_order_relaxed);  // counts the calls up
        }
    }
    if (g_phase.load(std::memory_order_relaxed) == kKeepWait) {
        const uint32_t calls = g_left.load(std::memory_order_relaxed) + 1;
        g_left.store(calls, std::memory_order_relaxed);
        if (keep_busy(*ctx)) {
            if (calls == kRecallWaitWarn)
                rt::log(rt::LogLevel::Warn, "battle-throw: sequence still busy after %u calls, still waiting", calls);
            ctx->x[0] = 0;
            return;
        }
        rt::log(rt::LogLevel::Info, "battle-throw: recall sequence done after %u calls, model kept", calls);
        const CpuState entry = *ctx;
        uint64_t ret = 0;
        run_hook_acts(g_config.keep_sendout, false, ctx, entry, 1, &ret);
        if (ctx->halt != kHaltNone)
            return;
        const std::string& keep_seq =
            g_partner.load(std::memory_order_relaxed) && !g_config.partner_keep_seq.empty() ? g_config.partner_keep_seq
                                                                                           : g_config.keep_seq;
        if (!keep_seq.empty()) {
            const bool written = seq_write(keep_seq);
            uint64_t r = 0;
            const bool played = call3(ctx, g_config.recall_start, g_start_x0.load(std::memory_order_relaxed),
                                      g_position.load(std::memory_order_relaxed), 0, &r);
            seq_restore();
            if (!played)
                return;
            if (!written)
                rt::log(rt::LogLevel::Warn, "battle-throw: %s not written, the recall played instead",
                        keep_seq.c_str());
        }
        *ctx = entry;
        g_phase.store(kKeepPlay, std::memory_order_relaxed);
        g_left.store(0, std::memory_order_relaxed);
        ctx->x[0] = 0;
        return;
    }
    if (g_phase.load(std::memory_order_relaxed) == kKeepPlay) {
        const uint32_t calls = g_left.load(std::memory_order_relaxed) + 1;
        g_left.store(calls, std::memory_order_relaxed);
        if (keep_busy(*ctx)) {
            if (calls == kRecallWaitWarn)
                rt::log(rt::LogLevel::Warn, "battle-throw: send-out sequence still busy after %u calls", calls);
            ctx->x[0] = 0;
            return;
        }
        g_phase.store(kIdle, std::memory_order_relaxed);
        rt::log(rt::LogLevel::Info, "battle-throw: sent out again (model kept) after %u calls", calls);
        original(ctx);
        return;
    }
    if (g_phase.load(std::memory_order_relaxed) == kRecallWait) {
        uint64_t r = 0;
        if (!call(ctx, g_config.recall_wait, {ctx->x[0]}, &r))
            return;
        const uint32_t calls = g_left.load(std::memory_order_relaxed) + 1;
        g_left.store(calls, std::memory_order_relaxed);
        if ((r & 0xff) == 0) {
            if (calls == kRecallWaitWarn)
                rt::log(rt::LogLevel::Warn, "battle-throw: recall wait not done after %u calls, still waiting", calls);
            ctx->x[0] = 0;
            return;
        }
        rt::log(rt::LogLevel::Info, "battle-throw: recall wait done after %u calls", calls);
        g_phase.store(after_recall(), std::memory_order_relaxed);
    }
    if (g_phase.load(std::memory_order_relaxed) == kBallLoad) {
        bool gone = false;
        if (ball_build(ctx, &gone)) {
            g_left.store(step_frames(0), std::memory_order_relaxed);
            g_phase.store(kBallAnim, std::memory_order_relaxed);
            ctx->x[0] = 0;
            return;
        }
        if (ctx->halt != kHaltNone)
            return;
        if (!gone) {
            ctx->x[0] = 0;
            return;
        }
        g_ball.view = 0;
        g_phase.store(kBallClear, std::memory_order_relaxed);
        g_left.store(0, std::memory_order_relaxed);
    }
    if (g_phase.load(std::memory_order_relaxed) == kBallAnim) {
        if (g_config.place_every_frame)
            place_ball();  // every call, as the send-out keeps its ball in place (Fang-17)
        if (const uint32_t left = g_left.load(std::memory_order_relaxed); left > 0) {
            g_left.store(left - 1, std::memory_order_relaxed);
            ctx->x[0] = 0;
            return;
        }
        if (++g_ball.step < g_ball.steps) {
            if (!ball_step(ctx, g_ball.step))
                return;
            g_left.store(step_frames(g_ball.step), std::memory_order_relaxed);
            ctx->x[0] = 0;
            return;
        }
        ball_remove(ctx);
        if (ctx->halt != kHaltNone)
            return;
        g_ball.view = 0;
        g_phase.store(kBallClear, std::memory_order_relaxed);
        g_left.store(0, std::memory_order_relaxed);
    }
    if (g_phase.load(std::memory_order_relaxed) == kBallClear) {
        if (!ball_clear(ctx)) {
            if (ctx->halt == kHaltNone)
                ctx->x[0] = 0;
            return;
        }
        if (g_success.load(std::memory_order_relaxed)) {
            g_phase.store(kIdle, std::memory_order_relaxed);
            original(ctx);
            return;
        }
        g_phase.store(kSendOut, std::memory_order_relaxed);
    }
    if (g_phase.load(std::memory_order_relaxed) == kSendOut) {
        uint64_t r = 0;
        if (!g_sendout_started.load(std::memory_order_relaxed)) {
            if (!call(ctx, g_config.sendout_start,
                      {ctx->x[0], g_position.load(std::memory_order_relaxed) & 0xffffffff, g_config.sendout_side,
                       g_config.sendout_team},
                      &r))
                return;
            g_sendout_started.store(true, std::memory_order_relaxed);
        }
        if (!call3(ctx, g_config.sendout_update, ctx->x[0], 0, 0, &r))
            return;
        if ((r & 0xff) == 0) {
            ctx->x[0] = 0;
            return;
        }
        g_phase.store(kIdle, std::memory_order_relaxed);
        rt::log(rt::LogLevel::Info, "battle-throw: sent out again");
        original(ctx);
        return;
    }
    original(ctx);
}

const ModLine* find_line(const ModFile& file, const char* directive, const char* name) {
    for (const ModLine& l : file.lines)
        if (l.directive == directive && l.name == name)
            return &l;
    return nullptr;
}

// `acts NAME 0 ITEMS`: comma separated LGP_HOOK_ACT items without their door,
// only set and call.
bool parse_act_list(const char* name, std::string_view rest, std::vector<HookAct>* out, std::string* why) {
    std::string spec;  // the items with a door 0, as LGP_HOOK_ACT wants them
    while (!rest.empty()) {
        const size_t comma = std::min(rest.find(','), rest.size());
        if (comma > 0)
            spec += (spec.empty() ? "0:" : ",0:") + std::string(rest.substr(0, comma));
        rest.remove_prefix(std::min(comma + 1, rest.size()));
    }
    std::string error;
    if (spec.empty() || !parse_hook_acts(spec, out, &error)) {
        *why = std::string("acts ") + name + ": " + (spec.empty() ? std::string("no acts") : error);
        return false;
    }
    for (const HookAct& a : *out)
        if (a.kind != HookAct::kCall && a.kind != HookAct::kSet) {
            *why = std::string("acts ") + name + ": only set and call";
            return false;
        }
    return true;
}

bool install(const ModFile& file, const StartOptions&, std::string* why) {
    Config c;
    const std::optional<uint64_t> recall = file.value("recall_start");
    const std::optional<uint64_t> sendout = file.value("sendout_update");
    if (!recall || !sendout) {
        *why = std::string("no `value ") + (recall ? "sendout_update" : "recall_start") + "` in " +
               file.path.filename().string();
        return false;
    }
    c.recall_start = *recall;
    c.sendout_update = *sendout;
    const ModLine* success = find_line(file, "expr", "throw_success");
    if (success == nullptr || (success->number != 1 && success->number != 2 && success->number != 4) ||
        !parse_hook_expr(success->rest, &c.success) || c.success.reg == HookDump::kRet) {
        *why = "no `expr throw_success SIZE EXPR` (SIZE 1, 2 or 4, EXPR as in LGP_HOOK_DUMP) in " +
               file.path.filename().string();
        return false;
    }
    c.success_size = static_cast<unsigned>(success->number);
    if (const std::optional<uint64_t> n = file.value("wait_frames"))
        c.wait_frames = static_cast<uint32_t>(*n);
    if (const std::optional<uint64_t> n = file.value("caught_release"))
        c.caught_release = static_cast<uint32_t>(*n);
    {
        const ModLine* busy = find_line(file, "expr", "keep_busy");
        const ModLine* acts = find_line(file, "acts", "keep_sendout");
        const ModLine* seq = find_line(file, "path", "keep_sendout_seq");
        if ((busy != nullptr) != (acts != nullptr || seq != nullptr)) {
            *why = std::string("expr keep_busy goes with acts keep_sendout or path keep_sendout_seq in ") +
                   file.path.filename().string();
            return false;
        }
        if (seq != nullptr) {
            if (seq->rest.empty() || seq->rest.size() >= 0x100) {
                *why = "path keep_sendout_seq: expected 0 and a TEXT under 256 characters";
                return false;
            }
            c.keep_seq = seq->rest;
        }
        if (busy != nullptr) {
            if ((busy->number != 1 && busy->number != 2 && busy->number != 4) ||
                !parse_hook_expr(busy->rest, &c.keep_busy)) {
                *why = "expr keep_busy: expected SIZE 1, 2 or 4 and an EXPR";
                return false;
            }
            c.keep_busy_size = static_cast<unsigned>(busy->number);
            if (acts != nullptr && !parse_act_list("keep_sendout", acts->rest, &c.keep_sendout, why))
                return false;
            c.keep = true;
        }
    }
    if (const ModLine* l = find_line(file, "expr", "trainer_battle")) {
        if ((l->number != 1 && l->number != 2 && l->number != 4) || !parse_hook_expr(l->rest, &c.trainer)) {
            *why = "expr trainer_battle: expected SIZE 1, 2 or 4 and an EXPR";
            return false;
        }
        c.trainer_size = static_cast<unsigned>(l->number);
        c.has_trainer = true;
    }
    if (const ModLine* l = find_line(file, "acts", "throw_prepare"))
        if (!parse_act_list("throw_prepare", l->rest, &c.throw_prepare, why))
            return false;
    if (const ModLine* l = find_line(file, "acts", "throw_started"))
        if (!parse_act_list("throw_started", l->rest, &c.throw_started, why))
            return false;
    // Without the recall wait the battle's end hangs after a miss (Fang-14):
    // the mod does not start rather than run without it.
    const std::optional<uint64_t> recall_wait = file.value("recall_wait");
    if (!recall_wait || *recall_wait == 0) {
        *why = "no `value recall_wait` in " + file.path.filename().string() +
               " (without it the end of the battle hangs after a ball breaks free, Fang-14)";
        return false;
    }
    c.recall_wait = *recall_wait;
    const std::optional<uint64_t> sendout_start = file.value("sendout_start");
    const std::optional<uint64_t> sendout_side = file.value("sendout_side");
    if (!sendout_start || *sendout_start == 0 || !sendout_side) {
        *why = std::string("no `value ") + (sendout_start && *sendout_start != 0 ? "sendout_side" : "sendout_start") +
               "` in " + file.path.filename().string() +
               " (without the send-out start a ball breaking free crashes, Fang-15)";
        return false;
    }
    c.sendout_start = *sendout_start;
    c.sendout_side = *sendout_side;
    if (const std::optional<uint64_t> n = file.value("sendout_team"))
        c.sendout_team = *n;
    c.success_at_handler = file.hook("throw_handler").has_value();
    if (const ModLine* l = find_line(file, "expr", "throw_shakes")) {
        if ((l->number != 1 && l->number != 2 && l->number != 4) || !parse_hook_expr(l->rest, &c.shakes)) {
            *why = "expr throw_shakes: expected SIZE 1, 2 or 4 and an EXPR";
            return false;
        }
        c.shakes_size = static_cast<unsigned>(l->number);
        c.has_shakes = true;
    }
    {
        const char* names[5] = {"throw_seq_success", "throw_seq_fail0", "throw_seq_fail1", "throw_seq_fail2",
                                "throw_seq_fail3"};
        bool any = false;
        for (int i = 0; i < 5; ++i)
            if (const ModLine* l = find_line(file, "path", names[i])) {
                if (l->rest.empty() || l->rest.size() >= 0x100) {
                    *why = std::string("path ") + names[i] + ": expected 0 and a TEXT under 256 characters";
                    return false;
                }
                c.seq[i] = l->rest;
                any = true;
            }
        const std::optional<uint64_t> text = file.value("recall_seq_text");
        if (any != (text && *text != 0) && (any || c.keep_seq.empty())) {
            *why = any ? "path throw_seq_…: also `value recall_seq_text` is needed in " + file.path.filename().string()
                       : "value recall_seq_text: also a `path throw_seq_success|fail0..3 0 TEXT` is needed in " +
                             file.path.filename().string();
            return false;
        }
        if (any && file.value("ball_create")) {
            *why = "the throw's sequences build the ball themselves: value ball_create and path throw_seq_… exclude "
                   "each other in " + file.path.filename().string();
            return false;
        }
        if (!c.keep_seq.empty() && (!text || *text == 0)) {
            *why = "path keep_sendout_seq: also `value recall_seq_text` is needed in " + file.path.filename().string();
            return false;
        }
        const char* partner_names[5] = {"throw_seq_partner_success", "throw_seq_partner_fail0",
                                        "throw_seq_partner_fail1", "throw_seq_partner_fail2",
                                        "throw_seq_partner_fail3"};
        bool any_partner = false;
        for (int i = 0; i < 6; ++i) {
            const char* name = i < 5 ? partner_names[i] : "keep_sendout_seq_partner";
            if (const ModLine* l = find_line(file, "path", name)) {
                if (l->rest.empty() || l->rest.size() >= 0x100) {
                    *why = std::string("path ") + name + ": expected 0 and a TEXT under 256 characters";
                    return false;
                }
                (i < 5 ? c.partner_seq[i] : c.partner_keep_seq) = l->rest;
                any_partner = true;
            }
        }
        const std::optional<uint64_t> partner_slot = file.value("throw_partner_slot");
        if (any_partner && (!partner_slot || *partner_slot == 0)) {
            *why = "path throw_seq_partner_…: also `value throw_partner_slot` is needed in " +
                   file.path.filename().string();
            return false;
        }
        if (any_partner && (!text || *text == 0)) {
            *why = "path throw_seq_partner_…: also `value recall_seq_text` is needed in " + file.path.filename().string();
            return false;
        }
        if (!c.partner_keep_seq.empty() && c.keep_seq.empty() && !c.keep) {
            *why = "path keep_sendout_seq_partner: also `expr keep_busy` is needed in " + file.path.filename().string();
            return false;
        }
        if (partner_slot && *partner_slot != 0 && !any_partner) {
            *why = "value throw_partner_slot: also a `path throw_seq_partner_success|fail0..3 0 TEXT` is needed in " +
                   file.path.filename().string();
            return false;
        }
        c.partner_slot = any_partner ? *partner_slot : 0;
        c.seq_text = (any || !c.keep_seq.empty() || any_partner) ? *text : 0;
        if (c.caught_release != 0 && c.seq[0].empty()) {
            *why = "value caught_release needs `path throw_seq_success` (the sequence keeps the ball in view) in " +
                   file.path.filename().string();
            return false;
        }
    }
    {  // the held ball (Fang-42): all of these or none
        const char* hooks[3] = {"ball_hold_init", "ball_hold_deinit", "ball_hold_release"};
        const ModLine* list = find_line(file, "expr", "ball_hold_list");
        bool any = list != nullptr || file.value("ball_hold_lr").has_value();
        for (const char* h : hooks)
            any = any || file.hook(h).has_value();
        if (any) {
            for (const char* h : hooks)
                if (!file.hook(h)) {
                    *why = std::string("the held ball also needs `hook ") + h + "` in " + file.path.filename().string();
                    return false;
                }
            if (list == nullptr || !parse_hook_expr(list->rest, &c.hold_list) || c.hold_list.reg == HookDump::kRet) {
                *why = "the held ball also needs `expr ball_hold_list 0 EXPR` in " + file.path.filename().string();
                return false;
            }
            const std::optional<uint64_t> deinit = file.value("ball_deinit");
            const std::optional<uint64_t> remove = file.value("ball_list_remove");
            if (!deinit || *deinit == 0 || !remove || *remove == 0) {
                *why = std::string("the held ball also needs `value ") + (deinit && *deinit != 0 ? "ball_list_remove" : "ball_deinit") +
                       "` in " + file.path.filename().string() + " (without the removal the end of the battle hangs white)";
                return false;
            }
            if (c.seq_text == 0) {
                *why = "the held ball comes from the throw's sequences: also a `path throw_seq_…` is needed in " +
                       file.path.filename().string();
                return false;
            }
            c.hold_deinit = *deinit;
            c.hold_list_remove = *remove;
            if (const std::optional<uint64_t> n = file.value("ball_hold_lr"))
                c.hold_lr = *n;
            c.hold = true;
        }
    }
    {  // the thrown ball stored with the catch (Fang-48): all three or none
        const std::optional<uint64_t> lr = file.value("catch_store_lr");
        const std::optional<uint64_t> setter = file.value("ball_setter");
        if (file.hook("catch_store") || lr || setter) {
            if (!file.hook("catch_store") || !lr || *lr == 0 || !setter || *setter == 0) {
                *why = "the stored ball needs `hook catch_store`, `value catch_store_lr` and `value ball_setter` in " +
                       file.path.filename().string();
                return false;
            }
            c.store_lr = *lr;
            c.ball_setter = *setter;
            c.store_ball = true;
        }
    }
    if (file.value("ball_create")) {
        const struct {
            const char* name;
            uint64_t* to;
        } values[] = {{"ball_alloc_slot", &c.alloc_slot}, {"ball_create", &c.create},
                      {"ball_init", &c.init},             {"ball_anim_attach", &c.attach},
                      {"ball_anim_play", &c.play},        {"ball_list_add", &c.list_add},
                      {"ball_list_remove", &c.list_remove}, {"ball_deinit", &c.deinit},
                      {"res_preload", &c.preload},        {"res_model", &c.res_model},
                      {"res_anim", &c.res_anim},          {"path_ball_model", &c.path_model},
                      {"path_from_text", &c.path_text}};
        for (const auto& v : values) {
            const std::optional<uint64_t> n = file.value(v.name);
            if (!n) {
                *why = std::string("with value ball_create also `value ") + v.name + "` is needed in " +
                       file.path.filename().string();
                return false;
            }
            *v.to = *n;
        }
        const struct {
            const char* name;
            HookDump* to;
        } exprs[] = {
            {"res_manager", &c.manager}, {"view_heap", &c.heap}, {"model_list", &c.list}, {"ball_pos", &c.pos}};
        if (const std::optional<uint64_t> n = file.value("slot_position"))
            c.slot_position = *n;
        if (const std::optional<uint64_t> n = file.value("ball_target_slot"))
            c.target_slot = *n;
        if (const std::optional<uint64_t> n = file.value("ball_place_every_frame"))
            c.place_every_frame = *n != 0;
        if (const std::optional<uint64_t> n = file.value("ball_raise_milli"))
            c.raise = static_cast<float>(static_cast<int64_t>(*n)) / 1000.0f;
        for (const auto& e : exprs) {
            if (c.slot_position != 0 && e.to == &c.pos)
                continue;
            const ModLine* l = find_line(file, "expr", e.name);
            if (l == nullptr || !parse_hook_expr(l->rest, e.to) || e.to->reg == HookDump::kRet) {
                *why = std::string("with value ball_create also `expr ") + e.name + " 0 EXPR` is needed in " +
                       file.path.filename().string();
                return false;
            }
        }
        if (const std::optional<uint64_t> n = file.value("path_package"))
            c.path_package = *n;
        if (const ModLine* l = find_line(file, "expr", "ball_pos_add")) {
            if (!parse_hook_expr(l->rest, &c.pos_add) || c.pos_add.reg == HookDump::kRet) {
                *why = "expr ball_pos_add: expected an EXPR";
                return false;
            }
            c.has_pos_add = true;
        }
        const std::optional<uint64_t> res_poll = file.value("res_poll");
        if (!res_poll || *res_poll == 0) {
            *why = "with value ball_create also `value res_poll` is needed in " + file.path.filename().string() +
                   " (without it the ball is built before its animation is ready and does not move, Fang-18)";
            return false;
        }
        c.res_poll = *res_poll;
        if (const std::optional<uint64_t> n = file.value("model_preload"))
            c.no_model_preload = *n == 0;
        if (const std::optional<uint64_t> n = file.value("res_preload_kind_model"))
            c.kind_model = *n;
        if (const std::optional<uint64_t> n = file.value("res_preload_kind_anim"))
            c.kind_anim = *n;
        if (const std::optional<uint64_t> n = file.value("res_preload_x5"))
            c.preload_x5 = *n;
        if (const std::optional<uint64_t> n = file.value("ball_ref_offset"))
            c.ref_offset = *n;
        if (const std::optional<uint64_t> n = file.value("ball_addref_slot"))
            c.addref_slot = *n;
        if (const std::optional<uint64_t> n = file.value("ball_release_slot"))
            c.release_slot = *n;
        c.refcount_offset = file.value("ball_refcount_offset");
        const std::optional<uint64_t> res_clear = file.value("res_clear");
        if (!res_clear || *res_clear == 0) {
            *why = "with value ball_create also `value res_clear` is needed in " + file.path.filename().string() +
                   " (without it the end of the battle hangs white, Fang-16)";
            return false;
        }
        c.res_clear = *res_clear;
        c.seq_busy_offset = file.value("seq_busy_offset");
        c.free_slot = file.value("ball_free_slot");
        if (!c.free_slot) {
            *why = "with value ball_create also `value ball_free_slot` is needed in " + file.path.filename().string() +
                   " (without it the end of the battle hangs white, Fang-16)";
            return false;
        }
        if (const ModLine* l = find_line(file, "expr", "heap_stack")) {
            if (!parse_hook_expr(l->rest, &c.heap_stack) || c.heap_stack.reg == HookDump::kRet) {
                *why = "expr heap_stack: expected an EXPR";
                return false;
            }
            c.has_heap_stack = true;
            if (const std::optional<uint64_t> n = file.value("heap_stack_count"))
                c.heap_stack_count = *n;
        }
        const ModLine* package = find_line(file, "path", "capture_package");
        const ModLine* anim = find_line(file, "path", "capture_anim");
        if (package == nullptr || anim == nullptr || package->rest.empty() || anim->rest.empty() ||
            package->rest.size() >= 0xc0 || anim->rest.size() >= 0xc0) {
            *why = "with value ball_create also `path capture_package 0 TEXT` and `path capture_anim 0 TEXT` (under "
                   "192 characters) are needed";
            return false;
        }
        c.package = package->rest;
        c.anim = anim->rest;
        const struct {
            const char* name;
            uint64_t* to;
        } opts[] = {{"ball_slot", &c.slot},
                    {"ball_kind", &c.kind},
                    {"ball_pos_offset", &c.pos_offset},
                    {"ball_dirty_offset", &c.dirty_offset},
                    {"name_word2", &c.name_word2}};
        for (const auto& o : opts)
            if (const std::optional<uint64_t> n = file.value(o.name))
                *o.to = *n;
        const struct {
            const char* name;
            uint32_t* to;
        } frames[] = {{"load_frames", &c.load_frames},
                      {"fall_frames", &c.fall_frames},
                      {"shake_frames", &c.shake_frames},
                      {"end_frames", &c.end_frames}};
        for (const auto& f : frames)
            if (const std::optional<uint64_t> n = file.value(f.name))
                *f.to = static_cast<uint32_t>(*n);
        c.ball = true;
    }
    seq_restore();  // a throw cut short by a restart: the original text back first
    g_seq_original.clear();
    g_config = c;
    g_ball = Ball();
    g_phase.store(kIdle);
    g_hold_armed.store(false);
    g_hold_list.store(0);
    for (std::atomic<uint64_t>& h : g_held)
        h.store(0);
    g_caught_ball.store(0);
    if (!hook_role(file, "throw_start", &on_start, nullptr, why) ||
        !hook_role(file, "throw_wait", &on_wait, nullptr, why))
        return false;
    if (c.hold && (!hook_role(file, "ball_hold_init", &on_hold_init, nullptr, why) ||
                   !hook_role(file, "ball_hold_deinit", &on_hold_deinit, nullptr, why) ||
                   !hook_role(file, "ball_hold_release", &on_hold_release, nullptr, why)))
        return false;
    if (c.store_ball && !hook_role(file, "catch_store", &on_catch_store, nullptr, why))
        return false;
    return !c.success_at_handler || hook_role(file, "throw_handler", &on_handler, nullptr, why);
}

}  // namespace

const ModImpl& battle_throw_mod() {
    static const ModImpl m = {
        {"battle-throw", "Catch style in battle",
         "Classic: the ball flies in the battle as in the main games, draws the Pokémon in and wobbles on the "
         "ground; if it breaks out, the Pokémon appears again. Let's Go: a ball chosen in the battle menu ends the "
         "battle, and the Let's Go catch scene follows. Only with Catch in battle.",
         true, true, false},
        &install,
        &g_on,
    };
    return m;
}

}  // namespace lgp::mods::detail

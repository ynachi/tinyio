# The Raft Paper I Wish I Had: A Systems Engineer's Specification (2026 Edition)

> **Why this document exists:**  
> The original 2014 USENIX ATC Raft paper (*"In Search of an Understandable Consensus Algorithm"*) is a masterpiece of pedagogy, **but if you only implement the 18-page conference paper, your production cluster will suffer split-brains on network partitions, stall during leader elections, corrupt linearizable reads, and choke NVMe drives.**  
> Half of the real algorithm lives in Diego Ongaro's 2014 PhD dissertation, and the other half was discovered by engineers building `etcd`, `TiKV`, `CockroachDB`, `Redpanda`, and `RethinkDB` over the last decade.  
>
> This is the **complete, demystified blueprint**—written for a C++ systems engineer building on a single-threaded, shared-nothing fiber runtime (`tinyio::IO`) and a unified 64-bit virtual address log (`HybridLog` / `FasterLog`).

---

## 1. The Big Mental Shift: What Raft Actually Does

Forget distributed magic for a moment. Every node in a 3-node or 5-node Raft group has two pieces of state:
1. **The Log (`HybridLog`):** An ordered, append-only array of commands indexed $1, 2, 3, \dots, N$.
2. **The State Machine:** Your actual database (KV store, SQL table, metadata catalog) that consumes commands from the log one by one (`lastApplied = 1, 2, 3...`).

```
+-----------------------------------------------------------------------------+
|                    Single Node's Consensus Pipeline                         |
+------------------------+-------------------------+--------------------------+
| 1. Consensus Module    | 2. Persistent Log       | 3. State Machine (KV)    |
| Receives RPCs,         | [1: Set X=1]            | Applies entry i ONLY     |
| tracks Term & Quorum   | [2: Set Y=5] <--commit  | when i <= commitIndex!   |
| advances commitIndex   | [3: Set X=9] <--tail    | Deterministic execution  |
+------------------------+-------------------------+--------------------------+
```

**The Entire Goal of Raft:**  
Ensure that if any node applies entry $(Index = i, Term = t, Command = C)$ to its state machine, **no other node in the universe will ever apply a different command $C'$ at $Index = i$.**

Everything in Raft—terms, elections, `AppendEntries` consistency checks, and commit rules—exists solely to protect that one invariant.

---

## 2. The 5 Sacred Invariants (Memorize These, Understand Everything)

If you ever wonder *"Wait, why do I have to check `prevLogTerm` here?"* or *"Why can't a new leader immediately commit an old entry from a previous term?"*, the answer is always one of these five invariants:

```
+-----------------------------------------------------------------------------+
|                        The 5 Sacred Raft Invariants                         |
+----------------------+------------------------------------------------------+
| 1. Election Safety   | At most ONE leader can be elected in a given term.   |
+----------------------+------------------------------------------------------+
| 2. Leader Append-Only| A leader NEVER overwrites or deletes its own entries;|
|                      | it only appends new entries.                         |
+----------------------+------------------------------------------------------+
| 3. Log Matching      | If two logs contain an entry with the same Index AND |
|                      | the same Term, then the logs are IDENTICAL in all    |
|                      | entries from Index 1 up through that Index!          |
+----------------------+------------------------------------------------------+
| 4. Leader            | If a log entry is committed in a given term, that    |
|    Completeness      | entry WILL be present in the logs of the leaders     |
|                      | for all higher-numbered terms.                       |
+----------------------+------------------------------------------------------+
| 5. State Machine     | If a server has applied a log entry at a given index |
|    Safety            | to its state machine, no other server will ever      |
|                      | apply a different log entry for the same index.      |
+----------------------+------------------------------------------------------+
```

### How Invariant #3 (Log Matching) Works by Induction
Why does matching `(Index, Term)` at position $k$ prove that indices $1 \dots k-1$ are also 100% identical without comparing a single byte of history?
1. **Base Case:** A leader creates at most one entry at a given `(Index, Term)`.
2. **Inductive Step:** Whenever a leader sends an `AppendEntries` RPC for index $k$, it includes `(prevLogIndex = k - 1, prevLogTerm)`. The follower **rejects** the append unless its own entry at $k - 1$ has the exact same `prevLogTerm`.
3. By mathematical induction back to Index 1, if two nodes agree on `(Index = 5000, Term = 12)`, their logs are guaranteed bit-for-bit identical across `[1 .. 5000]`.

---

## 3. Complete State & Data Structures (No Hidden Fields!)

Figure 2 of the original paper omits several critical fields required for linearizable reads (`ReadIndex`), partition immunity (`PreVote`, `CheckQuorum`), and `HybridLog` O(1) index-to-address mapping. Here is the **true** state table you need in C++23:

### A. Persistent State (MUST be flushed to NVMe *before* replying to any RPC)

```
+-----------------------------------------------------------------------------+
|            Persistent State on Every Node (Must Survive Crash)              |
+-----------------------+-----------------------------------------------------+
| HardState Record      | - currentTerm (uint64_t): Latest term node has seen |
| (16 Bytes)            | - votedFor    (uint64_t): NodeId voted for in       |
|                       |   currentTerm (0 = None)                            |
+-----------------------+-----------------------------------------------------+
| Log Entries           | Monotonic sequence of RaftEntry records:            |
| (In HybridLog)        | [index (8B) | term (8B) | type (1B) | payload]      |
+-----------------------+-----------------------------------------------------+
```

> **Pro-Tip on `HardState` Storage:**  
> Do not write `currentTerm` and `votedFor` to a separate POSIX file with `rename()` (which causes VFS metadata locks!). Append a 16-byte `HardState` record directly into your `HybridLog` (with `HybridRecordFlags::RAFT_HARDSTATE`), or store it in Block 0 / Sector 1 of your pre-allocated segment!

### B. Volatile State on All Servers

```cpp
struct RaftVolatileState {
    Role     role         = Role::FOLLOWER; // FOLLOWER, PRE_CANDIDATE, CANDIDATE, LEADER
    uint64_t leader_id    = 0;              // Current known leader (for client redirects)
    uint64_t commit_index = 0;              // Highest log index known to be committed
    uint64_t last_applied = 0;              // Highest log index applied to State Machine
};
```

### C. Volatile State on Leaders (Reinitialized after every election)

```cpp
struct PeerProgress {
    uint64_t next_index          = 1;     // Next log index to send to this follower
    uint64_t match_index         = 0;     // Highest log index KNOWN to be replicated on peer
    bool     recent_active       = false; // Set true on any RPC reply (used by CheckQuorum!)
    uint64_t inflight_append_cnt = 0;     // Number of pipelined AppendEntries RPCs in flight
};
```

### D. The Missing Bridge: `OffsetIndex` (`RaftIndex -> LogAddress`)
In our `HybridLog`, records live at 64-bit byte addresses (`LogAddress = (Page << 18) | Offset`), while Raft talks in sequential integers (`RaftIndex = 1, 2, 3...`). How do you translate `RaftIndex = 5042` into its 64-bit `LogAddress` in $O(1)$ time?

```
+-----------------------------------------------------------------------------+
|          In-Memory Circular `OffsetIndex` Array (8 Bytes per Entry)         |
+-----------------------+-----------------------+-----------------------------+
| first_index_ = 1000   | offsets_[0] (Idx 1000)| offsets_[k] (Idx 1000 + k)  |
| (Compacted base)      | LogAddress 0x00040040 | LogAddress 0x000812A0       |
+-----------------------+-----------------------+-----------------------------+
```
* Because Raft indices are dense and strictly sequential (`1000, 1001, 1002...`), you **do not** use a `std::unordered_map` or B-Tree!
* You use a flat circular ring/vector `std::vector<LogAddress> index_to_addr_` where `addr = index_to_addr_[raft_index - first_index_]`.
* **Cost:** 1 million Raft entries in the index takes just **8 MiB of RAM** and translates `RaftIndex -> HybridLog Address` in **1 array lookup (0.5 ns)**!
* When log entries are appended or truncated, you simply `push_back()` or `resize()` the vector.

---

## 4. Wire Format of All 4 RPC Pairs (With Modern Extensions)

```cpp
namespace raft::rpc {

// ----------------------------------------------------------------------------
// 1. RequestVote / PreVote RPC (Used by Candidates & Pre-Candidates)
// ----------------------------------------------------------------------------
struct RequestVoteReq {
    bool     is_pre_vote;    // TRUE = Phase 1 PreVote (does NOT increment term or persist vote!)
    uint64_t term;           // Candidate's term (or next_term = currentTerm + 1 if is_pre_vote)
    uint64_t candidate_id;   // Node requesting vote
    uint64_t last_log_index; // Index of candidate's last log entry
    uint64_t last_log_term;  // Term of candidate's last log entry
};

struct RequestVoteResp {
    uint64_t term;           // Receiver's currentTerm, for candidate to step down if higher
    bool     vote_granted;   // True means candidate received vote
};

// ----------------------------------------------------------------------------
// 2. AppendEntries RPC (Replication, Heartbeats, and ReadIndex Heartbeats)
// ----------------------------------------------------------------------------
struct AppendEntriesReq {
    uint64_t term;           // Leader's currentTerm
    uint64_t leader_id;      // So follower knows who to redirect clients to
    uint64_t prev_log_index; // Index of log entry immediately preceding new ones
    uint64_t prev_log_term;  // Term of prev_log_index entry
    uint64_t leader_commit;  // Leader's commit_index
    uint64_t read_ctx_id;    // Non-zero when this heartbeat is confirming a ReadIndex batch!
    // Followed by 0..N contiguous RaftEntry records streamed directly from HybridLog RAM!
};

struct AppendEntriesResp {
    uint64_t term;           // Follower's currentTerm (if > leader's term, leader steps down)
    bool     success;        // True if follower matched (prev_log_index, prev_log_term)
    uint64_t match_index;    // Highest index replicated after this RPC (for pipelined ACKs)
    uint64_t read_ctx_id;    // Echoed back from AppendEntriesReq so leader can release ReadIndex!
    // Fast Log Backtracking Hint (Section 5.3 of Dissertation):
    uint64_t conflict_term;  // Term of the conflicting entry on the follower (0 if log too short)
    uint64_t conflict_index; // First index follower stores for conflict_term (or log_last_index + 1)
};

// ----------------------------------------------------------------------------
// 3. InstallSnapshot RPC (When a slow follower needs entries < BeginAddress)
// ----------------------------------------------------------------------------
struct InstallSnapshotReq {
    uint64_t term;
    uint64_t leader_id;
    uint64_t last_included_index;
    uint64_t last_included_term;
    uint64_t byte_offset;
    bool     done;
    // Followed by raw chunk bytes
};

// ----------------------------------------------------------------------------
// 4. TimeoutNow RPC (Instant Zero-Downtime Leadership Transfer)
// ----------------------------------------------------------------------------
struct TimeoutNowReq {
    uint64_t term;
    uint64_t leader_id;
};

} // namespace raft::rpc
```

---

## 5. Sub-Problem 1: Leader Election & The Two Partition Bugs

The 2014 USENIX paper describes a 3-state election (`Follower -> Candidate -> Leader`). **In production, that 3-state machine has a fatal flaw during partial network partitions.**

### Bug #1: The Disruptive Partitioned Server (Solved by `PreVote`)
**The Scenario:**  
Consider a 3-node cluster `[A, B, C]` where `A` is the healthy Leader in `Term = 4`.  
Node `C` gets isolated by a flaky switch (it cannot reach `A` or `B`).
1. Without `PreVote`, `C`'s election timer fires every 300ms. Each time, `C` increments its `currentTerm` (`5, 6, 7, ... 400`).
2. Five minutes later, the switch recovers. `C` sends a packet with `Term = 400` to Leader `A`.
3. By the universal Raft rule (*"If RPC term > currentTerm, step down to Follower"*), **healthy Leader `A` is forced to step down!**
4. Even worse: `C` cannot even win the election because its log is 5 minutes behind `A` and `B`! Yet `C` just caused a cluster outage for no reason.

**The Fix (`PreVote` Phase — Section 9.6 of Ongaro's Dissertation):**  
Before a Follower is allowed to increment `currentTerm`, it enters `Role::PRE_CANDIDATE` and sends `RequestVoteReq{is_pre_vote = true, term = currentTerm + 1}`:

```
+-----------------------------------------------------------------------------+
|                Production 4-State Election Machine (With PreVote)           |
+-----------------+     Election Timeout      +-------------------------------+
|    FOLLOWER     | ------------------------> |         PRE_CANDIDATE         |
| (Term = T)      | <------------------------ | Does NOT increment Term!      |
+-----------------+   Leader Heartbeat or     | Sends PreVote(next_term=T+1)  |
        ^             Higher Term Discovered  +-------------------------------+
        |                                                     |
        | Steps down on                                       | Wins PreVote
        | higher Term                                         | from Quorum!
        |                                                     v
+-----------------+      Wins Real Vote       +-------------------------------+
|     LEADER      | <------------------------ |           CANDIDATE           |
| (Term = T + 1)  |       from Quorum!        | Increments Term = T + 1       |
| Appends No-Op!  |                           | Persists (Term=T+1, Vote=Self)|
+-----------------+                           +-------------------------------+
```

**Exact `RequestVote` Receiver Logic (Both PreVote and Real Vote):**
When Node `R` receives `RequestVoteReq(req)` from Candidate `C`:
1. **Up-to-Date Log Check (Safety Rule):** Is `C`'s log at least as up-to-date as `R`'s log?
   ```cpp
   const bool log_ok = (req.last_log_term > my_last_log_term) ||
                       (req.last_log_term == my_last_log_term && req.last_log_index >= my_last_log_index);
   ```
   *(Critical Trap: Compare **Terms** first, and only compare **Lengths** if the last terms are equal! A log ending at `(Index=10, Term=5)` is MORE up-to-date than a log ending at `(Index=999, Term=4)`!)*
2. **Leader Lease Protection (Disruption Prevention):** If `R` has heard a heartbeat from a valid Leader within the minimum election timeout (`now - last_heartbeat_time < min_election_timeout`), **reject `RequestVote` immediately** (unless it came from `TimeoutNow` leadership transfer)!
3. **If `req.is_pre_vote == true`:**
   * Grant pre-vote if `req.term >= current_term_` and `log_ok`. Never modify `current_term_` or `voted_for_`.
4. **If `req.is_pre_vote == false` (Real Vote):**
   * If `req.term > current_term_`: update `current_term_ = req.term`, `voted_for_ = 0`, convert to `FOLLOWER`.
   * If `req.term == current_term_` and `(voted_for_ == 0 || voted_for_ == req.candidate_id)` and `log_ok`:
     * Set `voted_for_ = req.candidate_id`.
     * **Flush `(current_term_, voted_for_)` to NVMe via `HybridLog::CommitAndWait()` BEFORE sending `vote_granted = true`!**

---

## 6. Sub-Problem 2: Log Replication, Fast Backtracking & The Figure 8 Trap

### A. Why Naive `nextIndex--` Backtracking Fails in Production
In the basic paper, if a Follower rejects `AppendEntries` because `prevLogIndex` doesn't match, the Leader decrements `nextIndex[peer]--` by 1 and retries.  
**The Problem:** If a follower was offline for 2 minutes and has 50,000 diverged uncommitted entries, decrementing by 1 requires **50,000 round-trip RPCs** before replication even starts!

**The Fix: Accelerated Term-By-Term Backtracking (1 RPC per Conflicting Term)**  
When a Follower rejects `AppendEntries(prev_log_index, prev_log_term)`:

```
+-----------------------------------------------------------------------------+
|                Fast Log Backtracking on AppendEntries Rejection             |
+--------------------------------------+--------------------------------------+
| Follower Side (On Rejection)         | Leader Side (On Receiving Resp)      |
+--------------------------------------+--------------------------------------+
| Case 1: Follower has no entry at     |                                      |
|   `prev_log_index`:                  | If `conflict_term == 0`:             |
|   -> Set `conflict_term = 0`         |   `next_index = conflict_index`      |
|   -> Set `conflict_index = last + 1` |                                      |
|                                      | Else:                                |
| Case 2: Follower's entry at          |   Search Leader's log for the LAST   |
|   `prev_log_index` has term `T !=    |   entry with `term == conflict_term` |
|   prev_log_term`:                    |   - Found at `k`: `next_index = k+1` |
|   -> Set `conflict_term = T`         |   - Not found:                       |
|   -> Walk backward to find the FIRST |     `next_index = conflict_index`    |
|      index `i` in term `T`, and set  |                                      |
|      `conflict_index = i`            | Skips entire conflicting terms in O(1)
+--------------------------------------+--------------------------------------+
```

### B. THE Deadliest Bug in Raft: The "Figure 8" Commit Rule
> **NEVER commit an entry from a previous term by counting replicas alone!**

Look at this timeline across 5 servers (`S1 .. S5`):

```
+-----------------------------------------------------------------------------+
|              The Figure 8 Trap: Why Old-Term Quorums Are Unsafe!            |
+------------+-------------------+-------------------+------------------------+
| Step 1     | Step 2            | Step 3            | Step 4 (Catastrophe!)  |
| Term = 2   | Term = 3          | Term = 4          | Term = 5               |
| S1 is Ldr  | S1 crashes;       | S5 crashes; S1    | S1 crashes; S5 wins    |
| Replicates | S5 wins Term 3    | wins Term 4 and   | Term 5 (voted by S3,S4)|
| (Idx2, T2) | (voted by S3,S4)  | replicates (Idx2, | S5 overwrites Idx 2 on |
| to S1, S2  | Appends (Idx2,T3) | T2) to S3!        | S1, S2, S3 with T3!    |
|            | locally on S5     | Now on S1,S2,S3!  | Majority T2 is ERASED! |
+------------+-------------------+-------------------+------------------------+
```
Look at Step 3 above: in Term 4, entry `(Index = 2, Term = 2)` is stored on a **majority of the cluster (`S1, S2, S3`)**. Yet in Step 4, `S5` can still legally win election for Term 5 (because its last log entry at Index 2 has `Term = 3 > Term = 2`!) and **overwrite `(Index = 2, Term = 2)` across the entire cluster!**

**The Golden Commit Rule (How Leader Advances `commit_index`):**
1. **Immediately upon winning an election**, a new Leader appends a blank **No-Op Entry** at its new `current_term_`.
2. When calculating the quorum median `M` of `match_index[1..N]`:
   ```cpp
   // ONLY advance commit_index to M if the entry at M was created in current_term_!
   if (M > commit_index_ && LogTermAt(M) == current_term_) {
       commit_index_ = M; // Indirectly commits all entries 1..M!
   }
   ```
Because of the Log Matching Property (Invariant #3), the moment the new Leader commits one entry from `current_term_` (even its initial No-Op entry!), all preceding entries from older terms are permanently locked in and can never be overwritten!

---

## 7. Sub-Problem 3: Linearizable Reads (Without Writing to the Log!)

Wait—if a client sends a `GET key` request to Leader `A`, can Leader `A` just read from its local state machine and reply?  
**NO!** What if Leader `A` was silently partitioned 50ms ago, and Node `B` has already been elected the new Leader in Term $T+1$ and modified `key`? If `A` serves local reads without checking, it serves stale data (violating linearizability).

On the other hand, writing every `GET` into the Raft log kills NVMe endurance and throughput. Instead, production systems use **`ReadIndex`** (or **`LeaseRead`**):

```
+-----------------------------------------------------------------------------+
|             Linearizable `ReadIndex` Protocol (Zero Disk Writes!)           |
+------------------------+-------------------------+--------------------------+
| 1. Record ReadIndex    | 2. Confirm Leadership   | 3. Wait for StateMachine |
| Client asks to read.   | Leader sends 1 round of | Once `last_applied >=    |
| Leader records current | lightweight Heartbeats  | read_index`, execute     |
| `read_index =          | (`read_ctx_id = B`) and | read in RAM and reply    |
|  commit_index`.        | waits for Quorum ACK.   | to all batched clients!  |
| (Must have committed   | (10,000 reads share ONE |                          |
|  current-term No-Op!)  |  heartbeat round!)      |                          |
+------------------------+-------------------------+--------------------------+
```

###Even Faster: `CheckQuorum` + `LeaseRead` (Zero Network RTTs!)
If your hardware clocks have bounded drift (true on modern x86_64/ARM64 invariant TSC `CLOCK_MONOTONIC`):
1. Followers promise **not** to grant any `RequestVote` (or `PreVote`) for `min_election_timeout` after receiving a valid Leader heartbeat.
2. Therefore, if a Leader receives heartbeat ACKs from a quorum at monotonic timestamp $t_0$, the Leader is **mathematically guaranteed** that no other Leader can be elected until $t_0 + \text{min\_election\_timeout} - \text{clock\_drift}$!
3. While `MonotonicNowNs() < lease_expiry_ns_`, the Leader serves `ReadIndex` requests **instantly in 0 network RTTs and 0 disk I/Os**!

---

## 8. Sub-Problem 4: Membership Changes (Add/Remove Nodes Safely)

Why can't you just send a configuration change from 3 nodes `[A, B, C]` to 5 nodes `[A, B, C, D, E]`?  
Because during the transition, `A` and `B` might still be using the old 3-node config (`majority = 2 of {A,B,C}`), while `C, D, E` are using the new 5-node config (`majority = 3 of {A,B,C,D,E}`). **Two disjoint majorities (`{A, B}` and `{C, D, E}`) could elect two simultaneous leaders in the same term!**

### The Simple Solution: Single-Server Membership Changes (Chapter 4.1 of Dissertation)
Instead of implementing complex two-phase Joint Consensus ($C_{old,new}$), Diego Ongaro proved a much simpler rule:
> **If you only add or remove ONE node at a time, the old quorum and the new quorum are mathematically guaranteed to overlap!**

```
+-----------------------------------------------------------------------------+
|     Why Single-Server Changes Never Split-Brain (Quorum Overlap Proof)      |
+-------------------------------------+---------------------------------------+
| Old Cluster: {A, B, C} (Maj = 2)    | New Cluster: {A, B, C, D} (Maj = 3)   |
| Any 2 of {A, B, C}                  | Any 3 of {A, B, C, D}                 |
+-------------------------------------+---------------------------------------+
| Can you pick 2 from {A,B,C} and 3 from {A,B,C,D} without sharing >= 1 node? |
| NO! Even if the new quorum picks D, it still needs 2 from {A,B,C}, which    |
| MUST intersect with any 2 chosen by the old quorum!                         |
+-----------------------------------------------------------------------------+
```

**The 3 Rules for Safe Single-Server Membership Changes:**
1. **Apply Immediately on Append (Not on Commit!):** Every node (Leader and Follower alike) switches its active membership configuration the moment a `RAFT_CONFIG_CHANGE` entry is **appended** to its log (and rolls back to the previous config if that uncommitted entry is truncated).
2. **One Change at a Time:** The Leader rejects any new `ChangeMembership` request if `last_config_change_index_ > commit_index_` (and before its initial `current_term_` No-Op is committed).
3. **Catch-Up as a `Learner` First:** When adding a brand-new empty node `D`, add it as a non-voting `Learner` first until its `match_index` catches up to within 1 `HybridLog` page of the Leader's tail, then append the `PromoteLearnerToVoter(D)` entry!

---

## 9. Mapping Raft onto `tinyio::IO` & `HybridLog` (The Execution Pipeline)

In traditional multi-threaded Raft engines (`etcd`, `braft`), threads fight over mutexes between the network reactor, the WAL writer, and the state machine applier.  
On your single-threaded `tinyio::IO` + `HybridLog` architecture, **every single step is lock-free and zero-copy**:

```
+-----------------------------------------------------------------------------+
|      The "Pipeline Batching" Pattern on `tinyio::IO` + `HybridLog`          |
+------------------------+-------------------------+--------------------------+
| Fiber Group 1:         | Fiber Group 2:          | Fiber Group 3:           |
| Client Propose Fibers  | Local Disk Flusher      | Peer Replicator Fibers   |
+------------------------+-------------------------+--------------------------+
| 1. Append entry to     | `HybridLog` Flusher     | Runs IN PARALLEL with    |
|    `HybridLog` RAM tail| batches all entries in  | local NVMe flush!        |
|    (0ns syscall).      | one `io_.WriteFixed()`. | Calls `ScanInMemory()`   |
| 2. Wake Peer Fibers    | When CQE completes,     | on `HybridLog` RAM ring  |
|    AND Disk Flusher!   | updates `match_index`   | and sends `AppendEntries`|
| 3. Wait on `Baton` for | for `self` and checks   | over socket with ZERO    |
|    `commit_index >= i` | `AdvanceCommitIndex()`. | disk reads!              |
+------------------------+-------------------------+--------------------------+
```

Look closely at **Parallel Local Disk + Network Replication** above:
* The Leader **does not wait** for its own NVMe `WriteFixed()` to finish before sending `AppendEntries` over the network to followers!
* As soon as a client fiber calls `hlog.Append(...)`, the entry is sitting in the `HybridLog` in-memory `FixedBuffer` Page Ring (`>= HeadAddress`).
* The Leader immediately wakes the Peer Replicator fibers (which stream the entry straight from the RAM Page Ring to the socket via `hlog.ScanInMemory()`) **at the exact same time** that the `HybridLog` Flusher Fiber submits `io_.WriteFixed()` to the local NVMe drive!
* The Leader simply treats its own local NVMe drive as one vote in the replication quorum (`match_index[self] = last_flushed_index`), and each follower ACK as another vote (`match_index[peer]`). Whichever majority (`self + 1 follower` in a 3-node group, or even `2 followers` if the local NVMe hiccups!) finishes first advances `commit_index_`!

---

## 10. Complete C++23 Reference Skeleton of the Raft Core State Machine

Below is the complete, self-contained C++23 state machine (`RaftNodeCore`) designed to plug directly into your `HybridLog` and `tinyio::Baton` primitives.

```cpp
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

#include "hybrid_log_rfc.hpp"
#include "tinyio/runtime/fiber_sync.hpp"

namespace storage::raft {

enum class Role : uint8_t {
    FOLLOWER      = 0,
    PRE_CANDIDATE = 1,
    CANDIDATE     = 2,
    LEADER        = 3
};

enum class EntryType : uint16_t {
    DATA          = 1, // Normal state-machine command
    NO_OP         = 2, // Blank entry appended at start of new Leader's term
    CONFIG_CHANGE = 3  // Single-server membership change
};

/**
 * @brief Stack-allocated waiter for a client fiber waiting for `commit_index_ >= target_index`.
 */
struct ProposeWaitNode {
    tinyio::Baton    baton;
    uint64_t         target_index = 0;
    uint64_t         expected_term = 0;
    bool             committed    = false;
    ProposeWaitNode* next         = nullptr;

    ProposeWaitNode(tinyio::IO& io, uint64_t idx, uint64_t term) noexcept
        : baton(io), target_index(idx), expected_term(term) {}
};

/**
 * @brief O(1) Circular Mapping from sequential `RaftIndex` -> 64-bit `HybridLog::LogAddress`.
 */
class OffsetIndex {
    uint64_t                          base_index_ = 1; // Raft index of slots_[0]
    std::vector<hybridlog::LogAddress> slots_{};

public:
    [[nodiscard]] uint64_t FirstIndex() const noexcept { return base_index_; }
    [[nodiscard]] uint64_t LastIndex() const noexcept {
        return slots_.empty() ? (base_index_ - 1) : (base_index_ + slots_.size() - 1);
    }

    void Append(uint64_t raft_index, hybridlog::LogAddress addr) noexcept {
        assert(raft_index == LastIndex() + 1);
        slots_.push_back(addr);
    }

    // Truncate conflicting uncommitted suffix `[from_index .. LastIndex()]`
    void TruncateFrom(uint64_t from_index) noexcept {
        if (from_index <= LastIndex() && from_index >= base_index_) {
            slots_.resize(from_index - base_index_);
        }
    }

    [[nodiscard]] hybridlog::LogAddress Lookup(uint64_t raft_index) const noexcept {
        if (raft_index < base_index_ || raft_index > LastIndex()) {
            return hybridlog::INVALID_ADDRESS;
        }
        return slots_[raft_index - base_index_];
    }
};

/**
 * @brief Core Raft Consensus Engine backed by `storage::hybridlog::HybridLog`.
 *        Note: In `HybridLog`, we store:
 *          - `HybridRecordHeader::record_id` = `raft_index`
 *          - `HybridRecordHeader::user_meta` = `static_cast<uint16_t>(raft_term)` (or prefix in payload)
 */
class RaftCore {
private:
    tinyio::IO&           io_;
    hybridlog::HybridLog& log_;
    OffsetIndex           offset_index_{};

    // Identity & Cluster Topology
    uint64_t              node_id_      = 1;
    std::vector<uint64_t> peers_{};     // All voting node IDs in cluster (including self)

    // Persistent State
    uint64_t              current_term_ = 0;
    uint64_t              voted_for_    = 0;

    // Volatile State
    Role                  role_         = Role::FOLLOWER;
    uint64_t              leader_id_    = 0;
    uint64_t              commit_index_ = 0;
    uint64_t              last_applied_ = 0;

    // Leader Progress Tracking
    struct PeerTracker {
        uint64_t node_id       = 0;
        uint64_t next_index    = 1;
        uint64_t match_index   = 0;
        bool     recent_active = false;
    };
    std::vector<PeerTracker> peer_trackers_{};

    // Intrusive queue of client fibers waiting for their entry to commit
    ProposeWaitNode* propose_waiters_head_ = nullptr;
    ProposeWaitNode* propose_waiters_tail_ = nullptr;

public:
    RaftCore(tinyio::IO& io, hybridlog::HybridLog& log, uint64_t self_id, std::vector<uint64_t> voters) noexcept
        : io_(io), log_(log), node_id_(self_id), peers_(std::move(voters)) {}

    [[nodiscard]] Role     CurrentRole() const noexcept { return role_; }
    [[nodiscard]] uint64_t CurrentTerm() const noexcept { return current_term_; }
    [[nodiscard]] uint64_t CommitIndex() const noexcept { return commit_index_; }
    [[nodiscard]] uint64_t LastLogIndex() const noexcept { return offset_index_.LastIndex(); }

    [[nodiscard]] uint64_t TermAt(uint64_t raft_index) const noexcept {
        if (raft_index == 0) {
            return 0;
        }
        const auto addr = offset_index_.Lookup(raft_index);
        if (addr == hybridlog::INVALID_ADDRESS) {
            return 0;
        }
        if (auto view = log_.TryReadInMemory(addr); view.has_value()) {
            return view->user_meta; // Fast O(1) RAM lookup in HybridLog Page Ring!
        }
        return 0;
    }

    // ------------------------------------------------------------------------
    // 1. Transition to Leader & Append Mandatory No-Op Entry
    // ------------------------------------------------------------------------
    tinyio::Result<void> BecomeLeader() noexcept {
        role_      = Role::LEADER;
        leader_id_ = node_id_;

        const uint64_t last_idx = LastLogIndex();
        peer_trackers_.clear();
        for (uint64_t peer : peers_) {
            peer_trackers_.push_back(PeerTracker{
                .node_id       = peer,
                .next_index    = last_idx + 1,
                .match_index   = (peer == node_id_) ? last_idx : 0,
                .recent_active = (peer == node_id_)
            });
        }

        // CRITICAL (Figure 8 Fix): Immediately append a blank No-Op entry in
        // `current_term_` so we can commit uncommitted entries from prior terms
        // and serve linearizable ReadIndex requests!
        const uint64_t noop_idx = last_idx + 1;
        TRY(addr, log_.Append(
            noop_idx,
            std::span<const std::byte>{},
            hybridlog::HybridRecordFlags::RAFT_ENTRY,
            static_cast<uint16_t>(current_term_)
        ));
        offset_index_.Append(noop_idx, addr);
        return {};
    }

    // ------------------------------------------------------------------------
    // 2. Leader Client Proposal (Zero-Lock Group Commit + Replication)
    // ------------------------------------------------------------------------
    [[nodiscard]] tinyio::Result<uint64_t> ProposeAndWait(
        std::span<const std::byte> command_payload
    ) noexcept {
        if (role_ != Role::LEADER) {
            return tinyio::Error::FailErrno(EPERM, "raft_not_leader");
        }

        const uint64_t assigned_idx  = LastLogIndex() + 1;
        const uint64_t proposal_term = current_term_;

        // Stage directly into the HybridLog RAM Page Ring (0 syscalls!)
        TRY(addr, log_.Append(
            assigned_idx,
            command_payload,
            hybridlog::HybridRecordFlags::RAFT_ENTRY,
            static_cast<uint16_t>(proposal_term)
        ));
        offset_index_.Append(assigned_idx, addr);

        // Allocate wait node on this fiber's stationary stack
        ProposeWaitNode waiter(io_, assigned_idx, proposal_term);
        if (!propose_waiters_tail_) {
            propose_waiters_head_ = propose_waiters_tail_ = &waiter;
        } else {
            propose_waiters_tail_->next = &waiter;
            propose_waiters_tail_       = &waiter;
        }

        // Flush local HybridLog in parallel with peer replication; once our
        // local flush finishes, update our own `match_index` and check quorum!
        TRY(log_.CommitAndWait(log_.TailAddress()));
        OnPeerMatchIndexUpdated(node_id_, assigned_idx);

        // Wait on stack Baton until a majority of nodes acknowledge `assigned_idx`
        (void)waiter.baton.Wait();
        if (!waiter.committed) {
            return tinyio::Error::FailErrno(ECANCELED, "raft_leadership_lost_before_commit");
        }
        return assigned_idx;
    }

    // ------------------------------------------------------------------------
    // 3. Quorum Median Calculation & Figure 8 Safe Commit Advancement
    // ------------------------------------------------------------------------
    void OnPeerMatchIndexUpdated(uint64_t peer_id, uint64_t new_match_idx) noexcept {
        if (role_ != Role::LEADER) {
            return;
        }

        for (auto& pt : peer_trackers_) {
            if (pt.node_id == peer_id) {
                pt.match_index   = std::max(pt.match_index, new_match_idx);
                pt.next_index    = std::max(pt.next_index, pt.match_index + 1);
                pt.recent_active = true;
                break;
            }
        }

        // Sort match_index across all voters to find the Quorum Median
        std::vector<uint64_t> sorted_matches;
        sorted_matches.reserve(peer_trackers_.size());
        for (const auto& pt : peer_trackers_) {
            sorted_matches.push_back(pt.match_index);
        }
        std::sort(sorted_matches.begin(), sorted_matches.end(), std::greater<uint64_t>{});

        const size_t   quorum_idx       = sorted_matches.size() / 2;
        const uint64_t quorum_match_idx = sorted_matches[quorum_idx];

        // FIGURE 8 RULE: Only advance `commit_index_` if the entry at
        // `quorum_match_idx` belongs to `current_term_`!
        if (quorum_match_idx > commit_index_ && TermAt(quorum_match_idx) == current_term_) {
            commit_index_ = quorum_match_idx;
            WakeCommittedWaiters();
        }
    }

private:
    void WakeCommittedWaiters() noexcept {
        while (propose_waiters_head_ && propose_waiters_head_->target_index <= commit_index_) {
            ProposeWaitNode* node = propose_waiters_head_;
            propose_waiters_head_ = node->next;
            if (!propose_waiters_head_) {
                propose_waiters_tail_ = nullptr;
            }
            node->next      = nullptr;
            node->committed = (TermAt(node->target_index) == node->expected_term);
            node->baton.Post();
        }
    }
};

} // namespace storage::raft
```

---

## 11. The Ultimate 10-Point Verification Checklist (Before You Ship)

When you write your deterministic simulation test suite for Raft on `tinyio`, test these exact 10 scenarios. If your implementation passes all 10, you have built a production-grade consensus engine:

1. **The Partitioned PreVote Test:** Partition Node `C` alone for 50 election timeouts, then heal the network. Verify that `currentTerm` on Leader `A` **never increments** and `A` never steps down.
2. **The Figure 8 Scenario:** Deterministically script the 5-node sequence from Section 6.B. Verify that `(Index 2, Term 2)` is **not** marked committed in Term 4 until Term 4's own No-Op entry reaches 3 nodes.
3. **The Stale Leader Read Test (`ReadIndex` / `CheckQuorum`):** Partition Leader `A` (2 nodes) from `{B, C, D}` (3 nodes). Elect `B` in `Term + 1` and write `X = 2`. Send a `ReadIndex` read of `X` to stale Leader `A` before `A` hears about `Term + 1`. Verify `A` never returns `X = 1`—it blocks waiting for heartbeat quorum, times out via `CheckQuorum`, and steps down.
4. **The Fast Backtracking Test:** Partition Follower `C`, append 10,000 uncommitted entries on Leader `A`, partition `A`, elect `B` in a new term, and reconnect `C` to `B`. Verify `B` resolves `C`'s 10,000-entry divergence in **2 `AppendEntries` round-trips** (using `conflict_term` / `conflict_index`), not 10,000.
5. **The Uncommitted Tail Truncation Test:** Verify that when Follower `C` truncates those 10,000 uncommitted entries, both `OffsetIndex::TruncateFrom()` and `HybridLog`'s `tail_address_` roll back cleanly and subsequent appends overwrite the exact 64-bit virtual address space.
6. **The Reordered / Duplicate `AppendEntries` RPC Test:** Have your simulated network deliver an older, shorter `AppendEntries` RPC from the *same* term *after* a newer, longer `AppendEntries` RPC has already been appended. Verify that the follower **does not truncate** the newer entries that already match! *(Classic bug: only truncate at `i` if an existing entry at `i` has a **different** term than the incoming entry!)*
7. **The Single-Byte Crash Before Vote Reply Test:** Crash a node immediately before and after `CommitAndWait()` in `RequestVote`. Verify a restarted node never votes for two different candidates in the same term.
8. **The Zero-Disk-Read Follower Catch-Up Test:** Let a follower lag by 500 entries (still inside the 4-page `HybridLog` RAM ring). Verify that the leader catches the follower up via `HybridLog::ScanInMemory()` with **0 `io_.ReadFixed()` calls**.
9. **The Cold Follower Catch-Up Test:** Let a follower lag by 5,000 entries (evicted past `HeadAddress`, still `>= BeginAddress`). Verify the leader seamlessly switches to `HybridLog::ReadRecord()` (`ReadFixed`) until the follower's `next_index` crosses `HeadAddress` back into RAM.
10. **The Single-Server Membership Change Overlap Test:** Simultaneously propose adding Node `D` and removing Node `A`. Verify the second config change is rejected until the first config change is committed.
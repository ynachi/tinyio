#![forbid(unsafe_code)]
//! The timeout wheel: arm and disarm are O(1) list operations, with no SQE and
//! no kernel timer; the kernel is involved only when a timeout fires. Port of
//! the C++ `detail/timer_wheel.hpp`, over a slab instead of fiber pointers.
//!
//! Entries live in a slab and each slot is a doubly linked list of slab
//! indices, so unlinking needs no search. The wheel stores an opaque key and
//! never dereferences it. A `TimerId` carries a generation: using one after its
//! entry expired or was disarmed aborts, always.
//!
//! Slots are 64 ms wide, so a timeout can fire up to one slot late. Deadlines
//! past the horizon (512 slots, ~32.7 s) alias into their slot; `expire`
//! checks the true deadline, so an alias costs one early no-op visit.

use crate::check::{check, fatal};

pub(crate) const TICK_NS: u64 = 64 * 1_000_000;
const SLOTS: usize = 512;
const WORDS: usize = SLOTS / 64;
const NONE: u32 = u32::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct TimerId {
    index: u32,
    generation: u32,
}

struct Entry<K> {
    key: K,
    deadline: u64,
    generation: u32,
    prev: u32,
    next: u32,
    live: bool,
}

pub(crate) struct Wheel<K: Copy> {
    entries: Vec<Entry<K>>,
    /// Free slab indices, linked through `Entry::next`.
    free: u32,
    heads: [u32; SLOTS],
    occupied: [u64; WORDS],
    last_tick: u64,
    armed: usize,
}

fn slot_of(deadline_ns: u64) -> usize {
    ((deadline_ns / TICK_NS) as usize) & (SLOTS - 1)
}

impl<K: Copy> Wheel<K> {
    pub(crate) fn new() -> Wheel<K> {
        Wheel {
            entries: Vec::new(),
            free: NONE,
            heads: [NONE; SLOTS],
            occupied: [0; WORDS],
            last_tick: 0,
            armed: 0,
        }
    }

    pub(crate) fn has_armed(&self) -> bool {
        self.armed != 0
    }

    pub(crate) fn arm(&mut self, key: K, deadline_ns: u64) -> TimerId {
        let slot = slot_of(deadline_ns);
        let index = if self.free == NONE {
            self.entries.push(Entry {
                key,
                deadline: deadline_ns,
                generation: 0,
                prev: NONE,
                next: NONE,
                live: true,
            });
            (self.entries.len() - 1) as u32
        } else {
            let i = self.free;
            let e = &mut self.entries[i as usize];
            self.free = e.next;
            e.key = key;
            e.deadline = deadline_ns;
            e.live = true;
            i
        };
        let head = self.heads[slot];
        let e = &mut self.entries[index as usize];
        e.prev = NONE;
        e.next = head;
        if head != NONE {
            self.entries[head as usize].prev = index;
        }
        self.heads[slot] = index;
        self.occupied[slot >> 6] |= 1 << (slot & 63);
        self.armed += 1;
        TimerId {
            index,
            generation: self.entries[index as usize].generation,
        }
    }

    pub(crate) fn disarm(&mut self, id: TimerId) {
        let live = self
            .entries
            .get(id.index as usize)
            .is_some_and(|e| e.live && e.generation == id.generation);
        check!(
            live,
            "stale TimerId {id:?}: disarmed after it expired or was disarmed"
        );
        self.remove(id.index);
    }

    /// Unlink, free the slab entry (bumping its generation), and update counts.
    fn remove(&mut self, index: u32) {
        let (prev, next, slot) = {
            let e = &self.entries[index as usize];
            (e.prev, e.next, slot_of(e.deadline))
        };
        if prev == NONE {
            self.heads[slot] = next;
        } else {
            self.entries[prev as usize].next = next;
        }
        if next != NONE {
            self.entries[next as usize].prev = prev;
        }
        if self.heads[slot] == NONE {
            self.occupied[slot >> 6] &= !(1 << (slot & 63));
        }
        let e = &mut self.entries[index as usize];
        e.live = false;
        e.generation = e.generation.wrapping_add(1);
        e.next = self.free;
        self.free = index;
        self.armed -= 1;
    }

    /// Remove every entry whose deadline has passed and append its key to
    /// `out`. Same-slot neighbours and horizon aliases still in the future stay
    /// armed. Keys are returned, not called back, so the caller may re-arm.
    pub(crate) fn expire(&mut self, now_ns: u64, out: &mut Vec<K>) {
        let cur = now_ns / TICK_NS;
        if self.armed == 0 {
            self.last_tick = cur;
            return;
        }
        // Lagging more than a full wrap: visiting each slot once covers everything.
        let mut from = self.last_tick;
        if cur - from >= SLOTS as u64 {
            from = cur - (SLOTS as u64 - 1);
        }
        for t in from..=cur {
            let slot = (t as usize) & (SLOTS - 1);
            if self.occupied[slot >> 6] & (1 << (slot & 63)) == 0 {
                continue;
            }
            let mut i = self.heads[slot];
            while i != NONE {
                let (next, due, key) = {
                    let e = &self.entries[i as usize];
                    (e.next, e.deadline <= now_ns, e.key)
                };
                if due {
                    self.remove(i);
                    out.push(key);
                }
                i = next;
            }
        }
        self.last_tick = cur;
    }

    /// Nanoseconds until the next candidate wake-up (the end of the nearest
    /// occupied slot), or `None` when nothing is armed. A horizon alias can make
    /// this early; `expire` then finds nothing due, which is harmless.
    pub(crate) fn ns_until_next(&self, now_ns: u64) -> Option<u64> {
        if self.armed == 0 {
            return None;
        }
        let cur = now_ns / TICK_NS;
        let start = (cur as usize) & (SLOTS - 1);
        let (start_word, start_bit) = (start >> 6, start & 63);
        let wake_at = |off: u64| {
            // The END of the slot, so every deadline inside it has passed.
            let target = (cur + off + 1) * TICK_NS;
            target.saturating_sub(now_ns)
        };
        // 1. The rest of the starting word.
        let mask = self.occupied[start_word] >> start_bit;
        if mask != 0 {
            return Some(wake_at(u64::from(mask.trailing_zeros())));
        }
        // 2. The following words.
        for w in 1..WORDS {
            let word = self.occupied[(start_word + w) % WORDS];
            if word != 0 {
                let off = (w * 64 - start_bit) as u64 + u64::from(word.trailing_zeros());
                return Some(wake_at(off));
            }
        }
        // 3. The wrapped low bits of the starting word.
        let word = self.occupied[start_word];
        if word != 0 {
            let off = (SLOTS - start_bit) as u64 + u64::from(word.trailing_zeros());
            return Some(wake_at(off));
        }
        fatal(format_args!(
            "timer wheel counts {} armed but no slot is occupied",
            self.armed
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::testutil::death_test;

    const MS: u64 = 1_000_000;

    fn due(w: &mut Wheel<u32>, now: u64) -> Vec<u32> {
        let mut out = Vec::new();
        w.expire(now, &mut out);
        out.sort_unstable();
        out
    }

    // Prediction: arming sets has_armed and disarming clears it.
    #[test]
    fn arm_then_disarm() {
        let mut w = Wheel::new();
        assert!(!w.has_armed());
        let id = w.arm(1u32, 500 * MS);
        assert!(w.has_armed());
        w.disarm(id);
        assert!(!w.has_armed());
        assert_eq!(due(&mut w, 10_000 * MS), []);
    }

    // Prediction: only entries whose deadline has passed come out.
    #[test]
    fn expire_returns_only_the_due() {
        let mut w = Wheel::new();
        for (key, ms) in [(1u32, 100), (2, 200), (3, 300)] {
            w.arm(key, ms * MS);
        }
        assert_eq!(due(&mut w, 250 * MS), [1, 2]);
        assert!(w.has_armed());
        assert_eq!(due(&mut w, 300 * MS), [3]);
        assert!(!w.has_armed());
    }

    // Prediction: two entries in one 64 ms slot expire separately, by true deadline.
    #[test]
    fn same_slot_neighbours_use_their_own_deadlines() {
        let mut w = Wheel::new();
        w.arm(1u32, 70 * MS);
        w.arm(2, 120 * MS); // same slot as 70 ms (64..128)
        assert_eq!(due(&mut w, 100 * MS), [1]);
        assert_eq!(due(&mut w, 130 * MS), [2]);
    }

    // Prediction: a deadline past the horizon aliases into an early slot but is
    // not returned until its true time.
    #[test]
    fn horizon_alias_waits_for_its_real_deadline() {
        let mut w = Wheel::new();
        let horizon = TICK_NS * SLOTS as u64;
        w.arm(1u32, 10 * MS + horizon);
        assert_eq!(due(&mut w, 20 * MS), []);
        assert_eq!(due(&mut w, horizon), []);
        assert_eq!(due(&mut w, 10 * MS + horizon), [1]);
    }

    // Prediction: after lagging several wraps, one expire still finds everything due.
    #[test]
    fn a_long_lag_still_expires_everything() {
        let mut w = Wheel::new();
        for key in 0..50u32 {
            w.arm(key, u64::from(key) * 700 * MS);
        }
        let got = due(&mut w, TICK_NS * SLOTS as u64 * 5);
        assert_eq!(got.len(), 50);
    }

    // Prediction: the wake-up bound is the end of the nearest occupied slot: never
    // before the deadline, never more than one slot after it.
    #[test]
    fn next_wake_bounds_the_deadline() {
        let mut w = Wheel::new();
        assert_eq!(w.ns_until_next(0), None);
        for ms in [5, 200, 3_000, 31_000] {
            let mut w = Wheel::new();
            w.arm(1u32, ms * MS);
            let wake = w.ns_until_next(0).unwrap();
            assert!(
                wake >= ms * MS && wake <= ms * MS + TICK_NS,
                "{ms} ms -> {wake}"
            );
        }
        // Armed and queried at the same `now` (the loop caches one per tick): the
        // entry's slot is the cursor's slot, so the wait is at most one slot.
        w.arm(1u32, 300 * MS);
        assert!(w.ns_until_next(300 * MS).unwrap() <= TICK_NS);
    }

    // Prediction: a slab slot freed and reused changes its generation, so the old id is dead.
    #[test]
    fn reuse_bumps_the_generation() {
        let mut w = Wheel::new();
        let a = w.arm(1u32, 100 * MS);
        w.disarm(a);
        let b = w.arm(2, 100 * MS);
        assert_eq!(a.index, b.index);
        assert_ne!(a.generation, b.generation);
        w.disarm(b);
    }

    // Prediction: 20k random arm / disarm / expire steps agree with a naive model.
    #[test]
    fn matches_a_naive_model() {
        let mut w = Wheel::new();
        let mut model: Vec<(u32, u64, TimerId)> = Vec::new();
        let (mut seed, mut now, mut next_key) = (0x9e37_79b9_7f4a_7c15u64, 0u64, 0u32);
        let mut rand = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let steps = if cfg!(miri) { 300 } else { 20_000 };
        for _ in 0..steps {
            match rand() % 4 {
                0 | 1 => {
                    let deadline = now + rand() % (40_000 * MS);
                    let id = w.arm(next_key, deadline);
                    model.push((next_key, deadline, id));
                    next_key += 1;
                }
                2 if !model.is_empty() => {
                    let (_, _, id) = model.swap_remove((rand() as usize) % model.len());
                    w.disarm(id);
                }
                _ => {
                    now += rand() % (300 * MS);
                    let got = due(&mut w, now);
                    let mut want: Vec<u32> =
                        model.iter().filter(|m| m.1 <= now).map(|m| m.0).collect();
                    want.sort_unstable();
                    assert_eq!(got, want, "at {now}");
                    model.retain(|m| m.1 > now);
                }
            }
            assert_eq!(w.has_armed(), !model.is_empty());
        }
    }

    // Prediction: using an id after its entry expired aborts.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn death_stale_id() {
        death_test(
            "timer::tests::death_stale_id",
            || {
                let mut w = Wheel::new();
                let id = w.arm(1u32, 10 * MS);
                due(&mut w, 100 * MS);
                w.disarm(id);
            },
            &["stale TimerId"],
        );
    }
}

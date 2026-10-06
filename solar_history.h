#pragma once

#include <cstdint>
#include <ctime>

// 6 hours of solar power history as a ring of 5-minute buckets, anchored to
// wall-clock time: bucket boundaries land on :00, :05, :10 past the hour
// regardless of when the device booted, so live samples and backfilled
// history always agree on which slot a given moment belongs to.
//
// Raw C-style fixed arrays hit a known limitation as ESPHome globals - they
// decay to pointers when passed into the component's constructor, breaking
// the copy - so this sidesteps the globals component entirely. It is a plain
// C++ global, defined once and included into the single generated main.cpp,
// referenced directly rather than via id(). All methods are defined in-class
// and so are implicitly inline, which keeps that safe if the header is ever
// pulled into a second translation unit.
//
// TWO CURSORS, deliberately separate:
//
//   live_bucket_  - the bucket the most recent accepted reading landed in.
//                   Owns open_count_, and only add_reading moves it. It is
//                   monotonic: a late reading for an older bucket is still
//                   recorded, but does not drag the cursor backwards.
//   clock_bucket_ - the bucket the ESP's own clock is in. Only advance_to
//                   moves it, and it clears ahead: any bucket the clock
//                   enters with no data yet is stamped as a gap, so a slot
//                   never redisplays the reading it held 6 hours ago.
//
// Those two writers commute. advance_to never writes at or below
// live_bucket_, and add_reading never writes above the clock because a
// reading's own timestamp cannot exceed now. So it does not matter whether a
// delayed on_time fire is serviced before or after an on_response that was
// blocked across a bucket boundary - both orderings reach the same state,
// and no sequencing guarantee is needed between them.
class SolarHistory {
 public:
  static constexpr int SLOTS = 72;              // 72 x 5min = 6 hours
  static constexpr uint32_t BUCKET_S = 300;
  static constexpr float GAP = -1.0f;           // "no data for this bucket"

  static bool is_gap(float v) { return v < 0.0f; }

  // Truncates an epoch to the 5-minute boundary it falls in. Every writer
  // and reader goes through this, so the wall-clock-to-slot mapping cannot
  // drift out of sync between them.
  static uint32_t bucket_of(time_t epoch) {
    return (uint32_t) ((epoch / (time_t) BUCKET_S) * (time_t) BUCKET_S);
  }

  static int slot_of(uint32_t bucket) {
    return (int) ((bucket / BUCKET_S) % (uint32_t) SLOTS);
  }

  SolarHistory() {
    for (int i = 0; i < SLOTS; i++) this->data_[i] = GAP;
  }

  // --- Writers -----------------------------------------------------------

  // Clock-driven. Call from the time component's 5-minute on_time trigger,
  // ungated by any polling switch: buckets the clock advances into must be
  // marked whether or not the network is up, since an outage is exactly when
  // an unmarked slot would otherwise show 6-hour-old data as current.
  //
  // Does not touch open_count_. That count belongs to live_bucket_, and
  // clearing it from here is the coupling this design exists to remove: a
  // reading for a bucket the clock has already left still extends that
  // bucket's average correctly.
  void advance_to(time_t now) {
    uint32_t c = bucket_of(now);

    // First valid call after boot or SNTP sync: adopt the current bucket
    // without marking anything, or every slot would be stamped as a gap.
    if (this->clock_bucket_ == 0) {
      this->clock_bucket_ = c;
      return;
    }
    // Backwards, or same bucket. A small backwards correction from an SNTP
    // resync should not unwind anything.
    if (c <= this->clock_bucket_) return;

    uint32_t from = this->clock_bucket_ + BUCKET_S;
    // A gap longer than the ring is fully covered by one pass over it.
    if (c - from >= (uint32_t) SLOTS * BUCKET_S)
      from = c - (uint32_t) (SLOTS - 1) * BUCKET_S;

    for (uint32_t b = from; b <= c; b += BUCKET_S) {
      // Skip anything a reading has already claimed. This is what lets the
      // two writers run in either order.
      if (b > this->live_bucket_) this->data_[slot_of(b)] = GAP;
    }
    this->clock_bucket_ = c;
  }

  // Live sample. value_epoch is the READING's own last-updated time
  // (now - data_age), not now: if GivTCP loses contact with the inverter the
  // same value repeats across many polls while data_age grows, and attributing
  // it to whatever bucket we happened to poll in would fabricate a
  // continuously-updating trace for a period where nothing changed.
  //
  // Returns false if this reading was already counted. Note the dedupe is an
  // exact epoch match, so backfill cannot usefully pre-seed it: that would
  // need the last history entry's real last_changed, not a bucket boundary.
  // One duplicated sample inside a bucket that receives ~10 is absorbed by
  // the average, so backfill does not try.
  bool add_reading(time_t now, time_t value_epoch, float watts) {
    // A reading cannot be from the future. Clamping keeps live_bucket_ from
    // running ahead of the clock on a bad data_age, which would make
    // advance_to skip real buckets until the clock caught up.
    if (value_epoch > now) value_epoch = now;

    uint32_t e = (uint32_t) value_epoch;
    if (e == this->last_reading_epoch_) return false;  // same reading as last poll
    this->last_reading_epoch_ = e;

    uint32_t b = bucket_of((time_t) e);
    int i = slot_of(b);

    if (b > this->live_bucket_) {
      // A newer bucket than we were accumulating into. Whatever the slot
      // held is either a gap advance_to stamped, or data from 6h ago.
      this->data_[i] = watts;
      this->open_count_ = 1;
      this->live_bucket_ = b;
    } else if (b == this->live_bucket_) {
      // Still accumulating. The gap check is not dead code even though
      // advance_to cannot clear the live bucket: backfill can, and a backfill
      // run interleaves with live polling.
      if (is_gap(this->data_[i]) || this->open_count_ == 0) {
        this->data_[i] = watts;
        this->open_count_ = 1;
      } else {
        this->data_[i] =
            (this->data_[i] * this->open_count_ + watts) / (this->open_count_ + 1);
        this->open_count_++;
      }
    } else {
      // Late reading for a bucket already closed. Record it in its own slot,
      // turning a gap into real data, but leave the cursors alone - they
      // describe the current bucket, not this older one.
      this->data_[i] = watts;
    }
    return true;
  }

  // Backfill. The caller's only job is to decide which buckets to ask HA
  // for and to report what came back; everything about ring slots, whether
  // the bucket is still representable, and whether it happens to be the
  // still-open bucket is decided here.
  //
  // samples <= 0 means the history window came back empty. Backfill is a
  // full graph refresh, so that must be stored as a gap rather than leaving
  // stale data from a previous run or from 6h ago via wraparound.
  //
  // Returns false if the write was ignored.
  bool store_bucket(uint32_t bucket, float avg, int samples) {
    // Too far back to represent: this bucket's slot now belongs to a newer
    // time, so writing it would clobber current data with old history. This
    // is the guard that lets backfill be ignorant of the clock - a run that
    // is superseded, or whose anchor is invalidated by an SNTP correction
    // mid-run, simply has its late writes dropped.
    if (!this->represents(bucket)) return false;

    bool gap = samples <= 0;

    // An empty HA window must not erase a live reading we already hold for
    // the bucket currently accumulating.
    if (gap && bucket == this->live_bucket_ && this->open_count_ > 0) return false;

    this->data_[slot_of(bucket)] = gap ? GAP : avg;

    // If this is the newest bucket written, it becomes the one live sampling
    // accumulates into, carrying backfill's own sample count so the next
    // poll extends the average via the weighted update rather than
    // restarting it. No step index needed - being newest is the condition.
    if (!gap && bucket >= this->live_bucket_) {
      this->live_bucket_ = bucket;
      this->open_count_ = samples;
    }
    return true;
  }

  // Oldest bucket the ring can still represent. Anything older maps into a
  // slot that now stands for a newer time.
  uint32_t oldest_bucket() const {
    return this->clock_bucket_ - (uint32_t) (SLOTS - 1) * BUCKET_S;
  }

  bool represents(uint32_t bucket) const {
    if (this->clock_bucket_ == 0) return false;  // clock not seeded yet
    return bucket <= this->clock_bucket_ && bucket >= this->oldest_bucket();
  }

  // --- Readers -----------------------------------------------------------

  // A view over the most recent `points` buckets, ending at the bucket `now`
  // falls in. Resolved once so a draw loop is not recomputing the walk-back
  // for every sample. Holds no copy of the data.
  struct Window {
    // Default-constructible so a view model can hold one before the first
    // rebuild. A null history reads as entirely empty rather than crashing.
    const SolarHistory *h{nullptr};
    int oldest{0};
    int points{0};

    // false means this bucket has no data - draw nothing rather than a fake
    // dip down to the sentinel.
    bool sample(int i, float &out) const {
      if (this->h == nullptr) return false;
      float v = this->h->data_[(this->oldest + i) % SLOTS];
      if (is_gap(v)) return false;
      out = v;
      return true;
    }

    int valid() const {
      if (this->h == nullptr) return 0;
      int n = 0;
      for (int i = 0; i < this->points; i++)
        if (!is_gap(this->h->data_[(this->oldest + i) % SLOTS])) n++;
      return n;
    }

    // True peak across the window, for the label. Kept separate from any
    // chart scaling floor: the floor stops a near-flat low-solar window
    // looking like noise, but the label should show the genuine reading.
    float peak() const {
      if (this->h == nullptr) return 0.0f;
      float m = 0.0f;
      for (int i = 0; i < this->points; i++) {
        float v = this->h->data_[(this->oldest + i) % SLOTS];
        if (!is_gap(v) && v > m) m = v;
      }
      return m;
    }
  };

  Window window(time_t now, int points) const {
    if (points > SLOTS) points = SLOTS;
    int current = slot_of(bucket_of(now));
    int oldest = ((current - (points - 1)) % SLOTS + SLOTS) % SLOTS;
    return Window{this, oldest, points};
  }

  uint32_t live_bucket() const { return this->live_bucket_; }
  uint32_t clock_bucket() const { return this->clock_bucket_; }

 private:
  float data_[SLOTS];
  uint32_t clock_bucket_{0};       // advance_to only
  uint32_t live_bucket_{0};        // add_reading / store_bucket only
  uint32_t last_reading_epoch_{0}; // dedupe guard
  int open_count_{0};              // samples in live_bucket_
};

SolarHistory solar_history_global;

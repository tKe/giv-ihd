#pragma once

#include <string>

#include <ArduinoJson.h>

#include "esphome/core/component.h"
#include "esphome/components/http_request/http_request.h"

namespace esphome {
namespace ha_history {

// Fetches a window of Home Assistant recorder history for one entity and
// folds it directly into caller-owned buckets, without ever holding more
// than one JSON record in memory.
//
// Why not http_request.get + capture_response + deserializeJson(body): that
// path buffers the ENTIRE response (a full 6h window is commonly tens of KB)
// as both a std::string and then a second time as an ArduinoJson DOM, which
// is exactly the kind of allocation that fragments an ESP8266's ~30-40KB
// heap. Here, the outer '[[ ... ]]' framing is walked byte-by-byte as the
// response streams in (see fetch_period()'s bracket/brace depth counter),
// and only ONE small record object - e.g. {"state":"1234.5",
// "last_changed":"2024-01-01T12:00:00+00:00"} - is ever materialized as an
// ArduinoJson document at a time. That per-record document is reused
// (.clear() between records) the same way the old backfill_doc was reused
// across steps, so parsing 700+ records allocates no more than parsing one.
class HAHistoryComponent : public Component {
 public:
  void set_http_request(http_request::HttpRequestComponent *http_request) { this->http_request_ = http_request; }
  void set_base_url(const std::string &url) { this->base_url_ = url; }
  void set_token(const std::string &token) { this->token_ = token; }
  void set_timeout(uint32_t timeout_ms) { this->timeout_ms_ = timeout_ms; }

  /** Fetch /api/history/period for entity_id over [start, end) and fold every
   * record into `num_buckets` buckets of `bucket_s` seconds, anchored at
   * `start` (bucket i covers [start + i*bucket_s, start + (i+1)*bucket_s)).
   *
   * out_sum[i] / out_count[i] receive the SUM and COUNT of samples landing in
   * bucket i (both zeroed by this call before fetching) - the caller divides
   * to get an average, and treats out_count[i] == 0 as "no data" exactly like
   * the per-step backfill used to treat samples <= 0. Buffers must each hold
   * at least num_buckets elements.
   *
   * Returns false only on a transport-level failure (couldn't connect, bad
   * HTTP status, or the read stalled past `timeout_ms`). A successful fetch
   * that legitimately found no history in some buckets still returns true -
   * those buckets are just left at count 0, same as the old cnt<=0 case.
   */
  bool fetch_period(const std::string &entity_id, time_t start, time_t end, uint32_t bucket_s, float *out_sum,
                     int *out_count, int num_buckets);

 protected:
  // Parses one complete record's JSON text and, if it carries a numeric
  // state and a recognisable timestamp, adds it to the matching bucket.
  // Silently ignores anything it can't make sense of (non-numeric states
  // like "unavailable", missing fields, a timestamp outside [start, end)) -
  // backfill is a best-effort refresh, not a strict contract with HA's API.
  void accumulate_record_(const char *json, size_t len, time_t start, uint32_t bucket_s, float *out_sum,
                          int *out_count, int num_buckets);

  http_request::HttpRequestComponent *http_request_{nullptr};
  std::string base_url_;
  std::string token_;
  uint32_t timeout_ms_{5000};

  // Reused across every record so 700+ small parses cost one allocation,
  // not 700 - the same reasoning that motivated the old backfill_doc.
  JsonDocument record_doc_;
};

}  // namespace ha_history
}  // namespace esphome

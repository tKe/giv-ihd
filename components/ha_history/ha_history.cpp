#include "ha_history.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "esphome/core/log.h"
#include "esphome/core/time.h"

namespace esphome {
namespace ha_history {

static const char *const TAG = "ha_history";

// HA's history API always reports last_changed in UTC (a trailing "+00:00"
// or "Z"), regardless of HA's configured local timezone - the rest of this
// project already leans on that (see givenergy_dashboard.yaml's time:
// block), so the offset/fractional-second suffix is simply ignored here
// rather than parsed. ESPTime::strptime() can't be reused for this: it only
// accepts a space-separated "YYYY-MM-DD HH:MM:SS" and rejects anything
// trailing the seconds field, neither of which matches ISO8601's 'T'
// separator and fractional/offset suffix. This reuses ESPTime's own
// calendar math (recalc_timestamp_utc) rather than duplicating it or
// depending on libc timegm(), which isn't reliably available on ESP8266
// Arduino.
static bool parse_iso8601_utc(const char *s, time_t &out) {
  if (s == nullptr || strlen(s) < 19)
    return false;
  if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':')
    return false;
  for (int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18}) {
    if (s[i] < '0' || s[i] > '9')
      return false;
  }
  auto digit2 = [](const char *p) -> uint8_t { return (p[0] - '0') * 10 + (p[1] - '0'); };
  auto digit4 = [](const char *p) -> uint16_t {
    return (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
  };

  ESPTime t{};
  t.year = digit4(s);
  t.month = digit2(s + 5);
  t.day_of_month = digit2(s + 8);
  t.hour = digit2(s + 11);
  t.minute = digit2(s + 14);
  t.second = digit2(s + 17);
  t.recalc_timestamp_utc(false);
  if (t.timestamp < 0)
    return false;
  out = t.timestamp;
  return true;
}

void HAHistoryComponent::accumulate_record_(const char *json, size_t len, time_t start, uint32_t bucket_s,
                                            float *out_sum, int *out_count, int num_buckets) {
  this->record_doc_.clear();
  DeserializationError err = deserializeJson(this->record_doc_, json, len);
  if (err) {
    ESP_LOGV(TAG, "record parse failed: %s", err.c_str());
    return;
  }

  // Full records (first row per entity) carry "state"/"last_changed";
  // minimal_response abbreviates continuation rows to "s"/"lu" (lu is a
  // Unix epoch, possibly fractional). Both shapes are handled since either
  // can appear within the same response.
  const char *state_str = nullptr;
  if (this->record_doc_["state"].is<const char *>())
    state_str = this->record_doc_["state"].as<const char *>();
  else if (this->record_doc_["s"].is<const char *>())
    state_str = this->record_doc_["s"].as<const char *>();
  if (state_str == nullptr)
    return;

  char *num_end;
  float value = strtof(state_str, &num_end);
  if (num_end == state_str)
    return;  // non-numeric state, e.g. "unavailable"/"unknown"

  time_t epoch;
  if (this->record_doc_["last_changed"].is<const char *>()) {
    if (!parse_iso8601_utc(this->record_doc_["last_changed"].as<const char *>(), epoch))
      return;
  } else if (!this->record_doc_["lu"].isNull()) {
    epoch = (time_t) this->record_doc_["lu"].as<double>();
  } else {
    return;
  }

  int idx = (int) ((epoch - start) / (time_t) bucket_s);
  if (idx < 0 || idx >= num_buckets)
    return;
  out_sum[idx] += value;
  out_count[idx]++;
}

bool HAHistoryComponent::fetch_period(const std::string &entity_id, time_t start, time_t end, uint32_t bucket_s,
                                      float *out_sum, int *out_count, int num_buckets) {
  for (int i = 0; i < num_buckets; i++) {
    out_sum[i] = 0.0f;
    out_count[i] = 0;
  }

  char start_buf[24], end_buf[24];
  ESPTime::from_epoch_utc(start).strftime(start_buf, sizeof(start_buf), "%Y-%m-%dT%H:%M:%SZ");
  ESPTime::from_epoch_utc(end).strftime(end_buf, sizeof(end_buf), "%Y-%m-%dT%H:%M:%SZ");

  char url_buf[256];
  snprintf(url_buf, sizeof(url_buf),
           "%s/api/history/period/%s?filter_entity_id=%s&end_time=%s&minimal_response&no_attributes",
           this->base_url_.c_str(), start_buf, entity_id.c_str(), end_buf);

  std::vector<http_request::Header> headers = {{"Authorization", this->token_}};
  auto container = this->http_request_->get(std::string(url_buf), headers);
  if (!container) {
    ESP_LOGW(TAG, "request failed to start");
    return false;
  }
  if (container->status_code != 200) {
    ESP_LOGW(TAG, "HTTP %d", container->status_code);
    container->end();
    return false;
  }

  // --- Streaming record framer -------------------------------------------
  // Walks raw response bytes tracking [/{ nesting depth (a single counter
  // for both, since we only care about *how deep*, not which kind). HA's
  // shape here is [[ record, record, ... ]] - one outer array of per-entity
  // results, one inner array of that entity's records - so a record's own
  // '{' brings depth to 3 and its matching '}' brings it back to 2. Nesting
  // *within* a record (there shouldn't be any with no_attributes, but this
  // doesn't assume that) is handled for free: the depth counter tracks it
  // correctly regardless, only firing on the specific 3->2 transition that
  // is this record's own close. Quote/escape tracking keeps a state value
  // or timestamp string from being misread as structural.
  int depth = 0;
  bool in_string = false, escaped = false, capturing = false;
  char record[256];
  size_t record_len = 0;
  uint8_t chunk[128];
  uint32_t last_data_time = millis();
  bool transport_ok = true;

  while (true) {
    int n = container->read(chunk, sizeof(chunk));
    auto result = http_request::http_read_loop_result(n, last_data_time, this->timeout_ms_,
                                                       container->is_read_complete());
    if (result == http_request::HttpReadLoopResult::RETRY)
      continue;
    if (result == http_request::HttpReadLoopResult::COMPLETE)
      break;
    if (result == http_request::HttpReadLoopResult::ERROR) {
      ESP_LOGW(TAG, "transport error mid-stream (%d)", n);
      transport_ok = false;
      break;
    }
    if (result == http_request::HttpReadLoopResult::TIMEOUT) {
      ESP_LOGW(TAG, "stalled waiting for data");
      transport_ok = false;
      break;
    }

    for (int bi = 0; bi < n; bi++) {
      char c = (char) chunk[bi];
      bool was_in_string = in_string;
      if (in_string) {
        if (escaped)
          escaped = false;
        else if (c == '\\')
          escaped = true;
        else if (c == '"')
          in_string = false;
      } else {
        if (c == '"')
          in_string = true;
        else if (c == '{' || c == '[')
          depth++;
        else if (c == '}' || c == ']')
          depth--;
      }

      if (!capturing) {
        if (!was_in_string && c == '{' && depth == 3) {
          capturing = true;
          record_len = 0;
        } else {
          continue;
        }
      }

      if (record_len < sizeof(record) - 1) {
        record[record_len++] = c;
      } else {
        // A record this large means the response shape isn't what we
        // expect - drop it rather than overrun the buffer.
        ESP_LOGW(TAG, "record exceeded %u bytes, dropped", (unsigned) sizeof(record));
        capturing = false;
        record_len = 0;
        continue;
      }

      if (!in_string && c == '}' && depth == 2) {
        capturing = false;
        record[record_len] = '\0';
        this->accumulate_record_(record, record_len, start, bucket_s, out_sum, out_count, num_buckets);
        record_len = 0;
      }
    }
  }

  container->end();
  return transport_ok;
}

}  // namespace ha_history
}  // namespace esphome

#ifndef CLAUDE_STATUS_CORE_H
#define CLAUDE_STATUS_CORE_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Result codes
 */
typedef enum CResultCode {
  Ok = 0,
  NoCredentials = 1,
  InvalidCredentials = 2,
  NetworkError = 3,
  ParseError = 4,
  AuthError = 5,
} CResultCode;

/**
 * Opaque handle to the Rust core state
 */
typedef struct ClaudeStatusCore ClaudeStatusCore;

/**
 * Credentials info returned to C
 */
typedef struct CCredentialsInfo {
  /**
   * Plan name ("Pro" or "Max"), null if unknown
   */
  const char *plan_name;
  /**
   * Whether credentials are valid
   */
  bool valid;
} CCredentialsInfo;

/**
 * Usage data returned to C
 */
typedef struct CUsageData {
  /**
   * 5-hour utilization percentage (0-100)
   */
  double five_hour_pct;
  /**
   * 7-day utilization percentage (0-100)
   */
  double seven_day_pct;
  /**
   * 5-hour reset time as Unix timestamp
   */
  int64_t five_hour_reset_ts;
  /**
   * 7-day reset time as Unix timestamp
   */
  int64_t seven_day_reset_ts;
  /**
   * Whether the data is valid
   */
  bool valid;
} CUsageData;

/**
 * Context window info returned to C
 */
typedef struct CContextInfo {
  /**
   * Context usage percentage (0-100)
   */
  double context_pct;
  /**
   * Number of tokens used
   */
  int64_t context_tokens;
  /**
   * Context window size
   */
  int64_t context_window_size;
  /**
   * Model name (owned by Rust, valid until next call)
   */
  const char *model_name;
  /**
   * Whether the data is valid
   */
  bool valid;
} CContextInfo;

/**
 * Create a new core instance
 *
 * # Safety
 * Returns a pointer that must be freed with `claude_status_core_free`
 */
struct ClaudeStatusCore *claude_status_core_new(void);

/**
 * Free the core instance
 *
 * # Safety
 * `core` must be a valid pointer returned by `claude_status_core_new`
 */
void claude_status_core_free(struct ClaudeStatusCore *core);

/**
 * Load credentials from the specified file
 *
 * # Safety
 * `core` must be valid, `path` must be a valid C string or null for default
 */
enum CResultCode claude_status_core_load_credentials(struct ClaudeStatusCore *core,
                                                     const char *path);

/**
 * Get credentials info
 *
 * # Safety
 * `core` must be valid
 */
struct CCredentialsInfo claude_status_core_get_credentials_info(const struct ClaudeStatusCore *core);

/**
 * Fetch usage data from the API (blocking)
 *
 * # Safety
 * `core` must be valid
 */
enum CResultCode claude_status_core_fetch_usage(struct ClaudeStatusCore *core);

/**
 * Get the last fetched usage data
 *
 * # Safety
 * `core` must be valid
 */
struct CUsageData claude_status_core_get_usage(const struct ClaudeStatusCore *core);

/**
 * Read context info from the latest transcript
 *
 * # Safety
 * `core` must be valid
 */
enum CResultCode claude_status_core_read_context(struct ClaudeStatusCore *core);

/**
 * Get the last read context info
 *
 * # Safety
 * `core` must be valid
 */
struct CContextInfo claude_status_core_get_context(const struct ClaudeStatusCore *core);

/**
 * Start monitoring the credentials file for changes
 *
 * # Safety
 * `core` must be valid, `path` must be a valid C string or null for default
 */
enum CResultCode claude_status_core_start_monitor(struct ClaudeStatusCore *core, const char *path);

/**
 * Stop monitoring the credentials file
 *
 * # Safety
 * `core` must be valid
 */
void claude_status_core_stop_monitor(struct ClaudeStatusCore *core);

/**
 * Check if credentials file has changed since last check
 * Resets the flag after checking
 *
 * # Safety
 * `core` must be valid
 */
bool claude_status_core_credentials_changed(struct ClaudeStatusCore *core);

/**
 * Set configuration value: update interval in seconds
 *
 * # Safety
 * `core` must be valid
 */
void claude_status_core_set_update_interval(struct ClaudeStatusCore *core, int32_t interval);

/**
 * Set configuration value: yellow threshold percentage
 *
 * # Safety
 * `core` must be valid
 */
void claude_status_core_set_yellow_threshold(struct ClaudeStatusCore *core, int32_t threshold);

/**
 * Set configuration value: orange threshold percentage
 *
 * # Safety
 * `core` must be valid
 */
void claude_status_core_set_orange_threshold(struct ClaudeStatusCore *core, int32_t threshold);

/**
 * Set configuration value: red threshold percentage
 *
 * # Safety
 * `core` must be valid
 */
void claude_status_core_set_red_threshold(struct ClaudeStatusCore *core, int32_t threshold);

/**
 * Get the color code for a percentage value based on thresholds
 * Returns a static string pointer (do not free)
 *
 * # Safety
 * `core` must be valid
 */
const char *claude_status_core_get_color(const struct ClaudeStatusCore *core, double pct);

#endif /* CLAUDE_STATUS_CORE_H */

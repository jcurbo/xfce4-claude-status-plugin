//! FFI boundary definitions for C interop

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;
use std::sync::{Arc, Mutex};

use crate::api::UsageData;
use crate::config::Config;
use crate::credentials::Credentials;
use crate::monitor::CredentialsMonitor;
use crate::transcript::ContextInfo;

/// Opaque handle to the Rust core state
pub struct ClaudeStatusCore {
    credentials: Option<Credentials>,
    config: Config,
    monitor: Option<CredentialsMonitor>,
    last_usage: Option<UsageData>,
    last_context: Option<ContextInfo>,
    creds_changed: Arc<Mutex<bool>>,
}

/// Usage data returned to C
#[repr(C)]
pub struct CUsageData {
    /// 5-hour utilization percentage (0-100)
    pub five_hour_pct: f64,
    /// 7-day utilization percentage (0-100)
    pub seven_day_pct: f64,
    /// 5-hour reset time as Unix timestamp
    pub five_hour_reset_ts: i64,
    /// 7-day reset time as Unix timestamp
    pub seven_day_reset_ts: i64,
    /// Whether the data is valid
    pub valid: bool,
}

/// Context window info returned to C
#[repr(C)]
pub struct CContextInfo {
    /// Context usage percentage (0-100)
    pub context_pct: f64,
    /// Number of tokens used
    pub context_tokens: i64,
    /// Context window size
    pub context_window_size: i64,
    /// Model name (owned by Rust, valid until next call)
    pub model_name: *const c_char,
    /// Whether the data is valid
    pub valid: bool,
}

/// Credentials info returned to C
#[repr(C)]
pub struct CCredentialsInfo {
    /// Plan name ("Pro" or "Max"), null if unknown
    pub plan_name: *const c_char,
    /// Whether credentials are valid
    pub valid: bool,
}

/// Result codes
#[repr(C)]
pub enum CResultCode {
    Ok = 0,
    NoCredentials = 1,
    InvalidCredentials = 2,
    NetworkError = 3,
    ParseError = 4,
    AuthError = 5,
}

// Static storage for strings returned to C
// These are overwritten on each call, so C code must copy if needed
thread_local! {
    static MODEL_NAME: std::cell::RefCell<Option<CString>> = std::cell::RefCell::new(None);
    static PLAN_NAME: std::cell::RefCell<Option<CString>> = std::cell::RefCell::new(None);
}

/// Create a new core instance
///
/// # Safety
/// Returns a pointer that must be freed with `claude_status_core_free`
#[no_mangle]
pub extern "C" fn claude_status_core_new() -> *mut ClaudeStatusCore {
    let core = Box::new(ClaudeStatusCore {
        credentials: None,
        config: Config::default(),
        monitor: None,
        last_usage: None,
        last_context: None,
        creds_changed: Arc::new(Mutex::new(false)),
    });
    Box::into_raw(core)
}

/// Free the core instance
///
/// # Safety
/// `core` must be a valid pointer returned by `claude_status_core_new`
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_free(core: *mut ClaudeStatusCore) {
    if !core.is_null() {
        drop(Box::from_raw(core));
    }
}

/// Load credentials from the specified file
///
/// # Safety
/// `core` must be valid, `path` must be a valid C string or null for default
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_load_credentials(
    core: *mut ClaudeStatusCore,
    path: *const c_char,
) -> CResultCode {
    let core = match core.as_mut() {
        Some(c) => c,
        None => return CResultCode::InvalidCredentials,
    };

    let path_str = if path.is_null() {
        None
    } else {
        match CStr::from_ptr(path).to_str() {
            Ok(s) => Some(s.to_string()),
            Err(_) => return CResultCode::InvalidCredentials,
        }
    };

    match crate::credentials::load_credentials(path_str.as_deref()) {
        Ok(creds) => {
            core.credentials = Some(creds);
            CResultCode::Ok
        }
        Err(_) => {
            core.credentials = None;
            CResultCode::NoCredentials
        }
    }
}

/// Get credentials info
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_get_credentials_info(
    core: *const ClaudeStatusCore,
) -> CCredentialsInfo {
    let core = match core.as_ref() {
        Some(c) => c,
        None => {
            return CCredentialsInfo {
                plan_name: ptr::null(),
                valid: false,
            }
        }
    };

    match &core.credentials {
        Some(creds) => {
            let plan_ptr = creds.plan_name.as_ref().map(|name| {
                PLAN_NAME.with(|cell| {
                    let cstring = CString::new(name.as_str()).unwrap_or_default();
                    let ptr = cstring.as_ptr();
                    *cell.borrow_mut() = Some(cstring);
                    ptr
                })
            });

            CCredentialsInfo {
                plan_name: plan_ptr.unwrap_or(ptr::null()),
                valid: true,
            }
        }
        None => CCredentialsInfo {
            plan_name: ptr::null(),
            valid: false,
        },
    }
}

/// Fetch usage data from the API (blocking)
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_fetch_usage(
    core: *mut ClaudeStatusCore,
) -> CResultCode {
    let core = match core.as_mut() {
        Some(c) => c,
        None => return CResultCode::InvalidCredentials,
    };

    let token = match &core.credentials {
        Some(c) => &c.access_token,
        None => return CResultCode::NoCredentials,
    };

    match crate::api::fetch_usage(token) {
        Ok(usage) => {
            core.last_usage = Some(usage);
            CResultCode::Ok
        }
        Err(crate::api::ApiError::AuthError) => CResultCode::AuthError,
        Err(crate::api::ApiError::NetworkError(_)) => CResultCode::NetworkError,
        Err(crate::api::ApiError::ParseError(_)) => CResultCode::ParseError,
    }
}

/// Get the last fetched usage data
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_get_usage(core: *const ClaudeStatusCore) -> CUsageData {
    let core = match core.as_ref() {
        Some(c) => c,
        None => {
            return CUsageData {
                five_hour_pct: 0.0,
                seven_day_pct: 0.0,
                five_hour_reset_ts: 0,
                seven_day_reset_ts: 0,
                valid: false,
            }
        }
    };

    match &core.last_usage {
        Some(usage) => CUsageData {
            five_hour_pct: usage.five_hour.utilization,
            seven_day_pct: usage.seven_day.utilization,
            five_hour_reset_ts: usage.five_hour.resets_at.timestamp(),
            seven_day_reset_ts: usage.seven_day.resets_at.timestamp(),
            valid: true,
        },
        None => CUsageData {
            five_hour_pct: 0.0,
            seven_day_pct: 0.0,
            five_hour_reset_ts: 0,
            seven_day_reset_ts: 0,
            valid: false,
        },
    }
}

/// Read context info from the latest transcript
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_read_context(
    core: *mut ClaudeStatusCore,
) -> CResultCode {
    let core = match core.as_mut() {
        Some(c) => c,
        None => return CResultCode::InvalidCredentials,
    };

    match crate::transcript::read_context() {
        Ok(info) => {
            core.last_context = Some(info);
            CResultCode::Ok
        }
        Err(_) => {
            core.last_context = None;
            CResultCode::ParseError
        }
    }
}

/// Get the last read context info
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_get_context(
    core: *const ClaudeStatusCore,
) -> CContextInfo {
    let core = match core.as_ref() {
        Some(c) => c,
        None => {
            return CContextInfo {
                context_pct: 0.0,
                context_tokens: 0,
                context_window_size: 0,
                model_name: ptr::null(),
                valid: false,
            }
        }
    };

    match &core.last_context {
        Some(info) => {
            let model_ptr = info.model_name.as_ref().map(|name| {
                MODEL_NAME.with(|cell| {
                    let cstring = CString::new(name.as_str()).unwrap_or_default();
                    let ptr = cstring.as_ptr();
                    *cell.borrow_mut() = Some(cstring);
                    ptr
                })
            });

            CContextInfo {
                context_pct: info.context_pct,
                context_tokens: info.context_tokens,
                context_window_size: info.context_window_size,
                model_name: model_ptr.unwrap_or(ptr::null()),
                valid: true,
            }
        }
        None => CContextInfo {
            context_pct: 0.0,
            context_tokens: 0,
            context_window_size: 0,
            model_name: ptr::null(),
            valid: false,
        },
    }
}

/// Start monitoring the credentials file for changes
///
/// # Safety
/// `core` must be valid, `path` must be a valid C string or null for default
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_start_monitor(
    core: *mut ClaudeStatusCore,
    path: *const c_char,
) -> CResultCode {
    let core = match core.as_mut() {
        Some(c) => c,
        None => return CResultCode::InvalidCredentials,
    };

    let path_str = if path.is_null() {
        None
    } else {
        match CStr::from_ptr(path).to_str() {
            Ok(s) => Some(s.to_string()),
            Err(_) => return CResultCode::InvalidCredentials,
        }
    };

    // Stop existing monitor
    core.monitor = None;

    let changed_flag = Arc::clone(&core.creds_changed);
    match crate::monitor::CredentialsMonitor::new(path_str.as_deref(), changed_flag) {
        Ok(monitor) => {
            core.monitor = Some(monitor);
            CResultCode::Ok
        }
        Err(_) => CResultCode::ParseError,
    }
}

/// Stop monitoring the credentials file
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_stop_monitor(core: *mut ClaudeStatusCore) {
    if let Some(core) = core.as_mut() {
        core.monitor = None;
    }
}

/// Check if credentials file has changed since last check
/// Resets the flag after checking
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_credentials_changed(
    core: *mut ClaudeStatusCore,
) -> bool {
    let core = match core.as_mut() {
        Some(c) => c,
        None => return false,
    };

    let mut changed = core.creds_changed.lock().unwrap();
    let result = *changed;
    *changed = false;
    result
}

/// Set configuration value: update interval in seconds
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_set_update_interval(
    core: *mut ClaudeStatusCore,
    interval: i32,
) {
    if let Some(core) = core.as_mut() {
        core.config.update_interval = interval;
    }
}

/// Set configuration value: yellow threshold percentage
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_set_yellow_threshold(
    core: *mut ClaudeStatusCore,
    threshold: i32,
) {
    if let Some(core) = core.as_mut() {
        core.config.yellow_threshold = threshold;
    }
}

/// Set configuration value: orange threshold percentage
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_set_orange_threshold(
    core: *mut ClaudeStatusCore,
    threshold: i32,
) {
    if let Some(core) = core.as_mut() {
        core.config.orange_threshold = threshold;
    }
}

/// Set configuration value: red threshold percentage
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_set_red_threshold(
    core: *mut ClaudeStatusCore,
    threshold: i32,
) {
    if let Some(core) = core.as_mut() {
        core.config.red_threshold = threshold;
    }
}

/// Get the color code for a percentage value based on thresholds
/// Returns a static string pointer (do not free)
///
/// # Safety
/// `core` must be valid
#[no_mangle]
pub unsafe extern "C" fn claude_status_core_get_color(
    core: *const ClaudeStatusCore,
    pct: f64,
) -> *const c_char {
    static GREEN: &[u8] = b"#5faf5f\0";
    static YELLOW: &[u8] = b"#d7af5f\0";
    static ORANGE: &[u8] = b"#d78700\0";
    static RED: &[u8] = b"#d75f5f\0";

    let core = match core.as_ref() {
        Some(c) => c,
        None => return GREEN.as_ptr() as *const c_char,
    };

    let color = if pct < core.config.yellow_threshold as f64 {
        GREEN
    } else if pct < core.config.orange_threshold as f64 {
        YELLOW
    } else if pct < core.config.red_threshold as f64 {
        ORANGE
    } else {
        RED
    };

    color.as_ptr() as *const c_char
}

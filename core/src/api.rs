//! API client for Anthropic usage endpoint

use chrono::{DateTime, Utc};
use serde::Deserialize;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum ApiError {
    #[error("Authentication failed (401)")]
    AuthError,
    #[error("Network error: {0}")]
    NetworkError(String),
    #[error("Failed to parse response: {0}")]
    ParseError(String),
}

#[derive(Debug, Clone)]
pub struct UsageData {
    pub five_hour: UsagePeriod,
    pub seven_day: UsagePeriod,
}

#[derive(Debug, Clone)]
pub struct UsagePeriod {
    pub utilization: f64,
    pub resets_at: DateTime<Utc>,
}

#[derive(Debug, Deserialize)]
struct ApiResponse {
    five_hour: ApiPeriod,
    seven_day: ApiPeriod,
}

#[derive(Debug, Deserialize)]
struct ApiPeriod {
    utilization: f64,
    resets_at: String,
}

const USAGE_API_URL: &str = "https://api.anthropic.com/api/oauth/usage";
const USER_AGENT: &str = "xfce-claude-status/0.1";

/// Fetch usage data from the Anthropic API
pub fn fetch_usage(access_token: &str) -> Result<UsageData, ApiError> {
    let response = ureq::get(USAGE_API_URL)
        .set("Authorization", &format!("Bearer {}", access_token))
        .set("anthropic-beta", "oauth-2025-04-20")
        .set("User-Agent", USER_AGENT)
        .call();

    match response {
        Ok(resp) => {
            let body = resp
                .into_string()
                .map_err(|e| ApiError::ParseError(e.to_string()))?;

            let api_resp: ApiResponse =
                serde_json::from_str(&body).map_err(|e| ApiError::ParseError(e.to_string()))?;

            let five_hour_reset = DateTime::parse_from_rfc3339(&api_resp.five_hour.resets_at)
                .map_err(|e| ApiError::ParseError(e.to_string()))?
                .with_timezone(&Utc);

            let seven_day_reset = DateTime::parse_from_rfc3339(&api_resp.seven_day.resets_at)
                .map_err(|e| ApiError::ParseError(e.to_string()))?
                .with_timezone(&Utc);

            Ok(UsageData {
                five_hour: UsagePeriod {
                    utilization: api_resp.five_hour.utilization,
                    resets_at: five_hour_reset,
                },
                seven_day: UsagePeriod {
                    utilization: api_resp.seven_day.utilization,
                    resets_at: seven_day_reset,
                },
            })
        }
        Err(ureq::Error::Status(401, _)) => Err(ApiError::AuthError),
        Err(e) => Err(ApiError::NetworkError(e.to_string())),
    }
}

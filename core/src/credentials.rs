//! Credential loading from Claude Code config files

use serde::Deserialize;
use std::fs;
use std::path::PathBuf;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum CredentialsError {
    #[error("Failed to read credentials file: {0}")]
    IoError(#[from] std::io::Error),
    #[error("Failed to parse credentials JSON: {0}")]
    ParseError(#[from] serde_json::Error),
    #[error("Missing OAuth credentials in file")]
    MissingOAuth,
    #[error("Missing access token")]
    MissingToken,
}

#[derive(Debug, Clone)]
pub struct Credentials {
    pub access_token: String,
    pub plan_name: Option<String>,
}

#[derive(Debug, Deserialize)]
struct CredentialsFile {
    #[serde(rename = "claudeAiOauth")]
    claude_ai_oauth: Option<OAuthSection>,
}

#[derive(Debug, Deserialize)]
struct OAuthSection {
    #[serde(rename = "accessToken")]
    access_token: Option<String>,
    #[serde(rename = "subscriptionType")]
    subscription_type: Option<String>,
}

/// Default credentials file path
const DEFAULT_CREDS_PATH: &str = ".claude/.credentials.json";

/// Expand ~ to home directory
fn expand_path(path: &str) -> PathBuf {
    if let Some(rest) = path.strip_prefix("~/") {
        if let Some(home) = dirs::home_dir() {
            return home.join(rest);
        }
    } else if path == "~" {
        if let Some(home) = dirs::home_dir() {
            return home;
        }
    }
    PathBuf::from(path)
}

/// Get the default credentials file path
pub fn default_credentials_path() -> PathBuf {
    dirs::home_dir()
        .map(|h| h.join(DEFAULT_CREDS_PATH))
        .unwrap_or_else(|| PathBuf::from(DEFAULT_CREDS_PATH))
}

/// Load credentials from a file
///
/// If `path` is None, uses the default path `~/.claude/.credentials.json`
pub fn load_credentials(path: Option<&str>) -> Result<Credentials, CredentialsError> {
    let path = match path {
        Some(p) => expand_path(p),
        None => default_credentials_path(),
    };

    let contents = fs::read_to_string(&path)?;
    let file: CredentialsFile = serde_json::from_str(&contents)?;

    let oauth = file.claude_ai_oauth.ok_or(CredentialsError::MissingOAuth)?;

    let access_token = oauth
        .access_token
        .filter(|t| !t.is_empty())
        .ok_or(CredentialsError::MissingToken)?;

    let plan_name = oauth.subscription_type.and_then(|sub| {
        if sub.contains("max") {
            Some("Max".to_string())
        } else if sub.contains("pro") {
            Some("Pro".to_string())
        } else {
            None
        }
    });

    Ok(Credentials {
        access_token,
        plan_name,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_expand_path_tilde() {
        let expanded = expand_path("~/test/path");
        assert!(expanded.to_string_lossy().contains("test/path"));
        assert!(!expanded.to_string_lossy().starts_with('~'));
    }

    #[test]
    fn test_expand_path_absolute() {
        let expanded = expand_path("/absolute/path");
        assert_eq!(expanded, PathBuf::from("/absolute/path"));
    }
}

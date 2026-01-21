//! Transcript parsing for context window usage

use serde::Deserialize;
use std::fs::{self, File};
use std::io::{BufRead, BufReader};
use std::path::PathBuf;
use std::time::SystemTime;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum TranscriptError {
    #[error("Failed to read transcript: {0}")]
    IoError(#[from] std::io::Error),
    #[error("No transcript files found")]
    NoTranscripts,
    #[error("Failed to parse transcript: {0}")]
    ParseError(String),
}

#[derive(Debug, Clone)]
pub struct ContextInfo {
    pub context_pct: f64,
    pub context_tokens: i64,
    pub context_window_size: i64,
    pub model_name: Option<String>,
}

/// Default context window size (200K tokens)
const CONTEXT_WINDOW_DEFAULT: i64 = 200_000;

#[derive(Debug, Deserialize)]
struct TranscriptEntry {
    #[serde(rename = "type")]
    entry_type: Option<String>,
    message: Option<MessageData>,
}

#[derive(Debug, Deserialize)]
struct MessageData {
    model: Option<String>,
    usage: Option<UsageData>,
}

#[derive(Debug, Deserialize)]
struct UsageData {
    input_tokens: Option<i64>,
    cache_creation_input_tokens: Option<i64>,
    cache_read_input_tokens: Option<i64>,
}

/// Find the most recently modified transcript file
fn find_latest_transcript() -> Result<PathBuf, TranscriptError> {
    let projects_dir = dirs::home_dir()
        .ok_or(TranscriptError::NoTranscripts)?
        .join(".claude")
        .join("projects");

    if !projects_dir.exists() {
        return Err(TranscriptError::NoTranscripts);
    }

    let mut latest_path: Option<PathBuf> = None;
    let mut latest_time: Option<SystemTime> = None;

    // Iterate through project directories
    for project_entry in fs::read_dir(&projects_dir)? {
        let project_entry = project_entry?;
        let project_path = project_entry.path();

        if !project_path.is_dir() {
            continue;
        }

        // Look for .jsonl files in each project
        for file_entry in fs::read_dir(&project_path)? {
            let file_entry = file_entry?;
            let file_path = file_entry.path();

            if file_path.extension().map_or(false, |ext| ext == "jsonl") {
                if let Ok(metadata) = file_entry.metadata() {
                    if let Ok(modified) = metadata.modified() {
                        if latest_time.is_none() || Some(modified) > latest_time {
                            latest_time = Some(modified);
                            latest_path = Some(file_path);
                        }
                    }
                }
            }
        }
    }

    latest_path.ok_or(TranscriptError::NoTranscripts)
}

/// Read context window usage from the latest transcript
pub fn read_context() -> Result<ContextInfo, TranscriptError> {
    let transcript_path = find_latest_transcript()?;
    let file = File::open(&transcript_path)?;
    let reader = BufReader::new(file);

    let mut last_input: i64 = 0;
    let mut last_cache_creation: i64 = 0;
    let mut last_cache_read: i64 = 0;
    let mut last_model: Option<String> = None;

    for line in reader.lines() {
        let line = line?;
        if line.is_empty() {
            continue;
        }

        // Try to parse each line as JSON
        if let Ok(entry) = serde_json::from_str::<TranscriptEntry>(&line) {
            // Only process assistant messages
            if entry.entry_type.as_deref() != Some("assistant") {
                continue;
            }

            if let Some(message) = entry.message {
                // Update model name if present
                if let Some(model) = message.model {
                    last_model = Some(model);
                }

                // Update usage if present
                if let Some(usage) = message.usage {
                    last_input = usage.input_tokens.unwrap_or(0);
                    last_cache_creation = usage.cache_creation_input_tokens.unwrap_or(0);
                    last_cache_read = usage.cache_read_input_tokens.unwrap_or(0);
                }
            }
        }
        // Silently skip lines that don't parse
    }

    let total_context = last_input + last_cache_creation + last_cache_read;
    let context_window = CONTEXT_WINDOW_DEFAULT;
    let context_pct = (total_context as f64 / context_window as f64 * 100.0).min(100.0);

    Ok(ContextInfo {
        context_pct,
        context_tokens: total_context,
        context_window_size: context_window,
        model_name: last_model,
    })
}

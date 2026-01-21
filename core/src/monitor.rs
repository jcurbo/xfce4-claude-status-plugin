//! File monitoring for credentials changes

use notify::{Config, Event, RecommendedWatcher, RecursiveMode, Watcher};
use std::path::PathBuf;
use std::sync::mpsc::{channel, Receiver};
use std::sync::{Arc, Mutex};
use std::thread;
use thiserror::Error;

use crate::credentials::default_credentials_path;

#[derive(Debug, Error)]
pub enum MonitorError {
    #[error("Failed to create file watcher: {0}")]
    WatcherError(String),
    #[error("Failed to watch path: {0}")]
    PathError(String),
}

pub struct CredentialsMonitor {
    _watcher: RecommendedWatcher,
    _handle: thread::JoinHandle<()>,
}

impl CredentialsMonitor {
    /// Create a new credentials monitor
    ///
    /// When the file changes, sets the `changed` flag to true.
    /// The caller should poll this flag and reset it after handling.
    pub fn new(
        path: Option<&str>,
        changed: Arc<Mutex<bool>>,
    ) -> Result<Self, MonitorError> {
        let watch_path = match path {
            Some(p) => {
                let p = if let Some(rest) = p.strip_prefix("~/") {
                    dirs::home_dir()
                        .map(|h| h.join(rest))
                        .unwrap_or_else(|| PathBuf::from(p))
                } else {
                    PathBuf::from(p)
                };
                p
            }
            None => default_credentials_path(),
        };

        let (tx, rx): (_, Receiver<Result<Event, notify::Error>>) = channel();

        let mut watcher = RecommendedWatcher::new(
            move |res| {
                let _ = tx.send(res);
            },
            Config::default(),
        )
        .map_err(|e| MonitorError::WatcherError(e.to_string()))?;

        watcher
            .watch(&watch_path, RecursiveMode::NonRecursive)
            .map_err(|e| MonitorError::PathError(e.to_string()))?;

        // Spawn thread to process events
        let handle = thread::spawn(move || {
            for res in rx {
                if let Ok(event) = res {
                    use notify::EventKind::*;
                    match event.kind {
                        Create(_) | Modify(_) => {
                            if let Ok(mut flag) = changed.lock() {
                                *flag = true;
                            }
                        }
                        _ => {}
                    }
                }
            }
        });

        Ok(CredentialsMonitor {
            _watcher: watcher,
            _handle: handle,
        })
    }
}

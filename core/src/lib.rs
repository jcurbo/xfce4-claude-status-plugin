//! Core library for xfce4-claude-status-plugin
//!
//! This library provides the business logic for the Claude status panel plugin,
//! exposed via a C FFI for integration with the XFCE panel.

mod credentials;
mod api;
mod transcript;
mod config;
mod monitor;
mod ffi;

pub use ffi::*;

//! Configuration management

/// Default configuration values
const DEFAULT_UPDATE_INTERVAL: i32 = 30;
const DEFAULT_YELLOW_THRESHOLD: i32 = 25;
const DEFAULT_ORANGE_THRESHOLD: i32 = 50;
const DEFAULT_RED_THRESHOLD: i32 = 75;

/// Plugin configuration
#[derive(Debug, Clone)]
pub struct Config {
    pub update_interval: i32,
    pub yellow_threshold: i32,
    pub orange_threshold: i32,
    pub red_threshold: i32,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            update_interval: DEFAULT_UPDATE_INTERVAL,
            yellow_threshold: DEFAULT_YELLOW_THRESHOLD,
            orange_threshold: DEFAULT_ORANGE_THRESHOLD,
            red_threshold: DEFAULT_RED_THRESHOLD,
        }
    }
}

use std::fs;

pub struct AppConfig {
    pub host: String,
    pub debug: bool,
    port: u16,
    max_connections: Option<usize>,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            host: "127.0.0.1".into(),
            port: default_port(),
            debug: false,
            max_connections: None,
        }
    }
}

impl AppConfig {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn load(path: &str) -> Result<Self, String> {
        let content = fs::read_to_string(path).map_err(|e| format!("read error: {}", e))?;
        let mut cfg = Self::new();
        for line in content.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if let Some((key, val)) = line.split_once('=') {
                match key.trim() {
                    "host" => cfg.host = val.trim().to_string(),
                    "port" => cfg.port = val.trim().parse().unwrap_or(default_port()),
                    "debug" => cfg.debug = val.trim() == "true",
                    "max_connections" => cfg.max_connections = val.trim().parse().ok(),
                    _ => {}
                }
            }
        }
        Ok(cfg)
    }

    pub fn validate(&self) -> bool {
        !self.host.is_empty() && self.port > 0 && self.port < 65535
    }

    pub fn port(&self) -> u16 {
        self.port
    }
}

pub fn default_port() -> u16 {
    8080
}

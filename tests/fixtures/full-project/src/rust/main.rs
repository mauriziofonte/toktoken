mod config;

use config::AppConfig;
use std::io::{self, Write};
use std::net::TcpListener;
use std::process;

fn initialize(cfg: &AppConfig) -> Result<(), String> {
    if !cfg.validate() {
        return Err("invalid configuration: check host and port".into());
    }
    println!("initializing with host={} port={}", cfg.host, cfg.port());
    Ok(())
}

fn run_server(cfg: &AppConfig) -> Result<(), String> {
    let addr = format!("{}:{}", cfg.host, cfg.port());
    let listener = TcpListener::bind(&addr).map_err(|e| format!("bind failed: {}", e))?;
    println!("listening on {}", addr);

    for stream in listener.incoming() {
        match stream {
            Ok(mut conn) => {
                let response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
                if let Err(e) = conn.write_all(response.as_bytes()) {
                    eprintln!("write error: {}", e);
                }
            }
            Err(e) => eprintln!("accept error: {}", e),
        }
    }
    Ok(())
}

fn main() {
    let cfg = AppConfig::load("config.toml").unwrap_or_else(|_| {
        eprintln!("using default configuration");
        AppConfig::new()
    });

    if let Err(e) = initialize(&cfg) {
        eprintln!("initialization failed: {}", e);
        process::exit(1);
    }

    if let Err(e) = run_server(&cfg) {
        eprintln!("server error: {}", e);
        process::exit(1);
    }
}

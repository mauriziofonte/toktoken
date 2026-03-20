package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os"

	"myapp/handlers"
)

type Config struct {
	Port    string `json:"port"`
	Host    string `json:"host"`
	Debug   bool   `json:"debug"`
	MaxConn int    `json:"max_connections"`
}

func loadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return &Config{Port: "8080", Host: "0.0.0.0", Debug: false, MaxConn: 100}, nil
	}
	var cfg Config
	if err := json.Unmarshal(data, &cfg); err != nil {
		return nil, fmt.Errorf("invalid config: %w", err)
	}
	if cfg.Port == "" {
		cfg.Port = "8080"
	}
	return &cfg, nil
}

func setupRoutes(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/items", handlers.HandleGet)
	mux.HandleFunc("POST /api/items", handlers.HandlePost)
}

func main() {
	cfg, err := loadConfig("config.json")
	if err != nil {
		log.Fatalf("failed to load config: %v", err)
	}

	mux := http.NewServeMux()
	setupRoutes(mux)

	addr := fmt.Sprintf("%s:%s", cfg.Host, cfg.Port)
	log.Printf("starting server on %s (debug=%v)", addr, cfg.Debug)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatalf("server error: %v", err)
	}
}

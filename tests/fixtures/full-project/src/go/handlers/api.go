package handlers

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
)

type Item struct {
	ID    int    `json:"id"`
	Name  string `json:"name"`
	Value float64 `json:"value"`
}

type ErrorResponse struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func ValidateRequest(r *http.Request, requiredContentType string) error {
	ct := r.Header.Get("Content-Type")
	if !strings.HasPrefix(ct, requiredContentType) {
		return fmt.Errorf("expected %s, got %s", requiredContentType, ct)
	}
	if r.ContentLength > 1<<20 {
		return fmt.Errorf("request body exceeds 1MB limit")
	}
	return nil
}

func ParseBody(r *http.Request, dest interface{}) error {
	body, err := io.ReadAll(io.LimitReader(r.Body, 1<<20))
	if err != nil {
		return fmt.Errorf("failed to read body: %w", err)
	}
	defer r.Body.Close()
	if err := json.Unmarshal(body, dest); err != nil {
		return fmt.Errorf("invalid JSON: %w", err)
	}
	return nil
}

func HandleGet(w http.ResponseWriter, r *http.Request) {
	items := []Item{
		{ID: 1, Name: "alpha", Value: 10.5},
		{ID: 2, Name: "beta", Value: 23.1},
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(items)
}

func HandlePost(w http.ResponseWriter, r *http.Request) {
	if err := ValidateRequest(r, "application/json"); err != nil {
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(ErrorResponse{Code: 400, Message: err.Error()})
		return
	}
	var item Item
	if err := ParseBody(r, &item); err != nil {
		w.WriteHeader(http.StatusUnprocessableEntity)
		json.NewEncoder(w).Encode(ErrorResponse{Code: 422, Message: err.Error()})
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(item)
}

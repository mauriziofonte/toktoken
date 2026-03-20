#!/usr/bin/env bash
set -euo pipefail

readonly DEPLOY_DIR="/opt/app"
readonly BACKUP_DIR="/opt/backups"
readonly LOG_FILE="/var/log/deploy.log"
readonly HEALTH_ENDPOINT="http://localhost:8080/health"
readonly MAX_RETRIES=5

deploy_app() {
    local version="${1:?Version required}"
    local artifact="app-${version}.tar.gz"

    validate_env

    echo "[$(date -Iseconds)] Deploying version ${version}..." >> "${LOG_FILE}"

    if [[ -d "${DEPLOY_DIR}/current" ]]; then
        local backup_name="backup-$(date +%Y%m%d-%H%M%S)"
        cp -r "${DEPLOY_DIR}/current" "${BACKUP_DIR}/${backup_name}"
        echo "Backup created: ${backup_name}"
    fi

    tar -xzf "${artifact}" -C "${DEPLOY_DIR}/current"
    chmod -R 755 "${DEPLOY_DIR}/current/bin"

    systemctl restart app.service

    if ! check_health; then
        echo "Health check failed, rolling back..." >&2
        rollback
        return 1
    fi

    echo "[$(date -Iseconds)] Deploy ${version} completed successfully" >> "${LOG_FILE}"
}

rollback() {
    local latest_backup
    latest_backup=$(ls -t "${BACKUP_DIR}" | head -1)

    if [[ -z "${latest_backup}" ]]; then
        echo "No backup available for rollback" >&2
        return 1
    fi

    echo "Rolling back to ${latest_backup}..."
    rm -rf "${DEPLOY_DIR}/current"
    cp -r "${BACKUP_DIR}/${latest_backup}" "${DEPLOY_DIR}/current"
    systemctl restart app.service

    if check_health; then
        echo "Rollback successful"
    else
        echo "Rollback failed — manual intervention required" >&2
        return 1
    fi
}

check_health() {
    local retries=0

    while [[ ${retries} -lt ${MAX_RETRIES} ]]; do
        if curl -sf "${HEALTH_ENDPOINT}" > /dev/null 2>&1; then
            return 0
        fi
        retries=$((retries + 1))
        echo "Health check attempt ${retries}/${MAX_RETRIES}..."
        sleep 2
    done

    return 1
}

validate_env() {
    local required_vars=("APP_ENV" "DB_HOST" "DB_PASSWORD")
    local missing=()

    for var in "${required_vars[@]}"; do
        if [[ -z "${!var:-}" ]]; then
            missing+=("${var}")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "Missing required environment variables: ${missing[*]}" >&2
        return 1
    fi

    for cmd in curl tar systemctl; do
        if ! command -v "${cmd}" &> /dev/null; then
            echo "Required command not found: ${cmd}" >&2
            return 1
        fi
    done
}

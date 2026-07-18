#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
codex_executable="${KICHAD_CODEX_EXECUTABLE:-codex}"
base_version="$(<"${repo_root}/.kichad-base-version")"

"${repo_root}/tools/check-codex-app-server.sh"

if ! command -v jq >/dev/null 2>&1; then
    echo "jq is required for the Codex app-server protocol smoke check." >&2
    exit 1
fi

mkdir -p -- "${repo_root}/build"
smoke_home="$(mktemp -d "${repo_root}/build/codex-smoke-home.XXXXXX")"
smoke_pid=""
smoke_write_fd=""
smoke_write_open=false

cleanup() {
    if [[ "$smoke_write_open" == true ]]; then
        exec {smoke_write_fd}>&-
        smoke_write_open=false
    fi

    if [[ -n "$smoke_pid" ]] && kill -0 "$smoke_pid" 2>/dev/null; then
        kill -TERM "$smoke_pid" 2>/dev/null || true
    fi

    if [[ -n "$smoke_pid" ]]; then
        wait "$smoke_pid" 2>/dev/null || true
    fi

    rm -rf -- "$smoke_home"
}

trap cleanup EXIT

coproc KICHAD_CODEX_SMOKE {
    CODEX_HOME="$smoke_home" "$codex_executable" app-server \
        -c 'mcp_servers={}' \
        --disable shell_tool \
        --disable unified_exec \
        --disable apps \
        --disable browser_use \
        --disable computer_use \
        --disable image_generation \
        --disable multi_agent \
        --disable plugins \
        --disable enable_mcp_apps \
        --disable hooks \
        --disable skill_mcp_dependency_install \
        --disable workspace_dependencies
}
smoke_pid="$KICHAD_CODEX_SMOKE_PID"
smoke_read_fd="${KICHAD_CODEX_SMOKE[0]}"
smoke_write_fd="${KICHAD_CODEX_SMOKE[1]}"
smoke_write_open=true

send_message() {
    printf '%s\n' "$1" >&"$smoke_write_fd"
}

expect_result() {
    local request_id="$1"
    local label="$2"
    local line
    local deadline=$(( SECONDS + 15 ))

    while (( SECONDS < deadline )); do
        if IFS= read -r -t 1 -u "$smoke_read_fd" line; then
            if jq -e --argjson wanted "$request_id" '.id == $wanted' <<<"$line" >/dev/null 2>&1; then
                if jq -e 'has("result")' <<<"$line" >/dev/null; then
                    printf 'Codex protocol verified: %s\n' "$label"
                    return 0
                fi

                echo "Codex app-server rejected ${label}:" >&2
                jq -c '.error // .' <<<"$line" >&2
                return 1
            fi
        fi
    done

    echo "Timed out waiting for Codex app-server ${label}." >&2
    return 1
}

send_message "$(jq -cn --arg version "$base_version" '{
    id: 1,
    method: "initialize",
    params: {
        clientInfo: { name: "kichad", title: "KiChad", version: $version },
        capabilities: { experimentalApi: true, requestAttestation: false }
    }
}')"
expect_result 1 initialize

send_message '{"method":"initialized"}'
send_message '{"id":2,"method":"account/read","params":{"refreshToken":false}}'
expect_result 2 account/read

send_message '{"id":3,"method":"model/list","params":{"includeHidden":false}}'
expect_result 3 model/list

send_message "$(jq -cn --arg cwd "$repo_root" '{
    id: 4,
    method: "thread/start",
    params: {
        cwd: $cwd,
        runtimeWorkspaceRoots: [$cwd],
        approvalPolicy: "never",
        sandbox: "read-only",
        ephemeral: true,
        historyMode: "paginated",
        serviceName: "KiChad",
        baseInstructions: "Use only native KiChad dynamic tools.",
        config: {
            features: {
                shell_tool: false,
                unified_exec: false,
                apps: false,
                browser_use: false,
                computer_use: false,
                image_generation: false,
                multi_agent: false,
                plugins: false,
                enable_mcp_apps: false,
                hooks: false,
                skill_mcp_dependency_install: false,
                workspace_dependencies: false
            },
            mcp_servers: {},
            web_search: "live"
        },
        dynamicTools: [
            {
                type: "function",
                name: "project",
                description: "Read KiChad project context.",
                inputSchema: {
                    type: "object",
                    additionalProperties: false,
                    required: ["operation"],
                    properties: {
                        operation: { type: "string", enum: ["context"] }
                    }
                }
            },
            {
                type: "function",
                name: "inspect",
                description: "Inspect a KiCad 10 design file without changing it.",
                inputSchema: {
                    type: "object",
                    additionalProperties: false,
                    required: ["operation", "path"],
                    properties: {
                        operation: { type: "string", enum: ["summary", "find"] },
                        path: { type: "string", maxLength: 4096 },
                        head: { type: "string", maxLength: 128 },
                        limit: { type: "integer", minimum: 1, maximum: 50 }
                    }
                }
            }
        ]
    }
}')"
expect_result 4 thread/start

exec {smoke_write_fd}>&-
smoke_write_open=false
wait "$smoke_pid"
rm -rf -- "$smoke_home"
trap - EXIT

echo "KiChad Codex app-server protocol smoke check passed."

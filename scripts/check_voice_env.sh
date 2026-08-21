#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_FILE="${1:-$ROOT/voice_interaction/config/voice_assistant.yaml}"

ok_count=0
warn_count=0
fail_count=0

section() {
  printf '\n== %s ==\n' "$1"
}

ok() {
  ok_count=$((ok_count + 1))
  printf '[OK]   %s\n' "$1"
}

warn() {
  warn_count=$((warn_count + 1))
  printf '[WARN] %s\n' "$1"
}

fail() {
  fail_count=$((fail_count + 1))
  printf '[FAIL] %s\n' "$1"
}

trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "$value"
}

expand_path() {
  local value
  value="$(trim "$1")"
  value="${value%\"}"
  value="${value#\"}"
  value="${value%\'}"
  value="${value#\'}"
  value="${value//\$\(env HOME\)/$HOME}"
  value="${value//\$\{HOME\}/$HOME}"
  value="${value/#\~/$HOME}"
  printf '%s' "$value"
}

yaml_value() {
  local key="$1"
  awk -v key="$key" '
    $1 == key ":" {
      sub(/^[^:]+:[[:space:]]*/, "")
      gsub(/[[:space:]]*#.*/, "")
      print
      exit
    }
  ' "$CONFIG_FILE" 2>/dev/null
}

check_command() {
  local name="$1"
  if command -v "$name" >/dev/null 2>&1; then
    ok "$name: $(command -v "$name")"
  else
    fail "$name not found in PATH"
  fi
}

check_file() {
  local label="$1"
  local path="$2"
  if [[ -f "$path" ]]; then
    ok "$label: $path"
  else
    fail "$label missing: $path"
  fi
}

check_optional_file() {
  local label="$1"
  local path="$2"
  if [[ -z "$path" ]]; then
    warn "$label not configured"
  elif [[ -f "$path" ]]; then
    ok "$label: $path"
  else
    warn "$label missing: $path"
  fi
}

check_model_file() {
  local label="$1"
  local dir="$2"
  shift 2
  local name path
  for name in "$@"; do
    path="$dir/$name"
    if [[ -f "$path" ]]; then
      ok "$label: $path"
      return
    fi
  done
  fail "$label missing under $dir"
}

suggest_sherpa_roots() {
  local roots=("$HOME" "$ROOT")
  local candidates
  candidates="$(
    find "${roots[@]}" -maxdepth 4 -type f \
      -path '*/lib/libsherpa-onnx-c-api.so' -printf '%h\n' 2>/dev/null |
      sed 's#/lib$##' | sort -u
  )"
  if [[ -n "$candidates" ]]; then
    printf '       Possible SHERPA_ONNX_ROOT values:\n'
    while IFS= read -r candidate; do
      printf '       export SHERPA_ONNX_ROOT=%s\n' "$candidate"
    done <<< "$candidates"
  fi
}

printf 'Voice environment check\n'
printf 'Root:   %s\n' "$ROOT"
printf 'Config: %s\n' "$CONFIG_FILE"

if [[ ! -f "$CONFIG_FILE" ]]; then
  fail "config file not found: $CONFIG_FILE"
  printf '\nSummary: %d OK, %d WARN, %d FAIL\n' \
    "$ok_count" "$warn_count" "$fail_count"
  exit 1
fi

asr_model_dir="$(expand_path "$(yaml_value asr_model_dir)")"
kws_model_dir="$(expand_path "$(yaml_value kws_model_dir)")"
kws_encoder="$(expand_path "$(yaml_value kws_encoder)")"
kws_decoder="$(expand_path "$(yaml_value kws_decoder)")"
kws_joiner="$(expand_path "$(yaml_value kws_joiner)")"
kws_tokens="$(expand_path "$(yaml_value kws_tokens)")"
kws_keywords_file="$(expand_path "$(yaml_value kws_keywords_file)")"
piper_model="$(expand_path "$(yaml_value piper_model)")"
capture_device="$(expand_path "$(yaml_value capture_device)")"
tts_device="$(expand_path "$(yaml_value tts_device)")"
use_ollama="$(expand_path "$(yaml_value use_ollama)")"
ollama_model="$(expand_path "$(yaml_value ollama_model)")"

section "Commands"
check_command ros2
check_command colcon
check_command ffplay
check_command espeak-ng
check_command piper
if command -v edge-tts >/dev/null 2>&1; then
  ok "edge-tts: $(command -v edge-tts)"
else
  warn "edge-tts not found; offline fallback can still use Piper/espeak-ng"
fi
if command -v ollama >/dev/null 2>&1; then
  ok "ollama: $(command -v ollama)"
elif [[ "$use_ollama" == "true" ]]; then
  fail "ollama not found but use_ollama is true"
else
  warn "ollama not found"
fi

section "Sherpa-ONNX"
if [[ -z "${SHERPA_ONNX_ROOT:-}" ]]; then
  fail "SHERPA_ONNX_ROOT is not set"
  suggest_sherpa_roots
elif [[ "$SHERPA_ONNX_ROOT" == /path/to/* ]]; then
  fail "SHERPA_ONNX_ROOT still uses placeholder path: $SHERPA_ONNX_ROOT"
  suggest_sherpa_roots
else
  ok "SHERPA_ONNX_ROOT=$SHERPA_ONNX_ROOT"
  check_file "Sherpa C API library" \
    "$SHERPA_ONNX_ROOT/lib/libsherpa-onnx-c-api.so"
  check_file "ONNX Runtime library" \
    "$SHERPA_ONNX_ROOT/lib/libonnxruntime.so"
  check_file "Sherpa C API header" \
    "$SHERPA_ONNX_ROOT/include/sherpa-onnx/c-api/c-api.h"
fi

section "ASR Model"
if [[ -d "$asr_model_dir" ]]; then
  ok "ASR model dir: $asr_model_dir"
  check_file "ASR encoder" "$asr_model_dir/encoder-epoch-99-avg-1.onnx"
  check_file "ASR decoder" "$asr_model_dir/decoder-epoch-99-avg-1.onnx"
  check_file "ASR joiner" "$asr_model_dir/joiner-epoch-99-avg-1.onnx"
  check_file "ASR tokens" "$asr_model_dir/tokens.txt"
else
  fail "ASR model dir missing: $asr_model_dir"
fi

section "KWS Model"
if [[ -d "$kws_model_dir" ]]; then
  ok "KWS model dir: $kws_model_dir"
  if [[ -n "$kws_encoder" ]]; then
    check_file "KWS encoder" "$kws_encoder"
  else
    check_model_file "KWS encoder" "$kws_model_dir" \
      encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx \
      encoder-epoch-12-avg-2-chunk-16-left-64.onnx
  fi
  if [[ -n "$kws_decoder" ]]; then
    check_file "KWS decoder" "$kws_decoder"
  else
    check_model_file "KWS decoder" "$kws_model_dir" \
      decoder-epoch-12-avg-2-chunk-16-left-64.onnx \
      decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx
  fi
  if [[ -n "$kws_joiner" ]]; then
    check_file "KWS joiner" "$kws_joiner"
  else
    check_model_file "KWS joiner" "$kws_model_dir" \
      joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx \
      joiner-epoch-12-avg-2-chunk-16-left-64.onnx
  fi
  if [[ -n "$kws_tokens" ]]; then
    check_file "KWS tokens" "$kws_tokens"
  else
    check_file "KWS tokens" "$kws_model_dir/tokens.txt"
    kws_tokens="$kws_model_dir/tokens.txt"
  fi
else
  warn "KWS model dir missing: $kws_model_dir; node will fall back to ASR wake"
fi
check_file "KWS keywords file" "$kws_keywords_file"

if [[ -f "$kws_keywords_file" && -f "$kws_tokens" ]]; then
  missing_tokens="$(awk '
    /^[[:space:]]*($|#)/ { next }
    {
      for (i = 1; i <= NF; ++i) {
        if ($i ~ /^@/) break
        print $i
      }
    }
  ' "$kws_keywords_file" | sort -u | while IFS= read -r token; do
    [[ -z "$token" ]] && continue
    if ! grep -Fqx "$token" "$kws_tokens" &&
       ! grep -Fq "$token " "$kws_tokens"; then
      printf '%s\n' "$token"
    fi
  done)"
  if [[ -z "$missing_tokens" ]]; then
    ok "KWS keyword tokens are present in tokens.txt"
  else
    fail "KWS keyword tokens missing from tokens.txt: $(echo "$missing_tokens" | tr '\n' ' ')"
  fi
fi

section "TTS"
check_optional_file "Piper model" "$piper_model"
if [[ -n "$piper_model" ]]; then
  if [[ -f "$piper_model.json" ]]; then
    ok "Piper model config: $piper_model.json"
  elif [[ "$piper_model" == *.onnx && -f "${piper_model%.onnx}.onnx.json" ]]; then
    ok "Piper model config: ${piper_model%.onnx}.onnx.json"
  elif [[ "$piper_model" == *.onnx && -f "${piper_model%.onnx}.json" ]]; then
    ok "Piper model config: ${piper_model%.onnx}.json"
  else
    warn "Piper model config missing next to: $piper_model"
  fi
fi
if [[ -z "$tts_device" || "$tts_device" == "default" ]]; then
  ok "tts_device uses system default output"
else
  ok "tts_device configured: $tts_device"
fi

section "Audio Devices"
if command -v arecord >/dev/null 2>&1; then
  ok "arecord is available"
  arecord -l 2>/dev/null | sed 's/^/  /' || warn "arecord -l failed"
  if [[ -z "$capture_device" || "$capture_device" == "auto" ]]; then
    ok "capture_device uses ReSpeaker auto-discovery"
  else
    ok "capture_device configured: $capture_device"
  fi
else
  fail "arecord not found"
fi

if command -v aplay >/dev/null 2>&1; then
  ok "aplay is available"
  aplay -l 2>/dev/null | sed 's/^/  /' || warn "aplay -l failed"
else
  fail "aplay not found"
fi

section "Ollama"
if [[ "$use_ollama" == "true" ]]; then
  if command -v ollama >/dev/null 2>&1; then
    if ollama list >/tmp/check_voice_env_ollama.txt 2>/dev/null; then
      ok "ollama service is reachable"
      if [[ -n "$ollama_model" ]] &&
         grep -Fq "${ollama_model%%:*}" /tmp/check_voice_env_ollama.txt; then
        ok "ollama model appears installed: $ollama_model"
      else
        warn "ollama model not found in 'ollama list': $ollama_model"
      fi
    else
      warn "ollama command exists but service is not reachable"
    fi
  fi
else
  ok "use_ollama is false"
fi
rm -f /tmp/check_voice_env_ollama.txt

printf '\nSummary: %d OK, %d WARN, %d FAIL\n' \
  "$ok_count" "$warn_count" "$fail_count"

if (( fail_count > 0 )); then
  exit 1
fi
exit 0

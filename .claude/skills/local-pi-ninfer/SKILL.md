---
name: local-pi-ninfer
description: Run and troubleshoot the local pi coding agent backed by ninfer at 127.0.0.1:8080, including Qwen chat-template/tool calls, 22 GB GPU context sizing, vision image input, and FM TOWNS capture inspection.
---

# Local pi + ninfer

The local coding agent is pi 0.84.x using provider `local-llm`, model `qwen3.8-27b`, and ninfer at
`http://127.0.0.1:8080/v1`. Ninfer must be running before pi is invoked.

## Start the server

Use the repository wrapper:

```sh
tools/local_ai/serve_ninfer.sh
```

It calls `/home/anonymous/Documents/DEV/ninfer-2080ti-22g/serve.sh` with vision enabled and a
16,384-token context/KV profile. On the installed 22 GB GPU, vision plus the default 131,072-token
reservation fails before serving:

```text
requested Engine runtime reservation requires 4817745408 bytes,
but only 3194336768 bytes are available for runtime capacity
```

A 32,768-token reservation still requests 3,421,036,032 bytes and does not fit. The verified 16K
profile loads and listens. Do not diagnose these capacity failures as Jinja faults.

Check readiness with `curl -sS http://127.0.0.1:8080/health` and model identity with
`curl -sS http://127.0.0.1:8080/v1/models`. Watch the server terminal/log while testing pi: pi's
streaming UI has previously hidden an HTTP 400 and exited successfully with no answer.

## Pi model contract

The `qwen3.8-27b` entry in `~/.pi/agent/models.json` must match the server profile:

- `input`: `["text", "image"]`; omitted defaults to text-only and pi drops attached images.
- `contextWindow`: `16384`; advertising 131K makes pi send prompts the server must reject.
- `maxTokens`: `4096` (or another value leaving prompt room).
- `compat.maxTokensField`: the literal field name `"max_tokens"`, never a numeric string such as
  `"131072"`.
- Ninfer accepts developer roles, reasoning effort, top-level `enable_thinking`, and the embedded
  Qwen template. Keep requested thinking modes internally consistent.

Because the vision server is limited to 16K, keep `AGENTS.md` compact and load detailed skills only
when relevant. The former 90 KB file alone rendered to 24,738 prompt tokens and caused
`context_length_exceeded` before any task work.

## Use as a coding agent

From the repository root:

```sh
pi --provider local-llm --model qwen3.8-27b --thinking medium
pi --provider local-llm --model qwen3.8-27b --thinking medium -p \
  'Read AGENTS.md and make the requested scoped change. Build and verify it.'
```

For a bounded automated edit, use a fresh named/session file and inspect `git diff` afterward. Pi's
built-in tools are `read`, `bash`, `edit`, and `write`; `grep`, `find`, and `ls` may need an explicit
`--tools` allowlist. The model must use tool calls for repository facts instead of inventing them.

The embedded `sharp_qwen_chat_template.jinja` supports parallel calls, but a coding task is easier
to debug when the agent performs a small read/search batch, consumes every tool result, and then
edits. If calls appear as prose/XML without execution, capture the exact response and compare it
with ninfer's parsed tool-call output before changing either pi or the template.

## Vision and image verification

Attach images with pi's `@path` syntax:

```sh
pi --provider local-llm --model qwen3.8-27b --no-tools --no-session -p \
  @build/fmtowns/shots/shot0.png \
  'Identify the game state and visible rendering defects. Do not infer unseen frames.'
```

Confirm the ninfer request log says `media=1` (or the expected count). `media=0` means pi did not
send the image, usually because the model entry lacks `input: ["text", "image"]`. A successful
description with `media=0` is not vision evidence.

For game captures, ask for observable details: labels/text, clipping boundaries, stripes, palette
errors, stale regions, and left/right differences. Pair vision with decoded frame stamps and pixel
comparison scripts. Vision is semantic inspection, not proof of byte identity or timing.

## Fault isolation

- Startup reservation error: reduce context/KV capacity; not a template fault.
- `vision_disabled`: server was started without `VISION=1`/`--vision`.
- pi sends `media=0`: fix pi model input capabilities.
- `context_length_exceeded`: align pi's declared context with ninfer and reduce always-loaded text.
- Empty pi output: inspect ninfer log/HTTP status before blaming the model.
- Leading literal `</think>` with thinking off: reproduced as a ninfer/model output-splitting
  edge case after successful vision inference. It is not a vision or Jinja-render failure; use
  `--thinking low` for clean coding-agent output until the non-thinking decoder strips a redundant
  close marker.
- Tool-call parse issue: reproduce with a tiny one-tool request, preserve raw model output, then
  separate template rendering, model emission, ninfer parsing, and pi replay as four distinct layers.

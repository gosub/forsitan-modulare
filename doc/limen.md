# limen

![limen](../img/limen.png)

*limen* is a TCP+JSON control interface for VCV Rack. It exposes a simple newline-delimited JSON protocol over a local TCP socket, letting you query and control your patch from scripts, Emacs, or any other tool that can open a socket.

## Module UI

A green LED at the centre of the panel indicates that the server is listening. Right-click the module for options:

- **Server enabled** - toggle the TCP server on or off without removing the module. The LED goes dark when the server is stopped.
- **TCP port** - choose a preset port (7000, 7001, 7002, 7777, 8000) or type any value in the text field (1–65535) and press Enter.

The port and enabled state are saved with the patch.

## Use cases

**CLI control** - the included `limen` CLI lets you inspect and modify a live patch from the terminal. Useful for quick experiments, parameter sweeps, or integrating Rack into shell scripts.

**Alternative interfaces** - any tool that can open a TCP socket can drive Rack. An Emacs minor mode (coming soon) will let you interact with your patch directly from your editor: list modules, tweak parameters, connect cables, all without touching the mouse.

**LLM interfacing** - a language model can use limen as a tool to explore and build patches. `list_models` gives it the full module catalogue; `list_ports` tells it what each port does; `add_module` and `add_cable` let it act. The JSON protocol is easy for models to generate and parse.

**Automated patch testing** - load a known patch, query its topology with `list_modules` and `list_cables`, assert that parameters are in expected ranges with `list_params`. Useful for regression testing or verifying that a saved patch loads correctly.

**Live coding / generative patching** - drive patch changes from a REPL, a script, or a custom sequencer. Add and remove modules, reconnect cables, and automate parameter changes in real time without touching the Rack UI.

**Patch documentation and archiving** - dump the current patch state to JSON for later analysis or archiving. `list_modules`, `list_cables`, and `list_params` together give a complete snapshot of what is patched and how it is configured, and `save_patch_as` writes the patch itself to a `.vcv` file.

## Security

The server binds **loopback only** (`127.0.0.1`), so it is reachable only from the same machine. There is **no authentication** by design: anything that can open the local socket can control Rack (add/remove modules, set parameters, even quit). This is fine for local scripting and editor integration, but do **not** expose the port to an untrusted network (e.g. via port forwarding or a public bind) without adding your own access control in front of it.

## JSON protocol

The server listens on `localhost:7000` by default. It handles **one client at a time** by design; open a connection, send your requests, and close it. Send one JSON object per line; receive one JSON response line per request.

**Request:**
```json
{"cmd": "<command>", ...}
```

**Success response:**
```json
{"ok": true, "result": <value>}
```

**Error response:**
```json
{"ok": false, "error": "<message>"}
```

### Commands

#### Discovery

| cmd | extra fields | result | description |
|-----|-------------|--------|-------------|
| `hello` | - | `{protocol, commands}` | protocol version and the list of supported commands; call first to check compatibility |

The current protocol version is **2**. Version 2 added module positions
(`pos`/`hp` in the module listings, `x`/`y` on `add_module`, `move_module`),
`save_patch` / `save_patch_as`, and `batch`. Clients that check the version
strictly, [limen-tools](https://github.com/gosub/limen-tools) among them, need
their own update to talk to a protocol 2 server; the `commands` list is the
reliable feature test for everything else.

#### Plugin and model registry

| cmd | extra fields | result | description |
|-----|-------------|--------|-------------|
| `list_plugins` | - | `[{slug, name, version}]` | all plugins loaded in Rack |
| `list_models` | `"plugin": "<slug>"` (opt.) | `[{plugin, slug, name, description}]` | all available models, optionally filtered by plugin |

#### Modules in the patch

| cmd | extra fields | result | description |
|-----|-------------|--------|-------------|
| `list_modules` | `"plugin": "<slug>"` (opt.) | `[{id, plugin, model, name, numParams, numInputs, numOutputs, pos: {x, y}, hp}]` | modules currently in the patch |
| `get_module` | `"id": <int>` | `{id, plugin, model, name, numParams, numInputs, numOutputs, pos: {x, y}, hp}` | detail for one module |
| `get_module_info` | `"id": <int>` | `{id, model: {slug, name, description, tags, manualUrl, modularGridUrl}, plugin: {slug, name, brand, version, license, author, authorUrl, pluginUrl, manualUrl, sourceUrl, donateUrl, changelogUrl}}` | the metadata Rack shows in a module's right-click Info menu |
| `list_ports` | `"id": <int>` | `{inputs: [{id, name}], outputs: [{id, name}]}` | input and output port names |
| `list_params` | `"id": <int>` | `[{id, value, name, min, max, unit}]` | params for a module |
| `get_param` | `"id": <int>`, `"param": <int>` | `{id, value, name, min, max, unit}` | one parameter's current value and metadata |
| `set_param` | `"id": <int>`, `"param": <int>`, `"value": <float>` | `null` | set a parameter value |
| `add_module` | `"plugin": "<slug>"`, `"model": "<slug>"`, `"x"`, `"y"`, `"mode"` (opt.) | `{id, pos: {x, y}, hp}` | add a module to the patch |
| `remove_module` | `"id": <int>` | `null` | remove a module from the patch |
| `move_module` | `"id": <int>`, `"x": <int>`, `"y": <int>`, `"mode"` (opt.) | `{id, pos: {x, y}}` | move a module to a grid position |

##### Module positions

`pos` is in **Rack grid coordinates**: `x` counts HP columns and `y` counts
rack rows, measured from Rack's origin, the same units the rack itself snaps
to. `hp` is the module's width in the same units, so `x + hp` is where the
next module can start. Positions are integers; a patch built by a script can
place a whole row with `x` running along it.

`add_module` without `x`/`y` keeps the old behaviour, dropping the module next
to an existing one. With them, it is placed where you asked.

`mode` decides what happens when the target is already occupied:

| mode | behaviour |
|------|-----------|
| `nearest` (default) | move to the closest free position, disturbing nothing else |
| `force` | move there, pushing the row's other modules left or right |
| `squeeze` | move there, contracting the old position and making room |
| `strict` | fail with `position occupied` rather than move elsewhere |

Only `strict` guarantees the module ends up exactly where asked, so both
commands return the position it actually landed on.

#### Cables in the patch

| cmd | extra fields | result | description |
|-----|-------------|--------|-------------|
| `list_cables` | `"id": <int>` (opt.), `"verbose": true` (opt.) | `[{id, outputModule, outputPort, inputModule, inputPort, …}]` | cables in the patch; filter by module id; verbose adds `outputModuleName`, `outputPortName`, `inputModuleName`, `inputPortName` |
| `add_cable` | `"outputModule": <int>`, `"outputPort": <int>`, `"inputModule": <int>`, `"inputPort": <int>` | `{id}` | connect two ports |
| `remove_cable` | `"id": <int>` | `null` | remove a cable from the patch |

#### Batching

| cmd | extra fields | result | description |
|-----|-------------|--------|-------------|
| `batch` | `"commands": [{cmd…}, …]`, `"stopOnError": <bool>` (opt., default `true`) | `{count, failed, stopped, results}` | run a sequence of commands in one round trip |

Each element of `commands` is an ordinary request object, and `results` holds
the reply each would have got on its own, in order:

```json
{"cmd": "batch", "commands": [
  {"cmd": "add_module", "plugin": "Fundamental", "model": "VCO", "x": 0, "y": 0},
  {"cmd": "add_module", "plugin": "Fundamental", "model": "VCF", "x": 10, "y": 0}
]}
```

**Partial failure.** The envelope's `ok` says only that the batch itself was
well formed; each command's success is its own entry in `results`. `count` is
how many ran, `failed` how many of those returned an error. With the default
`stopOnError`, the batch stops at the first failure, `stopped` gives its index,
and `results` is shorter than `commands` - the commands before it have already
taken effect and are not rolled back. With `"stopOnError": false` every command
runs and `results` always matches `commands` one for one.

A batch may not contain another batch. Batching saves round trips and client
bookkeeping, not time inside Rack: the commands still run one at a time, and
each mutating one still waits for a frame.

#### The patch file

| cmd | extra fields | result | description |
|-----|-------------|--------|-------------|
| `save_patch` | `"path": "<path>"` (opt.) | `{path}` | save the patch to its current file, or to `path` as a copy |
| `save_patch_as` | `"path": "<path>"` | `{path}` | save the patch to `path` and make it the patch's file |

`save_patch` with no `path` is Rack's **Save**: it writes the file the patch
was opened from, and fails with *patch has never been saved* if there is none.
Given a `path` it writes a copy there and leaves the current file alone.
`save_patch_as` is Rack's **Save as**: it writes `path`, adopts it as the
patch's file, and adds it to the recent patches. Neither prompts, and neither
asks before overwriting: a script that saves is trusted to know where.

There is no `load_patch`. Loading replaces every module in the rack, this one
included, which would cut the connection mid-command; open patches from
Rack's own file menu or from the command line.

#### Application / view

These control the Rack window and view rather than the patch. Useful for
scripting screenshots (e.g. add modules, `set_fullscreen`, `zoom_to_modules`,
then capture the window with an external tool).

| cmd | extra fields | result | description |
|-----|-------------|--------|-------------|
| `set_fullscreen` | `"on": <bool>` | `{fullscreen}` | enter or leave fullscreen; result reflects the resulting state |
| `zoom_to_modules` | - | `null` | set offset and zoom to fit all modules to the view (the F4 action) |
| `quit` | - | `null` | quit VCV Rack (window closes after the current frame) |

`zoom_to_modules` fits to the **current** viewport, so call it *after* any
viewport change such as `set_fullscreen`. It returns immediately but the fit
settles over the next few frames, so allow a short pause before capturing a
screenshot.

### Examples

```bash
# list modules (with netcat)
echo '{"cmd":"list_modules"}' | nc localhost 7000

# list all models available from the Fundamental plugin
echo '{"cmd":"list_models","plugin":"Fundamental"}' | nc localhost 7000

# list port names for module 8518972980240757
echo '{"cmd":"list_ports","id":8518972980240757}' | nc localhost 7000

# list params for a module
echo '{"cmd":"list_params","id":8518972980240757}' | nc localhost 7000

# set param 0 of module 8518972980240757 to 0.5
echo '{"cmd":"set_param","id":8518972980240757,"param":0,"value":0.5}' | nc localhost 7000

# list cables with module names and port names
echo '{"cmd":"list_cables","verbose":true}' | nc localhost 7000

# add a module at HP column 24 of the top row
echo '{"cmd":"add_module","plugin":"Fundamental","model":"VCO","x":24,"y":0}' | nc localhost 7000

# move a module one row down, refusing to land anywhere else
echo '{"cmd":"move_module","id":8518972980240757,"x":24,"y":1,"mode":"strict"}' | nc localhost 7000

# two modules in one round trip (one line: a request ends at the newline)
echo '{"cmd":"batch","commands":[{"cmd":"add_module","plugin":"Fundamental","model":"VCO","x":0,"y":0},{"cmd":"add_module","plugin":"Fundamental","model":"VCF","x":10,"y":0}]}' | nc localhost 7000

# save the patch under a new name
echo '{"cmd":"save_patch_as","path":"/tmp/generated.vcv"}' | nc localhost 7000
```

```python
# Python example
import socket, json
s = socket.create_connection(("127.0.0.1", 7000))
s.sendall(b'{"cmd":"list_modules"}\n')
print(json.loads(s.recv(65536)))
```

## CLI tool

A ready-made command-line client, **limen-cli**, lives in the separate
[limen-tools](https://github.com/gosub/limen-tools) repository, in two
flavors with the same interface: a compiled C client (prebuilt binaries for
Linux, Windows and macOS on the
[releases page](https://github.com/gosub/limen-tools/releases)) and a
dependency-free Python client. It covers the whole protocol - see the
[limen-tools readme](https://github.com/gosub/limen-tools#readme) for the
full command reference.

```bash
limen-cli hello
limen-cli modules
limen-cli set 8518972980240757 0 0.5
```

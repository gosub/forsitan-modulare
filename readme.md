# forsitan modulare

A collection of VCV Rack modules

- [alea](#alea) - Add a random module to your rack
- [interea](#interea) - Make a chord from a V/Oct input, with quality, voicing and inversion. Harmonize option.
- [cumuli](#cumuli) - Accumulator with up and down gates and rates.
- [deinde](#deinde) - Quad cascading addressable attack-hold envelope.
- [pavo](#pavo) - Spreader of polyphonic mono signals across the stereo field (Splay Ugen)
- [limen](#limen) - TCP+JSON Rack control interface

![forsitan-modulare build](https://github.com/gosub/forsitan-modulare/workflows/forsitan-modulare%20build/badge.svg)

## alea

![alea](img/alea.png)

*alea* adds a single random module to your rack, chosen from your entire library. Inspired by [WhatTheRack from korfuri](https://github.com/korfuri/WhatTheRack).

### how to use

Click on the ⚂ (die)

## interea

![interea](img/interea.png)

*interea* transforms a V/Oct signal into a chord. It has four chord qualities (Maj7, Min7, Dom7, Half Dim), four inversions (root, first, second, third) and four voicings (close, drop 2, drop 3, spread). When the *harmonize* button is pressed, the frequency input is treated as a bassline and the chord is harmonized with the major scale. This module is inspired by [Strum's Mental Chord](https://github.com/Strum/Strums_Mental_VCV_Modules/wiki/Chord) and the physical module [Chord v1 by Qu-Bit Electronix](https://www.modulargrid.net/e/qu-bit-electronix-chord).

### how to use

Connect the four outputs to the V/O input of four oscillators. You should now be listening to the classic C4 Major7 chord. Play with the *frequency*, *quality*, *inversion* and *voicing* knobs and inputs to play different chord. The *frequency* input is bipolar (+/- 5V), the range of the other inputs is uni-polar (0-10V). When the *harmonize* button is on, the chord *quality* is chosen automatically and the *quality* knob and input are disabled. In this state the notes of the chord will be harmonized with diatonic and modal interchange chords of the Major scale. See the [manual](https://www.qubitelectronix.com/s/Chord_Manual.pdf) for Qu-Bit Electronix Chord for addition explanations.

### bypass behavior

The root pitch input is copied to all the chord outputs (root, 3rd, 5th, 7th).

## cumuli

![cumuli](img/cumuli.png)

*cumuli* is an accumulator. The value accumulated grows when the *up* gate is open and grows to the rate indicated by the *up* knob. Symmetrically, the value decrease when the *down* gate is open, with a velocity specified by the *down* knob. When the *reset* gate is open, the output value goes immediately to zero. The output value is clamped between 0V and 10V when the polarity selector is on "0-10V", and between -5V and +5V when the selector is on "±5V". The output holds its value when neither the *up* or *down* gates are open. The *up* and *down* knobs are exponential, and their values range between 0.01 V/sec and 100 V/sec (1 V/sec default). Each gate has a corresponding button.

### how to use

Connect two gate signals to the *up* and *down* input, select the *up* and *down* rate with the respective knobs and watch the output go up, down or hold. This module was imagined as a companion to midi controllers like the [Korg nanoPAD2](https://www.korg.com/us/products/computergear/nanopad2/), which has no faders, only buttons.

## deinde

![deinde](img/deinde.png)

*deinde* is a quad cascading addressable attack-hold envelope.

- quad: because it outputs 4 envelopes
- cascading: because the 4 envelopes open one after the other
- addressable: because the envelopes can be scanned with the *cascade* knob and CV
- attack-hold envelope: because every envelope has a linear attack and then stays open

This module is inspired by [A-144 by Doepfer](http://www.doepfer.de/a144.htm), with a significant difference: the four envelopes of the A-144 are attack-decay, while in this module they are attack-hold. One of the initial ideas behind this module was opening four send effects with only one control signal, for example: volume ramp, then saturation, then distortion, then fuzz.

### how to use

Connect the four output as you would for four envelopes, for example to the vca of four oscillators tuned to a chord. Turn the *cascade* knob to open the four envelope in sequence. When there is a signal in the *CV in* input, the signal scan the envelopes like the *cascade* knob, and in that case the *CV* knob acts like an attenuator and the *cascade* knob like an offset. All inputs and outputs are 0V-10V.

## pavo

![pavo](img/pavo.png)

*pavo* spreads a polyphonic signal across the stereo field. It is heavily inspired by the [Splay Ugen](https://doc.sccode.org/Classes/Splay.html) available in the SuperCollider language. It works like this:

- If the input has 1 channel: `L-----------o-----------R`
- If the input has 2 channel: `o-----------------------o`
- If the input has 3 channel: `o-----------o-----------o`
- If the input has 4 channel: `o-------o-------o-------o`
- If the input has 5 channel: `o-----o-----o-----o-----o`
- and so on...

*pavo* uses the [square root method to approximate constant power panning](https://www.cs.cmu.edu/~music/icm-online/readings/panlaws/index.html). It also adjust the level of the input channels when mixing down (level compensation). Right now these behaviors are hard coded, future version of the module will present options to disable them.

### how to use

Connect a polyphonic cable to the *poly in* input. Adjust the *spread* knob to select the maximum spread across the stereo field: a value of 0% means that all the channels are at the center, a value of of 100% means that the first and last signal are panned hard left and hard right. Additionally, the center knob determines the midpoint of the stereo image: -100% means the center is on the left side, 100% on the right. With this parameter, it could happen that channels could fall outside the stereo field. This is prevented by clipping their final position. So, when the center is 100% left, all the channels that would fall on the left side are "squished" at 100% left.
The *spread CV* input accepts 0V-10V, while the *center CV* input is ±5V. When these inputs are plugged, the respective knobs act like offsets.

## limen

![limen](img/limen.png)

*limen* is a TCP+JSON control interface for VCV Rack. It exposes a simple newline-delimited JSON protocol over a local TCP socket, letting you query and control your patch from scripts, Emacs, or any other tool that can open a socket.

### module UI

A green LED at the centre of the panel indicates that the server is listening. Right-click the module for options:

- **Server enabled** — toggle the TCP server on or off without removing the module. The LED goes dark when the server is stopped.
- **TCP port** — choose a preset port (7000, 7001, 7002, 7777, 8000) or type any port number (1–65535) in the text field and press Enter.

The port selection and enabled state are saved with the patch.

### JSON protocol

The server listens on `localhost:7000` by default (configurable via right-click menu). Send one JSON object per line; receive one JSON response line per request.

**Request format:**
```json
{"cmd": "<command>", ...}
```

**Response format (success):**
```json
{"ok": true, "result": <value>}
```

**Response format (error):**
```json
{"ok": false, "error": "<message>"}
```

**Commands:**

| cmd | extra fields | description |
|-----|-------------|-------------|
| `list_plugins` | — | list all plugins loaded in Rack |
| `list_modules` | `"plugin": "<slug>"` (opt.) | list modules in the patch, optionally filtered by plugin |
| `get_module` | `"id": <int>` | get detail for one module |
| `list_params` | `"id": <int>` | list params for a module |
| `set_param` | `"id": <int>`, `"param": <int>`, `"value": <float>` | set a parameter value |
| `list_cables` | — | list all cables in the patch |
| `add_module` | `"plugin": "<slug>"`, `"model": "<slug>"` | add a module → `{"id": <int>}` |
| `remove_module` | `"id": <int>` | remove a module from the patch |
| `add_cable` | `"outputModule": <int>`, `"outputPort": <int>`, `"inputModule": <int>`, `"inputPort": <int>` | connect two ports → `{"id": <int>}` |
| `remove_cable` | `"id": <int>` | remove a cable from the patch |

### quick-start examples

```bash
# list modules with netcat
echo '{"cmd":"list_modules"}' | nc localhost 7000

# list params for module 8518972980240757
echo '{"cmd":"list_params","id":8518972980240757}' | nc localhost 7000

# set param 0 of module 8518972980240757 to 0.5
echo '{"cmd":"set_param","id":8518972980240757,"param":0,"value":0.5}' | nc localhost 7000

# from Python
import socket, json
s = socket.create_connection(("127.0.0.1", 7000))
s.sendall(b'{"cmd":"list_modules"}\n')
print(json.loads(s.recv(65536)))
```

### CLI tool

A standalone C client lives in `cli/`. No dependencies beyond a POSIX C compiler.

```bash
cd cli && make
```

**Usage:**
```
limen [--port N] [--host H] [--json] <command> [args]
```

```bash
# list all loaded plugins
./limen plugins

# list all modules (human-readable table)
./limen modules

# list modules from a specific plugin
./limen modules VCV

# get module detail
./limen get 8518972980240757

# list params for a module
./limen params 8518972980240757

# set a parameter
./limen set 8518972980240757 0 0.75

# list cables
./limen cables

# add a module, prints its id
./limen add VCV VCO-1

# remove a module (supports id prefix)
./limen rm 8518972980240757

# connect output 0 of one module to input 0 of another, prints cable id
./limen connect 8518972980240757:0 9876543210:0

# remove a cable by id
./limen disconnect 1234567890

# raw JSON output (pipe to jq)
./limen --json modules | jq .
```

Install:
```bash
make install   # installs to ~/.local/bin/limen
```

## Author

Giampaolo Guiducci <giampaolo.guiducci@gmail.com>

## License

GPL-3

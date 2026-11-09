# Custom pedals (.mdpd)

A `.mdpd` file is a small text program that turns an input sample into an
output sample, one at a time. Drop one into the app with **Load .mdpd…** in
the pedalboard.

## Quick start – the smallest working pedal

```
pedal "Clean Boost"
knob Level = 5

on sample(x) {
    return x * Level * 0.2;
}
```

Save as `Clean Boost.mdpd`. In the pedalboard, press **Load .mdpd…**. A
"Clean Boost" tile appears with one Level knob that scales the signal.

## File structure

```
pedal    "Name"          // required, exactly one
brand    "Your name"     // optional
category "overdrive"     // optional, colours the tile

knob  Gain = 5           // any number, before on sample
state memory = 0         // any number, before on sample

on sample(x) {           // required, exactly one
    return x * Gain;
}
```

`knob` and `state` must come **before** `on sample`.

## Knobs

```
knob Drive = 5
```

declares a knob called `Drive`, default 5, range 0..10. Read it anywhere
inside `on sample`. To change label or range:

```
knob drive {
    label:   "Drive"
    default: 5
    min:     0
    max:     20
}
```

Knobs are **read-only**. Names are case-sensitive.

## State variables

A `state` remembers its value from one sample to the next — a filter
memory, LFO phase, envelope follower, etc.

```
state lp    = 0        // low-pass memory
state phase = 0        // LFO phase
```

Read and assign like any variable:

```
lp = lp + (x - lp) * 0.1;
```

State is reset to its default whenever the pedal is (re)loaded or the
project is reset.

## `on sample(x)`

`x` is the input sample. You **must** return a number — that number is the
output sample.

Statements:

```
let y = expr;          // local variable
stateName = expr;      // assign to state
if (cond) { ... }
if (cond) { ... } else { ... }
return expr;
```

Semicolons are optional.

## Expressions

```
+  -  *  /  %
<  >  <=  >=  ==  !=
&&  ||  !
cond ? a : b         // ternary
```

`pi`, `true`, `false` are available.

## Built-in functions

| | |
|---|---|
| `tanh(v)`               | soft saturation, stays below 1 |
| `sin(v)` `cos(v)`       | trig |
| `atan(v)`               | soft tanh-like |
| `abs(v)` `sign(v)`      | magnitude / -1,0,+1 |
| `floor(v)` `ceil(v)`    | rounding |
| `exp(v)` `log(v)` `sqrt(v)` | usual |
| `min(a,b)` `max(a,b)`   | smallest / largest |
| `pow(a,b)`              | a to the power of b |
| `clip(v,lo,hi)`         | clamp |

## `dt` — seconds per sample

Use `dt` for anything time-based (LFOs, envelopes). It is `1 / sampleRate`,
so the pedal sounds the same at 44.1 kHz and 96 kHz:

```
state phase = 0
on sample(x) {
    phase = phase + 440.0 * dt;
    if (phase > 1.0) { phase = phase - 1.0; }
    return sin(phase * 2.0 * pi);
}
```

## Reserved names

`x`, `dt`, `pi`, `true`, `false` — don't use them as knob or state names.

## Worked examples

### Hard clipper / distortion

```
pedal "Hard Clip"
knob Drive = 5
knob Level = 5

on sample(x) {
    let g   = 1.0 + Drive * 5.0;
    let hot = x * g;
    return clip(hot, -0.8, 0.8) * Level * 0.2;
}
```

### One-pole low-pass "tone" control

```
pedal "Dark Tone"
knob Cut   = 6
knob Level = 8

state lp = 0

on sample(x) {
    let freq = 200.0 * pow(20.0, Cut * 0.1);
    let a    = 1.0 - exp(-2.0 * pi * freq * dt);
    lp = lp + (x - lp) * a;
    return lp * Level * 0.12;
}
```

### Auto-wah

```
pedal "Auto Wah"
knob Sens  = 5
knob Depth = 6

state env = 0
state lp  = 0

on sample(x) {
    let a   = abs(x) > env ? 0.05 : 0.002;
    env     = env + (abs(x) - env) * a;
    let amt = env * (1.0 + Sens);
    let f   = 300.0 + Depth * 400.0 + amt * 2000.0;
    let k   = 1.0 - exp(-2.0 * pi * f * dt);
    lp = lp + (x - lp) * k;
    return lp * 1.2;
}
```

### Tremolo

```
pedal "Tremolo"
knob Rate  = 4
knob Depth = 7

state phase = 0

on sample(x) {
    let freq = 0.5 + Rate * 0.95;
    phase    = phase + freq * dt;
    if (phase > 1.0) { phase = phase - 1.0; }
    let lfo = sin(phase * 2.0 * pi);
    let amt = Depth * 0.1;
    return x * (1.0 - amt * (0.5 + 0.5 * lfo));
}
```

## Debugging

* **Silent?** Make sure `on sample` has a `return`.
* **NaN / blowing up?** Wrap output in `tanh(...)` or `clip(..., -1, 1)`.
* **Zipper noise when you turn a knob?** Smooth it in a state variable:
  ```
  state last = 0
  on sample(x) {
      last = last + (Gain - last) * 0.001;
      ...
  }
  ```
* **Syntax error?** The dialog tells you the line number. Usually a missing
  `}` or `)`, or an unknown name (typo of a state/knob).

## Limitations

* No arrays, loops or delay lines. Anything time-based lives in a `state`.
* One `on sample` block per pedal.
* One knob-value poll per sample — automatic.

## Where files live

* **Load .mdpd…** copies the file to `~/Documents/MiniDAW/Pedals/` so it
  survives a restart.
* When you **save a project**, the source of any custom pedal on the board
  is embedded in the project. The project opens with the same pedals on any
  machine.

## One-page reference

```
pedal   "Name"
brand   "optional"
category "overdrive"

knob   Gain = 5
knob   Gain { label: "Gain", default: 5, min: 0, max: 20 }

state  lp = 0

on sample(x) {
    let y = tanh(x * Gain);
    lp    = lp + (y - lp) * 0.05;
    return lp;
}
```

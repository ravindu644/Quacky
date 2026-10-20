# Keys and chords

Everything you can press, and how to combine keys. For how `hold`, `release` and chords fit into a
script, see [language.md](language.md).

## Chords

A chord is one or more keys joined with `+`, pressed together and released. A single key name on its
own is a one-key chord.

```
enter
ctrl + c
ctrl + alt + delete
super + r
shift + tab
```

A chord may combine any number of modifiers with up to **six** other keys.

## Modifiers

Modifiers never take a key slot of their own; they stack onto the keys pressed with them. Each has a
canonical name and some common aliases.

| Modifier | Aliases | Side |
|---|---|---|
| `ctrl` | `control` | left control |
| `shift` | | left shift |
| `alt` | `option` | left alt |
| `super` | `win`, `gui`, `meta`, `command` | left GUI (Windows / Command) |
| `altgr` | `ralt` | right alt |

## Named keys

These are the keys that have a name. Use the name directly in a chord or as an argument to `hold` /
`release`.

| Group | Keys |
|---|---|
| Editing | `enter`, `esc`, `backspace`, `tab`, `space`, `delete`, `insert` |
| Navigation | `up`, `down`, `left`, `right`, `home`, `endkey`, `pageup`, `pagedown` |
| Locks | `capslock`, `numlock`, `scrolllock` |
| System | `printscreen`, `pause`, `menu` |
| Function | `f1` through `f12`, and `f13` through `f24` |

A few notes:

- The End key is **`endkey`**, not `end` (`end` is a language keyword).
- There are no media or consumer keys (volume, play, brightness). That is a deliberate limit: Quacky
  speaks only the standard keyboard.

## Letters, digits and symbols

Any printable character can be a key too, resolved through a US-QWERTY layout. So these all work:

```
a
ctrl + c
alt + 4
shift + tab
```

Shifted symbols (`! @ # $ % ^ & * ( ) _ + { } | : " ~ < > ?` and the capital letters) carry their
own Shift automatically when used as a key.

One edge case: a line that starts with a bare digit is read as a number, not a key. Press a digit on
its own by writing it after a modifier (`shift + 1`) or type it as text (`type 1`). A single letter
on its own line is fine (`a` presses the A key).

## Holding keys

`hold` presses keys and keeps them down until released; `release` lets specific keys go, and
`release all` lets go of everything held.

```
hold super
hold shift
left
release all
```

Releasing a key that is not currently held is an error, and at most six keys can be held at once.

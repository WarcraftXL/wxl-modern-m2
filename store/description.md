# Modern M2

> The client's model loader understands one format: the one it shipped with. A model authored for a
> later version of the game is not something it rejects politely. It is a file it cannot read at all.

This module reads that newer file **directly** and fills the client's own model runtime with it.

There is no conversion step, no intermediate copy on disk, and no rewriting of the file's version to
pass it off as something older. The model keeps being what it is. The client ends up holding it in
exactly the shape it has always expected, and from that point on it neither knows nor needs to know
where the model came from.

The same reading happens on both sides, inside the client and in the asset host, so a model is ready
whichever one reaches it first.

---

## Reading is the easy half

Getting the bytes in is straightforward. A model authored years later assumes an engine that grew in
the meantime, and the real work is everything the client would otherwise get quietly wrong.

### Records that changed shape

Particle emitters and cameras are wider in newer versions than the client expects. Each one is
rewritten on the way in, driven by a table that says which array, which widths, and from which
version onward.

Supporting a new record type is **one entry in that table**, not a new path through the reader.

### Rigs the client cannot draw in one go

The client has a hard ceiling on how many bones a single draw may use, and modern rigs pass it
routinely. Clamping would be the easy answer: drop the bones past the limit, deform the model, say
nothing.

Instead, a submesh over the ceiling is partitioned into pieces that each fit, with its own bone list
and geometry rebuilt to match. This runs for **every** model, including ones the client already
understood, because the ceiling belongs to the client and not to the format.

### Shadows that follow the camera

A modern model carries bone flags that send the shadow pass down a shortcut which never refreshes
the pose. The symptom is unmistakable once you have seen it: the shadow swings around as you orbit.

Detected and corrected per model.

### Effects that look like mistakes

Particle blending and ribbon drawing are corrected so an effect renders the way its author meant it,
rather than as a bright rectangle.

### Clicking still works

Triangle hit testing and opaque batch ordering are handled, so selecting a creature selects that
creature and not the one standing behind it.

---

## What it does not do yet

Stated plainly, because a page that only lists strengths is an advertisement.

| The model carries | What happens |
|---|---|
| An external skeleton file | The load is refused rather than half done |
| Animations in a separate file | Those sequences do not play |
| Several levels of detail | Only the highest is used |
| Other auxiliary data | Skipped, and named in the log |

Every skip is counted per model, so a model that looks wrong tells you which of these it hit instead
of leaving you to guess.

---

## Safety

Malformed input is a **logged failure, never a crash**. A model that fails to load leaves the client
running and the rest of your session untouched.

Session counters are readable from Lua, so a script can report how many models took the native path,
how many were refused, and how many textures resolved.

## Interfaces

- Publishes `wxl.m2draw`, for anything that needs to reach the model draw path.
- Reads the core's large-model arena, which is always there.
- Reads `wxl.fdid` to resolve textures a model refers to by id rather than by name. With nothing
  supplying it, those textures are simply not found and everything else still works.

## Requirements

WarcraftXL on a 3.3.5a client, build **12340**. The module refuses to load against anything else
rather than guessing, and says so in the log.

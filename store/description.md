# Modern M2

The client's own model loader only understands the format it shipped with. Anything authored for a
later version of the game is not a model it can refuse politely, it is a file it does not recognise.

This module reads the newer file and reshapes it into the native form **before the loader ever sees
it**. The client is never told anything changed: it receives a model in exactly the layout it has
always expected, and treats it as one of its own from that point on.

## What that means in practice

Nothing is converted on disk and nothing is repacked. The reshaping happens in memory, on the way in,
every time the model is read. Your files stay as their authors made them.

The same reshaping runs on both sides, inside the client and in the asset host, so a model is ready
whichever one reads it first.

## What it covers

Geometry and skins are rebuilt into the layout the native loader expects, including the batch and
material construction the client performs as it finishes loading a skin.

Animation is unwrapped as it completes, and the bone palette is rebuilt so rigs heavier than the
original format allowed still render, shadow pass included.

Particles and ribbons keep their blending, which is the difference between an effect that looks like
the artist intended and one that looks like a mistake.

Selection keeps working: triangle hit testing and opaque batch ordering are handled, so clicking a
creature still picks the creature.

## Interfaces

It publishes `wxl.m2draw` for anything that needs to reach the model draw path.

It reads `wxl.m2arena`, the large-model arena the core reserves during boot, and `wxl.fdid` for
resolving FileDataIDs when a model refers to its textures by id rather than by path. The arena is
always present. If nothing supplies `wxl.fdid`, models that reference textures by id will simply not
find them, and everything else still works.

## Requirements

WarcraftXL running in a 3.3.5a client, build 12340. The module refuses to load against anything else
rather than guessing, and says so in the log.

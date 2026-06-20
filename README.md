# wxl-modern-m2

A WarcraftXL module that lets the 3.3.5a client load model files authored for newer versions of the game.

The client's own loader only understands its native model format. This module reads a newer model and
reshapes it into that native form before the loader ever sees it, so the client treats it as if it had
always been one of its own. The same reshaping runs both inside the client and in the asset host, so a
model is ready whichever side reads it first.

Part of [WarcraftXL](../../README.md). Released under the GNU General Public License v3.0.

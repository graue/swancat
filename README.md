# Swancat

A program that pipes together generators and effects
from my [synth package](https://github.com/graue/synth)
to create random soundscapes.

See also [Boodler](http://boodler.org),
a recent discovery which motivated me
to release this old code.

## Dependencies

 * [synth utilities](https://github.com/graue/synth)
 * `fadef` and `dbtorat` from
   [graue-utils](https://github.com/graue/graue-utils)

All of the above must be in your path.
Swancat calls these programs from a shell
to generate the sounds it pieces together.

## Example

This seems to be a good series of commands
to turn Swancat's output into
an interesting and vaguely listenable soundscape:

    ./swancat | filter -type hp -cutoff 26 | amp -dB +9 \
    | comp -rms -threshdB -12 -ratio 10:1 -attack 500 -release 15000 \
    | comp -threshdB -1 -ratio 5:1 -attack 50 -release 5000 \
    | limit2 -threshdB +10 | reverb \
    | softsat -hardness `dbtorat -6` -range `dbtorat -0.1` | fmt -16

The output at the end of that is raw CD audio
(16-bit stereo, 44.1 KHz)
so you can add either

    > somefile.raw

to save it to a file, or

    | aplay -qfcd

(on Linux, using ALSA)
to play it directly.

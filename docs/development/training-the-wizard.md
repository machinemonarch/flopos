# Training the wizard

The wizard is to answer in his own words one day ([roadmap](../project/roadmap.md)).
His model is trained on this PC, not on the laptop; only the finished
file, `boot/wizard.wiz`, goes on the floppy, as `WIZARD.WIZ`. It is kept
in the repository, so building FLOPOS needs none of what follows; this is
only for teaching him again.

    W=~/.cache/flopos-wizard
    tools/wizard/corpus.py $W/corpus          # fetch and clean the two books
    tools/wizard/voice.py  $W/corpus          # write his own lines
    $W/venv/bin/python tools/wizard/train.py $W/corpus $W/wizard.bin
    tools/wizard/check.py  $W/wizard.bin $W/corpus
    tools/wizard/speak.py  $W/wizard.bin 10   # hear him
    tools/wizard/enginetest.py $W/wizard.bin  # FLOPOS's engine agrees?
    cp $W/wizard.bin boot/wizard.wiz          # onto the next floppy

- **What he reads.** Two public-domain books from Project Gutenberg,
  Shakespeare's complete works and the Jargon File 4.2.2, cut into single
  sentences; and six sets of lines written for FLOPOS in `voice.py`: the
  tired wizard himself, a bright land of sweets and quests, a neighbour
  with a scheme, the pirates of the phone lines, a singing fellow of the
  woods, and a game master. Those six are flavours only: no line is taken
  from a show, a film or a book. The books and the model stay outside the
  repository, in `~/.cache/flopos-wizard`.
- **The mix.** His own six flavours share the training evenly. The two
  books get a twentieth of it between them (`--books 0.05`): a model this
  small can't learn them and makes up words in their manner, so those
  lines are kept as a rare oddity.
- **The model.** A transformer of about 240,000 weights: 512 word pieces,
  3 layers, 96 wide, 64 pieces of memory. Every weight is one byte with a
  scale for each row, and the last fifth of the training is done with the
  weights already rounded so, to lose nothing when the file is written.
  The file is held under 256 KB.
- **`@`** in his lines stands for the word that was typed; FLOPOS will put
  the real word there.
- **`speak.py`** does the sums exactly as the engine on FLOPOS does
  (`kernel/wizsay.c`), from the same file. `enginetest.py` builds that
  engine for this PC and compares the two, score by score.
- **`check.py`** has him say 300 lines and measures them: that they end,
  that the words are real, that neighbouring words have met before (a
  rough test of grammar), that he doesn't repeat himself or stutter, and
  that he isn't reciting the books. It prints ok or FAILED.
- **In the emulator:** `tools/bench/run.py dist/flopos.img OUT
  'type:blorp\n=>\[wizard\]'`; he writes `[wizard] N pieces` on the serial
  wire for each answer.

`train.py` needs PyTorch, which lives in its own environment:

    python3 -m venv ~/.cache/flopos-wizard/venv
    ~/.cache/flopos-wizard/venv/bin/python -m pip install torch numpy

Training takes about a quarter of an hour on a graphics card. It is not
the same twice: each run gives a slightly different wizard.

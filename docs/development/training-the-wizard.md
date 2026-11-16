# Training the wizard

The wizard's model is trained on a PC. Only the finished file,
`data/wizard.wiz`, goes on the floppy, and it is in the repository, so
building FLOPOS needs none of this.

    W=~/.cache/flopos-wizard
    tools/wizard/corpus.py $W/corpus          # fetch and clean the two books
    tools/wizard/voice.py  $W/corpus          # write his own lines
    $W/venv/bin/python tools/wizard/train.py $W/corpus $W/wizard.bin
    tools/wizard/check.py  $W/wizard.bin $W/corpus
    tools/wizard/speak.py  $W/wizard.bin 10   # hear him
    $W/venv/bin/python tools/wizard/enginetest.py $W/wizard.bin
    cp $W/wizard.bin data/wizard.wiz          # onto the next floppy

`train.py` needs PyTorch in its own environment:

    python3 -m venv ~/.cache/flopos-wizard/venv
    ~/.cache/flopos-wizard/venv/bin/python -m pip install torch numpy

About a quarter of an hour on a graphics card; each run gives a slightly
different wizard.

- **What he reads:** six sets of lines written for FLOPOS in `voice.py`, evenly mixed, and a twentieth of two public-domain books (Shakespeare, the Jargon File), which he can only babble in the manner of. The books and the model stay in `~/.cache/flopos-wizard`. The flavours' inspirations are credited in the references[^1].
- **The model:** a transformer of about 240,000 weights (512 word pieces, 3 layers, 96 wide, 64 pieces of memory), one byte each, the last fifth of training done already rounded. Under 256 KB.
- **`@`** in a line is where FLOPOS puts the typed word.
- **`check.py`** has him say 300 lines and checks they end, use real words, don't repeat or recite the books. **`enginetest.py`** checks FLOPOS's engine (`src/kernel/wizsay.c`) against `speak.py`, score by score.
- **In the emulator:** `test/bench/run.py dist/flopos.img OUT 'type:blorp\n=>\[wizard\]'`.

[^1]: [References: inspiration for the wizard](../project/references.md#inspiration-for-the-wizard)

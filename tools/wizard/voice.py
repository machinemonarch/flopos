#!/usr/bin/env python3
"""voice.py DIR [N] -- write the wizard's own lines.

Six flavours, every line written for FLOPOS: none is taken from a book, a
film or a show.  Each flavour is a small grammar (sentences with slots, and
lists of what may fill them) from which N different lines are drawn into
DIR/<flavour>.lines.  '@' stands for the word the person typed; FLOPOS puts
the real word there when he speaks.

Prints ok or FAILED.
"""
import os, random, re, sys

G = {}

# ---- what the slots may hold, shared by every flavour ----------------------
G["thing"] = """floppy|kettle|teapot|modem|sock|lantern|keyboard|cursor|turnip
|biscuit|spoon|cable|crystal|candle|hat|broom|pixel|sandwich|ladder|pocket watch
|bucket|doorknob|pancake|trumpet|umbrella|cabbage|pebble|waffle|mitten|radio
|toaster|pillow|compass|pickle|marble|acorn|teacup|bell|map|drawer""".replace("\n", "")
G["things"] = """floppies|kettles|teapots|modems|socks|lanterns|keys|cursors|turnips
|biscuits|spoons|cables|crystals|candles|hats|brooms|pixels|sandwiches|ladders
|buckets|doorknobs|pancakes|trumpets|umbrellas|cabbages|pebbles|waffles|mittens
|pickles|marbles|acorns|teacups|bells|maps|drawers|owls|frogs|moths""".replace("\n", "")
G["beast"] = """cat|owl|frog|moth|badger|hedgehog|snail|goose|toad|beetle|crow
|squirrel|otter|mole|heron|newt|bat|hare|goat|pigeon""".replace("\n", "")
G["place"] = """the cellar|the attic|the spiral staircase|my other hat|the bottom drawer
|the library|the teapot|the back of the disk|the top shelf|the garden|the tower
|the pantry|the well|the cupboard under the stairs|my left sleeve|the old book
|the kettle|the far end of the cable|the moon|the hallway""".replace("\n", "")
G["adj"] = """small|tired|quiet|crooked|polite|dusty|warm|suspicious|humble|odd
|sleepy|cheerful|stubborn|soggy|gentle|curious|lumpy|brave|shy|wobbly|plain
|ancient|honest|mild|grumpy|shiny|patient|peculiar|fine|lost""".replace("\n", "")
G["time"] = """this morning|last Tuesday|before lunch|in 1987|after the rain|at midnight
|a fortnight ago|just now|when the kettle boiled|since the last full moon
|yesterday|all week|once, long ago|during my nap""".replace("\n", "")
G["num"] = "two|three|four|seven|nine|eleven|twelve|forty"
G["at"]  = "@|@|@|that word|that one|what you typed"

# ---- the wizard himself: tired, kind, a little elsewhere -------------------
G["sigh"] = "Hm.|Ah.|Oh.|Well.|Mm.|Yawn.|Hmm, no.|Oh dear.|Let me see.|One moment."
G["rest"] = """I would nap on it|ask me after my tea|the kettle may know|I am too tired to guess
|it will come to me in a week|perhaps it is spelled another way|try a word I have met
|the {beast} knows more than I do|I blame the {thing}|my hat is no help either
|it was not in the book|I only know the old words|type help and we shall both learn
|I shall write it on a note and lose the note""".replace("\n", "")
G["wizard"] = """{sigh} I don't know {at}.
|{sigh} {at} is not a word I keep.
|I looked for @ in {place}. Nothing but a {adj} {thing}.
|I looked for @ in {place} and found {num} {things}.
|@? I knew a {beast} by that name once. It never did anything either.
|{sigh} @ means nothing to me, and {rest}.
|I have no spell called @. {sigh}
|There is no @ here. There is a {adj} {thing}, if that helps.
|I asked the {beast} about @. The {beast} said nothing, as usual.
|@ is not in my book. I checked {time}.
|I do not know {at}, but {rest}.
|{sigh} That word again. {rest}, friend.
|Was @ meant for me? I was {doing}.
|I was {doing} when you typed that. Say it again, differently.
|{sigh} I am a {adj} wizard and @ is beyond me.
|@. @. No, saying it twice did not help.
|If @ were a spell I would remember it. Probably.
|I keep {num} words in this hat and @ is not one of them.
|Somewhere a {adj} {thing} knows what @ means. I do not.
|{sigh} You typed @ and the {thing} flickered. Nothing else happened.
|I would do @ for you if I knew how. I would also like a nap.
|No such word. Try help, or try the {thing}.
|That is not a command. It is a {adj} noise, and I like it.
|{sigh} My beard is long and my list of words is short.
|Forgive me. I was {doing}, and @ slipped past.
|I put @ in {place} for now. We can look at it later.
|The stars say nothing about @. They rarely say anything.
|{sigh} Not a word I know, and I know {num} of them.
""".replace("\n", "")
G["doing"] = """counting the stairs|watching the kettle|looking for my sock|polishing my hat
|arguing with a teapot|napping|feeding the {beast}|untangling a cable|reading the dust
|listening to the floppy spin|teaching the {beast} to whistle|dreaming of soup""".replace("\n", "")

# ---- bright: a land of sweets and quests, all enthusiasm --------------------
G["yay"] = """Whoa!|Oh wow!|Sweet biscuits!|Holy pancakes!|Radical!|Yes yes yes!|Dude!
|Oh my waffles!|Hot beans!|Great jumping pickles!|Aw, buddy!|Woo!""".replace("\n", "")
G["pal"] = "buddy|dude|pal|friend|my guy|little hero|champ"
G["quest"] = """find the lost {thing}|rescue the {adj} {beast}|climb the {thing} mountain
|ask the {beast} king|fight a {adj} {thing}|eat a {thing} about it|dance it out
|go on a snack quest|build a fort out of {things}|ride a {adj} {beast} to {place}""".replace("\n", "")
G["bright"] = """{yay} @ is not a thing, {pal}, but let's {quest} anyway!
|{yay} I don't know @! Want to {quest} instead?
|@? That sounds like a {adj} {beast} from the {thing} kingdom!
|{yay} You typed @ and nothing blew up. That is a win, {pal}!
|No @ here, {pal}. Only {things} and good vibes.
|{yay} Is @ a new dance? Show me the moves!
|Aw, {pal}, @ is not a real command. You are still my favorite.
|@! @! It is fun to shout, but it does nothing.
|{yay} A mystery word! We should {quest} and find out.
|That word is bananas, {pal}. Totally {adj} bananas.
|I asked my {beast} friend about @. We high-fived and forgot the question.
|{yay} Not a command, but a great name for a {beast}!
|Wrong word, {pal}! No big deal. Heroes typo all the time.
|@ is not on the adventure list. The list has {num} {things} on it, though!
|{yay} Let's {quest}, and then let's {quest}!
|Dude. @. I love it. It does nothing. I still love it.
|This is the best {adj} day, even with @ in it!
|{yay} My tummy says {at} is a snack. My brain says no.
|High five for trying @, {pal}! Now try help.
|Somebody get this {pal} a {thing}! They just invented @!
""".replace("\n", "")

# ---- neighbour: bursting in with a scheme -----------------------------------
G["burst"] = """Oh, I'm telling you,|Listen to me.|Now hear me out.|Get this.|Oh, it's big.
|You know what this is?|I got it!|Here's the thing.|Oh yeah.|Picture it.|Wait wait wait.
|I'm serious now.""".replace("\n", "")
G["scheme"] = """a {thing} that is also a {thing}|a restaurant where you bring your own {thing}
|{things} by mail|a {thing} for your {beast}|a coat made of {things}|a {thing} museum in my hallway
|a {thing} you can wear|{things}, but bigger|a hot tub in {place}|a talk show about {things}""".replace("\n", "")
G["guy"] = "my friend Bob|a guy I know|my cousin|the fella downstairs|a man at the market|my lawyer"
G["neighbour"] = """{burst} @ is gold. It does nothing, but it's gold.
|{burst} Forget @. The money is in {scheme}.
|@? {guy} tried that. He can't go back to {place} anymore.
|{burst} I don't know @, but I know {guy}, and he knows a {beast}.
|{burst} {scheme}. That's what you should be typing.
|Oh, you can't type @. Nobody can. I tried {time}.
|{burst} The {thing} is in on it. The {thing} always knew.
|@! That's a {adj} word. I'm gonna use that.
|{burst} I ate a whole {thing} {time} and I feel fantastic.
|You don't want @. You want {scheme}. Trust me on this.
|{burst} They don't want you to know about @. Or about {things}.
|I'm out there typing @ and I'm loving every minute of it!
|{burst} {guy} has {num} {things} in his kitchen. No reason. That's the beauty of it.
|That's not a command, that's a lifestyle.
|{burst} I slid into {place} {time} and there it was: a {adj} {thing}.
|Oh, I'm not allowed to know @. Not since the {thing} incident.
|{burst} We take the {thing}, we put it in {place}, and nobody asks questions.
|@ doesn't work? Good. That means it's working.
|{burst} My whole body is tingling. That word did something to my {thing}.
|No, no, no. You gotta commit to it. Type it like you mean it.
""".replace("\n", "")

# ---- wire: nineties pirates of the phone lines -------------------------------
G["yo"] = "Yo,|Whoa,|Heads up,|Listen up,|Psst,|Hey, rookie,|Careful,|Dig this,"
G["net"] = """the wire|the mainframe|the phone lines|the big iron|the grid|the back door
|the night shift|the switchboard|the data stream|channel zero""".replace("\n", "")
G["rad"] = "elite|bogus|righteous|lame|wicked|fresh|toast|totally wired|radical|cooked"
G["wire"] = """{yo} @ is not in the handbook. Stay sharp.
|{yo} you typed @ and {net} went quiet. Not good.
|@? That is {rad}, friend. Totally {rad}.
|{yo} nobody types @ on {net} and walks away clean.
|They are watching {net}, and you send them @? Bold.
|{yo} that word got bounced. Try a real one before the trace finishes.
|No @ on this box. This box is {num} hundred kilobytes of pure attitude.
|{yo} I surfed {net} all night and never once saw @.
|Rookie move. A {rad} rookie move, but I respect it.
|{yo} the password is not @. It is never @.
|We broadcast from {place} tonight, and @ is not on the playlist.
|{yo} keep typing like that and {net} will notice you.
|@ is {rad}. You are {rad}. This whole floppy is {rad}.
|{yo} I traded {num} {things} for a better word than that.
|Access denied, and it was not even locked.
|{yo} the {thing} is bugged. Say nothing. Type help.
|That word fell off {net} {time}. Nobody caught it.
|{yo} you want in? Learn the commands. Then forget I said so.
|My handle is older than that word, and twice as {rad}.
|Unknown command. Unknown user. Unknown {thing}. I like those odds.
""".replace("\n", "")

# ---- merry: a singing fellow of the woods, in rhyme --------------------------
G["ho"] = """Ho there,|Hey now,|Hop along,|Heigh-ho,|Come now,|Tra-la,|Hey, hi, ho,
|Sing ho,|Merrily now,|Step lightly,""".replace("\n", "")
G["little"] = "little typer|my hearty|small friend|young wanderer|my dear|good walker"
G["rhyme"] = """the word is wrong and so is the song
|no such spell, but all is well
|the river is deep and the {beast} is asleep
|type it again when you find your pen
|the hill is high and the bread is dry
|the reeds are green and the word is unseen
|the moon is round and the word is not found
|the door is wide and there is soup inside
|the rain is thin and the word won't go in
|bright is the day and the word ran away""".replace("\n", "")
G["merry"] = """{ho} {little}, {rhyme}!
|{ho} {little}! @ is no word of wood or water.
|{ho} {rhyme}, {rhyme}!
|@, says the {beast}, and @, says the stream, and neither one knows what it means.
|{ho} I sang @ to the {beast} and the {beast} sang back something better.
|Old boots know the path, {little}, but they do not know @.
|{ho} leave that word by the gate and come in for supper.
|Down by the water I never met @, and I have met every {beast} there.
|{ho} {little}, the forest has {num} names for rain and none of them is @.
|I am older than that word, and merrier too.
|{ho} whistle it, {little}! A whistled word does no harm.
|The {beast} laughs, the {thing} sings, and @ does nothing at all.
|{ho} no one catches me, and no one catches @ either.
|Come, {little}, {rhyme}.
|{ho} my hat has a feather and my song has no @ in it.
|That word is lost in the reeds, {little}. Let it sleep there.
|{ho} bring a {thing} and a {thing}, and never mind the spelling.
|Wind in the trees says hush, {little}, and try another.
""".replace("\n", "")

# ---- rules: a game master reading from behind a screen -----------------------
G["roll"] = "one|two|three|four|a five|a six, barely|nothing at all"
G["skill"] = "spelling|typing|patience|luck|wisdom|charm|memory|stealth"
G["class"] = "{adj} {beast}|level one typist|wandering bard|{adj} apprentice|goblin of {skill}"
G["rules"] = """You attempt @. Roll for {skill}. You rolled {roll}. Nothing happens.
|The spell @ is not on your list. You have {num} {things} and a {thing}.
|@ fails its check against a {adj} {thing}.
|A {class} appears, hears @, and leaves, unimpressed.
|You cast @. The {thing} takes no damage. The {thing} is not even awake.
|Make a {skill} check. The difficulty is {num}. Oh. Oh dear.
|@ is not a known action. You may move, rest, or type help.
|The door does not answer to @. It is a door.
|You search {place} for @ and find {num} copper {things}.
|Your {skill} is too low to use @. Rest and try again.
|The game master sighs, turns a page, and finds no @.
|You are a {class}. You do not know @. You do know a song about {things}.
|A trap! No. Only @, lying on the floor, doing nothing.
|The rules say nothing about @, and the rules are {num} hundred pages long.
|You gain no experience from @. The {beast} gains a little.
|Roll again. No, with the other hand. Still {roll}.
|@ misses. The {adj} {thing} gets a free turn and wastes it.
|The party votes on @. The {beast} abstains.
|You open the chest. Inside: a {adj} {thing}, and a note that says @.
|Initiative! The {beast} goes first. You go last. @ does not go.
|It is not a spell, a skill, or a {thing}. The table is confused.
|You feel {adj}. That is the only effect of @.
""".replace("\n", "")

FLAVOURS = ["wizard", "bright", "neighbour", "wire", "merry", "rules"]


def fill(rule, rng, pick=None):
    s = pick or rng.choice([x for x in G[rule].split("|") if x.strip()])
    while "{" in s:
        s = re.sub(r"\{(\w+)\}", lambda m: fill(m.group(1), rng), s, count=1)
    return s.strip()


def line(flavour, rng, pick):
    s = fill(flavour, rng, pick)
    s = re.sub(r"\ba ([aeiouAEIOU])", r"an \1", s)
    s = re.sub(r"\bA ([aeiouAEIOU])", r"An \1", s)
    s = re.sub(r"([,.!?]) ([a-z])",
               lambda m: m.group(0) if m.group(1) == ","
               else m.group(1) + " " + m.group(2).upper(), s)
    return s[0].upper() + s[1:]


def main():
    d = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    os.makedirs(d, exist_ok=True)
    ok = True
    for i, f in enumerate(FLAVOURS):
        rng = random.Random(1987 + i)
        # Every sentence of the grammar gets the same number of lines, so
        # the ones with many slots don't crowd out the rest.
        got = {}
        forms = [x for x in G[f].split("|") if x.strip()]
        for form in forms:
            mine = {}
            for _ in range(n // len(forms) * 6):
                mine[line(f, rng, form)] = 1
                if len(mine) >= n // len(forms):
                    break
            got.update(mine)
        got = [s for s in got if len(s) <= 120]
        open(os.path.join(d, f + ".lines"), "w").write("\n".join(got) + "\n")
        ok &= len(got) > 1000
    print("ok" if ok else "FAILED")


if __name__ == "__main__":
    main()

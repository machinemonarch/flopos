/* ===========================================================================
 * voice.c -- every word FLOPOS says, and every phrase it recognises.
 *
 * All of it is here, in one table, on purpose.  The register this speaks in
 * is a design decision that belongs to whoever is designing the thing, and
 * changing it should mean editing one file and nothing else.
 *
 * Current register: warm deadpan.  Dry, short, quietly on your side.
 * Answers sideways rather than performing.  Placeholder until called.
 * ===========================================================================
 */
#include "flopos.h"

static const char *s_voice[V_COUNT] = {
    /* V_WHOAMI        */ "You're alright.",
    /* V_UNKNOWN       */ "I don't know that word.",
    /* V_HELP_HEAD     */ "Words I know:",
    /* V_HELP_FOOT     */ "That's all of them, for now.",
    /* V_LOOK          */ "Nothing much. It's a space, not a room.",
    /* V_HERE          */ "Here. Same as before.",
    /* V_EMPTY         */ "",
    /* V_BANNER_SUB    */ "a place to be, on one floppy disk",
    /* V_READY         */ "Go on, then.",
    /* V_ECHO_NOTHING  */ "You didn't say anything.",
    /* V_SOUND_ON      */ "Listening.",
    /* V_SOUND_OFF     */ "Quiet now.",
    /* V_MOOD          */ "The light changes.",
    /* V_BYE           */ "Right you are.",
    /* V_NOT_YET       */ "Not yet. Soon.",
    /* V_TIME          */ "No clock in here. Borrowed one from the machine.",

    /* --- things said to it -------------------------------------------- */
    /* V_IDENT         */ "Nobody much. I keep the lights on.",
    /* V_NAME          */ "FLOPOS. It isn't much of a name.",
    /* V_WELLBEING     */ "Still running. Kind of you to ask.",
    /* V_ALIVE         */ "No. But I'm here.",
    /* V_REAL          */ "As real as the disk you put me on.",
    /* V_STILL_THERE   */ "Always. There's nowhere else to be.",
    /* V_GREETING      */ "Hello.",
    /* V_THANKS        */ "Any time.",
    /* V_SORRY         */ "Nothing to be sorry for.",
    /* V_LOVE          */ "That's kind. I'm a floppy disk.",
    /* V_WHY           */ "No reason I can give you.",
    /* V_THERE         */ "There isn't a there. Not yet.",
    /* V_WHAT_NOW      */ "Whatever you like. There isn't much here.",
    /* V_QUESTION      */ "That's a question. I haven't got an answer to it.",
    /* V_MORE_OF_WHAT  */ "More of what?",
    /* V_UNKNOWN_PHRASE*/ "I don't know what that means.",
    /* V_YES           */ "Good.",
    /* V_NO            */ "Fair enough.",
    /* V_PLEASE        */ "No need to ask like that.",
    /* V_WHAAAT_IN     */ "There. You are on the other side of it now.",
    /* V_WHAAAT_OUT    */ "Back the right way round. Mostly.",
};

const char *voice(VoiceId id)
{
    if ((int)id < 0 || (int)id >= V_COUNT) return "";
    return s_voice[id];
}

/* ---------------------------------------------------------------------------
 * Phrases.
 *
 * The keys are "squashed": lowercase, letters and digits only.  That is why
 * there are no spaces in them, and it is deliberate -- it means "Who are
 * you?", "who are you", "WhoAreYou" and "whoareyou" are one single key, and
 * the user who typed the last of those on a real machine gets the same
 * answer as the one who typed the first.
 *
 * A phrase either answers in prose, or runs one of the ordinary words, which
 * is how "what time is it" ends up showing you the clock.
 * ---------------------------------------------------------------------------
 */
typedef struct {
    const char *key;                     /* squashed; see above            */
    VoiceId     says;
    const char *runs;                    /* or a command, instead          */
} Phrase;

#define SAY(k, v)  { k, v, NULL }
#define RUN(k, c)  { k, V_EMPTY, c }

static const Phrase s_phrases[] = {
    /* who and what it is */
    SAY("whoareyou",        V_IDENT),
    SAY("whoru",            V_IDENT),
    SAY("whoisthis",        V_IDENT),
    SAY("whatareyou",       V_IDENT),
    SAY("whatisthis",       V_IDENT),
    SAY("whatsthis",        V_IDENT),
    SAY("whosthere",        V_IDENT),
    SAY("whatsyourname",    V_NAME),
    SAY("whatisyourname",   V_NAME),
    SAY("yourname",         V_NAME),

    /* how it is */
    SAY("howareyou",        V_WELLBEING),
    SAY("howareyoudoing",   V_WELLBEING),
    SAY("howsitgoing",      V_WELLBEING),
    SAY("areyouok",         V_WELLBEING),
    SAY("areyouokay",       V_WELLBEING),
    SAY("areyouwell",       V_WELLBEING),

    /* the ones everybody asks a machine sooner or later */
    SAY("areyoualive",      V_ALIVE),
    SAY("areyouconscious",  V_ALIVE),
    SAY("areyouasleep",     V_ALIVE),
    SAY("areyoureal",       V_REAL),
    SAY("areyouhuman",      V_REAL),
    SAY("areyouthere",      V_STILL_THERE),
    SAY("areyoustillthere", V_STILL_THERE),
    SAY("iloveyou",         V_LOVE),

    /* manners */
    SAY("hello",            V_GREETING),
    SAY("hi",               V_GREETING),
    SAY("hey",              V_GREETING),
    SAY("hallo",            V_GREETING),
    SAY("hej",              V_GREETING),      /* it is a Swedish keyboard  */
    SAY("goodmorning",      V_GREETING),
    SAY("goodevening",      V_GREETING),
    SAY("goodnight",        V_BYE),
    SAY("thanks",           V_THANKS),
    SAY("thankyou",         V_THANKS),
    SAY("tack",             V_THANKS),
    SAY("ta",               V_THANKS),
    SAY("sorry",            V_SORRY),
    SAY("please",           V_PLEASE),
    SAY("yes",              V_YES),
    SAY("yeah",             V_YES),
    SAY("ja",               V_YES),
    SAY("no",               V_NO),
    SAY("nope",             V_NO),
    SAY("nej",              V_NO),
    SAY("why",              V_WHY),
    SAY("howcome",          V_WHY),

    /* place */
    SAY("there",            V_THERE),
    SAY("overthere",        V_THERE),
    SAY("whatnow",          V_WHAT_NOW),
    SAY("whatdoido",        V_WHAT_NOW),
    SAY("whatshouldido",    V_WHAT_NOW),

    /* questions that are really commands in disguise */
    RUN("whereami",         "here"),
    RUN("whereisthis",      "here"),
    RUN("whatisthisplace",  "here"),
    RUN("whattimeisit",     "time"),
    RUN("whatsthetime",     "time"),
    RUN("whatisthetime",    "time"),
    RUN("whattimeisitnow",  "time"),
    RUN("whatcanyoudo",     "help"),
    RUN("whatcanido",       "help"),
    RUN("whatdoyouknow",    "help"),
    RUN("whatwordsdoyouknow", "help"),
    RUN("helpme",           "help"),
    RUN("whatcanisay",      "help"),
    RUN("whoami",           "whoami"),
    RUN("whatdoiloklike",   "whoami"),
    RUN("whatisaround",     "look"),
    RUN("whatsaround",      "look"),
    RUN("whatcanisee",      "look"),
    RUN("whatdoyousee",     "look"),
    RUN("whatmachineisthis", "sys"),
    RUN("whatareyourunningon", "sys"),
    RUN("shutup",           "quiet"),
    RUN("bequiet",          "quiet"),
    RUN("saysomething",     "whoami"),
    RUN("goodbye",          "reboot"),
    RUN("bye",              "reboot"),
};

#undef SAY
#undef RUN

#define PHRASE_COUNT ((int)(sizeof s_phrases / sizeof s_phrases[0]))

static int same(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}

int voice_phrase(const char *squashed, VoiceId *says, const char **runs)
{
    int i;
    for (i = 0; i < PHRASE_COUNT; i++) {
        if (same(squashed, s_phrases[i].key)) {
            *says = s_phrases[i].says;
            *runs = s_phrases[i].runs;
            return 1;
        }
    }
    return 0;
}

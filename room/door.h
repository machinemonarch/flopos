/* ===========================================================================
 * door.h -- what a page's scripts may ask browzer, through the door.
 * (Read by the room's engine, room/dom.c, and by browzer, script.c.)
 *
 * A request is an op byte, then its arguments: a number is four bytes
 * (little end first), a text is its length in four bytes and then its
 * UTF-8.  An element is its number in the tree.  The answer starts with a
 * kind byte.  Anything not on this list is refused, and the script that
 * asked is stopped.
 * ===========================================================================
 */
#ifndef DOOR_H
#define DOOR_H

enum {
    D_LOG = 1,      /* level, text                       console.log      */
    D_ALERT,        /* text                               alert()          */
    D_GETID,        /* text                    -> node    getElementById   */
    D_QUERY,        /* node, text, all         -> node or list            */
    D_TAGS,         /* node, text              -> list    getElementsByTagName */
    D_CLASSES,      /* node, text              -> list    getElementsByClassName */
    D_NAMES,        /* text                    -> list    getElementsByName */
    D_GET,          /* node, prop              -> a value                  */
    D_SET,          /* node, prop, text                                    */
    D_ATTR,         /* node, name              -> text or null             */
    D_SETATTR,      /* node, name, text                                    */
    D_DELATTR,      /* node, name                                          */
    D_CREATE,       /* tag                     -> node                     */
    D_TEXT,         /* text                    -> node                     */
    D_INSERT,       /* parent, child, before (0: at the end)               */
    D_REMOVE,       /* node                                                */
    D_STYLE,        /* node, name              -> text   (el.style.x)      */
    D_SETSTYLE,     /* node, name, text                                    */
    D_CLASS,        /* node, op (0 add 1 remove 2 toggle 3 contains), name -> bool */
    D_LISTEN,       /* node, type: a script listens here                   */
    D_FETCH,        /* method, url, body, type -> status, type, body       */
    D_GO,           /* url: location.href =                                */
    D_WRITE,        /* html: document.write                                */
    D_HREF,         /*                         -> text   the page's address */
    D_PATH,         /* node                    -> list   it and its ancestors */
    D_SHOWN,        /* node                    -> bool   drawn or hidden    */
    D_CLONE,        /* node, deep              -> node   cloneNode         */
    D_COUNT
};

/* properties, for D_GET and D_SET */
enum {
    DP_TAG = 1, DP_ID, DP_CLASS, DP_TEXT, DP_HTML, DP_OUTER, DP_VALUE, DP_CHECKED, DP_DISABLED,
    DP_SELECTED, DP_SELINDEX, DP_HREF, DP_SRC, DP_NAME, DP_TYPE, DP_PARENT, DP_FIRST, DP_LAST,
    DP_NEXT, DP_PREV, DP_CHILDREN, DP_NODES, DP_FIRSTEL, DP_LASTEL, DP_NEXTEL, DP_PREVEL,
    DP_TITLE, DP_BODY, DP_HEAD, DP_ROOT, DP_PLACEHOLDER, DP_ACTION, DP_METHOD, DP_FORM,
    DP_NODETYPE, DP_DATA, DP_HIDDEN, DP_OPTIONS, DP_COUNTEL, DP_READONLY, DP_ALT, DP_LABEL,
    DP_COUNT
};

/* answers */
enum { DK_UNDEF, DK_NULL, DK_BOOL, DK_NUM, DK_STR, DK_NODE, DK_LIST, DK_ERR };

/* Tasks beyond room.h's: a script's text in pieces, then run. */
#define T_SRC 6

#endif
